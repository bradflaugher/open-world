/* life.c - OPEN WORLD: small reasons to wander and small lives in the land.
 *
 * Wisps: lights that hang over the ground: one in four 16x16-cell chunks has one, at a place
 * fixed by the seed, and it shows if the ground there is open (about one chunk in eight). Walk into one and it rises into the sky: it is a star from then on
 * (the night sky starts almost empty and fills with what you gathered, in every later world
 * too). The nearest one shows as a glint on the horizon band, so there is always something to
 * walk towards.
 *
 * Birds and fireflies: a bird stands in the open by day and takes off when you come near;
 * fireflies drift at night. They borrow two of the weather's sprites, so they are only out
 * when it is not raining or snowing.
 *
 * wisp_task runs in the main loop (it asks the world core where the ground is open);
 * life_update runs in the VBlank game frame and reads the land cache only. */
#pragma bank 255
#define WORLD_INTERNAL          /* w_perm */
#include <gb/gb.h>
#include <string.h>
#include "game.h"
#include "gfx.h"
#include "land.h"
#include "assets.h"
#include "sound.h"

uint8_t stars;                       /* wisps gathered, ever (saved) */
volatile uint8_t stars_new;          /* one was just gathered: the sky is redrawn */
uint16_t wisp_got_x[WISP_KEEP], wisp_got_y[WISP_KEEP];   /* chunks emptied in this world */
uint8_t wisp_got_n, wisp_got_i;
volatile uint8_t wisp_on;            /* wisp_x / wisp_y hold the nearest one */
uint16_t wisp_x, wisp_y;
uint8_t wisp_rise;                   /* gathered: it is rising (pixels so far) */
static uint16_t wisp_px, wisp_py;    /* its cell's top-left in world pixels */
static volatile uint8_t wisp_dirty = 1;

/* search state (main loop) */
#define NCAND 4
static uint8_t sc_row = 7;          /* 0-6: rows of chunks; 7: idle; 8+: checking candidate sc_row - 8 */
static uint16_t sc_cx, sc_cy, sc_px, sc_py;
static uint16_t cand_x[NCAND], cand_y[NCAND];
static uint8_t cand_d[NCAND], cand_n;

/* four rounds through the core's permutation table (read-only: unlike the core's own hash this
   keeps no state, so it is safe beside it). Part of what a seed means: see wisp_roll. */
static uint8_t mix8(uint16_t a, uint16_t b)
{
    uint8_t h = w_perm[(uint8_t)((uint8_t)a ^ 0xA7)];
    h = w_perm[(uint8_t)(h ^ (uint8_t)b)];
    h = w_perm[(uint8_t)(h ^ (uint8_t)(a >> 8))];
    return w_perm[(uint8_t)(h ^ (uint8_t)(b >> 8))];
}

/* the wisp of chunk (cx, cy) (12-bit chunk coordinates): 1 and its cell, or 0 */
static uint8_t wisp_roll(uint16_t cx, uint16_t cy, uint16_t *x, uint16_t *y)
{
    uint8_t h = mix8((uint16_t)(cx ^ world.seed), (uint16_t)(cy + 0x3C6Eu)), g;
    if ((h & 3) != 3) return 0;
    g = mix8((uint16_t)(cy ^ (world.seed >> 3)), (uint16_t)(cx + 0x1F2Du));
    *x = (uint16_t)((cx << 4) + 2 + (g & 7) + ((h >> 3) & 3));
    *y = (uint16_t)((cy << 4) + 2 + ((g >> 3) & 7) + ((g >> 6) & 3));
    return 1;
}

static uint8_t wisp_taken(uint16_t cx, uint16_t cy)
{
    uint8_t i;
    for (i = 0; i < wisp_got_n; i++)
        if (wisp_got_x[i] == cx && wisp_got_y[i] == cy) return 1;
    return 0;
}

void wisps_clear(void) BANKED
{
    wisp_got_n = wisp_got_i = 0;
    wisp_on = 0;
    wisp_rise = 0;
    sc_row = 7;
    wisp_dirty = 1;
}

/* search again from where the wanderer is now (a wake, a screen that was left). The wisp in
   hand stays until the new search is done. */
void wisps_rescan(void) BANKED
{
    sc_row = 7;
    wisp_dirty = 1;
}

