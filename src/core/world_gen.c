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

/* ---- rare helpers for the hot path ------------------------------------------------------ */
static const uint8_t sq[8] = { 0, 1, 4, 9, 16, 25, 36, 49 };

static uint8_t fold(uint8_t v, uint8_t c)
{
    return v >= c ? (uint8_t)(v - c) : (uint8_t)(c - v);
}

/* v - c + r if |v - c| <= r, else 0xFF. A separate small function on purpose: SDCC 4.3
 * miscompiled the inline form (uint16_t)(mx - world.beacon[i].x + 6) inside piece() (the high
 * byte of the result was taken from the low byte); tests/test_core_rom.py catches that class. */
static uint8_t box_off(uint16_t v, uint16_t c, uint8_t r)
{
    uint16_t d = (uint16_t)(v - c);
    d = (uint16_t)(d + r);
    if (d > (uint16_t)(r + r)) return 0xFF;
    return (uint8_t)d;
}

/* (ux - c)^2 + (uy - c)^2 for ux, uy in 0..2c (c <= 7). One call per statement: two calls in
 * one expression were miscompiled by SDCC here (the first result was lost). */
static uint8_t dist2(uint8_t ux, uint8_t uy, uint8_t c)
{
    uint8_t a, b;
    a = fold(ux, c);
    a = sq[a];
    b = fold(uy, c);
    b = sq[b];
    return (uint8_t)(a + b);
}

/* major offset of breakpoint k (0..n+1): round(k * dmaj / (n + 1)) via the 8.8 step q, with two
 * 8x8 multiplies (exact and identical on SDCC and gcc) */
static uint8_t road_x(const w_road_t *r, uint8_t k)
{
    uint16_t a, b;
    if (k > r->n) return r->dmaj;
    a = (uint16_t)((uint16_t)k * (uint8_t)(r->q >> 8));
    b = (uint16_t)((uint16_t)k * (uint8_t)r->q);
    b = (uint16_t)(b + 128);
    return (uint8_t)(a + (b >> 8));
}

static uint8_t on_road(const w_road_t *r, uint16_t mx, uint16_t my)
{
    uint16_t maj, mn, t;
    uint8_t k, o;
    if (r->flags & W_R_YMAJOR) { maj = my; mn = mx; } else { maj = mx; mn = my; }
    t = (r->flags & W_R_MINNEG) ? (uint16_t)(r->a_min - mn) : (uint16_t)(mn - r->a_min);
    if (t > r->dmin) return 0;
    maj = (r->flags & W_R_MAJNEG) ? (uint16_t)(r->a_maj - maj) : (uint16_t)(maj - r->a_maj);
    if (maj > 255) return 0;
    o = (uint8_t)maj;
    if ((uint8_t)t == r->dmin) k = r->n;
    else {
        k = (uint8_t)((uint8_t)t >> r->shift);
        if ((uint8_t)t & (uint8_t)((1u << r->shift) - 1)) return o == road_x(r, (uint8_t)(k + 1));
    }
    if (o < road_x(r, k)) return 0;
    return o <= road_x(r, (uint8_t)(k + 1));
}

