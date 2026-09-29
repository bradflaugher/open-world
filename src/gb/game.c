/* game.c - OPEN WORLD front-end: title, world loop, time of day, warmth, weather, whiteout,
 * the ending, and world transitions. Banked; the hot streaming path lives in land.c (bank 0). */
#pragma bank 255
#include <gb/gb.h>
#include <gb/cgb.h>
#include <string.h>
#include "game.h"
#include "gfx.h"
#include "land.h"
#include "assets.h"
#include "sound.h"

volatile uint8_t game_state;
uint8_t pl_state, pl_face = D_N, pl_anim;
uint16_t pl_mx, pl_my;
uint8_t pl_sx, pl_sy;
uint16_t cam_mx, cam_my;
uint8_t cam_sx, cam_sy;
int8_t pl_lift;
uint16_t tod, day_count;
uint8_t phase = 0xFF, weather, biome_here;
uint8_t items = 1, equipped, beacons_lit, stones;
uint16_t warmth = WARMTH_MAX;
uint16_t respawn_x, respawn_y;
uint8_t worlds_done;
uint8_t cairn_n;
wpos_t cairns[MAX_CAIRNS];
uint8_t visited[VISIT_W * VISIT_W / 8];
uint8_t keys, pressed;
uint8_t idle_t, item_pulse, shake;
int8_t shake_x, shake_y;
uint8_t heart_revealed, glow_on;
uint8_t hint_x, hint_y, hint_on;
uint16_t dbg_seed;
uint8_t dbg_teleport;
uint16_t dbg_tx, dbg_ty;
uint8_t dbg_max_line;
uint8_t near_warm;
uint16_t dbg_world_frames;

static uint8_t prev_keys;
static uint16_t warm_acc;
static uint8_t amb_biome = 0xFF, amb_phase = 0xFF, amb_wx = 0xFF;
static uint16_t wx_region_x = 0xFFFF, wx_region_y = 0xFFFF, wx_day = 0xFFFF;
static uint8_t wake_t;
static uint8_t tick8;
static uint16_t shake_rng = 0xBEEF;

static void input(void)
{
    prev_keys = keys;
    keys = joypad();
    pressed = (uint8_t)(keys & ~prev_keys);
}

static uint8_t rnd8(void)
{
    uint16_t x = shake_rng;
    x ^= (uint16_t)(x << 7);
    x ^= (uint16_t)(x >> 9);
    x ^= (uint16_t)(x << 8);
    shake_rng = x;
    return (uint8_t)x;
}

/* ---------------------------------------------------------------- world edits */
void edit_mt(uint16_t mx, uint16_t my, uint8_t mt) BANKED
{
    if (!world_mod_set(mx, my, mt)) { sfx_play(SFX_NO); return; }
    land_set(mx, my, mt);
}

void light_beacon(uint8_t i) BANKED
{
    edit_mt(world.beacon[i].x, world.beacon[i].y, MT_BEACON_LIT);
    beacons_lit |= (uint8_t)(1 << i);
    respawn_x = world.beacon[i].x;
    respawn_y = world.beacon[i].y;
    ambient_beacons(beacons_lit);
    sfx_play(SFX_BEACON);
    shake = 6;
    if (beacons_lit == 7 && !heart_revealed) {
        heart_revealed = 1;
        sfx_play(SFX_HEART);
    }
    save_write();
}

/* ---------------------------------------------------------------- visited chunks */
static uint8_t chunk_of(uint16_t m, uint16_t s, uint8_t *out)
{
    int16_t d = (int16_t)(m - s);
    d = (int16_t)((d >> 3) + (VISIT_W / 2));
    if (d < 0 || d >= VISIT_W) return 0;
    *out = (uint8_t)d;
    return 1;
}

static void visit_at(uint16_t mx, uint16_t my)
{
    uint8_t cx, cy;
    if (!chunk_of(mx, world.start.x, &cx) || !chunk_of(my, world.start.y, &cy)) return;
    visited[((uint16_t)cy << 4) | (cx >> 3)] |= (uint8_t)(1 << (cx & 7));
}

