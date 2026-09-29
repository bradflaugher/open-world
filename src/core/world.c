/* world.c - OPEN WORLD world core, HOT PATH: noise, biomes, metatiles, set-piece lookup, mods.
 *
 * Portable C: compiles with SDCC (Game Boy, SM83) and gcc (host tests / owgen).
 * Not banked: keep this file in bank 0 (the engine calls world_mt while scrolling). The cold
 * half (world_init, layout, causeways, bearing / distance) is world_gen.c, banked.
 *
 * Performance notes (SM83):
 *  - No multiply / divide / modulo on variables here.
 *  - The noise fields are evaluated on a 4-metatile lattice (the corners of 4x4 blocks) and
 *    bilinearly interpolated inside the block with 2-bit weights (shifts and adds only).
 *    Bilinear-of-bilinear is exact, so this equals interpolating each octave on its own grid.
 *  - An 8-block cache keeps each block's corners and the cells computed so far. Cells are
 *    computed one at a time, on demand (bounded cost per call); a new block copies shared
 *    corners from its cached neighbours, so scrolling computes about one lattice point per
 *    new block, never more than 2 per call unless the caller jumps.
 *  - Rare work (lattice points, POI rolls, ruins, set pieces, old cairns) is in world_gen.c
 *    (banked).
 *  - All caches are pure: results never depend on the call order.
 */
#define WORLD_INTERNAL
#include <string.h>
#include "world.h"

world_layout_t world;
#ifndef __SDCC
uint32_t w_ops[W_OP_COUNT];
#endif
wmod_t  world_mods[MAX_MODS];
uint8_t world_mod_count;
coff_t  world_old_cairns[MAX_OLD_CAIRNS];
uint8_t world_old_cairn_count;

/* ---- metatile flags ------------------------------------------------------------------------ */
#define S_ MTF_SOLID
#define W_ MTF_WATER
#define H_ MTF_HIDE
#define L_ MTF_SLOW
#define C_ MTF_COLD
#define R_ MTF_WARM
#define I_ MTF_INTERACT
#define G_ MTF_GLIDE
const uint8_t mt_flags[MT_COUNT] = {
    S_ | W_ | G_,       /* MT_SEA */
    S_ | W_ | G_,       /* MT_SEA_GLINT */
    S_ | W_ | G_,       /* MT_SHALLOW */
    G_,                 /* MT_STEPSTONE */
    G_,                 /* MT_SAND */
    G_,                 /* MT_GRASS */
    H_ | G_,            /* MT_GRASS_TALL */
    G_,                 /* MT_FLOWERS */
    S_ | G_,            /* MT_TREE */
    L_ | H_ | G_,       /* MT_UNDERGROWTH */
    S_ | C_ | G_,       /* MT_PINE */
    C_ | G_,            /* MT_SNOW */
    G_,                 /* MT_DUNE */
    G_,                 /* MT_BONES */
    S_ | G_,            /* MT_ROCK */
    S_,                 /* MT_ROCK_PEAK (too high to glide over) */
    G_,                 /* MT_ASH */
    G_,                 /* MT_GLASS */
    S_,                 /* MT_MONOLITH (tall) */
    S_ | G_,            /* MT_RUIN_WALL */
    G_,                 /* MT_RUIN_FLOOR */
    S_,                 /* MT_PILLAR (tall) */
    S_,                 /* MT_STATUE_HAND (tall) */
    G_,                 /* MT_ROAD */
    S_ | G_,            /* MT_BRAMBLE */
    S_ | I_ | G_,       /* MT_FIRE_COLD */
    S_ | R_ | I_ | G_,  /* MT_FIRE_LIT (A: sit / rest) */
    S_ | I_ | G_,       /* MT_CAIRN */
    S_ | G_,            /* MT_CAIRN_OLD */
    S_ | I_,            /* MT_BEACON */
    S_ | R_,            /* MT_BEACON_LIT */
    S_ | I_ | G_,       /* MT_SHRINE */
    S_ | G_,            /* MT_SHRINE_EMPTY */
    S_ | I_,            /* MT_HEART */
    S_ | G_,            /* MT_TABLE */
    S_ | G_,            /* MT_WELL */
};
#undef S_
#undef W_
#undef H_
#undef L_
#undef C_
#undef R_
#undef I_
#undef G_

