"""Hardware-correctness tests: what is in VRAM is what the game meant to put there.

On a real Game Boy a write to VRAM (or to a CGB palette) while the LCD is drawing (mode 3)
is silently dropped. Nothing crashes: a stale tile simply stays on screen. PyBoy and mGBA both
model this, so these tests look at VRAM itself:

- every frame while walking, running and gliding in all eight directions, across map toggles
  and weather changes, each BG tile on screen must be the tile the world says is there (host
  generator + the ROM's mods + the water-edge rule), and on CGB its attribute too;
- the ROM's own counter of VBlanks whose VRAM work ran past the VBlank must stay 0;
- after each full-screen load done with the LCD on (title, world, map, lesson), the tile
  patterns and maps in VRAM must equal the ROM's data byte for byte.

They run on PyBoy (DMG and CGB) and on mGBA as well when build/mgba_server exists.

Run: python3 -m unittest tests/test_vram.py   (part of make test-rom)
"""
import os
import sys
import unittest

import test_rom as T

ROOT = T.ROOT
MGBA = os.path.join(ROOT, 'build', 'mgba_server')
MT = T.MT


def rom_bytes(g, name, n):
    """n bytes of a ROM symbol (banked or not), straight from the ROM file"""
    bank, addr = g.pb.symbol_lookup('_' + name)
    with open(T.ROM, 'rb') as f:
        f.seek(bank * 0x4000 + (addr - 0x4000) if addr >= 0x4000 else addr)
        return f.read(n)


def asset_const(name):
    with open(T.ASSETS_H) as f:
        for line in f:
            p = line.split()
            if len(p) >= 3 and p[0] == '#define' and p[1] == name:
                return int(p[2])
    raise KeyError(name)


class LandWatch:
    """Compares the visible land (BG map 0x9800 through the latched scroll) with the world."""

    def __init__(self, g, seed):
        self.g, self.seed = g, seed
        self.mt_tiles, self.edge_t = g._mt_tiles(), g.edge_table()
        a = g.addr('mt_a')
        self.mt_attr = [[g.pb.memory[a + m * 4 + i] for i in range(4)] for m in range(len(MT))]
        self.blocks = {}
        self.frames = 0
        self.bad = []           # (tag, frame, cell, quarter, got, expected)
        self.streak = {}        # (cell, quarter) -> consecutive wrong frames
        self.worst = 0

    def host(self, x, y):
        k = (x >> 5, y >> 5)
        if k not in self.blocks:
            self.blocks[k] = T.host_region(self.seed, k[0] << 5, k[1] << 5, 32, 32)
        return self.blocks[k][(x, y)]

    def mods(self):
        g, m = self.g, self.g.pb.memory
        a = g.addr('world_mods')
        out = {}
        for i in range(g.u8('world_mod_count')):
            b = a + i * 5
            out[(m[b] | m[b + 1] << 8, m[b + 2] | m[b + 3] << 8)] = m[b + 4]
        return out

    def look(self, tag):
        g = self.g
        if g.state() != T.GS_WORLD or g.u8('pal_fade') or not g.u8('split_mode') or not g.u8('hook_on'):
            self.streak.clear()
            return
        self.frames += 1
        scx, scy = g.u8('land_scx'), (g.u8('land_scy') + 24) & 255
        x0, y0 = g.u16('land_x0'), g.u16('land_y0')
        md = self.mods()
        # a burning bramble is shown as a fire for a moment before its mod exists
        burning = {k for k in ()}
        grid = {}

        def cell(x, y):
            k = (x & 0xFFFF, y & 0xFFFF)
            if k not in grid:
                grid[k] = md.get(k, self.host(*k))
            return grid[k]
        now = set()
        tx0, ty0 = scx >> 3, scy >> 3
        ntx = 21 if scx & 7 else 20
        nty = 16 if scy & 7 else 15
        cache_a = g.addr('land_cache')
        for j in range(nty):
            tr = (ty0 + j) & 31
            for i in range(ntx):
                tc = (tx0 + i) & 31
                x = (x0 + (((tc >> 1) - x0) & 15)) & 0xFFFF
                y = (y0 + (((tr >> 1) - y0) & 15)) & 0xFFFF
                q = (tr & 1) * 2 + (tc & 1)
                key = (x, y, q)
                if key in burning:
                    continue
                m = cell(x, y)
                # the ROM's cache is the truth for transient edits (a bramble burning)
                cm = g.pb.memory[cache_a + ((y & 15) << 4 | (x & 15))]
                if cm != m:
                    if cm == MT['MT_FIRE_LIT'] and m == MT['MT_BRAMBLE']:
                        continue
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        cell(x + dx, y + dy)
                exp = T.expected_tiles(grid, x, y, self.mt_tiles, self.edge_t)[q]
                a = 0x9800 + tr * 32 + tc
                got = g.vram(a)
                ok = got == exp
                if ok and g.cgb and hasattr(g.pb.memory, '__getitem__') and not getattr(g, 'no_attr', False):
                    ea = self.mt_attr[m][q]
                    ga = g.vram_attr(a)
                    if ga != ea:
                        ok, got, exp = False, ('attr', ga), ('attr', ea)
                if not ok:
                    now.add(key)
                    n = self.streak.get(key, 0) + 1
                    self.streak[key] = n
                    self.worst = max(self.worst, n)
                    if len(self.bad) < 12:
                        self.bad.append((tag, self.frames, (x, y), q, got, exp))
        for k in list(self.streak):
            if k not in now:
                del self.streak[k]