void visit_mark(void) BANKED
{
    visit_at(pl_mx, pl_my);
    visit_at(cam_mx, cam_my);
    visit_at((uint16_t)(cam_mx + 10), cam_my);
    visit_at(cam_mx, (uint16_t)(cam_my + 8));
    visit_at((uint16_t)(cam_mx + 10), (uint16_t)(cam_my + 8));
}

uint8_t visit_get(uint8_t cx, uint8_t cy) BANKED
{
    return (uint8_t)(visited[((uint16_t)cy << 4) | (cx >> 3)] & (1 << (cx & 7)));
}

/* ---------------------------------------------------------------- camera */
void camera_update(void) BANKED
{
    int8_t sy = (int8_t)(pl_sy - 4);
    uint8_t scx, scy;
    cam_mx = (uint16_t)(pl_mx - 5);
    cam_sx = pl_sx;
    cam_my = (uint16_t)(pl_my - 4);
    if (sy < 0) { sy += 16; cam_my--; }
    cam_sy = (uint8_t)sy;
    if (shake) {
        uint8_t r = rnd8();
        shake_x = (int8_t)((r & 1) ? (shake >> 1) + 1 : -(int8_t)((shake >> 1) + 1));
        shake_y = (int8_t)((r & 2) ? (shake >> 2) : -(int8_t)(shake >> 2));
        if ((vbl_frames & 1) == 0) shake--;
    } else {
        shake_x = 0;
        shake_y = 0;
    }
    scx = (uint8_t)((((uint8_t)cam_mx & 15) << 4) | cam_sx);
    scy = (uint8_t)((((uint8_t)cam_my & 15) << 4) | cam_sy);
    nx_land_scx = (uint8_t)(scx + shake_x);
    nx_land_scy = (uint8_t)(scy - BAND_LINES + shake_y);
}

/* ---------------------------------------------------------------- time, warmth, weather */
static uint8_t phase_of(uint16_t t)
{
    if (t < T_DAY) return PH_DAWN;
    if (t < T_DUSK) return PH_DAY;
    if (t < T_NIGHT) return PH_DUSK;
    return PH_NIGHT;
}

static const uint16_t phase_start[4] = { T_DAWN, T_DAY, T_DUSK, T_NIGHT };

static void phase_update(uint8_t instant)
{
    uint8_t p = phase_of(tod), t;
    uint16_t into = (uint16_t)(tod - phase_start[p]);
    if (p != phase) {
        uint8_t old = phase;
        phase = p;
        pal_phase_from = (instant || old > 3) ? p : old;
        pal_phase_to = p;
        pal_t = 0xFF;
        band_stars(p == PH_NIGHT || p == PH_DUSK);
        if (p == PH_DAWN && !instant) sfx_play(SFX_DAWN);
    }
    t = into >= PHASE_BLEND ? 16 : (uint8_t)(into >> 6);
    if (instant) t = 16;
    if (t != pal_t) {
        pal_t = t;
        pal_apply();
    }
    glow_on = (uint8_t)(p == PH_NIGHT || (p == PH_DUSK && into > 2000));
}

static void weather_update(void)
{
    uint16_t rx = (uint16_t)(pl_mx >> 6), ry = (uint16_t)(pl_my >> 6);
    uint8_t h, w, b;
    if (rx == wx_region_x && ry == wx_region_y && day_count == wx_day) return;
    wx_region_x = rx;
    wx_region_y = ry;
    wx_day = day_count;
    h = world_detail((uint16_t)(rx * 13u + day_count * 7u + 0x51u), (uint16_t)(ry * 29u + (day_count >> 1)));
    b = world_biome(pl_mx, pl_my);
    if (h < 150) w = WX_CLEAR;
    else if (h < 195) w = WX_RAIN;
    else if (h < 230) w = WX_FOG;
    else w = WX_STORM;
    if (b == B_TUNDRA && (w == WX_RAIN || w == WX_STORM)) w = WX_SNOW;
    if (b == B_DESERT && w != WX_CLEAR) w = (uint8_t)(h & 1 ? WX_CLEAR : WX_FOG);
    if (day_count == 0 && world_dist(pl_mx, pl_my, world.start.x, world.start.y) < 40) w = WX_CLEAR;
    if (w != weather) {
        weather = w;
        fx_weather_roll();
    }
}

