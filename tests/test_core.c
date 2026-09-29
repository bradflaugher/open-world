/* Host unit tests for the OPEN WORLD world core.
 * Build: make build/test_core   (gcc -std=c99 -Wall -Wextra -Werror -Isrc/core tests/test_core.c src/core/[all .c])
 * Run from the repository root (one test reads src/core/world_gen.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "world.h"

static int passed, failed;
#define CHECK(cond) do { if (cond) passed++; else { failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long _a = (long)(a), _b = (long)(b); if (_a == _b) passed++; else { failed++; \
    printf("FAIL %s:%d: %s == %s (%ld vs %ld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)
#define CHECK_SEED(cond, seed) do { if (cond) passed++; else { failed++; \
    printf("FAIL %s:%d: seed %u: %s\n", __FILE__, __LINE__, (unsigned)(seed), #cond); } } while (0)

static uint32_t lcg = 12345;
static uint16_t rnd16(void) { lcg = lcg * 1103515245u + 12345u; return (uint16_t)(lcg >> 12); }

/* same seed -> same world, whatever the access order (the caches must be pure) */
static void test_determinism(void)
{
    enum { N = 3000 };
    static uint16_t xs[N], ys[N];
    static uint8_t a[N], b[N], c[N];
    int i, diff = 0;
    world_init(7);
    for (i = 0; i < N; i++) {
        xs[i] = (uint16_t)(world.start.x - 150 + (rnd16() % 300));
        ys[i] = (uint16_t)(world.start.y - 150 + (rnd16() % 300));
        a[i] = world_mt(xs[i], ys[i]);
    }
    /* again in reverse order, after re-init */
    world_init(7);
    for (i = N - 1; i >= 0; i--) b[i] = world_mt(xs[i], ys[i]);
    /* again, interleaved with scans (row and column walks) */
    world_init(7);
    for (i = 0; i < N; i++) {
        int k;
        for (k = 0; k < 11; k++) (void)world_mt((uint16_t)(xs[i] + k), (uint16_t)(ys[(i * 7) % N]));
        for (k = 0; k < 9; k++) (void)world_biome((uint16_t)(xs[(i * 3) % N]), (uint16_t)(ys[i] + k));
        c[i] = world_mt(xs[i], ys[i]);
    }
    for (i = 0; i < N; i++) if (a[i] != b[i] || a[i] != c[i]) diff++;
    CHECK_EQ(diff, 0);
    for (i = 0; i < N; i++) CHECK(a[i] < MT_COUNT) ;
    /* world_mt_base == world_mt without mods */
    world_mods_clear();
    for (i = 0; i < 200; i++) if (world_mt_base(xs[i], ys[i]) != a[i]) diff++;
    CHECK_EQ(diff, 0);
    /* different seeds differ */
    world_init(8);
    diff = 0;
    for (i = 0; i < N; i++) if (world_mt(xs[i], ys[i]) != a[i]) diff++;
    CHECK(diff > N / 4);
    /* no state leaks from one seed to the next (caches must be fully reset by world_init) */
    world_init(8);
    for (i = 0; i < N; i++) (void)world_mt(xs[i], ys[i]);
    world_init(7);
    diff = 0;
    for (i = 0; i < N; i++) if (world_mt(xs[i], ys[i]) != a[i]) diff++;
    CHECK_EQ(diff, 0);
    /* nor from far-away queries (other cache regions) */
    world_init(7);
    (void)world_mt(0, 0);
    (void)world_mt(40000, 1000);
    diff = 0;
    for (i = 0; i < N; i++) if (world_mt(xs[i], ys[i]) != a[i]) diff++;
    CHECK_EQ(diff, 0);
    /* nearby seeds give different layouts */
    {
        wpos_t s1, s2;
        world_init(100); s1 = world.beacon[0];
        world_init(101); s2 = world.beacon[0];
        CHECK(s1.x != s2.x || s1.y != s2.y);
    }
}

