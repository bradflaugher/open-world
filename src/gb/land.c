/* land.c - OPEN WORLD streaming land ring (bank 0: it calls world_mt on the hot path).
 *
 * Desired window for a camera at metatile (cmx, cmy): x0 = cmx - 2, y0 = cmy - 3.
 * The visible land spans columns cmx..cmx+10 and rows cmy..cmy+8, so the committed window
 * may lag the desired one by up to 2 columns / 3 rows before anything unloaded could show.
 * A job is one full column (or row) of 15 metatiles, generated in order (the world core's
 * noise caches slide along it) into job_buf, then committed at once: cache + VRAM queue. */
#include <gb/gb.h>
#include <string.h>
#include "land.h"
#include "gfx.h"
#include "world.h"
#include "assets.h"

uint16_t land_x0, land_y0;
uint8_t land_cache[256];
uint8_t land_job;
uint16_t dbg_mt_calls;
uint8_t land_changed;
uint8_t dbg_hist[32];
uint16_t dbg_mt_max;           /* longest world_mt call seen, in scanlines */

static uint8_t job_i;
static uint16_t job_c;          /* world column (col jobs) or row (row jobs) being built */
static uint8_t job_buf[15];
static uint8_t row_t[64], row_a[64];

#define SLOT(mx, my) ((uint8_t)((((uint8_t)(my) & 15) << 4) | ((uint8_t)(mx) & 15)))

uint8_t land_in(uint16_t mx, uint16_t my)
{
    return (uint8_t)((uint16_t)(mx - land_x0) < 15u && (uint16_t)(my - land_y0) < 15u);
}

uint8_t land_mt(uint16_t mx, uint16_t my)
{
    if ((uint16_t)(mx - land_x0) < 15u && (uint16_t)(my - land_y0) < 15u)
        return land_cache[SLOT(mx, my)];
    return MT_SEA;     /* outside the window: treat as impassable (never call the core from ISRs) */
}

void land_set(uint16_t mx, uint16_t my, uint8_t mt)
{
    if (!land_in(mx, my)) {
        /* outside the window (main loop only: a mod was applied there): a job that already
           sampled the cell would commit a stale value, so restart it. Edits from the VBL
           ISR are always next to the wanderer, inside the window, and never touch jobs. */
        if (land_job) job_i = 0;
        return;
    }
    land_cache[SLOT(mx, my)] = mt;
    land_changed = 1;
    bq_push((uint8_t)(mx & 15), (uint8_t)(my & 15), mt);
}

void land_refill(uint16_t cmx, uint16_t cmy)
{
    uint8_t i, j, m, c, *t, *a;
    uint16_t x, y;
    land_job = 0;
    land_changed = 1;
    land_x0 = (uint16_t)(cmx - 2);
    land_y0 = (uint16_t)(cmy - 3);
    /* the queue may still hold writes for the old window */
    vram_flush();
    for (j = 0; j < 16; j++) {
        y = (uint16_t)(land_y0 + j);
        for (i = 0; i < 16; i++) {
            x = (uint16_t)(land_x0 + i);
            m = world_mt(x, y);
            land_cache[SLOT(x, y)] = m;
            c = (uint8_t)((x & 15) << 1);
            t = &mt_t[m << 2];
            a = &mt_a[m << 2];
            row_t[c] = t[0]; row_t[c + 1] = t[1]; row_t[c + 32] = t[2]; row_t[c + 33] = t[3];
            row_a[c] = a[0]; row_a[c + 1] = a[1]; row_a[c + 32] = a[2]; row_a[c + 33] = a[3];
        }
        c = (uint8_t)((y & 15) << 1);
        set_tiles(0, c, 32, 2, (uint8_t *)0x9800, row_t);
        if (is_cgb) {
            VBK_REG = 1;
            set_tiles(0, c, 32, 2, (uint8_t *)0x9800, row_a);
            VBK_REG = 0;
        }
    }
}

static void job_start(uint8_t kind)
{
    land_job = kind;
    job_i = 0;
    switch (kind) {
    case 1: job_c = (uint16_t)(land_x0 + 15); break;
    case 2: job_c = (uint16_t)(land_x0 - 1); break;
    case 3: job_c = (uint16_t)(land_y0 + 15); break;
    default: job_c = (uint16_t)(land_y0 - 1); break;
    }
}

