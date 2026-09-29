/* player.c - OPEN WORLD: the wanderer. 8-way movement with a sub-metatile hitbox, wall
 * sliding and corner forgiveness, run, slow ground, sit, glide, and the A verbs. */
#pragma bank 255
#include <gb/gb.h>
#include <string.h>
#include "game.h"
#include "gfx.h"
#include "land.h"
#include "sound.h"
#include "assets.h"

/* hitbox relative to the foot point */
#define HB_L (-5)
#define HB_R 4
#define HB_T (-5)
#define HB_B 0

static const int8_t dir_dx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
static const int8_t dir_dy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

static uint8_t acc_x, acc_y, walk_px, step_px;
static uint8_t glide_f;
static int16_t glide_dx, glide_dy;
static uint16_t glide_mx0, glide_my0;
static uint8_t glide_sx0, glide_sy0;
static uint8_t burn_t;
static uint16_t burn_x, burn_y;
static uint16_t tgt_x, tgt_y;         /* the cell the wanderer faces */
static uint8_t tgt_mt;
static uint16_t act_x, act_y;         /* nearby interactable (fire, beacon, shrine, heart) */
static uint8_t act_mt;
static uint8_t hint_bob;
uint8_t ending_req;
extern uint8_t dbg_ly[10];

uint8_t blocked_mt(uint8_t mt) BANKED
{
    return (uint8_t)(mt_flags[mt] & MTF_SOLID);
}

/* metatile under foot + (ox, oy) pixels */
static uint8_t mt_off(int8_t ox, int8_t oy)
{
    int8_t vx = (int8_t)(pl_sx + ox), vy = (int8_t)(pl_sy + oy);
    return land_rel((int8_t)(vx >> 4), (int8_t)(vy >> 4));
}

#define box_free(ox, oy) land_box_free((int8_t)(ox), (int8_t)(oy))

static void foot_add(int8_t dx, int8_t dy)
{
    int8_t v;
    if (dx) {
        v = (int8_t)(pl_sx + dx);
        if (v >= 16) { v -= 16; pl_mx++; }
        else if (v < 0) { v += 16; pl_mx--; }
        pl_sx = (uint8_t)v;
    }
    if (dy) {
        v = (int8_t)(pl_sy + dy);
        if (v >= 16) { v -= 16; pl_my++; }
        else if (v < 0) { v += 16; pl_my--; }
        pl_sy = (uint8_t)v;
    }
}

void player_place(uint16_t mx, uint16_t my) BANKED
{
    pl_mx = mx;
    pl_my = my;
    pl_sx = 8;
    pl_sy = 12;
}

/* one pixel along x (sx = +-1); slides round corners when the way is only partly blocked */
static uint8_t step_x(int8_t sx, uint8_t pure)
{
    int8_t k;
    if (box_free(sx, 0)) { foot_add(sx, 0); return 1; }
    if (!pure) return 0;
    for (k = 1; k <= 6; k++) {
        if (box_free(sx, (int8_t)-k) && box_free(0, -1)) { foot_add(0, -1); return 1; }
        if (box_free(sx, k) && box_free(0, 1)) { foot_add(0, 1); return 1; }
    }
    return 0;
}

static uint8_t step_y(int8_t sy, uint8_t pure)
{
    int8_t k;
    if (box_free(0, sy)) { foot_add(0, sy); return 1; }
    if (!pure) return 0;
    for (k = 1; k <= 6; k++) {
        if (box_free((int8_t)-k, sy) && box_free(-1, 0)) { foot_add(-1, 0); return 1; }
        if (box_free(k, sy) && box_free(1, 0)) { foot_add(1, 0); return 1; }
    }
    return 0;
}

static void footstep(void)
{
    uint8_t m = mt_off(0, -2), s;
    switch (m) {
    case MT_SAND: case MT_DUNE: case MT_BONES: case MT_ASH: s = SFX_STEP_SAND; break;
    case MT_SNOW: s = SFX_STEP_SNOW; break;
    case MT_ROAD: case MT_RUIN_FLOOR: case MT_STEPSTONE: case MT_GLASS: s = SFX_STEP_STONE; break;
    default: s = SFX_STEP_SOFT; break;
    }
    sfx_play(s);
}

