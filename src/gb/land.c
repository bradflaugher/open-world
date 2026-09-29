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
    return world_mt(mx, my);
}

void land_set(uint16_t mx, uint16_t my, uint8_t mt)
{
    if (!land_in(mx, my)) return;
    land_cache[SLOT(mx, my)] = mt;
    bq_push((uint8_t)(mx & 15), (uint8_t)(my & 15), mt);
    /* a job that already sampled this cell would commit a stale value: restart it */
    if (land_job) job_i = 0;
}

void land_refill(uint16_t cmx, uint16_t cmy)
{
    uint8_t i, j, m, c, *t, *a;
    uint16_t x, y;
    land_job = 0;
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
        if (land_job == 1) land_x0++; else land_x0--;
    } else {
        for (i = 0; i < 15; i++) {
            v = (uint16_t)(land_x0 + i);
            m = job_buf[i];
            s = SLOT(v, job_c);
            land_cache[s] = m;
            bq_push((uint8_t)(v & 15), (uint8_t)(job_c & 15), m);
        }
        if (land_job == 3) land_y0++; else land_y0--;
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
            if (!dx && !dy) return 0;
            /* the axis further behind first (rows have more slack) */
            if (dx && (adx + 1 >= ady || !dy)) job_start(dx > 0 ? 1 : 2);
            else job_start(dy > 0 ? 3 : 4);
        }
        while (budget && job_i < 15) {
            if (land_job <= 2) job_buf[job_i] = world_mt(job_c, (uint16_t)(land_y0 + job_i));
            else job_buf[job_i] = world_mt((uint16_t)(land_x0 + job_i), job_c);
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