static void ambient_update(void)
{
    if (biome_here != amb_biome || phase != amb_phase || weather != amb_wx) {
        amb_biome = biome_here;
        amb_phase = phase;
        amb_wx = weather;
        ambient_set(biome_here, phase, weather);
    }
}

static void scan_warm(void)
{
    int8_t dx, dy;
    uint8_t m;
    near_warm = 0;
    for (dy = -3; dy <= 3; dy++)
        for (dx = -3; dx <= 3; dx++) {
            m = land_mt((uint16_t)(pl_mx + dx), (uint16_t)(pl_my + dy));
            if (mt_flags[m] & MTF_WARM) { near_warm = 1; return; }
        }
}

static void warmth_tick(uint8_t tf)
{
    uint16_t rate = 0;
    uint8_t m;
    if (near_warm) {
        warmth += (uint16_t)(6u * tf);
        if (warmth > WARMTH_MAX) warmth = WARMTH_MAX;
        if ((items & (1 << IT_STONES)) && stones < STONES_MAX) stones = STONES_MAX;
        return;
    }
    if (phase == PH_NIGHT) {
        rate = 5;
        if (biome_here == B_TUNDRA || biome_here == B_DESERT) rate = 10;
        m = land_mt(pl_mx, pl_my);
        if (mt_flags[m] & MTF_COLD) rate += 5;
        if (weather == WX_RAIN || weather == WX_STORM) rate += 3;
        if (weather == WX_SNOW) rate += 5;
    } else if (phase == PH_DAY) {
        warm_acc += (uint16_t)(4u * tf);
        while (warm_acc >= 64) { warm_acc -= 64; if (warmth < WARMTH_MAX) warmth++; }
        return;
    } else if (biome_here == B_TUNDRA) {
        rate = 3;       /* the cold of the snowfields bites at dawn and dusk too */
    }
    warm_acc += (uint16_t)(rate * tf);
    while (warm_acc >= 64) {
        warm_acc -= 64;
        if (warmth) warmth--;
    }
}

static void time_tick(void)
{
    uint8_t tf = (uint8_t)(pl_state == PL_SIT ? 8 : 1);
    if (pl_state == PL_SLEEP) tf = 0;
    tod = (uint16_t)(tod + tf);
    if (tod >= DAY_FRAMES) { tod -= DAY_FRAMES; day_count++; }
    phase_update(0);
    tick8++;
    if ((tick8 & 7) == 0) scan_warm();
    if ((tick8 & 15) == 0) visit_mark();
    if ((tick8 & 31) == 0) {
        biome_here = world_biome(pl_mx, pl_my);
        weather_update();
        ambient_update();
    }
    warmth_tick(tf);
}

/* ---------------------------------------------------------------- transitions */
static void new_world(uint16_t seed)
{
    world_init(seed);
    world_mods_clear();
    items = 1 << IT_LANTERN;
    equipped = IT_LANTERN;
    beacons_lit = 0;
    heart_revealed = 0;
    stones = 0;
    warmth = WARMTH_MAX;
    tod = 600;          /* early dawn */
    day_count = 0;
    cairn_n = 0;
    memset(visited, 0, sizeof visited);
    respawn_x = world.start.x;
    respawn_y = world.start.y;
    player_place(world.start.x, (uint16_t)(world.start.y + 1));
    pl_face = D_N;
    pl_state = PL_SLEEP;
    ambient_seed(seed);
    ambient_beacons(0);
}

static uint16_t fresh_seed(void)
{
    uint16_t s;
    if (dbg_seed) { s = dbg_seed; dbg_seed = 0; return s; }
    s = (uint16_t)(((uint16_t)DIV_REG << 8) ^ vbl_frames ^ (world.seed * 31u) ^ 0x2B1Du);
    if (!s) s = 0x2B1D;
    return s;
}