/* ---- targets ---- */
static uint16_t ft_mx, ft_my;
static uint8_t ft_sx, ft_sy, ft_face = 0xFF, ft_age, ft_near = 0xFF;
static void find_targets(void)
{
    int8_t dx = dir_dx[pl_face], dy = dir_dy[pl_face];
    int8_t vx, vy;
    uint8_t i, m, cell_moved;
    cell_moved = (uint8_t)(ft_mx != pl_mx || ft_my != pl_my || land_changed || ++ft_age >= 32);
    if (!cell_moved && ft_sx == pl_sx && ft_sy == pl_sy && ft_face == pl_face) return;
    ft_sx = pl_sx; ft_sy = pl_sy; ft_face = pl_face;
    /* probe 7 px beyond the hitbox edge in the facing direction */
    vx = (int8_t)(pl_sx + (dx > 0 ? HB_R + 8 : dx < 0 ? HB_L - 8 : 0));
    vy = (int8_t)(pl_sy + (dy > 0 ? HB_B + 8 : dy < 0 ? HB_T - 8 : -3));
    vx >>= 4;
    vy >>= 4;
    tgt_x = (uint16_t)(pl_mx + vx);
    tgt_y = (uint16_t)(pl_my + vy);
    tgt_mt = land_rel(vx, vy);
    m = tgt_mt;
    if (m == MT_FIRE_COLD || m == MT_BEACON || m == MT_SHRINE || m == MT_HEART) {
        act_x = tgt_x; act_y = tgt_y; act_mt = m;
        ft_near = 0xFE;
        return;
    }
    /* things you light or take respond from any neighbouring cell */
    if (cell_moved || ft_near == 0xFE) {
        ft_mx = pl_mx; ft_my = pl_my; ft_age = 0; land_changed = 0;
        ft_near = land_near_act();
    }
    act_mt = 0xFF;
    if (ft_near < 8) {
        i = ft_near;
        act_x = (uint16_t)(pl_mx + dir_dx[i]);
        act_y = (uint16_t)(pl_my + dir_dy[i]);
        act_mt = land_rel(dir_dx[i], dir_dy[i]);
    }
}

/* the cell overlapped by the wanderer's own hitbox? */
static uint8_t under_me(uint16_t x, uint16_t y)
{
    uint16_t x0 = (uint16_t)(pl_mx + (((int16_t)pl_sx + HB_L) >> 4));
    uint16_t x1 = (uint16_t)(pl_mx + (((int16_t)pl_sx + HB_R) >> 4));
    uint16_t y0 = (uint16_t)(pl_my + (((int16_t)pl_sy + HB_T) >> 4));
    uint16_t y1 = (uint16_t)(pl_my + (((int16_t)pl_sy + HB_B) >> 4));
    return (uint8_t)((x == x0 || x == x1) && (y == y0 || y == y1));
}

static uint8_t own_cairn(uint16_t x, uint16_t y)
{
    uint8_t i;
    for (i = 0; i < cairn_n; i++)
        if (cairns[i].x == x && cairns[i].y == y) return (uint8_t)(i + 1);
    return 0;
}

static uint8_t glide_ok(void)
{
    int8_t dx = dir_dx[pl_face], dy = dir_dy[pl_face];
    uint8_t k, m;
    for (k = 1; k <= 2; k++) {
        m = land_mt((uint16_t)(pl_mx + dx * k), (uint16_t)(pl_my + dy * k));
        if (!(mt_flags[m] & MTF_GLIDE) && blocked_mt(m)) return 0;
    }
    m = land_mt((uint16_t)(pl_mx + dx * 3), (uint16_t)(pl_my + dy * 3));
    return (uint8_t)!blocked_mt(m);
}

/* what A would do right now (for the hint): 0 nothing */
static uint8_t can_act(void)
{
    uint8_t m = tgt_mt;
    if (act_mt != 0xFF) {
        if (act_mt == MT_HEART) return heart_revealed;
        return 1;
    }
    switch (equipped) {
    case IT_LANTERN:
        return (uint8_t)(m == MT_BRAMBLE);
    case IT_STONES:
        if (m == MT_SHALLOW) return (uint8_t)(stones != 0);
        if (m == MT_CAIRN && own_cairn(tgt_x, tgt_y)) return 1;
        return 0;
    case IT_CLOAK:
        return (uint8_t)(blocked_mt(m) && (mt_flags[m] & MTF_GLIDE) && glide_ok());
    }
    return 0;
}

