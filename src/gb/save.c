/* save.c - OPEN WORLD battery save (MBC5 SRAM, bank 0 of RAM): two checksummed copies, the
 * primary at 0xA000 and the backup at 0xB000. A write torn by a power cut can only damage
 * one copy; loading falls back to the other and repairs whichever copy is bad. Never called
 * from ISRs. */
#pragma bank 255
#include <gb/gb.h>
#include <string.h>
#include "game.h"
#include "gfx.h"

#define SAVE_VERSION 4   /* 2: 16x16-metatile visited chunks; 3: + the hints byte at the end;
                            4: + the stars gathered and this world's emptied wisp chunks */
#define SRAM_PRIMARY ((uint8_t *)0xA000)
#define SRAM_BACKUP  ((uint8_t *)0xB000)

typedef struct {
    uint8_t magic[2];
    uint8_t version;
    uint16_t seed;
    uint16_t mx, my;
    uint8_t sx, sy, face;
    uint16_t tod, day;
    uint8_t items, equipped, beacons, stones;
    uint16_t warmth;
    uint16_t rx, ry;
    uint8_t worlds;
    uint8_t cairn_n, old_n, mod_n;
} save_hdr_t;

#define SAVE_LEN_V2 (sizeof(save_hdr_t) + sizeof(cairns) + sizeof(world_old_cairns) + sizeof(world_mods) + sizeof(visited))
#define SAVE_LEN_V3 (SAVE_LEN_V2 + 1)   /* v3 appends `hints`, so a v2 save still loads */
#define WISP_LEN (3 + sizeof(wisp_got_x) + sizeof(wisp_got_y))
#define SAVE_LEN (SAVE_LEN_V3 + WISP_LEN)   /* v4 appends the wisps: v2 and v3 still load */
typedef char save_fits[(SAVE_LEN + 2 <= 0x1000) ? 1 : -1];
uint16_t dbg_save_len = SAVE_LEN;   /* tests: bytes per copy before the checksum */
uint16_t dbg_save_len_v3 = SAVE_LEN_V3;   /* ... of a version 3 copy (its last byte: hints) */

uint8_t dbg_saves;
static save_hdr_t hdr;
static uint8_t hints_snap;          /* hints, taken with the header: both copies agree */
/* the stars and the ring of emptied wisp chunks, taken together with the header: the game
   frame gathers wisps while the copies are written, and a copy holding the wisp as taken but
   the old star count would lose that star at a power cut */
static uint8_t wisp_snap[WISP_LEN];

/* checksum in isr.s (bank 0): a += byte, b += a */
const uint8_t *cks_ptr;
uint16_t cks_len;
uint8_t cks_a, cks_b;
void cks_run(void);

static void cks_begin(void) { cks_a = 0x5A; cks_b = 0xA5; }
static void cks_add(const void *p, uint16_t n) { cks_ptr = (const uint8_t *)p; cks_len = n; cks_run(); }

static uint16_t cks(const uint8_t *p, uint16_t n)
{
    cks_begin();
    cks_add(p, n);
    return (uint16_t)(((uint16_t)cks_b << 8) | cks_a);
}

static uint8_t *put(uint8_t *d, const void *s, uint16_t n)
{
    memcpy(d, s, n);
    return d + n;
}

/* One copy: the header and cairns come from the atomic snapshot; the mods (main-loop owned)
 * and the visited bitmap (the VBL game frame may set a bit meanwhile) straight from RAM. The
 * checksum is then taken over the copy as written in SRAM, so each copy is self-consistent
 * whatever the game frame did during the write. */
static void write_copy(uint8_t *base, const uint8_t *cairn_snap)
{
    uint8_t *d = base;
    uint16_t c;
    d = put(d, &hdr, sizeof hdr);
    d = put(d, cairn_snap, sizeof cairns);
    d = put(d, world_old_cairns, sizeof world_old_cairns);
    d = put(d, world_mods, sizeof world_mods);
    d = put(d, visited, sizeof visited);
    *d++ = hints_snap;
    d = put(d, wisp_snap, sizeof wisp_snap);     /* stars, ring count, cursor, the ring */
    c = cks(base, (uint16_t)SAVE_LEN);
    d[0] = (uint8_t)c;
    d[1] = (uint8_t)(c >> 8);
}

/* wait for a stretch of scanlines with no interrupt due (after the band split at line 23,
   well before the VBlank), for a short interrupts-off snapshot */