/* Build the world view around the player: expects the screen faded to white. */
void world_enter(uint8_t fresh) BANKED
{
    (void)fresh;
    game_state = GS_WORLD;
    split_enable(0);
    anim_on = 0;
    hide_sprites_from(0);
    wait_frames(1);
    gfx_load_world_tiles();
    band_build();
    camera_update();
    land_refill(cam_mx, cam_my);
    phase = 0xFF;
    pal_band_bright = 0;
    pal_flash = 0;
    phase_update(1);
    heart_revealed = (uint8_t)(beacons_lit == 7);
    ambient_beacons(beacons_lit);
    biome_here = world_biome(pl_mx, pl_my);
    wx_region_x = 0xFFFF;
    weather_update();
    pal_fog = weather == WX_FOG ? 8 : 0;
    amb_biome = 0xFF;
    ambient_update();
    band_reset_angle();
    watchers_reset();
    scan_warm();
    visit_mark();
    anim_on = 1;
    band_update();
    player_draw();
    fx_update();
    frame_commit();
    frame_sync();
    split_enable(1);
    wait_frames(2);
    fade_to(0, 1);
}

static void whiteout(void)
{
    game_state = GS_WHITEOUT;
    sfx_play(SFX_WHITEOUT);
    ambient_mode(AMB_SILENT);
    pl_state = PL_STAND;
    fade_to(16, 1);
    wait_frames(60);
    player_place(respawn_x, (uint16_t)(respawn_y + 1));
    pl_face = D_N;
    pl_state = PL_SLEEP;
    tod = 600;
    day_count++;
    warmth = WARMTH_MAX;
    world_enter(0);
    ambient_mode(AMB_WAKE);
    wake_t = 0;
}

static void carry_cairns(void)
{
    uint8_t i, j;
    int16_t dx, dy;
    for (i = 0; i < cairn_n && world_old_cairn_count < MAX_OLD_CAIRNS; i++) {
        dx = (int16_t)(cairns[i].x - world.start.x) >> 2;
        dy = (int16_t)(cairns[i].y - world.start.y) >> 2;
        if (dx < -128 || dx > 127 || dy < -128 || dy > 127) continue;
        for (j = 0; j < world_old_cairn_count; j++)
            if (world_old_cairns[j].dx == (int8_t)dx && world_old_cairns[j].dy == (int8_t)dy) break;
        if (j < world_old_cairn_count) continue;
        world_old_cairns[world_old_cairn_count].dx = (int8_t)dx;
        world_old_cairns[world_old_cairn_count].dy = (int8_t)dy;
        world_old_cairn_count++;
    }
}

static void ending(void)
{
    uint8_t i;
    game_state = GS_ENDING;
    ending_req = 0;
    pl_state = PL_SIT;
    pl_face = D_N;
    hint_on = 0;
    ambient_mode(AMB_ENDING);
    ambient_beacons(7);
    for (i = 0; i < 240; i++) {
        frame_sync();
        if ((i & 7) == 0 && pal_band_bright < 16) { pal_band_bright++; pal_apply(); }
        camera_update();
        band_update();
        player_draw();
        fx_update();
        frame_commit();
    }
    for (i = 0; i < 16; i++) { pal_fade = (uint8_t)(i + 1); pal_apply(); wait_frames(6); }
    hide_sprites_from(0);
    wait_frames(60);
    worlds_done++;
    carry_cairns();
    new_world(fresh_seed());
    save_write();
    world_enter(1);
    ambient_mode(AMB_WAKE);
    wake_t = 0;
}