static void mod_remove(uint16_t x, uint16_t y)
{
    uint8_t i;
    for (i = 0; i < world_mod_count; i++) {
        if (world_mods[i].x == x && world_mods[i].y == y) {
            world_mod_count--;
            world_mods[i] = world_mods[world_mod_count];
            world_mods_rebuild();
            break;
        }
    }
    land_set(x, y, world_mt(x, y));
}

void light_beacon(uint8_t i) BANKED;

static void act(void)
{
    uint8_t m, i;
    ft_face = 0xFF;
    ft_age = 32;
    find_targets();
    m = tgt_mt;
    if (act_mt != 0xFF) {
        switch (act_mt) {
        case MT_FIRE_COLD:
            edit_mt(act_x, act_y, MT_FIRE_LIT);
            respawn_x = act_x;
            respawn_y = act_y;
            sfx_play(SFX_LIGHT);
            shake = 2;
            save_write();
            return;
        case MT_BEACON:
            for (i = 0; i < NUM_BEACONS; i++)
                if (world.beacon[i].x == act_x && world.beacon[i].y == act_y) light_beacon(i);
            return;
        case MT_SHRINE:
            for (i = 0; i < 2; i++) {
                if (world.shrine[i].x == act_x && world.shrine[i].y == act_y) {
                    uint8_t it = (uint8_t)(i == 0 ? IT_STONES : IT_CLOAK);
                    items |= (uint8_t)(1 << it);
                    equipped = it;
                    if (it == IT_STONES) stones = STONES_MAX;
                    edit_mt(act_x, act_y, MT_SHRINE_EMPTY);
                    sfx_play(SFX_ITEM);
                    item_pulse = 120;
                    save_write();
                    return;
                }
            }
            /* a shrine that is not in the layout: empty it anyway */
            edit_mt(act_x, act_y, MT_SHRINE_EMPTY);
            return;
        case MT_HEART:
            if (heart_revealed) ending_req = 1;
            else { sfx_play(SFX_NO); shake = 3; }
            return;
        }
    }
    switch (equipped) {
    case IT_LANTERN:
        if (m == MT_BRAMBLE && !burn_t) {
            burn_x = tgt_x;
            burn_y = tgt_y;
            burn_t = 40;
            land_set(burn_x, burn_y, MT_FIRE_LIT);
            sfx_play(SFX_BURN);
            return;
        }
        break;
    case IT_STONES:
        if (m == MT_CAIRN && (i = own_cairn(tgt_x, tgt_y)) != 0) {
            i--;
            cairn_n--;
            cairns[i] = cairns[cairn_n];
            mod_remove(tgt_x, tgt_y);
            if (stones < STONES_MAX) stones++;
            sfx_play(SFX_PICKUP);
            save_write();
            return;
        }
        if (!stones) break;
        if (m == MT_SHALLOW) {
            edit_mt(tgt_x, tgt_y, MT_STEPSTONE);
            stones--;
            sfx_play(SFX_STEPSTONE);
            save_write();
            return;
        }
        if (!blocked_mt(m) && m != MT_STEPSTONE && m != MT_ROAD && !under_me(tgt_x, tgt_y) &&
            world_mod_count < MAX_MODS && cairn_n < MAX_CAIRNS) {
            edit_mt(tgt_x, tgt_y, MT_CAIRN);
            cairns[cairn_n].x = tgt_x;
            cairns[cairn_n].y = tgt_y;
            cairn_n++;
            stones--;
            sfx_play(SFX_CAIRN);
            save_write();
            return;
        }
        break;
    case IT_CLOAK:
        if (glide_ok()) {
            int8_t dx = dir_dx[pl_face], dy = dir_dy[pl_face];
            glide_mx0 = pl_mx; glide_my0 = pl_my;
            glide_sx0 = pl_sx; glide_sy0 = pl_sy;
            glide_dx = (int16_t)(dx * 48 + (8 - (int8_t)pl_sx));
            glide_dy = (int16_t)(dy * 48 + (12 - (int8_t)pl_sy));
            glide_f = 0;
            pl_state = PL_GLIDE;
            sfx_play(SFX_GLIDE);
            return;
        }
        break;
    }
    sfx_play(SFX_NO);
    shake = 3;
}

