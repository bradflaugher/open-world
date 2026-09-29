/* gfx.c - OPEN WORLD bank-0 graphics: hardware setup, VBlank handler, split-screen latch,
 * VRAM queues, animated tiles, palettes (DMG shade stepping and CGB RGB lerp) and fades.
 *
 * Frame protocol (tear-free): the main loop builds OAM in oam[] and the next scroll values
 * in nx_*, then frame_commit() copies OAM into shadow_OAM (never straddling a VBlank) and sets
 * frame_ready. The first VBlank after that DMAs the OAM (GBDK's standard handler) and our
 * handler latches the scroll, so sprites and both scroll planes always change together. */
#include <gb/gb.h>
#include <gb/cgb.h>
#include <gb/isr.h>
#include <string.h>
#include "gfx.h"
#include "assets.h"
#include "sound.h"

uint8_t is_cgb;
volatile uint8_t vbl_frames;
uint16_t dbg_frame_drops;
uint16_t dbg_vbl_count;
uint8_t dbg_count_on;

volatile uint8_t split_mode;
uint8_t nx_band_scx, nx_land_scx, nx_land_scy, nx_band_bgp = 0xE4, nx_land_bgp = 0xE4;
uint8_t nx_obp0 = 0xD0, nx_obp1 = 0xE0, nx_scx, nx_scy;
volatile uint8_t band_scx, land_scx, land_scy, band_bgp = 0xE4, land_bgp = 0xE4;
static volatile uint8_t obp0_v = 0xD0, obp1_v = 0xE0, scx_v, scy_v;
static volatile uint8_t frame_ready;
static uint8_t last_vbl;
uint8_t oam[160];

/* queues (drained by isr.s) */
uint8_t bq[64 * 4];
volatile uint8_t bq_head, bq_tail;
uint8_t bq_budget = 8;
uint8_t vq[32 * 4];
volatile uint8_t vq_head, vq_tail;

uint8_t mt_t[MT_COUNT * 4];
uint8_t mt_a[MT_COUNT * 4];

/* animated tiles */
volatile uint8_t anim_on;
static uint8_t anim_ram[ANIM_COUNT][ANIM_FRAMES][16];
static uint8_t *anim_dst[ANIM_COUNT];

/* palettes */
uint8_t pal_phase_from = PH_DAY, pal_phase_to = PH_DAY, pal_t = 16;
uint8_t pal_fog, pal_fade, pal_flash, pal_band_bright;
static uint16_t cgb_bg[4][8][4];     /* RAM copies (ROM data may be banked later) */
static uint16_t cgb_obj[4][8][4];
static uint16_t pal_bg_buf[32], pal_obj_buf[32];
static volatile uint8_t pal_req;     /* bit0: BG, bit1: OBJ (CGB) */

void stat_isr(void);
void bq_drain(void);
void vq_drain(void);
ISR_VECTOR(VECTOR_STAT, stat_isr)

static void vbl_isr(void)
{
    uint8_t vbk = 0, i, *d;
    const uint8_t *s;
    if (frame_ready) {
        band_scx = nx_band_scx;
        land_scx = nx_land_scx;
        land_scy = nx_land_scy;
        band_bgp = nx_band_bgp;
        land_bgp = nx_land_bgp;
        obp0_v = nx_obp0;
        obp1_v = nx_obp1;
        scx_v = nx_scx;
        scy_v = nx_scy;
        frame_ready = 0;
    }
    if (split_mode) {
        LCDC_REG |= LCDCF_BG9C00;
        SCX_REG = band_scx;
        SCY_REG = 0;
        BGP_REG = band_bgp;
    } else {
        SCX_REG = scx_v;
        SCY_REG = scy_v;
        BGP_REG = land_bgp;
    }
    OBP0_REG = obp0_v;
    OBP1_REG = obp1_v;
    if (is_cgb) {
        vbk = VBK_REG & 1;
        VBK_REG = 0;
        if (pal_req & 1) {
            set_bkg_palette(0, 8, pal_bg_buf);
            pal_req &= 2;
        } else if (pal_req & 2) {
            set_sprite_palette(0, 8, pal_obj_buf);
            pal_req = 0;
        }
    }
    if (vq_head != vq_tail) vq_drain();
    if (bq_head != bq_tail) bq_drain();
    if (anim_on) {
        i = vbl_frames & (ANIM_PERIOD - 1);
        if (i < ANIM_COUNT) {
            s = anim_ram[i][(vbl_frames >> 4) & (ANIM_FRAMES - 1)];
            d = anim_dst[i];
            memcpy(d, s, 16);
        }
    }
    if (is_cgb) VBK_REG = vbk;
    vbl_frames++;
    if (dbg_count_on) dbg_vbl_count++;
    __asm__("ei");
    sound_tick();
}