static void job_commit(void)
{
    uint8_t i, m, s;
    uint16_t v;
    if (land_job <= 2) {
        for (i = 0; i < 15; i++) {
            v = (uint16_t)(land_y0 + i);
            m = job_buf[i];
            s = SLOT(job_c, v);
            land_cache[s] = m;
            bq_push((uint8_t)(job_c & 15), (uint8_t)(v & 15), m);
        }
        __critical { if (land_job == 1) land_x0++; else land_x0--; }
        land_changed = 1;
    } else {
        for (i = 0; i < 15; i++) {
            v = (uint16_t)(land_x0 + i);
            m = job_buf[i];
            s = SLOT(v, job_c);
            land_cache[s] = m;
            bq_push((uint8_t)(v & 15), (uint8_t)(job_c & 15), m);
        }
        __critical { if (land_job == 3) land_y0++; else land_y0--; }
        land_changed = 1;
    }
    land_job = 0;
}

uint8_t land_update(uint16_t cmx, uint16_t cmy, uint8_t budget)
{
    int16_t dx = (int16_t)((uint16_t)(cmx - 2) - land_x0);
    int16_t dy = (int16_t)((uint16_t)(cmy - 3) - land_y0);
    int16_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx > 2 || ady > 3) return 1;           /* would show unloaded cells: caller refills */
    /* abandon a job that no longer moves towards the camera */
    if (land_job == 1 && dx <= 0) land_job = 0;
    if (land_job == 2 && dx >= 0) land_job = 0;
    if (land_job == 3 && dy <= 0) land_job = 0;
    if (land_job == 4 && dy >= 0) land_job = 0;
    /* at the edge of the hysteresis: finish the job this frame */
    if (adx >= 2 || ady >= 3) budget = 15;
    while (budget) {
        if (!land_job) {
            if (!dx && !dy) return 2;
            /* the axis further behind first (rows have more slack) */
            if (dx && (adx + 1 >= ady || !dy)) job_start(dx > 0 ? 1 : 2);
            else job_start(dy > 0 ? 3 : 4);
        }
        while (budget && job_i < 15) {
            {   /* perf stats: world_mt duration in scanlines (ISR time included) */
                uint8_t l0 = LY_REG, v0 = vbl_frames, l1, v1;
                l0 = (uint8_t)(l0 >= 144 ? l0 - 144 : l0 + 10);
                uint16_t d;
                if (land_job <= 2) job_buf[job_i] = world_mt(job_c, (uint16_t)(land_y0 + job_i));
                else job_buf[job_i] = world_mt((uint16_t)(land_x0 + job_i), job_c);
                l1 = LY_REG; v1 = vbl_frames;
                l1 = (uint8_t)(l1 >= 144 ? l1 - 144 : l1 + 10);
                d = (uint16_t)((uint8_t)(v1 - v0) * 154u + l1 - l0);
                if (d & 0x8000) d = 0;
                if (d > dbg_mt_max) dbg_mt_max = d;
                dbg_hist[d > 31 ? 31 : d]++;
            }
            job_i++;
            budget--;
            dbg_mt_calls++;
        }
        if (job_i < 15) return 0;
        job_commit();
        dx = (int16_t)((uint16_t)(cmx - 2) - land_x0);
        dy = (int16_t)((uint16_t)(cmy - 3) - land_y0);
        adx = dx < 0 ? -dx : dx;
        ady = dy < 0 ? -dy : dy;
    }
    return 0;
}

/* ---- fast helpers around the wanderer (always well inside the window: no range checks) ---- */
#include "game.h"

#define CELL(x8, y8) land_cache[(uint8_t)((((uint8_t)(y8)) & 15) << 4) | (((uint8_t)(x8)) & 15)]

uint8_t land_rel(int8_t dx, int8_t dy)
{
    return CELL((uint8_t)pl_mx + dx, (uint8_t)pl_my + dy);
}