/* ---- hashing ----------------------------------------------------------------------------- */
/* Ken Perlin's reference permutation. */
const uint8_t w_perm[256] = {
    151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,
    23,190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,88,237,149,56,87,
    174,20,125,136,171,168,68,175,74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,
    133,230,220,105,92,41,55,46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,
    89,18,169,200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,250,124,123,5,
    202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,119,
    248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,129,22,39,253,19,98,108,110,79,113,224,232,
    178,185,112,104,218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,
    14,239,107,49,192,214,31,181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,
    93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180
};

uint8_t w_s0, w_s1;
uint8_t w_salt;              /* salt for the next w_hash call (global: read by the asm) */

#define SALT_D  0x5A         /* per-cell detail (other salts: world_gen.c) */

/* h = P[P[P[P[xl ^ s0] ^ yl] ^ xh ^ s1] ^ yh ^ salt] */
#ifdef __SDCC
/* sdcccall(1): x in DE, y in BC, result in A. About 60 M-cycles. */
uint8_t w_hash(uint16_t x, uint16_t y) __naked
{
    (void)x; (void)y;
    __asm
    ld  a, (_w_s0)
    xor a, e
    ld  hl, #_w_perm
    add a, l
    ld  l, a
    adc a, h
    sub a, l
    ld  h, a
    ld  a, (hl)
    xor a, c
    ld  hl, #_w_perm
    add a, l
    ld  l, a
    adc a, h
    sub a, l
    ld  h, a
    ld  a, (hl)
    xor a, d
    ld  hl, #_w_s1
    xor a, (hl)
    ld  hl, #_w_perm
    add a, l
    ld  l, a
    adc a, h
    sub a, l
    ld  h, a
    ld  a, (hl)
    xor a, b
    ld  hl, #_w_salt
    xor a, (hl)
    ld  hl, #_w_perm
    add a, l
    ld  l, a
    adc a, h
    sub a, l
    ld  h, a
    ld  a, (hl)
    ret
    __endasm;
}
#else
uint8_t w_hash(uint16_t x, uint16_t y)
{
    W_OP(W_OP_HASH);
    uint8_t h = w_perm[(uint8_t)((uint8_t)x ^ w_s0)];
    h = w_perm[(uint8_t)(h ^ (uint8_t)y)];
    h = w_perm[(uint8_t)(h ^ (uint8_t)(x >> 8) ^ w_s1)];
    return w_perm[(uint8_t)(h ^ (uint8_t)(y >> 8) ^ w_salt)];
}
#endif

uint8_t w_hash_s(uint16_t x, uint16_t y, uint8_t salt)
{
    w_salt = salt;
    return w_hash(x, y);
}

uint8_t world_detail(uint16_t mx, uint16_t my)
{
    w_salt = SALT_D;
    return w_hash(mx, my);
}

/* ---- small arithmetic primitives -------------------------------------------------------- */
#define HV(a) ((uint8_t)((uint8_t)(a) >> 1))
#define QV(a) ((uint8_t)((uint8_t)(a) >> 2))

uint8_t w_lf, w_ln;          /* w_lerpn: fraction and its number of bits */
static uint8_t cfx, cfy;     /* cellf: the cell's position in its block */
static uint8_t gfld[3];      /* cellf output: elevation, moisture, strangeness */
static uint8_t gc[12];       /* corners of block (gc_x, gc_y): e0..3 m0..3 s0..3 (TL TR BL BR) */
static uint16_t gc_x = 0xFFFF, gc_y = 0xFFFF;
#define ge gfld[0]
#define gm gfld[1]
#define gs gfld[2]

#ifdef __SDCC
/* a + (b - a) * f / 2^n with f = w_lf (n = w_ln bits): bit-serial averaging of halves.
 * sdcccall(1): a in A, b in E. About 15 + 13 n M-cycles. */
uint8_t w_lerpn(uint8_t a, uint8_t b) __naked
{
    (void)a; (void)b;
    __asm
    ld  d, a
    ld  a, (_w_lf)
    or  a, a
    jr  nz, 1$
    ld  a, d
    ret
1$:
    ld  c, a
    ld  a, (_w_ln)
    ld  b, a
    ld  h, d
2$:
    srl c
    ld  a, d
    jr  nc, 3$
    ld  a, e
3$:
    srl a
    ld  l, a
    ld  a, h
    srl a
    add a, l
    ld  h, a
    dec b
    jr  nz, 2$
    ld  a, h
    ret
    __endasm;
}

