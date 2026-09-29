/* world_gen.c - OPEN WORLD world core, COLD PATH: world_init (layout, causeways, coarse filter)
 * and the bearing / distance helpers. Banked on the Game Boy; runs once per world.
 */
#ifdef __SDCC
#pragma bank 255
#endif
#define WORLD_INTERNAL
#include "world.h"

static uint16_t uabs16(int16_t v) { return v < 0 ? (uint16_t)(-v) : (uint16_t)v; }

uint16_t world_dist(uint16_t ax, uint16_t ay, uint16_t bx, uint16_t by) WBANKED
{
    uint16_t dx = uabs16((int16_t)(bx - ax)), dy = uabs16((int16_t)(by - ay)), mx, mn;
    if (dx > dy) { mx = dx; mn = dy; } else { mx = dy; mn = dx; }
    return (uint16_t)(mx + (mn >> 2) + (mn >> 3) + (mn >> 5));
}

/* atan(2^-i) in 1/256 turns */
static const uint8_t cordic_atan[8] = { 32, 19, 10, 5, 3, 1, 1, 0 };

/* CORDIC vectoring: shifts and adds only */
uint8_t world_bearing(uint16_t fx, uint16_t fy, uint16_t tx, uint16_t ty) WBANKED
{
    int16_t u = (int16_t)(fy - ty);   /* north component */
    int16_t v = (int16_t)(tx - fx);   /* east component */
    int16_t nu;
    uint8_t ang = 0, i;
    if (!u && !v) return 0;
    while (u > 4095 || u < -4095 || v > 4095 || v < -4095) { u >>= 1; v >>= 1; }
    while (u < 2048 && u > -2048 && v < 2048 && v > -2048) { u = (int16_t)(u << 1); v = (int16_t)(v << 1); }
    if (u < 0) { u = (int16_t)-u; v = (int16_t)-v; ang = 128; }
    for (i = 0; i < 8; i++) {
        if (v > 0) {
            nu = (int16_t)(u + (v >> i));
            v = (int16_t)(v - (u >> i));
            ang = (uint8_t)(ang + cordic_atan[i]);
        } else {
            nu = (int16_t)(u - (v >> i));
            v = (int16_t)(v + (u >> i));
            ang = (uint8_t)(ang - cordic_atan[i]);
        }
        u = nu;
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

static void polar(wpos_t *p, uint8_t bearing, uint16_t dist)
{
    int32_t dx = ((int32_t)dist * isin(bearing)) >> 8;
    int32_t dy = -(((int32_t)dist * isin((uint8_t)(bearing + 64))) >> 8);
    p->x = (uint16_t)(world.start.x + (int16_t)dx);
    p->y = (uint16_t)(world.start.y + (int16_t)dy);
}

/* A causeway from a to b: a stair of runs along the major axis joined by legs of 1 << shift
 * metatiles along the minor axis (both deltas must be <= 255). Returns 0 if the breakpoint pool
 * is full. */
static uint8_t pool;
static uint8_t road_build(w_road_t *r, const wpos_t *a, const wpos_t *b)
{
    int16_t dx = (int16_t)(b->x - a->x), dy = (int16_t)(b->y - a->y);
    uint16_t adx = uabs16(dx), ady = uabs16(dy), dmaj, dmn;
    uint8_t k, n, sh;
    uint8_t *X;
    r->flags = 0;
    if (ady > adx) {
        r->flags |= W_R_YMAJOR;
        r->a_maj = a->y; r->a_min = a->x; dmaj = ady; dmn = adx;
        if (dy < 0) r->flags |= W_R_MAJNEG;
        if (dx < 0) r->flags |= W_R_MINNEG;
    } else {
        r->a_maj = a->x; r->a_min = a->y; dmaj = adx; dmn = ady;
        if (dx < 0) r->flags |= W_R_MAJNEG;
        if (dy < 0) r->flags |= W_R_MINNEG;
    }
    if (dmaj > 255) return 0;
    r->dmin = (uint8_t)dmn;
    for (sh = 2;; sh++) {
        n = (uint8_t)((dmn + (1u << sh) - 1) >> sh);
        if ((uint16_t)pool + n + 2 <= W_ROAD_POOL) break;
        if (sh == 5) return 0;
    }
    r->shift = sh;
    r->n = n;
    r->x0 = pool;
    X = &w_road_x[pool];
    pool = (uint8_t)(pool + n + 2);
    X[0] = 0;
    for (k = 1; k <= n; k++) X[k] = (uint8_t)(((uint16_t)dmaj * k + ((n + 1u) >> 1)) / (n + 1u));
    X[n + 1] = (uint8_t)dmaj;
    /* bounding box */
    if (a->x < b->x) { r->bx = a->x; r->bw = (uint16_t)(b->x - a->x); }
    else { r->bx = b->x; r->bw = (uint16_t)(a->x - b->x); }
    if (a->y < b->y) { r->by = a->y; r->bh = (uint16_t)(b->y - a->y); }
    else { r->by = b->y; r->bh = (uint16_t)(a->y - b->y); }
    return 1;
}

/* segment k of road r as a rectangle: runs (leg = 0, k = 0..n) and legs (leg = 1, k = 0..n-1) */
static uint16_t gx0, gy0, gx1, gy1;
static void road_seg(const w_road_t *r, uint8_t k, uint8_t leg)
{
    const uint8_t *X = &w_road_x[r->x0];
    uint16_t m0, m1, n0, n1, t;
    t = k == r->n ? r->dmin : (uint16_t)((uint16_t)k << r->shift);
    n0 = (r->flags & W_R_MINNEG) ? (uint16_t)(r->a_min - t) : (uint16_t)(r->a_min + t);
    if (leg) {
        t = (uint8_t)(k + 1) == r->n ? r->dmin : (uint16_t)((uint16_t)(k + 1) << r->shift);
        n1 = (r->flags & W_R_MINNEG) ? (uint16_t)(r->a_min - t) : (uint16_t)(r->a_min + t);
        m0 = m1 = X[k + 1];
    } else {
        n1 = n0;
        m0 = X[k];
        m1 = X[k + 1];
    }
    if (r->flags & W_R_MAJNEG) { t = (uint16_t)(r->a_maj - m1); m1 = (uint16_t)(r->a_maj - m0); m0 = t; }
    else { m0 = (uint16_t)(r->a_maj + m0); m1 = (uint16_t)(r->a_maj + m1); }
    if (n0 > n1) { t = n0; n0 = n1; n1 = t; }
    if (r->flags & W_R_YMAJOR) { gx0 = n0; gx1 = n1; gy0 = m0; gy1 = m1; }
    else { gx0 = m0; gx1 = m1; gy0 = n0; gy1 = n1; }
}

static uint8_t seg_hits(const wpos_t *c, uint8_t rad)
{
    return gx1 + rad >= c->x && gx0 <= c->x + rad && gy1 + rad >= c->y && gy0 <= c->y + rad;
}

/* does road ri pass through any set-piece box other than its own target? */
static uint8_t road_clashes(uint8_t ri)
{
    const w_road_t *r = &w_roads[ri];
    uint8_t k, leg, i;
    for (k = 0; k <= r->n; k++)
        for (leg = 0; leg < 2; leg++) {
            if (leg && k == r->n) break;
            road_seg(r, k, leg);
            for (i = 0; i < NUM_BEACONS; i++)
                if (i != ri && seg_hits(&world.beacon[i], 7)) return 1;
            if (ri != NUM_BEACONS && seg_hits(&world.heart, 5)) return 1;
        }
    return 0;
}

static const uint8_t bitmask[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };

/* mark the coarse filter cells touched by the rectangle (gx0,gy0)-(gx1,gy1) */
static void sp_mark(void)
{
    uint16_t cx, cy, cx0 = (uint16_t)(gx0 - w_spx0) >> 6, cx1 = (uint16_t)(gx1 - w_spx0) >> 6;
    uint16_t cy0 = (uint16_t)(gy0 - w_spy0) >> 6, cy1 = (uint16_t)(gy1 - w_spy0) >> 6;
    for (cy = cy0; cy <= cy1 && cy < 16; cy++)
        for (cx = cx0; cx <= cx1 && cx < 16; cx++)
            w_sp_bits[(cy << 1) | (cx >> 3)] |= bitmask[cx & 7];
}

static void sp_mark_box(const wpos_t *c, uint8_t rad)
{
    gx0 = (uint16_t)(c->x - rad); gx1 = (uint16_t)(c->x + rad);
    gy0 = (uint16_t)(c->y - rad); gy1 = (uint16_t)(c->y + rad);
    sp_mark();
}

static void sp_prepare(void)
{
    uint8_t i, k, leg;
    const w_road_t *r;
    w_spx0 = (uint16_t)((world.start.x - 512) & 0xFFC0);   /* aligned: filter cells = m & ~63 */
    w_spy0 = (uint16_t)((world.start.y - 512) & 0xFFC0);
    for (i = 0; i < 32; i++) w_sp_bits[i] = 0;
    for (i = 0; i < NUM_BEACONS; i++) sp_mark_box(&world.beacon[i], 6);
    sp_mark_box(&world.heart, 4);
    sp_mark_box(&world.start, 3);
    for (i = 0; i < W_NUM_ROADS; i++) {
        r = &w_roads[i];
        for (k = 0; k <= r->n; k++)
            for (leg = 0; leg < 2; leg++) {
                if (leg && k == r->n) break;
                road_seg(r, k, leg);
                sp_mark();
            }
    }
}

static uint8_t land_biome_ok(uint8_t b)
{
    return b == B_MEADOW || b == B_SHORE || b == B_DESERT || b == B_FOREST || b == B_TUNDRA;
}

static const uint8_t biome_ground0[B_COUNT] = {
    MT_GRASS, MT_GRASS, MT_SAND, MT_GRASS, MT_GRASS, MT_DUNE, MT_SNOW, MT_GRASS, MT_ASH, MT_GRASS
};

void world_init(uint16_t seed) WBANKED
{
    wpos_t c, g;
    uint8_t i, tries, b, best;
    int16_t r, ox, oy;

    world.seed = seed;
    w_s0 = w_perm[(uint8_t)((uint8_t)seed ^ w_perm[(uint8_t)(seed >> 8)])];   /* spread nearby seeds */
    w_s1 = w_perm[(uint8_t)((uint8_t)(seed >> 8) ^ w_perm[w_s0])];
    rng = (uint16_t)(seed ^ 0x9E37u);
    if (!rng) rng = 0xACE1u;
    rnd();
    rnd();
    w_ready = 0;
    w_reset();

    /* start: near the centre, on open land (meadow preferred), searching outward in rings */
    c.x = (uint16_t)(32768u + (rnd() & 63) - 32);
    c.y = (uint16_t)(32768u + (rnd() & 63) - 32);
    world.start = c;
    best = 0;
    for (r = 0; r < 160 && best < 2; r += 4) {
        for (oy = (int16_t)-r; oy <= r && best < 2; oy += 4) {
            for (ox = (int16_t)-r; ox <= r; ox += (oy == -r || oy == r || !r) ? 4 : (int16_t)(2 * r)) {
                g.x = (uint16_t)(c.x + ox);
                g.y = (uint16_t)(c.y + oy);
                b = world_biome(g.x, g.y);
                if (b == B_MEADOW) { world.start = g; best = 2; break; }
                if (!best && land_biome_ok(b)) { world.start = g; best = 1; }
                if (!r) break;
            }
        }
    }
    w_start_ground = biome_ground0[world_biome(world.start.x, world.start.y)];

    for (tries = 0; tries < 64; tries++) {
        uint8_t hb;
        for (i = 0; i < NUM_BEACONS; i++) {
            uint8_t bear = (uint8_t)(i * 85 + (rnd() & 15) - 8);
            uint16_t dist = (uint16_t)(90 + (rnd() & 63) + (rnd() & 7));
            polar(&world.beacon[i], bear, dist);
        }
        hb = (uint8_t)(43 + (uint8_t)(rnd() % 3) * 85 + (rnd() & 15) - 8);
        polar(&world.heart, hb, (uint16_t)(200 + (rnd() & 31) + (rnd() & 15)));
        pool = 0;
        /* roads: start -> the gate point of each beacon (6 out along the road's major axis) */
        for (i = 0; i < NUM_BEACONS; i++) {
            int16_t dx = (int16_t)(world.beacon[i].x - world.start.x);
            int16_t dy = (int16_t)(world.beacon[i].y - world.start.y);
            g = world.beacon[i];
            world.shrine[i] = world.beacon[i];
            if (uabs16(dx) >= uabs16(dy)) {
                g.x = (uint16_t)(g.x + (dx > 0 ? -6 : 6));
                world.shrine[i].y = (uint16_t)(world.shrine[i].y + 2);   /* off the approach axis */
            } else {
                g.y = (uint16_t)(g.y + (dy > 0 ? -6 : 6));
                world.shrine[i].x = (uint16_t)(world.shrine[i].x + 2);
            }
            if (!road_build(&w_roads[i], &world.start, &g)) break;
        }
        world.shrine[2] = world.beacon[2];
        if (i < NUM_BEACONS || !road_build(&w_roads[NUM_BEACONS], &world.start, &world.heart)) continue;
        for (i = 0; i < W_NUM_ROADS; i++)
            if (road_clashes(i)) break;
        if (i == W_NUM_ROADS) break;
    }
    sp_prepare();
    world_mods_rebuild();
    w_reset();
    w_ready = 1;
}
