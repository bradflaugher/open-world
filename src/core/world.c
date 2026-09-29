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
#if !defined(__SDCC) || defined(WORLD_OPCOUNT)
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
static uint8_t *gcp;         /* corners of the current block: e0..3 m0..3 s0..3 (TL TR BL BR) */
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
    ld  hl, #_gcp
    ld  a, (hl+)
    ld  h, (hl)
    ld  l, a
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
    const uint8_t *c = gcp;
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
 * 3 = sea, at 8-metatile resolution (the nearest point of an 8-metatile grid; see w_map_point).
 * Cheapest when the map is scanned in rows. */
static uint16_t ms_x = 0xFFFF, ms_y = 0xFFFF;
static uint16_t rsh_x = 0xFFFF, rsh_y;          /* road_shows: last 16x16 cell and its answer */
static uint8_t rsh_v;
static uint8_t ms_v;
uint8_t world_map_shade(uint16_t mx, uint16_t my)
{
    uint16_t px = (uint16_t)(mx + 4) >> 3, py = (uint16_t)(my + 4) >> 3;
    if (px != ms_x || py != ms_y) {
        ms_x = px;
        ms_y = py;
        w_map_point(px, py);
        ms_v = shade[w_classify(w_le, w_lm, w_ls)];
    }
    return ms_v;
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
static uint16_t pq2_x = 0xFFFF, pq2_y;            /* the previous POI cell, kept for a swap */
static uint8_t pq2_type, pq2_px, pq2_py, pq2_ok, pq2_ground, pq2_axis;
w_road_t w_roads[W_NUM_ROADS];
uint8_t  w_ready;
uint8_t  w_start_ground;
uint16_t w_spx0, w_spy0;
uint8_t  w_sp_mask[64];
uint8_t  w_le, w_lm, w_ls;
uint8_t  w_mod_head[W_MOD_BUCKETS];
uint8_t  w_mod_next[MAX_MODS];
uint8_t  w_mods_seen;

static uint8_t hfold(uint8_t v, uint8_t c)
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
static uint8_t bcc[W_BC_N][12];                   /* corners of each block (once known) */
static uint8_t bck[W_BC_N];                       /* block initialised: corners, bpm, bpn */
static uint8_t bpm[W_BC_N];                       /* set pieces touching the block (W_SPM_*) */
static uint8_t bpn[W_BC_N];                       /* the block's POI can reach into it */
static uint8_t brm[W_BC_N][4];                    /* causeway cells, road-side fires (bit per cell) */
static uint8_t cur_bck, cur_bpm, cur_bpn, *ckr;    /* the current block's bck, bpm, bpn, brm */
static uint8_t *ckm, *ckv;                        /* current block's metatiles / valid bits */
static uint16_t ck_x = 0xFFFF, ck_y = 0xFFFF;     /* current block */
static uint8_t ckslot;

uint8_t w_bslot(uint16_t kx, uint16_t ky)
{
    uint8_t y = (uint8_t)((uint8_t)ky >> 2);
    return (uint8_t)(((uint8_t)((uint8_t)kx >> 2) + (uint8_t)(y << 2) + y) & (W_BC_N - 1));
}

static uint16_t bix[W_BC_N], biy[W_BC_N];
static void binfo(uint16_t kx, uint16_t ky);
void w_blocks_reset(void)
{
    uint8_t i;
    for (i = 0; i < W_BC_N; i++) w_bcx[i] = 0xFFFF, bix[i] = 0xFFFF;
    ck_x = ck_y = 0xFFFF;
}


static void lp_reset(void);
void w_reset(void)
{
    ms_x = 0xFFFF;
    rsh_x = 0xFFFF;
    lp_reset();
    w_lattice_reset();
    w_pq_x = w_pq_y = 0xFFFF;
    pq2_x = 0xFFFF;
    w_blocks_reset();
}


/* Lattice point cache: 64 entries, slot ((ly & 7) << 3) | (lx & 7): any 8x8 window of lattice
 * points (32x32 metatiles: the view, its streaming ring and a prefetch margin) never collides.
 * Tags hold bits 3..10 of lx, ly; bits 11+ are a global region (offset by 1024 lattice points so
 * that the region edges are far from the start), and the cache is flushed when it changes. */
#define LP_N 64
static uint8_t lptx[LP_N], lpty[LP_N], lpv[LP_N][3];
static uint8_t lpok[LP_N / 8];                    /* valid bits (every tag value is possible) */
static uint8_t lp_rx = 0xFF, lp_ry = 0xFF;

static void lp_reset(void)
{
    uint8_t i;
    for (i = 0; i < LP_N / 8; i++) lpok[i] = 0;
    lp_rx = lp_ry = 0xFF;
}

/* lattice point (lx, ly) into d[0], d[4], d[8]; computes it if needed (0: cached, 1: computed) */
static uint8_t lp_get(uint16_t lx, uint16_t ly, uint8_t *d)
{
    uint8_t j, tx, ty, r = 0, *v;
    tx = (uint8_t)((uint16_t)(lx + 1024) >> 11);
    ty = (uint8_t)((uint16_t)(ly + 1024) >> 11);
    if (tx != lp_rx || ty != lp_ry) {                /* another region: flush */
        lp_reset();
        lp_rx = tx;
        lp_ry = ty;
    }
    j = (uint8_t)((((uint8_t)ly & 7) << 3) | ((uint8_t)lx & 7));
    tx = (uint8_t)(lx >> 3);
    ty = (uint8_t)(ly >> 3);
    v = lpv[j];
    if (lptx[j] != tx || lpty[j] != ty || !(lpok[j >> 3] & w_bitmask[j & 7])) {
        w_lattice(lx, ly);
        v[0] = w_le;
        v[1] = w_lm;
        v[2] = w_ls;
        lptx[j] = tx;
        lpty[j] = ty;
        lpok[j >> 3] |= w_bitmask[j & 7];
        r = 1;
    }
    d[0] = v[0];
    d[4] = v[1];
    d[8] = v[2];
    return r;
}

/* lattice corners of the current block (ck_x, ck_y) into its slot */
static void block_init(void);
static void corners(void)
{
    uint16_t lx = ck_x >> 2, ly = ck_y >> 2;
    uint8_t i, j, x, y, tx, ty;
    const uint8_t *v;
    if (cur_bck) return;
    block_init();
    /* fast path: all four points cached, same region */
    x = (uint8_t)lx;
    y = (uint8_t)ly;
    if ((uint8_t)((uint16_t)(lx + 1024) >> 11) == lp_rx && (uint8_t)((uint16_t)(ly + 1024) >> 11) == lp_ry &&
        ((uint8_t)(lx + 1024) & 0xFF) != 0xFF && ((uint8_t)(ly + 1024) & 0xFF) != 0xFF) {
        for (i = 0; i < 4; i++) {
            tx = (uint8_t)((uint16_t)(lx + (i & 1)) >> 3);
            ty = (uint8_t)((uint16_t)(ly + (i >> 1)) >> 3);
            j = (uint8_t)(((uint8_t)((y + (i >> 1)) & 7) << 3) | ((uint8_t)(x + (i & 1)) & 7));
            if (lptx[j] != tx || lpty[j] != ty || !(lpok[j >> 3] & w_bitmask[j & 7])) break;
            v = lpv[j];
            gcp[i] = v[0];
            gcp[i + 4] = v[1];
            gcp[i + 8] = v[2];
        }
        if (i == 4) return;
    }
    lp_get(lx, ly, gcp);
    lp_get((uint16_t)(lx + 1), ly, gcp + 1);
    lp_get(lx, (uint16_t)(ly + 1), gcp + 2);
    lp_get((uint16_t)(lx + 1), (uint16_t)(ly + 1), gcp + 3);
}

static uint8_t pf_tmp[12];

/* Warm the caches for the 4x4 block holding (mx, my): computes at most one missing lattice
 * corner (~1500-2500 M-cycles). Returns 1 if it computed one (call again), 0 if the block's
 * corners are all cached. Results never depend on it: it only moves work to a quiet frame. */
uint8_t world_prefetch(uint16_t mx, uint16_t my)
{
    uint16_t lx = mx >> 2, ly = my >> 2, kx = mx & 0xFFFC, ky = my & 0xFFFC;
    uint8_t i;
    for (i = 0; i < 4; i++)
        if (lp_get((uint16_t)(lx + (i & 1)), (uint16_t)(ly + (i >> 1)), pf_tmp)) return 1;
    i = w_bslot(kx, ky);
    if (bix[i] == kx && biy[i] == ky) return 0;
    binfo(kx, ky);
    return 1;
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
    gcp = bcc[s];
    ckv = w_bcv[s];
    ckr = brm[s];
    if (w_bcx[s] != kx || w_bcy[s] != ky) {
        W_OP(W_OP_BLOCK);
        w_bcx[s] = kx;
        w_bcy[s] = ky;
        ckv[0] = ckv[1] = 0;
        bck[s] = 0;
    }
    cur_bck = bck[s];
    cur_bpm = bpm[s];
    cur_bpn = bpn[s];
}

/* POI tile for cell (lx, ly) in its 16x16 cell (lattice-aligned POI; validated from the
 * block corner it sits on), or 0xFF */
static uint8_t poi_cell(uint8_t lx, uint8_t ly, uint8_t b)
{
    uint8_t ax, ay;
    ax = hfold(lx, w_pq_px);
    ay = hfold(ly, w_pq_py);
    if (w_pq_type == W_POI_ROAD) {
        if (ax && ay) return 0xFF;
        if (b < B_SHORE || b == B_ROCK) return 0xFF;   /* the road only runs over land */
    } else if (ax > 2 || ay > 2) return 0xFF;
    if (!w_pq_ok) {
        w_poi_check(lx, ly, gcp);   /* banked: once per POI */
        if (w_pq_type == W_POI_NONE) return 0xFF;
    }
    return w_poi_mt(ax, ay, gd);
}

/* the POI roll of a 16x16 cell (key m & ~15) */
void w_poi_roll(uint16_t kx, uint16_t ky)
{
    uint8_t h;
    uint16_t sx = w_pq_x, sy = w_pq_y;
    /* swap with the previous cell (a column or row of blocks alternates between two cells) */
#define PQ_SWAP(a, b) (h = a, a = b, b = h)
    PQ_SWAP(w_pq_type, pq2_type); PQ_SWAP(w_pq_px, pq2_px); PQ_SWAP(w_pq_py, pq2_py);
    PQ_SWAP(w_pq_ok, pq2_ok); PQ_SWAP(w_pq_ground, pq2_ground); PQ_SWAP(w_pq_axis, pq2_axis);
#undef PQ_SWAP
    w_pq_x = pq2_x;
    w_pq_y = pq2_y;
    pq2_x = sx;
    pq2_y = sy;
    if (w_pq_x == kx && w_pq_y == ky) return;
    w_poi_gen(kx, ky);   /* banked: roll the cell */
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

/* Per-block data: which set pieces touch the block (bpm), its causeway cells (brm) and whether
 * its POI can reach into it (bpn). Computed once per block into a small cache of its own, so
 * world_prefetch can do it ahead of time (near the start, with 4 causeways around, it is the
 * most expensive part of entering a new block). */
static uint8_t bim[W_BC_N], bin_[W_BC_N], bir[W_BC_N][4];
/* w_binfo_* are declared in world.h */
uint8_t w_binfo_m, w_binfo_n, w_binfo_r[4];

static void binfo(uint16_t kx, uint16_t ky)
{
    uint8_t s = w_bslot(kx, ky);
    if (bix[s] == kx && biy[s] == ky) {
        w_binfo_m = bim[s];
        w_binfo_n = bin_[s];
        memcpy(w_binfo_r, bir[s], 4);
        return;
    }
    w_binfo(kx, ky);   /* banked: set pieces, causeway cells, POI reach */
    bix[s] = kx;
    biy[s] = ky;
    bim[s] = w_binfo_m;
    bin_[s] = w_binfo_n;
    memcpy(bir[s], w_binfo_r, 4);
}

static void block_init(void)
{
    bck[ckslot] = cur_bck = 1;
    binfo(ck_x, ck_y);
    memcpy(ckr, w_binfo_r, 4);
    bpm[ckslot] = cur_bpm = w_binfo_m;
    bpn[ckslot] = cur_bpn = w_binfo_n;
}

/* the visible stretches of a causeway on open land: about 30% of it, in pieces of a 16x16 cell */
#define SALT_W 0xD2
static uint8_t road_shows(uint16_t mx, uint16_t my)
{
    if ((mx & 0xFFF0) != rsh_x || (my & 0xFFF0) != rsh_y) {
        rsh_x = mx & 0xFFF0;
        rsh_y = my & 0xFFF0;
        w_salt = SALT_W;
        rsh_v = w_hash(mx >> 4, my >> 4) < 80;
    }
    return rsh_v;
}

/* compute one cell of the current block (without mods) */
static uint8_t cell(uint16_t mx, uint16_t my)
{
    uint8_t b, t, p = W_SP_NONE, road;
    W_OP(W_OP_CELL);
    if (!cur_bck) corners();
    cfx = (uint8_t)mx & 3;
    cfy = (uint8_t)my & 3;
    cellf();
    w_salt = SALT_D;
    gd = w_hash(mx, my);
    /* set pieces: beacons, the Heart, the start fire, then causeways, then the start clearing */
    if (cur_bpm) {
        p = w_piece(mx, my, cur_bpm);
        if (p != W_SP_NONE && p != W_SP_CLEAR) return p;
    }
    road = (uint8_t)((cfy << 2) | cfx);
    road = (uint8_t)(((ckr[road >> 3] & w_bitmask[road & 7]) ? 1 : 0) | ((ckr[2 + (road >> 3)] & w_bitmask[road & 7]) ? 2 : 0));
    /* classify */
    b = w_classify_base(ge, gm);
    if (b >= B_SHORE && b != B_ROCK) {
        if (gs >= T_ASH) b = B_ASH;
        else if (gs >= T_RUIN) b = B_RUINS;
    }
    /* the POI of this 16x16 cell */
    t = 0xFF;
    if (cur_bpn) {
        if ((mx & 0xFFF0) != w_pq_x || (my & 0xFFF0) != w_pq_y) w_poi_roll(mx & 0xFFF0, my & 0xFFF0);
        if (w_pq_type != W_POI_NONE) t = poi_cell((uint8_t)mx & 15, (uint8_t)my & 15, b);
    }
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
    if (road) {
        if (road & 1) {
            /* a causeway: a bridge or a pass wherever the land is impassable (so every set
             * piece stays reachable), elsewhere only fragments of an old buried road */
            if ((mt_flags[t] & MTF_SOLID) || road_shows(mx, my)) t = gd < 24 ? MT_RUIN_FLOOR : MT_ROAD;
        } else t = MT_FIRE_COLD;   /* a road-side fire (over water it stands on the causeway's edge) */
    }
    if (p == W_SP_CLEAR && (mt_flags[t] & MTF_SOLID)) t = w_start_ground;
    return t;
}

/* slow path: another block, a cell not computed yet, or the mods changed */
uint8_t world_mt_slow(uint16_t mx, uint16_t my);
uint8_t world_mt_slow(uint16_t mx, uint16_t my)
{
    uint8_t i, t, *v, m;
    if ((mx & 0xFFFC) != ck_x || (my & 0xFFFC) != ck_y || w_mods_seen != world_mod_count)
        block_get(mx & 0xFFFC, my & 0xFFFC);
    i = (uint8_t)((((uint8_t)my & 3) << 2) | ((uint8_t)mx & 3));
    v = ckv + (i >> 3);
    m = w_bitmask[i & 7];
    if (*v & m) return ckm[i];
    t = world_mod_count ? mod_find(mx, my) : W_MOD_NONE;
    t = t != W_MOD_NONE ? world_mods[t].mt : cell(mx, my);
    ckm[i] = t;
    *v |= m;
    return t;
}

#ifdef __SDCC
/* fast path in assembly (~80 M-cycles): a computed cell of the current block.
 * sdcccall(1): mx in DE, my in BC, result in A; falls through to world_mt_slow otherwise. */
uint8_t world_mt(uint16_t mx, uint16_t my) __naked
{
    (void)mx; (void)my;
    __asm
    ld  a, e
    and a, #0xFC
    ld  hl, #_ck_x
    cp  a, (hl)
    jr  nz, 9$
    inc hl
    ld  a, d
    cp  a, (hl)
    jr  nz, 9$
    ld  a, c
    and a, #0xFC
    ld  hl, #_ck_y
    cp  a, (hl)
    jr  nz, 9$
    inc hl
    ld  a, b
    cp  a, (hl)
    jr  nz, 9$
    ld  a, (_w_mods_seen)
    ld  hl, #_world_mod_count
    cp  a, (hl)
    jr  nz, 9$
    ld  a, c
    and a, #3
    add a, a
    add a, a
    ld  l, a
    ld  a, e
    and a, #3
    or  a, l
    ld  h, a
    and a, #7
    ld  l, a
    ld  a, #1
    inc l
    jr  2$
1$:
    add a, a
2$:
    dec l
    jr  nz, 1$
    ld  l, a
    push de
    ld  a, (_ckv)
    ld  e, a
    ld  a, (_ckv + 1)
    ld  d, a
    bit 3, h
    jr  z, 3$
    inc de
3$:
    ld  a, (de)
    pop de
    and a, l
    jr  z, 9$
    ld  a, (_ckm)
    add a, h
    ld  l, a
    ld  a, (_ckm + 1)
    adc a, #0
    ld  h, a
    ld  a, (hl)
    ret
9$:
    jp  _world_mt_slow
    __endasm;
}
#else
uint8_t world_mt(uint16_t mx, uint16_t my)
{
    W_OP(W_OP_MT);
    return world_mt_slow(mx, my);
}
#endif

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
    c = gcp + i;
    return w_classify(c[0], c[4], c[8]);
}
