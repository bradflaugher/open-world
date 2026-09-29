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
#ifndef DMG_RUN_QPX
#define DMG_RUN_QPX 6       /* DMG running speed in 1/4 px per frame (8 = same as CGB) */
#endif

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
uint16_t tgt_x, tgt_y;         /* the cell the wanderer faces */
uint8_t tgt_mt;
static uint16_t act_x, act_y;         /* nearby interactable (fire, beacon, shrine, heart) */
uint8_t act_mt;
static uint8_t hint_bob;
uint8_t ending_req;
static uint8_t a_buf;          /* A pressed mid-glide: acted on at the landing */
static uint8_t ca_v;           /* can_act(), cached */
static uint8_t sel_v;          /* another item you carry would act (the SELECT hint), cached */
uint8_t run_hint_on, lesson_req;
static uint8_t walk_t, run_t;  /* frames spent walking / running (the run hint) */
#define RUN_HINT_AFTER 150     /* walking this long without ever running shows hold-B */
#define RUN_LEARNT     30      /* running this long retires it for good */


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

/* one pixel along x (sx = +-1); slips round corners when only part of the way is blocked */
static uint8_t step_x(int8_t sx, uint8_t pure)
{
    int8_t k;
    if (box_free(sx, 0)) { foot_add(sx, 0); return 1; }
    if (!pure) return 0;
    k = land_slide_x(sx);
    if (k && box_free(0, k)) { foot_add(0, k); return 1; }
    return 0;
}

