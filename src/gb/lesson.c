/* lesson.c - OPEN WORLD: the first time an item is taken from its shrine, a short wordless
 * scene shows what it does. A small diorama of the world's own metatiles on a blank page: the
 * wanderer uses the item with the same pictograms as in the world (a finger on A over what it
 * acts on), then SELECT cycles the items you carry. The only letters are the buttons' names.
 * Shown once ever (the hints byte is saved); once it has played through, a bobbing A in the
 * corner says it can be closed (A, B or START). Main loop only, with the game frame off. */
#pragma bank 255
#include <gb/gb.h>
#include <gb/cgb.h>
#include <string.h>
#include "game.h"
#include "gfx.h"
#include "land.h"
#include "assets.h"
#include "sound.h"

#define L_W      8                     /* diorama, in cells */
#define L_H      3
#define L_TX     2                     /* its top-left BG tile */
#define L_TY     5
#define L_X0     (L_TX * 8)            /* ... in screen px */
#define L_Y0     (L_TY * 8)
#define L_ROW    1                     /* the wanderer walks the middle row */
#define BLANK_T  250                   /* a free BG tile: plain colour 0, the page */
#define SEL_Y    (L_Y0 + L_H * 16 + 20) /* the SELECT pictogram and the items row (screen px) */

/* slots: 0-1 wanderer, 2 A, 3 glide shadow, 4-6 SELECT, 7-9 items, 10 "close" A, 11 marker */
#define LS_A     2
#define LS_SHADOW 3
#define LS_SEL   4
#define LS_ICON  7
#define LS_DONE  10
#define LS_MARK  11

enum { OP_END = 0, OP_WAIT, OP_WALK, OP_PUT, OP_GLIDE, OP_SELS };
/* OP_WAIT frames | OP_WALK cells | OP_PUT mt: A on the cell ahead, which becomes mt |
   OP_GLIDE cells: A on the cell ahead, then the glide | OP_SELS -: SELECT once per item carried
   (round to the one in hand again) */
static const uint8_t cells_stones[L_W] = {
    MT_GRASS, MT_GRASS, MT_SAND, MT_SHALLOW, MT_SHALLOW, MT_SAND, MT_GRASS, MT_GRASS };
static const uint8_t script_stones[] = {
    OP_WAIT, 30, OP_WALK, 1, OP_PUT, MT_STEPSTONE, OP_WALK, 1, OP_PUT, MT_STEPSTONE,
    OP_WALK, 3, OP_PUT, MT_CAIRN, OP_WAIT, 36, OP_SELS, 0, OP_WAIT, 50, OP_END };
static const uint8_t cells_cloak[L_W] = {
    MT_GRASS, MT_GRASS, MT_ROCK, MT_ROCK, MT_GRASS, MT_SHALLOW, MT_SHALLOW, MT_GRASS };
static const uint8_t script_cloak[] = {
    OP_WAIT, 30, OP_GLIDE, 3, OP_WAIT, 12, OP_GLIDE, 3, OP_WAIT, 36, OP_SELS, 0,
    OP_WAIT, 50, OP_END };

static uint8_t cell[L_H][L_W];
static const uint8_t *cells0, *script, *pc;
static uint8_t op_t, op_k;            /* frames into the op, presses done (OP_SELS) */
static uint8_t wc, wx, lift, anim, gliding;   /* wanderer: cell, sprite x (screen px) ... */
static uint8_t l_eq, l_item, bob;
static uint8_t a_on, a_press, sel_on, sel_press;
uint8_t lesson_ready;                 /* played through once: it can be closed (tests) */
uint8_t dbg_lessons;                  /* lessons closed (tests) */

/* the joypad, polled once a frame and also while the diorama is redrawn (that takes several
   frames on a DMG, and a quick tap in between must not be lost): new presses are latched */
static uint8_t pad_old, pad_latch;
static void poll(void)
{
    uint8_t k = joypad();
    pad_latch |= (uint8_t)(k & ~pad_old);
    pad_old = k;
}

