/* map.c - OPEN WORLD map (START): the visited-chunk bitmap (128 x 128 chunks of 16x16
 * metatiles round the start, +-1024) drawn at two pixels per chunk: a 128 x 112 px chart of
 * 64 x 56 chunks (about +-500 metatiles) centred on the wanderer, in 224 unique BG tiles
 * (0..223) with the frame tiles from map_tiles at 224+. It "surveys" outwards from the wanderer
 * with the LCD on, drawing only tiles that hold visited chunks or a road; the rest stays fog.
 * A beacon or the Heart that lies off the chart is pinned to its edge, in its direction.
 * The ancient road to each beacon you have lit is drawn as a dotted line from the first fire,
 * and the road to the Heart once the Heart is revealed: the way there, without a word. */
#pragma bank 255
#include <gb/gb.h>
#include <gb/cgb.h>
#include <string.h>
#include "game.h"
#include "gfx.h"
#include "land.h"
#include "assets.h"
#include "sound.h"

#define MAP_T0     224          /* frame tiles live here */
#define MAP_COLS   16
#define MAP_ROWS   14
static uint8_t map_ox, map_oy;      /* first chunk column / row on the chart (of 128) */

/* in RAM: pal.c (another bank) reads it */
static uint16_t paper_pal[4] = { 0x6FBE, 0x4ED7, 0x2E10, 0x1485 };
static uint8_t fog[16];
/* chart inks: fog is paper (colour 0) with faint dots (1); pale land 1, land 2, sea 3 */
static const uint8_t shade_col[4] = { 1, 2, 2, 3 };
static uint8_t tbuf[16];
uint8_t map_done;              /* the survey finished (tests) */
#define line_n (scratch)         /* sprites per line (144 of the shared buffer) */

#define CLOSE_KEYS (J_START | J_B | J_A)

/* a world coordinate in half chunks from the bitmap's edge (0..255, the start at 128) */
static uint8_t half_of(uint16_t m, uint16_t s)
{
    int16_t d = (int16_t)(((int16_t)(m - s) >> 3) + 128);
    if (d < 0) d = 0;
    if (d > 255) d = 255;
    return (uint8_t)d;
}

/* chart pixel (0..127, 0..111) of a world position; off the chart: pinned to the edge */
static uint8_t map_xy(uint16_t mx, uint16_t my, uint8_t *sx, uint8_t *sy)
{
    int16_t x = (int16_t)half_of(mx, world.start.x) - (int16_t)((uint16_t)map_ox << 1);
    int16_t y = (int16_t)half_of(my, world.start.y) - (int16_t)((uint16_t)map_oy << 1);
    if (x < 0) x = 0;
    if (x > MAP_COLS * 8 - 1) x = MAP_COLS * 8 - 1;
    if (y < 0) y = 0;
    if (y > MAP_ROWS * 8 - 1) y = MAP_ROWS * 8 - 1;
    *sx = (uint8_t)(16 + x);
    *sy = (uint8_t)(16 + y);
    return 1;
}

#define VISITED(cx, cy) (visited[((uint16_t)(cy) << 4) | ((cx) >> 3)] & (uint8_t)(1 << ((cx) & 7)))

/* does map tile (tx, ty) hold any visited chunk? (4 x 4 chunks) */
static uint8_t tile_visited(uint8_t tx, uint8_t ty)
{
    uint8_t i, j, cx, cy = (uint8_t)(map_oy + (ty << 2));
    for (j = 0; j < 4; j++, cy++)
        for (i = 0, cx = (uint8_t)(map_ox + (tx << 2)); i < 4; i++, cx++)
            if (VISITED(cx, cy)) return 1;
    return 0;
}

/* ---- roads: straight lines from the first fire, in chart pixels ---- */
#define NROADS 4                      /* beacons 0-2, the Heart */
static int16_t rd_x0, rd_y0, rd_x1[NROADS], rd_y1[NROADS];
static int16_t rd_s[NROADS];          /* minor per major step, in 1/128ths */
static uint8_t rd_on, rd_ymaj;        /* bit i: road i is drawn / runs mostly north-south */

