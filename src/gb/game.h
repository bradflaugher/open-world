/* game.h - OPEN WORLD game state shared by the banked game modules (and read by the tests
 * through the .sym file, so keep these non-static). */
#ifndef OW_GAME_H
#define OW_GAME_H
#include <gb/gb.h>
#include <stdint.h>
#include "world.h"

enum { GS_BOOT = 0, GS_TITLE, GS_WORLD, GS_MAP, GS_ENDING, GS_WHITEOUT };
enum { PL_SLEEP = 0, PL_STAND, PL_WALK, PL_SIT, PL_GLIDE };
enum { D_N = 0, D_NE, D_E, D_SE, D_S, D_SW, D_W, D_NW };

#define DAY_FRAMES   28800u   /* 8 minutes at 60 fps */
#define T_DAWN       0u
#define T_DAY        2880u    /* dawn 10 %, day 40 %, dusk 10 %, night 40 % */
#define T_DUSK       14400u
#define T_NIGHT      17280u
#define WARMTH_WAKE  511u     /* a whiteout wakes you with two pips */
#define PHASE_BLEND  1024u    /* frames to blend into a new phase (16 steps) */
#define WARMTH_MAX   1023u
#define STONES_MAX   12
#define MAX_CAIRNS   32
#define VISIT_W      128      /* visited-chunk bitmap: 128 x 128 chunks of 16x16 metatiles (+-1024) */

extern volatile uint8_t game_state;
extern uint8_t pl_state, pl_face, pl_anim;
extern uint16_t pl_mx, pl_my;        /* the foot point: metatile ... */
extern uint8_t pl_sx, pl_sy;         /* ... + pixel inside it (0..15) */
extern uint16_t cam_mx, cam_my;      /* top-left of the land view */
extern uint8_t cam_sx, cam_sy;
extern int8_t pl_lift;               /* glide: sprite height above the ground */
extern uint16_t tod, day_count;
extern uint8_t phase, weather, biome_here;
extern uint8_t items, equipped, beacons_lit, stones;
extern uint16_t warmth;
extern uint16_t respawn_x, respawn_y;
extern uint8_t worlds_done;
extern uint8_t cairn_n;
extern wpos_t cairns[MAX_CAIRNS];
extern uint8_t visited[VISIT_W * VISIT_W / 8];
extern uint8_t keys, pressed;
extern uint8_t idle_t;
extern uint8_t item_pulse;
extern uint8_t shake;
extern int8_t shake_x, shake_y;
extern uint8_t ending_req;
extern uint8_t heart_revealed;
extern uint8_t glow_on;
extern uint8_t near_warm, warm6;   /* warm6: lit fire / beacon within 6 (main loop, every 8 frames) */
uint8_t warm_within(uint8_t r) BANKED;   /* lit fire / beacon within r cells (mods table) */          /* a lit fire / beacon within 3 cells */
extern uint8_t hint_x, hint_y, hint_on;
extern uint8_t band_mark_x[5];

/* test hooks */
extern uint16_t dbg_seed;            /* non-zero: seed for the next new world */
extern uint8_t dbg_teleport;         /* set dbg_tx/dbg_ty then 1: player jumps there */
extern uint16_t dbg_tx, dbg_ty;
extern uint8_t dbg_max_line;         /* most sprites seen on one line (land view) */
extern uint8_t dbg_saves;

/* modules (all banked) */
void game_main(void) BANKED;
void world_enter(uint8_t fresh) BANKED;           /* load the world view around the player */
void camera_update(void) BANKED;
/* world edits: shown now, the mod applied by the main loop. Return 1 if accepted. */
#define EDIT_RESERVE 16                                     /* mod slots kept for progression */
uint8_t edit_mt(uint16_t mx, uint16_t my, uint8_t mt) BANKED;           /* progression */
uint8_t edit_mt_r(uint16_t mx, uint16_t my, uint8_t mt, uint8_t reserve) BANKED;
void edit_remove(uint16_t mx, uint16_t my) BANKED;          /* drop a mod (cairn picked up) */
uint8_t edit_room(uint8_t reserve) BANKED;                  /* room for another mod? */
void world_frame(void) BANKED;                              /* one frame of play (from the VBL ISR) */
extern volatile uint8_t save_req;                           /* main loop saves when set */

void player_update(void) BANKED;
void player_draw(void) BANKED;
uint8_t blocked_mt(uint8_t mt) BANKED;
void player_place(uint16_t mx, uint16_t my) BANKED; /* foot at the centre of the cell */

void band_build(void) BANKED;
uint8_t band_stars(uint8_t on) BANKED;   /* 0: queue busy, try again */
void band_update(void) BANKED;
void band_reset_angle(void) BANKED;

void fx_update(void) BANKED;        /* glow, weather, HUD sprites */
void fx_weather_roll(void) BANKED;
void hud_update(void) BANKED;
void fx_redraw(void) BANKED;       /* forget cached sprite state (after the OAM was cleared) */

void map_screen(void) BANKED;
void light_beacon(uint8_t i) BANKED;

void save_write(void) BANKED;
uint8_t save_load(void) BANKED;     /* 1 if a valid save was loaded into the game state */
uint8_t save_exists(void) BANKED;

void visit_mark(void) BANKED;
uint8_t visit_get(uint8_t cx, uint8_t cy) BANKED;

void watchers_update(void) BANKED;
void watchers_reset(void) BANKED;
extern uint8_t watch_on;

#endif