static uint8_t grp(uint8_t m)
{
    if (m == MT_SEA || m == MT_SEA_GLINT) return 3;
    if (m == MT_SHALLOW || m == MT_STEPSTONE) return 2;
    return 0;
}

/* like the land's autotiler; outside the diorama counts as "same" (the slice is cut clean) */
static uint8_t oth(int8_t c, int8_t r, uint8_t cls)
{
    if (c < 0 || c >= L_W || r < 0 || r >= L_H) return 0;
    return (uint8_t)!(grp(cell[r][c]) & cls);
}

static uint8_t pick(uint8_t a, uint8_t b, uint8_t c)
{
    if (a) return b ? EDGE_OUTER : EDGE_H;
    if (b) return EDGE_V;
    return c ? EDGE_INNER : 0xFF;
}

static void put_cell(uint8_t c, uint8_t r)
{
    uint8_t t[4], a[4], m = cell[r][c], cls, v, q;
    const uint8_t *b = &mt_t[m << 2], *et;
    int8_t x = (int8_t)c, y = (int8_t)r;
    for (q = 0; q < 4; q++) { t[q] = b[q]; a[q] = mt_a[(m << 2) + q]; }
    cls = m == MT_SHALLOW ? 2 : (m == MT_SEA || m == MT_SEA_GLINT) ? 1 : 0;
    if (cls) {
        et = &edge_t[(uint8_t)((cls - 1) << 4)];
        if ((v = pick(oth(x, y - 1, cls), oth(x - 1, y, cls), oth(x - 1, y - 1, cls))) != 0xFF) t[0] = et[v];
        if ((v = pick(oth(x, y - 1, cls), oth(x + 1, y, cls), oth(x + 1, y - 1, cls))) != 0xFF) t[1] = et[4 + v];
        if ((v = pick(oth(x, y + 1, cls), oth(x - 1, y, cls), oth(x - 1, y + 1, cls))) != 0xFF) t[2] = et[8 + v];
        if ((v = pick(oth(x, y + 1, cls), oth(x + 1, y, cls), oth(x + 1, y + 1, cls))) != 0xFF) t[3] = et[12 + v];
    }
    x = (int8_t)(L_TX + (c << 1));
    y = (int8_t)(L_TY + (r << 1));
    set_tiles((uint8_t)x, (uint8_t)y, 2, 2, (uint8_t *)0x9800, t);
    if (is_cgb) {
        VBK_REG = 1;
        set_tiles((uint8_t)x, (uint8_t)y, 2, 2, (uint8_t *)0x9800, a);
        VBK_REG = 0;
    }
}

static void scene_reset(void)
{
    uint8_t r, c;
    for (r = 0; r < L_H; r++)
        for (c = 0; c < L_W; c++) cell[r][c] = cells0[c];
    for (r = 0; r < L_H; r++)
        for (c = 0; c < L_W; c++) { put_cell(c, r); poll(); }
    pc = script;
    op_t = op_k = 0;
    wc = 1;
    wx = (uint8_t)(L_X0 + 16);
    lift = anim = gliding = 0;
    a_on = sel_on = 0;
    l_eq = l_item;
}

static void page(void)
{
    uint8_t row[20], y;
    static const uint8_t blank[16] = { 0 };
    set_bkg_data(BLANK_T, 1, blank);
    memset(row, BLANK_T, sizeof row);
    for (y = 0; y < 18; y++) set_tiles(0, y, 20, 1, (uint8_t *)0x9800, row);
    if (is_cgb) {
        memset(row, PAL_SKY, sizeof row);
        VBK_REG = 1;
        for (y = 0; y < 18; y++) set_tiles(0, y, 20, 1, (uint8_t *)0x9800, row);
        VBK_REG = 0;
    }
}

static uint8_t next_item(uint8_t e)
{
    uint8_t k;
    for (k = 0; k < IT_COUNT; k++) {
        e = (uint8_t)(e + 1 == IT_COUNT ? 0 : e + 1);
        if (items & (1 << e)) break;
    }
    return e;
}

