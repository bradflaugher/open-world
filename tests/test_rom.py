"""End-to-end tests: boot the real ROM in PyBoy (headless, DMG and CGB) and play it.

The host build of the world core (build/owgen) is the oracle: the land the ROM streams into
its VRAM ring must match `owgen mt SEED X Y` metatile for metatile, wherever you walk.

Run: make test-rom   (needs `pip install pyboy pillow numpy`)
"""
import io
import os
import re
import subprocess
import unittest

from pyboy import PyBoy

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(ROOT, 'build', 'open-world.gb')
SYM = os.path.join(ROOT, 'build', 'open-world.sym')
OWGEN = os.path.join(ROOT, 'build', 'owgen')
ASSETS_H = os.path.join(ROOT, 'src', 'gb', 'assets.h')
WORLD_H = os.path.join(ROOT, 'src', 'core', 'world.h')
SHOTS = os.path.join(ROOT, 'build', 'screens')

GS_BOOT, GS_TITLE, GS_WORLD, GS_MAP, GS_ENDING, GS_WHITEOUT, GS_LESSON = range(7)
HINT_RUN, HINT_STONES, HINT_CLOAK = 1, 2, 4
PL_SLEEP, PL_STAND, PL_WALK, PL_SIT, PL_GLIDE = range(5)
PH_DAWN, PH_DAY, PH_DUSK, PH_NIGHT = range(4)
WX_CLEAR, WX_RAIN, WX_SNOW, WX_FOG, WX_STORM = range(5)
IT_LANTERN, IT_STONES, IT_CLOAK = range(3)
T_DAY, T_DUSK, T_NIGHT = 2880, 14400, 17280
SRAM_SIZE = 8192
SAVE_SLOTS = (0xA000, 0xB000)
MTF_SOLID = 0x01
MTF_GLIDE = 0x80


def _enum(path, first):
    """Names of a C enum starting with `first` (e.g. MT_SEA) -> index."""
    src = open(path).read()
    i = src.index(first)
    body = src[src.rindex('{', 0, i) + 1:src.index('}', i)]
    names = [re.sub(r'=.*', '', n).strip() for n in re.sub(r'/\*.*?\*/', '', body, flags=re.S).split(',')]
    return {n: k for k, n in enumerate(n for n in names if n)}


MT = {k: v for k, v in _enum(WORLD_H, 'MT_SEA').items() if k != 'MT_COUNT'}


SEA_SET = {MT['MT_SEA'], MT['MT_SEA_GLINT']}
WET_SET = SEA_SET | {MT['MT_SHALLOW'], MT['MT_STEPSTONE']}


def expected_tiles(grid, x, y, mt_tiles, edge_t):
    """The engine's autotile rule (land.c cell_tiles), with every neighbour known: a water
    cell's quarter picks H / V / OUTER / INNER from its 3 neighbours in that corner direction."""
    m = grid[(x, y)]
    t = list(mt_tiles[m])
    if m in SEA_SET:
        cls, same = 0, SEA_SET
    elif m == MT['MT_SHALLOW']:
        cls, same = 1, WET_SET
    else:
        return tuple(t)

    def other(dx, dy):
        n = grid.get(((x + dx) & 0xFFFF, (y + dy) & 0xFFFF))
        return n is not None and n not in same
    for q, (sx, sy) in enumerate(((-1, -1), (1, -1), (-1, 1), (1, 1))):
        a, b, c = other(0, sy), other(sx, 0), other(sx, sy)
        v = (2 if b else 0) if a else 1 if b else 3 if c else -1
        if v >= 0:
            t[q] = edge_t[cls * 16 + q * 4 + v]
    return tuple(t)


def anim_tiles():
    """the BG tiles the VBlank animates (anim_tile[] in the generated assets.c)"""
    with open(os.path.join(ROOT, 'src', 'gb', 'assets.c')) as f:
        for line in f:
            if line.startswith('const uint8_t anim_tile['):
                return {int(v) for v in line.split('{')[1].split('}')[0].split(',')}
    return set()


def owgen(*args):
    return subprocess.check_output([OWGEN] + [str(a) for a in args], text=True)


def _glyphs():
    src = open(os.path.join(ROOT, 'tools', 'owgen.c')).read()
    m = re.search(r'glyph\[MT_COUNT \+ 1\] =\s*"((?:[^"\\]|\\.)*)"', src)
    g = m.group(1).encode().decode('unicode_escape') if m else ''
    return {c: i for i, c in enumerate(g)} if len(set(g)) == len(g) == len(MT) else None


GLYPH = _glyphs() if os.path.exists(os.path.join(ROOT, 'tools', 'owgen.c')) else None