static void world_loop(void)
{
    uint8_t start_vbl, ly;
    for (;;) {
        frame_sync();
        start_vbl = vbl_frames;
        input();
        dbg_world_frames++;
        if (dbg_teleport) {
            dbg_teleport = 0;
            player_place(dbg_tx, dbg_ty);
            pl_state = PL_STAND;
            camera_update();
            land_refill(cam_mx, cam_my);
            band_reset_angle();
            scan_warm();
        }
        if (pl_state == PL_SLEEP) {
            if (wake_t < 255) wake_t++;
            if (wake_t > 30 && (pressed & (J_A | J_B | J_UP | J_DOWN | J_LEFT | J_RIGHT | J_START))) {
                pl_state = PL_STAND;
                idle_t = 0;
                pressed = 0;
                ambient_mode(AMB_WORLD);
            }
        } else if (wake_t < 255) {
            wake_t++;
        }
        if (pressed & J_START && pl_state != PL_GLIDE && pl_state != PL_SLEEP) {
            map_screen();
            continue;
        }
        player_update();
        if (ending_req) { ending(); continue; }
        camera_update();
        time_tick();
        watchers_update();
        band_update();
        player_draw();
        fx_update();
        if (!warmth) { frame_commit(); whiteout(); continue; }
        frame_commit();
        /* stream with the rest of the frame (at least 2 cells, more while time remains) */
        if (land_update(cam_mx, cam_my, 2)) {
            land_refill(cam_mx, cam_my);
        } else {
            for (;;) {
                if (!land_job && bq_pending() > 40) break;
                ly = LY_REG;
                if (vbl_frames != start_vbl || (ly >= 112 && ly < 144)) break;
                if (land_update(cam_mx, cam_my, 1)) break;
                if (!land_job && (uint16_t)((uint16_t)(cam_mx - 2) - land_x0) == 0 &&
                    (uint16_t)((uint16_t)(cam_my - 3) - land_y0) == 0) break;
            }
        }
    }
}

/* ---------------------------------------------------------------- title */
#define TSPR_BASE 100   /* generated title sprite tiles (runes, tallies) */

static void gen_rune(uint8_t slot, uint8_t v)
{
    uint8_t t[32], y, bit;
    memset(t, 0, sizeof t);
    /* stem */
    for (y = 1; y < 15; y++) { bit = 0x18; t[y * 2] |= bit; t[y * 2 + 1] |= bit; }
    /* four twigs chosen by the nibble */
    for (y = 0; y < 3; y++) {
        if (v & 1) { t[(4 - y) * 2] |= (uint8_t)(0x04 >> y); t[(4 - y) * 2 + 1] |= (uint8_t)(0x04 >> y); }
        if (v & 2) { t[(4 - y) * 2] |= (uint8_t)(0x20 << y); t[(4 - y) * 2 + 1] |= (uint8_t)(0x20 << y); }
        if (v & 4) { t[(10 + y) * 2] |= (uint8_t)(0x04 >> y); t[(10 + y) * 2 + 1] |= (uint8_t)(0x04 >> y); }
        if (v & 8) { t[(10 + y) * 2] |= (uint8_t)(0x20 << y); t[(10 + y) * 2 + 1] |= (uint8_t)(0x20 << y); }
    }
    if (!v) { t[7 * 2] |= 0x24; t[7 * 2 + 1] |= 0x24; t[8 * 2] |= 0x24; t[8 * 2 + 1] |= 0x24; }
    set_sprite_data(slot, 2, t);
}

static void gen_tally(uint8_t slot, uint8_t n)
{
    uint8_t t[32], y, row, k;
    memset(t, 0, sizeof t);
    for (y = 3; y < 13; y++) {
        row = 0;
        for (k = 0; k < n && k < 4; k++) row |= (uint8_t)(0x40 >> (k * 2));
        if (n >= 5) row |= (uint8_t)(0x80 >> ((y - 3) * 7 / 9));
        t[y * 2] = row;
        t[y * 2 + 1] = row;
    }
    set_sprite_data(slot, 2, t);
}