static uint8_t open_at(uint16_t x, uint16_t y)
{
    return (uint8_t)!(mt_flags[world_mt(x, y)] & MTF_SOLID);
}

/* Main loop, a little per call so the land streamer keeps the CPU (and the world core's
 * caches): first the wisps of the 7 x 7 chunks round the wanderer, one row of chunks per call,
 * keeping the four nearest; then those, nearest first, one per call, until one hangs over open
 * ground with open ground beside it (not over water, a tree or a crag). That one is published. */
void wisp_task(void) BANKED
{
    uint16_t mx, my, cx, cy, x, y, dx, dy;
    uint8_t i, k, d;
    if (sc_row == 7) {
        __critical { mx = pl_mx; my = pl_my; }
        /* again once the wanderer is a chunk's width from where the last search began */
        if (!wisp_dirty && (uint16_t)(mx - sc_px + 16) <= 32u && (uint16_t)(my - sc_py + 16) <= 32u) return;
        wisp_dirty = 0;
        sc_cx = (uint16_t)(mx >> 4); sc_cy = (uint16_t)(my >> 4); sc_px = mx; sc_py = my;
        sc_row = 0;
        cand_n = 0;
        return;
    }
    if (sc_row < 7) {
        cy = (uint16_t)((sc_cy - 3 + sc_row) & 0x0FFF);
        for (i = 0; i < 7; i++) {
            cx = (uint16_t)((sc_cx - 3 + i) & 0x0FFF);
            if (!wisp_roll(cx, cy, &x, &y)) continue;
            if (wisp_taken(cx, cy)) continue;
            dx = (uint16_t)(x - sc_px); if (dx & 0x8000) dx = (uint16_t)-dx;
            dy = (uint16_t)(y - sc_py); if (dy & 0x8000) dy = (uint16_t)-dy;
            if (dy > dx) dx = dy;
            d = dx > 250 ? 250 : (uint8_t)dx;
            /* insert by distance (an equal one stays behind those already there) */
            for (k = cand_n; k && cand_d[k - 1] > d; k--) {
                if (k < NCAND) { cand_d[k] = cand_d[k - 1]; cand_x[k] = cand_x[k - 1]; cand_y[k] = cand_y[k - 1]; }
            }
            if (k < NCAND) {
                cand_d[k] = d; cand_x[k] = x; cand_y[k] = y;
                if (cand_n < NCAND) cand_n++;
            }
        }
        sc_row++;
        if (sc_row == 7) sc_row = 8;
        return;
    }
    /* the candidates, nearest first */
    k = (uint8_t)(sc_row - 8);
    if (k < cand_n) {
        x = cand_x[k]; y = cand_y[k];
        if (!wisp_taken((uint16_t)(x >> 4), (uint16_t)(y >> 4)) && open_at(x, y) &&
            (open_at((uint16_t)(x + 1), y) || open_at((uint16_t)(x - 1), y) ||
             open_at(x, (uint16_t)(y + 1)) || open_at(x, (uint16_t)(y - 1)))) {
            sc_row = 7;
            __critical {
                if (!wisp_rise) {
                    wisp_x = x; wisp_y = y;
                    wisp_px = (uint16_t)(x << 4); wisp_py = (uint16_t)(y << 4);
                    wisp_on = 1;
                } else wisp_dirty = 1;      /* one is rising: look again once it is gone */
            }
            return;
        }
        sc_row++;
        return;
    }
    /* none of them */
    sc_row = 7;
    __critical { if (!wisp_rise) wisp_on = 0; else wisp_dirty = 1; }
}

/* ---------------------------------------------------------------- the VBlank game frame */
#define NCRIT 2
enum { CR_NONE = 0, CR_BIRD, CR_FLY, CR_FIREFLY };
static uint8_t ck[NCRIT], ct[NCRIT];
static int8_t cv[NCRIT];
static uint16_t cpx[NCRIT], cpy[NCRIT];
static uint8_t life_cd, life_owns;
static uint16_t lrng = 0xC0DE;
uint8_t dbg_birds, dbg_flushed, dbg_fireflies;     /* spawned / flown (tests) */

static uint8_t lr8(void)
{
    uint16_t x = lrng;
    x ^= (uint16_t)(x << 7);
    x ^= (uint16_t)(x >> 9);
    x ^= (uint16_t)(x << 8);
    lrng = x;
    return (uint8_t)x;
}

