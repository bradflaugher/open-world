"""SDCC-vs-gcc equivalence test for the world core.

Builds a tiny Game Boy ROM from src/core/*.c (build/core_rom/core_rom.gb) that computes world_mt
over regions on request, runs it in PyBoy, and compares every metatile with the host build
(build/owgen dump). Regions: around the start, every beacon and shrine, the Heart, points along
each causeway, and far-away open country; each region is read in several access orders (row,
column, reversed, scattered), because the core's caches must never change a result.

Run from the repository root:  python3 -m unittest tests/test_core_rom.py -v
(needs /opt/gbdk and the pyboy module; the ROM and owgen are built as needed.)
"""
import os
import re
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GBDK = os.environ.get('GBDK_HOME', '/opt/gbdk')
BUILD = os.path.join(ROOT, 'build', 'core_rom')
ROM = os.path.join(BUILD, 'core_rom.gb')
OWGEN = os.path.join(ROOT, 'build', 'owgen')
SEEDS = [0x1234, 1, 7, 0xBEEF, 4242, 65535]

ROM_SRC = r'''
#include <gb/gb.h>
#include <stdint.h>
#include "world.h"
/* driven by the Python side: it writes a job, sets cmd = 1 and waits for state == 2 */
volatile uint8_t cmd, state, ready;
volatile uint16_t job_seed = 0xFFFF, job_x, job_y;
volatile uint8_t job_w, job_h, job_order;
volatile uint16_t lay[16];
uint8_t buf[1024];
static uint16_t cur_seed = 0xFFFF;
static uint8_t hx, hy;
void main(void)
{
    uint16_t i, n, k;
    uint8_t w, h;
    state = 0;
    ready = 0x5A;
    while (1) {
        while (cmd != 1) wait_vbl_done();
        cmd = 0;
        state = 1;
        if (job_seed != cur_seed) {
            cur_seed = job_seed;
            world_init(cur_seed);
            lay[0] = world.start.x; lay[1] = world.start.y;
            for (i = 0; i < NUM_BEACONS; i++) {
                lay[2 + 2 * i] = world.beacon[i].x; lay[3 + 2 * i] = world.beacon[i].y;
                lay[8 + 2 * i] = world.shrine[i].x; lay[9 + 2 * i] = world.shrine[i].y;
            }
            lay[14] = world.heart.x; lay[15] = world.heart.y;
        }
        w = job_w;
        h = job_h;
        n = (uint16_t)w * h;
        for (k = 0; k < n; k++) {
            switch (job_order) {
            case 0: i = k; break;                                         /* rows */
            case 1: i = (uint16_t)((k % h) * w + k / h); break;          /* columns */
            case 2: i = (uint16_t)(n - 1 - k); break;                     /* reversed */
            default: i = (uint16_t)((k * 97u + 13u) % n); break;          /* scattered (n not a multiple of 97) */
            }
            hx = (uint8_t)(i % w);
            hy = (uint8_t)(i / w);
            buf[i] = world_mt((uint16_t)(job_x + hx), (uint16_t)(job_y + hy));
        }
        state = 2;
    }
}
'''


def build_rom():
    os.makedirs(BUILD, exist_ok=True)
    src = os.path.join(BUILD, 'core_rom.c')
    with open(src, 'w') as f:
        f.write(ROM_SRC)
    core = [os.path.join(ROOT, 'src', 'core', n) for n in sorted(os.listdir(os.path.join(ROOT, 'src', 'core')))
            if n.endswith('.c')]
    subprocess.run([os.path.join(GBDK, 'bin', 'lcc'), '-I' + os.path.join(ROOT, 'src', 'core'), '-Wl-j',
                    '-autobank', '-Wm-yt0x1B', '-Wm-ya1', '-Wf--max-allocs-per-node50000', '-o', ROM, src] + core,
                   check=True, capture_output=True)


def build_owgen():
    subprocess.run(['make', '-s', 'build/owgen'], cwd=ROOT, check=True, capture_output=True)


def host_dump(seed, x0, y0, w, h):
    out = subprocess.run([OWGEN, 'dump', str(seed), str(x0), str(y0), str(w), str(h)],
                         check=True, capture_output=True, text=True).stdout.split()
    return bytes.fromhex(''.join(out))