void frame_sync(void)
{
    uint8_t now;
    while ((now = vbl_frames) == last_vbl) {
        __asm__("halt");
        __asm__("nop");
    }
    if (dbg_count_on && (uint8_t)(now - last_vbl) > 1) dbg_frame_drops += (uint8_t)(now - last_vbl - 1);
    last_vbl = now;
}

void frame_commit(void)
{
    uint8_t ly = LY_REG, f;
    if (ly >= 120 && ly < 144) {
        /* too close to the VBlank: let it pass (the frame is late anyway) */
        f = vbl_frames;
        while (vbl_frames == f) { __asm__("halt"); __asm__("nop"); }
        if (dbg_count_on) dbg_frame_drops++;
        last_vbl = vbl_frames;
    }
    memcpy((void *)shadow_OAM, oam, 160);
    frame_ready = 1;
}

void wait_frames(uint8_t n)
{
    while (n--) {
        frame_commit();
        frame_sync();
    }
}

void spr_set(uint8_t i, uint8_t x, uint8_t y, uint8_t tile, uint8_t prop)
{
    uint8_t *p = &oam[i << 2];
    p[0] = y;
    p[1] = x;
    p[2] = tile;
    p[3] = prop;
}

void spr_hide(uint8_t i)
{
    oam[i << 2] = 0;
}

void hide_sprites_from(uint8_t first)
{
    for (; first < NUM_SPR; first++) oam[first << 2] = 0;
}

void split_enable(uint8_t on)
{
    CRITICAL {
        if (on) {
            LYC_REG = BAND_LINES - 1;
            STAT_REG = STATF_LYC;
            IF_REG &= ~LCD_IFLAG;
            set_interrupts(VBL_IFLAG | LCD_IFLAG);
        } else {
            STAT_REG = 0;
            set_interrupts(VBL_IFLAG);
            LCDC_REG &= ~LCDCF_BG9C00;
        }
        split_mode = on;
    }
}

/* ---- VRAM queues ---- */
void bq_push(uint8_t col, uint8_t row, uint8_t mt)
{
    uint8_t h = bq_head, *p;
    uint16_t a = 0x9800u + ((uint16_t)row << 6) + ((uint16_t)col << 1);
    while ((uint8_t)((h + 1) & 63) == bq_tail) { __asm__("halt"); __asm__("nop"); }
    p = &bq[h << 2];
    p[0] = (uint8_t)a;
    p[1] = (uint8_t)(a >> 8);
    p[2] = mt;
    bq_head = (uint8_t)((h + 1) & 63);
}

uint8_t bq_pending(void)
{
    return (uint8_t)((bq_head - bq_tail) & 63);
}

void vq_push(uint16_t addr, uint8_t tile, uint8_t attr)
{
    uint8_t h = vq_head, *p;
    while ((uint8_t)((h + 1) & 31) == vq_tail) { __asm__("halt"); __asm__("nop"); }
    p = &vq[h << 2];
    p[0] = (uint8_t)addr;
    p[1] = (uint8_t)(addr >> 8);
    p[2] = tile;
    p[3] = attr;
    vq_head = (uint8_t)((h + 1) & 31);
}

uint8_t vq_pending(void)
{
    return (uint8_t)((vq_head - vq_tail) & 31);
}

void vram_flush(void)
{
    while (bq_head != bq_tail || vq_head != vq_tail) { __asm__("halt"); __asm__("nop"); }
}

/* ---- palettes ---- */
static uint8_t shade_lerp(uint8_t a, uint8_t b, uint8_t t)
{
    /* per 2-bit entry: a + (b - a) * t / 16, rounded */
    uint8_t out = 0, i, sa, sb, s;
    for (i = 0; i < 8; i += 2) {
        sa = (uint8_t)((a >> i) & 3);
        sb = (uint8_t)((b >> i) & 3);
        if (sb >= sa) s = (uint8_t)(sa + (uint8_t)(((uint8_t)(sb - sa) * t + 8) >> 4));
        else s = (uint8_t)(sa - (uint8_t)(((uint8_t)(sa - sb) * t + 8) >> 4));
        out |= (uint8_t)(s << i);
    }
    return out;
}

uint8_t shade_fade(uint8_t p, uint8_t f)
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
    int8_t ra, ga, ba, rb, gb, bb;
    if (!t) return a;
    if (t >= 16) return b;
    ra = (int8_t)(a & 31); ga = (int8_t)((a >> 5) & 31); ba = (int8_t)((a >> 10) & 31);
    rb = (int8_t)(b & 31); gb = (int8_t)((b >> 5) & 31); bb = (int8_t)((b >> 10) & 31);
    ra = (int8_t)(ra + (((int16_t)(rb - ra) * t) >> 4));
    ga = (int8_t)(ga + (((int16_t)(gb - ga) * t) >> 4));
    ba = (int8_t)(ba + (((int16_t)(bb - ba) * t) >> 4));
    return (uint16_t)((uint16_t)ra | ((uint16_t)ga << 5) | ((uint16_t)ba << 10));
}