static void glide_tick(void)
{
    int16_t ox, oy, vx, vy;
    glide_f++;
    ox = (int16_t)((glide_dx * (int16_t)glide_f) >> 5);
    oy = (int16_t)((glide_dy * (int16_t)glide_f) >> 5);
    vx = (int16_t)glide_sx0 + ox;
    vy = (int16_t)glide_sy0 + oy;
    pl_mx = (uint16_t)(glide_mx0 + (vx >> 4));
    pl_my = (uint16_t)(glide_my0 + (vy >> 4));
    pl_sx = (uint8_t)(vx & 15);
    pl_sy = (uint8_t)(vy & 15);
    pl_lift = (int8_t)(((uint16_t)glide_f * (uint16_t)(32 - glide_f)) >> 5);
    if (glide_f >= 32) {
        pl_lift = 0;
        pl_state = PL_STAND;
        sfx_play(SFX_LAND);
        shake = 1;
        idle_t = 0;
    }
}

void player_update(void) BANKED
{
    int8_t dx = 0, dy = 0;
    uint8_t spd, n, moved = 0, pure, m;

    if (burn_t) {
        burn_t--;
        if (burn_t == 0) edit_mt(burn_x, burn_y, MT_ASH);
    }
    if (pl_state == PL_GLIDE) {
        glide_tick();
        hint_on = 0;
        return;
    }
    dbg_ly[1] = LY_REG;
    if (keys & J_LEFT) dx = -1;
    if (keys & J_RIGHT) dx = 1;
    if (keys & J_UP) dy = -1;
    if (keys & J_DOWN) dy = 1;

    if (pl_state == PL_SLEEP) return;       /* game.c wakes us */

    if (dx || dy) {
        if (dy < 0) pl_face = (uint8_t)(dx > 0 ? D_NE : dx < 0 ? D_NW : D_N);
        else if (dy > 0) pl_face = (uint8_t)(dx > 0 ? D_SE : dx < 0 ? D_SW : D_S);
        else pl_face = (uint8_t)(dx > 0 ? D_E : D_W);
        if (pl_state == PL_SIT) ambient_tempo(0);
        pl_state = PL_WALK;
        idle_t = 0;
        spd = (keys & J_B) ? 8 : 4;
        if (dx && dy) spd = (uint8_t)(spd - (spd >> 2));
        m = mt_off(0, -2);
        if (mt_flags[m] & MTF_SLOW) spd >>= 1;
        pure = (uint8_t)!(dx && dy);
        if (dx) {
            acc_x = (uint8_t)(acc_x + spd);
            n = (uint8_t)(acc_x >> 2);
            acc_x &= 3;
            while (n--) if (step_x(dx, pure)) moved++;
        }
        if (dy) {
            acc_y = (uint8_t)(acc_y + spd);
            n = (uint8_t)(acc_y >> 2);
            acc_y &= 3;
            while (n--) if (step_y(dy, pure)) moved++;
        }
        if (moved) {
            walk_px = (uint8_t)(walk_px + moved);
            if (walk_px >= 8) { walk_px -= 8; pl_anim ^= 1; }
            step_px = (uint8_t)(step_px + moved);
            if (step_px >= 16) { step_px -= 16; footstep(); }
        }
    } else {
        if (pl_state == PL_WALK) pl_state = PL_STAND;
        if (idle_t < 255) idle_t++;
        if (pl_state == PL_STAND && idle_t >= 180 && !(keys & (J_A | J_B))) {
            pl_state = PL_SIT;
            sfx_play(SFX_SIT);
            ambient_tempo(1);
        }
    }
    if (keys & (J_A | J_B | J_SELECT)) {
        if (pl_state == PL_SIT) { pl_state = PL_STAND; ambient_tempo(0); }
        idle_t = 0;
    }

    dbg_ly[2] = LY_REG;
    find_targets();
    dbg_ly[3] = LY_REG;
    if (pressed & J_A) {
        if (pl_state == PL_SIT) { pl_state = PL_STAND; ambient_tempo(0); }
        act();
    }
    if (pressed & J_SELECT) {
        uint8_t e = equipped, k;
        for (k = 0; k < IT_COUNT; k++) {
            e = (uint8_t)(e + 1 == IT_COUNT ? 0 : e + 1);
            if (items & (1 << e)) break;
        }
        if (e != equipped) { equipped = e; item_pulse = 30; sfx_play(SFX_SELECT); }
        else sfx_play(SFX_NO);
    }

    /* hint: bobbing pictogram above whatever A would act on */
    dbg_ly[4] = LY_REG;
    hint_on = 0;
    if (pl_state != PL_SIT && can_act()) {
        uint16_t hx = act_mt != 0xFF ? act_x : tgt_x, hy = act_mt != 0xFF ? act_y : tgt_y;
        int16_t sx = (int16_t)((int16_t)(hx - cam_mx) * 16 - cam_sx);
        int16_t sy = (int16_t)((int16_t)(hy - cam_my) * 16 - cam_sy);
        hint_bob++;
        if (sx > -8 && sx < 160 && sy > 8 && sy < 120) {
            hint_on = 1;
            hint_x = (uint8_t)(sx + 4 + 8);
            hint_y = (uint8_t)(sy + 24 - 14 + 16 - ((hint_bob >> 4) & 1));
        }
    }
}

