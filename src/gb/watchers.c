/* watchers.c - OPEN WORLD Watchers: tall pale figures that stand at night and in the Ash.
 * They turn to face your light; while the lantern shows and you are not hidden (tall grass,
 * undergrowth) they drift closer, very slowly. Their touch is a whiteout. Never explained.
 * One at a time (16x32 = 4 sprites, 2 per line: the land's line budget is 2 player + 4 glow
 * + 1 hint + 2 watcher + 1 weather). Runs from world_frame (ISR): cache reads only. */
#pragma bank 255
#include <gb/gb.h>
#include "game.h"
#include "gfx.h"
#include "land.h"
#include "assets.h"
#include "sound.h"

uint8_t watch_on;             /* 1 while a Watcher stands */
uint16_t watch_mx, watch_my;  /* its foot: metatile + pixel (like the wanderer) */
uint8_t watch_sx, watch_sy;
static uint8_t watch_t, watch_fade, watch_step;
static uint16_t wrng = 0x7A3D;

static uint8_t wr8(void)
{
    uint16_t x = wrng;
    x ^= (uint16_t)(x << 7);
    x ^= (uint16_t)(x >> 9);
    x ^= (uint16_t)(x << 8);
    wrng = x;
    return (uint8_t)x;
}

void watchers_reset(void) BANKED
{
    uint8_t i;
    watch_on = 0;
    watch_t = 0;
    for (i = 0; i < 4; i++) spr_hide((uint8_t)(SP_WATCH + i));
}

static int16_t rel_x(void)
{
    return (int16_t)((int16_t)(watch_mx - pl_mx) * 16 + (int16_t)watch_sx - (int16_t)pl_sx);
}

static int16_t rel_y(void)
{
    return (int16_t)((int16_t)(watch_my - pl_my) * 16 + (int16_t)watch_sy - (int16_t)pl_sy);
}

static void try_spawn(void)
{
    static const int8_t ox[8] = { 0, 4, 5, 4, 0, -4, -5, -4 };
    static const int8_t oy[8] = { -3, -3, 0, 3, 4, 3, 0, -3 };
    uint8_t d = (uint8_t)(wr8() & 7), m;
    m = land_rel(ox[d], oy[d]);
    if (mt_flags[m] & MTF_SOLID) return;
    watch_mx = (uint16_t)(pl_mx + ox[d]);
    watch_my = (uint16_t)(pl_my + oy[d]);
    watch_sx = 8;
    watch_sy = 12;
    watch_on = 1;
    watch_t = 0;
    watch_fade = 60;
}

void watchers_update(void) BANKED
{
    uint8_t i, lit, hidden, pal, flip, x, y;
    int16_t rx, ry, sx, sy;
    uint8_t ash = (uint8_t)(biome_here == B_ASH);
    uint8_t night = (uint8_t)(phase == PH_NIGHT);

    if (!watch_on) {
        /* called every 16 frames while none stands: a roll every ~4 s; likelier in the Ash */
        if (!(night || ash) || pl_state == PL_SLEEP) return;
        watch_t = (uint8_t)(watch_t + 16);
        if (watch_t == 0 && (wr8() & (ash ? 1 : 3)) == 0) try_spawn();
        return;
    }
    rx = rel_x();
    ry = rel_y();
    /* gone at dawn, or when left far behind */
    if ((!night && !ash) || rx > 150 || rx < -150 || ry > 110 || ry < -110) { watchers_reset(); return; }
    lit = glow_on;
    hidden = (uint8_t)((mt_flags[land_rel(0, 0)] & MTF_HIDE) != 0);
    if (watch_fade) watch_fade--;
    /* drift towards the light, one pixel every few frames */
    if (lit && !hidden && !watch_fade && pl_state != PL_SLEEP) {
        if (++watch_step >= 5) {
            int8_t v;
            watch_step = 0;
            if (rx > 1) { v = (int8_t)(watch_sx - 1); if (v < 0) { v += 16; watch_mx--; } watch_sx = (uint8_t)v; }
            else if (rx < -1) { v = (int8_t)(watch_sx + 1); if (v >= 16) { v -= 16; watch_mx++; } watch_sx = (uint8_t)v; }
            if (ry > 1) { v = (int8_t)(watch_sy - 1); if (v < 0) { v += 16; watch_my--; } watch_sy = (uint8_t)v; }
            else if (ry < -1) { v = (int8_t)(watch_sy + 1); if (v >= 16) { v -= 16; watch_my++; } watch_sy = (uint8_t)v; }
        }
    }
    /* the touch */
    if (rx < 7 && rx > -7 && ry < 7 && ry > -7 && pl_state != PL_GLIDE && pl_state != PL_SLEEP) {
        warmth = 0;
        watchers_reset();
        return;
    }
    /* draw: 16x32, foot at the bottom centre; faces the wanderer */
    sx = (int16_t)(80 + rx - 8);
    sy = (int16_t)(24 + 68 + ry - 31);
    if (sx < -16 || sx > 160 || sy < 24 || sy > 144 || (watch_fade && (watch_fade & 4))) {
        for (i = 0; i < 4; i++) spr_hide((uint8_t)(SP_WATCH + i));
        return;
    }
    pal = is_cgb ? OPAL_WATCHER : S_PALETTE;
    flip = (uint8_t)(rx > 0 ? S_FLIPX : 0);   /* to the right of you: it looks left */
    x = (uint8_t)(sx + 8 + shake_x);
    y = (uint8_t)(sy + 16);
    if (flip) {
        spr_set(SP_WATCH + 0, x, y, SPR_WATCHER + 2, (uint8_t)(pal | flip));
        spr_set(SP_WATCH + 1, (uint8_t)(x + 8), y, SPR_WATCHER, (uint8_t)(pal | flip));
        spr_set(SP_WATCH + 2, x, (uint8_t)(y + 16), SPR_WATCHER + 6, (uint8_t)(pal | flip));
        spr_set(SP_WATCH + 3, (uint8_t)(x + 8), (uint8_t)(y + 16), SPR_WATCHER + 4, (uint8_t)(pal | flip));
    } else {
        spr_set(SP_WATCH + 0, x, y, SPR_WATCHER, pal);
        spr_set(SP_WATCH + 1, (uint8_t)(x + 8), y, SPR_WATCHER + 2, pal);
        spr_set(SP_WATCH + 2, x, (uint8_t)(y + 16), SPR_WATCHER + 4, pal);
        spr_set(SP_WATCH + 3, (uint8_t)(x + 8), (uint8_t)(y + 16), SPR_WATCHER + 6, pal);
    }
}