/* fields of the cell (cfx, cfy) of the block with corners gc -> gfld[3]. ~250 M-cycles.
 * lerp2(a, b, f): f=0: a; 1: a - a/4 + b/4; 2: a/2 + b/2; 3: b - b/4 + a/4 */
static void cellf(void) __naked
{
    __asm
    ld  hl, #_gc
    call 8$
    ld  (_gfld+0), a
    call 8$
    ld  (_gfld+1), a
    call 8$
    ld  (_gfld+2), a
    ret
8$:
    ld  a, (_cfx)
    ld  d, a
    ld  a, (hl+)
    ld  e, (hl)
    inc hl
    call 9$
    ld  b, a
    ld  a, (hl+)
    ld  e, (hl)
    inc hl
    call 9$
    ld  e, a
    ld  a, (_cfy)
    ld  d, a
    ld  a, b
9$:
    ld  c, a
    ld  a, d
    or  a, a
    jr  z, 7$
    cp  a, #2
    jr  z, 6$
    jr  c, 5$
    ld  a, e
    ld  e, c
    ld  c, a
5$:
    ld  a, e
    srl a
    srl a
    ld  e, a
    ld  a, c
    srl a
    srl a
    cpl
    inc a
    add a, c
    add a, e
    ret
6$:
    ld  a, e
    srl a
    ld  e, a
    ld  a, c
    srl a
    add a, e
    ret
7$:
    ld  a, c
    ret
    __endasm;
}
#else
uint8_t w_lerpn(uint8_t a, uint8_t b)
{
    uint8_t v = a, f = w_lf, n = w_ln;
    if (!f) return a;
    do {
        v = (uint8_t)(HV(v) + HV((f & 1) ? b : a));
        f >>= 1;
    } while (--n);
    return v;
}

static uint8_t lerp2(uint8_t a, uint8_t b, uint8_t f)
{
    if (!f) return a;
    if (f == 2) return (uint8_t)(HV(a) + HV(b));
    if (f == 1) return (uint8_t)(a - QV(a) + QV(b));
    return (uint8_t)(b - QV(b) + QV(a));
}

static void cellf(void)
{
    const uint8_t *c = gc;
    uint8_t i;
    for (i = 0; i < 3; i++, c += 4)
        gfld[i] = lerp2(lerp2(c[0], c[1], cfx), lerp2(c[2], c[3], cfx), cfy);
}
#endif

/* ---- biomes ------------------------------------------------------------------------------ */
#define T_SEA      100   /* E below: deep sea */
#define T_SHALLOW  108   /* E below: shallows */
#define T_SHORE    W_T_SHORE   /* E below: shore sand */
#define T_ROCK     176   /* E at/above: rock */
#define T_PEAK     194   /* E at/above: peak */
#define T_ALPINE   158   /* E at/above: tundra (rock if very dry) */
#define T_RUIN     178   /* S at/above: ruins fringe */
#define T_ASH      192   /* S at/above: ash / glass */

/* land biome by moisture (M >> 4) */
static const uint8_t moist_biome[16] = {
    B_DESERT, B_DESERT, B_DESERT, B_DESERT, B_DESERT,
    B_MEADOW, B_MEADOW, B_MEADOW, B_MEADOW,
    B_FOREST, B_FOREST, B_FOREST, B_FOREST, B_FOREST, B_FOREST, B_FOREST
};

/* ground used for clearings / set pieces in each biome */
const uint8_t w_biome_ground[B_COUNT] = {
    MT_SAND, MT_SAND, MT_SAND, MT_GRASS, MT_GRASS, MT_DUNE, MT_SNOW, MT_GRASS, MT_ASH, MT_GRASS
};
const uint8_t w_bitmask[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };

/* underlying biome from the fields (no strangeness overlay) */
uint8_t w_classify_base(uint8_t e, uint8_t m)
{
    if (e < T_SEA) return B_SEA;
    if (e < T_SHALLOW) return B_SHALLOW;
    if (e < T_SHORE) return B_SHORE;
    if (e >= T_ROCK) return B_ROCK;
    if (e >= T_ALPINE) return m >= 48 ? B_TUNDRA : B_ROCK;
    return moist_biome[m >> 4];
}