static uint8_t items_n(void)
{
    uint8_t k, n = 0;
    for (k = 0; k < IT_COUNT; k++) if (items & (1 << k)) n++;
    return n;
}

static void op_next(void)
{
    pc += 2;
    op_t = op_k = 0;
    a_on = 0;
}

/* A over the cell ahead: it bobs, then the finger presses (returns 1 on the press frame) */
#define A_BOB   18
#define A_HOLD  10
static uint8_t a_step(void)
{
    a_on = 1;
    a_press = (uint8_t)(op_t >= A_BOB);
    return (uint8_t)(op_t == A_BOB);
}

static void step(void)
{
    uint8_t n;
    op_t++;
    switch (pc[0]) {
    case OP_WAIT:
        if (op_t >= pc[1]) op_next();
        break;
    case OP_WALK:
        wx++;
        if ((op_t & 7) == 0) anim ^= 1;
        if ((op_t & 15) == 0) { wc++; sfx_play(cells0[wc] == MT_SAND ? SFX_STEP_SAND : SFX_STEP_SOFT); }
        if (op_t >= (uint8_t)(pc[1] << 4)) { anim = 0; op_next(); }
        break;
    case OP_PUT:
        if (a_step()) {
            cell[L_ROW][wc + 1] = pc[1];
            put_cell((uint8_t)(wc + 1), L_ROW);
            sfx_play(pc[1] == MT_CAIRN ? SFX_CAIRN : SFX_STEPSTONE);
        }
        if (op_t >= A_BOB + A_HOLD) op_next();
        break;
    case OP_GLIDE:
        if (op_t <= A_BOB) {
            if (a_step()) { sfx_play(SFX_GLIDE); gliding = 1; }
            break;
        }
        a_on = 0;
        n = (uint8_t)(op_t - A_BOB);                  /* 1..32, as in the world */
        wx = (uint8_t)(L_X0 + (wc << 4) + (uint8_t)(((uint16_t)(pc[1] << 4) * n) >> 5));
        lift = (uint8_t)(((uint16_t)n * (uint16_t)(32 - n)) >> 5);
        if (n >= 32) {
            wc = (uint8_t)(wc + pc[1]);
            wx = (uint8_t)(L_X0 + (wc << 4));
            lift = 0;
            gliding = 0;
            sfx_play(SFX_LAND);
            op_next();
        }
        break;
    case OP_SELS:
        /* one press per item carried: 20 frames to look, the press, 16 frames to see it */
        sel_on = 1;
        sel_press = (uint8_t)(op_t >= 20 && op_t < 28);
        if (op_t == 20) { l_eq = next_item(l_eq); sfx_play(SFX_SELECT); }
        if (op_t >= 36) {
            op_t = 0;
            if (++op_k >= items_n()) { sel_on = 0; op_next(); }
        }
        break;
    default:            /* OP_END: again from the top */
        scene_reset();
        lesson_ready = 1;
        break;
    }
}