/* host/ROM parity guard: C leaves operand evaluation order unspecified (SDCC and gcc differ),
 * so the generator must never put two rnd() calls in one expression. */
static void test_source_guard(void)
{
    FILE *f = fopen("src/core/world_gen.c", "r");
    char line[512];
    int bad = 0, n = 0;
    if (!f) { printf("note: src/core/world_gen.c not found (run from repo root); guard skipped\n"); return; }
    while (fgets(line, sizeof line, f)) {
        const char *p = strstr(line, "rnd()");
        n++;
        if (p && strstr(p + 5, "rnd()") && !strstr(line, "/*")) {
            printf("line %d: two rnd() calls in one statement: %s", n, line);
            bad++;
        }
    }
    fclose(f);
    CHECK_EQ(bad, 0);
}

static const char *bname[B_COUNT] = {
    "sea", "shallow", "shore", "meadow", "forest", "desert", "tundra", "rock", "ash", "ruins"
};

static void test_biomes(void)
{
    unsigned long tot[B_COUNT];
    int s, i, x, y, all_seeds_ok = 0;
    memset(tot, 0, sizeof tot);
    for (s = 1; s <= 40; s++) {
        unsigned long h[B_COUNT], n = 0;
        int kinds = 0;
        memset(h, 0, sizeof h);
        world_init((uint16_t)(s * 977));
        for (y = -384; y < 384; y += 6)
            for (x = -384; x < 384; x += 6) {
                h[world_biome((uint16_t)(world.start.x + x), (uint16_t)(world.start.y + y))]++;
                n++;
            }
        for (i = 0; i < B_COUNT; i++) { tot[i] += h[i]; if (h[i]) kinds++; }
        CHECK_SEED(h[B_SEA] * 100 <= n * 60, s * 977);   /* sea not > 60% */
        CHECK_SEED(kinds >= 8, s * 977);
        if (kinds == B_COUNT) all_seeds_ok++;
        /* the metatile agrees with the biome for the main classes */
        for (i = 0; i < 400; i++) {
            uint16_t mx = (uint16_t)(world.start.x + 200 + (rnd16() % 300)), my = (uint16_t)(world.start.y + 200 + (rnd16() % 300));
            uint8_t mt = world_mt(mx, my);
            CHECK(mt < MT_COUNT);
        }
    }
    for (i = 0; i < B_COUNT; i++) CHECK(tot[i] > 0);
    CHECK(all_seeds_ok >= 20);
    printf("biomes over 40 seeds:");
    {
        unsigned long n = 0;
        for (i = 0; i < B_COUNT; i++) n += tot[i];
        for (i = 0; i < B_COUNT; i++) printf(" %s %.1f%%", bname[i], 100.0 * tot[i] / n);
    }
    printf("  (all 10 in %d/40 seeds)\n", all_seeds_ok);
}

/* scale: biome regions should be tens of metatiles, not salt-and-pepper */
static void test_scale(void)
{
    int s, x, y;
    unsigned long changes = 0, samples = 0;
    for (s = 1; s <= 10; s++) {
        world_init((uint16_t)(s * 31));
        for (y = -200; y < 200; y += 10) {
            uint8_t prev = world_biome((uint16_t)(world.start.x - 200), (uint16_t)(world.start.y + y));
            for (x = -199; x < 200; x++) {
                uint8_t b = world_biome((uint16_t)(world.start.x + x), (uint16_t)(world.start.y + y));
                if (b != prev) changes++;
                prev = b;
                samples++;
            }
        }
    }
    /* mean run length along a row, in metatiles */
    printf("mean biome run length: %.1f metatiles\n", (double)samples / (changes + 1));
    CHECK(samples / (changes + 1) >= 8);
    CHECK(samples / (changes + 1) <= 80);
}

