/* gfx.c - OPEN WORLD bank-0 graphics: hardware setup, VBlank handler, split-screen latch,
 * VRAM queues, animated tiles, palettes (DMG shade stepping and CGB RGB lerp) and fades.
 *
 * Frame protocol (tear-free): the frame writes sprites straight into shadow OAM early in the
 * frame (right after a VBlank) and the next scroll values in nx_*, then frame_commit() sets
 * frame_ready. The first VBlank after that DMAs the OAM (GBDK's standard handler) and our
 * handler latches the scroll, so sprites and both scroll planes always change together. */
#include <gb/gb.h>
#include <gb/cgb.h>
#include <gb/isr.h>
#include <string.h>
#include "gfx.h"
#include "assets.h"
#include "sound.h"
#include "game.h"

uint8_t is_cgb;
volatile uint8_t vbl_frames;
uint16_t dbg_frame_drops;
uint16_t dbg_vbl_count;
uint8_t dbg_count_on;
uint8_t dbg_vbl_ly[2];

volatile uint8_t split_mode;
uint8_t nx_band_scx, nx_land_scx, nx_land_scy, nx_band_bgp = 0xE4, nx_land_bgp = 0xE4;
uint8_t nx_obp0 = 0xD0, nx_obp1 = 0xE0, nx_scx, nx_scy;
volatile uint8_t band_scx, land_scx, land_scy, band_bgp = 0xE4, land_bgp = 0xE4;
volatile uint8_t obp0_v = 0xD0, obp1_v = 0xE0, scx_v, scy_v;
static volatile uint8_t frame_ready;
static uint8_t last_vbl;

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
volatile uint8_t hook_on, hook_busy;
static uint8_t anim_ram[ANIM_COUNT][ANIM_FRAMES][16];
static uint8_t *anim_dst[ANIM_COUNT];

/* palettes */
uint8_t pal_phase_from = PH_DAY, pal_phase_to = PH_DAY, pal_t = 16;
uint8_t pal_fog, pal_fade, pal_flash, pal_band_bright;
#define ASSETS_IN()  uint8_t _ab = CURRENT_BANK; SWITCH_ROM(BANK(assets))
#define ASSETS_OUT() SWITCH_ROM(_ab)

uint8_t dmg_bg_r[4], dmg_o0_r[4], dmg_o1_r[4];
uint16_t cgb_bg[4][8][4];            /* RAM copies: the art lives in a switchable bank */
uint16_t cgb_obj[4][8][4];
uint16_t title_pal_r[32];
uint16_t pal_bg_buf[32], pal_obj_buf[32];
volatile uint8_t pal_req;            /* bit0: BG, bit1: OBJ (CGB) */
static uint8_t flash_hw;
static const uint16_t white_pal[32] = {
    0x7FFF,0x7FFF,0x7FFF,0x7FFF, 0x7FFF,0x7FFF,0x7FFF,0x7FFF, 0x7FFF,0x7FFF,0x7FFF,0x7FFF, 0x7FFF,0x7FFF,0x7FFF,0x7FFF,
    0x7FFF,0x7FFF,0x7FFF,0x7FFF, 0x7FFF,0x7FFF,0x7FFF,0x7FFF, 0x7FFF,0x7FFF,0x7FFF,0x7FFF, 0x7FFF,0x7FFF,0x7FFF,0x7FFF
};

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
        if (pal_flash != flash_hw) {
            /* lightning: all BG light for a frame, then the real palettes again */
            flash_hw = pal_flash;
            if (pal_flash) set_bkg_palette(0, 8, white_pal);
            else pal_req |= 1;
        } else if (pal_flash) {
        } else if (pal_req & 1) {
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
    { uint8_t l = LY_REG; l = (uint8_t)(l >= 144 ? l - 144 : l + 10); if (l > dbg_vbl_ly[0]) dbg_vbl_ly[0] = l; }
    __asm__("ei");
    { uint8_t v0 = vbl_frames, l;
      sound_tick();
      l = LY_REG; l = (uint8_t)(l >= 144 ? l - 144 : l + 10);
      if (v0 != vbl_frames) l = 255;
      if (l > dbg_vbl_ly[1]) dbg_vbl_ly[1] = l; }
    /* the game frame runs here, so a slow world_mt in the main loop never costs a frame */
    if (hook_on) {
        if (hook_busy) {
            if (dbg_count_on) dbg_frame_drops++;
        } else {
            hook_busy = 1;
            world_frame();
            hook_busy = 0;
        }
    }
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
    frame_ready = 1;
}

