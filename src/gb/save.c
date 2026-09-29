/* save.c - OPEN WORLD battery save (MBC5 SRAM, bank 0 of RAM): two checksummed copies, the
 * primary at 0xA000 and the backup at 0xB000. A write torn by a power cut can only damage
 * one copy; loading falls back to the other and repairs the primary. Never called from ISRs. */
#pragma bank 255
#include <gb/gb.h>
#include <string.h>
#include "game.h"
#include "gfx.h"

#define SAVE_VERSION 1
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

#define SAVE_LEN (sizeof(save_hdr_t) + sizeof(cairns) + sizeof(world_old_cairns) + sizeof(world_mods) + sizeof(visited))
typedef char save_fits[(SAVE_LEN + 2 <= 0x1000) ? 1 : -1];

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

static void write_copy(uint8_t *d, uint16_t c)
{
    d = put(d, &hdr, sizeof hdr);
    d = put(d, cairns, sizeof cairns);
    d = put(d, world_old_cairns, sizeof world_old_cairns);
    d = put(d, world_mods, sizeof world_mods);
    d = put(d, visited, sizeof visited);
    d[0] = (uint8_t)c;
    d[1] = (uint8_t)(c >> 8);
}

void save_write(void) BANKED
{
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
    cks_begin();
    cks_add(&hdr, sizeof hdr);
    cks_add(cairns, sizeof cairns);
    cks_add(world_old_cairns, sizeof world_old_cairns);
    cks_add(world_mods, sizeof world_mods);
    cks_add(visited, sizeof visited);
    ENABLE_RAM;
    SWITCH_RAM(0);
    write_copy(SRAM_PRIMARY, (uint16_t)(((uint16_t)cks_b << 8) | cks_a));
    write_copy(SRAM_BACKUP, (uint16_t)(((uint16_t)cks_b << 8) | cks_a));
    DISABLE_RAM;
    dbg_saves++;
}

static uint8_t copy_valid(const uint8_t *base)
{
    uint16_t c;
    const save_hdr_t *h = (const save_hdr_t *)base;
    if (h->magic[0] != 'O' || h->magic[1] != 'W' || h->version != SAVE_VERSION) return 0;
    c = cks(base, (uint16_t)SAVE_LEN);
    return (uint8_t)(base[SAVE_LEN] == (uint8_t)c && base[SAVE_LEN + 1] == (uint8_t)(c >> 8));
}

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
    memcpy(visited, s, sizeof visited);
    if (!copy_valid(SRAM_PRIMARY)) memcpy(SRAM_PRIMARY, SRAM_BACKUP, SAVE_LEN + 2);   /* repair */
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
