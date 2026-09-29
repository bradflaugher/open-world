/* sound.c - OPEN WORLD sound (temporary stub; real engine in progress) */
#include <stdint.h>
#include "sound.h"
static uint8_t s_mode;
void sound_init(void) {}
void sound_tick(void) {}
void ambient_mode(uint8_t mode) { s_mode = mode; }
void ambient_set(uint8_t biome, uint8_t phase, uint8_t weather) { (void)biome; (void)phase; (void)weather; }
void ambient_seed(uint16_t seed) { (void)seed; }
void ambient_beacons(uint8_t lit_mask) { (void)lit_mask; }
void ambient_tempo(uint8_t fast) { (void)fast; }
void sfx_play(uint8_t sfx) { (void)sfx; }
void sound_mute_all(uint8_t on) { (void)on; }
uint8_t sfx_playing(void) { return 0; }
uint8_t sound_debug_mode(void) { return s_mode; }