void life_reset(void) BANKED
{
    uint8_t i;
    for (i = 0; i < NCRIT; i++) ck[i] = CR_NONE;
    life_cd = 60;
    life_owns = 0;
    lrng ^= (uint16_t)(((uint16_t)DIV_REG << 8) | vbl_frames);
    if (!lrng) lrng = 0xC0DE;
    spr_hide(SP_WISP);
}

static void wisp_frame(void)
{
    uint16_t x, y;
    int16_t dx, dy;
    uint8_t sy;
    if (!wisp_on) { spr_hide(SP_WISP); return; }
    x = (uint16_t)(wisp_px - cam_px);
    y = (uint16_t)(wisp_py - cam_py);
    if (!wisp_rise && pl_state != PL_SLEEP) {
        /* touch: the foot within a cell's reach of the light */
        dx = (int16_t)(wisp_px + 8 - pl_px);
        dy = (int16_t)(wisp_py + 10 - pl_py);
        if (dx > -15 && dx < 15 && dy > -15 && dy < 15) {
            uint8_t k = wisp_got_i;
            wisp_got_x[k] = (uint16_t)(wisp_x >> 4);
            wisp_got_y[k] = (uint16_t)(wisp_y >> 4);
            wisp_got_i = (uint8_t)((k + 1) & (WISP_KEEP - 1));
            if (wisp_got_n < WISP_KEEP) wisp_got_n++;
            if (stars < 255) stars++;
            stars_new = 1;
            wisp_rise = 1;
            sfx_play(SFX_WISP);
            save_req = 1;
        }
    }
    /* on screen? (a cell of margin left and above) */
    if ((uint16_t)(x + 16) >= 176u || (uint16_t)(y + 16) >= 136u) {
        spr_hide(SP_WISP);
        if (wisp_rise) { wisp_rise = 0; wisp_on = 0; wisp_dirty = 1; }
        return;
    }
    sy = (uint8_t)((uint8_t)y + 24 + 16 - 4 - ((vbl_frames >> 4) & 1));
    if (wisp_rise) {
        /* up into the sky, faster and faster; it ends as a star */
        wisp_rise = (uint8_t)(wisp_rise + 2 + (wisp_rise >> 3));
        if (wisp_rise >= sy || (uint8_t)(sy - wisp_rise) < 16 + 2 || wisp_rise > 200) {
            wisp_rise = 0;
            wisp_on = 0;
            wisp_dirty = 1;
            spr_hide(SP_WISP);
            return;
        }
        sy = (uint8_t)(sy - wisp_rise);
    }
    spr_set(SP_WISP, (uint8_t)((uint8_t)x + 4 + 8), sy, (vbl_frames & 16) ? SPR_WISP1 : SPR_WISP0,
            (uint8_t)(is_cgb ? OPAL_LIGHT : S_PALETTE));
}

static uint8_t open_ground(uint8_t m)
{
    return (uint8_t)(m == MT_GRASS || m == MT_FLOWERS || m == MT_SAND || m == MT_SNOW ||
                     m == MT_DUNE || m == MT_ROAD || m == MT_RUIN_FLOOR);
}

static void life_spawn(void)
{
    static const int8_t bx[8] = { -5, 5, -4, 4, 0, -3, 3, 2 };
    static const int8_t by[8] = { -2, -2, 3, 3, -3, -3, 3, 4 };
    uint8_t i, r = lr8(), m;
    int8_t dx, dy;
    if (pl_state == PL_SLEEP) return;
    for (i = 0; i < NCRIT; i++) if (ck[i] == CR_NONE) break;
    if (i == NCRIT) return;
    if (glow_on) {
        /* night: fireflies over the grass, anywhere in view */
        if (r & 1) return;
        dx = (int8_t)((int8_t)((r >> 1) & 7) - 4);
        dy = (int8_t)((int8_t)((r >> 4) & 7) - 4);
        m = land_rel(dx, dy);
        if (!(mt_flags[m] & MTF_HIDE) && m != MT_GRASS && m != MT_FLOWERS) return;
        ck[i] = CR_FIREFLY;
        ct[i] = 255;
        dbg_fireflies++;
    } else {
        /* day: one bird at a time, standing in the open near the edge of the view */
        if (phase == PH_NIGHT || (r & 3)) return;
        if (ck[0] == CR_BIRD || ck[1] == CR_BIRD) return;
        r = (uint8_t)((r >> 2) & 7);
        dx = bx[r]; dy = by[r];
        m = land_rel(dx, dy);
        if (!open_ground(m)) return;
        ck[i] = CR_BIRD;
        ct[i] = 0;
        cv[i] = (int8_t)(dx < 0 ? 1 : -1);        /* it looks towards you */
        dbg_birds++;
    }
    cpx[i] = (uint16_t)(((uint16_t)(pl_mx + dx) << 4) + 8);
    cpy[i] = (uint16_t)(((uint16_t)(pl_my + dy) << 4) + 10);
}

