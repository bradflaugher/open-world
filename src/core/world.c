/* world.c - OPEN WORLD world core, HOT PATH: noise, biomes, metatiles, set-piece lookup, mods.
 *
 * Portable C: compiles with SDCC (Game Boy, SM83) and gcc (host tests / owgen).
 * Not banked: keep this file in bank 0 (the engine calls world_mt while scrolling). The cold
 * half (world_init, layout, causeways, bearing / distance) is world_gen.c, banked.
 *
 * Performance notes (SM83):
 *  - No multiply / divide / modulo on variables here.
 *  - Metatiles are generated a 4x4 block at a time and kept in an 8-block cache, so world_mt
 *    is mostly a lookup; a scrolling engine uses every cell of a block before it is evicted.
 *  - The noise fields are evaluated on a 4-metatile lattice (the block corners) and bilinearly
 *    interpolated inside the block with 2-bit weights (shifts and adds only). Bilinear-of-
 *    bilinear is exact, so this equals interpolating each octave on its own grid. A new block
 *    copies shared corners from its cached neighbours: about one lattice point per new block.
 *  - Rare work (POI checks, ruins, set pieces, old cairns) is in world_gen.c (banked), called
 *    at most once per block or per ruin cell.
 *  - All caches are pure: results never depend on the call order.
 */
#define WORLD_INTERNAL
#include <string.h>
#include "world.h"

world_layout_t world;
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

/* ---- biomes ------------------------------------------------------------------------------ */
#define HV(a) ((uint8_t)((uint8_t)(a) >> 1))
#define QV(a) ((uint8_t)((uint8_t)(a) >> 2))

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
 * 3 = sea. Evaluates one lattice point (4-metatile resolution): ~2000 M-cycles. */
uint8_t world_map_shade(uint16_t mx, uint16_t my)
{
    w_lattice(mx >> 2, my >> 2);
    return shade[w_classify(w_le, w_lm, w_ls)];
}

/* ---- terrain for one cell ---------------------------------------------------------------- */
/* inputs in globals (SDCC addresses globals directly; argument passing is expensive) */
static uint8_t ge, gm, gs, gd;                    /* elevation, moisture, strangeness, detail hash */

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
uint16_t w_pq_x = 0xFFFF, w_pq_y = 0xFFFF;        /* current POI cell (cx, cy) */
uint8_t  w_pq_type, w_pq_px, w_pq_py, w_pq_ok, w_pq_ground, w_pq_axis;
w_road_t w_roads[W_NUM_ROADS];
uint8_t  w_road_x[W_ROAD_POOL];
uint8_t  w_ready;
uint8_t  w_start_ground;
uint16_t w_spx0, w_spy0;
uint8_t  w_sp_bits[32];
uint8_t  w_le, w_lm, w_ls;

static uint8_t fold(uint8_t v, uint8_t c)
{
    return v >= c ? (uint8_t)(v - c) : (uint8_t)(c - v);
}

/* ---- the block cache --------------------------------------------------------------------- */
/* 8 blocks of 4x4 metatiles, slot = (bx + 5 * by) & 7: a row or a column of blocks and their
 * neighbours land in different slots. Keys are mx & ~3, my & ~3 (0xFFFF: empty). The cached
 * metatiles include the mods (world_mod_set patches cached cells). */
uint16_t w_bcx[W_BC_N], w_bcy[W_BC_N];
uint8_t  w_bcm[W_BC_N][16];                       /* metatiles, index (fy << 2) | fx */
static uint8_t bcc[W_BC_N][12];                   /* lattice corners TL TR BL BR: e0..3 m0..3 s0..3 */
static uint16_t ck_x = 0xFFFF, ck_y = 0xFFFF;     /* last block used */
static uint8_t *ckm;                              /* ... its metatiles */
static uint8_t ckslot;
uint8_t  w_mod_bloom[16];                         /* 128-bit filter over blocks holding a mod */
uint8_t  w_mods_seen;                             /* world_mod_count when the filter was built */
uint8_t  w_mods_off;                              /* build blocks without mods (world_mt_base) */