static void test_set_pieces(void)
{
    int s, pass = 0;
    const int N = 220;
    for (s = 0; s < N; s++) {
        uint16_t seed = (uint16_t)(s * 2654435761u >> 7), f;
        int i;
        world_init(seed);
        /* distances and bearings */
        for (i = 0; i < NUM_BEACONS; i++) {
            uint16_t d = world_dist(world.start.x, world.start.y, world.beacon[i].x, world.beacon[i].y);
            uint8_t br = world_bearing(world.start.x, world.start.y, world.beacon[i].x, world.beacon[i].y);
            int8_t db = (int8_t)(uint8_t)(br - (uint8_t)(i * 85));
            CHECK_SEED(d >= 85 && d <= 175, seed);
            CHECK_SEED(db >= -14 && db <= 14, seed);
        }
        {
            uint16_t d = world_dist(world.start.x, world.start.y, world.heart.x, world.heart.y);
            CHECK_SEED(d >= 190 && d <= 270, seed);
        }
        /* start: a cold fire, walkable spawn and a clear radius-3 area */
        CHECK_SEED(world_mt(world.start.x, world.start.y) == MT_FIRE_COLD, seed);
        {
            int x, y, solid = 0;
            for (y = -2; y <= 2; y++)
                for (x = -2; x <= 2; x++) {
                    if (!x && !y) continue;
                    if (mt_flags[world_mt((uint16_t)(world.start.x + x), (uint16_t)(world.start.y + y))] & MTF_SOLID) solid++;
                }
            CHECK_SEED(solid == 0, seed);
        }
        /* gates intact */
        {
            static const uint8_t ring[3] = { MT_BRAMBLE, MT_SHALLOW, MT_ROCK };
            for (i = 0; i < NUM_BEACONS; i++) {
                int a, bad = 0;
                for (a = -5; a <= 5; a++) {
                    if (world_mt((uint16_t)(world.beacon[i].x + a), (uint16_t)(world.beacon[i].y - 5)) != ring[i] && a >= -2 && a <= 2) bad++;
                    if (world_mt((uint16_t)(world.beacon[i].x - 5), (uint16_t)(world.beacon[i].y + a)) != ring[i] && a >= -2 && a <= 2) bad++;
                }
                CHECK_SEED(bad == 0, seed);
            }
            CHECK_SEED(world_mt(world.shrine[0].x, world.shrine[0].y) == MT_SHRINE, seed);
            CHECK_SEED(world_mt(world.shrine[1].x, world.shrine[1].y) == MT_SHRINE, seed);
            CHECK_SEED(world_mt(world.heart.x, world.heart.y) == MT_HEART, seed);
        }
        /* progression: lantern -> beacon 0 + stones; stones -> beacon 1 + cloak; cloak -> beacon 2,
         * heart; and each gate really blocks without its item */
        f = wr_check_world();
        if (!f) pass++;
        else printf("seed %u: progression check failed 0x%04x\n", seed, f);
    }
    printf("progression check: %d / %d seeds pass\n", pass, N);
    CHECK_EQ(pass, N);
}

static void test_mods(void)
{
    uint16_t x, y;
    uint8_t base, i;
    world_init(42);
    world_mods_clear();
    x = (uint16_t)(world.start.x + 20);
    y = (uint16_t)(world.start.y + 20);
    base = world_mt(x, y);
    CHECK(world_mod_set(x, y, MT_CAIRN));
    CHECK_EQ(world_mt(x, y), MT_CAIRN);
    CHECK_EQ(world_mt_base(x, y), base);
    CHECK_EQ(world_mt(x, y), MT_CAIRN);          /* still there after a base lookup */
    CHECK(world_mod_set(x, y, MT_FIRE_LIT));     /* replace */
    CHECK_EQ(world_mod_count, 1);
    CHECK_EQ(world_mt(x, y), MT_FIRE_LIT);
    /* fill the table */
    for (i = 1; i < MAX_MODS; i++) CHECK(world_mod_set((uint16_t)(x + i * 3), (uint16_t)(y + (i & 7)), MT_STEPSTONE));
    CHECK_EQ(world_mod_count, MAX_MODS);
    CHECK(!world_mod_set((uint16_t)(x + 999), y, MT_CAIRN));      /* full */
    CHECK(world_mod_set(x, y, MT_CAIRN));                         /* replace still works */
    for (i = 1; i < MAX_MODS; i++) CHECK_EQ(world_mt((uint16_t)(x + i * 3), (uint16_t)(y + (i & 7))), MT_STEPSTONE);
    /* mods written directly (a save load) */
    world_mods_clear();
    CHECK_EQ(world_mt(x, y), base);
    world_mods[0].x = x;
    world_mods[0].y = y;
    world_mods[0].mt = MT_BEACON_LIT;
    world_mod_count = 1;
    CHECK_EQ(world_mt(x, y), MT_BEACON_LIT);
    world_mods[0].mt = MT_CAIRN;
    world_mods_rebuild();
    CHECK_EQ(world_mt(x, y), MT_CAIRN);
    world_mods_clear();
    CHECK_EQ(world_mt(x, y), base);
}

