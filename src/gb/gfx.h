/* gfx.h - OPEN WORLD bank-0 graphics services: interrupts, the split screen, VRAM queues,
 * palettes and fades. Everything here runs in bank 0 (the VBL / STAT handlers use it). */
#ifndef OW_GFX_H
#define OW_GFX_H
#include <stdint.h>
#include <gb/gb.h>

extern uint8_t is_cgb;
extern volatile uint8_t vbl_frames;       /* +1 every VBlank */
extern uint16_t dbg_frame_drops;          /* VBlanks the main loop missed while in the world */
extern uint16_t dbg_vbl_count;            /* VBlanks while dbg_count_on */
extern uint8_t dbg_count_on;

/* ---- split screen (lines 0-23 band on 0x9C00, 24-143 land on 0x9800) ---- */
#define BAND_LINES 24
extern volatile uint8_t split_mode;        /* 1: band/land split; 0: one plain screen */
/* next-frame scroll/palette values: written by the main loop, then frame_commit() */
extern uint8_t nx_band_scx, nx_land_scx, nx_land_scy, nx_band_bgp, nx_land_bgp, nx_obp0, nx_obp1;
extern uint8_t nx_scx, nx_scy;            /* plain-screen scroll (split_mode 0) */
/* live values used by the ISRs (latched from nx_* at the first VBlank after frame_commit) */
extern volatile uint8_t band_scx, land_scx, land_scy, band_bgp, land_bgp;
void frame_commit(void);                  /* publish nx_* + OAM for the next VBlank */
void frame_sync(void);                    /* wait for the next VBlank (counts misses) */
void split_enable(uint8_t on);

/* ---- VRAM queues, drained in VBlank ---- */
/* land metatile writes: ring slot (0..15, 0..15) of map 0x9800, its 4 tiles (attrs from mt) */
void bq_push(uint8_t col, uint8_t row, uint8_t mt, const uint8_t *t);
uint8_t bq_pending(void);
extern uint8_t scratch[160];           /* main-loop transient buffer (refill, map screen) */
/* single tile (+CGB attribute) writes anywhere in the BG maps */
void vq_push(uint16_t addr, uint8_t tile, uint8_t attr);
uint8_t vq_pending(void);
void vram_flush(void);                    /* wait until both queues are empty */

extern uint8_t mt_t[];                    /* RAM copies of mt_tiles / mt_attr: MT_COUNT * 4 */
extern uint8_t mt_a[];
extern volatile uint8_t anim_on;
extern volatile uint8_t hook_on, hook_busy;   /* VBL runs world_frame() while hook_on */

/* ---- palettes ---- */
extern uint8_t pal_phase_from, pal_phase_to, pal_t;   /* t: 0..16 blend from -> to */
extern uint8_t pal_fog;                   /* 0..8 contrast collapse */
extern uint8_t pal_fade;                  /* 0..16 towards white */
extern uint8_t pal_flash;                 /* 1: lightning (all light) */
extern uint8_t pal_band_bright;           /* 0..16 band towards white (the ending) */
void pal_tick(void) BANKED;                /* advance an incremental CGB palette job */
void pal_apply(void) BANKED;                     /* compute palettes into nx_* / CGB buffers */
void pal_title(void) BANKED;                     /* CGB: title palettes */
void pal_upload_now(void);
uint16_t pal_rom(uint8_t ph, uint8_t k);   /* colour k (0-31 BG, 32-63 OBJ) of phase ph from ROM */
void pal_paper(const uint16_t *c4, uint8_t f) BANKED;
uint8_t shade_fade(uint8_t p, uint8_t f) BANKED;  /* DMG palette p towards white (f 0..16) */

void gfx_init(void);
void gfx_load_world_tiles(void);          /* BG world tileset + sprite tiles (LCD may be on) */
void gfx_load_title(void);                /* title BG tiles/map/attr + sprite tiles */
void gfx_load_map(uint8_t first, uint8_t *fog);  /* map frame tiles at `first`, fog pattern out */
void hide_sprites_from(uint8_t first);
/* running late in the frame (skip optional work) */
#define FRAME_LATE() (LY_REG >= 96 && LY_REG < 144)
void fade_to(uint8_t target, uint8_t speed) BANKED; /* animate pal_fade (runs frames) */
void wait_frames(uint8_t n);

/* OAM helpers: sprites are written straight into GBDK's shadow OAM (DMA'd every VBlank);
   game frames run right after a VBlank, so a frame's writes all land before the next DMA */
#define oam ((uint8_t *)shadow_OAM)
void spr_set_f(uint8_t i, uint8_t x, uint8_t y, uint8_t tile, uint8_t prop);
#define spr_set(i, x, y, t, p) do { uint8_t *_o = &oam[(uint8_t)(i) << 2]; _o[0] = (uint8_t)(y); _o[1] = (uint8_t)(x); _o[2] = (uint8_t)(t); _o[3] = (uint8_t)(p); } while (0)
#define spr_hide(i) (oam[(uint8_t)(i) << 2] = 0)

/* engine-generated sprite tiles after the art's (8x16 pairs) */
#define SPR_SHADOW  126

/* sprite slots */
#define SP_PLAYER   0   /* 2 */
#define SP_HINT     2
#define SP_FX       3   /* 2: glide shadow, sparks */
#define SP_GLOW     5   /* 8 */
#define SP_WATCH    13  /* 4 */
#define SP_PIPS     17  /* 4 */
#define SP_ICON     21
#define SP_MARK     22  /* 5: beacons 0-2, heart, cairn */
#define SP_WX       27  /* 13 weather */
#define NUM_WX      13
#define NUM_SPR     40

#endif
