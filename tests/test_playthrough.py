"""A whole world, played by the autopilot with real button inputs only (tools/autopilot.py).

Boot -> title -> SELECT (seed poked into the ROM's dbg_seed hook, the only write) -> wake ->
light the start fire -> beacon 0 (burn brambles, take the STONES) -> beacon 1 (stepping stones,
take the CLOAK) -> beacon 2 (glide over the crags) -> the Heart -> a new world starts and
worlds_done goes up. Positions, items and the mods table are read from memory to close the
control loop; nothing is teleported.

Always run (CI): one seed on DMG and on CGB with a map-knowing pilot. That is ~3 minutes of
game time and ~3 s of wall time per model (PyBoy runs at 4000-8000 fps headless).

OW_SLOW=1 adds: three more seeds (one of them half water), a first-time "explore" pilot that only
knows what it has seen on screen and walks without B (~6-10 min of game time each), a run that
power-cycles after every leg and continues from the battery save, and a run started at dusk
(through the night, with the warmth / rest logic). About 2-4 minutes of wall time in all.

Run: make rom build/owgen && python3 -m unittest tests/test_playthrough.py -v
     (OW_ROM / OW_OWGEN point it at another build; OW_PLAYTHROUGH_OUT at another screenshot folder)
"""
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

ROM = os.environ.get('OW_ROM') or os.path.join(ROOT, 'build', 'open-world.gb')
SYM = os.path.splitext(ROM)[0] + '.sym'
OWGEN = os.environ.get('OW_OWGEN') or os.path.join(ROOT, 'build', 'owgen')
SLOW = os.environ.get('OW_SLOW') == '1'
OUT = os.environ.get('OW_PLAYTHROUGH_OUT') or os.path.join(ROOT, 'build', 'playthrough')


class Playthrough(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for f in (ROM, SYM, OWGEN):
            if not os.path.exists(f):
                raise unittest.SkipTest(f'missing {f}; run make rom build/owgen')
        try:
            import autopilot  # noqa: F401
        except ImportError as e:     # pyboy / pillow missing
            raise unittest.SkipTest(str(e))

    def play(self, seed, cgb, shots=False, **kw):
        import autopilot
        pc = kw.pop('power_cycle_after', ())
        tod = kw.pop('start_tod', None)
        out = OUT if shots else tempfile.mkdtemp(prefix='ow_pt_')
        p = autopilot.Pilot(ROM, SYM, OWGEN, seed, cgb, out, log=None, shots=shots, **kw)
        try:
            r = p.play(power_cycle_after=pc, start_tod=tod)
        finally:
            p.stop()
        self.check(r)
        return r

    def check(self, r):
        what = f"seed {r['seed']:#06x} {r['model']} {r['mode']}"
        self.assertTrue(r['completed'], f"{what}: not completed: {r.get('error')}")
        self.assertEqual([L['name'] for L in r['legs']], ['start', 'beacon0', 'beacon1', 'beacon2', 'heart'])
        self.assertEqual(r['worlds_done'], 1, what)
        self.assertNotEqual(r['new_seed'], r['seed'], what)
        self.assertTrue(r['new_world_moved'], f'{what}: cannot walk in the new world')
        self.assertEqual(r['collision_holes'], 0, f'{what}: the hitbox entered a solid cell')
        self.assertGreaterEqual(r['stones_used'], 1, f'{what}: the shallows were crossed without stones?')
        self.assertGreaterEqual(sum(L['burns'] for L in r['legs']), 1, what)
        self.assertGreaterEqual(sum(L['glides'] for L in r['legs']), 1, what)
        mism = [e for e in r['events'] if e['kind'] == 'land_mismatch']
        self.assertEqual(mism, [], f'{what}: streamed land differs from the host generator')
        for d in r.get('power_cycle_diffs', []):
            self.assertEqual(d, {}, f'{what}: state changed across a power cycle')

    def test_world_dmg(self):
        self.play(0x1234, False, shots=True)

    def test_world_cgb(self):
        self.play(0x1234, True, shots=True)

    @unittest.skipUnless(SLOW, 'set OW_SLOW=1')
    def test_more_seeds(self):
        for seed in (0x0001, 0x004E, 0xBEEF):      # 0x004E: about half of the region is water
            for cgb in (False, True):
                with self.subTest(seed=seed, cgb=cgb):
                    self.play(seed, cgb)

    @unittest.skipUnless(SLOW, 'set OW_SLOW=1')
    def test_first_time_player(self):
        for seed in (0x1234, 0x004E):
            with self.subTest(seed=seed):
                r = self.play(seed, False, explore=True, walk=True)
                self.assertLess(r['minutes'], 60)

    @unittest.skipUnless(SLOW, 'set OW_SLOW=1')
    def test_power_cycle_every_leg(self):
        self.play(0x1234, False, power_cycle_after=('start', 'beacon0', 'beacon1', 'beacon2'))

    @unittest.skipUnless(SLOW, 'set OW_SLOW=1')
    def test_through_the_night(self):
        r = self.play(0x1234, True, explore=True, walk=True, start_tod=24000)
        self.assertGreaterEqual(r['rests'] + r['whiteouts'], 1, 'the night never mattered')


if __name__ == '__main__':
    unittest.main()
