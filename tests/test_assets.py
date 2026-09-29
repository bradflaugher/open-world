"""Tests for the OPEN WORLD asset pipeline (tools/gen_assets.py). Stdlib unittest.

Run: python3 -m unittest discover -s tests -p 'test_assets.py'
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import gen_assets as G  # noqa: E402

LIGHT_MTS = {'MT_SEA_GLINT', 'MT_FIRE_LIT', 'MT_BEACON_LIT', 'MT_HEART'}


def c_array(src, name):
    """integer values of `const uintN_t name[...]... = {...};`"""
    m = re.search(r'const\s+uint(?:8|16)_t\s+%s\s*(?:\[[^\]]*\])+\s*=\s*\{(.*?)\};' % name, src, re.S)
    if not m:
        raise AssertionError('array %s not found' % name)
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    body = body.replace('{', ' ').replace('}', ' ')
    return [int(v, 0) for v in body.replace('\n', ' ').split(',') if v.strip()]


def luma(c):
    r, g, b = c
    return 0.299 * r + 0.587 * g + 0.114 * b


class TestAssets(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix='ow_assets_')
        assert G.main(['--assets', os.path.join(ROOT, 'assets'), '--out-dir', cls.tmp]) == 0
        with open(os.path.join(cls.tmp, 'assets.h')) as f:
            cls.h = f.read()
        with open(os.path.join(cls.tmp, 'assets.c')) as f:
            cls.c = f.read()
        cls.d = G.load_assets(os.path.join(ROOT, 'assets'))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def define(self, name):
        m = re.search(r'^#define\s+%s\s+(\S+)' % name, self.h, re.M)
        self.assertIsNotNone(m, 'missing #define %s' % name)
        return int(m.group(1), 0)

    # ---- pipeline ------------------------------------------------------------------------
    def test_encode_tile(self):
        t = [[0, 1, 2, 3, 0, 1, 2, 3]] + [[3] * 8] + [[0] * 8] * 6
        e = G.encode_tile(t)
        self.assertEqual(e[0:2], [0b01010101, 0b00110011])
        self.assertEqual(e[2:4], [0xFF, 0xFF])
        self.assertEqual(e[4:], [0] * 12)

    def test_deterministic(self):
        h1, c1 = G.gen(G.load_assets(os.path.join(ROOT, 'assets')))
        self.assertEqual(h1, self.h)
        self.assertEqual(c1, self.c)

    def test_generated_files_up_to_date(self):
        for fn, txt in (('assets.h', self.h), ('assets.c', self.c)):
            p = os.path.join(ROOT, 'src', 'gb', fn)
            self.assertTrue(os.path.exists(p), '%s missing: run make assets' % p)
            with open(p) as f:
                self.assertEqual(f.read(), txt, '%s is stale: run make assets' % fn)

    def test_bad_input_rejected(self):
        d = tempfile.mkdtemp(prefix='ow_bad_')
        try:
            with open(os.path.join(d, 'x.txt'), 'w') as f:
                f.write('@meta MT_SEA pal=WATER\n.......\n')
            self.assertRaises(G.AssetError, G.load_assets, d)
            with open(os.path.join(d, 'x.txt'), 'w') as f:
                f.write('@meta MT_NOPE pal=WATER\n' + ('.' * 16 + '\n') * 16)
            self.assertRaises(G.AssetError, G.load_assets, d, False)
        finally:
            shutil.rmtree(d)

    # ---- contract ------------------------------------------------------------------------
    def test_contract_symbols(self):
        with open(os.path.join(ROOT, 'docs', 'CONTRACTS.md')) as f:
            doc = f.read()
        block = doc[doc.index('## Assets'):doc.index('## World core')]
        for name in re.findall(r'#define\s+(\w+)', block):
            self.assertRegex(self.h, r'#define\s+%s\b' % name)
        for name in re.findall(r'extern\s+const\s+\w+\s+(\w+)\s*\[', block):
            self.assertRegex(self.h, r'extern\s+const\s+\w+\s+%s\s*\[' % name)
            self.assertRegex(self.c, r'const\s+\w+\s+%s\s*\[' % name)
        self.assertIn('extern const uint8_t dmg_obp0[4], dmg_obp1[4];', self.h)
        self.assertIn('enum { PAL_GRASS, PAL_FOREST, PAL_WATER, PAL_SAND, PAL_SNOW, PAL_ROCK, '
                      'PAL_LIGHT, PAL_SKY };', self.h)
        self.assertIn('#include "world.h"', self.h)

    def test_limits(self):
        self.assertLessEqual(self.define('BG_TILE_COUNT'), 224)
        self.assertLessEqual(self.define('SPR_TILE_COUNT'), 128)
        self.assertLessEqual(self.define('TITLE_TILE_COUNT'), 200)
        self.assertLessEqual(self.define('MAP_TILE_COUNT'), 256)
        self.assertEqual(self.define('ANIM_FRAMES'), 4)
        self.assertEqual(self.define('ANIM_PERIOD'), 16)
        # the engine updates anim i on frame (vbl & 15) == i
        self.assertLessEqual(self.define('ANIM_COUNT'), 16)
        self.assertEqual(len(c_array(self.c, 'bg_tiles')), 16 * self.define('BG_TILE_COUNT'))
        self.assertEqual(len(c_array(self.c, 'spr_tiles')), 16 * self.define('SPR_TILE_COUNT'))
        self.assertEqual(len(c_array(self.c, 'title_tiles')), 16 * self.define('TITLE_TILE_COUNT'))
        self.assertEqual(len(c_array(self.c, 'map_tiles')), 16 * self.define('MAP_TILE_COUNT'))
        self.assertEqual(len(c_array(self.c, 'anim_frames')), 64 * self.define('ANIM_COUNT'))

    def test_every_metatile(self):
        self.assertEqual(len(G.MT_ORDER), 36)
        with open(os.path.join(ROOT, 'src', 'core', 'world.h')) as f:
            wh = f.read()
        enum = re.findall(r'^\s+(MT_\w+)', wh[wh.index('MT_SEA = 0'):wh.index('MT_COUNT')], re.M)
        self.assertEqual(['MT_SEA'] + enum, G.MT_ORDER, 'MT order differs from world.h')
        n = self.define('BG_TILE_COUNT')
        tiles = c_array(self.c, 'mt_tiles')
        attrs = c_array(self.c, 'mt_attr')
        self.assertEqual(len(tiles), 36 * 4)
        self.assertEqual(len(attrs), 36 * 4)
        for t in tiles:
            self.assertLess(t, n)
        for a in attrs:
            self.assertLessEqual(a, 7, 'mt_attr must be a palette number, no flips / bank bits')

    def test_anims(self):
        n = self.define('BG_TILE_COUNT')
        at = c_array(self.c, 'anim_tile')
        self.assertEqual(len(at), self.define('ANIM_COUNT'))
        self.assertEqual(len(set(at)), len(at))
        used = set(c_array(self.c, 'mt_tiles'))
        for t in at:
            self.assertLess(t, n)
            self.assertIn(t, used, 'animated tile %d not used by any metatile' % t)
        # an animated tile is never shared with a static quarter
        for i, a in enumerate(self.d['anim_names']):
            for m, name in enumerate(G.MT_ORDER):
                for c, idx in enumerate(self.d['mt_tiles'][m]):
                    if idx == at[i]:
                        self.assertIn(c, self.d['metas'][name]['anim'])
        mt_of = dict((a, set()) for a in self.d['anim_names'])
        for name in G.MT_ORDER:
            for c, a in self.d['metas'][name]['anim'].items():
                mt_of[a].add(name)
        self.assertIn('MT_SEA', mt_of['SEA'])
        self.assertIn('MT_SEA_GLINT', mt_of['GLINT'])
        self.assertIn('MT_FIRE_LIT', mt_of['FIRE_L'])
        self.assertIn('MT_BEACON_LIT', mt_of['BEACON_L'])
        self.assertIn('MT_HEART', mt_of['HEART_L'])
        for a in self.d['anim_names']:
            frames = self.d['anims'][a]
            self.assertGreater(len(set(frames)), 1, 'anim %s does not move' % a)

    # ---- the colour-0 rule -------------------------------------------------------------------
    def mt_pixels(self, name, frame=0):
        m = G.MT_ORDER.index(name)
        px = []
        for c, idx in enumerate(self.d['mt_tiles'][m]):
            t = self.d['bg'][idx]
            for a, ai in self.d['anim_idx'].items():
                if ai == idx:
                    t = self.d['anims'][a][frame]
            px.extend(v for row in t for v in row)
        return px

    def test_colour0_is_light_only(self):
        for name in G.MT_ORDER:
            for f in range(4):
                px = self.mt_pixels(name, f)
                frac = px.count(0) / float(len(px))
                if name not in LIGHT_MTS:
                    self.assertLessEqual(frac, 0.10, '%s uses %.0f%% colour 0' % (name, frac * 100))
                else:
                    self.assertLessEqual(frac, 0.40, name)
        for name in ('MT_FIRE_LIT', 'MT_BEACON_LIT', 'MT_HEART'):
            for f in range(4):
                self.assertGreater(self.mt_pixels(name, f).count(0), 4, '%s frame %d has no light' % (name, f))
        glint = [self.mt_pixels('MT_SEA_GLINT', f).count(0) for f in range(4)]
        self.assertGreater(max(glint), 0)
        for name in ('MT_SEA', 'MT_GRASS', 'MT_SAND', 'MT_SNOW', 'MT_DUNE', 'MT_ASH', 'MT_ROAD',
                     'MT_RUIN_FLOOR', 'MT_UNDERGROWTH', 'MT_GRASS_TALL', 'MT_TREE', 'MT_ROCK',
                     'MT_FIRE_COLD', 'MT_BEACON', 'MT_SHRINE_EMPTY', 'MT_CAIRN'):
            for f in range(4):
                self.assertEqual(self.mt_pixels(name, f).count(0), 0, '%s must not emit light' % name)

    def test_ground_is_mostly_colour1(self):
        for name in ('MT_GRASS', 'MT_SAND', 'MT_SNOW', 'MT_ASH', 'MT_SHALLOW', 'MT_DUNE'):
            px = self.mt_pixels(name)
            self.assertGreater(px.count(1) / 256.0, 0.8, name)

    def test_ground_tiles_seamless_enough(self):
        """plain ground: no quarter may be completely empty of detail (grid look) and the
        four quarters are not all identical (visible 8-px repetition)"""
        for name in ('MT_GRASS', 'MT_SAND', 'MT_SNOW', 'MT_SEA', 'MT_SHALLOW'):
            m = G.MT_ORDER.index(name)
            self.assertGreater(len(set(self.d['mt_tiles'][m])), 1, name)

    # ---- palettes ----------------------------------------------------------------------------
    def test_dmg_palettes(self):
        bgp = c_array(self.c, 'dmg_bgp')
        self.assertEqual(len(bgp), 4)
        shade = lambda reg, i: (reg >> (2 * i)) & 3  # noqa: E731
        day = bgp[1]
        self.assertEqual(day, 0xE4)
        for reg in bgp:
            self.assertEqual(shade(reg, 0), 0, 'colour 0 must stay white (light) in every phase')
            self.assertLessEqual(shade(reg, 1), shade(reg, 2))
            self.assertLessEqual(shade(reg, 2), shade(reg, 3))
        night = bgp[3]
        self.assertGreaterEqual(shade(night, 1), 2, 'night ground must be dark')
        self.assertLess(shade(night, 1), 3, 'night must not be pure black: terrain stays faintly visible')
        self.assertGreaterEqual(sum(shade(night, i) for i in range(4)),
                                sum(shade(bgp[2], i) for i in range(4)), 'night darker than dusk')
        self.assertGreaterEqual(sum(shade(bgp[2], i) for i in range(4)),
                                sum(shade(bgp[0], i) for i in range(4)), 'dawn lighter than dusk')
        for arr in ('dmg_obp0', 'dmg_obp1'):
            obp = c_array(self.c, arr)
            self.assertEqual(len(obp), 4)
        o0 = c_array(self.c, 'dmg_obp0')
        # the player's rim (colour 2) must differ from the night ground shade
        self.assertNotEqual(shade(o0[3], 2), shade(night, 1))
        self.assertNotEqual(shade(o0[3], 3), shade(night, 1))

    def test_cgb_palettes(self):
        for arr, names in (('cgb_bg_pal', G.PAL_CLASSES), ('cgb_obj_pal', G.OBJ_SLOTS)):
            v = c_array(self.c, arr)
            self.assertEqual(len(v), 4 * 8 * 4)
            for x in v:
                self.assertLessEqual(x, 0x7FFF)
        tp = c_array(self.c, 'title_pal')
        self.assertEqual(len(tp), 32)
        for x in tp:
            self.assertLessEqual(x, 0x7FFF)
        # at night the light (colour 0) outshines everything else in every class
        for cls in G.PAL_CLASSES:
            p = self.d['bgpal'][('night', cls)]
            for i in (1, 2, 3):
                self.assertGreater(luma(p[0]), luma(p[i]) + 60, 'night %s colour %d vs light' % (cls, i))
        # day ground is lighter than day shadow
        for cls in G.PAL_CLASSES:
            p = self.d['bgpal'][('day', cls)]
            self.assertGreater(luma(p[1]), luma(p[3]))
        # player readable at night: rim clearly lighter than the night ground
        pl = self.d['objpal'][('night', 'PLAYER')]
        gr = self.d['bgpal'][('night', 'GRASS')]
        self.assertGreater(luma(pl[2]), luma(gr[1]) + 40)

    # ---- band ----------------------------------------------------------------------------
    def band(self, name, k=0):
        return self.d['bg'][self.define(name) + k]

    def test_band(self):
        for nm in ('BAND_SKY_TOP', 'BAND_SKY'):
            self.assertEqual(set(v for r in self.band(nm) for v in r), {1})
        for k in (0, 1):
            vals = [v for r in self.band('BAND_STAR0', k) for v in r]
            self.assertIn(0, vals)
            self.assertEqual(set(vals), {0, 1})
        self.assertEqual(set(v for r in self.band('BAND_LAND') for v in r), {3})
        for k in range(8):
            t = self.band('BAND_RIDGE0', k)
            for x in range(8):
                col = [t[y][x] for y in range(8)]
                self.assertEqual(sum(1 for v in col if v != 1), k + 1, 'ridge %d' % k)
            up = self.band('BAND_SLOPE_UP0', k)
            dn = self.band('BAND_SLOPE_DN0', k)
            hu = [sum(1 for y in range(8) if up[y][x] != 1) for x in range(8)]
            hd = [sum(1 for y in range(8) if dn[y][x] != 1) for x in range(8)]
            self.assertEqual(hu[0], k + 1)
            self.assertEqual(hu[7], 8)
            self.assertEqual(hd[0], 8)
            self.assertEqual(hd[7], k + 1)
            self.assertEqual(hu, sorted(hu))
            self.assertEqual(hd, sorted(hd, reverse=True))

    # ---- sprites -------------------------------------------------------------------------
    def test_sprites(self):
        idx = self.d['spr_idx']
        for n, (w, h) in G.SPR_REQUIRED.items():
            self.assertEqual(self.define(n), idx[n])
            self.assertEqual(idx[n] % 2, 0, '%s must start on an even (8x16 top) tile' % n)
            art = self.d['spr_art'][n]
            self.assertEqual((len(art[0]), len(art)), (w, h), n)
            self.assertTrue(any(v for r in art for v in r), '%s is empty' % n)
        self.assertEqual(idx['SPR_BIRD1'], idx['SPR_BIRD0'] + 2)
        # 8x16 order: the tile after a top tile is the one below it
        art = self.d['spr_art']['SPR_PL_DOWN0']
        t0, t1, t2 = (self.d['spr_tiles'][idx['SPR_PL_DOWN0'] + i] for i in range(3))
        self.assertEqual([list(r) for r in t0], [r[0:8] for r in art[0:8]])
        self.assertEqual([list(r) for r in t1], [r[0:8] for r in art[8:16]])
        self.assertEqual([list(r) for r in t2], [r[8:16] for r in art[0:8]])
        w = self.d['spr_art']['SPR_WATCHER']
        t4 = self.d['spr_tiles'][idx['SPR_WATCHER'] + 4]
        self.assertEqual([list(r) for r in t4], [r[0:8] for r in w[16:24]])
        # the glow quarter: its pixels thin out away from the centre (bottom-right corner)
        g = self.d['spr_art']['SPR_GLOW0']
        near = sum(1 for y in range(8, 16) for x in range(8, 16) if g[y][x])
        far = sum(1 for y in range(0, 8) for x in range(0, 8) if g[y][x])
        self.assertGreater(near, far)
        self.assertEqual(g[0][0], 0)

    # ---- title / map -----------------------------------------------------------------------
    def test_title(self):
        n = self.define('TITLE_TILE_COUNT')
        tm = c_array(self.c, 'title_map')
        ta = c_array(self.c, 'title_attr')
        self.assertEqual(len(tm), 360)
        self.assertEqual(len(ta), 360)
        self.assertTrue(all(t < n for t in tm))
        self.assertTrue(all(0 <= a <= 7 for a in ta))
        # reconstruct the canvas from tiles and compare
        pix = self.d['title_pix']
        for i, ti in enumerate(self.d['title_map']):
            tx, ty = (i % 20) * 8, (i // 20) * 8
            t = self.d['title_tiles'][ti]
            for y in range(8):
                self.assertEqual(list(t[y]), pix[ty + y][tx:tx + 8])

    def test_map_tiles(self):
        for i, n in enumerate(G.MAP_REQUIRED):
            self.assertLess(self.define(n), self.define('MAP_TILE_COUNT'))
        for k in range(4):
            t = self.d['map_tiles'][self.define('MAP_T_SHADE%d' % k)][1]
            self.assertEqual(set(v for r in t for v in r), {k})

    # ---- SDCC -----------------------------------------------------------------------------
    def test_compiles_with_sdcc(self):
        lcc = os.path.join(os.environ.get('GBDK_HOME', '/opt/gbdk'), 'bin', 'lcc')
        if not os.path.exists(lcc):
            self.skipTest('GBDK not installed')
        out = os.path.join(self.tmp, 'assets.o')
        r = subprocess.run([lcc, '-I' + os.path.join(ROOT, 'src', 'core'), '-I' + self.tmp, '-c',
                            '-o', out, os.path.join(self.tmp, 'assets.c')],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(r.returncode, 0, r.stdout.decode(errors='replace'))
        self.assertNotIn('warning', r.stdout.decode(errors='replace').lower())


if __name__ == '__main__':
    unittest.main()