uint8_t w_classify(uint8_t e, uint8_t m, uint8_t s)
{
    uint8_t b = w_classify_base(e, m);
    if (b >= B_SHORE && b != B_ROCK) {
        if (s >= T_ASH) return B_ASH;
        if (s >= T_RUIN) return B_RUINS;
    }
    return b;
}

static const uint8_t shade[B_COUNT] = { 3, 0, 0, 1, 2, 0, 0, 2, 2, 1 };

/* 0 = light (shore, desert, tundra, shallows), 1 = meadow / ruins, 2 = forest / rock / ash,
 * 3 = sea. Evaluates one lattice point (4-metatile resolution). */
uint8_t world_map_shade(uint16_t mx, uint16_t my)
{
    w_lattice(mx >> 2, my >> 2);
    return shade[w_classify(w_le, w_lm, w_ls)];
}

/* ---- terrain for one cell ---------------------------------------------------------------- */
static uint8_t gd;                                /* detail hash of the cell */

static uint8_t terrain(uint8_t b)
{
    uint8_t d = gd;
    switch (b) {
    case B_SEA:
        return d < 5 ? MT_SEA_GLINT : MT_SEA;
    case B_SHALLOW:
        return MT_SHALLOW;
    case B_SHORE:
        return d < 2 ? MT_BONES : MT_SAND;
    case B_MEADOW:
        if (d < 4) return MT_TREE;
        if (gm >= 150 && d < 24) return MT_TREE;                               /* forest edge */
        if ((uint8_t)((ge + gm) & 0x3F) < 10 && d < 220) return MT_GRASS_TALL;  /* swaths */
        if ((uint8_t)(ge & 0x0F) < 3 && d < 120) return MT_FLOWERS;
        return MT_GRASS;
    case B_FOREST:
        if ((uint8_t)((ge + HV(gs)) & 0x1F) < 3) return MT_UNDERGROWTH;         /* winding paths */
        if ((uint8_t)(gs & 0x3F) < 5) return d < 40 ? MT_UNDERGROWTH : MT_GRASS;   /* glades */
        return d < 64 ? MT_UNDERGROWTH : MT_TREE;
    case B_DESERT:
        if (d < 3) return MT_BONES;
        return ((uint8_t)(ge + HV(gm)) & 0x18) ? MT_DUNE : MT_SAND;
    case B_TUNDRA:
        if (d < 12 || ((ge & 0x10) && d < 70)) return MT_PINE;
        return MT_SNOW;
    case B_ROCK:
        return ge >= T_PEAK ? MT_ROCK_PEAK : MT_ROCK;
    default:  /* B_ASH */
        if (d < 2) return MT_MONOLITH;
        if (d < 12 || ((uint8_t)(ge + gm) & 0x1C) == 0) return MT_GLASS;
        return MT_ASH;
    }
}

/* ---- state shared with world_gen.c ------------------------------------------------------- */
uint16_t w_pq_x = 0xFFFF, w_pq_y = 0xFFFF;        /* current POI cell key (m & ~15) */
uint8_t  w_pq_type, w_pq_px, w_pq_py, w_pq_ok, w_pq_ground, w_pq_axis;
w_road_t w_roads[W_NUM_ROADS];
uint8_t  w_road_x[W_ROAD_POOL];
uint8_t  w_ready;
uint8_t  w_start_ground;
uint16_t w_spx0, w_spy0;
uint8_t  w_sp_mask[64];
uint8_t  w_le, w_lm, w_ls;
uint8_t  w_mod_head[W_MOD_BUCKETS];
uint8_t  w_mod_next[MAX_MODS];
uint8_t  w_mods_seen;

static uint8_t fold(uint8_t v, uint8_t c)
{
    return v >= c ? (uint8_t)(v - c) : (uint8_t)(c - v);
}

/* ---- the block cache --------------------------------------------------------------------- */
/* 8 blocks of 4x4 metatiles, slot = (bx + 5 * by) & 7: a row or a column of blocks and their
 * neighbours land in different slots. Keys are mx & ~3, my & ~3 (0xFFFF: empty). Each slot
 * has the block's lattice corners (once known) and the cells computed so far (with mods). */