uint8_t w_bslot(uint16_t kx, uint16_t ky)
{
    uint8_t y = (uint8_t)((uint8_t)ky >> 2);
    return (uint8_t)(((uint8_t)((uint8_t)kx >> 2) + (uint8_t)(y << 2) + y) & (W_BC_N - 1));
}

uint8_t w_mod_bit(uint16_t kx, uint16_t ky)
{
    uint8_t y = (uint8_t)((uint8_t)ky >> 2);
    return (uint8_t)(((uint8_t)((uint8_t)kx >> 2) ^ (uint8_t)(y << 3) ^ (uint8_t)(y >> 3) ^ (uint8_t)(ky >> 8)) & 127);
}

void w_blocks_reset(void)
{
    uint8_t i;
    for (i = 0; i < W_BC_N; i++) w_bcx[i] = 0xFFFF;
    ck_x = ck_y = 0xFFFF;
}

void w_reset(void)
{
    w_lattice_reset();
    w_pq_x = w_pq_y = 0xFFFF;
    w_blocks_reset();
}

/* neighbours (left, right, above, below): offsets and which of their corners are ours
 * (their corner sa -> our da, sb -> db) */
static const int8_t nb_d[4][2] = { { -4, 0 }, { 4, 0 }, { 0, -4 }, { 0, 4 } };
static const uint8_t nb_c[4][4] = { { 1, 0, 3, 2 }, { 0, 1, 2, 3 }, { 2, 0, 3, 1 }, { 0, 2, 1, 3 } };

static uint16_t spk_x = 0xFFFF, spk_y = 0xFFFF;   /* last set-piece filter cell (m & ~63) */
static uint8_t spk_on;

/* bilinear interpolation inside a block: quad(a, b) gives the 4 values a..b at weights 0..3/4 */
static uint8_t fe[16], fm[16], fs[16];            /* fields of the 16 cells of the block */
static uint8_t qe[4], qr[4];                      /* left / right edge columns */
static uint8_t hx[4];                             /* detail hash, first round, per column */

static void quad(uint8_t a, uint8_t b, uint8_t *q)
{
    q[0] = a;
    q[1] = (uint8_t)(a - QV(a) + QV(b));
    q[2] = (uint8_t)(HV(a) + HV(b));
    q[3] = (uint8_t)(b - QV(b) + QV(a));
}

/* one field: corners c[0], c[1], c[2], c[3] (TL TR BL BR) -> out[16] */
static void interp(const uint8_t *c, uint8_t *out)
{
    uint8_t fy;
    quad(c[0], c[2], qe);
    quad(c[1], c[3], qr);
    for (fy = 0; fy < 4; fy++, out += 4) quad(qe[fy], qr[fy], out);
}

/* block-building state (globals: cheaper than locals and arguments under SDCC) */
static uint8_t *g_out, *g_c;
static uint8_t g_sp, g_i, g_fx, g_fy, g_yl, g_oy, g_pnear, g_bx, g_by, g_s;
static uint16_t g_kx, g_ky;

