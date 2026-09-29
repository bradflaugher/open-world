/* world.c - OPEN WORLD world core, HOT PATH: noise, biomes, metatiles, set-piece lookup, mods.
 *
 * Portable C: compiles with SDCC (Game Boy, SM83) and gcc (host tests / owgen).
 * Not banked: keep this file in bank 0 (the engine calls world_mt while scrolling). The cold
 * half (world_init, layout, causeways, bearing / distance) is world_gen.c, banked.
 *
 * Performance notes (SM83):
 *  - No multiply / divide / modulo on variables here.
 *  - The noise fields are evaluated on a 4-metatile lattice and bilinearly interpolated inside
 *    each 4x4 block with 2-bit weights (shifts and adds only). Bilinear-of-bilinear is exact,
 *    so this equals interpolating each octave on its own grid.
 *  - Caches: 8 recent 4x4 blocks (their lattice corners; a new block copies shared corners from
 *    its cached neighbours, so scrolling computes about one lattice point per new block), the
 *    last lattice cell of each coarse octave, the last row/column partial interpolation, the
 *    last 4 POI cells and the last set-piece filter cell. All are pure caches: results never
 *    depend on the call order.
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

#define SALT_E  0x00         /* elevation, 16 grid */
#define SALT_F  0x3C         /* elevation, 4 grid */
#define SALT_M  0x95         /* moisture, 32 grid */
#define SALT_S  0xC6         /* strangeness, 64 grid */
#define SALT_C  0xE9         /* continents: elevation, 64 grid */
#define SALT_D  0x5A         /* per-cell detail */
#define SALT_P  0xA3         /* point of interest per 16x16 cell */
#define SALT_P2 0x17
#define SALT_R  0x6E         /* ruin room per 8x8 cell */

/* h = P[P[P[P[xl ^ s0] ^ yl] ^ xh ^ s1] ^ yh ^ salt] */
#ifdef __SDCC
/* sdcccall(1): x in DE, y in BC, result in A. About 60 M-cycles. */
static uint8_t w_hash(uint16_t x, uint16_t y) __naked
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
static uint8_t w_hash(uint16_t x, uint16_t y)
{
    uint8_t h = w_perm[(uint8_t)((uint8_t)x ^ w_s0)];
    h = w_perm[(uint8_t)(h ^ (uint8_t)y)];
    h = w_perm[(uint8_t)(h ^ (uint8_t)(x >> 8) ^ w_s1)];
    return w_perm[(uint8_t)(h ^ (uint8_t)(y >> 8) ^ w_salt)];
}
#endif

static uint8_t hash_s(uint16_t x, uint16_t y, uint8_t salt)
{
    w_salt = salt;
    return w_hash(x, y);
}

uint8_t world_detail(uint16_t mx, uint16_t my)
{
    w_salt = SALT_D;
    return w_hash(mx, my);
}

/* ---- interpolation (shifts and adds) ----------------------------------------------------- */
#define HV(a) ((uint8_t)((uint8_t)(a) >> 1))
#define QV(a) ((uint8_t)((uint8_t)(a) >> 2))

/* a + (b - a) * f / 4, f = 0..3 (within 1 of exact) */
static uint8_t lerp2(uint8_t a, uint8_t b, uint8_t f)
{
    if (!f) return a;
    if (f == 2) return (uint8_t)(HV(a) + HV(b));
    if (f == 1) return (uint8_t)(a - QV(a) + QV(b));
    return (uint8_t)(b - QV(b) + QV(a));
}

/* a + (b - a) * f / 2^n, f has n bits: bit-serial averaging of halves, LSB first */
static uint8_t lerpn(uint8_t a, uint8_t b, uint8_t f, uint8_t n)
{
    uint8_t v = a;
    if (!f) return a;
    do {
        v = (uint8_t)(HV(v) + HV((f & 1) ? b : a));
        f >>= 1;
    } while (--n);
    return v;
}