uint16_t w_bcx[W_BC_N], w_bcy[W_BC_N];
uint8_t  w_bcm[W_BC_N][16];                       /* metatiles, index (fy << 2) | fx */
uint8_t  w_bcv[W_BC_N][2];                        /* bit i: cell i computed */
static uint8_t *ckm, *ckv;                        /* current block's metatiles / valid bits */
static uint16_t ck_x = 0xFFFF, ck_y = 0xFFFF;     /* current block */
static uint8_t ckslot;

uint8_t w_bslot(uint16_t kx, uint16_t ky)
{
    uint8_t y = (uint8_t)((uint8_t)ky >> 2);
    return (uint8_t)(((uint8_t)((uint8_t)kx >> 2) + (uint8_t)(y << 2) + y) & (W_BC_N - 1));
}

void w_blocks_reset(void)
{
    uint8_t i;
    for (i = 0; i < W_BC_N; i++) w_bcx[i] = 0xFFFF;
    ck_x = ck_y = 0xFFFF;
}

static uint16_t spk_x = 0xFFFF, spk_y = 0xFFFF;   /* last set-piece filter cell (m & ~127) */
static uint8_t spk_on;                            /* ... its W_SPM_* mask */

static void lp_reset(void);
void w_reset(void)
{
    lp_reset();
    w_lattice_reset();
    w_pq_x = w_pq_y = 0xFFFF;
    spk_x = spk_y = 0xFFFF;
    w_blocks_reset();
}


/* Lattice point cache: 32 entries, slot (lx + 5 ly) & 31 (a 5 x 6 window never collides).
 * Holds the corners of the cached blocks and the points computed ahead by world_prefetch. */
#define LP_N 32
static uint16_t lpx[LP_N], lpy[LP_N];
static uint8_t lpv[LP_N][3];

static uint8_t lp_slot(uint16_t lx, uint16_t ly)
{
    uint8_t y = (uint8_t)ly;
    return (uint8_t)(((uint8_t)lx + (uint8_t)(y << 2) + y) & (LP_N - 1));
}

static void lp_reset(void)
{
    uint8_t i;
    for (i = 0; i < LP_N; i++) lpx[i] = 0xFFFF;
    gc_x = gc_y = 0xFFFF;
}

/* lattice point (lx, ly) into d[0], d[4], d[8]; computes it if needed (0: cached, 1: computed) */
static uint8_t lp_get(uint16_t lx, uint16_t ly, uint8_t *d)
{
    uint8_t j = lp_slot(lx, ly), r = 0;
    uint8_t *v = lpv[j];
    if (lpx[j] != lx || lpy[j] != ly) {
        w_lattice(lx, ly);
        v[0] = w_le;
        v[1] = w_lm;
        v[2] = w_ls;
        lpx[j] = lx;
        lpy[j] = ly;
        r = 1;
    }
    d[0] = v[0];
    d[4] = v[1];
    d[8] = v[2];
    return r;
}

/* lattice corners of the current block (ck_x, ck_y) into gc */
static void corners(void)
{
    uint16_t lx = ck_x >> 2, ly = ck_y >> 2;
    if (gc_x == ck_x && gc_y == ck_y) return;
    gc_x = ck_x;
    gc_y = ck_y;
    lp_get(lx, ly, gc);
    lp_get((uint16_t)(lx + 1), ly, gc + 1);
    lp_get(lx, (uint16_t)(ly + 1), gc + 2);
    lp_get((uint16_t)(lx + 1), (uint16_t)(ly + 1), gc + 3);
}

static uint8_t pf_tmp[12];

/* Warm the caches for the 4x4 block holding (mx, my): computes at most one missing lattice
 * corner (~1500-2500 M-cycles). Returns 1 if it computed one (call again), 0 if the block's
 * corners are all cached. Results never depend on it: it only moves work to a quiet frame. */
uint8_t world_prefetch(uint16_t mx, uint16_t my)
{
    uint16_t lx = mx >> 2, ly = my >> 2;
    uint8_t i;
    for (i = 0; i < 4; i++)
        if (lp_get((uint16_t)(lx + (i & 1)), (uint16_t)(ly + (i >> 1)), pf_tmp)) return 1;
    return 0;
}