/* one cell of the block being built: fields in ge, gm, gs, detail hash in gd */
static void cell(void)
{
    uint8_t b, t = 0xFF, o;
    /* classify (inline: hot) */
    b = w_classify_base(ge, gm);
    if (b >= B_SHORE && b != B_ROCK) {
        if (gs >= T_ASH) b = B_ASH;
        else if (gs >= T_RUIN) b = B_RUINS;
    }
    /* fires etc. make their own clearing; the road vignette only runs over land */
    if (g_pnear && (w_pq_type != W_POI_ROAD || (b >= B_SHORE && b != B_ROCK)))
        t = w_poi_mt(fold((uint8_t)(g_bx + g_fx), w_pq_px), g_oy, gd);
    if (t == 0xFF) {
        if (b == B_RUINS)
            t = w_ruin((uint16_t)(g_kx + g_fx), (uint16_t)(g_ky + g_fy), gd, terrain(w_classify_base(ge, gm)));
        else
            t = terrain(b);
        if (world_old_cairn_count && b >= B_SHORE && b != B_ROCK &&
            !(((uint8_t)((uint8_t)g_kx + g_fx - (uint8_t)world.start.x) | (uint8_t)(g_yl - (uint8_t)world.start.y)) & 3) &&
            w_ready && w_old_cairn((uint16_t)(g_kx + g_fx), (uint16_t)(g_ky + g_fy)))
            t = MT_CAIRN_OLD;
    }
    if (g_sp) {
        o = g_out[g_i];
        if (o != W_SP_NONE) {
            if (o != W_SP_CLEAR) t = o;
            else if (mt_flags[t] & MTF_SOLID) t = w_start_ground;
        }
    }
    g_out[g_i] = t;
}

/* 1. lattice corners of block (g_kx, g_ky) into g_c, shared with cached neighbours */
static void bb_corners(void)
{
    const uint8_t *e, *t;
    uint8_t i, j, have = 0;
    uint16_t nx, ny;
    for (i = 0; i < 4; i++) {
        nx = (uint16_t)(g_kx + nb_d[i][0]);
        ny = (uint16_t)(g_ky + nb_d[i][1]);
        j = w_bslot(nx, ny);
        if (w_bcx[j] != nx || w_bcy[j] != ny) continue;
        e = bcc[j];
        t = nb_c[i];
        for (j = 0; j < 12; j += 4) {
            g_c[t[1] + j] = e[t[0] + j];
            g_c[t[3] + j] = e[t[2] + j];
        }
        have |= (uint8_t)((1u << t[1]) | (1u << t[3]));
    }
    for (i = 0; i < 4; i++) {
        if (have & (1u << i)) continue;
        w_lattice((uint16_t)((g_kx >> 2) + (i & 1)), (uint16_t)((g_ky >> 2) + (i >> 1)));
        g_c[i] = w_le;
        g_c[i + 4] = w_lm;
        g_c[i + 8] = w_ls;
    }
}

/* 2. the POI of this 16x16 cell, if it can reach into the block -> g_pnear */
static void bb_poi(void)
{
    uint16_t cx = g_kx >> 4, cy = g_ky >> 4;
    if (cx != w_pq_x || cy != w_pq_y) w_poi_roll(cx, cy);
    g_pnear = 0;
    if (w_pq_type == W_POI_NONE) return;
    if (w_pq_type == W_POI_ROAD)
        g_pnear = (uint8_t)(w_pq_px - g_bx) < 4 || (uint8_t)(w_pq_py - g_by) < 4;
    else
        g_pnear = (uint8_t)(w_pq_px + 2 - g_bx) < 8 && (uint8_t)(w_pq_py + 2 - g_by) < 8;
    if (g_pnear && !w_pq_ok) w_poi_check((uint16_t)((g_kx & 0xFFF0) + w_pq_px), (uint16_t)((g_ky & 0xFFF0) + w_pq_py));
    if (w_pq_type == W_POI_NONE) g_pnear = 0;
}

/* 3. set pieces: coarse filter per 64x64 cell, then the banked per-block check; the overrides
 *    are written to g_out[] and merged by cell() -> g_sp */
static void bb_pieces(void)
{
    uint8_t cx, cy;
    g_sp = 0;
    if (!w_ready) return;
    if ((g_kx & 0xFFC0) != spk_x || (g_ky & 0xFFC0) != spk_y) {
        spk_x = g_kx & 0xFFC0;
        spk_y = g_ky & 0xFFC0;
        cx = (uint8_t)((uint16_t)(g_kx - w_spx0) >> 6);
        cy = (uint8_t)((uint16_t)(g_ky - w_spy0) >> 6);
        spk_on = (uint16_t)(g_kx - w_spx0) < 1024 && (uint16_t)(g_ky - w_spy0) < 1024 &&
                 (w_sp_bits[(uint8_t)((cy << 1) | (cx >> 3))] & w_bitmask[cx & 7]);
    }
    if (spk_on) g_sp = w_pieces(g_kx, g_ky, g_out);
}