class VramBase:
    emu = 'pyboy'
    cgb = False
    SEED = 0x1234

    def setUp(self):
        self._orig = T.PyBoy
        if self.emu == 'mgba':
            sys.path.insert(0, os.path.join(ROOT, 'tools', 'mgba'))
            from mgba_pyboy import MgbaBoy
            T.PyBoy = MgbaBoy
        self.g = T.Game(self.cgb)
        self.g.no_attr = self.emu == 'mgba'      # the harness reads VRAM bank 0 only

    def tearDown(self):
        self.g.stop()
        T.PyBoy = self._orig

    def hold(self, w, keys, n):
        g = self.g
        for b in keys:
            g.pb.button_press(b)
        for _ in range(n):
            g.pb.tick()
            w.look('+'.join(keys))
        for b in keys:
            g.pb.button_release(b)

    def map_toggle(self, w):
        g = self.g
        g.press('start', after=10)
        self.assertTrue(g.wait(lambda: g.state() == T.GS_MAP, 300))
        g.run(40)
        g.press('start', after=10)
        self.assertTrue(g.wait(lambda: g.state() == T.GS_WORLD and g.u8('pal_fade') == 0, 1500))
        for _ in range(20):
            g.pb.tick()
            w.look('after map')

    def finish(self, w, what):
        g = self.g
        self.assertGreater(w.frames, 500, 'the walk did not run')
        self.assertEqual(w.bad, [], f'{what}: {len(w.bad)}+ wrong tiles on screen, e.g. '
                         f'(where, frame, cell, quarter, vram, expected) {w.bad[:4]}')
        self.assertEqual(g.u16('dbg_vram_late'), 0, 'VBlank VRAM work ran past the VBlank')

    def test_land_on_screen_is_the_world_every_frame(self):
        g = self.g
        g.new_world(self.SEED)
        w = LandWatch(g, self.SEED)
        runs = (['right', 'b'], ['down', 'b'], ['left', 'b'], ['up', 'b'], ['down', 'right', 'b'],
                ['up', 'left', 'b'], ['up', 'right', 'b'], ['down', 'left', 'b'])
        for keys in runs:
            self.hold(w, keys, 110)
        self.map_toggle(w)
        g.set_u8('weather', T.WX_RAIN)
        g.set_time(T.T_NIGHT + 1100)
        for keys in (['right'], ['up'], ['left'], ['down'], ['up', 'right'], ['down', 'left']):
            self.hold(w, keys, 80)
        self.map_toggle(w)
        g.set_u8('weather', T.WX_STORM)
        g.set_time(T.T_DUSK + 200)          # a palette blend running under the streaming
        for keys in runs[4:] + runs[:4]:
            self.hold(w, keys, 90)
        g.set_u8('weather', T.WX_FOG)
        for keys in runs[:4]:
            self.hold(w, keys, 60)
        self.finish(w, 'walking and running')

    def test_land_far_from_the_start_and_gliding(self):
        """the coast (many water-edge fix-ups per column) and glides (32 px in 32 frames)"""
        g = self.g
        g.new_world(self.SEED)
        w = LandWatch(g, self.SEED)
        g.set_u8('items', 7)
        g.set_u8('hints', 7)
        g.set_u8('equipped', T.IT_CLOAK)
        wl = g.world()
        for (tx, ty) in (wl['beacon'][1], wl['beacon'][2]):
            g.teleport(tx, (ty + 2) & 0xFFFF)
            for keys in (['down'], ['left', 'b'], ['up', 'b'], ['right', 'b'], ['down', 'right', 'b']):
                for b in keys:
                    g.pb.button_press(b)
                for i in range(120):
                    if i % 12 == 0:
                        g.pb.button_press('a')
                    elif i % 12 == 3:
                        g.pb.button_release('a')
                    g.pb.tick()
                    w.look('glide ' + '+'.join(keys))
                for b in keys + ['a']:
                    g.pb.button_release(b)
        self.finish(w, 'gliding')

    # ---- full-screen loads with the LCD on
    def tile_data(self, first_addr, n):
        g = self.g
        return bytes(g.vram(first_addr + i) for i in range(n))

    def check_world_tiles(self, what):
        g = self.g
        n = asset_const('BG_TILE_COUNT')
        rom = rom_bytes(g, 'bg_tiles', n * 16)
        anim = T.anim_tiles()
        bad = []
        for t in range(n):
            if t in anim:
                continue
            a = 0x9000 + t * 16 if t < 128 else 0x8800 + (t - 128) * 16
            if self.tile_data(a, 16) != rom[t * 16:t * 16 + 16]:
                bad.append(t)
        self.assertEqual(bad, [], f'{what}: BG tile patterns differ from the ROM')
        n = asset_const('SPR_TILE_COUNT')
        rom = rom_bytes(g, 'spr_tiles', n * 16)
        got = self.tile_data(0x8000, n * 16)
        bad = [t for t in range(n) if got[t * 16:t * 16 + 16] != rom[t * 16:t * 16 + 16]]
        self.assertEqual(bad, [], f'{what}: sprite tile patterns differ from the ROM')
        # every animated tile holds one whole frame, never half of two
        na, nf = asset_const('ANIM_COUNT'), 4
        at = rom_bytes(g, 'anim_tile', na)
        fr = rom_bytes(g, 'anim_frames', na * nf * 16)
        for i in range(na):
            t = at[i]
            a = 0x9000 + t * 16 if t < 128 else 0x8800 + (t - 128) * 16
            got = self.tile_data(a, 16)
            self.assertIn(got, [fr[(i * nf + f) * 16:(i * nf + f + 1) * 16] for f in range(nf)],
                          f'{what}: animated tile {t} is not a whole frame')

    def test_screens_loaded_with_the_lcd_on_are_intact(self):
        g = self.g
        g.boot_to_title()
        n = asset_const('TITLE_TILE_COUNT')
        rom = rom_bytes(g, 'title_tiles', n * 16)
        bad = []
        for t in range(n):
            a = 0x9000 + t * 16 if t < 128 else 0x8800 + (t - 128) * 16
            if self.tile_data(a, 16) != rom[t * 16:t * 16 + 16]:
                bad.append(t)
        self.assertEqual(bad, [], 'title tile patterns differ from the ROM')
        tm = rom_bytes(g, 'title_map', 20 * 18)
        got = bytes(g.vram(0x9800 + y * 32 + x) for y in range(18) for x in range(20))
        self.assertEqual(got, tm, 'title map differs from the ROM')
        g.set_u16('dbg_seed', self.SEED)
        g.press('select')
        self.assertTrue(g.wait(lambda: g.state() == T.GS_WORLD and g.u8('pal_fade') == 0, 3000))
        g.run(40)
        g.wake()
        self.check_world_tiles('new world')
        for k in range(3):
            g.hold(['right', 'b'] if k & 1 else ['down'], 30)
            g.press('start', after=10)
            self.assertTrue(g.wait(lambda: g.u8('map_done'), 900), 'the map never finished')
            # the map: frame tiles from the ROM, and the tile map of the chart
            n = asset_const('MAP_TILE_COUNT')
            rom = rom_bytes(g, 'map_tiles', n * 16)
            got = self.tile_data(0x8800 + (224 - 128) * 16, n * 16)
            self.assertEqual(got, rom, 'map frame tiles differ from the ROM')
            for y in range(2, 16):
                for x in range(2, 18):
                    self.assertEqual(g.vram(0x9800 + y * 32 + x), (y - 2) * 16 + (x - 2), f'map cell {x},{y}')
            g.press('start', after=10)
            self.assertTrue(g.wait(lambda: g.state() == T.GS_WORLD and g.u8('pal_fade') == 0, 1500))
            g.run(20)
            self.check_world_tiles(f'after map {k}')
            w = LandWatch(g, self.SEED)
            w.look('after map')
            self.assertEqual(w.bad, [], 'land after the map')
        # a lesson (its own page), then the world again
        g.set_u8('items', 3)
        g.set_u8('lesson_req', T.IT_STONES + 1)
        g.close_lesson()
        g.run(20)
        self.check_world_tiles('after the lesson')
        w = LandWatch(g, self.SEED)
        w.look('after lesson')
        self.assertEqual(w.bad, [], 'land after the lesson')
        self.assertEqual(g.u16('dbg_vram_late'), 0, 'VBlank VRAM work ran past the VBlank')


class VramPyBoyDmg(VramBase, unittest.TestCase):
    pass


class VramPyBoyCgb(VramBase, unittest.TestCase):
    cgb = True


@unittest.skipUnless(os.path.exists(MGBA), 'build/mgba_server not built (make mgba)')
class VramMgbaDmg(VramBase, unittest.TestCase):
    emu = 'mgba'


@unittest.skipUnless(os.path.exists(MGBA), 'build/mgba_server not built (make mgba)')
class VramMgbaCgb(VramBase, unittest.TestCase):
    emu = 'mgba'
    cgb = True


if __name__ == '__main__':
    unittest.main()
