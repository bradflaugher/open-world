/* sound_int.h - internals shared by sound.c (bank 0: API + VBL entry) and
 * sound_core.c (autobanked: the ambient engine, sfx player and all sound data). */
#ifndef SOUND_INT_H
#define SOUND_INT_H

#include <stdint.h>

/* requests: written by the API (main thread), consumed by snd_core_tick() (VBL) */
extern volatile uint8_t snd_req_mode, snd_req_mute, snd_req_fast, snd_req_bm, snd_req_seed;
extern volatile uint8_t snd_p_biome, snd_p_phase, snd_p_wx, snd_p_dirty;
extern volatile uint16_t snd_seed;
extern volatile uint8_t snd_rq[4];
extern volatile uint8_t snd_rq_head, snd_rq_tail;

/* engine state read back by the API */
extern uint8_t snd_cur_mode, snd_xf, snd_xf_next, snd_owned, snd_vo_on;

#define SND_REQ_NONE 0xFF
#define SND_XF_OUT 1

void snd_core_init(void);
void snd_core_tick(void);

#endif
