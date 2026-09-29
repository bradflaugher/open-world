/* band.c - OPEN WORLD horizon band: a 256 px panorama (= 360 degrees, 1 px per bearing unit)
 * on rows 0-2 of BG map 0x9C00. Its centre follows the wanderer's facing, easing round.
 * The skyline comes from low-frequency noise of (seed, angle); markers are sprites placed at
 * world_bearing() of the beacons, the Heart and the nearest cairn. */
#pragma bank 255
#include <gb/gb.h>
#include <string.h>
#include "game.h"
#include "gfx.h"
#include "assets.h"
#include "sound.h"

#define NSTARS 12
static uint8_t band_row[3][32];
static uint8_t star_col[NSTARS], star_row[NSTARS], star_tile[NSTARS];
static uint8_t nstars;
static uint16_t band_ang;           /* 8.8 bearing at the centre of the screen */
static uint8_t mark_b[5];           /* cached bearings */
static uint8_t mark_rr;
static uint8_t cairn_near = 0xFF;
uint8_t band_mark_x[5];
static uint8_t mark_last_a, mark_last_state = 0xFF;

static uint16_t brng;
static uint8_t brnd(void)
{
    uint16_t x = brng;
    x ^= (uint16_t)(x << 7);
    x ^= (uint16_t)(x >> 9);
    x ^= (uint16_t)(x << 8);
    brng = x;
    return (uint8_t)x;
}

void band_build(void) BANKED
{
    uint8_t P[9], T[32], c, h, k, t, up, lo, i, r;
    int8_t d;
    uint8_t attr[32];
    brng = (uint16_t)(world.seed * 7u + 0x5EEDu);
    if (!brng) brng = 1;
    brnd(); brnd();
    /* 8 coarse control points round the circle, heights 3..15, with one big massif */
    for (i = 0; i < 8; i++) P[i] = (uint8_t)(3 + (brnd() & 7));
    P[brnd() & 7] = (uint8_t)(12 + (brnd() & 3));
    P[8] = P[0];
    for (c = 0; c < 32; c++) {
        i = (uint8_t)(c >> 2);
        k = (uint8_t)(c & 3);
        t = (uint8_t)((P[i] * (4 - k) + P[i + 1] * k) >> 2);
        t = (uint8_t)(t + (brnd() & 3) - 1);
        if (t < 1) t = 1;
        if (t > 16) t = 16;
        T[c] = t;
    }
    h = T[0];
    for (c = 0; c < 32; c++) {
        d = (int8_t)(T[c] - h);
        if (d >= 3 && h >= 1 && h <= 7) {            /* rise to the top of the lower row */
            up = BAND_SKY; lo = (uint8_t)(BAND_SLOPE_UP0 + h - 1); h = 8;
        } else if (d >= 3 && h >= 9 && h <= 15) {    /* rise to the top of the upper row */
            up = (uint8_t)(BAND_SLOPE_UP0 + h - 9); lo = BAND_LAND; h = 16;
        } else if (d <= -3 && h == 8) {
            k = (uint8_t)(T[c] < 1 ? 1 : T[c]);
            up = BAND_SKY; lo = (uint8_t)(BAND_SLOPE_DN0 + k - 1); h = k;
        } else if (d <= -3 && h == 16) {
            k = (uint8_t)(T[c] < 9 ? 9 : T[c]);
            up = (uint8_t)(BAND_SLOPE_DN0 + k - 9); lo = BAND_LAND; h = k;
        } else {
            if (d > 2) d = 2;
            if (d < -2) d = -2;
            h = (uint8_t)(h + d);
            if (h < 1) h = 1;
            if (h > 16) h = 16;
            up = h > 8 ? (uint8_t)(BAND_RIDGE0 + h - 9) : BAND_SKY;
            lo = h >= 8 ? BAND_LAND : (uint8_t)(BAND_RIDGE0 + h - 1);
        }
        band_row[0][c] = BAND_SKY_TOP;
        band_row[1][c] = up;
        band_row[2][c] = lo;
    }
    /* stars: in open sky only */
    nstars = 0;
    for (i = 0; i < 40 && nstars < NSTARS; i++) {
        c = (uint8_t)(brnd() & 31);
        r = (uint8_t)((brnd() & 3) ? 0 : 1);
        if (band_row[r][c] != BAND_SKY && band_row[r][c] != BAND_SKY_TOP) continue;
        for (k = 0; k < nstars; k++) if (star_col[k] == c && star_row[k] == r) break;
        if (k < nstars) continue;
        star_col[nstars] = c;
        star_row[nstars] = r;
        star_tile[nstars] = (uint8_t)(BAND_STAR0 + (brnd() & 1));
        nstars++;
    }
    set_tiles(0, 0, 32, 3, (uint8_t *)0x9C00, &band_row[0][0]);
    if (is_cgb) {
        memset(attr, PAL_SKY, 32);
        VBK_REG = 1;
        for (r = 0; r < 3; r++) set_tiles(0, r, 32, 1, (uint8_t *)0x9C00, attr);
        VBK_REG = 0;
    }
    cairn_near = 0xFF;
    memset(mark_b, 0, sizeof mark_b);
    for (i = 0; i < 5; i++) band_mark_x[i] = 0xFF;
}

