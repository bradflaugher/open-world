/* world.h - OPEN WORLD portable world core (compiles with SDCC for the Game Boy and gcc on host).
 *
 * THE SHARED CONTRACT between src/core (world generation), src/gb (engine) and assets (art).
 * Do not renumber the MT_* enum without updating assets/metatiles.txt.
 *
 * World: 65536 x 65536 metatiles (16x16 px each), wrapping (torus). Deterministic from a
 * 16-bit seed: same seed + same coordinates -> same metatile, on host and on Game Boy.
 * All maths is fixed-width integer; no multiply/divide in hot paths (SDCC makes them calls).
 */
#ifndef WORLD_H
#define WORLD_H

#include <stdint.h>

/* Cold functions (world_init, bearing / distance helpers) live in world_gen.c, which is banked on
 * the Game Boy; the hot path (world_mt and friends, world.c) is plain bank-0 code. */
#ifdef __SDCC
#include <gb/gb.h>
#define WBANKED BANKED
#else
#define WBANKED
#endif

/* ---- metatiles: what a 16x16 cell looks like / is ---------------------------------------- */
enum {
    MT_SEA = 0,        /* deep water, animated                       impassable */
    MT_SEA_GLINT,      /* deep water with a light glint (colour 0)   impassable */
    MT_SHALLOW,        /* pale shallows                              impassable (unless stepping stone) */
    MT_STEPSTONE,      /* stepping stone placed on shallows          walkable */
    MT_SAND,           /* shore sand                                 walkable */
    MT_GRASS,          /* meadow ground                              walkable */
    MT_GRASS_TALL,     /* tall grass                                 walkable, hides */
    MT_FLOWERS,        /* meadow flowers                             walkable */
    MT_TREE,           /* broadleaf tree                             solid */
    MT_UNDERGROWTH,    /* forest floor / ferns                       walkable, slow, hides */
    MT_PINE,           /* snowy pine                                 solid */
    MT_SNOW,           /* snow ground                                walkable, cold */
    MT_DUNE,           /* desert dune ripples                        walkable */
    MT_BONES,          /* desert bones / ribcage                     walkable */
    MT_ROCK,           /* crag / mountain                            solid (cloak can glide over) */
    MT_ROCK_PEAK,      /* high peak (darker)                         solid */
    MT_ASH,            /* ash ground                                 walkable */
    MT_GLASS,          /* glass shards (colour-0 glints)             walkable */
    MT_MONOLITH,       /* black standing stone                       solid */
    MT_RUIN_WALL,      /* broken wall                                solid */
    MT_RUIN_FLOOR,     /* flagstones                                 walkable */
    MT_PILLAR,         /* standing column                            solid */
    MT_STATUE_HAND,    /* fragment of an enormous statue             solid */
    MT_ROAD,           /* ancient causeway                           walkable */
    MT_BRAMBLE,        /* thorn wall                                 solid (lantern burns -> MT_ASH) */
    MT_FIRE_COLD,      /* cold campfire                              solid, interact */
    MT_FIRE_LIT,       /* burning campfire (animated, colour 0)      solid, warmth */
    MT_CAIRN,          /* cairn you built                            solid, interact (pick up) */
    MT_CAIRN_OLD,      /* ancient cairn (from an earlier world)      solid */
    MT_BEACON,         /* unlit beacon pillar                        solid, interact */
    MT_BEACON_LIT,     /* lit beacon (animated light)                solid, warmth */
    MT_SHRINE,         /* pedestal holding an item                   solid, interact */
    MT_SHRINE_EMPTY,   /* empty pedestal                             solid */
    MT_HEART,          /* the Heart: the world's end                 solid, interact */
    MT_TABLE,          /* a table set for two (vignette)             solid */
    MT_WELL,           /* a well (vignette)                          solid */
    MT_COUNT
};

