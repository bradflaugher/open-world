/* fx.c - OPEN WORLD sprite effects: the night lantern glow (hardware trick #2: a 32x32
 * dithered disc of 8 OBJ sprites round the wanderer), rain / snow with parallax, fog and
 * lightning, and the HUD (warmth pips + equipped item) inside the band. */
#pragma bank 255
#include <gb/gb.h>
#include <string.h>
#include "game.h"
#include "gfx.h"
#include "assets.h"
#include "sound.h"

#define CX 80           /* wanderer centre on screen */
#define CY 85

static uint8_t wx_y, wx_x[NUM_WX], wx_depth[NUM_WX];
static uint8_t wx_ph;
static uint16_t last_cam_px, last_cam_py;
static uint8_t lightning_t, thunder_t;
static uint16_t storm_rng = 0x1234;

static uint8_t srand8(void)
{
    uint16_t x = storm_rng;
    x ^= (uint16_t)(x << 7);
    x ^= (uint16_t)(x >> 9);
    x ^= (uint16_t)(x << 8);
    storm_rng = x;
    return (uint8_t)x;
}

static const int8_t sway[16] = { 0, 1, 1, 2, 2, 2, 1, 1, 0, -1, -1, -2, -2, -2, -1, -1 };

static uint8_t wx_rolled = 0xFF;
static int8_t wx_skip_dx, wx_skip_dy;
static uint8_t glow_last = 0xFF, glow_last_x, wx_hidden, hud_key[6];

static void glow_draw(void)
{
    uint8_t pal = is_cgb ? OPAL_GLOW : S_PALETTE;
    uint8_t x = (uint8_t)(CX + 8 + shake_x), y = (uint8_t)(CY + 16);
    if (glow_on == glow_last && x == glow_last_x) return;
    glow_last = glow_on;
    glow_last_x = x;
    if (!glow_on) {
        uint8_t i;
        for (i = 0; i < 8; i++) spr_hide((uint8_t)(SP_GLOW + i));
        return;
    }
    spr_set(SP_GLOW + 0, (uint8_t)(x - 16), (uint8_t)(y - 16), SPR_GLOW0, pal);
    spr_set(SP_GLOW + 1, (uint8_t)(x - 8), (uint8_t)(y - 16), SPR_GLOW0 + 2, pal);
    spr_set(SP_GLOW + 2, x, (uint8_t)(y - 16), SPR_GLOW0 + 2, (uint8_t)(pal | S_FLIPX));
    spr_set(SP_GLOW + 3, (uint8_t)(x + 8), (uint8_t)(y - 16), SPR_GLOW0, (uint8_t)(pal | S_FLIPX));
    spr_set(SP_GLOW + 4, (uint8_t)(x - 16), y, SPR_GLOW0, (uint8_t)(pal | S_FLIPY));
    spr_set(SP_GLOW + 5, (uint8_t)(x - 8), y, SPR_GLOW0 + 2, (uint8_t)(pal | S_FLIPY));
    spr_set(SP_GLOW + 6, x, y, SPR_GLOW0 + 2, (uint8_t)(pal | S_FLIPX | S_FLIPY));
    spr_set(SP_GLOW + 7, (uint8_t)(x + 8), y, SPR_GLOW0, (uint8_t)(pal | S_FLIPX | S_FLIPY));
}

void fx_weather_roll(void) BANKED
{
    uint8_t i;
    for (i = 0; i < NUM_WX; i++) {
        { uint8_t r = srand8(); if (r >= 168) r = (uint8_t)(r - 88); wx_x[i] = r; }
        wx_depth[i] = (uint8_t)(i & 3 ? (i & 1) + 1 : 0);
    }
}

