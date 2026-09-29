/* map.c - OPEN WORLD map (START): the visited-chunk bitmap (128 x 128 chunks of 16x16
 * metatiles round the start, +-1024) drawn at one pixel per chunk: a 128 x 112 px chart of chunk
 * rows 8..119 (+-896 metatiles north-south, which holds every beacon and the Heart), in 224
 * unique BG tiles (0..223) with the frame tiles from map_tiles at 224+. It "surveys" outwards
 * from the wanderer with the LCD on, drawing only tiles that hold visited chunks; the rest
 * stays fog. */
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
#define MAP_CX0    0            /* first chunk column / row shown (of 128) */
#define MAP_CY0    8

/* in RAM: pal.c (another bank) reads it */
static uint16_t paper_pal[4] = { 0x6FBE, 0x4ED7, 0x2E10, 0x1485 };
static uint8_t fog[16];
/* chart inks: fog is paper (colour 0) with faint dots (1); pale land 1, land 2, sea 3 */
static const uint8_t shade_col[4] = { 1, 2, 2, 3 };
static uint8_t tbuf[16];
uint8_t map_done;              /* the survey finished (tests) */
#define line_n (scratch)         /* sprites per line (144 of the shared buffer) */

static uint8_t map_xy(uint16_t mx, uint16_t my, uint8_t *sx, uint8_t *sy)
{
    int16_t cx = (int16_t)((int16_t)(mx - world.start.x) >> 4) + 64 - MAP_CX0;
    int16_t cy = (int16_t)((int16_t)(my - world.start.y) >> 4) + 64 - MAP_CY0;
    if (cx < 0 || cx >= MAP_COLS * 8 || cy < 0 || cy >= MAP_ROWS * 8) return 0;
    *sx = (uint8_t)(16 + cx);
    *sy = (uint8_t)(16 + cy);
    return 1;
}

/* does map tile (tx, ty) hold any visited chunk? (8 chunks x 8 rows = 8 bitmap bytes) */
static uint8_t tile_visited(uint8_t tx, uint8_t ty)
{
    uint8_t y, cy = (uint8_t)(MAP_CY0 + (ty << 3));
    for (y = 0; y < 8; y++, cy++)
        if (visited[((uint16_t)cy << 4) | (uint8_t)((MAP_CX0 >> 3) + tx)]) return 1;
    return 0;
}

/* one 8x8 map tile = 8x8 chunks, one pixel each */
static void render_tile(uint8_t tx, uint8_t ty)
{
    uint8_t y, x, cx0 = (uint8_t)(MAP_CX0 + (tx << 3)), cy, bits, s, lo, hi, m2;
    uint16_t mx0 = (uint16_t)(world.start.x - 1024 + ((uint16_t)cx0 << 4) + 8), my;
    for (y = 0; y < 8; y++) {
        cy = (uint8_t)(MAP_CY0 + (ty << 3) + y);
        bits = visited[((uint16_t)cy << 4) | (cx0 >> 3)];
        lo = fog[y * 2];
        hi = fog[y * 2 + 1];
        if (bits) {
            my = (uint16_t)(world.start.y - 1024 + ((uint16_t)cy << 4) + 8);
            for (x = 0; x < 8; x++) {
                if (!(bits & (1 << x))) continue;
                s = shade_col[world_map_shade((uint16_t)(mx0 + ((uint16_t)x << 4)), my) & 3];
                m2 = (uint8_t)(0x80 >> x);
                lo &= (uint8_t)~m2;
                hi &= (uint8_t)~m2;
                if (s & 1) lo |= m2;
                if (s & 2) hi |= m2;
            }
        }
        tbuf[y * 2] = lo;
        tbuf[y * 2 + 1] = hi;
    }
    set_bkg_data((uint8_t)(ty * MAP_COLS + tx), 1, tbuf);
}

static uint8_t place(uint8_t slot, uint8_t sx, uint8_t sy, uint8_t tile, uint8_t pal)
{
    uint8_t y = (uint8_t)(sy - 3), i;
    for (i = 0; i < 8; i++) if (line_n[(uint8_t)(y + i)] >= 9) return 0;
    for (i = 0; i < 16; i++) line_n[(uint8_t)(y + i) < 144 ? (uint8_t)(y + i) : 143]++;
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
    uint8_t keys_old;
    dbg_count_on = 0;
    map_done = 0;
    game_state = GS_MAP;
    sfx_play(SFX_MAP);
    ambient_mode(AMB_MAP);
    visit_mark();
    fade_to(16, 4);
    split_enable(0);
    anim_on = 0;
    hide_sprites_from(0);
    wait_frames(1);
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
    if (!map_xy(pl_mx, pl_my, &px, &py)) { px = 80; py = 72; }
    px = (uint8_t)((px - 16) >> 3);
    py = (uint8_t)((py - 16) >> 3);
    keys_old = joypad();
    r = 0;
    for (;;) {
        if (!done) {
            /* ring r: tiles at Chebyshev distance r from (px, py) */
            k = 0;
            for (ty = 0; ty < MAP_ROWS; ty++) {
                dy = (uint8_t)(ty > py ? ty - py : py - ty);
                if (dy > r) continue;
                for (tx = 0; tx < MAP_COLS; tx++) {
                    dx = (uint8_t)(tx > px ? tx - px : px - tx);
                    if (dx > r) continue;
                    if (dx != r && dy != r) continue;
                    if (tile_visited(tx, ty)) { render_tile(tx, ty); k++; }
                }
            }
            r++;
            if (r > 16) { done = 1; map_done = 1; }
        }
        t++;
        markers(t);
        frame_commit();
        frame_sync();
        keys = joypad();
        pressed = (uint8_t)(keys & ~keys_old);
        keys_old = keys;
        if (pressed & (J_START | J_B | J_A)) break;
    }
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