/* set piece at one cell, or W_SP_NONE / W_SP_CLEAR */
static uint8_t piece(uint16_t mx, uint16_t my, uint8_t mask)
{
    uint8_t i, d2, ux, uy;
    const w_road_t *r;
    for (i = 0; i < NUM_BEACONS; i++) {
        if (!(mask & (uint8_t)(W_SPM_BEACON0 << i))) continue;
        ux = box_off(mx, world.beacon[i].x, 6);
        if (ux == 0xFF) continue;
        uy = box_off(my, world.beacon[i].y, 6);
        if (uy == 0xFF) continue;
        d2 = dist2(ux, uy, 6);
        if (d2 > 34) continue;
        if (d2 == 0) return MT_BEACON;
        if (i < 2 && mx == world.shrine[i].x && my == world.shrine[i].y) return MT_SHRINE;
        if (d2 <= 2) return MT_RUIN_FLOOR;
        if (d2 <= 13) {   /* the plateau */
            if (i == 0) return MT_GRASS;
            if (i == 1) return MT_SAND;
            return (world_detail(mx, my) & 3) ? MT_SNOW : MT_RUIN_FLOOR;
        }
        /* the gate ring */
        if (i == 0) return MT_BRAMBLE;
        if (i == 1) return MT_SHALLOW;
        return MT_ROCK;
    }
    if (mask & W_SPM_OTHER) {
        ux = box_off(mx, world.heart.x, 4);
        uy = box_off(my, world.heart.y, 4);
        if (ux != 0xFF && uy != 0xFF) {
            d2 = dist2(ux, uy, 4);
            if (d2 == 0) return MT_HEART;
            if (d2 <= 20) return (world_detail(mx, my) & 1) ? MT_GLASS : MT_ASH;
        }
        if (mx == world.start.x && my == world.start.y) return MT_FIRE_COLD;
    }
    for (i = 0; i < W_NUM_ROADS; i++) {
        if (!(mask & (uint8_t)(W_SPM_ROAD0 << i))) continue;
        r = &w_roads[i];
        if ((uint16_t)(mx - r->bx) <= r->bw && (uint16_t)(my - r->by) <= r->bh && on_road(r, mx, my))
            return world_detail(mx, my) < 24 ? MT_RUIN_FLOOR : MT_ROAD;
    }
    if (!(mask & W_SPM_OTHER)) return W_SP_NONE;
    ux = box_off(mx, world.start.x, 3);
    uy = box_off(my, world.start.y, 3);
    if (ux != 0xFF && uy != 0xFF && dist2(ux, uy, 3) <= 10) return W_SP_CLEAR;
    return W_SP_NONE;
}

/* does the 4x4 block at (kx, ky) touch the box of radius rad around c? */
static uint8_t box_hit(uint16_t kx, uint16_t ky, const wpos_t *c, uint8_t rad)
{
    uint16_t d;
    d = (uint16_t)(c->x - kx);
    d = (uint16_t)(d + rad);
    if (d > (uint16_t)(rad + rad + 3)) return 0;
    d = (uint16_t)(c->y - ky);
    d = (uint16_t)(d + rad);
    return d <= (uint16_t)(rad + rad + 3);
}

/* can road r pass through the 4x4 block at (kx, ky)? (conservative) */
static uint8_t road_hits_block(const w_road_t *r, uint16_t kx, uint16_t ky)
{
    uint16_t maj, mn, t0, t1, o0;
    uint8_t k0, k1, a, b;
    if (r->flags & W_R_YMAJOR) { maj = ky; mn = kx; } else { maj = kx; mn = ky; }
    /* minor range of the block, as road parameter t */
    if (r->flags & W_R_MINNEG) {
        t0 = (uint16_t)(r->a_min - (uint16_t)(mn + 3));
        t1 = (uint16_t)(r->a_min - mn);
    } else {
        t0 = (uint16_t)(mn - r->a_min);
        t1 = (uint16_t)(t0 + 3);
    }
    if ((int16_t)t1 < 0 || (int16_t)t0 > (int16_t)r->dmin) return 0;
    if ((int16_t)t0 < 0) t0 = 0;
    if (t1 > r->dmin) t1 = r->dmin;
    k0 = (uint8_t)((uint8_t)t0 >> r->shift);
    k1 = (uint8_t)((uint8_t)t1 >> r->shift);
    if ((uint8_t)t1 == r->dmin) k1 = r->n;
    a = road_x(r, k0);
    b = road_x(r, (uint8_t)(k1 + 1));
    /* major range of the block, as offset o from a_maj */
    if (r->flags & W_R_MAJNEG) o0 = (uint16_t)(r->a_maj - (uint16_t)(maj + 3));
    else o0 = (uint16_t)(maj - r->a_maj);
    if ((int16_t)o0 < -3) return 0;
    if ((int16_t)o0 > (int16_t)b) return 0;
    return (int16_t)(o0 + 3) >= (int16_t)a;
}

