"""Frame-budget and stack tests: a representative stress run with the ROM's own profiler.

The ROM stamps LY at each stage of the VBlank work (gfx.h PSTAGE: drain, sound, then the
game frame's stages) and keeps, per stage, the longest run in lines, a histogram of the
line the game frame ended on (10-line buckets; bucket 16 = ran into the next frame), and the
breakdown of the worst frame. At boot the free RAM between the heap and the stack is painted
(isr.s stack_paint), so the stack's high-water mark is the first unpainted byte.

The stress: running in six directions through each time of day and weather (rain, snow,
fog, storm; day, dusk, night, dawn), map toggles, and a Watcher standing next to you.
It runs on PyBoy, DMG and CGB, and on mGBA as well when build/mgba_server exists
(`make mgba`); skipped otherwise.

Run: make test-perf (also part of make test-rom).
"""
import json
import os
import sys
import unittest

import test_rom as T

ROOT = T.ROOT
MGBA = os.path.join(ROOT, 'build', 'mgba_server')
PF = ['drain', 'sound', 'input', 'player', 'time', 'watch', 'band', 'draw', 'fx', 'commit', 'end']

# Worst-case end of the VBlank work, in lines after the start of VBlank (a frame is 154).
# DMG measured ~95-100 on this stress (was 142-152 before the budget work); CGB (double
# speed) ~56-72. The margins absorb emulator and seed differences, not regressions.
END_MAX = {False: 112, True: 80}
STALL_FRAC = 0.015          # frames the wanderer waited for the land streamer
STACK_MIN = 256             # bytes of stack never touched, after the whole run


def profile(pb):
    m = pb.memory

    def a(n):
        return pb.symbol_lookup('_' + n)[1]

    def u16(n, i=0):
        return m[a(n) + 2 * i] | m[a(n) + 2 * i + 1] << 8
    pmax = [m[a('dbg_pmax') + i] for i in range(len(PF))]
    worst = [m[a('dbg_pworst') + i] for i in range(len(PF))]
    hist = [u16('dbg_phist', i) for i in range(17)]
    end = u16('dbg_ram_end')
    lo = end
    while lo < 0xE000 and m[lo] == 0xA5:
        lo += 1
    return {'pmax': dict(zip(PF, pmax)), 'worst': dict(zip(PF, worst)), 'hist': hist,
            'stack_free': lo - end, 'drops': u16('dbg_frame_drops'), 'nest': u16('dbg_nest'),
            'stalls': u16('dbg_stalls'), 'frames': u16('dbg_world_frames')}


def stress(g):
    g.new_world(0x1234)
    for tod, wx in ((T.T_DAY + 500, T.WX_CLEAR), (T.T_DUSK + 300, T.WX_RAIN),
                    (T.T_DUSK + 1100, T.WX_STORM), (T.T_NIGHT + 1100, T.WX_RAIN),
                    (T.T_NIGHT + 1100, T.WX_FOG), (T.T_NIGHT + 1100, T.WX_SNOW),
                    (T.T_DAY + 100, T.WX_FOG), (600, T.WX_STORM)):
        g.set_time(tod)
        g.set_u8('weather', wx)
        for keys in (['right', 'b'], ['down', 'right', 'b'], ['up', 'b'], ['left', 'up', 'b'],
                     ['down'], ['left', 'b']):
            g.hold(keys, 90)
        g.press('start', after=10)
        g.wait(lambda: g.state() == T.GS_MAP, 300)
        g.run(90)
        g.press('start', after=10)
        g.wait(lambda: g.state() == T.GS_WORLD and g.u8('pal_fade') == 0, 1500)
        mx, my, _, _ = g.pos()
        g.set_u16('watch_mx', mx + 3)
        g.set_u16('watch_my', my)
        g.set_u8('watch_on', 1)
        g.run(240)
    return profile(g.pb)


class PerfBase:
    emu = 'pyboy'
    cgb = False

    def test_stress_budget(self):
        orig = T.PyBoy
        if self.emu == 'mgba':
            sys.path.insert(0, os.path.join(ROOT, 'tools', 'mgba'))
            from mgba_pyboy import MgbaBoy
            T.PyBoy = MgbaBoy
        try:
            g = T.Game(self.cgb)
            try:
                r = stress(g)
            finally:
                g.stop()
        finally:
            T.PyBoy = orig
        tag = f'{self.emu} {"cgb" if self.cgb else "dmg"}'
        print(f'\nPERF {tag}: ' + json.dumps(r), file=sys.stderr)
        self.assertGreater(r['frames'], 5000, 'the stress did not run')
        self.assertEqual(r['hist'][16], 0, 'a game frame ran into the next frame')
        self.assertEqual(r['drops'], 0, 'game frames were dropped')
        self.assertEqual(r['nest'], 0, 'a VBlank interrupt arrived inside the game frame')
        self.assertLessEqual(r['pmax']['end'], END_MAX[self.cgb],
                             f'worst VBlank work end {r["pmax"]["end"]} lines (worst frame {r["worst"]})')
        self.assertLessEqual(r['stalls'], STALL_FRAC * r['frames'], 'the land streamer fell behind')
        self.assertGreaterEqual(r['stack_free'], STACK_MIN, 'stack headroom')


class PerfPyBoyDmg(PerfBase, unittest.TestCase):
    pass


class PerfPyBoyCgb(PerfBase, unittest.TestCase):
    cgb = True


@unittest.skipUnless(os.path.exists(MGBA), 'build/mgba_server not built (make mgba)')
class PerfMgbaDmg(PerfBase, unittest.TestCase):
    emu = 'mgba'


@unittest.skipUnless(os.path.exists(MGBA), 'build/mgba_server not built (make mgba)')
class PerfMgbaCgb(PerfBase, unittest.TestCase):
    emu = 'mgba'
    cgb = True


if __name__ == '__main__':
    unittest.main()