/* ---- noise fields ------------------------------------------------------------------------ */
/* coarse-octave lattice hashes, one cached cell per octave */
static uint16_t kc_x, kc_y, ke_x, ke_y, km_x, km_y;
static uint8_t cc[4], cs[4], ch[4], cw[4];   /* continents 64, strangeness 64, elevation 16, moisture 32 */
static uint8_t kc_fx, hc_t, hc_b, hs_t, hs_b; /* 64-grid horizontal lerps for the last fx */

static void corners4(uint8_t *c, uint16_t kx, uint16_t ky, uint8_t salt)
{
    uint16_t kx1 = (uint16_t)(kx + 1), ky1 = (uint16_t)(ky + 1);
    w_salt = salt;
    c[0] = w_hash(kx, ky);
    c[1] = w_hash(kx1, ky);
    c[2] = w_hash(kx, ky1);
    c[3] = w_hash(kx1, ky1);
}

static uint8_t bil(const uint8_t *c, uint8_t fx, uint8_t fy, uint8_t n)
{
    return lerpn(lerpn(c[0], c[1], fx, n), lerpn(c[2], c[3], fx, n), fy, n);
}

/* Field values at a 4-metatile lattice point (lx, ly) = (mx >> 2, my >> 2). */
static uint8_t le, lm, ls;
static void lattice(uint16_t lx, uint16_t ly)
{
    uint8_t c, k, fx;
    uint16_t kx = lx >> 4, ky = ly >> 4;
    fx = (uint8_t)lx & 15;
    if (kx != kc_x || ky != kc_y) {
        kc_x = kx;
        kc_y = ky;
        corners4(cc, kx, ky, SALT_C);
        corners4(cs, kx, ky, SALT_S);
        kc_fx = 0xFF;
    }
    if (fx != kc_fx) {
        kc_fx = fx;
        hc_t = lerpn(cc[0], cc[1], fx, 4); hc_b = lerpn(cc[2], cc[3], fx, 4);
        hs_t = lerpn(cs[0], cs[1], fx, 4); hs_b = lerpn(cs[2], cs[3], fx, 4);
    }
    fx = (uint8_t)ly & 15;
    k = lerpn(hc_t, hc_b, fx, 4);
    ls = lerpn(hs_t, hs_b, fx, 4);

    kx = lx >> 3;
    ky = ly >> 3;
    if (kx != km_x || ky != km_y) {
        km_x = kx;
        km_y = ky;
        corners4(cw, kx, ky, SALT_M);
    }
    lm = bil(cw, (uint8_t)lx & 7, (uint8_t)ly & 7, 3);

    kx = lx >> 2;
    ky = ly >> 2;
    if (kx != ke_x || ky != ke_y) {
        ke_x = kx;
        ke_y = ky;
        corners4(ch, kx, ky, SALT_E);
    }
    fx = (uint8_t)lx & 3;
    c = lerp2(lerp2(ch[0], ch[1], fx), lerp2(ch[2], ch[3], fx), (uint8_t)ly & 3);
    k = (uint8_t)(HV(k) + QV(c) + (c >> 3));
    w_salt = SALT_F;
    le = (uint8_t)(k + (w_hash(lx, ly) >> 3));
}

/* Block cache: lattice corners of 8 recent 4x4 blocks, slot = (bx + 5 * by) & 7 (a row or a
 * column of blocks, and their neighbours, land in different slots). Keys are mx & ~3, my & ~3.
 * Entry layout: e0..e3, m0..m3, s0..s3 (corners TL, TR, BL, BR). */
#define BC_N 8
static uint16_t bcx[BC_N], bcy[BC_N];
static uint8_t bcd[BC_N][12];
static uint16_t ck_x, ck_y;                       /* current block key */
static uint8_t cb[12];                            /* current block corners */
/* partial interpolations inside the current block, reused while the caller walks a column
 * (same fx: horizontal-first) or a row (same fy: vertical-first) */