void wait_frames(uint8_t n)
{
    while (n--) {
        frame_commit();
        frame_sync();
    }
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
    uint8_t h, *p;
    uint16_t a = 0x9800u + ((uint16_t)row << 6) + ((uint16_t)col << 1);
    for (;;) {
        __critical {
            h = bq_head;
            if ((uint8_t)((h + 1) & 63) != bq_tail) {
                p = &bq[h << 2];
                p[0] = (uint8_t)a;
                p[1] = (uint8_t)(a >> 8);
                p[2] = mt;
                bq_head = (uint8_t)((h + 1) & 63);
                h = 0xFF;
            }
        }
        if (h == 0xFF) return;
        __asm__("halt");
        __asm__("nop");
    }
}

uint8_t bq_pending(void)
{
    return (uint8_t)((bq_head - bq_tail) & 63);
}

void vq_push(uint16_t addr, uint8_t tile, uint8_t attr)
{
    uint8_t h, *p;
    for (;;) {
        __critical {
            h = vq_head;
            if ((uint8_t)((h + 1) & 31) != vq_tail) {
                p = &vq[h << 2];
                p[0] = (uint8_t)addr;
                p[1] = (uint8_t)(addr >> 8);
                p[2] = tile;
                p[3] = attr;
                vq_head = (uint8_t)((h + 1) & 31);
                h = 0xFF;
            }
        }
        if (h == 0xFF) return;
        __asm__("halt");
        __asm__("nop");
    }
}

uint8_t vq_pending(void)
{
    return (uint8_t)((vq_head - vq_tail) & 31);
}

void vram_flush(void)
{
    while (bq_head != bq_tail || vq_head != vq_tail) { __asm__("halt"); __asm__("nop"); }
}

void pal_upload_now(void)
{
    if (!is_cgb) return;
    if (pal_req & 1) set_bkg_palette(0, 8, pal_bg_buf);
    if (pal_req & 2) set_sprite_palette(0, 8, pal_obj_buf);
    pal_req = 0;
}

/* ---- setup ---- */
static const uint8_t shadow_tile[32] = {
    0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0,
    0,0, 0,0, 0,0, 0x3C,0x3C, 0x7E,0x7E, 0x3C,0x3C, 0,0, 0,0
};

void gfx_load_world_tiles(void)
{
    ASSETS_IN();
    set_bkg_data(0, BG_TILE_COUNT, bg_tiles);
    set_sprite_data(0, SPR_TILE_COUNT, spr_tiles);
    ASSETS_OUT();
    set_sprite_data(SPR_SHADOW, 2, shadow_tile);
}

void gfx_load_title(void)
{
    ASSETS_IN();
    set_bkg_data(0, TITLE_TILE_COUNT, title_tiles);
    set_tiles(0, 0, 20, 18, (uint8_t *)0x9800, title_map);
    if (is_cgb) {
        VBK_REG = 1;
        set_tiles(0, 0, 20, 18, (uint8_t *)0x9800, title_attr);
        VBK_REG = 0;
    }
    set_sprite_data(0, SPR_TILE_COUNT, spr_tiles);
    memcpy(title_pal_r, title_pal, sizeof title_pal_r);
    ASSETS_OUT();
}

void gfx_load_map(uint8_t first, uint8_t *fog)
{
    ASSETS_IN();
    set_bkg_data(first, MAP_TILE_COUNT, map_tiles);
    memcpy(fog, &map_tiles[MAP_T_FOG * 16], 16);
    ASSETS_OUT();
}

void gfx_init(void)
{
    uint8_t i, t;
    DISPLAY_OFF;
    VBK_REG = 0;
    is_cgb = (uint8_t)(_cpu == CGB_TYPE && (VBK_REG & 0xFE) == 0xFE);
    if (is_cgb) cpu_fast();
    {
        ASSETS_IN();
        memcpy(mt_t, mt_tiles, sizeof mt_t);
        memcpy(mt_a, mt_attr, sizeof mt_a);
        memcpy(anim_ram, anim_frames, sizeof anim_ram);
        memcpy(cgb_bg, cgb_bg_pal, sizeof cgb_bg);
        memcpy(cgb_obj, cgb_obj_pal, sizeof cgb_obj);
        memcpy(dmg_bg_r, dmg_bgp, 4);
        memcpy(dmg_o0_r, dmg_obp0, 4);
        memcpy(dmg_o1_r, dmg_obp1, 4);
        for (i = 0; i < ANIM_COUNT; i++) {
            t = anim_tile[i];
            anim_dst[i] = (uint8_t *)(t < 128 ? 0x9000u + ((uint16_t)t << 4) : 0x8800u + ((uint16_t)(t - 128) << 4));
        }
        ASSETS_OUT();
    }
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