static void quiet_window(void)
{
    uint8_t ly;
    if (!(LCDC_REG & LCDCF_ON)) return;
    for (;;) {
        ly = LY_REG;
        if (ly >= 28 && ly < 100) return;
    }
}

/* Main loop only. The game frame (VBL ISR) keeps running: it owns the position, items and
 * cairns (snapshotted here with interrupts off, ~10 scanlines) and may set visited bits while
 * the copies are written; each copy's checksum is taken from SRAM after writing it. */
void save_write(void) BANKED
{
    uint8_t *cairn_snap = scratch;       /* 128 of the shared main-loop buffer */
    quiet_window();
    __critical {
        memcpy(cairn_snap, cairns, sizeof cairns);
        hdr.magic[0] = 'O';
        hdr.magic[1] = 'W';
        hdr.version = SAVE_VERSION;
        hdr.seed = world.seed;
        hdr.mx = pl_mx; hdr.my = pl_my;
        hdr.sx = pl_sx; hdr.sy = pl_sy; hdr.face = pl_face;
        hdr.tod = tod; hdr.day = day_count;
        hdr.items = items; hdr.equipped = equipped; hdr.beacons = beacons_lit; hdr.stones = stones;
        hdr.warmth = warmth;
        hdr.rx = respawn_x; hdr.ry = respawn_y;
        hdr.worlds = worlds_done;
        hdr.cairn_n = cairn_n;
        hdr.old_n = world_old_cairn_count;
        hdr.mod_n = world_mod_count;
        hints_snap = hints;
        wisp_snap[0] = stars;
        wisp_snap[1] = wisp_got_n;
        wisp_snap[2] = wisp_got_i;
        memcpy(wisp_snap + 3, wisp_got_x, sizeof wisp_got_x);
        memcpy(wisp_snap + 3 + sizeof wisp_got_x, wisp_got_y, sizeof wisp_got_y);
    }
    ENABLE_RAM;
    SWITCH_RAM(0);
    write_copy(SRAM_PRIMARY, cairn_snap);
    write_copy(SRAM_BACKUP, cairn_snap);
    DISABLE_RAM;
    dbg_saves++;
}

/* the length of a valid copy (before its checksum), 0 if the copy is not valid */
static uint16_t copy_len(const uint8_t *base)
{
    uint16_t c, n;
    const save_hdr_t *h = (const save_hdr_t *)base;
    if (h->magic[0] != 'O' || h->magic[1] != 'W') return 0;
    if (h->version == SAVE_VERSION) n = (uint16_t)SAVE_LEN;
    else if (h->version == 3) n = (uint16_t)SAVE_LEN_V3;
    else if (h->version == 2) n = (uint16_t)SAVE_LEN_V2;
    else return 0;
    c = cks(base, n);
    return (uint16_t)(base[n] == (uint8_t)c && base[n + 1] == (uint8_t)(c >> 8) ? n : 0);
}
#define copy_valid(b) (copy_len(b) != 0)

static uint8_t *which_valid(void)
{
    if (copy_valid(SRAM_PRIMARY)) return SRAM_PRIMARY;
    if (copy_valid(SRAM_BACKUP)) return SRAM_BACKUP;
    return 0;
}

uint8_t save_exists(void) BANKED
{
    uint8_t ok;
    ENABLE_RAM;
    SWITCH_RAM(0);
    ok = (uint8_t)(which_valid() != 0);
    DISABLE_RAM;
    return ok;
}

/* which set piece stands at (x, y): 0-2 a beacon, 3 the Heart, 4 / 5 the stones' / the cloak's
   shrine, 0xFF none */
static uint8_t mod_site(uint16_t x, uint16_t y)
{
    uint8_t i;
    for (i = 0; i < NUM_BEACONS; i++)
        if (world.beacon[i].x == x && world.beacon[i].y == y) return i;
    if (world.heart.x == x && world.heart.y == y) return 3;
    for (i = 0; i < 2; i++)
        if (world.shrine[i].x == x && world.shrine[i].y == y) return (uint8_t)(4 + i);
    return 0xFF;
}