/* make block (kx, ky) current (corners are computed on first use) */
static void block_get(uint16_t kx, uint16_t ky)
{
    uint8_t s = w_bslot(kx, ky);
    if (w_mods_seen != world_mod_count) world_mods_rebuild();   /* mods written directly */
    ck_x = kx;
    ck_y = ky;
    ckslot = s;
    ckm = w_bcm[s];
    ckv = w_bcv[s];
    if (w_bcx[s] != kx || w_bcy[s] != ky) {
        W_OP(W_OP_BLOCK);
        w_bcx[s] = kx;
        w_bcy[s] = ky;
        ckv[0] = ckv[1] = 0;
    }
}

/* POI tile for cell (lx, ly) in its 16x16 cell (lattice-aligned POI; validated from the
 * block corner it sits on), or 0xFF */
static uint8_t poi_cell(uint8_t lx, uint8_t ly, uint8_t b)
{
    uint8_t ax, ay, i;
    ax = fold(lx, w_pq_px);
    ay = fold(ly, w_pq_py);
    const uint8_t *c;
    if (w_pq_type == W_POI_ROAD) {
        if (ax && ay) return 0xFF;
        if (b < B_SHORE || b == B_ROCK) return 0xFF;   /* the road only runs over land */
    } else if (ax > 2 || ay > 2) return 0xFF;
    if (!w_pq_ok) {
        if (w_pq_type == W_POI_ROAD) w_poi_check();
        else {
            /* the POI is on a lattice point: a corner of this block */
            w_pq_ok = 1;
            i = (uint8_t)((lx < w_pq_px ? 1 : 0) | (ly < w_pq_py ? 2 : 0));
            c = gc + i;
            i = w_classify(c[0], c[4], c[8]);
            w_pq_ground = w_biome_ground[i];
            if (i <= B_SHORE || i == B_ROCK || c[0] < T_SHORE + 6) w_pq_type = W_POI_NONE;
            else if (w_pq_type == W_POI_MONOLITH && i == B_ASH) w_pq_ground = MT_GLASS;
        }
        if (w_pq_type == W_POI_NONE) return 0xFF;
    }
    return w_poi_mt(ax, ay, gd);
}

/* the POI roll of a 16x16 cell (key m & ~15) */
#define SALT_P  0xA3
#define SALT_P2 0x17
static const uint8_t poi_pos[4] = { 4, 8, 8, 12 };   /* lattice-aligned, away from the edges */
static void poi_roll(uint16_t kx, uint16_t ky)
{
    uint8_t h, h2;
    W_OP(W_OP_POI_ROLL);
    uint16_t cx = kx >> 4, cy = ky >> 4;
    w_pq_x = kx;
    w_pq_y = ky;
    w_pq_ok = 0;
    w_salt = SALT_P;
    h = w_hash(cx, cy);
    w_salt = SALT_P2;
    h2 = w_hash(cx, cy);
    w_pq_px = poi_pos[h2 & 3];
    w_pq_py = poi_pos[(h2 >> 2) & 3];
    w_pq_axis = (uint8_t)((h2 >> 4) & 1);
    if (h < 86) h = W_POI_FIRE;            /* ~1 in 3 cells: a cold campfire */
    else if (h < 100) h = W_POI_MONOLITH;
    else if (h < 105) h = W_POI_TABLE;     /* a table set for two */
    else if (h < 110) h = W_POI_WELL;
    else if (h < 114) h = W_POI_HAND;      /* a giant stone hand */
    else if (h < 120) h = W_POI_ROAD;      /* a road ending in the sea */
    else h = W_POI_NONE;
    w_pq_type = h;
}

static uint8_t mod_find(uint16_t mx, uint16_t my)
{
    uint8_t i = w_mod_head[w_mod_bucket(mx, my)];
    W_OP(W_OP_MOD_FIND);
    while (i != W_MOD_NONE) {
        if (world_mods[i].x == mx && world_mods[i].y == my) return i;
        i = w_mod_next[i];
    }
    return W_MOD_NONE;
}

