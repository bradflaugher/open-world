/* pal.c - OPEN WORLD palettes: DMG shade stepping and CGB RGB555 lerps between the four
 * phases, fog, lightning, fades and the ending's bright band. Computed here (banked) into
 * RAM buffers; gfx.c uploads them in VBlank. */
#pragma bank 255
#include <gb/gb.h>
#include <gb/cgb.h>
#include <string.h>
#include "gfx.h"
#include "assets.h"
#include "sound.h"

extern uint8_t dmg_bg_r[4], dmg_o0_r[4], dmg_o1_r[4];
extern uint16_t cgb_bg[4][8][4], cgb_obj[4][8][4], title_pal_r[32];
extern uint16_t pal_bg_buf[32], pal_obj_buf[32];
extern volatile uint8_t pal_req, obp0_v, obp1_v;

/* (d * t) >> 4 for d in -31..31, t in 0..16, without a multiply call */
static int8_t mul_t(int8_t d, uint8_t t)
{
    int16_t r = 0, v = d;
    if (t & 1) r += v;
    if (t & 2) r += (int16_t)(v << 1);
    if (t & 4) r += (int16_t)(v << 2);
    if (t & 8) r += (int16_t)(v << 3);
    if (t & 16) r += (int16_t)(v << 4);
    return (int8_t)(r >> 4);
}

static uint8_t shade_lerp(uint8_t a, uint8_t b, uint8_t t)
{
    /* per 2-bit entry: a + (b - a) * t / 16, rounded */
    uint8_t out = 0, i, sa, sb, s;
    for (i = 0; i < 8; i += 2) {
        sa = (uint8_t)((a >> i) & 3);
        sb = (uint8_t)((b >> i) & 3);
        if (sb >= sa) s = (uint8_t)(sa + (uint8_t)((uint8_t)(mul_t((int8_t)((sb - sa) << 2), t) + 2) >> 2));
        else s = (uint8_t)(sa - (uint8_t)((uint8_t)(mul_t((int8_t)((sa - sb) << 2), t) + 2) >> 2));
        out |= (uint8_t)(s << i);
    }
    return out;
}

uint8_t shade_fade(uint8_t p, uint8_t f) BANKED
{
    return shade_lerp(p, 0x00, f);
}

static const uint8_t fog_tab[4] = { 0, 1, 1, 2 };   /* contrast collapses in fog */

static uint8_t shade_fog(uint8_t p)
{
    uint8_t out = 0, i;
    for (i = 0; i < 8; i += 2) out |= (uint8_t)(fog_tab[(p >> i) & 3] << i);
    return out;
}

static uint8_t shade_post(uint8_t p, uint8_t band)
{
    if (pal_flash) return 0x00;
    if (pal_fog) p = shade_lerp(p, shade_fog(p), (uint8_t)(pal_fog << 1));
    if (band && pal_band_bright) p = shade_lerp(p, 0x00, pal_band_bright);
    if (pal_fade) p = shade_lerp(p, 0x00, pal_fade);
    return p;
}

static uint16_t lerp555(uint16_t a, uint16_t b, uint8_t t)
{
    uint8_t ra, ga, ba;
    if (!t || a == b) return a;
    if (t >= 16) return b;
    ra = (uint8_t)(a & 31); ga = (uint8_t)((a >> 5) & 31); ba = (uint8_t)((a >> 10) & 31);
    ra = (uint8_t)(ra + mul_t((int8_t)((uint8_t)(b & 31) - ra), t));
    ga = (uint8_t)(ga + mul_t((int8_t)((uint8_t)((b >> 5) & 31) - ga), t));
    ba = (uint8_t)(ba + mul_t((int8_t)((uint8_t)((b >> 10) & 31) - ba), t));
    return (uint16_t)((uint16_t)ra | ((uint16_t)ga << 5) | ((uint16_t)ba << 10));
}

#define FOG_COL 0x5EF7u

/* colour k of 64 (0-31 BG, 32-63 OBJ) for the current phase blend, fog, band and fade */
static uint16_t cgb_colour(uint8_t k)
{
    uint8_t p = (uint8_t)((k >> 2) & 7), c = (uint8_t)(k & 3);
    uint16_t (*src)[8][4] = k < 32 ? cgb_bg : cgb_obj;
    uint16_t v = lerp555(src[pal_phase_from][p][c], src[pal_phase_to][p][c], pal_t);
    if (pal_fog) v = lerp555(v, FOG_COL, (uint8_t)(pal_fog + (pal_fog >> 1)));
    if (k < 32 && p == PAL_SKY && pal_band_bright) v = lerp555(v, 0x7FFF, pal_band_bright);
    if (pal_fade) v = lerp555(v, 0x7FFF, pal_fade);
    return v;
}