/* is the hitbox [x-5, x+4] x [y-5, y] free, with the foot moved by (ox, oy) pixels? */
uint8_t land_box_free(int8_t ox, int8_t oy)
{
    int8_t vx = (int8_t)(pl_sx + ox), vy = (int8_t)(pl_sy + oy);
    uint8_t x0 = (uint8_t)((uint8_t)pl_mx + (int8_t)((int8_t)(vx - 5) >> 4));
    uint8_t x1 = (uint8_t)((uint8_t)pl_mx + (int8_t)((int8_t)(vx + 4) >> 4));
    uint8_t y0 = (uint8_t)((uint8_t)pl_my + (int8_t)((int8_t)(vy - 5) >> 4));
    uint8_t y1 = (uint8_t)((uint8_t)pl_my + (int8_t)(vy >> 4));
    if (mt_flags[CELL(x0, y0)] & MTF_SOLID) return 0;
    if (x1 != x0 && (mt_flags[CELL(x1, y0)] & MTF_SOLID)) return 0;
    if (y1 != y0) {
        if (mt_flags[CELL(x0, y1)] & MTF_SOLID) return 0;
        if (x1 != x0 && (mt_flags[CELL(x1, y1)] & MTF_SOLID)) return 0;
    }
    return 1;
}

/* any metatile with `flag` within Chebyshev radius r of the wanderer's cell */
uint8_t land_scan_flag(uint8_t flag, uint8_t r)
{
    uint8_t x, y, x0 = (uint8_t)((uint8_t)pl_mx - r), y0 = (uint8_t)((uint8_t)pl_my - r), n = (uint8_t)(r * 2 + 1), i, j;
    for (j = 0, y = y0; j < n; j++, y++)
        for (i = 0, x = x0; i < n; i++, x++)
            if (mt_flags[CELL(x, y)] & flag) return 1;
    return 0;
}

static const int8_t nb_dx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
static const int8_t nb_dy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

/* first neighbour (N, NE, ... NW) holding something A lights or takes; 0xFF if none */
uint8_t land_near_act(void)
{
    uint8_t i, m, x = (uint8_t)pl_mx, y = (uint8_t)pl_my;
    for (i = 0; i < 8; i++) {
        m = CELL(x + nb_dx[i], y + nb_dy[i]);
        if (m == MT_FIRE_COLD || m == MT_BEACON || m == MT_SHRINE || m == MT_HEART) return i;
    }
    return 0xFF;
}

/* Blocked moving sx (+-1) along x: which way to nudge in y to slip round a corner?
   Only when the hitbox straddles two rows and one of them is open ahead. 0 = no way. */
int8_t land_slide_x(int8_t sx)
{
    int8_t vx = (int8_t)(pl_sx + sx), vy = (int8_t)pl_sy;
    uint8_t xe = (uint8_t)((uint8_t)pl_mx + (int8_t)((int8_t)(sx > 0 ? vx + 4 : vx - 5) >> 4));
    uint8_t y0 = (uint8_t)((uint8_t)pl_my + (int8_t)((int8_t)(vy - 5) >> 4));
    uint8_t y1 = (uint8_t)((uint8_t)pl_my + (int8_t)(vy >> 4));
    uint8_t b0, b1;
    if (y0 == y1) return 0;
    b0 = (uint8_t)(mt_flags[CELL(xe, y0)] & MTF_SOLID);
    b1 = (uint8_t)(mt_flags[CELL(xe, y1)] & MTF_SOLID);
    if (b0 && !b1) return 1;
    if (b1 && !b0) return -1;
    return 0;
}

int8_t land_slide_y(int8_t sy)
{
    int8_t vx = (int8_t)pl_sx, vy = (int8_t)(pl_sy + sy);
    uint8_t ye = (uint8_t)((uint8_t)pl_my + (int8_t)((int8_t)(sy > 0 ? vy : vy - 5) >> 4));
    uint8_t x0 = (uint8_t)((uint8_t)pl_mx + (int8_t)((int8_t)(vx - 5) >> 4));
    uint8_t x1 = (uint8_t)((uint8_t)pl_mx + (int8_t)((int8_t)(vx + 4) >> 4));
    uint8_t b0, b1;
    if (x0 == x1) return 0;
    b0 = (uint8_t)(mt_flags[CELL(x0, ye)] & MTF_SOLID);
    b1 = (uint8_t)(mt_flags[CELL(x1, ye)] & MTF_SOLID);
    if (b0 && !b1) return 1;
    if (b1 && !b0) return -1;
    return 0;
}
