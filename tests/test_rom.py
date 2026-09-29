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

GS_BOOT, GS_TITLE, GS_WORLD, GS_MAP, GS_ENDING, GS_WHITEOUT = range(6)
PL_SLEEP, PL_STAND, PL_WALK, PL_SIT, PL_GLIDE = range(5)
PH_DAWN, PH_DAY, PH_DUSK, PH_NIGHT = range(4)
WX_CLEAR, WX_RAIN, WX_SNOW, WX_FOG, WX_STORM = range(5)
IT_LANTERN, IT_STONES, IT_CLOAK = range(3)
T_DAY, T_DUSK, T_NIGHT = 4320, 21600, 25920
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

    def face(self, d):
        """Turn to face direction d ('up' ...) with a tap too short to move far."""
        self.press(d, hold=1, after=2)

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
