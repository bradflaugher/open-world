# Module contracts

The three shared contracts are:
- `src/core/world.h`: the world core API and the metatile IDs.
- `src/gb/sound.h`: the sound engine API.
- the asset API below.

The asset API is generated as `src/gb/assets.h` / `src/gb/assets.c` by `tools/gen_assets.py`
from `assets/*.txt`.

## Assets (`src/gb/assets.h`)

VRAM plan:
- **Sprites, 8x16 mode (LCDC.2 = 1):** tiles 0-127 at 0x8000-0x87FF.
- **BG, 0x8800 addressing (LCDC.4 = 0):** 256 tiles. Indices 0-127 are at 0x9000 and
  128-255 at 0x8800. The world tileset (metatiles plus horizon band) must fit in
  **<= 224 tiles**. The engine loads it with `set_bkg_data(0, BG_TILE_COUNT, bg_tiles)`,
  which honours LCDC.4.
- The title and the map screen load their own BG tilesets and reload the world set on exit.

```c
#include <stdint.h>
#include "world.h"            /* MT_COUNT */

/* ---- world BG tileset ---- */
#define BG_TILE_COUNT  ...    /* <= 224 */
extern const uint8_t bg_tiles[];              /* BG_TILE_COUNT * 16 bytes, 2bpp GB format */
extern const uint8_t mt_tiles[MT_COUNT][4];   /* tile indices TL, TR, BL, BR for each metatile */
extern const uint8_t mt_attr[MT_COUNT][4];    /* CGB attribute byte per tile (palette 0-7 | flips) */

/* Animated BG tiles: the engine rewrites the pattern of bg tile anim_tile[i] with
   anim_frames[i][f] (16 bytes) every ANIM_PERIOD frames, cycling f = 0..ANIM_FRAMES-1. */
#define ANIM_COUNT   ...
#define ANIM_FRAMES  4
#define ANIM_PERIOD  16
extern const uint8_t anim_tile[ANIM_COUNT];
extern const uint8_t anim_frames[ANIM_COUNT][ANIM_FRAMES][16];

/* Horizon band tiles (BG). The band is 3 tile rows (24 px) x 32 columns on map 0x9C00. */
#define BAND_SKY_TOP     ...  /* plain sky tile, row 0 (index into bg_tiles) */
#define BAND_SKY         ...  /* plain sky tile, rows 1-2 */
#define BAND_STAR0       ...  /* sky with a star (colour 0), 2 variants: BAND_STAR0, BAND_STAR0+1 */
#define BAND_LAND        ...  /* fully solid distant land */
#define BAND_RIDGE0      ...  /* 8 tiles: land filling the bottom k+1 pixel rows, k = 0..7 (flat tops) */
#define BAND_SLOPE_UP0   ...  /* 8 tiles: a 45-degree rise within a tile, base heights 0..7 (optional) */
#define BAND_SLOPE_DN0   ...  /* 8 tiles: a 45-degree fall */

/* ---- sprites: 8x16 OBJ tiles (each sprite uses 2 consecutive tiles: even index = top) ---- */
#define SPR_TILE_COUNT ...    /* <= 128 */
extern const uint8_t spr_tiles[];
/* Player (16x16 = two 8x16 sprites side by side; each constant is the LEFT half's top tile,
   the right half is +2). Side-facing frames face RIGHT; the engine flips for left. */
#define SPR_PL_DOWN0 ... /* walk frames 0/1: SPR_PL_DOWN0, SPR_PL_DOWN1 */
#define SPR_PL_DOWN1 ...
#define SPR_PL_UP0 ...
#define SPR_PL_UP1 ...
#define SPR_PL_SIDE0 ...
#define SPR_PL_SIDE1 ...
#define SPR_PL_SIT   ...  /* sitting, seen from behind (the Rückenfigur) */
#define SPR_PL_SLEEP ...  /* lying by the fire */
#define SPR_PL_GLIDE ...  /* cloak spread */
/* single 8x16 sprites: */
#define SPR_PIP_FULL ...  /* warmth ember, lit */
#define SPR_PIP_EMPTY ...
#define SPR_ICON_LANTERN ...
#define SPR_ICON_STONES ...
#define SPR_ICON_CLOAK ...
#define SPR_HINT_A ...    /* bobbing "press A" pictogram (a hand / button, no letters) */
#define SPR_BAND_BEACON ...     /* unlit beacon pillar on the horizon */
#define SPR_BAND_BEACON_LIT ... /* lit beacon: column of light */
#define SPR_BAND_HEART ...      /* the Heart on the horizon */
#define SPR_BAND_CAIRN ...      /* small cairn on the horizon */
#define SPR_RAIN ...      /* rain streak */
#define SPR_SNOW ...      /* snowflake */
#define SPR_GLOW0 ...     /* lantern glow dither, 4 tiles forming a quarter ring (see note) */
#define SPR_WATCHER ...   /* Watcher: 16x32 = 2x2 sprites; constant = top-left top tile, TR +2, BL +4, BR +6 */
#define SPR_BIRD0 ...     /* 2 frames */
#define SPR_MAP_PLAYER ...  /* map markers */
#define SPR_MAP_BEACON ...
#define SPR_MAP_CAIRN ...
#define SPR_MAP_HEART ...

/* ---- palettes ---- */
/* DMG: BGP for each phase (PH_DAWN, PH_DAY, PH_DUSK, PH_NIGHT). Colour 0 = LIGHT. */
extern const uint8_t dmg_bgp[4];
extern const uint8_t dmg_obp0[4], dmg_obp1[4];
/* CGB: 8 BG palettes x 4 colours (RGB555) for each phase, and 8 OBJ palettes. */
enum { PAL_GRASS, PAL_FOREST, PAL_WATER, PAL_SAND, PAL_SNOW, PAL_ROCK, PAL_LIGHT, PAL_SKY };
extern const uint16_t cgb_bg_pal[4][8][4];
extern const uint16_t cgb_obj_pal[4][8][4];

/* ---- title screen ---- */
#define TITLE_TILE_COUNT ...
extern const uint8_t title_tiles[];
extern const uint8_t title_map[20 * 18];
extern const uint8_t title_attr[20 * 18];
extern const uint16_t title_pal[8][4];

/* ---- map screen tiles (frame/border/fog) ---- */
#define MAP_TILE_COUNT ...
extern const uint8_t map_tiles[];
```

**Art rule:** colour index 0 is LIGHT (fire, beacons, stars, glints and glow). Ground mostly
uses index 1, detail uses 2 and deep shadow uses 3. The night palette crushes 1, 2 and 3
towards black and keeps 0 bright, so at night only light stays light.

## World core (`src/core/world.h`)

See the header. Hot path: the engine calls `world_mt(mx, my)` about 11-22 times per 16
frames while scrolling. It must cost no more than ~1500 M-cycles on SM83 when compiled by
SDCC.

## Sound (`src/gb/sound.h`)

See the header. The engine calls `sound_tick()` from its VBL ISR.
