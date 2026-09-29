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
uint8_t near_warm, warm_dirty, warm6;
uint16_t dbg_world_frames;
uint8_t dbg_ly[10];
uint8_t dbg_refills;
uint16_t dbg_dmax[10], dbg_tm[8], dbg_tsum[8];
static uint8_t st_l[10], st_v[10], tm_l, tm_v;
/* cheap LY stamps: lines between two stamps, assuming less than two frames apart */
static uint8_t lrel(void) { uint8_t l = LY_REG; return (uint8_t)(l >= 144 ? l - 144 : l + 10); }
#define LREL(x) lrel()
#define STAMP(i) do { st_l[i] = LREL(LY_REG); st_v[i] = vbl_frames; } while (0)
static uint16_t st_d(uint8_t a, uint8_t b)
{
    uint16_t d = (uint16_t)(st_l[b] - st_l[a]);
    if (st_v[b] != st_v[a]) d = (uint16_t)(d + 154);
    return (uint16_t)(d & 0x3FF);
}
#define TM_B() do { } while (0)       /* profiling hooks (define to measure) */
#define TM_E(k) do { } while (0)

static uint8_t prev_keys;
static uint16_t warm_acc;
static uint8_t amb_biome = 0xFF, amb_phase = 0xFF, amb_wx = 0xFF;
static uint16_t wx_region_x = 0xFFFF, wx_region_y = 0xFFFF, wx_day = 0xFFFF;
static uint8_t wx_biome = 0xFF;
static uint8_t wake_t;
static uint8_t tick8;
static uint16_t shake_rng = 0xBEEF;

/* biome from the metatile underfoot (world_biome is far too slow to call from the loop) */
static const uint8_t mt_biome[MT_COUNT] = {
    B_SEA, B_SEA, B_SHALLOW, B_SHALLOW, B_SHORE, B_MEADOW, B_MEADOW, B_MEADOW,   /* sea .. flowers */
    B_FOREST, B_FOREST, B_TUNDRA, B_TUNDRA, B_DESERT, B_DESERT, B_ROCK, B_ROCK,   /* tree .. peak */
    B_ASH, B_ASH, B_ASH, B_RUINS, B_RUINS, B_RUINS, B_RUINS, 0xFF,               /* ash .. road */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF        /* set pieces keep */
};

static void biome_update(void)
{
    uint8_t b = mt_biome[land_rel(0, 0)];
    if (b != 0xFF) biome_here = b;
}

