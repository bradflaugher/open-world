/* world_gen.c - OPEN WORLD world core, COLD PATH: world_init (layout, causeways, coarse filter)
 * and the bearing / distance helpers. Banked on the Game Boy; runs once per world.
 */
#ifdef __SDCC
#pragma bank 255
#endif
#define WORLD_INTERNAL
#include <string.h>
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
    /* scale up by doubling (a left shift of a negative value is undefined in C99) */
    while (u < 2048 && u > -2048 && v < 2048 && v > -2048) { u = (int16_t)(u * 2); v = (int16_t)(v * 2); }
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

/* floor(n^2 / 4) for n = 0..510: a * b = qsq[a + b] - qsq[|a - b|] (8 x 8 bits, exact) */
static const uint16_t qsq[511] = {
    0, 0, 1, 2, 4, 6, 9, 12, 16, 20, 25, 30, 36, 42, 49, 56,
    64, 72, 81, 90, 100, 110, 121, 132, 144, 156, 169, 182, 196, 210, 225, 240,
    256, 272, 289, 306, 324, 342, 361, 380, 400, 420, 441, 462, 484, 506, 529, 552,
    576, 600, 625, 650, 676, 702, 729, 756, 784, 812, 841, 870, 900, 930, 961, 992,
    1024, 1056, 1089, 1122, 1156, 1190, 1225, 1260, 1296, 1332, 1369, 1406, 1444, 1482, 1521, 1560,
    1600, 1640, 1681, 1722, 1764, 1806, 1849, 1892, 1936, 1980, 2025, 2070, 2116, 2162, 2209, 2256,
    2304, 2352, 2401, 2450, 2500, 2550, 2601, 2652, 2704, 2756, 2809, 2862, 2916, 2970, 3025, 3080,
    3136, 3192, 3249, 3306, 3364, 3422, 3481, 3540, 3600, 3660, 3721, 3782, 3844, 3906, 3969, 4032,
    4096, 4160, 4225, 4290, 4356, 4422, 4489, 4556, 4624, 4692, 4761, 4830, 4900, 4970, 5041, 5112,
    5184, 5256, 5329, 5402, 5476, 5550, 5625, 5700, 5776, 5852, 5929, 6006, 6084, 6162, 6241, 6320,
    6400, 6480, 6561, 6642, 6724, 6806, 6889, 6972, 7056, 7140, 7225, 7310, 7396, 7482, 7569, 7656,
    7744, 7832, 7921, 8010, 8100, 8190, 8281, 8372, 8464, 8556, 8649, 8742, 8836, 8930, 9025, 9120,
    9216, 9312, 9409, 9506, 9604, 9702, 9801, 9900, 10000, 10100, 10201, 10302, 10404, 10506, 10609, 10712,
    10816, 10920, 11025, 11130, 11236, 11342, 11449, 11556, 11664, 11772, 11881, 11990, 12100, 12210, 12321, 12432,
    12544, 12656, 12769, 12882, 12996, 13110, 13225, 13340, 13456, 13572, 13689, 13806, 13924, 14042, 14161, 14280,
    14400, 14520, 14641, 14762, 14884, 15006, 15129, 15252, 15376, 15500, 15625, 15750, 15876, 16002, 16129, 16256,
    16384, 16512, 16641, 16770, 16900, 17030, 17161, 17292, 17424, 17556, 17689, 17822, 17956, 18090, 18225, 18360,
    18496, 18632, 18769, 18906, 19044, 19182, 19321, 19460, 19600, 19740, 19881, 20022, 20164, 20306, 20449, 20592,
    20736, 20880, 21025, 21170, 21316, 21462, 21609, 21756, 21904, 22052, 22201, 22350, 22500, 22650, 22801, 22952,
    23104, 23256, 23409, 23562, 23716, 23870, 24025, 24180, 24336, 24492, 24649, 24806, 24964, 25122, 25281, 25440,
    25600, 25760, 25921, 26082, 26244, 26406, 26569, 26732, 26896, 27060, 27225, 27390, 27556, 27722, 27889, 28056,
    28224, 28392, 28561, 28730, 28900, 29070, 29241, 29412, 29584, 29756, 29929, 30102, 30276, 30450, 30625, 30800,
    30976, 31152, 31329, 31506, 31684, 31862, 32041, 32220, 32400, 32580, 32761, 32942, 33124, 33306, 33489, 33672,
    33856, 34040, 34225, 34410, 34596, 34782, 34969, 35156, 35344, 35532, 35721, 35910, 36100, 36290, 36481, 36672,
    36864, 37056, 37249, 37442, 37636, 37830, 38025, 38220, 38416, 38612, 38809, 39006, 39204, 39402, 39601, 39800,
    40000, 40200, 40401, 40602, 40804, 41006, 41209, 41412, 41616, 41820, 42025, 42230, 42436, 42642, 42849, 43056,
    43264, 43472, 43681, 43890, 44100, 44310, 44521, 44732, 44944, 45156, 45369, 45582, 45796, 46010, 46225, 46440,
    46656, 46872, 47089, 47306, 47524, 47742, 47961, 48180, 48400, 48620, 48841, 49062, 49284, 49506, 49729, 49952,
    50176, 50400, 50625, 50850, 51076, 51302, 51529, 51756, 51984, 52212, 52441, 52670, 52900, 53130, 53361, 53592,
    53824, 54056, 54289, 54522, 54756, 54990, 55225, 55460, 55696, 55932, 56169, 56406, 56644, 56882, 57121, 57360,
    57600, 57840, 58081, 58322, 58564, 58806, 59049, 59292, 59536, 59780, 60025, 60270, 60516, 60762, 61009, 61256,
    61504, 61752, 62001, 62250, 62500, 62750, 63001, 63252, 63504, 63756, 64009, 64262, 64516, 64770, 65025
};