/* road cells of the 4x4 block at (kx, ky): bit (fy << 2) | fx */
uint16_t w_block_roads(uint16_t kx, uint16_t ky, uint8_t mask) WBANKED
{
    uint16_t bits = 0, maj0, mn0, t;
    uint8_t i, a, b, j, f, o;
    const w_road_t *r;
    for (i = 0; i < W_NUM_ROADS; i++) {
        if (!(mask & (uint8_t)(W_SPM_ROAD0 << i))) continue;
        r = &w_roads[i];
        if (r->flags & W_R_YMAJOR) { maj0 = ky; mn0 = kx; } else { maj0 = kx; mn0 = ky; }
        for (j = 0; j < 4; j++) {          /* the 4 minor coordinates of the block */
            t = (r->flags & W_R_MINNEG) ? (uint16_t)(r->a_min - (uint16_t)(mn0 + j)) : (uint16_t)((uint16_t)(mn0 + j) - r->a_min);
            if (t > r->dmin) continue;
            if ((uint8_t)t == r->dmin) { a = road_x(r, r->n); b = r->dmaj; }
            else {
                f = (uint8_t)((uint8_t)t >> r->shift);
                b = road_x(r, (uint8_t)(f + 1));
                a = ((uint8_t)t & (uint8_t)((1u << r->shift) - 1)) ? b : road_x(r, f);
            }
            for (f = 0; f < 4; f++) {      /* the 4 major coordinates */
                t = (r->flags & W_R_MAJNEG) ? (uint16_t)(r->a_maj - (uint16_t)(maj0 + f)) : (uint16_t)((uint16_t)(maj0 + f) - r->a_maj);
                if (t > 255) continue;
                o = (uint8_t)t;
                if (o < a || o > b) continue;
                if (r->flags & W_R_YMAJOR) bits |= (uint16_t)(1u << ((f << 2) | j));
                else bits |= (uint16_t)(1u << ((j << 2) | f));
            }
        }
    }
    return bits;
}

uint8_t w_block_mask(uint16_t kx, uint16_t ky, uint8_t mask) WBANKED
{
    uint8_t i, m = 0;
    for (i = 0; i < NUM_BEACONS; i++)
        if ((mask & (uint8_t)(W_SPM_BEACON0 << i)) && box_hit(kx, ky, &world.beacon[i], 6))
            m |= (uint8_t)(W_SPM_BEACON0 << i);
    if ((mask & W_SPM_OTHER) && (box_hit(kx, ky, &world.heart, 4) || box_hit(kx, ky, &world.start, 3)))
        m |= W_SPM_OTHER;
    for (i = 0; i < W_NUM_ROADS; i++)
        if ((mask & (uint8_t)(W_SPM_ROAD0 << i)) && road_hits_block(&w_roads[i], kx, ky))
            m |= (uint8_t)(W_SPM_ROAD0 << i);
    return m;
}

uint8_t w_piece(uint16_t mx, uint16_t my, uint8_t mask) WBANKED
{
    W_OP(W_OP_PIECE);
    return piece(mx, my, mask);
}

#define SALT_R 0x6E   /* ruin room per 8x8 cell */