static void roads_setup(void)
{
    uint8_t i;
    int16_t dx, dy, adx, ady;
    rd_on = (uint8_t)(beacons_lit | (heart_revealed ? 8 : 0));
    rd_ymaj = 0;
    rd_x0 = (int16_t)128 - (int16_t)((uint16_t)map_ox << 1);
    rd_y0 = (int16_t)128 - (int16_t)((uint16_t)map_oy << 1);
    for (i = 0; i < NROADS; i++) {
        const wpos_t *t = i < 3 ? &world.beacon[i] : &world.heart;
        rd_x1[i] = (int16_t)half_of(t->x, world.start.x) - (int16_t)((uint16_t)map_ox << 1);
        rd_y1[i] = (int16_t)half_of(t->y, world.start.y) - (int16_t)((uint16_t)map_oy << 1);
        dx = (int16_t)(rd_x1[i] - rd_x0); adx = dx < 0 ? -dx : dx;
        dy = (int16_t)(rd_y1[i] - rd_y0); ady = dy < 0 ? -dy : dy;
        if (!adx && !ady) { rd_on &= (uint8_t)~(1 << i); continue; }
        if (ady > adx) { rd_ymaj |= (uint8_t)(1 << i); rd_s[i] = (int16_t)((dx * 128) / dy); }
        else rd_s[i] = (int16_t)((dy * 128) / dx);
    }
}

/* road i through map tile (tx, ty): 1 if it passes; with plot, its dots go into tbuf (ink 3) */
static uint8_t road_tile(uint8_t i, uint8_t tx, uint8_t ty, uint8_t plot)
{
    int16_t a, a0, a1, b, b0, lo, hi, m0, m1, n0;
    uint8_t ym = (uint8_t)(rd_ymaj & (1 << i)), any = 0, r, m;
    if (ym) { a0 = (int16_t)(ty << 3); b0 = (int16_t)(tx << 3); m0 = rd_y0; m1 = rd_y1[i]; n0 = rd_x0; }
    else { a0 = (int16_t)(tx << 3); b0 = (int16_t)(ty << 3); m0 = rd_x0; m1 = rd_x1[i]; n0 = rd_y0; }
    a1 = (int16_t)(a0 + 7);
    lo = m0 < m1 ? m0 : m1;
    hi = m0 < m1 ? m1 : m0;
    if (a0 < lo) a0 = lo;
    if (a1 > hi) a1 = hi;
    if (a0 > a1) return 0;
    /* where it enters and leaves this strip: both to one side of the tile means it misses */
    lo = (int16_t)(n0 + (((int16_t)(a0 - m0) * rd_s[i]) >> 7));
    hi = (int16_t)(n0 + (((int16_t)(a1 - m0) * rd_s[i]) >> 7));
    if ((lo < b0 && hi < b0) || (lo > b0 + 7 && hi > b0 + 7)) return 0;
    if (!plot) return 1;
    for (a = a0; a <= a1; a++) {
        if ((uint8_t)(a - m0) & 1) continue;                 /* dotted */
        b = (int16_t)(n0 + (((int16_t)(a - m0) * rd_s[i]) >> 7));
        if (b < b0 || b > b0 + 7) continue;
        if (ym) { r = (uint8_t)((uint8_t)a & 7); m = (uint8_t)(0x80 >> ((uint8_t)b & 7)); }
        else { r = (uint8_t)((uint8_t)b & 7); m = (uint8_t)(0x80 >> ((uint8_t)a & 7)); }
        tbuf[r << 1] |= m;
        tbuf[(r << 1) + 1] |= m;
        any = 1;
    }
    return any;
}

static uint8_t tile_road(uint8_t tx, uint8_t ty)
{
    uint8_t i;
    for (i = 0; i < NROADS; i++)
        if ((rd_on & (1 << i)) && road_tile(i, tx, ty, 0)) return 1;
    return 0;
}

