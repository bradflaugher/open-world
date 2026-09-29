/* owgen - host CLI for the OPEN WORLD world core.
 *   owgen show SEED [CX CY [W H]]   ASCII render (one char per metatile), default around the start
 *   owgen layout SEED               start / beacons / shrines / heart
 *   owgen stats FIRSTSEED COUNT     biome histogram, distances, progression check pass count
 *   owgen png SEED FILE [SCALE [W H [CX CY]]]  PPM image (default 512x512 metatiles, scale 1)
 *   owgen mt SEED X Y               world_mt value (decimal)
 *   owgen dump SEED X0 Y0 W H       world_mt of a W x H region, row-major, as hex (one row per line)
 *   owgen layoutraw SEED            start, beacons, shrines, heart as 18 decimal numbers
 *   owgen legend                    character legend
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "world.h"

static const char glyph[MT_COUNT + 1] =
    "~`=o:.\"*T;A',b#M-/|H+Ih%&fFcCBLSsXtw";

static const unsigned char rgb[MT_COUNT][3] = {
    { 30, 50, 110 }, { 90, 120, 190 }, { 80, 130, 180 }, { 140, 130, 110 },   /* sea glint shallow stepstone */
    { 220, 205, 150 }, { 120, 180, 80 }, { 90, 150, 60 }, { 200, 190, 90 },   /* sand grass tall flowers */
    { 30, 90, 40 }, { 70, 120, 60 }, { 40, 80, 70 }, { 235, 240, 245 },       /* tree under pine snow */
    { 210, 180, 110 }, { 230, 225, 200 }, { 110, 100, 95 }, { 60, 55, 55 },   /* dune bones rock peak */
    { 80, 75, 85 }, { 150, 170, 200 }, { 10, 10, 20 }, { 120, 90, 80 },       /* ash glass monolith wall */
    { 170, 150, 130 }, { 200, 190, 180 }, { 230, 220, 210 }, { 160, 110, 60 },/* floor pillar hand road */
    { 100, 40, 60 }, { 255, 120, 0 }, { 255, 220, 0 }, { 255, 0, 255 },       /* bramble fire fire cairn */
    { 200, 0, 200 }, { 255, 0, 0 }, { 255, 255, 0 }, { 0, 255, 255 },         /* old cairn beacon lit shrine */
    { 0, 160, 160 }, { 255, 255, 255 }, { 150, 100, 50 }, { 90, 90, 140 },    /* shrine empty heart table well */
};

static const char *biome_name[B_COUNT] = {
    "sea", "shallow", "shore", "meadow", "forest", "desert", "tundra", "rock", "ash", "ruins"
};

static long num(const char *s) { return strtol(s, NULL, 0); }

static void show(uint16_t cx, uint16_t cy, int w, int h)
{
    int x, y;
    uint16_t x0 = (uint16_t)(cx - w / 2), y0 = (uint16_t)(cy - h / 2);
    printf("seed %u  region x %u..%u  y %u..%u\n", world.seed, x0, (uint16_t)(x0 + w - 1), y0,
           (uint16_t)(y0 + h - 1));
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) putchar(glyph[world_mt((uint16_t)(x0 + x), (uint16_t)(y0 + y))]);
        putchar('\n');
    }
}

static void layout(void)
{
    int i;
    printf("seed %u\nstart  %u %u\n", world.seed, world.start.x, world.start.y);
    for (i = 0; i < NUM_BEACONS; i++)
        printf("beacon%d %u %u  dist %u  bearing %u  shrine %u %u\n", i, world.beacon[i].x,
               world.beacon[i].y,
               world_dist(world.start.x, world.start.y, world.beacon[i].x, world.beacon[i].y),
               world_bearing(world.start.x, world.start.y, world.beacon[i].x, world.beacon[i].y),
               world.shrine[i].x, world.shrine[i].y);
    printf("heart  %u %u  dist %u  bearing %u\n", world.heart.x, world.heart.y,
           world_dist(world.start.x, world.start.y, world.heart.x, world.heart.y),
           world_bearing(world.start.x, world.start.y, world.heart.x, world.heart.y));
}

static int ppm(const char *file, int scale, int w, int h, uint16_t cx, uint16_t cy)
{
    FILE *f = fopen(file, "wb");
    int x, y, i, j;
    uint16_t x0 = (uint16_t)(cx - w / 2), y0 = (uint16_t)(cy - h / 2);
    unsigned char *row;
    if (!f) { perror(file); return 1; }
    row = malloc((size_t)w * scale * 3);
    fprintf(f, "P6\n%d %d\n255\n", w * scale, h * scale);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t mt = world_mt((uint16_t)(x0 + x), (uint16_t)(y0 + y));
            for (i = 0; i < scale; i++)
                for (j = 0; j < 3; j++) row[(x * scale + i) * 3 + j] = rgb[mt][j];
        }
        for (i = 0; i < scale; i++) fwrite(row, 3, (size_t)w * scale, f);
    }
    free(row);
    fclose(f);
    return 0;
}

