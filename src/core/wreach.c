/* wreach.c - host-only reachability checker for tests and owgen (compiled out on the Game Boy).
 *
 * Rules (mirroring the engine):
 *  - walkable: not MTF_SOLID; MT_BRAMBLE counts as walkable with the LANTERN (it burns),
 *    MT_SHALLOW with STONES (a stepping stone can be placed).
 *  - glide (CLOAK): from a walkable cell jump exactly 3 cells (or 2..3 when permissive) in one of
 *    8 directions; every cell jumped over must have MTF_GLIDE, the landing cell must be walkable.
 *  - permissive mode (diag = 1) also allows 8-way walking with no corner rule: use it to prove
 *    that a gate really blocks; strict mode (diag = 0) to prove that something is reachable.
 */
#ifndef __SDCC
#include <stdlib.h>
#include <string.h>
#include "world.h"

#define WR_N (2 * WR_HALF)
static uint8_t *grid;      /* WR_N * WR_N metatiles */
static uint8_t *seen;
static uint32_t *queue;
static uint16_t ox, oy;    /* window origin */
static uint32_t nseen;

void wr_load(uint16_t cx, uint16_t cy)
{
    int x, y;
    if (!grid) {
        grid = malloc((size_t)WR_N * WR_N);
        seen = malloc((size_t)WR_N * WR_N);
        queue = malloc(sizeof(uint32_t) * (size_t)WR_N * WR_N);
        if (!grid || !seen || !queue) abort();
    }
    ox = (uint16_t)(cx - WR_HALF);
    oy = (uint16_t)(cy - WR_HALF);
    for (y = 0; y < WR_N; y++)
        for (x = 0; x < WR_N; x++)
            grid[y * WR_N + x] = world_mt((uint16_t)(ox + x), (uint16_t)(oy + y));
}

static int idx(uint16_t mx, uint16_t my)
{
    uint16_t x = (uint16_t)(mx - ox), y = (uint16_t)(my - oy);
    if (x >= WR_N || y >= WR_N) return -1;
    return y * WR_N + x;
}

uint8_t wr_get(uint16_t mx, uint16_t my)
{
    int i = idx(mx, my);
    return i < 0 ? 0xFF : grid[i];
}

static int walkable(uint8_t mt, uint8_t items)
{
    if (!(mt_flags[mt] & MTF_SOLID)) return 1;
    if (mt == MT_BRAMBLE && (items & WR_LANTERN)) return 1;
    if (mt == MT_SHALLOW && (items & WR_STONES)) return 1;
    return 0;
}

static const int8_t ddx[8] = { 0, 1, 0, -1, 1, 1, -1, -1 };
static const int8_t ddy[8] = { -1, 0, 1, 0, -1, 1, 1, -1 };

void wr_bfs(uint16_t sx, uint16_t sy, uint8_t items, uint8_t diag)
{
    uint32_t head = 0, tail = 0;
    int s = idx(sx, sy), d, k;
    memset(seen, 0, (size_t)WR_N * WR_N);
    nseen = 0;
    if (s < 0 || !walkable(grid[s], items)) return;
    seen[s] = 1;
    queue[tail++] = (uint32_t)s;
    while (head < tail) {
        uint32_t c = queue[head++];
        int x = (int)(c % WR_N), y = (int)(c / WR_N);
        nseen++;
        for (d = 0; d < (diag ? 8 : 4); d++) {
            int nx = x + ddx[d], ny = y + ddy[d], n;
            if (nx < 0 || ny < 0 || nx >= WR_N || ny >= WR_N) continue;
            n = ny * WR_N + nx;
            if (!seen[n] && walkable(grid[n], items)) { seen[n] = 1; queue[tail++] = (uint32_t)n; }
        }
        if (!(items & WR_CLOAK)) continue;
        for (d = 0; d < 8; d++) {
            for (k = 1; k <= 3; k++) {
                int nx = x + ddx[d] * k, ny = y + ddy[d] * k, n;
                if (nx < 0 || ny < 0 || nx >= WR_N || ny >= WR_N) break;
                n = ny * WR_N + nx;
                if ((k == 3 || (diag && k == 2)) && !seen[n] && walkable(grid[n], items)) {
                    seen[n] = 1;
                    queue[tail++] = (uint32_t)n;
                }
                if (!(mt_flags[grid[n]] & MTF_GLIDE)) break;
            }
        }
    }
}

uint8_t wr_reached(uint16_t mx, uint16_t my)
{
    int i = idx(mx, my);
    return i >= 0 && seen[i];
}

uint8_t wr_reached_adj(uint16_t mx, uint16_t my)
{
    return wr_reached(mx, (uint16_t)(my - 1)) || wr_reached(mx, (uint16_t)(my + 1)) ||
           wr_reached((uint16_t)(mx - 1), my) || wr_reached((uint16_t)(mx + 1), my);
}

uint32_t wr_count(void) { return nseen; }

uint16_t wr_check_world(void)
{
    uint16_t f = 0, sx = world.start.x, sy = (uint16_t)(world.start.y + 1);
    uint8_t i;
    wr_load(world.start.x, world.start.y);
    if (wr_get(world.start.x, world.start.y) != MT_FIRE_COLD) f |= WRF_START;
    if (mt_flags[wr_get(sx, sy)] & MTF_SOLID) f |= WRF_START;
    for (i = 0; i < NUM_BEACONS; i++) {
        if (wr_get(world.beacon[i].x, world.beacon[i].y) != MT_BEACON) f |= WRF_PIECES;
        if (i < 2 && wr_get(world.shrine[i].x, world.shrine[i].y) != MT_SHRINE) f |= WRF_PIECES;
    }
    if (wr_get(world.heart.x, world.heart.y) != MT_HEART) f |= WRF_PIECES;

    wr_bfs(sx, sy, 0, 1);
    if (wr_reached_adj(world.beacon[0].x, world.beacon[0].y)) f |= WRF_GATE0;
    wr_bfs(sx, sy, WR_LANTERN, 0);
    if (wr_count() < 40) f |= WRF_START;
    if (!wr_reached_adj(world.beacon[0].x, world.beacon[0].y) ||
        !wr_reached_adj(world.shrine[0].x, world.shrine[0].y)) f |= WRF_B0;
    wr_bfs(sx, sy, WR_LANTERN, 1);
    if (wr_reached_adj(world.beacon[1].x, world.beacon[1].y)) f |= WRF_GATE1;
    wr_bfs(sx, sy, WR_LANTERN | WR_STONES, 0);
    if (!wr_reached_adj(world.beacon[1].x, world.beacon[1].y) ||
        !wr_reached_adj(world.shrine[1].x, world.shrine[1].y)) f |= WRF_B1;
    wr_bfs(sx, sy, WR_LANTERN | WR_STONES, 1);
    if (wr_reached_adj(world.beacon[2].x, world.beacon[2].y)) f |= WRF_GATE2;
    wr_bfs(sx, sy, WR_LANTERN | WR_STONES | WR_CLOAK, 0);
    if (!wr_reached_adj(world.beacon[2].x, world.beacon[2].y)) f |= WRF_B2;
    if (!wr_reached_adj(world.heart.x, world.heart.y)) f |= WRF_HEART;
    return f;
}

#else
typedef int wreach_unused_t;   /* empty translation unit on the Game Boy */
#endif