void life_update(void) BANKED
{
    uint8_t i, t, tile, prop, hide;
    uint16_t x, y;
    int16_t dx, dy;
    uint8_t *o;
    wisp_frame();
    /* the weather owns its sprites while anything falls */
    if (weather == WX_RAIN || weather == WX_STORM || weather == WX_SNOW) {
        if (life_owns) { life_owns = 0; ck[0] = ck[1] = CR_NONE; }
        return;
    }
    life_owns = 1;
    if (life_cd) life_cd--;
    else if ((vbl_frames & 31) == 17) life_spawn();
    for (i = 0; i < NCRIT; i++) {
        o = &oam[(uint8_t)((SP_WX + i) << 2)];
        if (ck[i] == CR_NONE) { o[0] = 0; continue; }
        t = ++ct[i];
        hide = 0;
        prop = (uint8_t)(is_cgb ? OPAL_PLAYER : 0);
        switch (ck[i]) {
        case CR_BIRD:
            tile = SPR_BIRD_SIT;
            if ((t & 63) == 0 && (lr8() & 1)) cv[i] = (int8_t)-cv[i];     /* it looks about */
            dx = (int16_t)(cpx[i] - pl_px);
            dy = (int16_t)(cpy[i] - pl_py);
            if (dx > -34 && dx < 34 && dy > -30 && dy < 30) {
                ck[i] = CR_FLY;
                cv[i] = (int8_t)(dx < 0 ? -2 : 2);                         /* away from you */
                ct[i] = 0;
                sfx_play(SFX_FLUTTER);
                dbg_flushed++;
            }
            if (cv[i] < 0) prop |= S_FLIPX;
            break;
        case CR_FLY:
            tile = (t & 4) ? SPR_BIRD1 : SPR_BIRD0;
            cpx[i] = (uint16_t)(cpx[i] + cv[i]);
            cpy[i] = (uint16_t)(cpy[i] - (t < 8 ? 3 : 2));
            break;
        default:
            tile = SPR_FIREFLY;
            prop = (uint8_t)(is_cgb ? OPAL_LIGHT : S_PALETTE);
            if ((t & 3) == 0) {
                cpx[i] = (uint16_t)(cpx[i] + ((t & 0x40) ? 1 : -1));
                cpy[i] = (uint16_t)(cpy[i] + ((t & 0x20) ? 1 : -1));
            }
            hide = (uint8_t)((t & 0x38) == 0x38);                          /* it blinks */
            if (t == 255 || !glow_on) { ck[i] = CR_NONE; life_cd = 30; }
            break;
        }
        x = (uint16_t)(cpx[i] - cam_px);
        y = (uint16_t)(cpy[i] - cam_py);
        /* gone once out of view (a flying bird also when it reaches the sky) */
        if ((uint16_t)(x + 8) >= 176u || (uint16_t)(y + 16) >= 152u || (ck[i] == CR_FLY && (uint8_t)y < 14)) {
            if (ck[i] != CR_NONE) life_cd = (uint8_t)(90 + (lr8() & 127));
            ck[i] = CR_NONE;
            o[0] = 0;
            continue;
        }
        t = (uint8_t)((uint8_t)y + 24 + 16 - 15);       /* the sprite's OAM y: feet on (x, y) */
        /* with a Watcher on screen the rows under the lantern glow are full (10 per line) */
        if (hide || (watch_on && glow_on && t >= 85 - 32 + 16 && t < 85 + 16 + 16)) { o[0] = 0; continue; }
        o[0] = t; o[1] = (uint8_t)((uint8_t)x + 8 - 4); o[2] = tile; o[3] = prop;
    }
}