/* 5. mods in this block */
static void bb_mods(void)
{
    const wmod_t *w = world_mods;
    uint8_t i = w_mod_bit(g_kx, g_ky);
    if (!(w_mod_bloom[i >> 3] & w_bitmask[i & 7])) return;
    for (i = world_mod_count; i; i--, w++)
        if ((w->x & 0xFFFC) == g_kx && (w->y & 0xFFFC) == g_ky)
            g_out[(uint8_t)(((uint8_t)w->y & 3) << 2) | ((uint8_t)w->x & 3)] = w->mt;
}

static void block_build(void)
{
    uint8_t i, j;
    w_bcx[g_s] = 0xFFFF;   /* being rebuilt: not a valid neighbour */
    g_c = bcc[g_s];
    g_out = w_bcm[g_s];
    g_bx = (uint8_t)g_kx & 15;
    g_by = (uint8_t)g_ky & 15;
    bb_corners();
    bb_poi();
    bb_pieces();

    /* 4. the 16 cells */
    interp(g_c, fe);
    interp(g_c + 4, fm);
    interp(g_c + 8, fs);
    for (i = 0; i < 4; i++) hx[i] = w_perm[(uint8_t)(((uint8_t)g_kx + i) ^ w_s0)];
    i = (uint8_t)((uint8_t)(g_kx >> 8) ^ w_s1);
    j = (uint8_t)((uint8_t)(g_ky >> 8) ^ SALT_D);
    g_i = 0;
    for (g_fy = 0; g_fy < 4; g_fy++) {
        g_yl = (uint8_t)((uint8_t)g_ky + g_fy);
        g_oy = fold((uint8_t)(g_by + g_fy), w_pq_py);
        for (g_fx = 0; g_fx < 4; g_fx++) {
            ge = fe[g_i]; gm = fm[g_i]; gs = fs[g_i];
            /* detail hash = w_hash(mx, my) with SALT_D, sharing the first round per column */
            gd = w_perm[(uint8_t)(w_perm[(uint8_t)(w_perm[(uint8_t)(hx[g_fx] ^ g_yl)] ^ i)] ^ j)];
            cell();
            g_i++;
        }
    }
    if (world_mod_count && !w_mods_off) bb_mods();
    w_bcx[g_s] = g_kx;
    w_bcy[g_s] = g_ky;
}

/* make block (kx, ky) current */
static void block_get(uint16_t kx, uint16_t ky)
{
    uint8_t s = w_bslot(kx, ky);
    if (w_mods_seen != world_mod_count) world_mods_rebuild();   /* mods written directly */
    if (w_bcx[s] != kx || w_bcy[s] != ky) {
        g_s = s;
        g_kx = kx;
        g_ky = ky;
        block_build();
    }
    ck_x = kx;
    ck_y = ky;
    ckm = w_bcm[s];
    ckslot = s;
}

uint8_t world_mt(uint16_t mx, uint16_t my)
{
    if ((mx & 0xFFFC) != ck_x || (my & 0xFFFC) != ck_y || w_mods_seen != world_mod_count)
        block_get(mx & 0xFFFC, my & 0xFFFC);
    return ckm[(uint8_t)(((uint8_t)my & 3) << 2) | ((uint8_t)mx & 3)];
}

/* biome at the nearest lattice point (within 2 metatiles; cheap: uses the block corners) */
uint8_t world_biome(uint16_t mx, uint16_t my)
{
    const uint8_t *c;
    uint8_t i = (uint8_t)((((uint8_t)mx >> 1) & 1) | ((uint8_t)my & 2));
    world_mt(mx, my);
    c = &bcc[ckslot][i];
    return w_classify(c[0], c[4], c[8]);
}