/* compute one cell of the current block (without mods) */
static uint8_t cell(uint16_t mx, uint16_t my)
{
    uint8_t b, t, p = W_SP_NONE;
    W_OP(W_OP_CELL);
    corners();
    cfx = (uint8_t)mx & 3;
    cfy = (uint8_t)my & 3;
    cellf();
    w_salt = SALT_D;
    gd = w_hash(mx, my);
    /* set pieces: coarse filter per 64x64 cell, then the banked per-cell test */
    if (w_ready) {
        if ((mx & 0xFF80) != spk_x || (my & 0xFF80) != spk_y) {
            uint16_t cx = (uint16_t)(mx - w_spx0), cy = (uint16_t)(my - w_spy0);
            spk_x = mx & 0xFF80;
            spk_y = my & 0xFF80;
            spk_on = 0;
            if (cx < 1024 && cy < 1024)
                spk_on = w_sp_mask[(uint8_t)(((uint8_t)(cy >> 7) << 3) | (uint8_t)(cx >> 7))];
        }
        if (spk_on) {
            p = w_piece(mx, my, spk_on);
            if (p != W_SP_NONE && p != W_SP_CLEAR) return p;
        }
    }
    /* classify */
    b = w_classify_base(ge, gm);
    if (b >= B_SHORE && b != B_ROCK) {
        if (gs >= T_ASH) b = B_ASH;
        else if (gs >= T_RUIN) b = B_RUINS;
    }
    /* the POI of this 16x16 cell */
    t = 0xFF;
    if ((mx & 0xFFF0) != w_pq_x || (my & 0xFFF0) != w_pq_y) poi_roll(mx & 0xFFF0, my & 0xFFF0);
    if (w_pq_type != W_POI_NONE) t = poi_cell((uint8_t)mx & 15, (uint8_t)my & 15, b);
    if (t == 0xFF) {
        if (b == B_RUINS) {
            t = w_classify_base(ge, gm);
            t = terrain(t);
            t = w_ruin(mx, my, gd, t);
        }
        else t = terrain(b);
        if (world_old_cairn_count && b >= B_SHORE && b != B_ROCK && w_ready &&
            !(((uint8_t)((uint8_t)mx - (uint8_t)world.start.x) | (uint8_t)((uint8_t)my - (uint8_t)world.start.y)) & 3) &&
            w_old_cairn(mx, my))
            t = MT_CAIRN_OLD;
    }
    if (p == W_SP_CLEAR && (mt_flags[t] & MTF_SOLID)) t = w_start_ground;
    return t;
}

uint8_t world_mt(uint16_t mx, uint16_t my)
{
    uint8_t i, t, *v, m;
    W_OP(W_OP_MT);
    if ((mx & 0xFFFC) != ck_x || (my & 0xFFFC) != ck_y || w_mods_seen != world_mod_count)
        block_get(mx & 0xFFFC, my & 0xFFFC);
    i = (uint8_t)((((uint8_t)my & 3) << 2) | ((uint8_t)mx & 3));
    v = ckv + (i >> 3);
    m = w_bitmask[i & 7];
    if (*v & m) { W_OP(W_OP_HIT); return ckm[i]; }
    t = world_mod_count ? mod_find(mx, my) : W_MOD_NONE;
    t = t != W_MOD_NONE ? world_mods[t].mt : cell(mx, my);
    ckm[i] = t;
    *v |= m;
    return t;
}

/* generated metatile, ignoring mods */
uint8_t world_mt_base(uint16_t mx, uint16_t my)
{
    if (!world_mod_count || mod_find(mx, my) == W_MOD_NONE) return world_mt(mx, my);
    if ((mx & 0xFFFC) != ck_x || (my & 0xFFFC) != ck_y) block_get(mx & 0xFFFC, my & 0xFFFC);
    return cell(mx, my);
}

/* biome at the nearest lattice point (within 2 metatiles; cheap: uses the block corners) */
uint8_t world_biome(uint16_t mx, uint16_t my)
{
    const uint8_t *c;
    uint8_t i = (uint8_t)((((uint8_t)mx >> 1) & 1) | ((uint8_t)my & 2));
    if ((mx & 0xFFFC) != ck_x || (my & 0xFFFC) != ck_y || w_mods_seen != world_mod_count)
        block_get(mx & 0xFFFC, my & 0xFFFC);
    corners();
    c = gc + i;
    return w_classify(c[0], c[4], c[8]);
}