static uint8_t carry_pressed;     /* presses from a game frame that skipped the wanderer */
static void input(void)
{
    prev_keys = keys;
    keys = joypad();
    pressed = (uint8_t)((keys & ~prev_keys) | carry_pressed);
    carry_pressed = 0;
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
/* World edits requested during play (from the VBL ISR) are shown at once through the land
 * cache, and queued; the main loop applies them to the core's mod table between world_mt calls
 * (the core is not re-entrant), then saves if asked. */
#define EQ_N 8
static uint16_t eq_x[EQ_N], eq_y[EQ_N];
static uint8_t eq_mt[EQ_N];
static volatile uint8_t eq_head, eq_tail;
volatile uint8_t save_req;
#define EQ_REMOVE 0xFF

static uint8_t eq_push(uint16_t mx, uint16_t my, uint8_t mt)
{
    uint8_t h = eq_head, n = (uint8_t)((h + 1) & (EQ_N - 1));
    if (n == eq_tail) return 0;
    eq_x[h] = mx; eq_y[h] = my; eq_mt[h] = mt;
    eq_head = n;
    return 1;
}

/* room for another mod, keeping `reserve` slots free (progression edits pass 0; cairns and
   fires keep EDIT_RESERVE free, so beacons, shrines, stepping stones and brambles always fit) */
uint8_t edit_room(uint8_t reserve) BANKED
{
    uint8_t used = (uint8_t)(world_mod_count + ((eq_head - eq_tail) & (EQ_N - 1)));
    return (uint8_t)(used < MAX_MODS && (uint8_t)(MAX_MODS - used) > reserve);
}

/* 1 if the edit was accepted (shown now, applied to the mods by the main loop); 0 (with the
   "no" sound) if the table is full or the queue is busy: the caller must not advance */
uint8_t edit_mt_r(uint16_t mx, uint16_t my, uint8_t mt, uint8_t reserve) BANKED
{
    if (!edit_room(reserve) || !eq_push(mx, my, mt)) { sfx_play(SFX_NO); shake = 3; return 0; }
    land_set(mx, my, mt);
    warm_dirty = 1;
    return 1;
}

uint8_t edit_mt(uint16_t mx, uint16_t my, uint8_t mt) BANKED
{
    return edit_mt_r(mx, my, mt, 0);
}

void edit_remove(uint16_t mx, uint16_t my) BANKED
{
    eq_push(mx, my, EQ_REMOVE);
}

/* main loop: apply one queued edit; returns 1 if it did something */
static uint8_t eq_apply(void)
{
    uint8_t t = eq_tail, i, m;
    uint16_t x, y;
    if (t == eq_head) return 0;
    x = eq_x[t]; y = eq_y[t]; m = eq_mt[t];
    if (m == EQ_REMOVE) {
        for (i = 0; i < world_mod_count; i++) {
            if (world_mods[i].x == x && world_mods[i].y == y) {
                world_mod_count--;
                world_mods[i] = world_mods[world_mod_count];
                world_mods_rebuild();
                break;
            }
        }
        land_set(x, y, world_mt(x, y));
    } else {
        world_mod_set(x, y, m);
    }
    warm_dirty = 1;           /* warmth is read from the mods: look again now it is there */
    eq_tail = (uint8_t)((t + 1) & (EQ_N - 1));
    return 1;
}

static void main_flush(void)
{
    while (eq_apply()) ;
    if (save_req) { save_req = 0; save_write(); }
}

void light_beacon(uint8_t i) BANKED
{
    if (!edit_mt(world.beacon[i].x, world.beacon[i].y, MT_BEACON_LIT)) return;
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
    save_req = 1;
}

/* ---------------------------------------------------------------- visited chunks */
static uint8_t chunk_of(uint16_t m, uint16_t s, uint8_t *out)
{
    int16_t d = (int16_t)(m - s);
    d = (int16_t)((d >> 4) + (VISIT_W / 2));
    if (d < 0 || d >= VISIT_W) return 0;
    *out = (uint8_t)d;
    return 1;
}

/* main-loop jobs read the wanderer / camera position through one atomic snapshot (the VBL
 * game frame moves them; a 16-bit read could straddle it) */
static uint16_t s_mx, s_my, s_cx, s_cy;
static void snap(void)
{
    __critical { s_mx = pl_mx; s_my = pl_my; s_cx = cam_mx; s_cy = cam_my; }
}

static void visit_at(uint16_t mx, uint16_t my)
{
    uint8_t cx, cy;
    if (!chunk_of(mx, world.start.x, &cx) || !chunk_of(my, world.start.y, &cy)) return;
    visited[((uint16_t)cy << 4) | (cx >> 3)] |= (uint8_t)(1 << (cx & 7));
}

void visit_mark(void) BANKED
{
    snap();
    visit_at(s_mx, s_my);
    visit_at(s_cx, s_cy);
    visit_at((uint16_t)(s_cx + 10), s_cy);
    visit_at(s_cx, (uint16_t)(s_cy + 8));
    visit_at((uint16_t)(s_cx + 10), (uint16_t)(s_cy + 8));
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

static uint8_t stars_shown = 0xFF;
static const uint16_t phase_start[4] = { T_DAWN, T_DAY, T_DUSK, T_NIGHT };

static void phase_update(uint8_t instant)
{
    uint16_t now, into;
    uint8_t p, t;
    __critical { now = tod; }
    p = phase_of(now);
    into = (uint16_t)(now - phase_start[p]);
    if (p != phase) {
        uint8_t old = phase;
        phase = p;
        pal_phase_from = (instant || old > 3) ? p : old;
        pal_phase_to = p;
        pal_t = 0xFF;
        if (p == PH_DAWN && !instant) { __critical { sfx_play(SFX_DAWN); } }
    }
    t = (uint8_t)(p == PH_NIGHT || p == PH_DUSK);
    if (t != stars_shown && band_stars(t)) stars_shown = t;
    t = into >= PHASE_BLEND ? 16 : (uint8_t)(into >> 6);
    if (instant) t = 16;
    if (t != pal_t) {
        pal_t = t;
        pal_apply();
    }
    glow_on = (uint8_t)(p == PH_NIGHT || (p == PH_DUSK && into > 2000));
}

/* weather hash (own: the core's hashes are not re-entrant and this runs in the VBL ISR) */
static uint8_t wx_hash(uint16_t a, uint16_t b)
{
    /* xorshift mixing only: no multiplies (this runs in the VBL ISR) */
    uint16_t x = (uint16_t)(a ^ 0x5A17u);
    x ^= (uint16_t)(x << 7);
    x ^= (uint16_t)(x >> 9);
    x ^= b;
    x ^= (uint16_t)(x << 8);
    x ^= (uint16_t)(x >> 7);
    x ^= (uint16_t)(x << 5);
    return (uint8_t)(x ^ (x >> 8));
}

static void weather_update(void)
{
    uint16_t rx, ry, dc;
    uint8_t h, w, b;
    snap();
    __critical { dc = day_count; }
    rx = (uint16_t)(s_mx >> 6); ry = (uint16_t)(s_my >> 6);
    /* the biome is part of the key: its rules (snow in tundra, dry desert) change the result */
    if (rx == wx_region_x && ry == wx_region_y && dc == wx_day && biome_here == wx_biome) return;
    wx_region_x = rx;
    wx_region_y = ry;
    wx_day = dc;
    wx_biome = biome_here;
    h = wx_hash((uint16_t)(rx ^ world.seed), (uint16_t)(ry ^ (dc << 6) ^ (dc << 11)));
    b = biome_here;
    if (h < 160) w = WX_CLEAR;
    else if (h < 200) w = WX_RAIN;
    else if (h < 230) w = WX_FOG;
    else w = WX_STORM;
    if (b == B_TUNDRA && (w == WX_RAIN || w == WX_STORM)) w = WX_SNOW;
    if (b == B_DESERT && w != WX_CLEAR) w = (uint8_t)(h & 1 ? WX_CLEAR : WX_FOG);
    if (dc == 0 && (uint16_t)(s_mx - world.start.x + 40) < 80 && (uint16_t)(s_my - world.start.y + 40) < 80)
        w = WX_CLEAR;
    weather = w;          /* fx re-rolls the drops itself when the weather changes */
}

static void ambient_update(void)
{
    if (biome_here != amb_biome || phase != amb_phase || weather != amb_wx) {
        amb_biome = biome_here;
        amb_phase = phase;
        amb_wx = weather;
        __critical { ambient_set(biome_here, phase, weather); }
    }
}

/* a lit fire or beacon within Chebyshev radius r of the wanderer? Warmth sources only ever
   come from mods (lighting), so this walks the (short) mods table, not the land */
uint8_t warm_within(uint8_t r) BANKED
{
    uint8_t i, n = world_mod_count, m, r2 = (uint8_t)(r << 1);
    const wmod_t *p = world_mods;
    snap();
    for (i = 0; i < n; i++, p++) {
        m = p->mt;
        if (m != MT_FIRE_LIT && m != MT_BEACON_LIT) continue;
        if ((uint16_t)(p->x - s_mx + r) > r2) continue;
        if ((uint16_t)(p->y - s_my + r) > r2) continue;
        return 1;
    }
    return 0;
}

static uint16_t warm_mx = 0xFFFF, warm_my;
static void scan_warm(void)
{
    near_warm = warm_within(3);     /* takes the snapshot */
    warm_mx = s_mx;
    warm_my = s_my;
}

/* rescan only when the wanderer changes cell or the land changed (a fire was lit) */
static void scan_warm_maybe(void)
{
    snap();
    if (s_mx != warm_mx || s_my != warm_my || warm_dirty) { warm_dirty = 0; scan_warm(); }
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
    /* (rates in 1/64 warmth per frame; 1023 is full) a clear night in the meadow away from any
       fire costs about seven eighths of your warmth instead of more than all of it; a snowy
       tundra night still bites, but leaves about 80 s to find a fire instead of under one */
    if (phase == PH_NIGHT) {
        rate = 5;
        if (biome_here == B_TUNDRA || biome_here == B_DESERT) rate = 7;
        m = land_rel(0, 0);
        if (mt_flags[m] & MTF_COLD) rate += 3;
        if (weather == WX_RAIN || weather == WX_STORM) rate += 2;
        if (weather == WX_SNOW) rate += 3;
    } else if (phase == PH_DAY) {
        warm_acc += (uint16_t)(4u * tf);
        while (warm_acc >= 64) { warm_acc -= 64; if (warmth < WARMTH_MAX) warmth++; }
        return;
    } else if (biome_here == B_TUNDRA) {
        rate = 2;       /* the cold of the snowfields bites at dawn and dusk too */
    }
    warm_acc += (uint16_t)(rate * tf);
    while (warm_acc >= 64) {
        warm_acc -= 64;
        if (warmth) warmth--;
    }
}

static void time_tick(void)
{
    /* sitting fast-forwards time only within reach of a fire: in the open it is just a rest */
    uint8_t tf = (uint8_t)(pl_state == PL_SIT && near_warm ? 8 : 1);
    if (pl_state == PL_SLEEP) tf = 0;
    tod = (uint16_t)(tod + tf);
    if (tod >= DAY_FRAMES) { tod -= DAY_FRAMES; day_count++; }
    /* the occasional jobs (phase and palettes, warmth scan, map, weather, biome, music,
       band bearings) run in the main loop: bg_task() */
    warmth_tick(tf);
}

/* Main loop, one job per new frame (between streaming calls), so the VBlank game frame only
 * does what must happen every frame. Reads the ISR-owned state (a snapshot where it matters). */
static uint8_t bg_frame;
void band_bearing_task(void) BANKED;

static void bg_task(void)
{
    uint8_t f = vbl_frames;
    if (f == bg_frame) {
        if (pal_dirty) { pal_dirty = 0; pal_apply(); }   /* palettes asap */
        return;
    }
    bg_frame = f;
    switch (f & 7) {
    case 0: phase_update(0); break;
    case 1: case 5: band_bearing_task(); break;
    case 2: case 6: scan_warm_maybe(); break;
    case 3: if (f & 8) visit_mark(); break;
    case 4: warm6 = warm_within(6); break;
    case 7:
        switch ((f >> 3) & 3) {
        case 0: biome_update(); break;
        case 1: weather_update(); break;
        case 2: ambient_update(); break;
        }
        break;
    }
    if (pal_dirty) { pal_dirty = 0; pal_apply(); }
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
    /* the moment you press / arrive (DIV runs at 16 kHz) plus how the last world was played */
    s = (uint16_t)(((uint16_t)DIV_REG << 8) ^ vbl_frames ^ (world.seed * 31u) ^ 0x2B1Du);
    s ^= (uint16_t)(tod ^ (day_count << 9) ^ ((uint16_t)pl_mx << 3) ^ pl_my ^ ((uint16_t)cairn_n << 12));
    if (s == world.seed) s ^= 0x9E37;       /* never the same world twice in a row */
    if (!s) s = 0x2B1D;
    return s;
}

/* Build the world view around the player: expects the screen faded to white. */
void world_enter(uint8_t fresh) BANKED
{
    dbg_count_on = 0;
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
    stars_shown = 0xFF;
    pal_band_bright = 0;
    pal_flash = 0;
    phase_update(1);
    heart_revealed = (uint8_t)(beacons_lit == 7);
    ambient_beacons(beacons_lit);
    biome_here = B_MEADOW;
    biome_update();
    wx_region_x = 0xFFFF;
    weather_update();
    pal_fog = weather == WX_FOG ? 8 : 0;
    amb_biome = 0xFF;
    ambient_update();
    band_reset_angle();
    fx_redraw();
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
    dbg_count_on = 0;
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
    warmth = WARMTH_WAKE;
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
    dbg_count_on = 0;
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

/* ---------------------------------------------------------------- the world loop */
enum { REQ_NONE = 0, REQ_MAP, REQ_WHITEOUT, REQ_ENDING, REQ_TELEPORT, REQ_REFILL };
volatile uint8_t world_req;
uint16_t dbg_stalls, dbg_late;
volatile uint8_t dbg_stage;
uint8_t dbg_hook_ly;          /* frames the wanderer waited for the streamer */

static void request(uint8_t r)
{
    world_req = r;
    hook_on = 0;
}

/* One frame of play. Runs from the VBlank interrupt (after the VRAM work and sound), so it
 * must not call the world core: it reads the streamed cache only. */
void world_frame(void) BANKED
{
    int16_t rx, ry;
    uint8_t ly = LY_REG;
    /* a late start (the VBlank was held off) could run into the next frame and be seen half
       done: skip this frame instead */
    if (ly >= 100 && ly < 144) { if (dbg_count_on) { dbg_frame_drops++; dbg_late++; } return; }
    dbg_count_on = 1;
    dbg_stage = 1;
    input();
    dbg_world_frames++;
    if (dbg_teleport == 1) { dbg_teleport = 2; request(REQ_TELEPORT); return; }
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
    if ((pressed & J_START) && pl_state != PL_GLIDE && pl_state != PL_SLEEP) { request(REQ_MAP); return; }
    /* The wanderer waits (rarely) when the streamer is at the edge of its slack. Pixel gate:
       the view must stay off the outer half of the window's edge columns / rows, which are
       provisional (their far neighbours are not loaded, so their water edges are not final),
       with 2 px for this frame's move and some for the VBlank queue. */
    rx = (int16_t)(((int16_t)(cam_mx - land_x0) << 4) + cam_sx);
    ry = (int16_t)(((int16_t)(cam_my - land_y0) << 4) + cam_sy);
    if (rx < 0 || rx > 88 || ry < 0 || ry > 128) {
        carry_pressed = (uint8_t)(pressed & (J_A | J_SELECT));
        request(REQ_REFILL);
        return;
    }
    PSTAGE(PF_INPUT);
    if (rx < 10 || rx > 62 || ry < 10 || ry > 102) {
        /* the wanderer waits for the streamer: a press this frame is kept, not dropped */
        dbg_stalls++;
        carry_pressed = (uint8_t)(pressed & (J_A | J_SELECT));
    }
    else player_update();
    PSTAGE(PF_PLAYER);
    dbg_stage = 2;
    if (ending_req) { request(REQ_ENDING); return; }
    camera_update();
    time_tick();
    PSTAGE(PF_TIME);
    dbg_stage = 3;
    if (watch_on || (vbl_frames & 15) == 4) watchers_update();   /* idle: roll every 16 frames (slot 4) */
    PSTAGE(PF_WATCH);
    band_update();
    PSTAGE(PF_BAND);
    dbg_stage = 4;
    player_draw();
    PSTAGE(PF_DRAW);
    dbg_stage = 5;
    fx_update();
    PSTAGE(PF_FX);
    dbg_stage = 6;
    frame_commit();
    dbg_stage = 7;
    if (!warmth) request(REQ_WHITEOUT);
    PSTAGE(PF_COMMIT);
    if (prof_t > dbg_pmax[PF_END]) { dbg_pmax[PF_END] = prof_t; dbg_pcur[PF_END] = prof_t; memcpy(dbg_pworst, dbg_pcur, 16); }
    if (prof_t > dbg_hook_ly) dbg_hook_ly = prof_t;
}

/* The mods table is finite (and saved as is). When it runs low, the edits that are easy to make
 * again and far out of sight return to the land, farthest first: a lit fire burns down (never
 * the one you would wake at), a stepping stone sinks, burnt thorns grow back. Beacons, shrines
 * and cairns are never touched. Without this a long world filled the table and the fires on the
 * way to the Heart could no longer be lit. Main loop only (the core is not re-entrant). */
#define TIDY_FREE (EDIT_RESERVE + 8)     /* keep this many slots free */
#define TIDY_DIST 20                     /* only well outside the streamed window */
static uint8_t mods_tidy(void)
{
    uint8_t i, best = 0xFF, m;
    uint16_t bd = TIDY_DIST, d, e;
    const wmod_t *p = world_mods;
    uint16_t rsx, rsy;
    if ((uint8_t)(MAX_MODS - world_mod_count) > TIDY_FREE) return 0;
    snap();
    __critical { rsx = respawn_x; rsy = respawn_y; }
    for (i = 0; i < world_mod_count; i++, p++) {
        m = p->mt;
        if (m != MT_FIRE_LIT && m != MT_STEPSTONE && m != MT_ASH) continue;
        if (m == MT_FIRE_LIT && p->x == rsx && p->y == rsy) continue;
        d = (uint16_t)(p->x - s_mx); if (d & 0x8000) d = (uint16_t)-d;
        e = (uint16_t)(p->y - s_my); if (e & 0x8000) e = (uint16_t)-e;
        if (e > d) d = e;
        if (d > bd) { bd = d; best = i; }
    }
    if (best == 0xFF) return 0;
    {
        uint16_t x = world_mods[best].x, y = world_mods[best].y;
        world_mod_count--;
        world_mods[best] = world_mods[world_mod_count];
        world_mods_rebuild();
        land_set(x, y, world_mt(x, y));
    }
    warm_dirty = 1;
    return 1;
}

static void world_run(void)
{
    uint8_t r, cx0, cy0;
    uint16_t cx, cy;
    for (;;) {
        world_req = REQ_NONE;
        hook_on = 1;
        while (!world_req) {
            bg_task();
            if (eq_apply()) continue;
            if (!(vbl_frames & 15) && mods_tidy()) continue;
            if (save_req) { save_req = 0; save_write(); continue; }
            __critical { cx = cam_mx; cy = cam_my; }
            r = land_update(cx, cy, 3);
            if (r == 2) {
                static const int8_t fdx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
                static const int8_t fdy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
                uint8_t f = pl_face & 7;
                if (!land_prefetch(fdx[f], fdy[f])) { __asm__("halt"); __asm__("nop"); }
            }
            else if (r == 1) request(REQ_REFILL);
        }
        while (hook_busy) { __asm__("halt"); __asm__("nop"); }
        dbg_count_on = 0;
        main_flush();
        switch (world_req) {
        case REQ_MAP:
            map_screen();
            break;
        case REQ_WHITEOUT:
            whiteout();
            break;
        case REQ_ENDING:
            ending();
            break;
        case REQ_TELEPORT:
            player_place(dbg_tx, dbg_ty);
            pl_state = PL_STAND;
            camera_update();
            land_refill(cam_mx, cam_my);
            band_reset_angle();
            scan_warm();
            dbg_teleport = 0;
            break;
        case REQ_REFILL:
            dbg_refills++;
            camera_update();
            land_refill(cam_mx, cam_my);
            break;
        }
        (void)cx0; (void)cy0;
    }
}

/* ---------------------------------------------------------------- title */
#define TSPR_BASE 100   /* generated title sprite tiles (runes, tallies) */

/* an 8x16 sprite from a 16-row stroke mask: strokes in colour 3 with a colour-1 halo */
static void gen_glyph(uint8_t slot, const uint8_t *mask)
{
    uint8_t t[32], y, m, h;
    for (y = 0; y < 16; y++) {
        m = mask[y];
        h = (uint8_t)(m | (m << 1) | (m >> 1));
        if (y) h |= mask[y - 1];
        if (y < 15) h |= mask[y + 1];
        t[y * 2] = h;          /* low plane: halo + stroke */
        t[y * 2 + 1] = m;      /* high plane: stroke */
    }
    set_sprite_data(slot, 2, t);
}

/* a rune for one nibble of the seed: a stem with up to four twigs */
static void gen_rune(uint8_t slot, uint8_t v)
{
    uint8_t mk[16], y;
    memset(mk, 0, sizeof mk);
    for (y = 2; y < 14; y++) mk[y] = 0x10;
    for (y = 0; y < 3; y++) {
        if (v & 1) mk[5 - y] |= (uint8_t)(0x08 >> y);
        if (v & 2) mk[5 - y] |= (uint8_t)(0x20 << y);
        if (v & 4) mk[10 + y] |= (uint8_t)(0x08 >> y);
        if (v & 8) mk[10 + y] |= (uint8_t)(0x20 << y);
    }
    if (!v) { mk[7] |= 0x28; mk[8] |= 0x28; }
    gen_glyph(slot, mk);
}

/* n tally marks (1..5): four strokes and the fifth across */
static void gen_tally(uint8_t slot, uint8_t n)
{
    uint8_t mk[16], y, k;
    memset(mk, 0, sizeof mk);
    for (y = 4; y < 13; y++) {
        for (k = 0; k < n && k < 4; k++) mk[y] |= (uint8_t)(0x40 >> (k * 2 - 0));
        if (n >= 5) mk[y] |= (uint8_t)(0x80 >> ((y - 4) * 7 / 8));
    }
    gen_glyph(slot, mk);
}

static uint8_t title_hold;       /* frames SELECT has been held (a new world at TITLE_HOLD) */
#define TITLE_HOLD 120

static void title_sprites(uint8_t have_save, uint16_t seed, uint8_t t)
{
    uint8_t i, n, x, pal_ui = is_cgb ? OPAL_UI : 0, h = title_hold;
    uint8_t flick = (uint8_t)((t >> 3) & 1), show = 1, tile = SPR_PIP_FULL, y = 16 + 104;
    /* the flame; while SELECT is held it gutters: faster flicker, sinking, then gaps, then out */
    if (h) {
        flick = (uint8_t)((t >> (h > 60 ? 1 : 2)) & 1);
        y = (uint8_t)(y + (h >> 5));
        if (h > 40 && ((t ^ (t >> 2)) & 3) < (uint8_t)(h >> 5)) show = 0;
        if (h > 90) tile = SPR_PIP_EMPTY;
        if (h >= TITLE_HOLD - 6) show = 0;
    }
    if (show) spr_set(0, 80 - 4 + 8, (uint8_t)(y - flick), tile, (uint8_t)(is_cgb ? OPAL_EMBER : S_PALETTE));
    else spr_hide(0);
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
        if (have_save) spr_set((uint8_t)(10 + i), (uint8_t)(80 - 22 + i * 12 + 8), (uint8_t)(16 + 124), (uint8_t)(TSPR_BASE + i * 2), pal_ui);
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
    uint8_t choice = 1;
    uint8_t t = 0, have = save_exists();
    uint16_t seed = 0;
    game_state = GS_BOOT;
    split_enable(0);
    anim_on = 0;
    hide_sprites_from(0);
    pal_fade = 16;
    pal_title();
    wait_frames(2);
    gfx_load_title();
    if (have) {
        /* peek at the saved seed / worlds for the sigil and tallies */
        if (save_load()) seed = world.seed;
    }
    title_sigil(seed);
    nx_scx = 0;
    nx_scy = 0;
    ambient_mode(AMB_TITLE);
    /* fade in */
    while (pal_fade) {
        pal_fade--;
        pal_title();
        title_sprites(have, seed, t++);
        wait_frames(3);
    }
    game_state = GS_TITLE;
    for (;;) {
        frame_sync();
        input();
        t++;
        if (have) {
            /* a saved world: A continues it; replacing it takes a deliberate, held SELECT */
            if (keys & J_SELECT) { if (title_hold < 255) title_hold++; }
            else title_hold = 0;
        }
        title_sprites(have, seed, t);
        frame_commit();
        if (have && title_hold >= TITLE_HOLD) { choice = 2; break; }
        if (have && (pressed & (J_A | J_START))) { choice = 1; break; }
        if (!have && (pressed & (J_A | J_START | J_SELECT))) { choice = 2; break; }
    }
    sfx_play(SFX_SELECT);
    title_hold = 0;
    /* fade the title out */
    while (pal_fade < 16) {
        pal_fade++;
        pal_title();
        wait_frames(2);
    }
    return choice;
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
        if (choice == 1 && save_load()) {
            pl_state = PL_SLEEP;
            if (items & (1 << IT_STONES)) stones = STONES_MAX;   /* older saves: the pouch is endless now */
            heart_revealed = (uint8_t)(beacons_lit == 7);
            ambient_seed(world.seed);
        } else {
            new_world(fresh_seed());
            save_write();
        }
        world_enter(1);
        ambient_mode(AMB_WAKE);
        wake_t = 0;
        world_run();
    }
}