static void title_sprites(uint8_t have_save, uint16_t seed, uint8_t t)
{
    uint8_t i, n, x, pal_ui = is_cgb ? OPAL_UI : 0;
    uint8_t flick = (uint8_t)((t >> 3) & 1);
    /* the flame */
    spr_set(0, 80 - 4 + 8, (uint8_t)(16 + 104 - flick), SPR_PIP_FULL, (uint8_t)(is_cgb ? OPAL_EMBER : S_PALETTE));
    /* tally marks, groups of five */
    n = worlds_done > 40 ? 40 : worlds_done;
    x = 80 + 8 + 8;
    for (i = 0; i < 8; i++) {
        if (n) {
            uint8_t g = n >= 5 ? 5 : n;
            spr_set((uint8_t)(1 + i), x, 16 + 104, (uint8_t)(TSPR_BASE + 8 + (g - 1) * 2), pal_ui);
            n = (uint8_t)(n - g);
            x = (uint8_t)(x + 9);
        } else spr_hide((uint8_t)(1 + i));
    }
    /* seed sigil */
    for (i = 0; i < 4; i++) {
        if (have_save) spr_set((uint8_t)(10 + i), (uint8_t)(80 - 22 + i * 12 + 8), 16 + 124, (uint8_t)(TSPR_BASE + i * 2), pal_ui);
        else spr_hide((uint8_t)(10 + i));
    }
    (void)seed;
}

static void title_sigil(uint16_t seed)
{
    uint8_t i;
    for (i = 0; i < 4; i++) gen_rune((uint8_t)(TSPR_BASE + i * 2), (uint8_t)((seed >> (12 - i * 4)) & 15));
    for (i = 1; i <= 5; i++) gen_tally((uint8_t)(TSPR_BASE + 8 + (i - 1) * 2), i);
}

/* returns 1 = continue, 2 = new world */
static uint8_t title(void)
{
    uint8_t t = 0, have = save_exists();
    uint16_t seed = 0;
    game_state = GS_TITLE;
    split_enable(0);
    anim_on = 0;
    hide_sprites_from(0);
    pal_fade = 16;
    pal_title();
    nx_land_bgp = 0x00;
    wait_frames(2);
    set_bkg_data(0, TITLE_TILE_COUNT, title_tiles);
    set_tiles(0, 0, 20, 18, (uint8_t *)0x9800, title_map);
    if (is_cgb) {
        VBK_REG = 1;
        set_tiles(0, 0, 20, 18, (uint8_t *)0x9800, title_attr);
        VBK_REG = 0;
    }
    set_sprite_data(0, SPR_TILE_COUNT, spr_tiles);
    if (have) {
        /* peek at the saved seed / worlds for the sigil and tallies */
        uint16_t ps = world.seed;
        if (save_load()) seed = world.seed;
        (void)ps;
    }
    title_sigil(seed);
    nx_scx = 0;
    nx_scy = 0;
    ambient_mode(AMB_TITLE);
    /* fade in */
    while (pal_fade) {
        pal_fade--;
        pal_title();
        nx_land_bgp = shade_fade(0xE4, pal_fade);
        nx_obp0 = dmg_obp0[PH_DAY];
        nx_obp1 = dmg_obp1[PH_DAY];
        title_sprites(have, seed, t++);
        wait_frames(3);
    }
    for (;;) {
        frame_sync();
        input();
        t++;
        title_sprites(have, seed, t);
        frame_commit();
        if (pressed & (J_A | J_START)) return have ? 1 : 2;
        if (pressed & J_SELECT) return 2;
    }
}

void game_main(void) BANKED
{
    uint8_t choice;
    game_state = GS_BOOT;
    sound_init();
    gfx_init();
    DISPLAY_ON;
    for (;;) {
        choice = title();
        sfx_play(SFX_SELECT);
        pal_fade = 0;
        fade_to(16, 1);
        if (choice == 1 && save_load()) {
            pl_state = PL_SLEEP;
            heart_revealed = (uint8_t)(beacons_lit == 7);
            ambient_seed(world.seed);
        } else {
            new_world(fresh_seed());
            save_write();
        }
        world_enter(1);
        ambient_mode(AMB_WAKE);
        wake_t = 0;
        world_loop();
    }
}