def host_region(seed, x0, y0, w, h):
    """{(mx, my): mt} from the host generator for the w x h region at (x0, y0)."""
    out = {}
    if GLYPH:
        cx, cy = (x0 + w // 2) & 0xFFFF, (y0 + h // 2) & 0xFFFF
        lines = owgen('show', seed, cx, cy, w, h).split('\n')[1:1 + h]
        for r, line in enumerate(lines):
            for c, ch in enumerate(line[:w]):
                out[((x0 + c) & 0xFFFF, (y0 + r) & 0xFFFF)] = GLYPH[ch]
        return out
    for r in range(h):
        for c in range(w):
            x, y = (x0 + c) & 0xFFFF, (y0 + r) & 0xFFFF
            out[(x, y)] = int(owgen('mt', seed, x, y))
    return out


class Game:
    def __init__(self, cgb, sram=None):
        # always pass a RAM file so a stray build/*.gb.ram never leaks into a test
        self.ram = io.BytesIO(sram if sram is not None else bytes(SRAM_SIZE))
        self.pb = PyBoy(ROM, window='null', cgb=cgb, symbols=SYM, sound_emulated=False, ram_file=self.ram)
        self.pb.set_emulation_speed(0)
        self.tag = 'cgb' if cgb else 'dmg'
        self.cgb = cgb

    # ---- memory
    def addr(self, name):
        return self.pb.symbol_lookup('_' + name)[1]

    def u8(self, name, off=0):
        return self.pb.memory[self.addr(name) + off]

    def s8(self, name, off=0):
        v = self.u8(name, off)
        return v - 256 if v >= 128 else v

    def u16(self, name, off=0):
        a = self.addr(name) + off
        return self.pb.memory[a] | self.pb.memory[a + 1] << 8

    def set_u8(self, name, v, off=0):
        self.pb.memory[self.addr(name) + off] = v & 0xFF

    def set_u16(self, name, v, off=0):
        a = self.addr(name) + off
        self.pb.memory[a] = v & 0xFF
        self.pb.memory[a + 1] = (v >> 8) & 0xFF

    def vram(self, a):
        return self.pb.memory[0, a] if self.cgb else self.pb.memory[a]

    def vram_attr(self, a):
        return self.pb.memory[1, a]

    # ---- time
    def run(self, n=1):
        for _ in range(n):
            self.pb.tick()

    def press(self, button, hold=3, after=3):
        self.pb.button_press(button)
        self.run(hold)
        self.pb.button_release(button)
        self.run(after)

    def hold(self, buttons, frames):
        for b in buttons:
            self.pb.button_press(b)
        self.run(frames)
        for b in buttons:
            self.pb.button_release(b)

    def wait(self, cond, limit=2000):
        for _ in range(limit):
            if cond():
                return True
            self.pb.tick()
        return cond()

    def shot(self, name):
        os.makedirs(SHOTS, exist_ok=True)
        self.pb.screen.image.save(os.path.join(SHOTS, f'{self.tag}_{name}.png'))

    # ---- game
    def state(self):
        return self.u8('game_state')

    def world(self):
        """world_layout_t: seed, start, beacon[3], shrine[3], heart."""
        a = self.addr('world')
        w = [self.pb.memory[a + i] | self.pb.memory[a + i + 1] << 8 for i in range(0, 34, 2)]
        return {'seed': w[0], 'start': (w[1], w[2]),
                'beacon': [(w[3 + 2 * i], w[4 + 2 * i]) for i in range(3)],
                'shrine': [(w[9 + 2 * i], w[10 + 2 * i]) for i in range(3)],
                'heart': (w[15], w[16])}

    def pos(self):
        return self.u16('pl_mx'), self.u16('pl_my'), self.u8('pl_sx'), self.u8('pl_sy')

    def boot_to_title(self):
        assert self.wait(lambda: self.state() == GS_TITLE, 900)
        self.run(40)

    def new_world(self, seed):
        self.boot_to_title()
        self.set_u16('dbg_seed', seed)
        self.press('select')
        assert self.wait(lambda: self.state() == GS_WORLD and self.u8('pal_fade') == 0, 3000)
        self.run(40)
        self.wake()

    def wake(self):
        if self.u8('pl_state') == PL_SLEEP:
            self.run(40)
            self.press('a')
            assert self.wait(lambda: self.u8('pl_state') != PL_SLEEP, 100)

    def teleport(self, x, y):
        self.set_u16('dbg_tx', x)
        self.set_u16('dbg_ty', y)
        self.set_u8('dbg_teleport', 1)
        assert self.wait(lambda: self.u8('dbg_teleport') == 0, 200)
        self.run(8)

    def close_lesson(self):
        """A shrine was just taken for the first time: its lesson plays through, then A closes it."""
        assert self.wait(lambda: self.state() == GS_LESSON, 30), 'no lesson'
        assert self.wait(lambda: self.u8('lesson_ready'), 1500), 'the lesson never finished'
        self.press('a')
        assert self.wait(lambda: self.state() == GS_WORLD and self.u8('pal_fade') == 0, 1500)
        self.run(4)

    def oam(self, i):
        """sprite i as (y, x, tile) plus the tiles of the next two slots (all from real OAM)"""
        m = self.pb.memory
        return (m[0xFE00 + i * 4], m[0xFE00 + i * 4 + 1], m[0xFE00 + i * 4 + 2],
                m[0xFE00 + i * 4 + 6], m[0xFE00 + i * 4 + 10])

    def spr_tile(self, name):
        """a sprite tile number from the generated assets.h"""
        with open(ASSETS_H) as f:
            for line in f:
                p = line.split()
                if len(p) >= 3 and p[0] == '#define' and p[1] == name:
                    return int(p[2])
        raise KeyError(name)

    def face(self, d):
        """Turn to face direction d ('up' ...) with a tap too short to move far."""
        want = {'up': 0, 'right': 2, 'down': 4, 'left': 6}[d]
        self.pb.button_press(d)
        self.wait(lambda: self.u8('pl_face') == want, 30)
        self.pb.button_release(d)
        self.run(3)

    def set_time(self, t):
        self.set_u16('tod', t)
        self.run(4)

    def sram(self):
        return bytes(self.pb.memory[0, 0xA000 + i] for i in range(SRAM_SIZE))

    def land_ring(self):
        """Metatiles on screen, decoded from VRAM map 0x9800 through the ring and scroll.
        Returns {(mx, my): mt} for every fully loaded visible slot."""
        mt_tiles = self._mt_tiles()
        inv = {}
        for m, t in enumerate(mt_tiles):
            inv.setdefault(tuple(t), m)
        cmx, cmy = self.u16('cam_mx'), self.u16('cam_my')
        out = {}
        for dy in range(9):
            for dx in range(11):
                mx, my = (cmx + dx) & 0xFFFF, (cmy + dy) & 0xFFFF
                c, r = (mx & 15) * 2, (my & 15) * 2
                a = 0x9800 + r * 32 + c
                t = (self.vram(a), self.vram(a + 1), self.vram(a + 32), self.vram(a + 33))
                out[(mx, my)] = inv.get(t, -1)
        return out

    def ring_tiles(self, mx, my):
        """The 4 BG tiles (TL, TR, BL, BR) VRAM shows for world cell (mx, my)."""
        c, r = (mx & 15) * 2, (my & 15) * 2
        a = 0x9800 + r * 32 + c
        return (self.vram(a), self.vram(a + 1), self.vram(a + 32), self.vram(a + 33))

    def edge_table(self):
        a = self.addr('edge_t')
        return [self.pb.memory[a + i] for i in range(32)]

    def _mt_tiles(self):
        a = self.addr('mt_t')
        return [[self.pb.memory[a + m * 4 + i] for i in range(4)] for m in range(len(MT))]

    def oam_line_counts(self):
        """Sprites on each of the 144 lines (8x16 mode), from real OAM."""
        cnt = [0] * 144
        for i in range(40):
            y = self.pb.memory[0xFE00 + i * 4] - 16
            x = self.pb.memory[0xFE00 + i * 4 + 1]
            if x == 0 or x >= 168:
                pass  # off-screen X still counts on hardware; keep it
            for ly in range(max(0, y), min(144, y + 16)):
                cnt[ly] += 1
        return cnt

    def stop(self):
        self.pb.stop(save=False)


DIRV = {'up': (0, -1), 'down': (0, 1), 'left': (-1, 0), 'right': (1, 0)}


def save_copy_valid(sram, base, n):
    """the ROM's save checksum (a += byte, b += a) over one copy"""
    a, b = 0x5A, 0xA5
    for x in sram[base:base + n]:
        a = (a + x) & 0xFF
        b = (b + a) & 0xFF
    return sram[base:base + 2] == b'OW' and sram[base + n] == a and sram[base + n + 1] == b


class Base(unittest.TestCase):
    CGB = False
    SEED = 0x1234

    @classmethod
    def setUpClass(cls):
        for f in (ROM, SYM, OWGEN):
            if not os.path.exists(f):
                raise unittest.SkipTest(f'missing {f}; run make rom build/owgen')

    def setUp(self):
        self.g = Game(self.CGB)

    def tearDown(self):
        self.g.stop()

    # ---- helpers
    def flags(self, mt):
        return self.g.pb.memory[self.g.addr('mt_flags') + mt]

    def walkable(self, mt):
        return not (self.flags(mt) & MTF_SOLID)

    def mt_now(self, x, y):
        """World metatile as the ROM sees it (host generator + the ROM's mods)."""
        return self.mods().get((x, y), host_region(self.SEED, x, y, 1, 1)[(x, y)])

    def mods(self):
        g = self.g
        n = g.u8('world_mod_count')
        a = g.addr('world_mods')
        out = {}
        for i in range(n):
            b = a + i * 5
            m = g.pb.memory
            out[(m[b] | m[b + 1] << 8, m[b + 2] | m[b + 3] << 8)] = m[b + 4]
        return out

    def check_land(self, what):
        """Every visible cell's four VRAM tiles equal the host generator's metatile drawn with
        the engine's water-edge rule (neighbours from the host too)."""
        g = self.g
        g.run(6)   # let the VBlank queue drain
        cmx, cmy = g.u16('cam_mx'), g.u16('cam_my')
        grid = host_region(self.SEED, (cmx - 1) & 0xFFFF, (cmy - 1) & 0xFFFF, 13, 11)
        grid.update({k: v for k, v in self.mods().items() if k in grid})
        mt_tiles, edge_t = g._mt_tiles(), g.edge_table()
        inv = {v: k for k, v in MT.items()}
        bad = []
        for dy in range(9):
            for dx in range(11):
                x, y = (cmx + dx) & 0xFFFF, (cmy + dy) & 0xFFFF
                exp = expected_tiles(grid, x, y, mt_tiles, edge_t)
                got = g.ring_tiles(x, y)
                if got != exp:
                    bad.append(((x, y), inv[grid[(x, y)]], got, exp))
        self.assertEqual(bad, [], f'{what}: land VRAM differs from owgen + edge rule at {bad[:3]}')

    def open_ground(self, center, rmin=8, rmax=30):
        """A walkable cell with walkable 4-neighbours, rmin..rmax cells from center."""
        cx, cy = center
        reg = host_region(self.SEED, cx - rmax, cy - rmax, 2 * rmax + 1, 2 * rmax + 1)
        for r in range(rmin, rmax):
            for (x, y), m in reg.items():
                if max(abs(x - cx), abs(y - cy)) != r or not self.walkable(m):
                    continue
                if all(self.walkable(reg.get((x + dx, y + dy), 0)) for dx, dy in DIRV.values()):
                    return x, y
        self.fail('no open ground')

    def stand_next_to(self, target, max_r=3):
        """Teleport onto a walkable cell 4-adjacent to target and face it. Returns the dir."""
        tx, ty = target
        for d, (dx, dy) in DIRV.items():
            x, y = (tx - dx) & 0xFFFF, (ty - dy) & 0xFFFF
            if self.walkable(self.mt_now(x, y)):
                self.g.teleport(x, y)
                self.g.face(d)
                return d
        self.fail(f'no walkable cell next to {target}')


class RomTest(Base):
    def test_boot_title(self):
        g = self.g
        g.boot_to_title()
        self.assertEqual(g.u8('is_cgb'), 1 if self.CGB else 0)
        g.run(30)
        g.shot('title')

    def test_stream_matches_host_walking(self):
        g = self.g
        g.new_world(self.SEED)
        self.assertEqual(g.world()['seed'], self.SEED)
        self.check_land('spawn')
        g.shot('day')
        for keys, n in ((['right'], 90), (['down'], 70), (['left', 'b'], 60), (['up', 'b'], 60),
                        (['down', 'right', 'b'], 80), (['up', 'left'], 70), (['up', 'right', 'b'], 60),
                        (['down', 'left', 'b'], 60)):
            g.hold(keys, n)
            self.check_land('after ' + '+'.join(keys))
        # far away, then keep streaming there
        w = g.world()
        g.teleport(w['beacon'][1][0], (w['beacon'][1][1] + 2) & 0xFFFF)
        self.check_land('teleport')
        g.hold(['left', 'b'], 90)
        self.check_land('after teleport walk')

    def test_split_line_is_clean(self):
        g = self.g
        g.new_world(self.SEED)
        g.hold(['right'], 20)
        g.run(120)      # let the band finish turning so the scroll values are stable
        if self.CGB:
            return
        for _ in range(3):
            g.run(7)
            img = g.pb.screen.ndarray
            exp = self.render_bg_lines(range(20, 28))
            spr = self.sprite_columns(range(20, 28))
            for ly in range(20, 28):
                for x in range(160):
                    if (ly, x) in spr or exp[ly][x] is None:
                        continue
                    self.assertEqual(img[ly][x][0], exp[ly][x], f'line {ly} x {x} differs')

    def render_bg_lines(self, lines):
        """the BG as VRAM says it is now; None where an animated tile is (the VBlank may have
        moved it on a frame since the screen was drawn)"""
        g, m = self.g, self.g.pb.memory
        shades = (255, 153, 85, 0)     # PyBoy's default DMG palette
        anim = anim_tiles()
        out = {}
        for ly in lines:
            band = ly < 24
            base = 0x9C00 if band else 0x9800
            scx = g.u8('band_scx') if band else g.u8('land_scx')
            scy = 0 if band else g.u8('land_scy')
            bgp = g.u8('band_bgp') if band else g.u8('land_bgp')
            row = []
            y = (scy + ly) & 255
            for x in range(160):
                px = (scx + x) & 255
                t = m[base + (y >> 3) * 32 + (px >> 3)]
                if not band and t in anim:
                    row.append(None)
                    continue
                ta = 0x9000 + t * 16 if t < 128 else 0x8800 + (t - 128) * 16
                lo, hi = m[ta + (y & 7) * 2], m[ta + (y & 7) * 2 + 1]
                bit = 7 - (px & 7)
                c = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1)
                row.append(shades[(bgp >> (c * 2)) & 3])
            out[ly] = row
        return out

    def sprite_columns(self, lines):
        m = self.g.pb.memory
        cov = set()
        for i in range(40):
            y, x = m[0xFE00 + i * 4] - 16, m[0xFE00 + i * 4 + 1] - 8
            for ly in lines:
                if y <= ly < y + 16:
                    for px in range(x, x + 8):
                        cov.add((ly, px))
        return cov

    def test_light_start_fire_and_save(self):
        g = self.g
        g.new_world(self.SEED)
        w = g.world()
        start = w['start']
        self.assertEqual(self.mt_now(*start), MT['MT_FIRE_COLD'])
        saves = g.u8('dbg_saves')
        g.face('up')
        g.press('a', after=10)
        self.assertEqual(self.mods().get(start), MT['MT_FIRE_LIT'])
        self.assertEqual((g.u16('respawn_x'), g.u16('respawn_y')), start)
        self.assertGreater(g.u8('dbg_saves'), saves)
        sram = g.sram()
        self.assertEqual(sram[0:2], b'OW')
        self.assertEqual(sram[0x1000:0x1002], b'OW')
        g.run(30)
        g.shot('fire_lit')
        self.check_land('fire lit')

    def test_collision_blocks_solid(self):
        g = self.g
        g.new_world(self.SEED)
        sx, sy = g.world()['start']
        reg = host_region(self.SEED, sx - 12, sy - 12, 24, 24)
        tried = 0
        for (x, y), m in sorted(reg.items()):
            if not (self.flags(m) & MTF_SOLID) or m == MT['MT_FIRE_COLD']:
                continue
            for d, (dx, dy) in DIRV.items():
                ax, ay = x - dx, y - dy
                if (ax, ay) in reg and self.walkable(reg[(ax, ay)]) and self.walkable(reg.get((ax - dx, ay - dy), 0)):
                    g.teleport(ax, ay)
                    g.hold([d], 40)
                    px, py, qx, qy = g.pos()
                    fx, fy = px * 16 + qx, py * 16 + qy
                    # the hitbox ([-5,4] x [-5,0] round the foot) never enters the solid cell
                    self.assertFalse(x * 16 - 4 <= fx <= x * 16 + 20 and y * 16 <= fy <= y * 16 + 20
                                     and (px, py) == (x, y), 'walked into a solid cell')
                    if d == 'right':
                        self.assertLessEqual(fx + 4, x * 16 - 1)
                    if d == 'left':
                        self.assertGreaterEqual(fx - 5, x * 16 + 16)
                    if d == 'down':
                        self.assertLessEqual(fy, y * 16 - 1)
                    if d == 'up':
                        self.assertGreaterEqual(fy - 5, y * 16 + 16)
                    tried += 1
                    break
            if tried >= 4:
                break
        self.assertGreaterEqual(tried, 2)

    def test_beacon_shrines_stones_glide(self):
        g = self.g
        g.new_world(self.SEED)
        w = g.world()
        # shrine 0 holds the stones
        self.stand_next_to(w['shrine'][0])
        g.press('a', after=10)
        g.close_lesson()
        self.assertTrue(g.u8('items') & (1 << IT_STONES))
        self.assertEqual(g.u8('equipped'), IT_STONES)
        self.assertEqual(g.u8('stones'), 12)
        self.assertEqual(self.mods().get(w['shrine'][0]), MT['MT_SHRINE_EMPTY'])
        g.shot('shrine')
        # beacon 0 lights (any item equipped)
        self.stand_next_to(w['beacon'][0])
        g.press('a', after=20)
        self.assertEqual(g.u8('beacons_lit') & 1, 1)
        self.assertEqual(self.mods().get(w['beacon'][0]), MT['MT_BEACON_LIT'])
        g.run(40)
        g.shot('beacon_lit')
        # a cairn on open ground (away from any fire, which refills the pouch), then pick it up
        ox, oy = self.open_ground(w['beacon'][0], rmin=12)
        g.teleport(ox, oy)
        g.face('right')
        g.set_u8('stones', 5)
        g.press('a', after=6)
        self.assertEqual(g.u8('cairn_n'), 1)
        self.assertEqual(g.u8('stones'), 5)          # the pouch never runs out
        c = (g.u16('cairns'), g.u16('cairns', 2))
        self.assertEqual(self.mods().get(c), MT['MT_CAIRN'])
        g.run(4)
        g.shot('cairn')
        g.press('a', after=6)
        self.assertEqual(g.u8('cairn_n'), 0)
        self.assertEqual(g.u8('stones'), 5)
        self.assertNotIn(c, self.mods())
        self.check_land('cairn picked up')
        # stepping stone on the shallows round beacon 1
        b1 = w['beacon'][1]
        reg = host_region(self.SEED, b1[0] - 7, b1[1] - 7, 15, 15)
        shallow = [k for k, v in reg.items() if v == MT['MT_SHALLOW']]
        placed = False
        for s in sorted(shallow, key=lambda k: abs(k[0] - b1[0]) + abs(k[1] - b1[1]), reverse=True):
            for d, (dx, dy) in DIRV.items():
                a = (s[0] - dx, s[1] - dy)
                if a in reg and self.walkable(reg[a]):
                    g.teleport(*a)
                    g.face(d)
                    g.press('a', after=6)
                    if self.mods().get(s) == MT['MT_STEPSTONE']:
                        placed = True
                        g.hold([d], 24)
                        self.assertEqual((g.u16('pl_mx'), g.u16('pl_my')), s, 'could not step onto the stone')
                    break
            if placed:
                break
        self.assertTrue(placed, 'no stepping stone placed')
        self.check_land('stepping stone')
        # the cloak glides over a crag
        g.set_u8('items', 7)
        g.set_u8('equipped', IT_CLOAK)
        sx, sy = w['start']
        glided = False
        reg = host_region(self.SEED, sx - 40, sy - 40, 80, 80)
        for (x, y), m in sorted(reg.items()):
            if m != MT['MT_ROCK']:
                continue
            for d, (dx, dy) in DIRV.items():
                a, l = (x - dx, y - dy), (x + 2 * dx, y + 2 * dy)
                mid = (x + dx, y + dy)
                if a in reg and l in reg and mid in reg and self.walkable(reg[a]) and self.walkable(reg[l]) \
                        and self.flags(reg[mid]) & (MTF_GLIDE) | (not self.flags(reg[mid]) & MTF_SOLID):
                    g.teleport(*a)
                    g.face(d)
                    g.pb.button_press('a')
                    g.run(12)
                    self.assertEqual(g.u8('pl_state'), PL_GLIDE)
                    g.shot('glide')
                    g.pb.button_release('a')
                    self.assertTrue(g.wait(lambda: g.u8('pl_state') != PL_GLIDE, 200))
                    self.assertEqual((g.u16('pl_mx'), g.u16('pl_my')), l)
                    glided = True
                    break
            if glided:
                break
        self.assertTrue(glided, 'no rock to glide over near the start')
        self.check_land('after glide')

    def test_whiteout_respawn(self):
        g = self.g
        g.new_world(self.SEED)
        w = g.world()
        g.face('up')
        g.press('a', after=10)      # light the start fire: the respawn point
        g.teleport(*self.open_ground(w['start']))
        g.set_time(T_NIGHT + 1100)
        g.run(40)
        g.shot('night')
        g.set_u16('warmth', 3)
        self.assertTrue(g.wait(lambda: g.state() == GS_WHITEOUT, 600))
        self.assertTrue(g.wait(lambda: g.state() == GS_WORLD and g.u8('pal_fade') == 0, 1200))
        self.assertEqual((g.u16('pl_mx'), g.u16('pl_my')), (w['start'][0], w['start'][1] + 1))
        self.assertGreaterEqual(g.u16('warmth'), 500)     # a whiteout wakes you with two pips
        self.assertLess(g.u16('tod'), T_DAY)
        self.assertEqual(self.mods().get(w['start']), MT['MT_FIRE_LIT'])
        self.check_land('after whiteout')

    def test_map_opens_and_restores(self):
        g = self.g
        g.new_world(self.SEED)
        g.hold(['right', 'b'], 60)
        g.press('start', after=10)
        self.assertTrue(g.wait(lambda: g.state() == GS_MAP, 200))
        g.run(120)
        g.shot('map')
        g.press('start', after=10)
        self.assertTrue(g.wait(lambda: g.state() == GS_WORLD and g.u8('pal_fade') == 0, 1200))
        g.run(10)
        self.check_land('after map')
        self.assertEqual(g.u8('split_mode'), 1)

    def test_save_roundtrip_continue(self):
        g = self.g
        g.new_world(self.SEED)
        g.face('up')
        g.press('a', after=10)
        g.hold(['right'], 40)
        g.face('left')
        # a save with the wanderer away from the fire: build a cairn needs stones; light instead
        g.set_u8('items', 3)
        g.set_u8('equipped', IT_STONES)
        g.set_u8('stones', 5)
        g.hold(['down'], 30)
        g.press('a', after=10)
        pos = g.pos()
        sram = g.sram()
        g.stop()
        self.g = g = Game(self.CGB, sram)
        g.boot_to_title()
        g.press('a')
        self.assertTrue(g.wait(lambda: g.state() == GS_WORLD and g.u8('pal_fade') == 0, 3000))
        self.assertEqual(g.world()['seed'], self.SEED)
        self.assertEqual(g.pos(), pos)
        self.assertEqual(self.mods().get(g.world()['start']), MT['MT_FIRE_LIT'])
        g.wake()
        self.check_land('continued')
        # a torn primary falls back to the backup copy
        bad = bytearray(sram)
        bad[5] ^= 0x5A
        g.stop()
        self.g = g = Game(self.CGB, bytes(bad))
        g.boot_to_title()
        g.press('a')
        self.assertTrue(g.wait(lambda: g.state() == GS_WORLD, 3000))
        self.assertEqual(g.pos(), pos)

    def test_ending_new_world(self):
        g = self.g
        g.new_world(self.SEED)
        w = g.world()
        g.set_u8('beacons_lit', 7)
        g.set_u8('heart_revealed', 1)
        self.stand_next_to(w['heart'])
        g.run(10)
        g.press('a')
        self.assertTrue(g.wait(lambda: g.state() == GS_ENDING, 60))
        g.run(200)
        g.shot('ending')
        self.assertTrue(g.wait(lambda: g.state() == GS_WORLD, 3000))
        self.assertEqual(g.u8('worlds_done'), 1)
        self.assertNotEqual(g.world()['seed'], self.SEED)
        self.assertEqual(g.u8('beacons_lit'), 0)

    def test_watcher_drifts_and_touch_whites_out(self):
        g = self.g
        g.new_world(self.SEED)
        w = g.world()
        g.teleport(*self.open_ground(w['start']))
        g.set_time(T_NIGHT + 1100)
        g.run(20)
        mx, my, sx, sy = g.pos()
        g.set_u16('watch_mx', mx + 2)
        g.set_u16('watch_my', my)
        g.set_u8('watch_sx', sx)
        g.set_u8('watch_sy', sy)
        g.set_u8('watch_on', 1)
        g.run(80)
        g.shot('watcher')
        self.assertEqual(g.u8('watch_on'), 1)
        d1 = (g.u16('watch_mx') - mx) * 16 + g.u8('watch_sx') - sx
        self.assertLess(d1, 32, 'the Watcher did not drift towards the light')
        self.assertTrue(g.wait(lambda: g.state() == GS_WHITEOUT, 900), 'its touch did not white out')

    def test_save_while_walking_stays_valid(self):
        """saves taken while the game frame keeps changing the visited map and the position"""
        g = self.g
        g.new_world(self.SEED)
        n = g.u16('dbg_save_len')
        saves = g.u8('dbg_saves')
        for keys in (['right', 'b'], ['down', 'b'], ['left', 'b'], ['up', 'b']):
            g.pb.button_press(keys[0]); g.pb.button_press('b')
            for _ in range(6):
                g.set_u8('save_req', 1)
                g.run(20)
            g.pb.button_release(keys[0]); g.pb.button_release('b')
        g.run(30)
        self.assertGreater(g.u8('dbg_saves'), saves + 10)
        sram = g.sram()
        for base in (0, 0x1000):
            self.assertTrue(save_copy_valid(sram, base, n), f'save copy at +{base:#x} is invalid')
        g.stop()
        self.g = g = Game(self.CGB, sram)
        g.boot_to_title()
        g.press('a')
        self.assertTrue(g.wait(lambda: g.state() == GS_WORLD, 3000))
        self.assertEqual(g.world()['seed'], self.SEED)

    def test_torn_backup_is_repaired(self):
        g = self.g
        g.new_world(self.SEED)
        g.face('up')
        g.press('a', after=10)
        n = g.u16('dbg_save_len')
        sram = bytearray(g.sram())
        g.stop()
        sram[0x1000 + 40] ^= 0xA5                    # the backup copy is damaged
        self.g = g = Game(self.CGB, bytes(sram))
        g.boot_to_title()                           # the title reads the save
        fixed = g.sram()
        self.assertTrue(save_copy_valid(fixed, 0x1000, n), 'backup not repaired')
        self.assertEqual(fixed[0x1000:0x1000 + n + 2], fixed[0:n + 2])

    def fill_mods(self, count, mt='MT_TABLE'):
        """pad the mods table with harmless far-away entries up to `count` (the default kind is
        one the engine never tidies away)"""
        g = self.g
        a = g.addr('world_mods')
        k = g.u8('world_mod_count')
        for i in range(k, count):
            b = a + i * 5
            x, y = 1000 + i * 3, 1000
            for j, v in enumerate((x & 0xFF, x >> 8, y & 0xFF, y >> 8, MT[mt])):
                g.pb.memory[b + j] = v
        g.set_u8('world_mod_count', count)
        g.run(4)

    def test_full_mod_table_refuses_without_advancing(self):
        g = self.g
        g.new_world(self.SEED)
        w = g.world()
        max_mods = 96
        # cairns may not use the last 16 slots
        self.fill_mods(max_mods - 16)
        g.set_u8('items', 3)
        g.set_u8('equipped', IT_STONES)
        g.teleport(*self.open_ground(w['start']))
        g.face('right')
        g.set_u8('stones', 5)
        g.press('a', after=6)
        self.assertEqual(g.u8('cairn_n'), 0)
        self.assertEqual(g.u8('stones'), 5)
        # ... but a beacon still lights from the reserve
        self.stand_next_to(w['beacon'][0])
        g.press('a', after=10)
        self.assertEqual(g.u8('beacons_lit') & 1, 1)
        # a truly full table: the beacon is refused and progression does not advance
        self.fill_mods(max_mods)
        self.stand_next_to(w['beacon'][2])
        g.press('a', after=10)
        self.assertEqual(g.u8('beacons_lit') & 4, 0)
        self.assertNotEqual(self.mods().get(w['beacon'][2]), MT['MT_BEACON_LIT'])

    def put_mods(self, cells):
        """write world edits straight into the mods table (the next teleport reloads the land)"""
        g = self.g
        a = g.addr('world_mods')
        k = g.u8('world_mod_count')
        for i, ((x, y), mt) in enumerate(cells):
            b = a + (k + i) * 5
            for j, v in enumerate((x & 0xFF, x >> 8, y & 0xFF, y >> 8, MT[mt])):
                g.pb.memory[b + j] = v
        g.set_u8('world_mod_count', k + len(cells))
        g.run(2)

    def open_row(self, center, n):
        """(x, y): x .. x+n on row y all walkable, and the rows above and below too"""
        cx, cy = center
        reg = host_region(self.SEED, cx - 30, cy - 30, 61, 61)
        for r in range(8, 24):
            for (x, y), m in sorted(reg.items()):
                if max(abs(x - cx), abs(y - cy)) != r:
                    continue
                if all(self.walkable(reg.get((x + i, y + j), 0)) for i in range(n + 1) for j in (-1, 0, 1)):
                    return x, y
        self.fail('no open row')

    def cloak_on(self):
        self.g.set_u8('items', 7)
        self.g.set_u8('equipped', IT_CLOAK)

    def test_glide_over_a_thick_crag(self):
        # three crags in a row (the ring is that thick at an angle): the cloak carries you over
        g = self.g
        g.new_world(self.SEED)
        x, y = self.open_row(g.world()['start'], 5)
        self.put_mods([((x + i, y), 'MT_ROCK') for i in (1, 2, 3)])
        self.cloak_on()
        g.teleport(x, y)
        g.face('right')
        g.press('a', after=2)
        self.assertEqual(g.u8('pl_state'), PL_GLIDE)
        self.assertTrue(g.wait(lambda: g.u8('pl_state') != PL_GLIDE, 200))
        self.assertEqual((g.u16('pl_mx'), g.u16('pl_my')), (x + 4, y))
        self.check_land('after a long glide')

    def test_glide_right_after_resting(self):
        # stop in front of the crags long enough to sit down: the next A still glides
        g = self.g
        g.new_world(self.SEED)
        x, y = self.open_row(g.world()['start'], 4)
        self.put_mods([((x + i, y), 'MT_ROCK') for i in (1, 2)])
        self.cloak_on()
        g.teleport(x, y)
        g.face('right')
        self.assertTrue(g.wait(lambda: g.u8('pl_state') == PL_SIT, 400))
        g.press('a', after=2)
        self.assertEqual(g.u8('pl_state'), PL_GLIDE)
        self.assertTrue(g.wait(lambda: g.u8('pl_state') != PL_GLIDE, 200))
        self.assertEqual((g.u16('pl_mx'), g.u16('pl_my')), (x + 3, y))

    def test_mashing_a_chains_glides(self):
        # A pressed just before landing is not lost: the next glide starts as you touch down
        g = self.g
        g.new_world(self.SEED)
        x, y = self.open_row(g.world()['start'], 7)
        self.put_mods([((x + i, y), 'MT_ROCK') for i in (1, 2, 4, 5)])
        self.cloak_on()
        g.teleport(x, y)
        g.face('right')
        g.press('a', hold=2, after=0)
        self.assertTrue(g.wait(lambda: g.u8('pl_state') == PL_GLIDE, 4))
        for _ in range(14):
            g.press('a', hold=1, after=1)           # mashing
        self.assertTrue(g.wait(lambda: g.u16('pl_mx') == x + 6 and g.u8('pl_state') != PL_GLIDE, 200))
        self.assertEqual(g.u16('pl_my'), y)

    def test_a_after_sitting_down_in_the_open_acts(self):
        # stand at a cold fire long enough that the wanderer sits: one A still lights it
        g = self.g
        g.new_world(self.SEED)
        w = g.world()
        self.stand_next_to(w['start'])
        self.assertTrue(g.wait(lambda: g.u8('pl_state') == PL_SIT, 400))
        g.press('a', after=10)
        self.assertEqual(self.mods().get(w['start']), MT['MT_FIRE_LIT'])
        self.assertNotEqual(g.u8('pl_state'), PL_SIT)

    def test_fires_still_light_after_a_long_world(self):
        # a world's worth of lit fires fills the mods table: the far ones burn down (never the
        # one you wake at), so the fires on the way to the Heart can still be lit
        g = self.g
        g.new_world(self.SEED)
        w = g.world()
        start_n = g.u8('world_mod_count')
        self.fill_mods(96, 'MT_FIRE_LIT')
        keep = (1000 + start_n * 3, 1000)
        g.set_u16('respawn_x', keep[0])
        g.set_u16('respawn_y', keep[1])
        self.assertTrue(g.wait(lambda: g.u8('world_mod_count') <= 96 - 25, 1200))
        self.assertEqual(self.mods().get(keep), MT['MT_FIRE_LIT'], 'the respawn fire went out')
        self.stand_next_to(w['start'])
        g.press('a', after=10)
        self.assertEqual(self.mods().get(w['start']), MT['MT_FIRE_LIT'])
        self.check_land('after the tidy')

    def test_title_select_needs_a_hold_over_a_save(self):
        g = self.g
        g.new_world(self.SEED)
        g.face('up')
        g.press('a', after=10)                  # light the fire: a save
        sram = g.sram()
        g.stop()
        self.g = g = Game(self.CGB, sram)
        g.boot_to_title()
        g.press('select', hold=10, after=30)    # a short press does nothing
        self.assertEqual(g.state(), GS_TITLE)
        g.shot('title_hold')
        g.pb.button_press('select')
        g.run(60)
        self.assertEqual(g.state(), GS_TITLE)   # still guttering
        g.shot('title_guttering')
        self.assertTrue(g.wait(lambda: g.state() == GS_WORLD, 400))
        g.pb.button_release('select')
        self.assertNotEqual(g.world()['seed'], self.SEED)

    def test_a_at_lit_fire_sits_and_rests(self):
        g = self.g
        g.new_world(self.SEED)
        g.face('up')
        g.press('a', after=10)                  # light it
        g.run(20)
        g.press('a', after=4)                   # and rest by it
        self.assertEqual(g.u8('pl_state'), PL_SIT)
        self.assertEqual(g.u8('shake'), 0)
        t0 = g.u16('tod')
        g.run(60)
        self.assertGreaterEqual(g.u16('tod') - t0, 60 * 7, 'time runs fast by a fire')
        g.press('b', after=4)
        self.assertNotEqual(g.u8('pl_state'), PL_SIT)

    def test_sitting_in_the_open_is_normal_speed(self):
        g = self.g
        g.new_world(self.SEED)
        g.teleport(*self.open_ground(g.world()['start']))
        g.set_time(T_NIGHT + 1100)
        g.run(200)                               # sits after ~3 s idle
        self.assertEqual(g.u8('pl_state'), PL_SIT)
        t0 = g.u16('tod')
        g.run(60)
        self.assertLessEqual(g.u16('tod') - t0, 62)

    def test_watcher_withdraws_near_a_fire(self):
        g = self.g
        g.new_world(self.SEED)
        g.face('up')
        g.press('a', after=10)
        g.hold(['down'], 12)
        g.set_time(T_NIGHT + 1100)
        g.run(20)
        mx, my, sx, sy = g.pos()
        g.set_u16('watch_mx', mx + 2)
        g.set_u16('watch_my', my)
        g.set_u8('watch_sx', sx)
        g.set_u8('watch_sy', sy)
        g.set_u8('watch_on', 1)
        self.assertTrue(g.wait(lambda: g.u8('watch_on') == 0, 200), 'the Watcher stayed by the fire')
        g.run(600)
        self.assertEqual(g.state(), GS_WORLD)
        self.assertEqual(g.u8('watch_on'), 0, 'a Watcher spawned by the fire')

    def test_dmg_fog_keeps_the_land_legible(self):
        g = self.g
        g.new_world(self.SEED)
        g.set_u8('weather', WX_FOG)
        g.run(120)
        bgp = g.u8('land_bgp')
        shades = [(bgp >> (2 * c)) & 3 for c in range(4)]
        self.assertNotEqual(shades[1], shades[2], 'fog merged ground and detail')
        g.shot('fog')

    # ---- hints and lessons
    def shallow_side(self, center, r=7):
        """Teleport next to a shallows cell near center and face it."""
        g = self.g
        reg = host_region(self.SEED, center[0] - r, center[1] - r, 2 * r + 1, 2 * r + 1)
        for s, v in sorted(reg.items()):
            if v != MT['MT_SHALLOW']:
                continue
            for d, (dx, dy) in DIRV.items():
                a = (s[0] - dx, s[1] - dy)
                if a in reg and self.walkable(reg[a]):
                    g.teleport(*a)
                    g.face(d)
                    return s
        self.fail('no shallows')

    def test_lessons_play_once_and_are_saved(self):
        g = self.g
        g.new_world(self.SEED)
        w = g.world()
        self.assertEqual(g.u8('hints'), 0)
        self.stand_next_to(w['shrine'][0])
        g.press('a', after=4)
        self.assertTrue(g.wait(lambda: g.state() == GS_LESSON, 30))
        g.run(60)
        g.press('a', after=10)
        self.assertEqual(g.state(), GS_LESSON, 'closed before it had played through')
        g.shot('lesson_stones')
        g.close_lesson()
        self.assertEqual(g.u8('hints') & HINT_STONES, HINT_STONES)
        self.assertEqual(g.u8('equipped'), IT_STONES)       # the lesson's SELECTs are only shown
        self.assertEqual(g.u8('dbg_lessons'), 1)
        self.check_land('after the lesson')
        self.stand_next_to(w['shrine'][1])
        g.press('a', after=4)
        g.run(200)
        g.shot('lesson_cloak')
        g.close_lesson()
        self.assertEqual(g.u8('hints') & (HINT_STONES | HINT_CLOAK), HINT_STONES | HINT_CLOAK)
        self.assertEqual(g.u8('equipped'), IT_CLOAK)
        g.set_u8('save_req', 1)
        g.run(20)
        # a new world over the save (hold SELECT on the title): the lessons are not shown again
        sram = g.sram()
        g.stop()
        self.g = g = Game(self.CGB, sram)
        g.boot_to_title()
        g.pb.button_press('select')
        self.assertTrue(g.wait(lambda: g.state() == GS_WORLD and g.u8('pal_fade') == 0, 3000))
        g.pb.button_release('select')
        g.run(40)
        g.wake()
        self.assertEqual(g.u8('hints') & 6, 6)
        w = g.world()
        self.SEED = w['seed']
        self.stand_next_to(w['shrine'][0])
        g.press('a', after=30)
        self.assertTrue(g.u8('items') & (1 << IT_STONES))
        self.assertEqual(g.state(), GS_WORLD)
        self.assertEqual(g.u8('dbg_lessons'), 0)

    def test_run_hint_until_the_first_run(self):
        g = self.g
        g.new_world(self.SEED)
        slot = 4 * 4
        g.pb.button_press('left')
        g.run(60)
        self.assertEqual(g.u8('run_hint_on'), 0, 'too soon')
        g.run(120)
        self.assertEqual(g.u8('run_hint_on'), 1)
        self.assertNotEqual(g.pb.memory[0xFE00 + slot], 0)
        self.assertEqual(g.pb.memory[0xFE00 + slot + 2], g.spr_tile('SPR_HINT_B'))
        g.shot('run_hint')
        g.pb.button_press('b')
        g.run(40)
        g.pb.button_release('b')
        g.pb.button_release('left')
        self.assertTrue(g.u8('hints') & HINT_RUN)
        g.hold(['right'], 200)
        self.assertEqual(g.u8('run_hint_on'), 0, 'the run hint came back')

    def test_select_hint_when_another_item_would_act(self):
        g = self.g
        g.new_world(self.SEED)
        g.set_u8('hints', 0xFF)
        g.set_u8('items', (1 << IT_LANTERN) | (1 << IT_STONES))
        g.set_u8('equipped', IT_LANTERN)
        self.shallow_side(g.world()['beacon'][1])
        g.run(6)
        # (read from OAM: hint_on itself may be caught mid game frame)
        self.assertEqual(g.oam(2)[2:], (g.spr_tile('SPR_HINT_SEL'), g.spr_tile('SPR_HINT_SEL') + 2,
                                        g.spr_tile('SPR_HINT_SEL') + 4))
        g.shot('select_hint')
        g.press('select', after=6)
        self.assertEqual(g.u8('equipped'), IT_STONES)
        self.assertEqual(g.oam(2)[2], g.spr_tile('SPR_HINT_A'))
        self.assertEqual(g.pb.memory[0xFE00 + 3 * 4], 0, 'the SELECT hint is still shown')

    def test_version_2_save_still_loads(self):
        g = self.g
        g.new_world(self.SEED)
        g.face('up')
        g.press('a', after=10)                  # light the fire: a save
        g.set_u8('items', 3)
        g.set_u8('save_req', 1)
        g.run(20)
        pos = g.pos()
        n = g.u16('dbg_save_len')
        sram = bytearray(g.sram())
        g.stop()
        # the same save as a version 2 copy: no hints byte at the end
        for base in (0, 0x1000):
            sram[base + 2] = 2
            a, b = 0x5A, 0xA5
            for x in sram[base:base + n - 1]:
                a = (a + x) & 0xFF
                b = (b + a) & 0xFF
            sram[base + n - 1] = a
            sram[base + n] = b
        self.g = g = Game(self.CGB, bytes(sram))
        g.boot_to_title()
        g.press('a')
        self.assertTrue(g.wait(lambda: g.state() == GS_WORLD and g.u8('pal_fade') == 0, 3000))
        self.assertEqual(g.world()['seed'], self.SEED)
        self.assertEqual(g.pos(), pos)
        self.assertEqual(g.u8('hints'), HINT_STONES)    # carried items count as taught
        g.wake()
        self.check_land('continued from a v2 save')
        g.set_u8('save_req', 1)
        g.run(20)
        sram = g.sram()
        for base in (0, 0x1000):
            self.assertEqual(sram[base + 2], 3)
            self.assertTrue(save_copy_valid(sram, base, n))

    def test_no_frame_drops_walking(self):
        g = self.g
        g.new_world(self.SEED)
        g.run(30)
        d0 = g.u16('dbg_frame_drops')
        for keys in (['right'], ['down', 'b'], ['left'], ['up', 'right', 'b'], ['down', 'left'], ['right', 'b']):
            g.hold(keys, 100)
        self.assertEqual(g.u16('dbg_frame_drops') - d0, 0, 'frames dropped while walking')

    def test_sprites_per_line_at_night_in_rain(self):
        g = self.g
        g.new_world(self.SEED)
        g.set_time(T_NIGHT + 1100)
        g.set_u8('weather', WX_RAIN)
        g.run(60)
        worst = 0
        g.pb.button_press('right')
        for f in range(240):
            g.run(1)
            if f % 3 == 0:
                worst = max(worst, max(g.oam_line_counts()))
            if f == 120:
                g.shot('night_rain')
        g.pb.button_release('right')
        self.assertLessEqual(worst, 10)
        self.assertEqual(g.u8('glow_on'), 1)


class RomTestCGB(RomTest):
    CGB = True


if __name__ == '__main__':
    unittest.main()