/* ---- sprites ---- */
void player_draw(void) BANKED
{
    uint8_t base, flip = 0, x = 80 - 8 + 8, y, prop, i;
    uint8_t pal = is_cgb ? OPAL_PLAYER : 0;
    switch (pl_state) {
    case PL_SLEEP: base = SPR_PL_SLEEP; break;
    case PL_SIT: base = SPR_PL_SIT; break;
    case PL_GLIDE: base = SPR_PL_GLIDE; break;
    default:
        switch (pl_face) {
        case D_N: base = pl_anim ? SPR_PL_UP1 : SPR_PL_UP0; break;
        case D_S: base = pl_anim ? SPR_PL_DOWN1 : SPR_PL_DOWN0; break;
        case D_NE: case D_E: case D_SE: base = pl_anim ? SPR_PL_SIDE1 : SPR_PL_SIDE0; break;
        default: base = pl_anim ? SPR_PL_SIDE1 : SPR_PL_SIDE0; flip = S_FLIPX; break;
        }
        if (pl_state == PL_STAND) {
            if (base == SPR_PL_UP1) base = SPR_PL_UP0;
            else if (base == SPR_PL_DOWN1) base = SPR_PL_DOWN0;
            else if (base == SPR_PL_SIDE1) base = SPR_PL_SIDE0;
        }
        break;
    }
    if (pl_state == PL_GLIDE && (pl_face == D_W || pl_face == D_NW || pl_face == D_SW)) flip = S_FLIPX;
    y = (uint8_t)(24 + 68 - 15 + 16 - pl_lift);
    x = (uint8_t)(x + shake_x);
    prop = (uint8_t)(pal | flip);
    if (flip) {
        spr_set(SP_PLAYER, x, y, (uint8_t)(base + 2), prop);
        spr_set(SP_PLAYER + 1, (uint8_t)(x + 8), y, base, prop);
    } else {
        spr_set(SP_PLAYER, x, y, base, prop);
        spr_set(SP_PLAYER + 1, (uint8_t)(x + 8), y, (uint8_t)(base + 2), prop);
    }
    /* glide shadow */
    if (pl_state == PL_GLIDE && pl_lift > 1) {
        i = (uint8_t)(24 + 68 - 15 + 16 + 2);
        spr_set(SP_FX, (uint8_t)(x + 4), i, SPR_SHADOW, (uint8_t)(is_cgb ? OPAL_PLAYER : 0));
    } else spr_hide(SP_FX);
    /* hint */
    if (hint_on) spr_set(SP_HINT, hint_x, hint_y, SPR_HINT_A, (uint8_t)(is_cgb ? OPAL_UI : 0));
    else spr_hide(SP_HINT);
}