/* one 8x8 map tile = 4x4 chunks, 2x2 pixels each, and the roads over it */
static void render_tile(uint8_t tx, uint8_t ty)
{
    uint8_t i, j, cx, cy = (uint8_t)(map_oy + (ty << 2)), s, m2, *p;
    uint16_t mx, my;
    memcpy(tbuf, fog, 16);
    for (j = 0; j < 4; j++, cy++) {
        my = (uint16_t)(world.start.y - 1024 + ((uint16_t)cy << 4) + 8);
        p = &tbuf[j << 2];
        for (i = 0, cx = (uint8_t)(map_ox + (tx << 2)); i < 4; i++, cx++) {
            if (!VISITED(cx, cy)) continue;
            mx = (uint16_t)(world.start.x - 1024 + ((uint16_t)cx << 4) + 8);
            s = shade_col[world_map_shade(mx, my) & 3];
            m2 = (uint8_t)(0xC0 >> (i << 1));
            p[0] &= (uint8_t)~m2; p[1] &= (uint8_t)~m2; p[2] &= (uint8_t)~m2; p[3] &= (uint8_t)~m2;
            if (s & 1) { p[0] |= m2; p[2] |= m2; }
            if (s & 2) { p[1] |= m2; p[3] |= m2; }
        }
    }
    for (i = 0; i < NROADS; i++)
        if (rd_on & (1 << i)) road_tile(i, tx, ty, 1);
    set_bkg_data((uint8_t)(ty * MAP_COLS + tx), 1, tbuf);
}

static uint8_t place(uint8_t slot, uint8_t sx, uint8_t sy, uint8_t tile, uint8_t pal)
{
    uint8_t y = (uint8_t)(sy - 3), i;
    /* an 8x16 sprite covers 16 lines: all of them must have room (lines past the screen are
       not drawn, so they do not count) */
    for (i = 0; i < 16; i++) {
        uint8_t l = (uint8_t)(y + i);
        if (l < 144 && line_n[l] >= 9) return 0;
    }
    for (i = 0; i < 16; i++) {
        uint8_t l = (uint8_t)(y + i);
        if (l < 144) line_n[l]++;
    }
    spr_set(slot, (uint8_t)(sx - 3 + 8), (uint8_t)(y + 16), tile, pal);
    return 1;
}

/* t: frame counter. The wanderer blinks; lit beacons pulse; the Heart is drawn dark (legible
   on the pale paper) and pulses slowly */
static void markers(uint8_t t)
{
    uint8_t i, sx, sy, slot = 1, pal = is_cgb ? OPAL_UI : 0, lit;
    memset(line_n, 0, 144);
    hide_sprites_from(0);
    if (map_xy(pl_mx, pl_my, &sx, &sy) && ((t >> 3) & 1)) place(0, sx, sy, SPR_MAP_PLAYER, pal);
    for (i = 0; i < NUM_BEACONS; i++) {
        lit = (uint8_t)(beacons_lit & (1 << i));
        if (lit && ((t >> 4) & 3) == 0) continue;
        if (map_xy(world.beacon[i].x, world.beacon[i].y, &sx, &sy))
            if (place(slot, sx, sy, SPR_MAP_BEACON, pal)) slot++;
    }
    if (heart_revealed && ((t >> 5) & 3) != 0 && map_xy(world.heart.x, world.heart.y, &sx, &sy))
        if (place(slot, sx, sy, SPR_MAP_HEART, pal)) slot++;
    for (i = 0; i < cairn_n && slot < 40; i++)
        if (map_xy(cairns[i].x, cairns[i].y, &sx, &sy))
            if (place(slot, sx, sy, SPR_MAP_CAIRN, pal)) slot++;
}

static void frame_tiles(void)
{
    uint8_t row[20], x, y;
    for (y = 0; y < 18; y++) {
        for (x = 0; x < 20; x++) {
            uint8_t t = (uint8_t)(MAP_T0 + MAP_T_SHADE1);
            if (y == 1 && x >= 1 && x <= 18) t = (uint8_t)(MAP_T0 + (x == 1 ? MAP_T_TL : x == 18 ? MAP_T_TR : MAP_T_T));
            else if (y == 16 && x >= 1 && x <= 18) t = (uint8_t)(MAP_T0 + (x == 1 ? MAP_T_BL : x == 18 ? MAP_T_BR : MAP_T_B));
            else if (y >= 2 && y <= 15 && x == 1) t = (uint8_t)(MAP_T0 + MAP_T_L);
            else if (y >= 2 && y <= 15 && x == 18) t = (uint8_t)(MAP_T0 + MAP_T_R);
            else if (y >= 2 && y <= 15 && x >= 2 && x <= 17) t = (uint8_t)((y - 2) * MAP_COLS + (x - 2));
            row[x] = t;
        }
        set_tiles(0, y, 20, 1, (uint8_t *)0x9800, row);
    }
    if (is_cgb) {
        memset(row, 0, sizeof row);
        VBK_REG = 1;
        for (y = 0; y < 18; y++) set_tiles(0, y, 20, 1, (uint8_t *)0x9800, row);
        VBK_REG = 0;
    }
}