/* metatile property flags (world_flags) */
#define MTF_SOLID    0x01  /* blocks walking */
#define MTF_WATER    0x02  /* is water (sea or shallows) */
#define MTF_HIDE     0x04  /* hides the player from Watchers */
#define MTF_SLOW     0x08  /* half speed */
#define MTF_COLD     0x10  /* extra warmth drain */
#define MTF_WARM     0x20  /* lit fire / beacon: warmth source (adjacent) */
#define MTF_INTERACT 0x40  /* A does something here (fire, beacon, shrine, heart, cairn) */
#define MTF_GLIDE    0x80  /* cloak may glide over this (rock, water, bramble, ...) */

extern const uint8_t mt_flags[MT_COUNT];

/* biomes (for music, warmth, map shading, Watchers) */
enum { B_SEA, B_SHALLOW, B_SHORE, B_MEADOW, B_FOREST, B_DESERT, B_TUNDRA, B_ROCK, B_ASH, B_RUINS, B_COUNT };

/* items (bitmask for owned items) */
enum { IT_LANTERN = 0, IT_STONES = 1, IT_CLOAK = 2, IT_COUNT };

/* set pieces */
#define NUM_BEACONS 3
typedef struct { uint16_t x, y; } wpos_t;

typedef struct {
    uint16_t seed;
    wpos_t start;                  /* the cold fire you wake beside is at start (player spawns just south) */
    wpos_t beacon[NUM_BEACONS];    /* beacon pillar positions */
    wpos_t shrine[NUM_BEACONS];    /* shrine pedestal next to beacon 0 (STONES) and 1 (CLOAK); [2] unused = beacon */
    wpos_t heart;
} world_layout_t;

extern world_layout_t world;       /* filled by world_init */

/* Compute the layout; clears nothing else (mods are separate). Set world_old_cairns first.
 * Slow (~0.5-1 s on the Game Boy): call once per world, never per frame. */
void    world_init(uint16_t seed) WBANKED;
uint8_t world_mt(uint16_t mx, uint16_t my);    /* final metatile incl. mods; the hot path */
uint8_t world_mt_base(uint16_t mx, uint16_t my);/* generated metatile, ignoring mods */
uint8_t world_biome(uint16_t mx, uint16_t my); /* biome of that cell (cheap-ish) */
uint8_t world_map_shade(uint16_t mx, uint16_t my); /* 0..3 map shade for the map screen (fast, coarse) */
uint8_t world_detail(uint16_t mx, uint16_t my); /* 0..255 hash for per-cell variation (tile flips, anims) */

/* ---- mods: persistent world edits (lit fires, cairns, stepping stones, burnt brambles...) ---- */
#define MAX_MODS 96
typedef struct { uint16_t x, y; uint8_t mt; } wmod_t;
extern wmod_t  world_mods[MAX_MODS];
extern uint8_t world_mod_count;
uint8_t world_mod_set(uint16_t mx, uint16_t my, uint8_t mt); /* add/replace; returns 0 if table full */
void    world_mods_clear(void);
/* Rebuild the mods lookup index. Call after writing world_mods[] / world_mod_count directly
 * (e.g. after loading a save). world_mt also rebuilds lazily when world_mod_count changes. */
void    world_mods_rebuild(void);

/* ---- ancient cairns carried over from earlier worlds (offsets from start) ---- */
#define MAX_OLD_CAIRNS 32
typedef struct { int8_t dx, dy; } coff_t;     /* offset in units of 4 metatiles from start */
extern coff_t  world_old_cairns[MAX_OLD_CAIRNS];
extern uint8_t world_old_cairn_count;

/* ---- helpers ---- */
/* banked (cold) but cheap: a few hundred M-cycles each, fine to call a few times per frame */
uint8_t world_bearing(uint16_t fx, uint16_t fy, uint16_t tx, uint16_t ty) WBANKED; /* 0..255 angle, 0 = north, 64 = east */
uint16_t world_dist(uint16_t ax, uint16_t ay, uint16_t bx, uint16_t by) WBANKED;   /* approx (octagonal) distance, wraps */