static void draw(void)
{
    uint8_t pal = is_cgb ? OPAL_PLAYER : 0, pu = is_cgb ? OPAL_UI : 0, dim = is_cgb ? OPAL_WATCHER : S_PALETTE;
    uint8_t base, y = (uint8_t)(L_Y0 + (L_ROW << 4) - 3 + 16), i, k, n, x;
    bob++;
    base = gliding ? SPR_PL_GLIDE : anim ? SPR_PL_SIDE1 : SPR_PL_SIDE0;
    if (gliding && !is_cgb) pal |= S_PALETTE;
    spr_set(0, (uint8_t)(wx + 8), (uint8_t)(y - lift), base, pal);
    spr_set(1, (uint8_t)(wx + 16), (uint8_t)(y - lift), (uint8_t)(base + 2), pal);
    if (gliding && lift > 1) spr_set(LS_SHADOW, (uint8_t)(wx + 12), (uint8_t)(y + 2), SPR_SHADOW, is_cgb ? OPAL_PLAYER : 0);
    else spr_hide(LS_SHADOW);
    /* A over the cell ahead, exactly where the world puts it */
    if (a_on) {
        x = (uint8_t)(L_X0 + ((wc + 1) << 4) + 4 + 8);
        k = (uint8_t)(L_Y0 + (L_ROW << 4) - 14 + 16);
        k = a_press ? (uint8_t)(k + 1) : (uint8_t)(k - ((bob >> 4) & 1));
        spr_set(LS_A, x, k, SPR_HINT_A, pu);
    } else spr_hide(LS_A);
    /* the items you carry, the one in hand raised; SELECT beside them while it is pressed */
    n = items_n();
    x = (uint8_t)((160 - (24 + 12 + (n << 4) - 8)) >> 1);
    if (sel_on) {
        k = (uint8_t)(SEL_Y + 16 + sel_press);
        spr_set(LS_SEL, (uint8_t)(x + 8), k, SPR_HINT_SEL, pu);
        spr_set(LS_SEL + 1, (uint8_t)(x + 16), k, SPR_HINT_SEL + 2, pu);
        spr_set(LS_SEL + 2, (uint8_t)(x + 24), k, SPR_HINT_SEL + 4, pu);
    } else for (i = 0; i < 3; i++) spr_hide((uint8_t)(LS_SEL + i));
    x = (uint8_t)(x + 24 + 12);
    for (i = 0, k = 0; i < IT_COUNT; i++) {
        if (!(items & (1 << i))) continue;
        base = i == IT_STONES ? SPR_ICON_STONES : i == IT_CLOAK ? SPR_ICON_CLOAK : SPR_ICON_LANTERN;
        spr_set((uint8_t)(LS_ICON + k), (uint8_t)(x + 8), (uint8_t)(SEL_Y + 16 + 7 - (i == l_eq ? 2 : 0)),
                base, i == l_eq ? pu : dim);
        /* a marker under the one in hand */
        if (i == l_eq) spr_set(LS_MARK, (uint8_t)(x + 8 + 1), (uint8_t)(SEL_Y + 16 + 17), SPR_MAP_PLAYER, pu);
        x = (uint8_t)(x + 16);
        k++;
    }
    /* played through: an A in the corner closes it */
    if (lesson_ready) spr_set(LS_DONE, 160 - 12 + 8, (uint8_t)(144 - 20 + 16 - ((bob >> 4) & 1)), SPR_HINT_A, pu);
}

void lesson_screen(uint8_t it) BANKED
{
    dbg_count_on = 0;
    game_state = GS_LESSON;
    lesson_ready = 0;
    l_item = it;
    cells0 = it == IT_CLOAK ? cells_cloak : cells_stones;
    script = it == IT_CLOAK ? script_cloak : script_stones;
    ambient_mode(AMB_MAP);
    fade_to(16, 2);
    split_enable(0);
    hide_sprites_from(0);
    wait_frames(1);
    page();
    scene_reset();
    nx_scx = 0;
    nx_scy = 0;
    /* shown in daylight whatever the hour (the world's own palettes come back after) */
    pal_phase_from = pal_phase_to = PH_DAY;
    pal_t = 16;
    pal_fog = 0;
    pal_flash = 0;
    draw();
    frame_commit();
    fade_to(0, 2);
    pad_old = joypad();
    pad_latch = 0;
    for (;;) {
        frame_sync();
        poll();
        keys = pad_old;
        pressed = pad_latch;
        pad_latch = 0;
        step();
        draw();
        frame_commit();
        if (lesson_ready && (pressed & (J_A | J_B | J_START))) break;
    }
    sfx_play(SFX_SELECT);
    fade_to(16, 2);
    hide_sprites_from(0);
    dbg_lessons++;
    ambient_mode(AMB_WORLD);
    world_enter(0);
    keys = joypad();
    pressed = 0;
}
