/* world.c - OPEN WORLD world core: noise, biomes, metatiles, set pieces, mods.
 *
 * Portable C: compiles with SDCC (Game Boy, SM83) and gcc (host tests / owgen).
 *
 * Performance notes (SM83):
 *  - No multiply / divide / modulo on variables in the hot path (world_mt and below).
 *    world_init uses a few divides/multiplies; it runs once per world.
 *  - All noise fields are evaluated on a 4-metatile lattice and bilinearly interpolated inside
 *    each 4x4 block with 2-bit weights (averaging only). The four lattice corners are cached, and
 *    the cache slides by one lattice step when the caller walks along a row or column, so a
 *    scrolling engine mostly hits the cache. The coarse octaves (16/32/64 grids) also cache their
 *    four lattice hashes. Bilinear-of-bilinear is exact, so this equals interpolating each octave
 *    on its own grid.
 *  - Hot tables are const (ROM). The whole file is small; keep it in bank 0 if possible
 *    (it is not marked BANKED).
 */
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
static const uint8_t P[256] = {
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

static uint8_t s0, s1;       /* seed bytes mixed into every hash */

#define SALT_E  0x00         /* elevation, 16 grid */
#define SALT_F  0x3C         /* elevation, 4 grid */
#define SALT_M  0x95         /* moisture, 32 grid */
#define SALT_S  0xC6         /* strangeness, 64 grid */
#define SALT_C  0xE9         /* continents: elevation, 64 grid */
#define SALT_D  0x5A         /* per-cell detail */
#define SALT_P  0xA3         /* point of interest per 16x16 cell */
#define SALT_P2 0x17
#define SALT_R  0x6E         /* ruin room per 8x8 cell */

/* 4 table lookups; a macro so the hot path has no call overhead (SDCC passes 3 args badly) */
#define PX(i) P[(uint8_t)(i)]
#define HASH(x, y, salt) \
    PX(PX(PX(PX((uint8_t)(x) ^ s0) ^ (uint8_t)(y)) ^ (uint8_t)((x) >> 8) ^ s1) ^ (uint8_t)((y) >> 8) ^ (salt))

static uint8_t hash16(uint16_t x, uint16_t y, uint8_t salt)
{
    return HASH(x, y, salt);
}

uint8_t world_detail(uint16_t mx, uint16_t my)
{
    return HASH(mx, my, SALT_D);
}

/* ---- interpolation (averaging only) ------------------------------------------------------ */
#define AVG(a, b) ((uint8_t)(((uint16_t)(a) + (uint8_t)(b)) >> 1))

/* a + (b - a) * f / 4, f = 0..3, in 8-bit ops (within 1 of exact) */
#define L1(a, b) ((uint8_t)((uint8_t)((a) - ((a) >> 2)) + ((b) >> 2)))   /* 3/4 a + 1/4 b */
#define L2(a, b) ((uint8_t)(((a) >> 1) + ((b) >> 1)))
static uint8_t lerp2(uint8_t a, uint8_t b, uint8_t f)
{
    switch (f) {
    case 0: return a;
    case 1: return L1(a, b);
    case 2: return L2(a, b);
    default: return L1(b, a);
    }
}

/* a + (b - a) * f / 8 and / 16: bit-serial averaging of halves, LSB first (8-bit ops only;
 * within ~2 of exact) */
#define HV(a) ((uint8_t)((uint8_t)(a) >> 1))
static uint8_t lerp3(uint8_t a, uint8_t b, uint8_t f)
{
    uint8_t v = a;
    if (!f) return a;
    if (f & 1) v = (uint8_t)(HV(a) + HV(b));
    v = (uint8_t)(HV(v) + HV((f & 2) ? b : a));
    return (uint8_t)(HV(v) + HV((f & 4) ? b : a));
}

static uint8_t lerp4(uint8_t a, uint8_t b, uint8_t f)
{
    uint8_t v = a;
    if (!f) return a;
    if (f & 1) v = (uint8_t)(HV(a) + HV(b));
    v = (uint8_t)(HV(v) + HV((f & 2) ? b : a));
    v = (uint8_t)(HV(v) + HV((f & 4) ? b : a));
    return (uint8_t)(HV(v) + HV((f & 8) ? b : a));
}

/* coarse-octave lattice hashes, one cached cell per octave */
static uint16_t kc_x, kc_y, ke_x, ke_y, km_x, km_y;
static uint8_t cc0, cc1, cc2, cc3;   /* continents (64) */
static uint8_t cs0, cs1, cs2, cs3;   /* strangeness (64) */
static uint8_t ch0, ch1, ch2, ch3;   /* elevation (16) */
static uint8_t cw0, cw1, cw2, cw3;   /* moisture (32) */
/* horizontal lerps of the 64-grid octaves for the last fx (a new lattice column shares them) */
static uint8_t kc_fx, hc_t, hc_b, hs_t, hs_b;

/* Field values at a 4-metatile lattice point (lx, ly) = (mx >> 2, my >> 2). */
static uint8_t le, lm, ls;
static void lattice(uint16_t lx, uint16_t ly)
{
    uint8_t c, k, f, fx, fy;
    uint16_t kx = lx >> 4, ky = ly >> 4, kx1, ky1;
    fx = (uint8_t)lx & 15;
    if (kx != kc_x || ky != kc_y) {
        kc_x = kx; kc_y = ky;
        kx1 = (uint16_t)(kx + 1); ky1 = (uint16_t)(ky + 1);
        cc0 = HASH(kx, ky, SALT_C);  cc1 = HASH(kx1, ky, SALT_C);
        cc2 = HASH(kx, ky1, SALT_C); cc3 = HASH(kx1, ky1, SALT_C);
        cs0 = HASH(kx, ky, SALT_S);  cs1 = HASH(kx1, ky, SALT_S);
        cs2 = HASH(kx, ky1, SALT_S); cs3 = HASH(kx1, ky1, SALT_S);
        kc_fx = (uint8_t)~fx;
    }
    if (fx != kc_fx) {
        kc_fx = fx;
        hc_t = lerp4(cc0, cc1, fx); hc_b = lerp4(cc2, cc3, fx);
        hs_t = lerp4(cs0, cs1, fx); hs_b = lerp4(cs2, cs3, fx);
    }
    fy = (uint8_t)ly & 15;
    k = lerp4(hc_t, hc_b, fy);
    ls = lerp4(hs_t, hs_b, fy);

    kx = lx >> 3; ky = ly >> 3;
    if (kx != km_x || ky != km_y) {
        km_x = kx; km_y = ky;
        kx1 = (uint16_t)(kx + 1); ky1 = (uint16_t)(ky + 1);
        cw0 = HASH(kx, ky, SALT_M);  cw1 = HASH(kx1, ky, SALT_M);
        cw2 = HASH(kx, ky1, SALT_M); cw3 = HASH(kx1, ky1, SALT_M);
    }
    fx = (uint8_t)lx & 7;
    fy = (uint8_t)ly & 7;
    lm = lerp3(lerp3(cw0, cw1, fx), lerp3(cw2, cw3, fx), fy);

    kx = lx >> 2; ky = ly >> 2;
    if (kx != ke_x || ky != ke_y) {
        ke_x = kx; ke_y = ky;
        kx1 = (uint16_t)(kx + 1); ky1 = (uint16_t)(ky + 1);
        ch0 = HASH(kx, ky, SALT_E);  ch1 = HASH(kx1, ky, SALT_E);
        ch2 = HASH(kx, ky1, SALT_E); ch3 = HASH(kx1, ky1, SALT_E);
    }
    fx = (uint8_t)lx & 3;
    fy = (uint8_t)ly & 3;
    c = lerp2(lerp2(ch0, ch1, fx), lerp2(ch2, ch3, fx), fy);
    f = HASH(lx, ly, SALT_F);
    le = (uint8_t)((k >> 1) + (c >> 2) + (c >> 3) + (f >> 3));
}

/* Lattice point cache: 8x8 direct-mapped by the low bits of (lx, ly). */
#define PC_N 64
static uint16_t pcx[PC_N], pcy[PC_N];
static uint8_t pce[PC_N], pcm[PC_N], pcs[PC_N];

static uint8_t point(uint16_t lx, uint16_t ly)
{
    uint8_t j = (uint8_t)((((uint8_t)ly & 7) << 3) | ((uint8_t)lx & 7));
    if (pcx[j] != lx || pcy[j] != ly) {
        lattice(lx, ly);
        pcx[j] = lx;
        pcy[j] = ly;
        pce[j] = le;
        pcm[j] = lm;
        pcs[j] = ls;
    }
    return j;
}

/* Block cache: the corners of 4 recent 4x4 blocks, direct-mapped by (bx ^ by) so that a
 * column or a row of blocks fits; the current block is copied to cb[] (fixed addresses). */
#define BC_N 4
static uint16_t bcx[BC_N], bcy[BC_N];            /* block keys: mx & ~3, my & ~3 */
static uint8_t bcd[BC_N][12];                     /* e0..e3, m0..m3, s0..s3 */
static uint16_t ck_x, ck_y;                       /* current block key */
static uint8_t cb[12];
/* partial interpolations inside the current block, reused while the caller walks a column
 * (same fx: horizontal-first) or a row (same fy: vertical-first) */
static uint8_t hfx, vfy, lfy;
static uint8_t hte, htm, hts, hbe, hbm, hbs;      /* top / bottom rows at fx = hfx */
static uint8_t vle, vlm, vls, vre, vrm, vrs;      /* left / right columns at fy = vfy */

static void corners_fill(uint16_t kx, uint16_t ky)
{
    uint8_t j, i = (uint8_t)((((uint8_t)kx ^ (uint8_t)ky) >> 2) & (BC_N - 1));
    uint8_t *d = bcd[i];
    uint16_t lx, ly, lx1, ly1;
    ck_x = kx;
    ck_y = ky;
    hfx = vfy = 0xFF;
    if (bcx[i] != kx || bcy[i] != ky) {
        bcx[i] = kx;
        bcy[i] = ky;
        lx = kx >> 2;
        ly = ky >> 2;
        lx1 = (uint16_t)(lx + 1);
        ly1 = (uint16_t)(ly + 1);
        j = point(lx, ly);   d[0] = pce[j]; d[4] = pcm[j]; d[8] = pcs[j];
        j = point(lx1, ly);  d[1] = pce[j]; d[5] = pcm[j]; d[9] = pcs[j];
        j = point(lx, ly1);  d[2] = pce[j]; d[6] = pcm[j]; d[10] = pcs[j];
        j = point(lx1, ly1); d[3] = pce[j]; d[7] = pcm[j]; d[11] = pcs[j];
    }
    memcpy(cb, d, 12);
}

/* fields at a metatile */
static uint8_t fE, fM, fS;
static void fields_at(uint16_t mx, uint16_t my)
{
    uint8_t fx = (uint8_t)mx & 3, fy = (uint8_t)my & 3;
    if ((mx & 0xFFFC) != ck_x || (my & 0xFFFC) != ck_y) corners_fill(mx & 0xFFFC, my & 0xFFFC);
    if (fx == hfx) {
        fE = lerp2(hte, hbe, fy); fM = lerp2(htm, hbm, fy); fS = lerp2(hts, hbs, fy);
    } else if (fy == vfy) {
        fE = lerp2(vle, vre, fx); fM = lerp2(vlm, vrm, fx); fS = lerp2(vls, vrs, fx);
    } else if (fy == lfy) {   /* walking a row: interpolate vertically first */
        vfy = fy;
        vle = lerp2(cb[0], cb[2], fy); vre = lerp2(cb[1], cb[3], fy);
        vlm = lerp2(cb[4], cb[6], fy); vrm = lerp2(cb[5], cb[7], fy);
        vls = lerp2(cb[8], cb[10], fy); vrs = lerp2(cb[9], cb[11], fy);
        fE = lerp2(vle, vre, fx); fM = lerp2(vlm, vrm, fx); fS = lerp2(vls, vrs, fx);
    } else {
        hfx = fx;
        hte = lerp2(cb[0], cb[1], fx); hbe = lerp2(cb[2], cb[3], fx);
        htm = lerp2(cb[4], cb[5], fx); hbm = lerp2(cb[6], cb[7], fx);
        hts = lerp2(cb[8], cb[9], fx); hbs = lerp2(cb[10], cb[11], fx);
        fE = lerp2(hte, hbe, fy); fM = lerp2(htm, hbm, fy); fS = lerp2(hts, hbs, fy);
    }
    lfy = fy;
}

/* ---- biomes ------------------------------------------------------------------------------ */
#define T_SEA      100   /* E below: deep sea */
#define T_SHALLOW  108   /* E below: shallows */
#define T_SHORE    114   /* E below: shore sand */
#define T_ROCK     176   /* E at/above: rock */
#define T_PEAK     194   /* E at/above: peak */
#define T_ALPINE   158   /* E at/above (and wet enough): tundra */
#define T_RUIN     178   /* S at/above: ruins fringe */
#define T_ASH      192   /* S at/above: ash / glass */

/* land biome by moisture (M >> 4) */
static const uint8_t moist_biome[16] = {
    B_DESERT, B_DESERT, B_DESERT, B_DESERT, B_DESERT,
    B_MEADOW, B_MEADOW, B_MEADOW, B_MEADOW,
    B_FOREST, B_FOREST, B_FOREST, B_FOREST, B_FOREST, B_FOREST, B_FOREST
};

/* ground used for clearings / set pieces in each biome */
static const uint8_t biome_ground[B_COUNT] = {
    MT_SAND, MT_SAND, MT_SAND, MT_GRASS, MT_GRASS, MT_DUNE, MT_SNOW, MT_GRASS, MT_ASH, MT_GRASS
};

/* underlying biome from the three fields (no strangeness overlay) */
static uint8_t classify_base(uint8_t e, uint8_t m)
{
    if (e < T_SEA) return B_SEA;
    if (e < T_SHALLOW) return B_SHALLOW;
    if (e < T_SHORE) return B_SHORE;
    if (e >= T_ROCK) return B_ROCK;
    if (e >= T_ALPINE) return m >= 48 ? B_TUNDRA : B_ROCK;
    return moist_biome[m >> 4];
}

static uint8_t classify(uint8_t e, uint8_t m, uint8_t s)
{
    uint8_t b = classify_base(e, m);
    if (b >= B_SHORE && b != B_ROCK) {
        if (s >= T_ASH) return B_ASH;
        if (s >= T_RUIN) return B_RUINS;
    }
    return b;
}

uint8_t world_biome(uint16_t mx, uint16_t my)
{
    fields_at(mx, my);
    return classify(fE, fM, fS);
}

uint8_t world_map_shade(uint16_t mx, uint16_t my)
{
    uint8_t b;
    lattice(mx >> 2, my >> 2);
    b = classify(le, lm, ls);
    switch (b) {
    case B_SEA: return 3;
    case B_SHALLOW: case B_SHORE: case B_DESERT: case B_TUNDRA: return 0;
    case B_MEADOW: case B_RUINS: return 1;
    default: return 2;   /* forest, rock, ash */
    }
}

/* ---- set pieces -------------------------------------------------------------------------- */
static const uint8_t sq[8] = { 0, 1, 4, 9, 16, 25, 36, 49 };

#define ROAD_TAB   64
#define NUM_ROADS  (NUM_BEACONS + 1)
typedef struct {
    uint16_t a_maj, a_min;      /* start (major, minor) */
    uint16_t lo_maj, span_maj;  /* major bbox */
    uint16_t dmin;              /* |minor delta| */
    uint8_t ymajor;             /* 1: major axis is y */
    uint8_t smaj_neg, smin_neg; /* 1: coordinate decreases along the road */
    uint8_t n;                  /* number of minor steps (legs) */
    uint16_t X[ROAD_TAB];       /* major coordinate of the breakpoints, n + 2 entries */
} road_t;
static road_t roads[NUM_ROADS];
static uint8_t sp_ready;        /* set pieces valid (world_init done) */
static uint8_t start_ground;

/* precomputed for the hot path */
static uint16_t bx0[NUM_BEACONS], by0[NUM_BEACONS];   /* beacon box corners (beacon - 6) */
static uint16_t hx0, hy0, sx0, sy0;                  /* heart box (- 4), start box (- 3) */
static uint16_t rbx[NUM_ROADS], rby[NUM_ROADS], rbw[NUM_ROADS], rbh[NUM_ROADS];   /* road bboxes */
/* coarse filter: 32x32 cells of 32x32 metatiles around the start; bit set = a set piece or a
 * road touches that cell */
static uint16_t spx0, spy0;
static uint8_t sp_bits[128];
static uint16_t spk_x = 0xFFFF, spk_y = 0xFFFF;   /* last 32-cell looked up (key: m & ~31) */
static uint8_t spk_on;
static const uint8_t bitmask[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };

static uint8_t on_road(const road_t *r, uint16_t mx, uint16_t my)
{
    uint16_t maj, mn, t, a, b;
    uint8_t k;
    if (r->ymajor) { maj = my; mn = mx; } else { maj = mx; mn = my; }
    t = r->smin_neg ? (uint16_t)(r->a_min - mn) : (uint16_t)(mn - r->a_min);
    if (t == r->dmin) k = r->n;
    else {
        k = (uint8_t)(t >> 2);
        if ((uint8_t)t & 3) return maj == r->X[k + 1];
    }
    a = r->X[k];
    b = r->X[k + 1];
    if (r->smaj_neg) return (uint16_t)(maj - b) <= (uint16_t)(a - b);
    return (uint16_t)(maj - a) <= (uint16_t)(b - a);
}

/* set piece at (mx,my), or 0xFF. Also handles the start clearing (needs the base). */
static uint8_t base_terrain(uint16_t mx, uint16_t my);

static uint8_t beacon_piece(uint8_t i, uint8_t ux, uint8_t uy, uint16_t mx, uint16_t my)
{
    uint8_t d2;
    ux = ux >= 6 ? (uint8_t)(ux - 6) : (uint8_t)(6 - ux);
    uy = uy >= 6 ? (uint8_t)(uy - 6) : (uint8_t)(6 - uy);
    d2 = (uint8_t)(sq[ux] + sq[uy]);
    if (d2 > 34) return 0xFF;
    if (d2 == 0) return MT_BEACON;
    if (i < 2 && mx == world.shrine[i].x && my == world.shrine[i].y) return MT_SHRINE;
    if (d2 <= 2) return MT_RUIN_FLOOR;
    if (d2 <= 13) {
        if (i == 0) return MT_GRASS;
        if (i == 1) return MT_SAND;
        return (HASH(mx, my, SALT_D) & 3) ? MT_SNOW : MT_RUIN_FLOOR;
    }
    if (i == 0) return MT_BRAMBLE;
    if (i == 1) return MT_SHALLOW;
    return MT_ROCK;
}

static uint8_t set_piece(uint16_t mx, uint16_t my)
{
    uint16_t ux, uy;
    uint8_t i, ax, ay, d2;
    ux = mx & 0xFFE0;
    uy = my & 0xFFE0;
    if (ux != spk_x || uy != spk_y) {
        spk_x = ux;
        spk_y = uy;
        ux = (uint16_t)(mx - spx0) >> 5;
        uy = (uint16_t)(my - spy0) >> 5;
        spk_on = (ux | uy) < 32 &&
                 (sp_bits[(uint8_t)(((uint8_t)uy << 2) | ((uint8_t)ux >> 3))] & bitmask[(uint8_t)ux & 7]);
    }
    if (!spk_on) return 0xFF;

    ux = (uint16_t)(mx - bx0[0]);
    uy = (uint16_t)(my - by0[0]);
    if (ux < 13 && uy < 13 && (i = beacon_piece(0, (uint8_t)ux, (uint8_t)uy, mx, my)) != 0xFF) return i;
    ux = (uint16_t)(mx - bx0[1]);
    uy = (uint16_t)(my - by0[1]);
    if (ux < 13 && uy < 13 && (i = beacon_piece(1, (uint8_t)ux, (uint8_t)uy, mx, my)) != 0xFF) return i;
    ux = (uint16_t)(mx - bx0[2]);
    uy = (uint16_t)(my - by0[2]);
    if (ux < 13 && uy < 13 && (i = beacon_piece(2, (uint8_t)ux, (uint8_t)uy, mx, my)) != 0xFF) return i;
    ux = (uint16_t)(mx - hx0);
    uy = (uint16_t)(my - hy0);
    if (ux < 9 && uy < 9) {
        ax = (uint8_t)ux; ay = (uint8_t)uy;
        ax = ax >= 4 ? (uint8_t)(ax - 4) : (uint8_t)(4 - ax);
        ay = ay >= 4 ? (uint8_t)(ay - 4) : (uint8_t)(4 - ay);
        d2 = (uint8_t)(sq[ax] + sq[ay]);
        if (d2 == 0) return MT_HEART;
        if (d2 <= 20) return (HASH(mx, my, SALT_D) & 1) ? MT_GLASS : MT_ASH;
    }
    if (mx == world.start.x && my == world.start.y) return MT_FIRE_COLD;
    for (i = 0; i < NUM_ROADS; i++)
        if ((uint16_t)(mx - rbx[i]) <= rbw[i] && (uint16_t)(my - rby[i]) <= rbh[i] && on_road(&roads[i], mx, my))
            return HASH(mx, my, SALT_D) < 24 ? MT_RUIN_FLOOR : MT_ROAD;
    ux = (uint16_t)(mx - sx0);
    uy = (uint16_t)(my - sy0);
    if (ux < 7 && uy < 7) {
        ax = (uint8_t)ux; ay = (uint8_t)uy;
        ax = ax >= 3 ? (uint8_t)(ax - 3) : (uint8_t)(3 - ax);
        ay = ay >= 3 ? (uint8_t)(ay - 3) : (uint8_t)(3 - ay);
        d2 = (uint8_t)(sq[ax] + sq[ay]);
        if (d2 <= 10) {
            i = base_terrain(mx, my);
            if (mt_flags[i] & MTF_SOLID) return start_ground;
            return i;
        }
    }
    return 0xFF;
}

/* ---- points of interest ------------------------------------------------------------------ */
/* One hash roll per 16x16-metatile cell. A 4-entry cache (direct-mapped) holds the rolls; the
 * biome check at the POI's centre is done lazily, only when a cell near the POI is asked for. */
enum { POI_NONE, POI_FIRE, POI_MONOLITH, POI_TABLE, POI_WELL, POI_HAND, POI_ROAD };
#define PQ_N 4
static uint16_t pq_x[PQ_N], pq_y[PQ_N];
static uint8_t pq_type[PQ_N], pq_px[PQ_N], pq_py[PQ_N], pq_ok[PQ_N], pq_ground[PQ_N], pq_axis[PQ_N];
static uint16_t pqk_x = 0xFFFF, pqk_y = 0xFFFF;   /* last POI cell (key: m & ~15) and its slot */
static uint8_t pql;

static uint8_t poi_get(uint16_t cx, uint16_t cy)
{
    uint8_t h, h2, j = (uint8_t)(((uint8_t)cx ^ (uint8_t)((uint8_t)cy << 1)) & (PQ_N - 1));
    pql = j;
    if (pq_x[j] == cx && pq_y[j] == cy) return j;
    pq_x[j] = cx;
    pq_y[j] = cy;
    pq_ok[j] = 0;
    h = hash16(cx, cy, SALT_P);
    h2 = hash16(cx, cy, SALT_P2);
    pq_px[j] = (uint8_t)(3 + (h2 & 7) + ((h2 >> 6) & 1));
    pq_py[j] = (uint8_t)(3 + ((h2 >> 3) & 7) + (h2 >> 7));
    if (h < 86) h = POI_FIRE;
    else if (h < 100) h = POI_MONOLITH;
    else if (h < 105) h = POI_TABLE;
    else if (h < 110) h = POI_WELL;
    else if (h < 114) h = POI_HAND;
    else if (h < 120) h = POI_ROAD;
    else h = POI_NONE;
    pq_type[j] = h;
    return j;
}

/* lazily check the biome at the POI centre (coarse lattice value); may clear the type */
static void poi_validate(uint8_t j)
{
    uint16_t lx, ly;
    uint8_t b, e0, e1, e2, e3;
    pq_ok[j] = 1;
    lx = (uint16_t)(((pq_x[j] << 4) + pq_px[j]) >> 2);
    ly = (uint16_t)(((pq_y[j] << 4) + pq_py[j]) >> 2);
    lattice(lx, ly);
    b = classify(le, lm, ls);
    pq_ground[j] = biome_ground[b];
    if (pq_type[j] == POI_ROAD) {
        if (b != B_SHORE) { pq_type[j] = POI_NONE; return; }
        lattice((uint16_t)(lx - 1), ly); e0 = le;
        lattice((uint16_t)(lx + 1), ly); e1 = le;
        lattice(lx, (uint16_t)(ly - 1)); e2 = le;
        lattice(lx, (uint16_t)(ly + 1)); e3 = le;
        e0 = e0 > e1 ? (uint8_t)(e0 - e1) : (uint8_t)(e1 - e0);
        e2 = e2 > e3 ? (uint8_t)(e2 - e3) : (uint8_t)(e3 - e2);
        pq_axis[j] = e2 > e0;   /* the road runs along the steeper axis: inland to the sea */
        return;
    }
    if (b <= B_SHORE || b == B_ROCK || le < T_SHORE + 6) { pq_type[j] = POI_NONE; return; }
    if (pq_type[j] == POI_MONOLITH && b == B_ASH) pq_ground[j] = MT_GLASS;
}

static void caches_reset(void)
{
    uint8_t i;
    kc_x = kc_y = ke_x = ke_y = km_x = km_y = 0xFFFF;
    for (i = 0; i < PQ_N; i++) pq_x[i] = 0xFFFF;
    pqk_x = pqk_y = 0xFFFF;
    ck_x = ck_y = 0xFFFF;
    for (i = 0; i < PC_N; i++) pcx[i] = 0xFFFF;
    for (i = 0; i < BC_N; i++) bcx[i] = 0xFFFF;
}

/* ---- terrain ----------------------------------------------------------------------------- */
static uint8_t ruin_piece(uint16_t mx, uint16_t my, uint8_t d, uint8_t ground)
{
    uint8_t r = hash16(mx >> 3, my >> 3, SALT_R);
    uint8_t lx = (uint8_t)mx & 7, ly = (uint8_t)my & 7;
    uint8_t x0, x1, y0, y1, side;
    switch (r & 3) {
    case 0:   /* a room */
    case 1:
        x0 = (uint8_t)(1 + ((r >> 2) & 1));
        x1 = (uint8_t)(x0 + 3 + ((r >> 3) & 1));
        y0 = (uint8_t)(1 + ((r >> 4) & 1));
        y1 = (uint8_t)(y0 + 3 + ((r >> 5) & 1));
        if (lx < x0 || lx > x1 || ly < y0 || ly > y1) return ground;
        if (lx == x0 || lx == x1 || ly == y0 || ly == y1) {
            side = r >> 6;   /* door in the middle of one side */
            if (side == 0 && ly == y0 && lx == (uint8_t)(x0 + 2)) return MT_RUIN_FLOOR;
            if (side == 1 && ly == y1 && lx == (uint8_t)(x0 + 2)) return MT_RUIN_FLOOR;
            if (side == 2 && lx == x0 && ly == (uint8_t)(y0 + 2)) return MT_RUIN_FLOOR;
            if (side == 3 && lx == x1 && ly == (uint8_t)(y0 + 2)) return MT_RUIN_FLOOR;
            if (d < 70) return d < 30 ? ground : MT_RUIN_FLOOR;   /* broken wall */
            return MT_RUIN_WALL;
        }
        return d < 50 ? ground : MT_RUIN_FLOOR;
    case 2:   /* colonnade */
        if (ly < 1 || ly > 6) return ground;
        if ((lx == 1 || lx == 5) && (ly & 1)) return d < 60 ? MT_RUIN_FLOOR : MT_PILLAR;
        if (lx >= 1 && lx <= 5) return d < 40 ? ground : MT_RUIN_FLOOR;
        return ground;
    default:  /* a statue fragment in rubble, or nothing */
        if (!(r & 0x40)) return ground;
        if (lx == 3 && ly == 3) return MT_STATUE_HAND;
        if (lx >= 2 && lx <= 4 && ly >= 2 && ly <= 4) return d < 90 ? ground : MT_RUIN_FLOOR;
        return ground;
    }
}

static uint8_t terrain(uint8_t b, uint8_t e, uint8_t m, uint8_t s, uint8_t d, uint16_t mx, uint16_t my)
{
    switch (b) {
    case B_SEA:
        return d < 5 ? MT_SEA_GLINT : MT_SEA;
    case B_SHALLOW:
        return MT_SHALLOW;
    case B_SHORE:
        if (d < 2) return MT_BONES;
        return MT_SAND;
    case B_MEADOW:
        if (d < 4) return MT_TREE;
        if (m >= 150 && d < 24) return MT_TREE;               /* forest edge */
        if ((uint8_t)((e + m) & 0x3F) < 10 && d < 220) return MT_GRASS_TALL;   /* swaths */
        if ((uint8_t)(e & 0x0F) < 3 && d < 120) return MT_FLOWERS;
        return MT_GRASS;
    case B_FOREST:
        if ((uint8_t)((e + (s >> 1)) & 0x1F) < 3) return MT_UNDERGROWTH;   /* winding paths */
        if ((uint8_t)(s & 0x3F) < 5) return d < 40 ? MT_UNDERGROWTH : MT_GRASS;   /* glades */
        if (d < 64) return MT_UNDERGROWTH;
        return MT_TREE;
    case B_DESERT:
        if (d < 3) return MT_BONES;
        return ((uint8_t)(e + (m >> 1)) & 0x18) ? MT_DUNE : MT_SAND;
    case B_TUNDRA:
        if (d < 12) return MT_PINE;
        if ((e & 0x10) && d < 70) return MT_PINE;
        return MT_SNOW;
    case B_ROCK:
        return e >= T_PEAK ? MT_ROCK_PEAK : MT_ROCK;
    case B_ASH:
        if (d < 2) return MT_MONOLITH;
        if (d < 12 || ((uint8_t)(e + m) & 0x1C) == 0) return MT_GLASS;
        return MT_ASH;
    default:  /* B_RUINS: ruin pattern on the underlying ground */
        b = classify_base(e, m);
        return ruin_piece(mx, my, d, terrain(b, e, m, 0x40, d, mx, my));
    }
}

static uint8_t base_terrain(uint16_t mx, uint16_t my)
{
    uint8_t b, d, t, ax, ay, j, pt;
    uint16_t rx, ry;
    rx = mx & 0xFFF0;
    ry = my & 0xFFF0;
    if (rx != pqk_x || ry != pqk_y) {
        pqk_x = rx;
        pqk_y = ry;
        poi_get(mx >> 4, my >> 4);
    }
    j = pql;
    pt = pq_type[j];
    ax = 0xFF;
    ay = 0xFF;
    if (pt != POI_NONE) {
        ax = (uint8_t)mx & 15;
        ay = (uint8_t)my & 15;
        ax = ax >= pq_px[j] ? (uint8_t)(ax - pq_px[j]) : (uint8_t)(pq_px[j] - ax);
        ay = ay >= pq_py[j] ? (uint8_t)(ay - pq_py[j]) : (uint8_t)(pq_py[j] - ay);
        if (pt == POI_ROAD ? (ax == 0 || ay == 0) : (ax <= 2 && ay <= 2)) {
            if (!pq_ok[j]) poi_validate(j);
            pt = pq_type[j];
        } else pt = POI_NONE;
    }
    fields_at(mx, my);
    b = classify(fE, fM, fS);
    d = HASH(mx, my, SALT_D);
    if (pt != POI_NONE) {
        if (pt == POI_ROAD) {
            if ((pq_axis[j] ? ax == 0 : ay == 0) && b >= B_SHORE && b != B_ROCK)
                return d < 40 ? MT_RUIN_FLOOR : MT_ROAD;
        } else {
            t = (uint8_t)(sq[ax] + sq[ay]);
            if (t == 0) {
                switch (pt) {
                case POI_FIRE: return MT_FIRE_COLD;
                case POI_MONOLITH: return MT_MONOLITH;
                case POI_TABLE: return MT_TABLE;
                case POI_WELL: return MT_WELL;
                default: return MT_STATUE_HAND;
                }
            }
            if (t <= 5) {
                if (pt == POI_WELL && t <= 2) return MT_RUIN_FLOOR;
                if (pt == POI_HAND) {
                    if (t == 1 && (d & 1)) return MT_STATUE_HAND;
                    return d < 100 ? MT_RUIN_FLOOR : pq_ground[j];
                }
                if (pt == POI_MONOLITH && t > 2) goto natural;
                return pq_ground[j];
            }
        }
    }
natural:
    if (world_old_cairn_count && sp_ready) {
        rx = (uint16_t)(mx - world.start.x);
        ry = (uint16_t)(my - world.start.y);
        if (!(((uint8_t)rx | (uint8_t)ry) & 3) && b >= B_SHORE && b != B_ROCK) {
            int16_t ox = (int16_t)rx, oy = (int16_t)ry;
            ox >>= 2;
            oy >>= 2;
            if (ox >= -128 && ox <= 127 && oy >= -128 && oy <= 127) {
                int8_t cx = (int8_t)ox, cy = (int8_t)oy;
                for (t = 0; t < world_old_cairn_count; t++)
                    if (world_old_cairns[t].dx == cx && world_old_cairns[t].dy == cy) return MT_CAIRN_OLD;
            }
        }
    }
    return terrain(b, fE, fM, fS, d, mx, my);
}

uint8_t world_mt_base(uint16_t mx, uint16_t my)
{
    uint8_t t;
    /* fast path: same 32x32 filter cell as last time and nothing there */
    if (sp_ready && (spk_on || (mx & 0xFFE0) != spk_x || (my & 0xFFE0) != spk_y)) {
        t = set_piece(mx, my);
        if (t != 0xFF) return t;
    }
    return base_terrain(mx, my);
}

/* ---- mods -------------------------------------------------------------------------------- */
#define MOD_BUCKETS 32
#define MOD_NONE 0xFF
static uint8_t mod_head[MOD_BUCKETS];
static uint8_t mod_next[MAX_MODS];
static uint8_t mod_indexed;     /* number of entries in the index */

static uint8_t mod_bucket(uint16_t x, uint16_t y)
{
    return (uint8_t)(((uint8_t)x ^ (uint8_t)((uint8_t)y << 3) ^ (uint8_t)((uint8_t)y >> 2)) & (MOD_BUCKETS - 1));
}

void world_mods_rebuild(void)
{
    uint8_t i, b;
    for (i = 0; i < MOD_BUCKETS; i++) mod_head[i] = MOD_NONE;
    if (world_mod_count > MAX_MODS) world_mod_count = MAX_MODS;
    for (i = 0; i < world_mod_count; i++) {
        b = mod_bucket(world_mods[i].x, world_mods[i].y);
        mod_next[i] = mod_head[b];
        mod_head[b] = i;
    }
    mod_indexed = world_mod_count;
}

void world_mods_clear(void)
{
    world_mod_count = 0;
    world_mods_rebuild();
}

static uint8_t mod_find(uint16_t mx, uint16_t my)
{
    uint8_t i;
    if (mod_indexed != world_mod_count) world_mods_rebuild();
    i = mod_head[mod_bucket(mx, my)];
    while (i != MOD_NONE) {
        if (world_mods[i].x == mx && world_mods[i].y == my) return i;
        i = mod_next[i];
    }
    return MOD_NONE;
}

uint8_t world_mod_set(uint16_t mx, uint16_t my, uint8_t mt)
{
    uint8_t i = mod_find(mx, my), b;
    if (i != MOD_NONE) {
        world_mods[i].mt = mt;
        return 1;
    }
    if (world_mod_count >= MAX_MODS) return 0;
    i = world_mod_count;
    world_mods[i].x = mx;
    world_mods[i].y = my;
    world_mods[i].mt = mt;
    b = mod_bucket(mx, my);
    mod_next[i] = mod_head[b];
    mod_head[b] = i;
    world_mod_count = (uint8_t)(i + 1);
    mod_indexed = world_mod_count;
    return 1;
}

uint8_t world_mt(uint16_t mx, uint16_t my)
{
    uint8_t i;
    if (world_mod_count) {
        i = mod_find(mx, my);
        if (i != MOD_NONE) return world_mods[i].mt;
    }
    return world_mt_base(mx, my);
}

/* ---- helpers ----------------------------------------------------------------------------- */
static uint16_t uabs16(int16_t v) { return v < 0 ? (uint16_t)(-v) : (uint16_t)v; }

uint16_t world_dist(uint16_t ax, uint16_t ay, uint16_t bx, uint16_t by)
{
    uint16_t dx = uabs16((int16_t)(bx - ax)), dy = uabs16((int16_t)(by - ay)), mx, mn;
    if (dx > dy) { mx = dx; mn = dy; } else { mx = dy; mn = dx; }
    return (uint16_t)(mx + (mn >> 2) + (mn >> 3) + (mn >> 5));
}

/* atan(2^-i) in 1/256 turns */
static const uint8_t cordic_atan[8] = { 32, 19, 10, 5, 3, 1, 1, 0 };

uint8_t world_bearing(uint16_t fx, uint16_t fy, uint16_t tx, uint16_t ty)
{
    int16_t u = (int16_t)(fy - ty);   /* north component */
    int16_t v = (int16_t)(tx - fx);   /* east component */
    int16_t nu, nv;
    uint8_t ang = 0, i;
    if (!u && !v) return 0;
    /* scale to 11..12 bits */
    while (u > 4095 || u < -4095 || v > 4095 || v < -4095) { u >>= 1; v >>= 1; }
    while (u < 2048 && u > -2048 && v < 2048 && v > -2048) { u = (int16_t)(u << 1); v = (int16_t)(v << 1); }
    if (u < 0) { u = (int16_t)-u; v = (int16_t)-v; ang = 128; }
    for (i = 0; i < 8; i++) {
        if (v > 0) {
            nu = (int16_t)(u + (v >> i));
            nv = (int16_t)(v - (u >> i));
            ang = (uint8_t)(ang + cordic_atan[i]);
        } else {
            nu = (int16_t)(u - (v >> i));
            nv = (int16_t)(v + (u >> i));
            ang = (uint8_t)(ang - cordic_atan[i]);
        }
        u = nu;
        v = nv;
    }
    return ang;
}

/* ---- world_init -------------------------------------------------------------------------- */
static uint16_t rng;
static uint16_t rnd(void)
{
    uint16_t x = rng;
    x ^= (uint16_t)(x << 7);
    x ^= (uint16_t)(x >> 9);
    x ^= (uint16_t)(x << 8);
    rng = x;
    return x;
}

/* sin of 0..64 (quarter turn, 1/256 turns), scaled to 0..256 */
static const uint16_t sin_q[65] = {
    0, 6, 13, 19, 25, 31, 38, 44, 50, 56, 62, 68, 74, 80, 86, 92, 98, 104, 109, 115, 121, 126, 132,
    137, 142, 147, 152, 157, 162, 167, 172, 177, 181, 185, 190, 194, 198, 202, 206, 209, 213, 216,
    220, 223, 226, 229, 231, 234, 237, 239, 241, 243, 245, 247, 248, 250, 251, 252, 253, 254, 255,
    255, 256, 256, 256
};

static int16_t isin(uint8_t a)   /* sin(a / 256 turn) * 256 */
{
    uint8_t q = a & 63;
    switch (a >> 6) {
    case 0: return (int16_t)sin_q[q];
    case 1: return (int16_t)sin_q[64 - q];
    case 2: return (int16_t)-(int16_t)sin_q[q];
    default: return (int16_t)-(int16_t)sin_q[64 - q];
    }
}

static wpos_t polar(wpos_t o, uint8_t bearing, uint16_t dist)
{
    wpos_t p;
    int32_t dx = ((int32_t)dist * isin(bearing)) >> 8;
    int32_t dy = -(((int32_t)dist * isin((uint8_t)(bearing + 64))) >> 8);
    p.x = (uint16_t)(o.x + (int16_t)dx);
    p.y = (uint16_t)(o.y + (int16_t)dy);
    return p;
}

static void road_build(road_t *r, wpos_t a, wpos_t b)
{
    int16_t dx = (int16_t)(b.x - a.x), dy = (int16_t)(b.y - a.y);
    uint16_t adx = uabs16(dx), ady = uabs16(dy), dmaj;
    uint16_t amaj, bmaj;
    uint8_t k;
    r->ymajor = ady > adx;
    if (r->ymajor) {
        amaj = a.y; bmaj = b.y; r->a_min = a.x; dmaj = ady; r->dmin = adx;
        r->smaj_neg = dy < 0; r->smin_neg = dx < 0;
    } else {
        amaj = a.x; bmaj = b.x; r->a_min = a.y; dmaj = adx; r->dmin = ady;
        r->smaj_neg = dx < 0; r->smin_neg = dy < 0;
    }
    r->a_maj = amaj;
    r->n = (uint8_t)((r->dmin + 3) >> 2);
    r->X[0] = amaj;
    for (k = 1; k <= r->n; k++) {
        uint16_t off = (uint16_t)(((uint32_t)dmaj * k + ((r->n + 1u) >> 1)) / (r->n + 1u));
        r->X[k] = r->smaj_neg ? (uint16_t)(amaj - off) : (uint16_t)(amaj + off);
    }
    r->X[r->n + 1] = bmaj;
    r->lo_maj = r->smaj_neg ? bmaj : amaj;
    r->span_maj = dmaj;
}

/* segment k of road r as a rectangle: runs (leg = 0, k = 0..n) and legs (leg = 1, k = 0..n-1) */
static uint16_t gx0, gy0, gx1, gy1;
static void road_seg(const road_t *r, uint8_t k, uint8_t leg)
{
    uint16_t m0, m1, n0, n1, t;
    t = k == r->n ? r->dmin : (uint16_t)(k << 2);
    n0 = r->smin_neg ? (uint16_t)(r->a_min - t) : (uint16_t)(r->a_min + t);
    if (leg) {
        t = (uint16_t)(k + 1) == r->n ? r->dmin : (uint16_t)((k + 1) << 2);
        n1 = r->smin_neg ? (uint16_t)(r->a_min - t) : (uint16_t)(r->a_min + t);
        m0 = m1 = r->X[k + 1];
    } else {
        n1 = n0;
        m0 = r->X[k];
        m1 = r->X[k + 1];
    }
    if (m0 > m1) { t = m0; m0 = m1; m1 = t; }
    if (n0 > n1) { t = n0; n0 = n1; n1 = t; }
    if (r->ymajor) { gx0 = n0; gx1 = n1; gy0 = m0; gy1 = m1; }
    else { gx0 = m0; gx1 = m1; gy0 = n0; gy1 = n1; }
}

static uint8_t seg_hits(wpos_t c, uint8_t rad)
{
    return gx1 + rad >= c.x && gx0 <= c.x + rad && gy1 + rad >= c.y && gy0 <= c.y + rad;
}

/* does road ri pass through any set-piece box other than its own target? */
static uint8_t road_clashes(uint8_t ri)
{
    const road_t *r = &roads[ri];
    uint8_t k, leg, i;
    for (k = 0; k <= r->n; k++)
        for (leg = 0; leg < 2; leg++) {
            if (leg && k == r->n) break;
            road_seg(r, k, leg);
            for (i = 0; i < NUM_BEACONS; i++)
                if (i != ri && seg_hits(world.beacon[i], 7)) return 1;
            if (ri != NUM_BEACONS && seg_hits(world.heart, 5)) return 1;
        }
    return 0;
}

/* mark the coarse filter cells touched by the rectangle (gx0,gy0)-(gx1,gy1) */
static void sp_mark(void)
{
    uint16_t cx, cy, cx0 = (uint16_t)(gx0 - spx0) >> 5, cx1 = (uint16_t)(gx1 - spx0) >> 5;
    uint16_t cy0 = (uint16_t)(gy0 - spy0) >> 5, cy1 = (uint16_t)(gy1 - spy0) >> 5;
    for (cy = cy0; cy <= cy1 && cy < 32; cy++)
        for (cx = cx0; cx <= cx1 && cx < 32; cx++)
            sp_bits[(cy << 2) | (cx >> 3)] |= bitmask[cx & 7];
}

static void sp_mark_box(wpos_t c, uint8_t rad)
{
    gx0 = (uint16_t)(c.x - rad); gx1 = (uint16_t)(c.x + rad);
    gy0 = (uint16_t)(c.y - rad); gy1 = (uint16_t)(c.y + rad);
    sp_mark();
}

static void sp_prepare(void)
{
    uint8_t i, k, leg;
    const road_t *r;
    for (i = 0; i < NUM_BEACONS; i++) {
        bx0[i] = (uint16_t)(world.beacon[i].x - 6);
        by0[i] = (uint16_t)(world.beacon[i].y - 6);
    }
    hx0 = (uint16_t)(world.heart.x - 4);
    hy0 = (uint16_t)(world.heart.y - 4);
    sx0 = (uint16_t)(world.start.x - 3);
    sy0 = (uint16_t)(world.start.y - 3);
    spx0 = (uint16_t)((world.start.x - 512) & 0xFFE0);   /* aligned: filter cells = m & ~31 */
    spy0 = (uint16_t)((world.start.y - 512) & 0xFFE0);
    spk_x = 0xFFFF;
    for (i = 0; i < 128; i++) sp_bits[i] = 0;
    for (i = 0; i < NUM_BEACONS; i++) sp_mark_box(world.beacon[i], 6);
    sp_mark_box(world.heart, 4);
    sp_mark_box(world.start, 3);
    for (i = 0; i < NUM_ROADS; i++) {
        r = &roads[i];
        rbx[i] = rby[i] = 0xFFFF;
        rbw[i] = rbh[i] = 0;
        for (k = 0; k <= r->n; k++)
            for (leg = 0; leg < 2; leg++) {
                if (leg && k == r->n) break;
                road_seg(r, k, leg);
                sp_mark();
                if (rbx[i] == 0xFFFF) { rbx[i] = gx0; rby[i] = gy0; rbw[i] = gx1; rbh[i] = gy1; }
                if (gx0 < rbx[i]) rbx[i] = gx0;
                if (gy0 < rby[i]) rby[i] = gy0;
                if (gx1 > rbw[i]) rbw[i] = gx1;
                if (gy1 > rbh[i]) rbh[i] = gy1;
            }
        rbw[i] = (uint16_t)(rbw[i] - rbx[i]);
        rbh[i] = (uint16_t)(rbh[i] - rby[i]);
    }
}

static uint8_t land_biome_ok(uint8_t b)
{
    return b == B_MEADOW || b == B_SHORE || b == B_DESERT || b == B_FOREST || b == B_TUNDRA;
}

void world_init(uint16_t seed)
{
    wpos_t c, g;
    uint8_t i, tries, b, best;
    int16_t r, ox, oy;

    world.seed = seed;
    s0 = (uint8_t)seed;
    s1 = (uint8_t)(seed >> 8);
    s0 = P[(uint8_t)(s0 ^ P[s1])];      /* spread nearby seeds */
    s1 = P[(uint8_t)(s1 ^ P[s0])];
    rng = (uint16_t)(seed ^ 0x9E37u);
    if (!rng) rng = 0xACE1u;
    rnd(); rnd();
    sp_ready = 0;
    caches_reset();

    /* start: near the centre, on open land (meadow preferred), searching outward */
    c.x = (uint16_t)(32768u + (rnd() & 63) - 32);
    c.y = (uint16_t)(32768u + (rnd() & 63) - 32);
    world.start = c;
    best = 0;
    for (r = 0; r < 160 && best < 2; r += 4) {
        for (oy = (int16_t)-r; oy <= r && best < 2; oy += 4) {
            for (ox = (int16_t)-r; ox <= r; ox += 4) {
                if (ox != -r && ox != r && oy != -r && oy != r) continue;
                g.x = (uint16_t)(c.x + ox);
                g.y = (uint16_t)(c.y + oy);
                b = world_biome(g.x, g.y);
                if (b == B_MEADOW) { world.start = g; best = 2; break; }
                if (!best && land_biome_ok(b)) { world.start = g; best = 1; }
            }
        }
    }
    b = world_biome(world.start.x, world.start.y);
    start_ground = biome_ground[b];
    if (start_ground == MT_SAND && b != B_SHORE) start_ground = MT_GRASS;

    for (tries = 0; tries < 64; tries++) {
        uint8_t hb;
        for (i = 0; i < NUM_BEACONS; i++) {
            uint8_t bear = (uint8_t)(i * 85 + (rnd() & 15) - 8);
            uint16_t dist = (uint16_t)(90 + (rnd() & 63) + (rnd() & 7));
            world.beacon[i] = polar(world.start, bear, dist);
        }
        hb = (uint8_t)(43 + (rnd() % 3) * 85 + (rnd() & 15) - 8);
        world.heart = polar(world.start, hb, (uint16_t)(200 + (rnd() & 63) - (rnd() & 3)));
        /* roads: start -> the gate point of each beacon (6 out along the road's major axis) */
        for (i = 0; i < NUM_BEACONS; i++) {
            int16_t dx = (int16_t)(world.beacon[i].x - world.start.x);
            int16_t dy = (int16_t)(world.beacon[i].y - world.start.y);
            g = world.beacon[i];
            if (uabs16(dx) >= uabs16(dy)) g.x = (uint16_t)(g.x + (dx > 0 ? -6 : 6));
            else g.y = (uint16_t)(g.y + (dy > 0 ? -6 : 6));
            road_build(&roads[i], world.start, g);
            /* shrine beside the beacon, off the approach axis */
            world.shrine[i] = world.beacon[i];
            if (uabs16(dx) >= uabs16(dy)) world.shrine[i].y = (uint16_t)(world.shrine[i].y + 2);
            else world.shrine[i].x = (uint16_t)(world.shrine[i].x + 2);
        }
        world.shrine[2] = world.beacon[2];
        road_build(&roads[NUM_BEACONS], world.start, world.heart);
        for (i = 0; i < NUM_ROADS; i++)
            if (road_clashes(i)) break;
        if (i == NUM_ROADS) break;
    }
    sp_prepare();
    world_mods_rebuild();
    caches_reset();
    sp_ready = 1;
}
