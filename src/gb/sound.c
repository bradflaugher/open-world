/* sound.c - OPEN WORLD sound: the bank-0 part (public API + VBL entry point).
 *
 * The engine itself (generative ambient + sfx, ~7 KB of code and data) lives in
 * sound_core.c, which is autobanked.  sound_init() / sound_tick() map its bank in, call it
 * and restore the caller's bank (safe from the VBL ISR, even with interrupts enabled).
 * All other API calls only post requests into WRAM, so they are cheap and bank-free.
 *
 * Host tests (-DHOST_TEST) compile this file alone: it then includes sound_core.c.
 */
#include <stdint.h>
#include "sound.h"
#include "hw_sound.h"
#include "sound_int.h"

volatile uint8_t snd_req_mode = SND_REQ_NONE, snd_req_mute, snd_req_fast, snd_req_bm, snd_req_seed;
volatile uint8_t snd_p_biome, snd_p_phase, snd_p_wx, snd_p_dirty;
volatile uint16_t snd_seed;
volatile uint8_t snd_rq[4];
volatile uint8_t snd_rq_head, snd_rq_tail;

#ifdef HOST_TEST
uint8_t host_snd_regs[0x40];
host_snd_hook_t host_snd_hook;
uint8_t host_snd_src;
void host_snd_write(uint8_t reg, uint8_t val)
{
    if (reg < 0x40)
        host_snd_regs[reg] = val;
    if (host_snd_hook)
        host_snd_hook(reg, val);
}
#define SND_CALL(f) f()
#else
#include <gb/gb.h>
BANKREF_EXTERN(sound_core)
#define SND_CALL(f) do { uint8_t sb_ = CURRENT_BANK; SWITCH_ROM(BANK(sound_core)); f(); SWITCH_ROM(sb_); } while (0)
#endif

void sound_init(void)
{
    SND_CALL(snd_core_init);
}

void sound_tick(void)
{
    SND_CALL(snd_core_tick);
}

void ambient_mode(uint8_t mode)
{
    if (mode >= NUM_AMB)
        return;
    if (mode == AMB_WORLD && snd_req_mode == AMB_WAKE)
        return;                             /* WAKE flows into WORLD by itself */
    snd_req_mode = mode;
}

void ambient_set(uint8_t biome, uint8_t phase, uint8_t weather)
{
    if (biome != snd_p_biome || phase != snd_p_phase || weather != snd_p_wx) {
        snd_p_biome = biome;
        snd_p_phase = phase;
        snd_p_wx = weather;
        snd_p_dirty = 1;
    }
}

void ambient_seed(uint16_t seed)
{
    snd_seed = seed;
    snd_req_seed = 1;
    snd_p_dirty = 1;
}

void ambient_beacons(uint8_t lit_mask) { snd_req_bm = (uint8_t)(lit_mask & 7); }
void ambient_tempo(uint8_t fast) { snd_req_fast = fast ? 1 : 0; }
void sound_mute_all(uint8_t on) { snd_req_mute = on ? 1 : 0; }

void sfx_play(uint8_t sfx)
{
    uint8_t h;
    if (sfx >= NUM_SFX)
        return;
    h = snd_rq_head;
    if (((h + 1) & 3) == snd_rq_tail)
        return;                             /* queue full: drop */
    snd_rq[h] = sfx;
    snd_rq_head = (uint8_t)((h + 1) & 3);
}

uint8_t sfx_playing(void)
{
    return (uint8_t)(snd_vo_on != 0 || snd_rq_head != snd_rq_tail);
}

uint8_t sound_debug_mode(void)
{
    uint8_t r = snd_req_mode;
    if (r != SND_REQ_NONE)
        return r;
    if (snd_xf == SND_XF_OUT)
        return snd_xf_next;
    return snd_cur_mode;
}

uint8_t sound_debug_owned(void) { return snd_owned; }

#ifdef HOST_TEST
#include "sound_core.c"
#endif