/* ruins: small rectangular rooms, colonnades and statue fragments, one pattern per 8x8 cell */
uint8_t w_ruin(uint16_t mx, uint16_t my, uint8_t d, uint8_t ground) WBANKED
{
    W_OP(W_OP_RUIN);
    uint8_t r = w_hash_s(mx >> 3, my >> 3, SALT_R);
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

/* is there an ancient cairn at (mx,my)? (the caller checked the 4-alignment with the start) */
uint8_t w_old_cairn(uint16_t mx, uint16_t my) WBANKED
{
    W_OP(W_OP_CAIRN);
    int16_t ox = (int16_t)((int16_t)(mx - world.start.x) >> 2);
    int16_t oy = (int16_t)((int16_t)(my - world.start.y) >> 2);
    uint8_t t;
    if (ox < -128 || ox > 127 || oy < -128 || oy > 127) return 0;
    for (t = 0; t < world_old_cairn_count; t++)
        if (world_old_cairns[t].dx == (int8_t)ox && world_old_cairns[t].dy == (int8_t)oy) return 1;
    return 0;
}

/* ---- noise fields at lattice points ----------------------------------------------------- */
#define SALT_E  0x00         /* elevation, 16 grid */
#define SALT_F  0x3C         /* elevation, 4 grid */
#define SALT_M  0x95         /* moisture, 32 grid */
#define SALT_S  0xC6         /* strangeness, 64 grid */
#define SALT_C  0xE9         /* continents: elevation, 64 grid */
#define HV(a) ((uint8_t)((uint8_t)(a) >> 1))
#define QV(a) ((uint8_t)((uint8_t)(a) >> 2))

/* Two cached lattice cells per octave, slot (kx ^ ky) & 1 (so horizontally or vertically
 * adjacent cells never evict each other), each with its rows interpolated at the last fx.
 * Always horizontal first: the rounding makes the order matter, and results must not depend
 * on the cache state. Written out per slot with macros: SDCC compiles plain globals far
 * better than struct pointers or indexed arrays. */
#define OCT_VALS(o) \
    static uint8_t o##_a0, o##_b0, o##_c0, o##_d0, o##_f0, o##_t0, o##_u0; \
    static uint8_t o##_a1, o##_b1, o##_c1, o##_d1, o##_f1, o##_t1, o##_u1;
#define OCT_DECL(o) static uint16_t o##_x0, o##_y0, o##_x1, o##_y1; OCT_VALS(o)
OCT_DECL(oc)   /* continents, 64 grid */
OCT_VALS(os)   /* strangeness, 64 grid (same keys as oc) */
OCT_DECL(om)   /* moisture, 32 grid */
OCT_DECL(oe)   /* elevation, 16 grid */

static uint16_t mp_cx, mp_mx, mp_ex;
void w_lattice_reset(void) WBANKED
{
    oc_x0 = oc_x1 = om_x0 = om_x1 = oe_x0 = oe_x1 = 0xFFFF;
    mp_cx = mp_mx = mp_ex = 0xFFFF;
}

/* inner three rounds of w_hash (shared by all salts) */
static uint8_t hash3(uint16_t x, uint16_t y)
{
    uint8_t h = w_perm[(uint8_t)((uint8_t)x ^ w_s0)];
    h = w_perm[(uint8_t)(h ^ (uint8_t)y)];
    return (uint8_t)(w_perm[(uint8_t)(h ^ (uint8_t)(x >> 8) ^ w_s1)] ^ (uint8_t)(y >> 8));
}

static uint16_t okx, oky;
static uint8_t w_la, w_lb, w_lc, w_ld, w_sa, w_sb, w_sc, w_sd;
static uint8_t ofx, ofy;

/* 4 corner hashes of cell (okx, oky) with salt -> a, b, c, d */
#define OCT_FILL(o, j, salt) do { \
    w_salt = (salt); \
    o##_a##j = w_hash(okx, oky); \
    o##_b##j = w_hash((uint16_t)(okx + 1), oky); \
    o##_c##j = w_hash(okx, (uint16_t)(oky + 1)); \
    o##_d##j = w_hash((uint16_t)(okx + 1), (uint16_t)(oky + 1)); \
    o##_f##j = 0xFF; } while (0)
/* value at (ofx, ofy) (n-bit fractions) of slot j */
#define OCT_VAL(o, j, n, out) do { \
    w_ln = (n); \
    if (ofx != o##_f##j) { \
        o##_f##j = ofx; \
        w_lf = ofx; \
        o##_t##j = w_lerpn(o##_a##j, o##_b##j); \
        o##_u##j = w_lerpn(o##_c##j, o##_d##j); \
    } \
    w_lf = ofy; \
    out = w_lerpn(o##_t##j, o##_u##j); } while (0)
/* the single-field octaves */
#define OCT(o, salt, n, out) do { \
    if (((uint8_t)okx ^ (uint8_t)oky) & 1) { \
        if (o##_x1 != okx || o##_y1 != oky) { o##_x1 = okx; o##_y1 = oky; OCT_FILL(o, 1, salt); } \
        OCT_VAL(o, 1, n, out); \
    } else { \
        if (o##_x0 != okx || o##_y0 != oky) { o##_x0 = okx; o##_y0 = oky; OCT_FILL(o, 0, salt); } \
        OCT_VAL(o, 0, n, out); \
    } } while (0)

/* continents and strangeness: one cell, shared inner hash */
static void cs_fill(void)
{
    uint8_t h;
    h = hash3(okx, oky);
    w_la = w_perm[h ^ SALT_C]; w_sa = w_perm[h ^ SALT_S];
    h = hash3((uint16_t)(okx + 1), oky);
    w_lb = w_perm[h ^ SALT_C]; w_sb = w_perm[h ^ SALT_S];
    h = hash3(okx, (uint16_t)(oky + 1));
    w_lc = w_perm[h ^ SALT_C]; w_sc = w_perm[h ^ SALT_S];
    h = hash3((uint16_t)(okx + 1), (uint16_t)(oky + 1));
    w_ld = w_perm[h ^ SALT_C]; w_sd = w_perm[h ^ SALT_S];
}

/* Field values at a 4-metatile lattice point (lx, ly) = (mx >> 2, my >> 2):
 * elevation = continents (64 grid) / 2 + elevation (16 grid) * 3/8 + detail (4 grid) / 8,
 * moisture (32 grid), strangeness (64 grid); value noise, bilinear. */
void w_lattice(uint16_t lx, uint16_t ly) WBANKED
{
    uint8_t c, k, xl = (uint8_t)lx, yl = (uint8_t)ly;
    W_OP(W_OP_LATTICE);
    okx = lx >> 4;
    oky = ly >> 4;
    ofx = xl & 15;
    ofy = yl & 15;
    if (((uint8_t)okx ^ (uint8_t)oky) & 1) {
        if (oc_x1 != okx || oc_y1 != oky) {
            oc_x1 = okx; oc_y1 = oky;
            cs_fill();
            oc_a1 = w_la; oc_b1 = w_lb; oc_c1 = w_lc; oc_d1 = w_ld; oc_f1 = 0xFF;
            os_a1 = w_sa; os_b1 = w_sb; os_c1 = w_sc; os_d1 = w_sd; os_f1 = 0xFF;
        }
        OCT_VAL(oc, 1, 4, k);
        OCT_VAL(os, 1, 4, w_ls);
    } else {
        if (oc_x0 != okx || oc_y0 != oky) {
            oc_x0 = okx; oc_y0 = oky;
            cs_fill();
            oc_a0 = w_la; oc_b0 = w_lb; oc_c0 = w_lc; oc_d0 = w_ld; oc_f0 = 0xFF;
            os_a0 = w_sa; os_b0 = w_sb; os_c0 = w_sc; os_d0 = w_sd; os_f0 = 0xFF;
        }
        OCT_VAL(oc, 0, 4, k);
        OCT_VAL(os, 0, 4, w_ls);
    }
    okx = lx >> 3;
    oky = ly >> 3;
    ofx = xl & 7;
    ofy = yl & 7;
    OCT(om, SALT_M, 3, w_lm);
    okx = lx >> 2;
    oky = ly >> 2;
    ofx = xl & 3;
    ofy = yl & 3;
    OCT(oe, SALT_E, 2, c);
    k = (uint8_t)(HV(k) + QV(c) + (c >> 3));
    w_salt = SALT_F;
    w_le = (uint8_t)(k + (w_hash(lx, ly) >> 3));
}

/* Fields at an 8-metatile grid point (px, py) = (mx >> 3, my >> 3), for the map. Close to
 * w_lattice(2 px, 2 py) but interpolated vertically first, with the columns cached, so a map
 * scanned in rows costs about one lerp per octave per sample. Not bit-exact with the terrain
 * (rounding), which is fine for a map. Sets w_le, w_lm, w_ls. */
static uint16_t mp_cy, mp_my, mp_ey;
static uint8_t mp_c[4], mp_s[4], mp_m[4], mp_e[4];
static uint8_t mc_fy, mc_l, mc_r, ms_l, ms_r, mm_fy, mm_l, mm_r, me_fy, me_l, me_r;

void w_map_point(uint16_t px, uint16_t py) WBANKED
{
    uint16_t kx = px >> 3, ky = py >> 3;
    uint8_t f, c, e;
    if (kx != mp_cx || ky != mp_cy) {
        mp_cx = kx;
        mp_cy = ky;
        okx = kx;
        oky = ky;
        cs_fill();
        mp_c[0] = w_la; mp_c[1] = w_lb; mp_c[2] = w_lc; mp_c[3] = w_ld;
        mp_s[0] = w_sa; mp_s[1] = w_sb; mp_s[2] = w_sc; mp_s[3] = w_sd;
        mc_fy = 0xFF;
    }
    w_ln = 4;
    f = (uint8_t)(((uint8_t)py & 7) << 1);
    if (f != mc_fy) {
        mc_fy = f;
        w_lf = f;
        mc_l = w_lerpn(mp_c[0], mp_c[2]); mc_r = w_lerpn(mp_c[1], mp_c[3]);
        ms_l = w_lerpn(mp_s[0], mp_s[2]); ms_r = w_lerpn(mp_s[1], mp_s[3]);
    }
    w_lf = (uint8_t)(((uint8_t)px & 7) << 1);
    c = w_lerpn(mc_l, mc_r);
    w_ls = w_lerpn(ms_l, ms_r);

    kx = px >> 2;
    ky = py >> 2;
    if (kx != mp_mx || ky != mp_my) {
        mp_mx = kx;
        mp_my = ky;
        w_salt = SALT_M;
        mp_m[0] = w_hash(kx, ky); mp_m[1] = w_hash((uint16_t)(kx + 1), ky);
        mp_m[2] = w_hash(kx, (uint16_t)(ky + 1)); mp_m[3] = w_hash((uint16_t)(kx + 1), (uint16_t)(ky + 1));
        mm_fy = 0xFF;
    }
    w_ln = 3;
    f = (uint8_t)(((uint8_t)py & 3) << 1);
    if (f != mm_fy) {
        mm_fy = f;
        w_lf = f;
        mm_l = w_lerpn(mp_m[0], mp_m[2]); mm_r = w_lerpn(mp_m[1], mp_m[3]);
    }
    w_lf = (uint8_t)(((uint8_t)px & 3) << 1);
    w_lm = w_lerpn(mm_l, mm_r);

    kx = px >> 1;
    ky = py >> 1;
    if (kx != mp_ex || ky != mp_ey) {
        mp_ex = kx;
        mp_ey = ky;
        w_salt = SALT_E;
        mp_e[0] = w_hash(kx, ky); mp_e[1] = w_hash((uint16_t)(kx + 1), ky);
        mp_e[2] = w_hash(kx, (uint16_t)(ky + 1)); mp_e[3] = w_hash((uint16_t)(kx + 1), (uint16_t)(ky + 1));
        me_fy = 0xFF;
    }
    w_ln = 2;
    f = (uint8_t)(((uint8_t)py & 1) << 1);
    if (f != me_fy) {
        me_fy = f;
        w_lf = f;
        me_l = w_lerpn(mp_e[0], mp_e[2]); me_r = w_lerpn(mp_e[1], mp_e[3]);
    }
    w_lf = (uint8_t)(((uint8_t)px & 1) << 1);
    e = w_lerpn(me_l, me_r);
    c = (uint8_t)(HV(c) + QV(e) + (e >> 3));
    w_salt = SALT_F;
    w_le = (uint8_t)(c + (w_hash((uint16_t)(px << 1), (uint16_t)(py << 1)) >> 3));
}

/* ---- points of interest: one hash roll per 16x16-metatile cell --------------------------- */
/* the POI's metatile at offset (ax, ay) = |cell - poi|, or 0xFF (d = detail hash) */
uint8_t w_poi_mt(uint8_t ax, uint8_t ay, uint8_t d) WBANKED
{
    uint8_t t;
    if (w_pq_type == W_POI_ROAD) {
        if (w_pq_axis ? ax == 0 : ay == 0) return d < 40 ? MT_RUIN_FLOOR : MT_ROAD;
        return 0xFF;
    }
    if (ax > 2 || ay > 2) return 0xFF;
    t = (uint8_t)(sq[ax] + sq[ay]);
    if (t == 0) {
        switch (w_pq_type) {
        case W_POI_FIRE: return MT_FIRE_COLD;
        case W_POI_MONOLITH: return MT_MONOLITH;
        case W_POI_TABLE: return MT_TABLE;
        case W_POI_WELL: return MT_WELL;
        default: return MT_STATUE_HAND;
        }
    }
    if (t > 5 || (w_pq_type == W_POI_MONOLITH && t > 2)) return 0xFF;
    if (w_pq_type == W_POI_WELL && t <= 2) return MT_RUIN_FLOOR;
    if (w_pq_type == W_POI_HAND) {
        if (t == 1 && (d & 1)) return MT_STATUE_HAND;
        if (d < 100) return MT_RUIN_FLOOR;
    }
    return w_pq_ground;   /* a small clearing */
}

/* ---- mods -------------------------------------------------------------------------------- */
void world_mods_rebuild(void) WBANKED
{
    uint8_t i, b;
    if (world_mod_count > MAX_MODS) world_mod_count = MAX_MODS;
    for (i = 0; i < W_MOD_BUCKETS; i++) w_mod_head[i] = W_MOD_NONE;
    for (i = 0; i < world_mod_count; i++) {
        b = w_mod_bucket(world_mods[i].x, world_mods[i].y);
        w_mod_next[i] = w_mod_head[b];
        w_mod_head[b] = i;
    }
    w_mods_seen = world_mod_count;
    w_blocks_reset();
}

void world_mods_clear(void) WBANKED
{
    world_mod_count = 0;
    world_mods_rebuild();
}

uint8_t world_mod_set(uint16_t mx, uint16_t my, uint8_t mt) WBANKED
{
    uint16_t kx = mx & 0xFFFC, ky = my & 0xFFFC;
    uint8_t i, s, b;
    if (w_mods_seen != world_mod_count) world_mods_rebuild();
    i = w_mod_head[w_mod_bucket(mx, my)];
    while (i != W_MOD_NONE && (world_mods[i].x != mx || world_mods[i].y != my)) i = w_mod_next[i];
    if (i == W_MOD_NONE) {
        if (world_mod_count >= MAX_MODS) return 0;
        i = world_mod_count;
        world_mods[i].x = mx;
        world_mods[i].y = my;
        b = w_mod_bucket(mx, my);
        w_mod_next[i] = w_mod_head[b];
        w_mod_head[b] = i;
        w_mods_seen = ++world_mod_count;
    }
    world_mods[i].mt = mt;
    s = w_bslot(kx, ky);   /* forget the cached cell */
    if (w_bcx[s] == kx && w_bcy[s] == ky) {
        b = (uint8_t)((((uint8_t)my & 3) << 2) | ((uint8_t)mx & 3));
        w_bcv[s][b >> 3] &= (uint8_t)~w_bitmask[b & 7];
    }
    return 1;
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
 * metatiles along the minor axis (both deltas must be <= 255). Step 4 (shift 2) normally, 8 for
 * steep long roads (at most 40 steps). */
static uint8_t road_build(w_road_t *r, const wpos_t *a, const wpos_t *b)
{
    int16_t dx = (int16_t)(b->x - a->x), dy = (int16_t)(b->y - a->y);
    uint16_t adx = uabs16(dx), ady = uabs16(dy), dmaj, dmn;
    uint8_t n, sh;
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
        if (n <= 40) break;
    }
    r->shift = sh;
    r->n = n;
    r->dmaj = (uint8_t)dmaj;
    r->q = (uint16_t)(((uint16_t)dmaj << 8) / (n + 1u));   /* major advance per step, 8.8 */
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
    uint16_t m0, m1, n0, n1, t;
    t = k == r->n ? r->dmin : (uint16_t)((uint16_t)k << r->shift);
    n0 = (r->flags & W_R_MINNEG) ? (uint16_t)(r->a_min - t) : (uint16_t)(r->a_min + t);
    if (leg) {
        t = (uint8_t)(k + 1) == r->n ? r->dmin : (uint16_t)((uint16_t)(k + 1) << r->shift);
        n1 = (r->flags & W_R_MINNEG) ? (uint16_t)(r->a_min - t) : (uint16_t)(r->a_min + t);
        m0 = m1 = road_x(r, (uint8_t)(k + 1));
    } else {
        n1 = n0;
        m0 = road_x(r, k);
        m1 = road_x(r, (uint8_t)(k + 1));
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


/* mark the coarse filter cells touched by the rectangle (gx0,gy0)-(gx1,gy1) with bits */
static void sp_mark(uint8_t bits)
{
    uint16_t cx, cy, cx0 = (uint16_t)(gx0 - w_spx0) >> 7, cx1 = (uint16_t)(gx1 - w_spx0) >> 7;
    uint16_t cy0 = (uint16_t)(gy0 - w_spy0) >> 7, cy1 = (uint16_t)(gy1 - w_spy0) >> 7;
    for (cy = cy0; cy <= cy1 && cy < 8; cy++)
        for (cx = cx0; cx <= cx1 && cx < 8; cx++)
            w_sp_mask[(cy << 3) | cx] |= bits;
}

static void sp_mark_box(const wpos_t *c, uint8_t rad, uint8_t bits)
{
    gx0 = (uint16_t)(c->x - rad); gx1 = (uint16_t)(c->x + rad);
    gy0 = (uint16_t)(c->y - rad); gy1 = (uint16_t)(c->y + rad);
    sp_mark(bits);
}

static void sp_prepare(void)
{
    uint8_t i, k, leg;
    const w_road_t *r;
    w_spx0 = (uint16_t)((world.start.x - 512) & 0xFF80);   /* aligned: filter cells = m & ~127 */
    w_spy0 = (uint16_t)((world.start.y - 512) & 0xFF80);
    for (i = 0; i < 64; i++) w_sp_mask[i] = 0;
    for (i = 0; i < NUM_BEACONS; i++) sp_mark_box(&world.beacon[i], 6, (uint8_t)(W_SPM_BEACON0 << i));
    sp_mark_box(&world.heart, 4, W_SPM_OTHER);
    sp_mark_box(&world.start, 3, W_SPM_OTHER);
    for (i = 0; i < W_NUM_ROADS; i++) {
        r = &w_roads[i];
        for (k = 0; k <= r->n; k++)
            for (leg = 0; leg < 2; leg++) {
                if (leg && k == r->n) break;
                road_seg(r, k, leg);
                sp_mark((uint8_t)(W_SPM_ROAD0 << i));
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
    c.x = (uint16_t)(32768u - 32 + (rnd() & 60));   /* one rnd() per statement (see below) */
    c.y = (uint16_t)(32768u - 32 + (rnd() & 60));   /* multiples of 4: lattice points */
    world.start = c;
    best = 0;
    for (r = 0; r < 160 && best < 2; r += 4) {
        for (oy = (int16_t)-r; oy <= r && best < 2; oy += 4) {
            for (ox = (int16_t)-r; ox <= r; ox += (oy == -r || oy == r || !r) ? 4 : (int16_t)(2 * r)) {
                g.x = (uint16_t)(c.x + ox);
                g.y = (uint16_t)(c.y + oy);
                w_lattice(g.x >> 2, g.y >> 2);   /* exact at lattice points, and cheap */
                b = w_classify(w_le, w_lm, w_ls);
                if (b == B_MEADOW) { world.start = g; best = 2; break; }
                if (!best && land_biome_ok(b)) { world.start = g; best = 1; }
                if (!r) break;
            }
        }
    }
    w_lattice(world.start.x >> 2, world.start.y >> 2);
    w_start_ground = biome_ground0[w_classify(w_le, w_lm, w_ls)];

    for (tries = 0; tries < 64; tries++) {
        uint8_t hb;
        uint16_t hdist, adx, ady;
        /* NOTE: one rnd() per statement. C leaves the evaluation order of operands unspecified
         * and SDCC and gcc differ, so two rnd() calls in one expression break host/ROM parity. */
        for (i = 0; i < NUM_BEACONS; i++) {
            uint8_t bear;
            uint16_t dist;
            bear = (uint8_t)(i * 85 - 8);
            bear = (uint8_t)(bear + (rnd() & 15));
            dist = (uint16_t)(90 + (rnd() & 63));
            dist = (uint16_t)(dist + (rnd() & 7));
            polar(&world.beacon[i], bear, dist);
        }
        hb = (uint8_t)(rnd() % 3);
        hb = (uint8_t)(43 - 8 + hb * 85);
        hb = (uint8_t)(hb + (rnd() & 15));
        hdist = (uint16_t)(200 + (rnd() & 31));
        hdist = (uint16_t)(hdist + (rnd() & 15));
        polar(&world.heart, hb, hdist);
        /* roads: start -> the gate point of each beacon (6 out along the road's major axis) */
        for (i = 0; i < NUM_BEACONS; i++) {
            int16_t dx = (int16_t)(world.beacon[i].x - world.start.x);
            int16_t dy = (int16_t)(world.beacon[i].y - world.start.y);
            g = world.beacon[i];
            world.shrine[i] = world.beacon[i];
            adx = uabs16(dx);
            ady = uabs16(dy);
            if (adx >= ady) {
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
