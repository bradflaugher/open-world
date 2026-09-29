/* land.c - OPEN WORLD streaming land ring (bank 0: it calls world_mt on the hot path).
 *
 * Desired window for a camera at metatile (cmx, cmy): x0 = cmx - 2, y0 = cmy - 3.
 * The visible land spans columns cmx..cmx+10 and rows cmy..cmy+8, so the committed window
 * may lag the desired one by up to 2 columns / 3 rows before anything unloaded could show.
 * A job is one full column (or row) of 15 metatiles, generated in order (the world core's
 * noise caches slide along it) into job_buf, then committed at once: cache + VRAM queue.
 *
 * Autotiled water edges: each water cell's four quarters are chosen from the cache neighbours
 * (see assets.h). A cell whose neighbour is outside the window treats it as "same" (no edge),
 * so the outermost column / row is provisional on its far side; each commit also re-draws the
 * water cells of the line behind it, which now has all its neighbours. The camera gate in
 * world_frame keeps the provisional far halves of the edge lines off screen. */
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
#define row_t (scratch)          /* refill row buffers (main loop, hook off) */
#define row_a (scratch + 64)
uint8_t edge_t[EDGE_CLASS_COUNT * 4 * EDGE_VARIANT_COUNT];   /* RAM copy of edge_tiles */
static uint8_t mt_grp[MT_COUNT];      /* bit0: deep sea, bit1: any water (incl. stepping stones) */

#define SLOT(mx, my) ((uint8_t)((((uint8_t)(my) & 15) << 4) | ((uint8_t)(mx) & 15)))
#define CELL8(x8, y8) land_cache[(uint8_t)((((uint8_t)(y8)) & 15) << 4) | (((uint8_t)(x8)) & 15)]

void land_init(void)
{
    uint8_t i;
    for (i = 0; i < MT_COUNT; i++) mt_grp[i] = 0;
    mt_grp[MT_SEA] = mt_grp[MT_SEA_GLINT] = 3;
    mt_grp[MT_SHALLOW] = mt_grp[MT_STEPSTONE] = 2;
}

/* the variant for one quarter from its horizontal-side, vertical-side and diagonal neighbours */
static uint8_t pick(uint8_t a, uint8_t b, uint8_t c)
{
    if (a) return b ? EDGE_OUTER : EDGE_H;
    if (b) return EDGE_V;
    return c ? EDGE_INNER : 0xFF;
}

/* the four BG tiles of the cell at (x, y) holding metatile m */
static void cell_tiles(uint16_t x, uint16_t y, uint8_t m, uint8_t *t)
{
    uint8_t cls, rx, ry, x8 = (uint8_t)x, y8 = (uint8_t)y, v;
    uint8_t n, s, w, e, nw, ne, sw, se;
    const uint8_t *b = &mt_t[m << 2], *et;
    t[0] = b[0]; t[1] = b[1]; t[2] = b[2]; t[3] = b[3];
    if (m == MT_SEA || m == MT_SEA_GLINT) cls = 1;
    else if (m == MT_SHALLOW) cls = 2;
    else return;
    rx = (uint8_t)(x8 - (uint8_t)land_x0);
    ry = (uint8_t)(y8 - (uint8_t)land_y0);
#define OTH(dx, dy) ((uint8_t)(rx + (dx)) < 15 && (uint8_t)(ry + (dy)) < 15 && \
                     !(mt_grp[CELL8(x8 + (dx), y8 + (dy))] & cls))
    n = OTH(0, -1); s = OTH(0, 1); w = OTH(-1, 0); e = OTH(1, 0);
    nw = OTH(-1, -1); ne = OTH(1, -1); sw = OTH(-1, 1); se = OTH(1, 1);
#undef OTH
    et = &edge_t[(uint8_t)((cls - 1) << 4)];
    if ((v = pick(n, w, nw)) != 0xFF) t[0] = et[v];
    if ((v = pick(n, e, ne)) != 0xFF) t[1] = et[4 + v];
    if ((v = pick(s, w, sw)) != 0xFF) t[2] = et[8 + v];
    if ((v = pick(s, e, se)) != 0xFF) t[3] = et[12 + v];
}

