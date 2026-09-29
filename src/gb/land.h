/* land.h - OPEN WORLD streaming land: a 16x16-metatile ring on BG map 0x9800 (32x32 tiles).
 *
 * The committed window is [land_x0, land_x0+15) x [land_y0, land_y0+15): every slot in it holds
 * the right metatile both in the cache (for collision / logic) and (after the VBlank queue
 * drains) in VRAM. Moving the window by one column or row is one atomic "job" of 15
 * world_mt() calls, spread over frames by the per-frame budget. */
#ifndef OW_LAND_H
#define OW_LAND_H
#include <stdint.h>

extern uint16_t land_x0, land_y0;       /* committed window origin (metatiles) */
extern uint8_t land_cache[256];         /* [(my & 15) << 4 | (mx & 15)] */
extern uint8_t land_job;                /* 0 idle, 1 col+, 2 col-, 3 row+, 4 row- */
extern uint16_t dbg_mt_calls;           /* world_mt calls made by the streamer (tests) */

void land_refill(uint16_t cam_mx, uint16_t cam_my);  /* full synchronous load (LCD may be on) */
/* stream towards the camera's window; budget = world_mt calls allowed this frame.
   Returns 1 if the window lags so far behind that garbage could show (caller refills). */
uint8_t land_update(uint16_t cam_mx, uint16_t cam_my, uint8_t budget);
uint8_t land_mt(uint16_t mx, uint16_t my);           /* cached metatile (falls back to world_mt) */
void land_set(uint16_t mx, uint16_t my, uint8_t mt); /* show mt at (mx,my) now (no mod) */
uint8_t land_in(uint16_t mx, uint16_t my);           /* inside the committed window */

#endif