#ifdef WORLD_INTERNAL
/* ---- shared between world.c (hot) and world_gen.c (cold); not part of the engine API ---- */
#define W_NUM_ROADS (NUM_BEACONS + 1)
#define W_ROAD_POOL 168                   /* breakpoint bytes shared by all roads */
typedef struct {
    uint16_t a_maj, a_min;                /* start point, (major, minor) axis */
    uint16_t bx, by, bw, bh;              /* bounding box: x in [bx, bx+bw], y in [by, by+bh] */
    uint8_t dmin;                         /* |minor delta| */
    uint8_t flags;                        /* W_R_* */
    uint8_t n;                            /* number of minor steps */
    uint8_t shift;                        /* minor step = 1 << shift metatiles */
    uint8_t x0;                           /* first breakpoint in w_road_x[] (n + 2 entries) */
} w_road_t;
#define W_R_YMAJOR 1                      /* major axis is y */
#define W_R_MAJNEG 2                      /* major coordinate decreases along the road */
#define W_R_MINNEG 4                      /* minor coordinate decreases along the road */
extern w_road_t w_roads[W_NUM_ROADS];
extern uint8_t  w_road_x[W_ROAD_POOL];    /* |major - a_maj| of each breakpoint */
extern uint8_t  w_s0, w_s1;               /* seed bytes mixed into every hash */
extern uint8_t  w_ready;                  /* set pieces valid */
extern uint8_t  w_start_ground;
extern uint16_t w_spx0, w_spy0;           /* coarse filter origin (64-metatile cells) */
extern uint8_t  w_sp_bits[32];            /* 16 x 16 cells, bit set = set piece / road there */
extern const uint8_t w_perm[256];
void    w_reset(void);                    /* clear all caches (seed or layout changed) */
#endif


#ifndef __SDCC
/* ---- host-only (tests / owgen): reachability over a window of metatiles (src/core/wreach.c) ---- */
#define WR_HALF 320                          /* window is (2*WR_HALF)^2 metatiles around the centre */
#define WR_LANTERN (1u << IT_LANTERN)
#define WR_STONES  (1u << IT_STONES)
#define WR_CLOAK   (1u << IT_CLOAK)
void    wr_load(uint16_t cx, uint16_t cy);   /* sample world_mt over the window */
uint8_t wr_get(uint16_t mx, uint16_t my);    /* sampled metatile (0xFF outside the window) */
/* flood fill from (sx,sy) with the given item bits. diag = 0: strict (4-way walk, glide exactly 3);
 * diag = 1: permissive (8-way walk without corner rule, glide 2 or 3) */
void    wr_bfs(uint16_t sx, uint16_t sy, uint8_t items, uint8_t diag);
uint8_t wr_reached(uint16_t mx, uint16_t my);      /* this cell was reached */
uint8_t wr_reached_adj(uint16_t mx, uint16_t my);  /* a 4-neighbour of this cell was reached */
uint32_t wr_count(void);                           /* cells reached */
/* Full progression check of the current world (after world_init; loads the window itself).
 * Returns 0 if OK, else a bitmask of WRF_* failures. */
#define WRF_START    0x0001   /* start is not a cold fire with walkable spawn / clearing */
#define WRF_B0       0x0002   /* beacon 0 or its shrine unreachable with the lantern */
#define WRF_B1       0x0004   /* beacon 1 or its shrine unreachable with lantern + stones */
#define WRF_B2       0x0008   /* beacon 2 unreachable with all items */
#define WRF_HEART    0x0010   /* heart unreachable with all items */
#define WRF_GATE0    0x0020   /* beacon 0 reachable without the lantern */
#define WRF_GATE1    0x0040   /* beacon 1 reachable without stones */
#define WRF_GATE2    0x0080   /* beacon 2 reachable without the cloak */
#define WRF_PIECES   0x0100   /* set pieces not intact (beacon, shrine, heart tiles) */
uint16_t wr_check_world(void);
#endif

#endif
