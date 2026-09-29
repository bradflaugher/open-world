/* map.c - OPEN WORLD map (START): one pixel per visited chunk (8x8 metatiles), 128 x 112 px
 * (the 128 x 128-chunk region centred on the start, rows 8..119), drawn into 224 unique BG
 * tiles (0..223) with the frame tiles from map_tiles at 224+. It "surveys" outwards from the
 * wanderer with the LCD on; unvisited chunks show the fog dither. */
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
#define MAP_ROW0   8            /* first chunk row shown */

static const uint16_t paper_pal[4] = { 0x6FDE, 0x4F18, 0x2E31, 0x1486 };
static uint8_t fog[16];
static uint8_t tbuf[16];
static uint8_t line_n[144];

static uint8_t map_xy(uint16_t mx, uint16_t my, uint8_t *sx, uint8_t *sy)
{
    int16_t cx = (int16_t)((int16_t)(mx - world.start.x) >> 3) + 64;
    int16_t cy = (int16_t)((int16_t)(my - world.start.y) >> 3) + 64 - MAP_ROW0;
    if (cx < 0 || cx >= 128 || cy < 0 || cy >= MAP_ROWS * 8) return 0;
    *sx = (uint8_t)(16 + cx);
    *sy = (uint8_t)(16 + cy);
    return 1;
}

static void render_tile(uint8_t tx, uint8_t ty)
{
    uint8_t y, x, lo, hi, bits, s, cy;
    uint16_t base_x = (uint16_t)(world.start.x - 512 + ((uint16_t)tx << 6) + 4);
    uint16_t my;
    for (y = 0; y < 8; y++) {
        cy = (uint8_t)(MAP_ROW0 + (ty << 3) + y);
        bits = visited[((uint16_t)cy << 4) | tx];
        if (!bits) {
            tbuf[y * 2] = fog[y * 2];
            tbuf[y * 2 + 1] = fog[y * 2 + 1];
            continue;
        }
        my = (uint16_t)(world.start.y - 512 + ((uint16_t)cy << 3) + 4);
        lo = fog[y * 2];
        hi = fog[y * 2 + 1];
        for (x = 0; x < 8; x++) {
            if (!(bits & (1 << x))) continue;
            s = world_map_shade((uint16_t)(base_x + ((uint16_t)x << 3)), my);
            lo &= (uint8_t)~(0x80 >> x);
            hi &= (uint8_t)~(0x80 >> x);
            if (s & 1) lo |= (uint8_t)(0x80 >> x);
            if (s & 2) hi |= (uint8_t)(0x80 >> x);
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

static void markers(uint8_t blink)
{
    uint8_t i, sx, sy, slot = 1, pal = is_cgb ? OPAL_UI : 0;
    memset(line_n, 0, sizeof line_n);
    hide_sprites_from(0);
    if (map_xy(pl_mx, pl_my, &sx, &sy) && blink) place(0, sx, sy, SPR_MAP_PLAYER, pal);
    else line_n[0] = line_n[0];
    for (i = 0; i < NUM_BEACONS; i++)
        if (map_xy(world.beacon[i].x, world.beacon[i].y, &sx, &sy))
            if (place(slot, sx, sy, SPR_MAP_BEACON,
                      (uint8_t)((beacons_lit & (1 << i)) ? (is_cgb ? OPAL_LIGHT : S_PALETTE) : pal))) slot++;
    if (heart_revealed && map_xy(world.heart.x, world.heart.y, &sx, &sy))
        if (place(slot, sx, sy, SPR_MAP_HEART, (uint8_t)(is_cgb ? OPAL_LIGHT : S_PALETTE))) slot++;
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
    for (i = 0; i < MAP_COLS * MAP_ROWS; i++) set_bkg_data(i, 1, fog);
    frame_tiles();
    nx_scx = 0;
    nx_scy = 0;
    /* fade the paper in */
    for (i = 16; i; i -= 2) {
        pal_paper(paper_pal, (uint8_t)(i - 2));
        markers(1);
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
                    render_tile(tx, ty);
                    k++;
                }
            }
            r++;
            if (r > 16) done = 1;
        }
        t++;
        markers((uint8_t)((t >> 3) & 1));
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
