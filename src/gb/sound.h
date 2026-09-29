/* sound.h - OPEN WORLD generative ambient sound engine + sfx (GBDK-2020, host-testable).
 *
 * CONTRACT between the engine (src/gb/game*.c) and the audio module (src/gb/sound.c).
 * Every public call only posts a request; all APU register work happens in sound_tick(),
 * which the engine calls exactly once per frame from the VBL interrupt. Safe from main loop.
 * Build on host with -DHOST_TEST (register writes go to a fake APU for tests).
 */
#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

/* ambient modes */
enum { AMB_SILENT, AMB_TITLE, AMB_WORLD, AMB_MAP, AMB_ENDING, AMB_WAKE, NUM_AMB };
/* time-of-day phases (shared with the engine) */
enum { PH_DAWN, PH_DAY, PH_DUSK, PH_NIGHT };
/* weather */
enum { WX_CLEAR, WX_RAIN, WX_SNOW, WX_FOG, WX_STORM };

enum {
    SFX_STEP_SOFT,   /* grass, meadow, undergrowth */
    SFX_STEP_SAND,   /* sand, dunes, ash */
    SFX_STEP_SNOW,   /* snow crunch */
    SFX_STEP_STONE,  /* road, ruin floor, stepping stone, glass */
    SFX_LIGHT,       /* lantern lights a fire */
    SFX_BURN,        /* brambles burn away */
    SFX_CAIRN,       /* stone placed (cairn) */
    SFX_STEPSTONE,   /* stone dropped in water */
    SFX_PICKUP,      /* picked a stone back up */
    SFX_GLIDE,       /* cloak glide (whoosh) */
    SFX_LAND,        /* glide landing */
    SFX_NO,          /* refused action (soft low blip) */
    SFX_ITEM,        /* new item taken from a shrine (ascending, important) */
    SFX_BEACON,      /* a beacon ignites (big, resonant) */
    SFX_HEART,       /* the Heart revealed on the horizon */
    SFX_WHITEOUT,    /* warmth gone / touched by a Watcher */
    SFX_DAWN,        /* morning arrives */
    SFX_SELECT,      /* item cycled */
    SFX_MAP,         /* map opened / closed (paper) */
    SFX_THUNDER,     /* lightning */
    SFX_SIT,         /* sitting down (soft) */
    NUM_SFX
};

void sound_init(void);                 /* enable APU, init state; call once at boot */
void sound_tick(void);                 /* once per frame from VBL */
void ambient_mode(uint8_t mode);       /* AMB_*; crossfades where sensible */
void ambient_set(uint8_t biome, uint8_t phase, uint8_t weather); /* B_* from world.h, PH_*, WX_* */
void ambient_seed(uint16_t seed);      /* world seed: flavours the generative phrases */
void ambient_beacons(uint8_t lit_mask);/* bit i = beacon i lit: each adds a tone to the motif */
void ambient_tempo(uint8_t fast);      /* 1 while sitting (time runs 8x): music drifts quicker */
void sfx_play(uint8_t sfx);            /* play sfx (borrows channels, gives them back) */
void sound_mute_all(uint8_t on);

/* debug / tests */
uint8_t sfx_playing(void);
uint8_t sound_debug_mode(void);        /* current ambient mode */

#endif