static void weather_draw(void)
{
    uint8_t i, top, x, tile, pal, fall, hide;
    int8_t dcx, dcy;
    uint16_t px = (uint16_t)((cam_mx << 4) | cam_sx), py = (uint16_t)((cam_my << 4) | cam_sy);
    dcx = (int8_t)(px - last_cam_px);
    dcy = (int8_t)(py - last_cam_py);
    last_cam_px = px;
    last_cam_py = py;
    if (weather != WX_RAIN && weather != WX_STORM && weather != WX_SNOW) {
        if (!wx_hidden) for (i = 0; i < NUM_WX; i++) spr_hide((uint8_t)(SP_WX + i));
        wx_hidden = 1;
        return;
    }
    wx_hidden = 0;
    /* DMG: the drops move every other frame, twice as far (half the cost) */
    if (!is_cgb) {
        if (vbl_frames & 1) { wx_skip_dx += dcx; wx_skip_dy += dcy; return; }
        dcx = (int8_t)(dcx + wx_skip_dx); dcy = (int8_t)(dcy + wx_skip_dy);
        wx_skip_dx = wx_skip_dy = 0;
    }
    if (wx_rolled != weather) { wx_rolled = weather; fx_weather_roll(); }
    if (weather == WX_SNOW) { tile = SPR_SNOW; fall = (uint8_t)(!is_cgb || (vbl_frames & 1) ? 1 : 0); }
    else { tile = SPR_RAIN; fall = weather == WX_STORM ? 5 : 4; if (!is_cgb) fall <<= 1; }
    pal = is_cgb ? OPAL_WEATHER : S_PALETTE;
    /* the world moves under the weather (parallax: nearer layers move more) */
    wx_y = (uint8_t)(wx_y + fall - dcy);
    while (wx_y >= 120) wx_y = (uint8_t)(wx_y + (wx_y >= 200 ? 120 : -120));
    wx_ph++;
    for (i = 0; i < NUM_WX; i++) {
        uint8_t d = wx_depth[i];
        int8_t mv = (int8_t)(-(dcx) - (d ? (dcx >> (3 - d)) : 0));
        if (weather != WX_SNOW) mv = (int8_t)(mv - ((1 + (d >> 1)) << (is_cgb ? 0 : 1)));   /* wind */
        wx_x[i] = (uint8_t)(wx_x[i] + mv);
        if (wx_x[i] >= 168) wx_x[i] = (uint8_t)(wx_x[i] + (wx_x[i] >= 212 ? 168 : -168));
        top = (uint8_t)(wx_y + i * 9);
        if (top >= 120) top -= 120;
        top = (uint8_t)(top + 24);
        x = wx_x[i];
        if (weather == WX_SNOW) x = (uint8_t)(x + sway[(uint8_t)((wx_ph >> 3) + i * 5) & 15]);
        hide = 0;
        /* keep lines under the glow within the 10-sprites-per-line budget */
        if (glow_on && top + 16 > CY - 16 && top < CY + 16) hide = 1;
        if (hide) spr_hide((uint8_t)(SP_WX + i));
        else spr_set((uint8_t)(SP_WX + i), x, (uint8_t)(top + 16), tile, pal);
    }
}

static void storm_tick(void)
{
    if (thunder_t) {
        if (--thunder_t == 0) sfx_play(SFX_THUNDER);
    }
    if (lightning_t) {
        lightning_t--;
        {
            uint8_t fl = (uint8_t)(lightning_t == 6 || lightning_t == 5 || lightning_t == 2);
            if (fl != pal_flash) { pal_flash = fl; pal_apply(); }
        }
        return;
    }
    if (weather == WX_STORM && game_state == GS_WORLD && srand8() == 7 && (srand8() & 3) == 0) {
        lightning_t = 7;
        thunder_t = (uint8_t)(20 + (srand8() & 31));
    }
}

void hud_update(void) BANKED
{
    uint8_t i, full = (uint8_t)((warmth + 128) >> 8), tile, y, pal = is_cgb ? OPAL_EMBER : S_PALETTE, blink, show = 1;
    blink = (uint8_t)(phase == PH_NIGHT && full <= 1 && (vbl_frames & 32));
    switch (equipped) {
    case IT_STONES: tile = SPR_ICON_STONES; break;
    case IT_CLOAK: tile = SPR_ICON_CLOAK; break;
    default: tile = SPR_ICON_LANTERN; break;
    }
    y = 16 + 1;
    if (item_pulse) {
        item_pulse--;
        y = (uint8_t)(y - ((item_pulse >> 2) & 1));
        if ((item_pulse & 8) && item_pulse > 60) show = 0;
    }
    if (equipped == IT_STONES && !stones && (vbl_frames & 32)) show = 0;
    /* only touch OAM when something changed */
    if (hud_key[0] == full && hud_key[1] == blink && hud_key[2] == tile && hud_key[3] == y &&
        hud_key[4] == show && hud_key[5] == 1) return;
    hud_key[0] = full; hud_key[1] = blink; hud_key[2] = tile; hud_key[3] = y; hud_key[4] = show; hud_key[5] = 1;
    for (i = 0; i < 4; i++) {
        uint8_t t = i < full ? SPR_PIP_FULL : SPR_PIP_EMPTY;
        if (blink && i + 1 == full) t = SPR_PIP_EMPTY;
        spr_set((uint8_t)(SP_PIPS + i), (uint8_t)(8 + 4 + i * 9), 16 + 1, t, pal);
    }
    if (show) spr_set(SP_ICON, (uint8_t)(160 - 12 + 8), y, tile, (uint8_t)(is_cgb ? OPAL_UI : 0));
    else spr_hide(SP_ICON);
}

void fx_redraw(void) BANKED
{
    glow_last = 0xFF;
    wx_hidden = 0;
    hud_key[5] = 0;
}

void fx_update(void) BANKED
{
    glow_draw();
    if (FRAME_LATE()) return;       /* the rest can wait a frame */
    weather_draw();
    storm_tick();
    /* fog rolls in and out slowly */
    if ((vbl_frames & 7) == 0) {
        uint8_t f = pal_fog;
        if (weather == WX_FOG && f < 8) f++;
        else if (weather != WX_FOG && f) f--;
        if (f != pal_fog) { pal_fog = f; pal_apply(); }
    }
    hud_update();
}