static uint8_t hfx, vfy, lfy;
static uint8_t hte, htm, hts, hbe, hbm, hbs;      /* top / bottom rows at fx = hfx */
static uint8_t vle, vlm, vls, vre, vrm, vrs;      /* left / right columns at fy = vfy */

static uint8_t bslot(uint16_t kx, uint16_t ky)
{
    uint8_t y = (uint8_t)((uint8_t)ky >> 2);
    return (uint8_t)(((uint8_t)((uint8_t)kx >> 2) + (uint8_t)(y << 2) + y) & (BC_N - 1));
}

/* if block (kx,ky) is cached, copy its corners sa, sb into our corners da, db */
static uint8_t have;                              /* bit i: corner i of the new block known */
static void from_nb(uint8_t *d, uint16_t kx, uint16_t ky, uint8_t sa, uint8_t da, uint8_t sb, uint8_t db)
{
    uint8_t s = bslot(kx, ky);
    const uint8_t *e;
    if (bcx[s] != kx || bcy[s] != ky) return;
    e = bcd[s];
    d[da] = e[sa]; d[da + 4] = e[sa + 4]; d[da + 8] = e[sa + 8];
    d[db] = e[sb]; d[db + 4] = e[sb + 4]; d[db + 8] = e[sb + 8];
    have |= (uint8_t)((1u << da) | (1u << db));
}