class CoreRomEquivalence(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import pyboy  # noqa: F401
        except ImportError:
            raise unittest.SkipTest('pyboy not installed')
        if not os.path.exists(os.path.join(GBDK, 'bin', 'lcc')):
            raise unittest.SkipTest('GBDK not found')
        build_rom()
        build_owgen()
        cls.syms = {}
        with open(ROM[:-3] + '.noi') as f:
            for line in f:
                m = re.match(r'DEF (\S+) 0x([0-9A-Fa-f]+)', line)
                if m:
                    cls.syms[m.group(1)] = int(m.group(2), 16)
        from pyboy import PyBoy
        cls.pb = PyBoy(ROM, window='null')
        cls.pb.set_emulation_speed(0)
        for _ in range(60 * 30):   # boot, crt0 (clears RAM), then the command loop
            cls.pb.tick()
            if cls.pb.memory[cls.syms['_ready']] == 0x5A:
                break

    @classmethod
    def tearDownClass(cls):
        cls.pb.stop(save=False)

    def w8(self, name, v, off=0):
        self.pb.memory[self.syms[name] + off] = v & 0xFF

    def w16(self, name, v):
        self.w8(name, v)
        self.w8(name, v >> 8, 1)

    def r16(self, name, i=0):
        a = self.syms[name] + 2 * i
        return self.pb.memory[a] | (self.pb.memory[a + 1] << 8)

    def job(self, seed, x0, y0, w, h, order):
        self.w16('_job_seed', seed)
        self.w16('_job_x', x0)
        self.w16('_job_y', y0)
        self.w8('_job_w', w)
        self.w8('_job_h', h)
        self.w8('_job_order', order)
        self.w8('_state', 0)
        self.w8('_cmd', 1)
        for _ in range(60 * 120):
            self.pb.tick()
            if self.pb.memory[self.syms['_state']] == 2:
                break
        else:
            self.fail('ROM did not finish the job')
        b = self.syms['_buf']
        return bytes(self.pb.memory[b + i] for i in range(w * h))

    def regions(self, seed):
        self.job(seed, 0, 0, 1, 1, 0)   # world_init
        lay = [self.r16('_lay', i) for i in range(16)]
        sx, sy = lay[0], lay[1]
        pts = [(sx, sy)] + [(lay[2 + 2 * i], lay[3 + 2 * i]) for i in range(3)] + [(lay[14], lay[15])]
        regs = [(x - 8, y - 8, 17, 17) for x, y in pts]
        # along each causeway (start -> beacons / heart), a few samples
        for x, y in pts[1:]:
            for t in (1, 2, 3):
                regs.append((sx + (x - sx) * t // 4 - 6, sy + (y - sy) * t // 4 - 6, 13, 13))
        regs.append((sx + 2000, sy - 1500, 24, 20))      # open country
        regs.append((sx - 300, sy + 400, 20, 24))
        return lay, [(x & 0xFFFF, y & 0xFFFF, w, h) for x, y, w, h in regs]

    def test_layout_matches_host(self):
        for seed in SEEDS:
            lay, _ = self.regions(seed)
            host = subprocess.run([OWGEN, 'layoutraw', str(seed)], check=True, capture_output=True,
                                  text=True).stdout.split()
            self.assertEqual(lay, [int(v) for v in host], 'layout differs for seed %#x' % seed)

    def test_metatiles_match_host(self):
        bad = []
        for seed in SEEDS:
            _, regs = self.regions(seed)
            for n, (x0, y0, w, h) in enumerate(regs):
                ref = host_dump(seed, x0, y0, w, h)
                for order in range(4) if n < 5 else (0, 3):
                    got = self.job(seed, x0, y0, w, h, order)
                    if got != ref:
                        diffs = [(x0 + i % w, y0 + i // w, got[i], ref[i]) for i in range(w * h) if got[i] != ref[i]]
                        bad.append('seed %#x region (%d,%d %dx%d) order %d: %d cells differ, e.g. %s (rom, host)'
                                   % (seed, x0, y0, w, h, order, len(diffs), diffs[:4]))
        self.assertEqual(bad, [], '\n'.join(bad))

    def test_lead_repro(self):
        """seed 0x1234, beacon 1 area refilled in rows (the engine's pattern)"""
        ref = host_dump(0x1234, 32923, 32845, 16, 16)
        self.assertEqual(self.job(0x1234, 32923, 32845, 16, 16, 0), ref)


if __name__ == '__main__':
    unittest.main()
