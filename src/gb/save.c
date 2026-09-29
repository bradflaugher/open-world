/* save.c - OPEN WORLD battery save (MBC5 SRAM, bank 0 of RAM): two checksummed copies, the
 * primary at 0xA000 and the backup at 0xB000. A write torn by a power cut can only damage
 * one copy; loading falls back to the other and repairs whichever copy is bad. Never called
 * from ISRs. */
#pragma bank 255
#include <gb/gb.h>
#include <string.h>
#include "game.h"
#include "gfx.h"

#define SAVE_VERSION 3   /* 2: 16x16-metatile visited chunks; 3: + the hints byte at the end */
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
#define SAVE_LEN (SAVE_LEN_V2 + 1)      /* v3 appends `hints`, so a v2 save still loads */
typedef char save_fits[(SAVE_LEN + 2 <= 0x1000) ? 1 : -1];
uint16_t dbg_save_len = SAVE_LEN;   /* tests: bytes per copy before the checksum */

uint8_t dbg_saves;
static save_hdr_t hdr;

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
    *d++ = hints;
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

uint8_t save_load(void) BANKED
{
    const uint8_t *s;
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
    hints = hdr.version == SAVE_VERSION ? *s :
            (uint8_t)((hdr.items & (1 << IT_STONES) ? HINT_STONES : 0) | (hdr.items & (1 << IT_CLOAK) ? HINT_CLOAK : 0));
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
    world_init(hdr.seed);
    world_mod_count = hdr.mod_n > MAX_MODS ? MAX_MODS : hdr.mod_n;
    world_mods_rebuild();
    return 1;
}