#define FOG_COL 0x5EF7u

static void cgb_compute(uint16_t (*src)[8][4], uint16_t *dst, uint8_t band_pal)
{
    uint8_t p, c;
    uint16_t v;
    for (p = 0; p < 8; p++) {
        for (c = 0; c < 4; c++) {
            v = lerp555(src[pal_phase_from][p][c], src[pal_phase_to][p][c], pal_t);
            if (pal_flash) v = 0x7FFF;
            if (pal_fog) v = lerp555(v, FOG_COL, (uint8_t)(pal_fog + (pal_fog >> 1)));
            if (p == band_pal && pal_band_bright) v = lerp555(v, 0x7FFF, pal_band_bright);
            if (pal_fade) v = lerp555(v, 0x7FFF, pal_fade);
            *dst++ = v;
        }
    }
}

void pal_apply(void)
{
    uint8_t bgp = shade_lerp(dmg_bgp[pal_phase_from], dmg_bgp[pal_phase_to], pal_t);
    uint8_t o0 = shade_lerp(dmg_obp0[pal_phase_from], dmg_obp0[pal_phase_to], pal_t);
    uint8_t o1 = shade_lerp(dmg_obp1[pal_phase_from], dmg_obp1[pal_phase_to], pal_t);
    nx_land_bgp = shade_post(bgp, 0);
    nx_band_bgp = shade_post(bgp, 1);
    if (pal_flash) { o0 = 0; o1 = 0; }
    if (pal_fade) { o0 = shade_lerp(o0, 0, pal_fade); o1 = shade_lerp(o1, 0, pal_fade); }
    nx_obp0 = o0;
    nx_obp1 = o1;
    if (is_cgb) {
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

void pal_title(void)
{
    uint8_t p, c;
    if (!is_cgb) return;
    if (LCDC_REG & LCDCF_ON) while (pal_req) { __asm__("halt"); __asm__("nop"); }
    for (p = 0; p < 8; p++)
        for (c = 0; c < 4; c++)
            pal_bg_buf[(p << 2) + c] = pal_fade ? lerp555(title_pal[p][c], 0x7FFF, pal_fade) : title_pal[p][c];
    pal_req = 1;
    if (!(LCDC_REG & LCDCF_ON)) pal_upload_now();
}

void pal_upload_now(void)
{
    if (!is_cgb) return;
    if (pal_req & 1) set_bkg_palette(0, 8, pal_bg_buf);
    if (pal_req & 2) set_sprite_palette(0, 8, pal_obj_buf);
    pal_req = 0;
}

void fade_to(uint8_t target, uint8_t speed)
{
    while (pal_fade != target) {
        if (pal_fade < target) pal_fade = (uint8_t)(pal_fade + speed > target ? target : pal_fade + speed);
        else pal_fade = (uint8_t)(pal_fade < target + speed ? target : pal_fade - speed);
        pal_apply();
        wait_frames(2);
    }
}

/* ---- setup ---- */
void gfx_load_world_tiles(void)
{
    set_bkg_data(0, BG_TILE_COUNT, bg_tiles);
    set_sprite_data(0, SPR_TILE_COUNT, spr_tiles);
}

void gfx_init(void)
{
    uint8_t i, t;
    DISPLAY_OFF;
    VBK_REG = 0;
    is_cgb = (uint8_t)(_cpu == CGB_TYPE && (VBK_REG & 0xFE) == 0xFE);
    if (is_cgb) cpu_fast();
    memcpy(mt_t, mt_tiles, sizeof mt_t);
    memcpy(mt_a, mt_attr, sizeof mt_a);
    memcpy(anim_ram, anim_frames, sizeof anim_ram);
    memcpy(cgb_bg, cgb_bg_pal, sizeof cgb_bg);
    memcpy(cgb_obj, cgb_obj_pal, sizeof cgb_obj);
    for (i = 0; i < ANIM_COUNT; i++) {
        t = anim_tile[i];
        anim_dst[i] = (uint8_t *)(t < 128 ? 0x9000u + ((uint16_t)t << 4) : 0x8800u + ((uint16_t)(t - 128) << 4));
    }
    memset(oam, 0, sizeof oam);
    memset((void *)shadow_OAM, 0, 160);
    LCDC_REG = LCDCF_OFF | LCDCF_BG8800 | LCDCF_OBJ16 | LCDCF_OBJON | LCDCF_BGON | LCDCF_WINOFF;
    SCX_REG = 0;
    SCY_REG = 0;
    BGP_REG = 0x00;
    OBP0_REG = 0x00;
    OBP1_REG = 0x00;
    CRITICAL {
        add_VBL(vbl_isr);
        STAT_REG = 0;
    }
    set_interrupts(VBL_IFLAG);
}