/* does the water cell at (x, y) see an "other" cell among the 3 new neighbours at
   (nx, y-1..y+1) (column commit) or (x-1..x+1, ny) (row commit)? Only then did its edge,
   drawn while those were unknown, change. */
static uint8_t fix_needed(uint16_t x, uint16_t y, uint8_t col, uint16_t nc)
{
    uint8_t m = land_cache[SLOT(x, y)], cls, i;
    if (m == MT_SEA || m == MT_SEA_GLINT) cls = 1;
    else if (m == MT_SHALLOW) cls = 2;
    else return 0;
    for (i = 0; i < 3; i++) {
        uint8_t n = col ? land_cache[SLOT(nc, (uint16_t)(y - 1 + i))] : land_cache[SLOT((uint16_t)(x - 1 + i), nc)];
        if (!(mt_grp[n] & cls)) return 1;
    }
    return 0;
}

/* queue the cell's tiles for VBlank (it must be inside the window) */
static void push_cell(uint16_t x, uint16_t y)
{
    uint8_t t[4], m = land_cache[SLOT(x, y)];
    cell_tiles(x, y, m, t);
    bq_push((uint8_t)(x & 15), (uint8_t)(y & 15), m, t);
}

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
    push_cell(mx, my);
}

void land_refill(uint16_t cmx, uint16_t cmy)
{
    uint8_t i, j, m, c, *t, *a;
    uint8_t row_q[4];
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
            land_cache[SLOT(x, y)] = world_mt(x, y);
        }
    }
    t = row_q;
    for (j = 0; j < 16; j++) {
        y = (uint16_t)(land_y0 + j);
        for (i = 0; i < 16; i++) {
            x = (uint16_t)(land_x0 + i);
            m = land_cache[SLOT(x, y)];
            cell_tiles(x, y, m, t);
            c = (uint8_t)((x & 15) << 1);
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
    uint8_t i;
    uint16_t v, fix;
    if (land_job <= 2) {
        for (i = 0; i < 15; i++) land_cache[SLOT(job_c, (uint16_t)(land_y0 + i))] = job_buf[i];
        __critical { if (land_job == 1) land_x0++; else land_x0--; }
        fix = land_job == 1 ? (uint16_t)(job_c - 1) : (uint16_t)(job_c + 1);
        for (i = 0; i < 15; i++) {
            v = (uint16_t)(land_y0 + i);
            push_cell(job_c, v);
            if (fix_needed(fix, v, 1, job_c)) push_cell(fix, v);   /* now fully known */
        }
    } else {
        for (i = 0; i < 15; i++) land_cache[SLOT((uint16_t)(land_x0 + i), job_c)] = job_buf[i];
        __critical { if (land_job == 3) land_y0++; else land_y0--; }
        fix = land_job == 3 ? (uint16_t)(job_c - 1) : (uint16_t)(job_c + 1);
        for (i = 0; i < 15; i++) {
            v = (uint16_t)(land_x0 + i);
            push_cell(v, job_c);
            if (fix_needed(v, fix, 0, job_c)) push_cell(v, fix);
        }
    }
    land_changed = 1;
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

/* Idle time: warm the core's caches for the cells the scroll will need next, a few metatiles
   beyond the window edge in direction (dx, dy). One world_prefetch call per invocation;
   returns 0 when everything ahead is ready. Main loop only (the core is not re-entrant). */
uint8_t land_prefetch(int8_t dx, int8_t dy)
{
    static const uint8_t along[3] = { 0, 7, 14 };
    uint8_t i;
    uint16_t x, y;
    if (dx) {
        x = dx > 0 ? (uint16_t)(land_x0 + 15 + 3) : (uint16_t)(land_x0 - 4);
        for (i = 0; i < 3; i++)
            if (world_prefetch(x, (uint16_t)(land_y0 + along[i]))) return 1;
    }
    if (dy) {
        y = dy > 0 ? (uint16_t)(land_y0 + 15 + 3) : (uint16_t)(land_y0 - 4);
        for (i = 0; i < 3; i++)
            if (world_prefetch((uint16_t)(land_x0 + along[i]), y)) return 1;
    }
    return 0;
}