static uint8_t step_y(int8_t sy, uint8_t pure)
{
    int8_t k;
    if (box_free(0, sy)) { foot_add(0, sy); return 1; }
    if (!pure) return 0;
    k = land_slide_y(sy);
    if (k && box_free(k, 0)) { foot_add(k, 0); return 1; }
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
static uint8_t ft_sx, ft_sy, ft_face = 0xFF, ft_age, ft_near = 0xFF, ft_new = 1;
static void find_targets(void)
{
    int8_t dx = dir_dx[pl_face], dy = dir_dy[pl_face];
    int8_t vx, vy;
    uint8_t i, m, cell_moved;
    cell_moved = (uint8_t)(ft_mx != pl_mx || ft_my != pl_my || land_changed || ++ft_age >= 32);
    if (!cell_moved && ft_sx == pl_sx && ft_sy == pl_sy && ft_face == pl_face) return;
    ft_new = 1;
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

/* Glide landing: in direction d, the nearest free cell 3 away (else 4, else 2) with only glide-able
 * (or open) ground on the way. The crag ring is two cells thick straight on but thicker at an angle,
 * so a fixed 3-cell hop was refused from many approaches. Returns the distance, 0 if none. */
static const uint8_t glide_try[3] = { 3, 4, 2 };
static uint8_t glide_len(uint8_t d)
{
    int8_t dx = dir_dx[d], dy = dir_dy[d];
    uint8_t k, j, m, far = 0;
    /* cells 1..far that the wanderer passes over (read once, in order) */
    uint8_t over[4];
    for (k = 1; k <= 4; k++) {
        m = land_mt((uint16_t)(pl_mx + dx * (int8_t)k), (uint16_t)(pl_my + dy * (int8_t)k));
        over[k - 1] = m;
        if (!(mt_flags[m] & MTF_GLIDE) && blocked_mt(m)) break;    /* a wall the cloak can't pass */
        far = k;
    }
    for (j = 0; j < 3; j++) {
        k = glide_try[j];
        /* the landing may be the first cell the cloak can't pass (it is solid then: refused) */
        if (k > far + 1 || k > 4) continue;
        if (!blocked_mt(over[k - 1])) return k;
    }
    return 0;
}

static uint8_t glide_dir, glide_k;
/* facing first; on a diagonal, also the two straight directions it is made of */
static uint8_t glide_ok(void)
{
    uint8_t d = pl_face, k;
    if ((k = glide_len(d)) != 0) { glide_dir = d; glide_k = k; return 1; }
    if (d & 1) {
        uint8_t a = (uint8_t)((d + 7) & 7), b = (uint8_t)((d + 1) & 7);
        /* prefer the side that is actually blocked (the one the wanderer is pressing into) */
        if (!blocked_mt(land_rel(dir_dx[a], dir_dy[a]))) { uint8_t t = a; a = b; b = t; }
        if ((k = glide_len(a)) != 0) { glide_dir = a; glide_k = k; return 1; }
        if ((k = glide_len(b)) != 0) { glide_dir = b; glide_k = k; return 1; }
    }
    return 0;
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
        if (m == MT_SHALLOW) return 1;
        if (m == MT_CAIRN && own_cairn(tgt_x, tgt_y)) return 1;
        return 0;
    case IT_CLOAK:
        return (uint8_t)(blocked_mt(m) && (mt_flags[m] & MTF_GLIDE) && glide_ok());
    }
    return 0;
}

/* the item in hand can't act on what you face: would another one you carry? (the SELECT hint) */
static uint8_t other_item_acts(void)
{
    uint8_t m = tgt_mt, e = equipped;
    if (act_mt != 0xFF) return 0;
    if (e != IT_LANTERN && (items & (1 << IT_LANTERN)) && m == MT_BRAMBLE) return 1;
    if (e != IT_STONES && (items & (1 << IT_STONES)) &&
        (m == MT_SHALLOW || (m == MT_CAIRN && own_cairn(tgt_x, tgt_y)))) return 1;
    if (e != IT_CLOAK && (items & (1 << IT_CLOAK)) && blocked_mt(m) && (mt_flags[m] & MTF_GLIDE) &&
        glide_ok()) return 1;
    return 0;
}

void light_beacon(uint8_t i) BANKED;

static void sit_down(void)
{
    pl_state = PL_SIT;
    idle_t = 0;
    sfx_play(SFX_SIT);
    ambient_tempo(near_warm);     /* time runs fast only by a fire */
}

static void act(void)
{
    uint8_t m, i;
    ft_face = 0xFF;
    ft_age = 32;
    find_targets();
    m = tgt_mt;
    /* A at a burning fire or beacon: sit and rest by it */
    if (m == MT_FIRE_LIT || m == MT_BEACON_LIT) { sit_down(); return; }
    if (act_mt != 0xFF) {
        switch (act_mt) {
        case MT_FIRE_COLD:
            if (!edit_mt_r(act_x, act_y, MT_FIRE_LIT, EDIT_RESERVE)) return;
            respawn_x = act_x;
            respawn_y = act_y;
            sfx_play(SFX_LIGHT);
            shake = 2;
            save_req = 1;
            return;
        case MT_BEACON:
            for (i = 0; i < NUM_BEACONS; i++)
                if (world.beacon[i].x == act_x && world.beacon[i].y == act_y) light_beacon(i);
            return;
        case MT_SHRINE:
            for (i = 0; i < 2; i++) {
                if (world.shrine[i].x == act_x && world.shrine[i].y == act_y) {
                    uint8_t it = (uint8_t)(i == 0 ? IT_STONES : IT_CLOAK);
                    if (!edit_mt(act_x, act_y, MT_SHRINE_EMPTY)) return;
                    items |= (uint8_t)(1 << it);
                    equipped = it;
                    if (it == IT_STONES) stones = STONES_MAX;
                    sfx_play(SFX_ITEM);
                    item_pulse = 120;
                    /* the first time ever: a short wordless lesson in how to use it (its hint
                       bit is set once it has played through: see lesson_owed) */
                    if (!(hints & (it == IT_STONES ? HINT_STONES : HINT_CLOAK))) lesson_req = (uint8_t)(it + 1);
                    save_req = 1;
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
        if (m == MT_BRAMBLE && !burn_t && edit_room(0)) {
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
            edit_remove(tgt_x, tgt_y);
            sfx_play(SFX_PICKUP);
            save_req = 1;
            return;
        }
        /* the pouch never runs out: the land is full of stones */
        if (m == MT_SHALLOW) {
            if (!edit_mt(tgt_x, tgt_y, MT_STEPSTONE)) return;
            sfx_play(SFX_STEPSTONE);
            save_req = 1;
            return;
        }
        if (!blocked_mt(m) && m != MT_STEPSTONE && m != MT_ROAD && !under_me(tgt_x, tgt_y) &&
            edit_room(EDIT_RESERVE + 1)) {
            if (cairn_n >= MAX_CAIRNS) {
                /* every cairn is standing: the oldest one is taken down to build this one */
                edit_remove(cairns[0].x, cairns[0].y);
                cairn_n--;
                memmove(cairns, cairns + 1, sizeof(wpos_t) * cairn_n);
            }
            if (!edit_mt_r(tgt_x, tgt_y, MT_CAIRN, EDIT_RESERVE)) return;
            cairns[cairn_n].x = tgt_x;
            cairns[cairn_n].y = tgt_y;
            cairn_n++;
            sfx_play(SFX_CAIRN);
            save_req = 1;
            return;
        }
        break;
    case IT_CLOAK:
        if (glide_ok()) {
            int8_t dx = dir_dx[glide_dir], dy = dir_dy[glide_dir];
            int16_t len = (int16_t)((uint16_t)glide_k << 4);
            pl_face = glide_dir;
            glide_mx0 = pl_mx; glide_my0 = pl_my;
            glide_sx0 = pl_sx; glide_sy0 = pl_sy;
            glide_dx = (int16_t)((dx < 0 ? -len : dx ? len : 0) + (8 - (int8_t)pl_sx));
            glide_dy = (int16_t)((dy < 0 ? -len : dy ? len : 0) + (12 - (int8_t)pl_sy));
            glide_f = 0;
            pl_state = PL_GLIDE;
            sfx_play(SFX_GLIDE);
            return;
        }
        break;
    }
    /* nothing to do here, but a fire is at hand: rest by it rather than a refusal */
    if (land_scan_flag(MTF_WARM, 1)) { sit_down(); return; }
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
    uint8_t spd, n, moved = 0, pure, m, slow;

    if (burn_t) {
        burn_t--;
        if (burn_t == 0 && !edit_mt(burn_x, burn_y, MT_ASH))
            land_set(burn_x, burn_y, MT_BRAMBLE);    /* refused: the thorns are still there */
    }
    if (pl_state == PL_GLIDE) {
        /* an A in the last few frames of a glide is kept for the landing (chained glides) */
        if (pressed & J_A) a_buf = 10;
        else if (a_buf) a_buf--;
        glide_tick();
        hint_on = 0;
        run_hint_on = 0;
        return;
    }
    if (a_buf) { a_buf = 0; pressed |= J_A; }
    if (keys & J_LEFT) dx = -1;
    if (keys & J_RIGHT) dx = 1;
    if (keys & J_UP) dy = -1;
    if (keys & J_DOWN) dy = 1;

    if (pl_state == PL_SLEEP) { run_hint_on = 0; return; }      /* game.c wakes us */

    if (dx || dy) {
        if (dy < 0) pl_face = (uint8_t)(dx > 0 ? D_NE : dx < 0 ? D_NW : D_N);
        else if (dy > 0) pl_face = (uint8_t)(dx > 0 ? D_SE : dx < 0 ? D_SW : D_S);
        else pl_face = (uint8_t)(dx > 0 ? D_E : D_W);
        if (pl_state == PL_SIT) ambient_tempo(0);
        pl_state = PL_WALK;
        idle_t = 0;
        /* quarter pixels per frame: walk 1 px, run 2 px (DMG: 1.5 px, so the land streamer
           keeps up with the slower CPU; see DMG_RUN_QPX) */
        spd = (keys & J_B) ? (is_cgb ? 8 : DMG_RUN_QPX) : 4;
        if (dx && dy) spd = (uint8_t)(spd - (spd >> 2));
        m = mt_off(0, -2);
        /* heavy ground (shallows, drifts, dunes): 3/4 speed and shorter strides, so it reads
           as wading rather than as the game slowing down */
        slow = (uint8_t)(mt_flags[m] & MTF_SLOW);
        if (slow) spd = (uint8_t)(spd - (spd >> 2));
        if (keys & J_B) {
            if (run_t < 255) run_t++;
            if (run_t >= RUN_LEARNT && !(hints & HINT_RUN)) { hints |= HINT_RUN; save_req = 1; }
        } else if (walk_t < 255) walk_t++;
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
            n = (uint8_t)(slow ? 11 : 16);      /* stride */
            if (step_px >= n) { step_px -= n; footstep(); }
        }
    } else {
        if (pl_state == PL_WALK) pl_state = PL_STAND;
        if (idle_t < 255) idle_t++;
        if (pl_state == PL_STAND && idle_t >= 180 && !(keys & (J_A | J_B))) {
            sit_down();
        }
    }
    if (keys & (J_A | J_B | J_SELECT)) idle_t = 0;
    if ((pressed & (J_B | J_SELECT)) && pl_state == PL_SIT) { pl_state = PL_STAND; ambient_tempo(0); }

    /* DMG: the target probe runs on odd frames only (a frame of latency on the hint / A is
       invisible) so it never shares a frame's budget with the even-frame jobs */
    if (is_cgb || (vbl_frames & 1) || (pressed & J_A)) find_targets();
    /* reaching the revealed Heart is enough */
    if (act_mt == MT_HEART && heart_revealed && pl_state != PL_GLIDE) ending_req = 1;
    if (pressed & J_A) {
        if (pl_state == PL_SIT) {
            /* resting away from a fire (the wanderer sits whenever you stop for a moment): A
               stands up and acts at once, so a pause in front of the crags or a cold fire does
               not swallow the next press. By a fire, A just gets up. */
            pl_state = PL_STAND;
            ambient_tempo(0);
            if (ca_v && !near_warm) act();
        }
        else act();
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
    hint_on = 0;
    /* can_act (glide probes, cairn search) only when the target or the inputs to it changed */
    {
        static uint8_t ca_key[4];
        if (ft_new || ca_key[0] != equipped || ca_key[1] != stones || ca_key[2] != heart_revealed ||
            ca_key[3] != items) {
            ft_new = 0;
            ca_key[0] = equipped; ca_key[1] = stones; ca_key[2] = heart_revealed; ca_key[3] = items;
            ca_v = can_act();
            sel_v = (uint8_t)(!ca_v && other_item_acts());
        }
        m = ca_v;
    }
    if (pl_state != PL_SIT && (m || sel_v)) {
        uint16_t hx = act_mt != 0xFF ? act_x : tgt_x, hy = act_mt != 0xFF ? act_y : tgt_y;
        int16_t sx = (int16_t)((int16_t)(hx - cam_mx) * 16 - cam_sx);
        int16_t sy = (int16_t)((int16_t)(hy - cam_my) * 16 - cam_sy);
        hint_bob++;
        if (sx > -8 && sx < 160 && sy > 8 && sy < 120) {
            hint_on = m ? HINT_SHOW_A : HINT_SHOW_SEL;
            hint_x = (uint8_t)(sx + 4 + 8);
            hint_y = (uint8_t)(sy + 24 - 14 + 16 - ((hint_bob >> 4) & 1));
        }
    }
    /* one time only: walking for a while without ever running, a finger holds B over the
       wanderer (until the first run) */
    run_hint_on = (uint8_t)(!(hints & HINT_RUN) && !hint_on && pl_state == PL_WALK &&
                            walk_t >= RUN_HINT_AFTER && !(keys & J_B));
    if (run_hint_on) hint_bob++;
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
    /* DMG: the gliding cloak uses the light palette so it reads over dark crags */
    if (pl_state == PL_GLIDE && !is_cgb) prop |= S_PALETTE;
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
    /* hints: A over what it acts on (one sprite); SELECT over the wanderer's head (three, over
       the shadow's free slots: never while gliding); or the one-time hold-B, also overhead */
    pal = is_cgb ? OPAL_UI : 0;
    if (hint_on == HINT_SHOW_SEL) {
        i = (uint8_t)(y - 17 - ((hint_bob >> 4) & 1));
        spr_set(SP_HINT, (uint8_t)(x - 4), i, SPR_HINT_SEL, pal);
        spr_set(SP_HINT + 1, (uint8_t)(x + 4), i, SPR_HINT_SEL + 2, pal);
        spr_set(SP_HINT + 2, (uint8_t)(x + 12), i, SPR_HINT_SEL + 4, pal);
        return;
    }
    if (hint_on) spr_set(SP_HINT, hint_x, hint_y, SPR_HINT_A, pal);
    else spr_hide(SP_HINT);
    if (run_hint_on) spr_set(SP_HINT + 2, (uint8_t)(x + 4), (uint8_t)(y - 18 - ((hint_bob >> 4) & 1)), SPR_HINT_B, pal);
    else spr_hide(SP_HINT + 2);
}