void map_screen(void) BANKED
{
    uint8_t i, r, tx, ty, px, py, done = 0, t = 0, dx, dy, k;
    dbg_count_on = 0;
    map_done = 0;
    game_state = GS_MAP;
    sfx_play(SFX_MAP);
    ambient_mode(AMB_MAP);
    visit_mark();
    /* START is still down: only a new press counts. From here the VBlank reads the pad: the
       loads and the survey below can each take several frames on a DMG, and a tap of START
       in between used to be lost (the map stayed open) */
    pad_now = joypad();
    pad_hits = 0;
    pad_watch = 1;
    fade_to(16, 4);
    split_enable(0);
    anim_on = 0;
    hide_sprites_from(0);
    wait_frames(1);
    /* the chart: 64 x 56 chunks round the wanderer, kept inside the bitmap */
    px = (uint8_t)(half_of(pl_mx, world.start.x) >> 1);
    py = (uint8_t)(half_of(pl_my, world.start.y) >> 1);
    map_ox = (uint8_t)(px < 32 ? 0 : px > 96 ? 64 : px - 32);
    map_oy = (uint8_t)(py < 28 ? 0 : py > 100 ? 72 : py - 28);
    roads_setup();
    /* tiles: frame set at 224, every map tile starts as fog */
    gfx_load_map(MAP_T0, fog);
    for (i = 0; i < 8; i++) { fog[i * 2] = fog[i * 2 + 1]; fog[i * 2 + 1] = 0; }   /* colours 1,2 -> 0,1 */
    for (i = 0; i < MAP_COLS * MAP_ROWS; i++) set_bkg_data(i, 1, fog);
    frame_tiles();
    nx_scx = 0;
    nx_scy = 0;
    /* fade the paper in */
    for (i = 16; i; i -= 2) {
        pal_paper(paper_pal, (uint8_t)(i - 2));
        markers(8);
        wait_frames(2);
    }
    /* survey outwards from the wanderer, a ring of tiles at a time */
    map_xy(pl_mx, pl_my, &px, &py);
    px = (uint8_t)((px - 16) >> 3);
    py = (uint8_t)((py - 16) >> 3);
    r = 0;
    for (;;) {
        if (!done) {
            /* ring r: tiles at Chebyshev distance r from (px, py) */
            k = 0;
            for (ty = 0; ty < MAP_ROWS && !(pad_hits & CLOSE_KEYS); ty++) {
                dy = (uint8_t)(ty > py ? ty - py : py - ty);
                if (dy > r) continue;
                for (tx = 0; tx < MAP_COLS; tx++) {
                    dx = (uint8_t)(tx > px ? tx - px : px - tx);
                    if (dx > r) continue;
                    if (dx != r && dy != r) continue;
                    if (tile_visited(tx, ty) || tile_road(tx, ty)) { render_tile(tx, ty); k++; }
                }
            }
            r++;
            if (r > 16) { done = 1; map_done = 1; }
        }
        t++;
        markers(t);
        frame_commit();
        frame_sync();
        __critical { keys = pad_now; pressed = pad_hits; pad_hits = 0; }
        if (pressed & CLOSE_KEYS) break;
    }
    pad_watch = 0;
    sfx_play(SFX_MAP);
    for (i = 0; i <= 16; i += 2) {
        pal_paper(paper_pal, i);
        wait_frames(2);
    }
    pal_fade = 16;
    ambient_mode(AMB_WORLD);
    world_enter(0);
    keys = joypad();
    pressed = 0;
}