static void cgb_compute(uint16_t (*src)[8][4], uint16_t *dst, uint8_t band_pal)
{
    uint8_t k, base = (uint8_t)(src == cgb_bg ? 0 : 32);
    (void)band_pal;
    for (k = 0; k < 32; k++) dst[k] = cgb_colour((uint8_t)(base + k));
}

/* During play (from the VBL ISR) the CGB palettes are recomputed incrementally, 8 colours a
 * frame, into a staging copy that is published whole: a full lerp is ~36k cycles. */
static uint16_t pal_stage[64];
static uint8_t pal_job = 0xFF;
uint8_t pal_pending;

void pal_tick(void) BANKED
{
    uint8_t n;
    if (pal_job == 0xFF) return;
    for (n = 0; n < 8 && pal_job < 64; n++, pal_job++) pal_stage[pal_job] = cgb_colour(pal_job);
    if (pal_job < 64 || pal_req) return;
    memcpy(pal_bg_buf, pal_stage, 64);
    memcpy(pal_obj_buf, &pal_stage[32], 64);
    pal_req = 3;
    pal_job = 0xFF;
}

void pal_apply(void) BANKED
{
    uint8_t bgp = shade_lerp(dmg_bg_r[pal_phase_from], dmg_bg_r[pal_phase_to], pal_t);
    uint8_t o0 = shade_lerp(dmg_o0_r[pal_phase_from], dmg_o0_r[pal_phase_to], pal_t);
    uint8_t o1 = shade_lerp(dmg_o1_r[pal_phase_from], dmg_o1_r[pal_phase_to], pal_t);
    nx_land_bgp = shade_post(bgp, 0);
    nx_band_bgp = shade_post(bgp, 1);
    if (pal_flash) { o0 = 0; o1 = 0; }
    if (pal_fade) { o0 = shade_lerp(o0, 0, pal_fade); o1 = shade_lerp(o1, 0, pal_fade); }
    nx_obp0 = o0;
    nx_obp1 = o1;
    if (is_cgb) {
        if (hook_busy && (LCDC_REG & LCDCF_ON)) {
            pal_job = 0;            /* restart the incremental job */
            return;
        }
        pal_job = 0xFF;
        if (LCDC_REG & LCDCF_ON) while (pal_req) { __asm__("halt"); __asm__("nop"); }
        cgb_compute(cgb_bg, pal_bg_buf, PAL_SKY);
        cgb_compute(cgb_obj, pal_obj_buf, 0xFF);
        pal_req = 3;
        if (!(LCDC_REG & LCDCF_ON)) pal_upload_now();
    }
    if (!(LCDC_REG & LCDCF_ON)) {
        land_bgp = band_bgp = BGP_REG = nx_land_bgp;
        obp0_v = OBP0_REG = nx_obp0;
        obp1_v = OBP1_REG = nx_obp1;
    }
}

void pal_title(void) BANKED
{
    uint8_t p;
    nx_land_bgp = shade_lerp(0xE4, 0x00, pal_fade);
    nx_obp0 = shade_lerp(dmg_o0_r[PH_DAY], 0x00, pal_fade);
    nx_obp1 = shade_lerp(dmg_o1_r[PH_DAY], 0x00, pal_fade);
    if (!is_cgb) return;
    if (LCDC_REG & LCDCF_ON) while (pal_req) { __asm__("halt"); __asm__("nop"); }
    for (p = 0; p < 32; p++) pal_bg_buf[p] = lerp555(title_pal_r[p], 0x7FFF, pal_fade);
    cgb_compute(cgb_obj, pal_obj_buf, 0xFF);
    pal_req = 3;
    if (!(LCDC_REG & LCDCF_ON)) pal_upload_now();
}

/* one 4-colour palette on every BG slot (the map's paper), towards white by f */
void pal_paper(const uint16_t *c4, uint8_t f) BANKED
{
    uint8_t p;
    nx_land_bgp = shade_lerp(0xE4, 0x00, f);
    nx_obp0 = shade_lerp(dmg_o0_r[PH_DAY], 0x00, f);
    nx_obp1 = shade_lerp(dmg_o1_r[PH_DAY], 0x00, f);
    if (!is_cgb) return;
    if (LCDC_REG & LCDCF_ON) while (pal_req) { __asm__("halt"); __asm__("nop"); }
    for (p = 0; p < 32; p++) pal_bg_buf[p] = lerp555(c4[p & 3], 0x7FFF, f);
    for (p = 0; p < 32; p++) pal_obj_buf[p] = lerp555(cgb_obj[PH_DAY][p >> 2][p & 3], 0x7FFF, f);
    pal_req = 3;
}

void fade_to(uint8_t target, uint8_t speed) BANKED
{
    while (pal_fade != target) {
        if (pal_fade < target) pal_fade = (uint8_t)(pal_fade + speed > target ? target : pal_fade + speed);
        else pal_fade = (uint8_t)(pal_fade < target + speed ? target : pal_fade - speed);
        pal_apply();
        wait_frames(2);
    }
}

