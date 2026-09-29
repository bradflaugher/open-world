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

/* set piece at one cell, or W_SP_NONE / W_SP_CLEAR */
static uint8_t piece(uint16_t mx, uint16_t my)
{
    uint16_t ux, uy;
    uint8_t i, d2;
    for (i = 0; i < NUM_BEACONS; i++) {
        ux = (uint16_t)(mx - world.beacon[i].x + 6);
        uy = (uint16_t)(my - world.beacon[i].y + 6);
        if (ux >= 13 || uy >= 13) continue;
        d2 = (uint8_t)(sq[fold((uint8_t)ux, 6)] + sq[fold((uint8_t)uy, 6)]);
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
    if (ux < 7 && uy < 7 && (uint8_t)(sq[fold((uint8_t)ux, 3)] + sq[fold((uint8_t)uy, 3)]) <= 10)
        return W_SP_CLEAR;
    return W_SP_NONE;
}

static uint8_t box_hit(uint16_t kx, uint16_t ky, const wpos_t *c, uint8_t rad)
{
    /* does the 4x4 block at (kx,ky) touch the box of radius rad around c? */
    return (uint16_t)(c->x + rad - kx) <= (uint16_t)(2 * rad + 3) &&
           (uint16_t)(c->y + rad - ky) <= (uint16_t)(2 * rad + 3);
}

uint8_t w_pieces(uint16_t kx, uint16_t ky, uint8_t *out) WBANKED
{
    uint8_t i, any = 0, fx, fy;
    for (i = 0; i < NUM_BEACONS; i++)
        if (box_hit(kx, ky, &world.beacon[i], 6)) any = 1;
    if (box_hit(kx, ky, &world.heart, 4) || box_hit(kx, ky, &world.start, 3)) any = 1;
    for (i = 0; i < W_NUM_ROADS; i++) {
        const w_road_t *r = &w_roads[i];
        if ((uint16_t)(kx + 3 - r->bx) <= (uint16_t)(r->bw + 3) && (uint16_t)(ky + 3 - r->by) <= (uint16_t)(r->bh + 3))
            any = 1;
    }
    if (!any) return 0;
    any = 0;
    for (fy = 0; fy < 4; fy++)
        for (fx = 0; fx < 4; fx++) {
            i = piece((uint16_t)(kx + fx), (uint16_t)(ky + fy));
            *out++ = i;
            if (i != W_SP_NONE) any = 1;
        }
    return any;
}

#define SALT_R 0x6E   /* ruin room per 8x8 cell */

/* ruins: small rectangular rooms, colonnades and statue fragments, one pattern per 8x8 cell */
uint8_t w_ruin(uint16_t mx, uint16_t my, uint8_t d, uint8_t ground) WBANKED
{
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
#define SALT_P  0xA3         /* point of interest per 16x16 cell */
#define SALT_P2 0x17
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

/* coarse-octave lattice hashes, one cached cell per octave */
static uint16_t kc_x, kc_y, ke_x, ke_y, km_x, km_y;
static uint8_t cc[4], cs[4], ch[4], cw[4];   /* continents 64, strangeness 64, elevation 16, moisture 32 */
static uint8_t kc_fx, hc_t, hc_b, hs_t, hs_b; /* 64-grid horizontal lerps for the last fx */

void w_lattice_reset(void) WBANKED
{
    kc_x = kc_y = ke_x = ke_y = km_x = km_y = 0xFFFF;
}

static void corners4(uint8_t *c, uint16_t kx, uint16_t ky, uint8_t salt)
{
    uint16_t kx1 = (uint16_t)(kx + 1), ky1 = (uint16_t)(ky + 1);
    w_salt = salt;
    c[0] = w_hash(kx, ky);
    c[1] = w_hash(kx1, ky);
    c[2] = w_hash(kx, ky1);
    c[3] = w_hash(kx1, ky1);
}

/* Field values at a 4-metatile lattice point (lx, ly) = (mx >> 2, my >> 2):
 * elevation = continents (64 grid) / 2 + elevation (16 grid) * 3/8 + detail (4 grid) / 8,
 * moisture (32 grid), strangeness (64 grid); value noise, bilinear. */
void w_lattice(uint16_t lx, uint16_t ly) WBANKED
{
    uint8_t c, k, fx, fy;
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
    fy = (uint8_t)ly & 15;
    k = lerpn(hc_t, hc_b, fy, 4);
    w_ls = lerpn(hs_t, hs_b, fy, 4);

    kx = lx >> 3;
    ky = ly >> 3;
    if (kx != km_x || ky != km_y) {
        km_x = kx;
        km_y = ky;
        corners4(cw, kx, ky, SALT_M);
    }
    fx = (uint8_t)lx & 7;
    fy = (uint8_t)ly & 7;
    w_lm = lerpn(lerpn(cw[0], cw[1], fx, 3), lerpn(cw[2], cw[3], fx, 3), fy, 3);

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
    w_le = (uint8_t)(k + (w_hash(lx, ly) >> 3));
}

/* ---- points of interest: one hash roll per 16x16-metatile cell --------------------------- */
void w_poi_roll(uint16_t cx, uint16_t cy) WBANKED
{
    uint8_t h, h2;
    w_pq_x = cx;
    w_pq_y = cy;
    w_pq_ok = 0;
    h = w_hash_s(cx, cy, SALT_P);
    h2 = w_hash_s(cx, cy, SALT_P2);
    w_pq_px = (uint8_t)(3 + (h2 & 7) + ((h2 >> 6) & 1));
    w_pq_py = (uint8_t)(3 + ((h2 >> 3) & 7) + (h2 >> 7));
    if (h < 86) h = W_POI_FIRE;            /* ~1 in 3 cells: a cold campfire */
    else if (h < 100) h = W_POI_MONOLITH;
    else if (h < 105) h = W_POI_TABLE;     /* a table set for two */
    else if (h < 110) h = W_POI_WELL;
    else if (h < 114) h = W_POI_HAND;      /* a giant stone hand */
    else if (h < 120) h = W_POI_ROAD;      /* a road ending in the sea */
    else h = W_POI_NONE;
    w_pq_type = h;
}

/* lazy validation: the biome at the POI centre (px, py) (lattice value) */
void w_poi_check(uint16_t px, uint16_t py) WBANKED
{
    uint16_t lx = px >> 2, ly = py >> 2;
    uint8_t b, e0, e1;
    w_pq_ok = 1;
    w_lattice(lx, ly);
    b = w_classify(w_le, w_lm, w_ls);
    w_pq_ground = w_biome_ground[b];
    if (w_pq_type == W_POI_ROAD) {
        if (b != B_SHORE) { w_pq_type = W_POI_NONE; return; }
        w_lattice((uint16_t)(lx - 1), ly);
        e0 = w_le;
        w_lattice((uint16_t)(lx + 1), ly);
        e0 = fold(e0, w_le);
        w_lattice(lx, (uint16_t)(ly - 1));
        e1 = w_le;
        w_lattice(lx, (uint16_t)(ly + 1));
        e1 = fold(e1, w_le);
        w_pq_axis = e1 > e0;   /* the road runs along the steeper axis: inland to the sea */
        return;
    }
    if (b <= B_SHORE || b == B_ROCK || w_le < W_T_SHORE + 6) { w_pq_type = W_POI_NONE; return; }
    if (w_pq_type == W_POI_MONOLITH && b == B_ASH) w_pq_ground = MT_GLASS;
}

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
    const wmod_t *w = world_mods;
    if (world_mod_count > MAX_MODS) world_mod_count = MAX_MODS;
    for (i = 0; i < 16; i++) w_mod_bloom[i] = 0;
    for (i = world_mod_count; i; i--, w++) {
        b = w_mod_bit(w->x & 0xFFFC, w->y & 0xFFFC);
        w_mod_bloom[b >> 3] |= w_bitmask[b & 7];
    }
    w_mods_seen = world_mod_count;
    w_blocks_reset();
}

void world_mods_clear(void) WBANKED
{
    world_mod_count = 0;
    world_mods_rebuild();
}

static wmod_t *mod_find(uint16_t mx, uint16_t my)
{
    uint8_t i;
    wmod_t *w = world_mods;
    for (i = world_mod_count; i; i--, w++)
        if (w->x == mx && w->y == my) return w;
    return 0;
}

uint8_t world_mod_set(uint16_t mx, uint16_t my, uint8_t mt) WBANKED
{
    wmod_t *w;
    uint16_t kx = mx & 0xFFFC, ky = my & 0xFFFC;
    uint8_t s, b;
    if (w_mods_seen != world_mod_count) world_mods_rebuild();
    w = mod_find(mx, my);
    if (!w) {
        if (world_mod_count >= MAX_MODS) return 0;
        w = &world_mods[world_mod_count];
        w->x = mx;
        w->y = my;
        w_mods_seen = ++world_mod_count;
        b = w_mod_bit(kx, ky);
        w_mod_bloom[b >> 3] |= w_bitmask[b & 7];
    }
    w->mt = mt;
    s = w_bslot(kx, ky);   /* patch the cached block */
    if (w_bcx[s] == kx && w_bcy[s] == ky) w_bcm[s][(uint8_t)(((uint8_t)my & 3) << 2) | ((uint8_t)mx & 3)] = mt;
    return 1;
}

uint8_t world_mt_base(uint16_t mx, uint16_t my) WBANKED
{
    uint8_t t;
    if (!world_mod_count || !mod_find(mx, my)) return world_mt(mx, my);
    w_mods_off = 1;
    w_blocks_reset();
    t = world_mt(mx, my);
    w_mods_off = 0;
    w_blocks_reset();
    return t;
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


/* mark the coarse filter cells touched by the rectangle (gx0,gy0)-(gx1,gy1) */
static void sp_mark(void)
{
    uint16_t cx, cy, cx0 = (uint16_t)(gx0 - w_spx0) >> 6, cx1 = (uint16_t)(gx1 - w_spx0) >> 6;
    uint16_t cy0 = (uint16_t)(gy0 - w_spy0) >> 6, cy1 = (uint16_t)(gy1 - w_spy0) >> 6;
    for (cy = cy0; cy <= cy1 && cy < 16; cy++)
        for (cx = cx0; cx <= cx1 && cx < 16; cx++)
            w_sp_bits[(cy << 1) | (cx >> 3)] |= w_bitmask[cx & 7];
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
        uint16_t hdist;
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