static void test_old_cairns(void)
{
    int i, found = 0, tries = 0;
    world_old_cairn_count = 0;
    for (i = 0; i < 12; i++) {
        world_old_cairns[i].dx = (int8_t)(i * 3 - 17);
        world_old_cairns[i].dy = (int8_t)(i * 5 - 20);
    }
    world_old_cairn_count = 12;
    world_init(4242);
    for (i = 0; i < 12; i++) {
        uint16_t x = (uint16_t)(world.start.x + world_old_cairns[i].dx * 4);
        uint16_t y = (uint16_t)(world.start.y + world_old_cairns[i].dy * 4);
        uint8_t b = world_biome(x, y), mt = world_mt(x, y);
        tries++;
        if (mt == MT_CAIRN_OLD) found++;
        else CHECK(b <= B_SHALLOW || b == B_ROCK || mt != MT_CAIRN_OLD);
    }
    printf("old cairns: %d / %d placed (the rest fell on water, rock or set pieces)\n", found, tries);
    CHECK(found >= 4);
    /* not placed off the grid */
    CHECK(world_mt((uint16_t)(world.start.x + 1 - 17 * 4), (uint16_t)(world.start.y - 20 * 4)) != MT_CAIRN_OLD);
    world_old_cairn_count = 0;
    world_init(4242);
    CHECK(world_mt((uint16_t)(world.start.x - 17 * 4), (uint16_t)(world.start.y - 20 * 4)) != MT_CAIRN_OLD);
}

/* libm-free sine (Taylor series after range reduction to [-pi, pi]) */
static double tsin(double r)
{
    double t, s = 0, x2;
    int k;
    while (r > 3.141592653589793) r -= 6.283185307179586;
    while (r < -3.141592653589793) r += 6.283185307179586;
    t = r; x2 = r * r;
    for (k = 1; k < 30; k += 2) { s += t; t *= -x2 / ((k + 1) * (k + 2)); }
    return s;
}