static uint16_t mul8(uint8_t a, uint8_t b)
{
    uint16_t s = (uint16_t)((uint16_t)a + b);
    uint8_t d = a > b ? (uint8_t)(a - b) : (uint8_t)(b - a);
    return (uint16_t)(qsq[s] - qsq[d]);
}

/* set piece at one cell, or W_SP_NONE / W_SP_CLEAR */
static uint8_t piece(uint16_t mx, uint16_t my, uint8_t mask)
{
    uint8_t i, d2, ux, uy;
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

/* road cells of the 4x4 block at (kx, ky): bit (fy << 2) | fx. The block's 4 minor rows span
 * at most two steps of the stair (a step is >= 4), so three breakpoints are enough. */
static w_road_t rr;   /* the road being tested (a global copy: SDCC handles it far better) */

/* major offset of breakpoint k (0..n+1) of road rr: round(k * dmaj / (n + 1)) from the step
 * (qi + qf / 256), with 8 x 8 multiplies by quarter squares (exact, identical on SDCC and gcc) */
static uint16_t rr_x(uint8_t k)
{
    uint16_t a, b;
    if (k > rr.n) return rr.dmaj;
    a = (uint16_t)(mul8(k, (uint8_t)rr.qi) + (mul8(k, (uint8_t)(rr.qi >> 8)) << 8));
    b = (uint16_t)(mul8(k, rr.qf) + 128);
    return (uint16_t)(a + (b >> 8));
}

/* bits lo..hi of a 4-bit row (lo, hi clipped to 0..3; empty if lo > hi) */
static uint8_t span4(int16_t lo, int16_t hi)
{
    if (lo < 0) lo = 0;
    if (hi > 3) hi = 3;
    if (lo > hi) return 0;
    return (uint8_t)((0x0F << (uint8_t)lo) & (0x0F >> (uint8_t)(3 - hi)) & 0x0F);
}

/* a 4-bit row mask spread to bits 0, 4, 8, 12 (a column of the block) */
static const uint16_t spread4[16] = {
    0x0000, 0x0001, 0x0010, 0x0011, 0x0100, 0x0101, 0x0110, 0x0111,
    0x1000, 0x1001, 0x1010, 0x1011, 0x1100, 0x1101, 0x1110, 0x1111
};

/* the range [ra, rb] of major offsets of the road cells in the row of parameter t (0..dmin);
 * k0 and x0..x2 are the breakpoints k0..k0+2 (the block's rows span at most two steps) */
static uint16_t ra, rb, rx0, rx1, rx2;
static uint8_t rk0, rsm;
static void rr_row(uint16_t t)
{
    uint8_t f;
    if (t == rr.dmin) { ra = rr_x(rr.n); rb = rr.dmaj; return; }
    f = (uint8_t)(t >> rr.shift);
    if (f == rk0) { rb = rx1; ra = ((uint8_t)t & rsm) ? rx1 : rx0; }
    else if (f == (uint8_t)(rk0 + 1)) { rb = rx2; ra = ((uint8_t)t & rsm) ? rx2 : rx1; }
    else { rb = rr_x((uint8_t)(f + 1)); ra = ((uint8_t)t & rsm) ? rb : rr_x(f); }
}

static uint16_t road_fires;

/* Causeway cells of the 4x4 block at (kx, ky): bit (fy << 2) | fx. Also sets road_fires:
 * cold fires beside the causeway, one row off a run, every 64 major steps (64-90 metatiles
 * along the road; with the ordinary fires nearby there is one every 40-60 metatiles). */
static uint16_t block_roads(uint16_t kx, uint16_t ky, uint8_t mask)
{
    uint16_t bits = 0, fires = 0, maj0, mn0, o;
    uint8_t i, j, f, row, frow;
    int16_t ts, o0, t, lo, hi;
    for (i = 0; i < W_NUM_ROADS; i++) {
        if (!(mask & (uint8_t)(W_SPM_ROAD0 << i))) continue;
        memcpy(&rr, &w_roads[i], sizeof rr);
        if (rr.flags & W_R_YMAJOR) { maj0 = ky; mn0 = kx; } else { maj0 = kx; mn0 = ky; }
        /* road parameter t of the block's first minor row, major offset of its first cell */
        ts = (rr.flags & W_R_MINNEG) ? (int16_t)(rr.a_min - mn0) : (int16_t)(mn0 - rr.a_min);
        if (ts < -4 || ts > (int16_t)rr.dmin + 4) continue;
        o0 = (rr.flags & W_R_MAJNEG) ? (int16_t)(rr.a_maj - maj0) : (int16_t)(maj0 - rr.a_maj);
        if (o0 < -3 || o0 > (int16_t)rr.dmaj + 3) continue;
        t = (rr.flags & W_R_MINNEG) ? (int16_t)(ts - 4) : (int16_t)(ts - 1);   /* smallest t used */
        if (t < 0) t = 0;
        rk0 = (uint8_t)((uint16_t)t >> rr.shift);
        rx0 = rr_x(rk0);
        rx2 = rr_x((uint8_t)(rk0 + 2));
        /* the block's rows only hold road cells with major offsets in [rx0, rx2] (up to dmaj if
         * they reach the last row) */
        lo = (int16_t)rx0;
        hi = ((rr.flags & W_R_MINNEG) ? ts : (int16_t)(ts + 3)) >= (int16_t)rr.dmin ? (int16_t)rr.dmaj : (int16_t)rx2;
        if (rr.flags & W_R_MAJNEG) { if (o0 < lo || (int16_t)(o0 - 3) > hi) continue; }
        else if ((int16_t)(o0 + 3) < lo || o0 > hi) continue;
        rx1 = rr_x((uint8_t)(rk0 + 1));
        rsm = (uint8_t)((1u << rr.shift) - 1);
        for (j = 0; j < 4; j++) {          /* the 4 minor rows of the block */
            t = (rr.flags & W_R_MINNEG) ? (int16_t)(ts - j) : (int16_t)(ts + j);
            row = frow = 0;
            if (t >= 0 && t <= (int16_t)rr.dmin) {
                rr_row((uint16_t)t);
                if (rr.flags & W_R_MAJNEG) { lo = (int16_t)(o0 - (int16_t)rb); hi = (int16_t)(o0 - (int16_t)ra); }
                else { lo = (int16_t)((int16_t)ra - o0); hi = (int16_t)((int16_t)rb - o0); }
                row = span4(lo, hi);
            }
            /* a fire one row after a run row (t - 1 is a run: its first row or not a leg) */
            t--;
            if (t >= 0 && t <= (int16_t)rr.dmin && (!((uint8_t)t & rsm) || t == (int16_t)rr.dmin)) {
                rr_row((uint16_t)t);
                for (f = 0; f < 4; f++) {
                    o = (rr.flags & W_R_MAJNEG) ? (uint16_t)(o0 - f) : (uint16_t)(o0 + f);
                    if (((uint8_t)o & 63) == 32 && (int16_t)o >= (int16_t)ra && (int16_t)o <= (int16_t)rb)
                        frow |= (uint8_t)(1u << f);
                }
            }
            if (rr.flags & W_R_YMAJOR) {   /* major = y: the row is a column of the block */
                bits |= (uint16_t)(spread4[row] << j);
                fires |= (uint16_t)(spread4[frow] << j);
            } else {
                bits |= (uint16_t)((uint16_t)row << (j << 2));
                fires |= (uint16_t)((uint16_t)frow << (j << 2));
            }
        }
    }
    road_fires = (uint16_t)(fires & ~bits);
    return bits;
}

static uint8_t block_mask(uint16_t kx, uint16_t ky, uint8_t mask)
{
    uint8_t i, m = 0;
    for (i = 0; i < NUM_BEACONS; i++)
        if ((mask & (uint8_t)(W_SPM_BEACON0 << i)) && box_hit(kx, ky, &world.beacon[i], 6))
            m |= (uint8_t)(W_SPM_BEACON0 << i);
    if ((mask & W_SPM_OTHER) && (box_hit(kx, ky, &world.heart, 4) || box_hit(kx, ky, &world.start, 3)))
        m |= W_SPM_OTHER;
    return (uint8_t)(m | (mask & 0x0F));   /* causeways: resolved by block_roads */
}

/* Per-block data for the hot file's block cache (on a miss): the set pieces touching the 4x4
 * block (w_binfo_m, W_SPM_* without the causeway bits), its causeway and road-side fire cells
 * (w_binfo_r: two 16-bit cell masks) and whether its 16x16 cell's POI can reach into it. */
static uint16_t spk_x = 0xFFFF, spk_y = 0xFFFF;   /* last set-piece filter cell (m & ~255) */
static uint8_t spk_on;                            /* ... its W_SPM_* mask */
void w_binfo(uint16_t kx, uint16_t ky) WBANKED
{
    uint8_t m = 0, bx, by;
    if (w_ready) {
        if ((kx & 0xFF00) != spk_x || (ky & 0xFF00) != spk_y) {
            uint16_t cx = (uint16_t)(kx - w_spx0), cy = (uint16_t)(ky - w_spy0);
            spk_x = kx & 0xFF00;
            spk_y = ky & 0xFF00;
            spk_on = 0;
            if (cx < 2048 && cy < 2048)
                spk_on = w_sp_mask[(uint8_t)(((uint8_t)(cy >> 8) << 3) | (uint8_t)(cx >> 8))];
        }
        if (spk_on) m = block_mask(kx, ky, spk_on);
    }
    w_binfo_r[0] = w_binfo_r[1] = w_binfo_r[2] = w_binfo_r[3] = 0;
    if (m & 0x0F) {   /* causeways: resolved per block into cell masks */
        uint16_t rb = block_roads(kx, ky, m);
        w_binfo_r[0] = (uint8_t)rb;
        w_binfo_r[1] = (uint8_t)(rb >> 8);
        w_binfo_r[2] = (uint8_t)road_fires;
        w_binfo_r[3] = (uint8_t)(road_fires >> 8);
        m &= 0xF0;
    }
    w_binfo_m = m;
    /* the POI of the block's 16x16 cell */
    if ((kx & 0xFFF0) != w_pq_x || (ky & 0xFFF0) != w_pq_y) w_poi_roll(kx & 0xFFF0, ky & 0xFFF0);
    m = 0;
    bx = (uint8_t)kx & 15;
    by = (uint8_t)ky & 15;
    if (w_pq_type == W_POI_ROAD)
        m = (uint8_t)(w_pq_px - bx) < 4 || (uint8_t)(w_pq_py - by) < 4;
    else if (w_pq_type != W_POI_NONE)
        m = (uint8_t)(w_pq_px + 2 - bx) < 8 && (uint8_t)(w_pq_py + 2 - by) < 8;
    w_binfo_n = m;
}

/* Validate the current POI (once per POI cell): a road ending in the sea needs shore at its
 * centre (a lattice point); any other POI stands on a lattice point, a corner of the block
 * whose corners are c (e0..3 m0..3 s0..3), holding cell (lx, ly) of the 16x16 cell. */
void w_poi_check(uint8_t lx, uint8_t ly, const uint8_t *c) WBANKED
{
    uint8_t i;
    W_OP(W_OP_POI_CHECK);
    w_pq_ok = 1;
    if (w_pq_type == W_POI_ROAD) {
        w_lattice((uint16_t)((w_pq_x + w_pq_px) >> 2), (uint16_t)((w_pq_y + w_pq_py) >> 2));
        if (w_classify(w_le, w_lm, w_ls) != B_SHORE) w_pq_type = W_POI_NONE;
        return;
    }
    i = (uint8_t)((lx < w_pq_px ? 1 : 0) | (ly < w_pq_py ? 2 : 0));
    c += i;
    i = w_classify(c[0], c[4], c[8]);
    w_pq_ground = w_biome_ground[i];
    if (i <= B_SHORE || i == B_ROCK || c[0] < W_T_SHORE + 6) w_pq_type = W_POI_NONE;
    else if (w_pq_type == W_POI_MONOLITH && i == B_ASH) w_pq_ground = MT_GLASS;
}

/* the POI roll of a 16x16 cell (key m & ~15), after w_poi_roll found it in neither slot */
#define SALT_P  0xA3
#define SALT_P2 0x17
static const uint8_t poi_pos[4] = { 4, 8, 8, 12 };   /* lattice-aligned, away from the edges */
void w_poi_gen(uint16_t kx, uint16_t ky) WBANKED
{
    uint8_t h, h2;
    uint16_t cx = kx >> 4, cy = ky >> 4;
    W_OP(W_OP_POI_ROLL);
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
    spk_x = spk_y = 0xFFFF;                       /* the set-piece filter cache of w_binfo */
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
    r->dmin = dmn;
    for (sh = 2;; sh++) {
        n = (uint8_t)((dmn + (1u << sh) - 1) >> sh);
        if (((dmn + (1u << sh) - 1) >> sh) <= 200) break;
    }
    r->shift = sh;
    r->n = n;
    r->dmaj = dmaj;
    {
        uint32_t q = ((uint32_t)dmaj << 8) / (n + 1u);   /* major advance per step, 8.8 */
        r->qi = (uint16_t)(q >> 8);
        r->qf = (uint8_t)q;
    }
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
    memcpy(&rr, r, sizeof rr);
    uint16_t m0, m1, n0, n1, t;
    t = k == r->n ? r->dmin : (uint16_t)((uint16_t)k << r->shift);
    n0 = (r->flags & W_R_MINNEG) ? (uint16_t)(r->a_min - t) : (uint16_t)(r->a_min + t);
    if (leg) {
        t = (uint8_t)(k + 1) == r->n ? r->dmin : (uint16_t)((uint16_t)(k + 1) << r->shift);
        n1 = (r->flags & W_R_MINNEG) ? (uint16_t)(r->a_min - t) : (uint16_t)(r->a_min + t);
        m0 = m1 = rr_x((uint8_t)(k + 1));
    } else {
        n1 = n0;
        m0 = rr_x(k);
        m1 = rr_x((uint8_t)(k + 1));
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
    uint16_t cx, cy, cx0 = (uint16_t)(gx0 - w_spx0) >> 8, cx1 = (uint16_t)(gx1 - w_spx0) >> 8;
    uint16_t cy0 = (uint16_t)(gy0 - w_spy0) >> 8, cy1 = (uint16_t)(gy1 - w_spy0) >> 8;
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
    w_spx0 = (uint16_t)((world.start.x - 1024) & 0xFF00);   /* aligned: filter cells = m & ~255 */
    w_spy0 = (uint16_t)((world.start.y - 1024) & 0xFF00);
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
            dist = (uint16_t)(250 + (rnd() & 127));
            dist = (uint16_t)(dist + (rnd() & 15));
            polar(&world.beacon[i], bear, dist);
        }
        hb = (uint8_t)(rnd() % 3);
        hb = (uint8_t)(43 - 8 + hb * 85);
        hb = (uint8_t)(hb + (rnd() & 15));
        hdist = (uint16_t)(450 + (rnd() & 127));
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