static void corners_fill(uint16_t kx, uint16_t ky)
{
    uint8_t i = bslot(kx, ky), c;
    uint8_t *d = bcd[i];
    uint16_t lx, ly;
    ck_x = kx;
    ck_y = ky;
    hfx = vfy = 0xFF;
    if (bcx[i] != kx || bcy[i] != ky) {
        bcx[i] = 0xFFFF;   /* being rebuilt: not a valid neighbour */
        have = 0;
        from_nb(d, (uint16_t)(kx - 4), ky, 1, 0, 3, 2);   /* left: its TR, BR = our TL, BL */
        from_nb(d, (uint16_t)(kx + 4), ky, 0, 1, 2, 3);   /* right */
        from_nb(d, kx, (uint16_t)(ky - 4), 2, 0, 3, 1);   /* above: its BL, BR = our TL, TR */
        from_nb(d, kx, (uint16_t)(ky + 4), 0, 2, 1, 3);   /* below */
        lx = kx >> 2;
        ly = ky >> 2;
        for (c = 0; c < 4; c++) {
            if (have & (1u << c)) continue;
            lattice((uint16_t)(lx + (c & 1)), (uint16_t)(ly + (c >> 1)));
            d[c] = le;
            d[c + 4] = lm;
            d[c + 8] = ls;
        }
        bcx[i] = kx;
        bcy[i] = ky;
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
static const uint8_t biome_ground[B_COUNT] = {
    MT_SAND, MT_SAND, MT_SAND, MT_GRASS, MT_GRASS, MT_DUNE, MT_SNOW, MT_GRASS, MT_ASH, MT_GRASS
};

/* underlying biome from the fields (no strangeness overlay) */
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

/* 0 = light (shore, desert, tundra, shallows), 1 = meadow / ruins, 2 = forest / rock / ash,
 * 3 = sea. Uses the lattice point (4-metatile resolution) only. */
uint8_t world_map_shade(uint16_t mx, uint16_t my)
{
    static const uint8_t shade[B_COUNT] = { 3, 0, 0, 1, 2, 0, 0, 2, 2, 1 };
    lattice(mx >> 2, my >> 2);
    return shade[classify(le, lm, ls)];
}

/* ---- set pieces (layout computed by world_init in world_gen.c) --------------------------- */
static const uint8_t sq[8] = { 0, 1, 4, 9, 16, 25, 36, 49 };
static const uint8_t bitmask[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };

w_road_t w_roads[W_NUM_ROADS];
uint8_t  w_road_x[W_ROAD_POOL];
uint8_t  w_ready;
uint8_t  w_start_ground;
uint16_t w_spx0, w_spy0;
uint8_t  w_sp_bits[32];
static uint16_t spk_x = 0xFFFF, spk_y = 0xFFFF;   /* last filter cell looked up (key: m & ~63) */
static uint8_t spk_on;

static uint8_t on_road(const w_road_t *r, uint16_t mx, uint16_t my)
{
    uint16_t maj, mn, t;
    uint8_t k, o;
    const uint8_t *X;
    if (r->flags & W_R_YMAJOR) { maj = my; mn = mx; } else { maj = mx; mn = my; }
    t = (r->flags & W_R_MINNEG) ? (uint16_t)(r->a_min - mn) : (uint16_t)(mn - r->a_min);
    if (t > r->dmin) return 0;
    maj = (r->flags & W_R_MAJNEG) ? (uint16_t)(r->a_maj - maj) : (uint16_t)(maj - r->a_maj);
    if (maj > 255) return 0;
    o = (uint8_t)maj;
    X = &w_road_x[r->x0];
    if ((uint8_t)t == r->dmin) k = r->n;
    else {
        k = (uint8_t)((uint8_t)t >> r->shift);
        if ((uint8_t)t & (uint8_t)((1u << r->shift) - 1)) return o == X[k + 1];
    }
    return o >= X[k] && o <= X[k + 1];
}

static uint8_t base_terrain(uint16_t mx, uint16_t my);

/* |v - c| */
static uint8_t fold(uint8_t v, uint8_t c)
{
    return v >= c ? (uint8_t)(v - c) : (uint8_t)(c - v);
}

static uint8_t beacon_piece(uint8_t i, uint8_t ux, uint8_t uy, uint16_t mx, uint16_t my)
{
    uint8_t d2 = (uint8_t)(sq[fold(ux, 6)] + sq[fold(uy, 6)]);
    if (d2 > 34) return 0xFF;
    if (d2 == 0) return MT_BEACON;
    if (i < 2 && mx == world.shrine[i].x && my == world.shrine[i].y) return MT_SHRINE;
    if (d2 <= 2) return MT_RUIN_FLOOR;
    if (d2 <= 13) {
        if (i == 0) return MT_GRASS;
        if (i == 1) return MT_SAND;
        return (world_detail(mx, my) & 3) ? MT_SNOW : MT_RUIN_FLOOR;
    }
    if (i == 0) return MT_BRAMBLE;
    if (i == 1) return MT_SHALLOW;
    return MT_ROCK;
}

static uint8_t set_piece(uint16_t mx, uint16_t my)
{
    uint16_t ux, uy;
    uint8_t i, d2;
    for (i = 0; i < NUM_BEACONS; i++) {
        ux = (uint16_t)(mx - world.beacon[i].x + 6);
        uy = (uint16_t)(my - world.beacon[i].y + 6);
        if (ux < 13 && uy < 13) {
            d2 = beacon_piece(i, (uint8_t)ux, (uint8_t)uy, mx, my);
            if (d2 != 0xFF) return d2;
        }
    }
    ux = (uint16_t)(mx - world.heart.x + 4);
    uy = (uint16_t)(my - world.heart.y + 4);
    if (ux < 9 && uy < 9) {
        d2 = (uint8_t)(sq[fold((uint8_t)ux, 4)] + sq[fold((uint8_t)uy, 4)]);
        if (d2 == 0) return MT_HEART;
        if (d2 <= 20) return (world_detail(mx, my) & 1) ? MT_GLASS : MT_ASH;
    }
    if (mx == world.start.x && my == world.start.y) return MT_FIRE_COLD;
    for (i = 0; i < W_NUM_ROADS; i++) {
        const w_road_t *r = &w_roads[i];
        if ((uint16_t)(mx - r->bx) <= r->bw && (uint16_t)(my - r->by) <= r->bh && on_road(r, mx, my))
            return world_detail(mx, my) < 24 ? MT_RUIN_FLOOR : MT_ROAD;
    }
    ux = (uint16_t)(mx - world.start.x + 3);
    uy = (uint16_t)(my - world.start.y + 3);
    if (ux < 7 && uy < 7) {
        d2 = (uint8_t)(sq[fold((uint8_t)ux, 3)] + sq[fold((uint8_t)uy, 3)]);
        if (d2 <= 10) {
            i = base_terrain(mx, my);
            return (mt_flags[i] & MTF_SOLID) ? w_start_ground : i;
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
static uint16_t pqk_x = 0xFFFF, pqk_y = 0xFFFF;   /* last POI cell (key: m & ~15) */
static uint8_t pql;                               /* ... and its slot */

static void poi_get(uint16_t cx, uint16_t cy)
{
    uint8_t h, h2, j = (uint8_t)(((uint8_t)cx ^ (uint8_t)((uint8_t)cy << 1)) & (PQ_N - 1));
    pql = j;
    if (pq_x[j] == cx && pq_y[j] == cy) return;
    pq_x[j] = cx;
    pq_y[j] = cy;
    pq_ok[j] = 0;
    h = hash_s(cx, cy, SALT_P);
    h2 = hash_s(cx, cy, SALT_P2);
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
}

/* lazily check the biome at the POI centre (lattice value); may clear the type */
static void poi_validate(uint8_t j)
{
    uint16_t lx, ly;
    uint8_t b, e0, e1, e2;
    pq_ok[j] = 1;
    lx = (uint16_t)(((pq_x[j] << 4) + pq_px[j]) >> 2);
    ly = (uint16_t)(((pq_y[j] << 4) + pq_py[j]) >> 2);
    lattice(lx, ly);
    b = classify(le, lm, ls);
    pq_ground[j] = biome_ground[b];
    if (pq_type[j] == POI_ROAD) {
        if (b != B_SHORE) { pq_type[j] = POI_NONE; return; }
        lattice((uint16_t)(lx - 1), ly); e0 = le;
        lattice((uint16_t)(lx + 1), ly); e0 = fold(e0, le);
        lattice(lx, (uint16_t)(ly - 1)); e1 = le;
        lattice(lx, (uint16_t)(ly + 1)); e2 = fold(e1, le);
        pq_axis[j] = e2 > e0;   /* the road runs along the steeper axis: inland to the sea */
        return;
    }
    if (b <= B_SHORE || b == B_ROCK || le < T_SHORE + 6) { pq_type[j] = POI_NONE; return; }
    if (pq_type[j] == POI_MONOLITH && b == B_ASH) pq_ground[j] = MT_GLASS;
}

void w_reset(void)
{
    uint8_t i;
    kc_x = kc_y = ke_x = ke_y = km_x = km_y = 0xFFFF;
    ck_x = ck_y = 0xFFFF;
    for (i = 0; i < BC_N; i++) bcx[i] = 0xFFFF;
    for (i = 0; i < PQ_N; i++) pq_x[i] = 0xFFFF;
    pqk_x = pqk_y = 0xFFFF;
    spk_x = spk_y = 0xFFFF;
}

/* ---- terrain ----------------------------------------------------------------------------- */
static uint8_t ruin_piece(uint16_t mx, uint16_t my, uint8_t d, uint8_t ground)
{
    uint8_t r = hash_s(mx >> 3, my >> 3, SALT_R);
    uint8_t lx = (uint8_t)mx & 7, ly = (uint8_t)my & 7;
    uint8_t x0, x1, y0, y1;
    switch (r & 3) {
    case 0:   /* a room */
    case 1:
        x0 = (uint8_t)(1 + ((r >> 2) & 1));
        x1 = (uint8_t)(x0 + 3 + ((r >> 3) & 1));
        y0 = (uint8_t)(1 + ((r >> 4) & 1));
        y1 = (uint8_t)(y0 + 3 + ((r >> 5) & 1));
        if (lx < x0 || lx > x1 || ly < y0 || ly > y1) return ground;
        if (lx == x0 || lx == x1 || ly == y0 || ly == y1) {
            /* a door in the middle of one side */
            switch (r >> 6) {
            case 0: if (ly == y0 && lx == (uint8_t)(x0 + 2)) return MT_RUIN_FLOOR; break;
            case 1: if (ly == y1 && lx == (uint8_t)(x0 + 2)) return MT_RUIN_FLOOR; break;
            case 2: if (lx == x0 && ly == (uint8_t)(y0 + 2)) return MT_RUIN_FLOOR; break;
            default: if (lx == x1 && ly == (uint8_t)(y0 + 2)) return MT_RUIN_FLOOR; break;
            }
            if (d < 70) return d < 30 ? ground : MT_RUIN_FLOOR;   /* broken wall */
            return MT_RUIN_WALL;
        }
        return d < 50 ? ground : MT_RUIN_FLOOR;
    case 2:   /* colonnade */
        if (ly < 1 || ly > 6 || lx < 1 || lx > 5) return ground;
        if ((lx == 1 || lx == 5) && (ly & 1)) return d < 60 ? MT_RUIN_FLOOR : MT_PILLAR;
        return d < 40 ? ground : MT_RUIN_FLOOR;
    default:  /* a statue fragment in rubble, or nothing */
        if (!(r & 0x40)) return ground;
        if (lx == 3 && ly == 3) return MT_STATUE_HAND;
        if (lx >= 2 && lx <= 4 && ly >= 2 && ly <= 4) return d < 90 ? ground : MT_RUIN_FLOOR;
        return ground;
    }
}

static uint8_t terrain(uint8_t b, uint8_t e, uint8_t m, uint8_t s, uint8_t d)
{
    switch (b) {
    case B_SEA:
        return d < 5 ? MT_SEA_GLINT : MT_SEA;
    case B_SHALLOW:
        return MT_SHALLOW;
    case B_SHORE:
        return d < 2 ? MT_BONES : MT_SAND;
    case B_MEADOW:
        if (d < 4) return MT_TREE;
        if (m >= 150 && d < 24) return MT_TREE;                               /* forest edge */
        if ((uint8_t)((e + m) & 0x3F) < 10 && d < 220) return MT_GRASS_TALL;  /* swaths */
        if ((uint8_t)(e & 0x0F) < 3 && d < 120) return MT_FLOWERS;
        return MT_GRASS;
    case B_FOREST:
        if ((uint8_t)((e + HV(s)) & 0x1F) < 3) return MT_UNDERGROWTH;         /* winding paths */
        if ((uint8_t)(s & 0x3F) < 5) return d < 40 ? MT_UNDERGROWTH : MT_GRASS;   /* glades */
        return d < 64 ? MT_UNDERGROWTH : MT_TREE;
    case B_DESERT:
        if (d < 3) return MT_BONES;
        return ((uint8_t)(e + HV(m)) & 0x18) ? MT_DUNE : MT_SAND;
    case B_TUNDRA:
        if (d < 12 || ((e & 0x10) && d < 70)) return MT_PINE;
        return MT_SNOW;
    case B_ROCK:
        return e >= T_PEAK ? MT_ROCK_PEAK : MT_ROCK;
    default:  /* B_ASH */
        if (d < 2) return MT_MONOLITH;
        if (d < 12 || ((uint8_t)(e + m) & 0x1C) == 0) return MT_GLASS;
        return MT_ASH;
    }
}

static uint8_t old_cairn(uint16_t mx, uint16_t my)
{
    uint16_t rx = (uint16_t)(mx - world.start.x), ry = (uint16_t)(my - world.start.y);
    int16_t ox, oy;
    uint8_t t;
    if (((uint8_t)rx | (uint8_t)ry) & 3) return 0;
    ox = (int16_t)((int16_t)rx >> 2);
    oy = (int16_t)((int16_t)ry >> 2);
    if (ox < -128 || ox > 127 || oy < -128 || oy > 127) return 0;
    for (t = 0; t < world_old_cairn_count; t++)
        if (world_old_cairns[t].dx == (int8_t)ox && world_old_cairns[t].dy == (int8_t)oy) return 1;
    return 0;
}

static uint8_t base_terrain(uint16_t mx, uint16_t my)
{
    uint8_t b, d, t, ax, ay, j, pt;
    if ((mx & 0xFFF0) != pqk_x || (my & 0xFFF0) != pqk_y) {
        pqk_x = mx & 0xFFF0;
        pqk_y = my & 0xFFF0;
        poi_get(mx >> 4, my >> 4);
    }
    j = pql;
    pt = pq_type[j];
    ax = ay = 0;
    if (pt != POI_NONE) {
        ax = fold((uint8_t)mx & 15, pq_px[j]);
        ay = fold((uint8_t)my & 15, pq_py[j]);
        if (pt == POI_ROAD ? (ax == 0 || ay == 0) : (ax <= 2 && ay <= 2)) {
            if (!pq_ok[j]) poi_validate(j);
            pt = pq_type[j];
        } else pt = POI_NONE;
    }
    fields_at(mx, my);
    b = classify(fE, fM, fS);
    d = world_detail(mx, my);
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
            if (t <= 5 && !(pt == POI_MONOLITH && t > 2)) {
                if (pt == POI_WELL && t <= 2) return MT_RUIN_FLOOR;
                if (pt == POI_HAND) {
                    if (t == 1 && (d & 1)) return MT_STATUE_HAND;
                    return d < 100 ? MT_RUIN_FLOOR : pq_ground[j];
                }
                return pq_ground[j];
            }
        }
    }
    if (b <= B_SHALLOW || b == B_ROCK) return terrain(b, fE, fM, fS, d);
    if (world_old_cairn_count && w_ready && old_cairn(mx, my)) return MT_CAIRN_OLD;
    if (b == B_RUINS)   /* ruin pattern on the underlying ground */
        return ruin_piece(mx, my, d, terrain(classify_base(fE, fM), fE, fM, 0x40, d));
    return terrain(b, fE, fM, fS, d);
}

uint8_t world_mt_base(uint16_t mx, uint16_t my)
{
    uint8_t t;
    if (w_ready) {
        /* coarse filter: is any set piece or road in this 64x64 cell? (cached per cell) */
        if ((mx & 0xFFC0) != spk_x || (my & 0xFFC0) != spk_y) {
            uint16_t cx = (uint16_t)(mx - w_spx0) >> 6, cy = (uint16_t)(my - w_spy0) >> 6;
            spk_x = mx & 0xFFC0;
            spk_y = my & 0xFFC0;
            spk_on = (cx | cy) < 16 &&
                     (w_sp_bits[(uint8_t)(((uint8_t)cy << 1) | ((uint8_t)cx >> 3))] & bitmask[(uint8_t)cx & 7]);
        }
        if (spk_on) {
            t = set_piece(mx, my);
            if (t != 0xFF) return t;
        }
    }
    return base_terrain(mx, my);
}

/* ---- mods -------------------------------------------------------------------------------- */
#define MOD_BUCKETS 16
#define MOD_NONE 0xFF
static uint8_t mod_head[MOD_BUCKETS];
static uint8_t mod_next[MAX_MODS];
static uint8_t mod_indexed = 0xFF;   /* number of entries in the index (0xFF: never built) */

static uint8_t mod_bucket(uint16_t x, uint16_t y)
{
    return (uint8_t)(((uint8_t)x ^ (uint8_t)((uint8_t)y << 2) ^ (uint8_t)((uint8_t)y >> 3)) & (MOD_BUCKETS - 1));
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