void band_stars(uint8_t on) BANKED
{
    uint8_t i, t;
    for (i = 0; i < nstars; i++) {
        t = on ? star_tile[i] : band_row[star_row[i]][star_col[i]];
        vq_push((uint16_t)(0x9C00u + ((uint16_t)star_row[i] << 5) + star_col[i]), t, PAL_SKY);
    }
}

static uint8_t face_bearing(void)
{
    return (uint8_t)(pl_face << 5);
}

void band_reset_angle(void) BANKED
{
    mark_last_state = 0xFF;
    band_ang = (uint16_t)face_bearing() << 8;
    mark_rr = 0;
    for (mark_rr = 0; mark_rr < 5; mark_rr++) {
        if (mark_rr < 3) mark_b[mark_rr] = world_bearing(pl_mx, pl_my, world.beacon[mark_rr].x, world.beacon[mark_rr].y);
    }
    mark_b[3] = world_bearing(pl_mx, pl_my, world.heart.x, world.heart.y);
    mark_rr = 0;
}

static void nearest_cairn(void)
{
    uint8_t i;
    uint16_t best = 0xFFFF, d;
    cairn_near = 0xFF;
    for (i = 0; i < cairn_n; i++) {
        d = world_dist(pl_mx, pl_my, cairns[i].x, cairns[i].y);
        if (d < best && d > 1) { best = d; cairn_near = i; }
    }
}

void band_update(void) BANKED
{
    uint16_t tgt = (uint16_t)face_bearing() << 8;
    int16_t d = (int16_t)(tgt - band_ang), s;
    uint8_t i, a, x, tile, pal, show;
    /* ease towards the facing direction (shortest way round) */
    if (pl_state != PL_SLEEP) {
        s = (int16_t)(d >> 4);
        if (!s && d) s = d > 0 ? 1 : -1;
        if (s > 0x180) s = 0x180;
        if (s < -0x180) s = -0x180;
        band_ang = (uint16_t)(band_ang + s);
    }
    a = (uint8_t)(band_ang >> 8);
    nx_band_scx = (uint8_t)(a - 80);

    /* one bearing refreshed every 8 frames (world_bearing is a banked CORDIC) */
    if ((vbl_frames & 7) == 0) switch (mark_rr) {
    case 0: case 1: case 2:
        mark_b[mark_rr] = world_bearing(pl_mx, pl_my, world.beacon[mark_rr].x, world.beacon[mark_rr].y);
        break;
    case 3:
        mark_b[3] = world_bearing(pl_mx, pl_my, world.heart.x, world.heart.y);
        break;
    case 4:
        nearest_cairn();
        if (cairn_near != 0xFF)
            mark_b[4] = world_bearing(pl_mx, pl_my, cairns[cairn_near].x, cairns[cairn_near].y);
        break;
    }
    if ((vbl_frames & 7) == 0 && ++mark_rr >= 5) mark_rr = 0;

    /* markers move only when the band turns or a bearing / state changes */
    i = (uint8_t)(beacons_lit | (heart_revealed << 3) | ((cairn_near != 0xFF) << 4));
    if (a == mark_last_a && i == mark_last_state && (vbl_frames & 7) != 1) return;
    mark_last_a = a;
    mark_last_state = i;
    for (i = 0; i < 5; i++) {
        show = 1;
        if (i < 3) {
            if (beacons_lit & (1 << i)) { tile = SPR_BAND_BEACON_LIT; pal = OPAL_LIGHT | S_PALETTE; }
            else { tile = SPR_BAND_BEACON; pal = OPAL_BAND; }
        } else if (i == 3) {
            tile = SPR_BAND_HEART; pal = OPAL_LIGHT | S_PALETTE;
            show = heart_revealed;
        } else {
            tile = SPR_BAND_CAIRN; pal = OPAL_BAND;
            show = (uint8_t)(cairn_near != 0xFF);
        }
        x = (uint8_t)(mark_b[i] - a + 80);
        if (!show || x < 4 || x > 156) {
            spr_hide((uint8_t)(SP_MARK + i));
            band_mark_x[i] = 0xFF;
            continue;
        }
        band_mark_x[i] = x;
        if (!is_cgb) pal &= S_PALETTE;
        spr_set((uint8_t)(SP_MARK + i), (uint8_t)(x + 8 - 4), (uint8_t)(16 + 8), tile, pal);
    }
}