uint8_t save_load(void) BANKED
{
    const uint8_t *s;
    uint8_t i, k, n, bits;
    ENABLE_RAM;
    SWITCH_RAM(0);
    s = which_valid();
    if (!s) { DISABLE_RAM; return 0; }
    memcpy(&hdr, s, sizeof hdr); s += sizeof hdr;
    memcpy(cairns, s, sizeof cairns); s += sizeof cairns;
    memcpy(world_old_cairns, s, sizeof world_old_cairns); s += sizeof world_old_cairns;
    memcpy(world_mods, s, sizeof world_mods); s += sizeof world_mods;
    memcpy(visited, s, sizeof visited); s += sizeof visited;
    /* a v2 save had no hints: lessons for the items already carried were never needed */
    hints = hdr.version >= 3 ? *s :
            (uint8_t)((hdr.items & (1 << IT_STONES) ? HINT_STONES : 0) | (hdr.items & (1 << IT_CLOAK) ? HINT_CLOAK : 0));
    /* before version 4 there were no wisps: the sky starts as it was, nothing gathered */
    wisps_clear();
    if (hdr.version >= 4) {
        s++;
        stars = *s++;
        wisp_got_n = *s++;
        wisp_got_i = *s++;
        memcpy(wisp_got_x, s, sizeof wisp_got_x); s += sizeof wisp_got_x;
        memcpy(wisp_got_y, s, sizeof wisp_got_y);
        if (wisp_got_n > WISP_KEEP) wisp_got_n = WISP_KEEP;
        /* until the ring is full the cursor is the count (wisp_taken looks at the first n) */
        if (wisp_got_n < WISP_KEEP) wisp_got_i = wisp_got_n;
        else wisp_got_i &= WISP_KEEP - 1;
    }
    /* repair whichever copy is damaged (a write torn by a power cut) from the good one */
    if (!copy_valid(SRAM_PRIMARY)) memcpy(SRAM_PRIMARY, SRAM_BACKUP, copy_len(SRAM_BACKUP) + 2);
    else if (!copy_valid(SRAM_BACKUP)) memcpy(SRAM_BACKUP, SRAM_PRIMARY, copy_len(SRAM_PRIMARY) + 2);
    DISABLE_RAM;
    pl_mx = hdr.mx; pl_my = hdr.my;
    pl_sx = hdr.sx; pl_sy = hdr.sy; pl_face = hdr.face;
    tod = hdr.tod; day_count = hdr.day;
    items = hdr.items; equipped = hdr.equipped; beacons_lit = hdr.beacons; stones = hdr.stones;
    warmth = hdr.warmth;
    respawn_x = hdr.rx; respawn_y = hdr.ry;
    worlds_done = hdr.worlds;
    cairn_n = hdr.cairn_n > MAX_CAIRNS ? MAX_CAIRNS : hdr.cairn_n;
    world_old_cairn_count = hdr.old_n > MAX_OLD_CAIRNS ? MAX_OLD_CAIRNS : hdr.old_n;
    /* a copy can pass its checksum and still hold values this build would never write (bit rot
       that happens to balance, a save from some other build): nothing below may index a table
       or leave the wanderer unable to move */
    pl_sx &= 15; pl_sy &= 15; pl_face &= 7;
    if (tod >= DAY_FRAMES) tod = 600;
    if (warmth > WARMTH_MAX) warmth = WARMTH_MAX;
    if (!warmth) warmth = 1;
    items = (uint8_t)((items & ((1 << IT_COUNT) - 1)) | (1 << IT_LANTERN));
    if (equipped >= IT_COUNT || !(items & (1 << equipped))) equipped = IT_LANTERN;
    beacons_lit &= (1 << NUM_BEACONS) - 1;
    world_init(hdr.seed);
    n = hdr.mod_n > MAX_MODS ? MAX_MODS : hdr.mod_n;
    /* Edits: only what the game itself writes, and only where it writes it. A lit beacon stands
       on a beacon, an emptied shrine on a shrine whose item is carried, and nothing else may
       cover a beacon, a shrine or the Heart (that would end the world's progression). */
    bits = 0;
    for (i = 0, k = 0; i < n; i++) {
        uint8_t m = world_mods[i].mt, at = mod_site(world_mods[i].x, world_mods[i].y);
        if (m == MT_BEACON_LIT) {
            if (at >= NUM_BEACONS) continue;
            bits |= (uint8_t)(1 << at);
        } else if (m == MT_SHRINE_EMPTY) {
            if (at != 4 && at != 5) { if (at != 0xFF) continue; }
            else if (!(items & (at == 4 ? 1 << IT_STONES : 1 << IT_CLOAK))) continue;
        } else if (at != 0xFF || (m != MT_FIRE_LIT && m != MT_CAIRN && m != MT_STEPSTONE && m != MT_ASH)) {
            continue;
        }
        if (k != i) world_mods[k] = world_mods[i];
        k++;
    }
    world_mod_count = k;
    beacons_lit = bits;       /* a beacon is lit exactly when its light is in the land */
    world_mods_rebuild();
    return 1;
}