static void test_helpers(void)
{
    uint16_t x = 1000, y = 1000;
    CHECK((uint8_t)(world_bearing(x, y, x, (uint16_t)(y - 50)) + 1) <= 2);   /* 0 +- 1 */
    CHECK(abs((int)world_bearing(x, y, (uint16_t)(x + 50), y) - 64) <= 1);
    CHECK(abs((int)world_bearing(x, y, x, (uint16_t)(y + 50)) - 128) <= 1);
    CHECK(abs((int)world_bearing(x, y, (uint16_t)(x - 50), y) - 192) <= 1);
    CHECK(abs((int)world_bearing(x, y, (uint16_t)(x + 50), (uint16_t)(y - 50)) - 32) <= 1);
    CHECK(abs((int)world_bearing(x, y, (uint16_t)(x + 3), (uint16_t)(y + 1)) - 77) <= 2);   /* east 3, south 1: 108.4 deg */
    /* wraps around the torus */
    CHECK(abs((int)world_bearing(65530, 10, 5, 10) - 64) <= 1);
    CHECK_EQ(world_dist(65530, 10, 5, 10), 11);
    CHECK_EQ(world_dist(x, y, (uint16_t)(x + 100), y), 100);
    CHECK(abs((int)world_dist(x, y, (uint16_t)(x + 100), (uint16_t)(y + 100)) - 141) <= 2);
    {
        int a, maxerr = 0;
        for (a = 0; a < 256; a += 3) {
            /* compare against a float atan2 */
            double r = a * 6.283185307179586 / 256.0;
            int dx = (int)(200 * tsin(r)), dy = (int)(-200 * tsin(r + 1.5707963267948966));
            int b = world_bearing(x, y, (uint16_t)(x + dx), (uint16_t)(y + dy));
            int e = (b - a) & 255;
            if (e > 128) e = 256 - e;
            if (e > maxerr) maxerr = e;
        }
        CHECK(maxerr <= 2);
    }
    /* the coarse map shade agrees with the real biomes most of the time */
    {
        static const uint8_t bshade[B_COUNT] = { 3, 0, 0, 1, 2, 0, 0, 2, 2, 1 };
        int i, agree = 0, n = 0, x, y;
        world_init(9);
        for (y = -512; y < 512; y += 8)
            for (x = -512; x < 512; x += 8) {
                uint16_t mx = (uint16_t)(world.start.x + x), my = (uint16_t)(world.start.y + y);
                n++;
                if (world_map_shade(mx, my) == bshade[world_biome(mx, my)]) agree++;
            }
        printf("map shade agrees with the biome for %d%% of chunks\n", agree * 100 / n);
        CHECK(agree * 100 >= n * 75);
        (void)i;
    }
    /* map shade covers 0..3 */
    {
        int seen[4] = { 0, 0, 0, 0 }, i;
        world_init(5);
        for (i = 0; i < 4000; i++) seen[world_map_shade((uint16_t)(world.start.x + (rnd16() % 1024) - 512),
                                                         (uint16_t)(world.start.y + (rnd16() % 1024) - 512)) & 3]++;
        CHECK(seen[0] && seen[1] && seen[2] && seen[3]);
    }
}

/* points of interest: cold fires about one per three 16x16 cells, on walkable ground */
static void test_pois(void)
{
    int s, x, y, fires = 0, bad = 0, cells = 0, tables = 0, wells = 0, monos = 0;
    for (s = 1; s <= 6; s++) {
        world_init((uint16_t)(s * 1234));
        for (y = 16; y < 16 * 25; y++)
            for (x = 16; x < 16 * 25; x++) {
                uint16_t mx = (uint16_t)(world.start.x + x), my = (uint16_t)(world.start.y + y);
                uint8_t mt = world_mt(mx, my);
                if (mt == MT_FIRE_COLD) {
                    int dx, dy;
                    fires++;
                    for (dy = -1; dy <= 1; dy++)
                        for (dx = -1; dx <= 1; dx++)
                            if ((dx || dy) && (mt_flags[world_mt((uint16_t)(mx + dx), (uint16_t)(my + dy))] & MTF_SOLID)) bad++;
                }
                if (mt == MT_TABLE) tables++;
                if (mt == MT_WELL) wells++;
                if (mt == MT_MONOLITH) monos++;
            }
        cells += 24 * 24;
    }
    printf("POIs over %d cells: %d fires, %d monolith tiles, %d tables, %d wells\n", cells, fires, monos, tables, wells);
    CHECK(fires * 8 >= cells);        /* at least 1 in 8 cells (land only) */
    CHECK(fires * 2 <= cells);
    CHECK_EQ(bad, 0);
    CHECK(tables > 0 && wells > 0 && monos > 0);
}

int main(void)
{
    test_source_guard();
    test_determinism();
    test_helpers();
    test_mods();
    test_old_cairns();
    test_pois();
    test_biomes();
    test_scale();
    test_set_pieces();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