static void stats(uint16_t first, int count)
{
    unsigned long hist[B_COUNT], mth[MT_COUNT], total = 0, mtotal = 0;
    unsigned long dsum[NUM_BEACONS + 1];
    int s, i, pass = 0, x, y;
    memset(hist, 0, sizeof hist);
    memset(mth, 0, sizeof mth);
    memset(dsum, 0, sizeof dsum);
    for (s = 0; s < count; s++) {
        uint16_t f;
        world_init((uint16_t)(first + s));
        for (y = -256; y < 256; y += 4)
            for (x = -256; x < 256; x += 4) {
                hist[world_biome((uint16_t)(world.start.x + x), (uint16_t)(world.start.y + y))]++;
                total++;
            }
        for (y = -64; y < 64; y++)
            for (x = -64; x < 64; x++) {
                mth[world_mt((uint16_t)(world.start.x + x), (uint16_t)(world.start.y + y))]++;
                mtotal++;
            }
        for (i = 0; i < NUM_BEACONS; i++)
            dsum[i] += world_dist(world.start.x, world.start.y, world.beacon[i].x, world.beacon[i].y);
        dsum[NUM_BEACONS] += world_dist(world.start.x, world.start.y, world.heart.x, world.heart.y);
        f = wr_check_world();
        if (!f) pass++;
        else printf("seed %u: progression check failed 0x%04x\n", (unsigned)(uint16_t)(first + s), f);
    }
    printf("biomes (512x512 around start, %d seeds):\n", count);
    for (i = 0; i < B_COUNT; i++) printf("  %-8s %5.1f%%\n", biome_name[i], 100.0 * hist[i] / total);
    printf("metatiles (128x128 around start):\n");
    for (i = 0; i < MT_COUNT; i++)
        if (mth[i]) printf("  %c %2d %6.2f%%\n", glyph[i], i, 100.0 * mth[i] / mtotal);
    for (i = 0; i < NUM_BEACONS; i++) printf("avg dist beacon%d %.1f\n", i, (double)dsum[i] / count);
    printf("avg dist heart   %.1f\n", (double)dsum[NUM_BEACONS] / count);
    printf("progression check: %d / %d pass\n", pass, count);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "legend")) {
        int i;
        static const char *names[MT_COUNT] = {
            "sea", "sea glint", "shallow", "stepping stone", "sand", "grass", "tall grass", "flowers",
            "tree", "undergrowth", "pine", "snow", "dune", "bones", "rock", "peak", "ash", "glass",
            "monolith", "ruin wall", "ruin floor", "pillar", "statue hand", "road", "bramble",
            "cold fire", "lit fire", "cairn", "old cairn", "beacon", "lit beacon", "shrine",
            "empty shrine", "heart", "table", "well"
        };
        for (i = 0; i < MT_COUNT; i++) printf("%c %2d %s\n", glyph[i], i, names[i]);
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: owgen show SEED [CX CY [W H]] | layout SEED | stats FIRST COUNT |\n"
                        "       png SEED FILE [SCALE [W H [CX CY]]] | mt SEED X Y | legend\n");
        return 2;
    }
    world_init((uint16_t)num(argv[2]));
    if (!strcmp(argv[1], "show")) {
        uint16_t cx = world.start.x, cy = world.start.y;
        int w = 100, h = 60;
        if (argc >= 5) { cx = (uint16_t)num(argv[3]); cy = (uint16_t)num(argv[4]); }
        if (argc >= 7) { w = (int)num(argv[5]); h = (int)num(argv[6]); }
        show(cx, cy, w, h);
    } else if (!strcmp(argv[1], "layout")) {
        layout();
    } else if (!strcmp(argv[1], "stats") && argc >= 4) {
        stats((uint16_t)num(argv[2]), (int)num(argv[3]));
    } else if (!strcmp(argv[1], "png") && argc >= 4) {
        int scale = argc >= 5 ? (int)num(argv[4]) : 1, w = 512, h = 512;
        uint16_t cx = world.start.x, cy = world.start.y;
        if (argc >= 7) { w = (int)num(argv[5]); h = (int)num(argv[6]); }
        if (argc >= 9) { cx = (uint16_t)num(argv[7]); cy = (uint16_t)num(argv[8]); }
        return ppm(argv[3], scale < 1 ? 1 : scale, w, h, cx, cy);
    } else if (!strcmp(argv[1], "dump") && argc >= 7) {
        uint16_t x0 = (uint16_t)num(argv[3]), y0 = (uint16_t)num(argv[4]);
        int w = (int)num(argv[5]), h = (int)num(argv[6]), x, y;
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) printf("%02x", world_mt((uint16_t)(x0 + x), (uint16_t)(y0 + y)));
            putchar('\n');
        }
    } else if (!strcmp(argv[1], "layoutraw")) {
        int i;
        printf("%u %u", world.start.x, world.start.y);
        for (i = 0; i < NUM_BEACONS; i++) printf(" %u %u", world.beacon[i].x, world.beacon[i].y);
        for (i = 0; i < NUM_BEACONS; i++) printf(" %u %u", world.shrine[i].x, world.shrine[i].y);
        printf(" %u %u\n", world.heart.x, world.heart.y);
    } else if (!strcmp(argv[1], "mt") && argc >= 5) {
        printf("%u\n", world_mt((uint16_t)num(argv[3]), (uint16_t)num(argv[4])));
    } else {
        fprintf(stderr, "bad command\n");
        return 2;
    }
    return 0;
}
