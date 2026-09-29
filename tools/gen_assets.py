#!/usr/bin/env python3
"""OPEN WORLD asset generator.

Parses the ASCII-art sources in ``assets/*.txt`` and writes ``src/gb/assets.c`` and
``src/gb/assets.h`` (GBDK-2020 / SDCC), exactly as specified by docs/CONTRACTS.md.

Stdlib only and deterministic (same input -> byte-identical output).

Source format
-------------
* Outside a block, a line starting with ``#`` is a comment.  A blank line ends a block
  (inside a block every line is a pixel row, so ``#`` pixels in column 0 are fine).
* ``@chars a=0 b=1 ...`` (a line of its own) sets the pixel alphabet for the rest of the file.
  Digits ``0-3`` always mean the raw colour index.  The convention used everywhere is
  *semantic*:

  ==========  ==================================  ===============================
  char        BG tiles (metatiles, band, title)    sprites
  ==========  ==================================  ===============================
  ``o``       colour 0 = LIGHT                     colour 1 = light
  ``.``       colour 1 = ground                    colour 0 = transparent
  ``+``       colour 2 = detail                    colour 2 = mid
  ``#``       colour 3 = deep shadow               colour 3 = dark
  ==========  ==================================  ===============================

* Block kinds (header line ``@kind NAME [key=value ...]`` followed by pixel rows):

  ``@meta MT_x pal=CLASS [pals=A,B,C,D] [anim=CORNER:NAME,...]``
      a 16x16 metatile (quarters TL, TR, BL, BR).  ``pal`` is its CGB palette class
      (GRASS, FOREST, WATER, SAND, SNOW, ROCK, LIGHT, SKY); ``pals`` overrides per quarter.
      ``anim=TL:SEA`` makes that quarter the animated tile ``SEA`` (its pixels in the block
      are ignored; the anim's frame 0 is used).
  ``@anim NAME``      one animated 8x8 tile: 8 rows x 32 columns = frames 0..3 side by side.
  ``@band NAME``      an 8x8 horizon-band tile (BAND_SKY_TOP, BAND_SKY, BAND_STAR0, BAND_STAR1,
                      BAND_LAND).  The ridge / slope tiles are generated (see band_tiles()).
  ``@sprite SPR_x``   8x16, 16x16 or 16x32 sprite art.  Tiles are emitted per 16-px row band,
                      per 8-px column, top tile then bottom tile (8x16 OBJ mode), so a 16x16
                      frame is L-top, L-bottom, R-top, R-bottom (right half = +2) and a 16x32
                      one is TL, TR (+2), BL (+4), BR (+6).
  ``@title``          the 160x144 title canvas.        ``@title_attr``  20x18 palette digits.
  ``@maptile MAP_T_x [MAP_T_y ...]``  map-screen tiles (8x8 each, row-major within the block).
  ``@bgpal PHASE CLASS c0 c1 c2 c3``   ``@objpal PHASE SLOT c0 c1 c2 c3``  (``#rrggbb``)
  ``@titlepal N c0 c1 c2 c3``          ``@dmg PHASE bgp=0x.. obp0=0x.. obp1=0x..``

The world BG tileset is deduplicated: identical 8x8 tiles are stored once (animated tiles are
always unique).  BG flips are never used (DMG has none), so DMG and CGB show the same pixels.
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools'))

MT_ORDER = [
    'MT_SEA', 'MT_SEA_GLINT', 'MT_SHALLOW', 'MT_STEPSTONE', 'MT_SAND', 'MT_GRASS',
    'MT_GRASS_TALL', 'MT_FLOWERS', 'MT_TREE', 'MT_UNDERGROWTH', 'MT_PINE', 'MT_SNOW',
    'MT_DUNE', 'MT_BONES', 'MT_ROCK', 'MT_ROCK_PEAK', 'MT_ASH', 'MT_GLASS', 'MT_MONOLITH',
    'MT_RUIN_WALL', 'MT_RUIN_FLOOR', 'MT_PILLAR', 'MT_STATUE_HAND', 'MT_ROAD', 'MT_BRAMBLE',
    'MT_FIRE_COLD', 'MT_FIRE_LIT', 'MT_CAIRN', 'MT_CAIRN_OLD', 'MT_BEACON', 'MT_BEACON_LIT',
    'MT_SHRINE', 'MT_SHRINE_EMPTY', 'MT_HEART', 'MT_TABLE', 'MT_WELL',
]

PAL_CLASSES = ['GRASS', 'FOREST', 'WATER', 'SAND', 'SNOW', 'ROCK', 'LIGHT', 'SKY']
OBJ_SLOTS = ['PLAYER', 'EMBER', 'UI', 'WEATHER', 'WATCHER', 'BAND', 'LIGHT', 'GLOW']
PHASES = ['dawn', 'day', 'dusk', 'night']

# Animated tiles that must exist (NAME, metatile that uses it).
ANIM_REQUIRED = ['SEA', 'GLINT', 'FIRE_L', 'FIRE_R', 'BEACON_L', 'BEACON_R', 'HEART_L', 'HEART_R']

BAND_REQUIRED = ['BAND_SKY_TOP', 'BAND_SKY', 'BAND_STAR0', 'BAND_STAR1', 'BAND_LAND']

# Sprite blocks: name -> (w, h).  Order in the file is the order in VRAM.
SPR_REQUIRED = {
    'SPR_PL_DOWN0': (16, 16), 'SPR_PL_DOWN1': (16, 16), 'SPR_PL_UP0': (16, 16),
    'SPR_PL_UP1': (16, 16), 'SPR_PL_SIDE0': (16, 16), 'SPR_PL_SIDE1': (16, 16),
    'SPR_PL_SIT': (16, 16), 'SPR_PL_SLEEP': (16, 16), 'SPR_PL_GLIDE': (16, 16),
    'SPR_PIP_FULL': (8, 16), 'SPR_PIP_EMPTY': (8, 16), 'SPR_ICON_LANTERN': (8, 16),
    'SPR_ICON_STONES': (8, 16), 'SPR_ICON_CLOAK': (8, 16), 'SPR_HINT_A': (8, 16),
    'SPR_BAND_BEACON': (8, 16), 'SPR_BAND_BEACON_LIT': (8, 16), 'SPR_BAND_HEART': (8, 16),
    'SPR_BAND_CAIRN': (8, 16), 'SPR_RAIN': (8, 16), 'SPR_SNOW': (8, 16),
    'SPR_GLOW0': (16, 16), 'SPR_WATCHER': (16, 32), 'SPR_BIRD0': (8, 16),
    'SPR_BIRD1': (8, 16), 'SPR_MAP_PLAYER': (8, 16), 'SPR_MAP_BEACON': (8, 16),
    'SPR_MAP_CAIRN': (8, 16), 'SPR_MAP_HEART': (8, 16),
}

MAP_REQUIRED = [
    'MAP_T_FOG', 'MAP_T_SHADE0', 'MAP_T_SHADE1', 'MAP_T_SHADE2', 'MAP_T_SHADE3',
    'MAP_T_TL', 'MAP_T_T', 'MAP_T_TR', 'MAP_T_L', 'MAP_T_R', 'MAP_T_BL', 'MAP_T_B', 'MAP_T_BR',
]

# Put all asset data in a switchable ROM bank (autobank). The engine must then
# SWITCH_ROM(BANK(assets)) before reading any asset array. Off = unbanked (the contract default).
BANKED = True

MAX_BG_TILES = 224
MAX_SPR_TILES = 128
MAX_TITLE_TILES = 200

CORNERS = ['TL', 'TR', 'BL', 'BR']


class AssetError(Exception):
    pass


# --------------------------------------------------------------------------------------------
# parsing
# --------------------------------------------------------------------------------------------

class Block(object):
    def __init__(self, kind, args, opts, where, alphabet):
        self.kind, self.args, self.opts, self.where = kind, args, opts, where
        self.alphabet = alphabet
        self.rows = []

    @property
    def w(self):
        return len(self.rows[0]) if self.rows else 0

    @property
    def h(self):
        return len(self.rows)

    def pixels(self):
        """rows of ints"""
        out = []
        for y, r in enumerate(self.rows):
            row = []
            for ch in r:
                if ch not in self.alphabet:
                    raise AssetError('%s: bad pixel char %r in row %d' % (self.where, ch, y))
                row.append(self.alphabet[ch])
            out.append(row)
        return out


RAW = {'0': 0, '1': 1, '2': 2, '3': 3}
NOPIX_KINDS = ('bgpal', 'objpal', 'titlepal', 'dmg', 'chars')


def parse_file(path):
    blocks = []
    cur = None
    alphabet = dict(RAW)
    with open(path, 'r') as f:
        lines = f.read().split('\n')
    for ln, raw in enumerate(lines, 1):
        line = raw.rstrip()
        where = '%s:%d' % (os.path.basename(path), ln)
        if cur is None and line.startswith('#'):
            continue            # comments live between blocks; inside a block '#' is a pixel
        if not line.strip():
            cur = None
            continue
        if line.startswith('@'):
            toks = line[1:].split()
            kind = toks[0]
            if kind == 'chars':
                alphabet = dict(RAW)
                for t in toks[1:]:
                    if len(t) != 3 or t[1] != '=' or t[2] not in '0123':
                        raise AssetError('%s: bad @chars entry %r' % (where, t))
                    alphabet[t[0]] = int(t[2])
                cur = None
                continue
            args, opts = [], {}
            for t in toks[1:]:
                if '=' in t and not t.startswith('#'):
                    k, v = t.split('=', 1)
                    opts[k] = v
                else:
                    args.append(t)
            cur = Block(kind, args, opts, where, dict(alphabet))
            blocks.append(cur)
            if kind in NOPIX_KINDS:
                cur = None
            continue
        if cur is None:
            raise AssetError('%s: pixel row outside of a block' % where)
        row = line.strip()
        if cur.rows and len(row) != len(cur.rows[0]):
            raise AssetError('%s: row width %d != %d' % (where, len(row), len(cur.rows[0])))
        cur.rows.append(row)
    return blocks


def split8(pix, where, order='rows'):
    h, w = len(pix), len(pix[0]) if pix else 0
    if not h or w % 8 or h % 8:
        raise AssetError('%s: block size %dx%d not a multiple of 8' % (where, w, h))
    tiles = []

    def tile(tx, ty):
        return tuple(tuple(pix[ty * 8 + y][tx * 8:tx * 8 + 8]) for y in range(8))
    if order == 'rows':
        for ty in range(h // 8):
            for tx in range(w // 8):
                tiles.append(tile(tx, ty))
    elif order == 'obj16':      # 8x16 OBJ: per 16-px band, per 8-px column, top then bottom
        if h % 16:
            raise AssetError('%s: sprite height %d not a multiple of 16' % (where, h))
        for by in range(h // 16):
            for tx in range(w // 8):
                tiles.append(tile(tx, by * 2))
                tiles.append(tile(tx, by * 2 + 1))
    return tiles


def encode_tile(tile):
    out = []
    for row in tile:
        lo = hi = 0
        for x, v in enumerate(row):
            lo |= (v & 1) << (7 - x)
            hi |= ((v >> 1) & 1) << (7 - x)
        out.append(lo)
        out.append(hi)
    return out


def parse_color(s, where):
    s = s.lstrip('#')
    if len(s) != 6:
        raise AssetError('%s: bad colour %r' % (where, s))
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))


def rgb555(c):
    r, g, b = c
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)


def solid(v):
    return tuple(tuple([v] * 8) for _ in range(8))


# --------------------------------------------------------------------------------------------
# generated band tiles: ridges and slopes.  Land = colour 3, its top edge (the skyline) is
# drawn in colour 2 so that at night (sky and land both crushed dark) a faint rim of the
# far mountains remains, like moonlight on a ridge.  Sky = colour 1.
# --------------------------------------------------------------------------------------------

def land_column_tile(heights):
    """heights[x] = number of land pixel rows at the bottom of column x (0..8)."""
    t = []
    for y in range(8):
        row = []
        for x in range(8):
            h = heights[x]
            top = 8 - h
            if y < top:
                row.append(1)
            else:
                # rim: the topmost land pixel of the column, and any pixel exposed on the side
                # of a steeper neighbour
                exposed = (y == top)
                for nx in (x - 1, x + 1):
                    if 0 <= nx < 8 and y < 8 - heights[nx]:
                        exposed = True
                row.append(2 if exposed else 3)
        t.append(tuple(row))
    return tuple(t)


def band_generated():
    out = []
    for k in range(8):
        out.append(('BAND_RIDGE%d' % k, land_column_tile([k + 1] * 8)))
    for k in range(8):   # rise: left edge at height k, climbing 1 px per px (clamped at 8)
        out.append(('BAND_SLOPE_UP%d' % k, land_column_tile([min(8, k + 1 + x) for x in range(8)])))
    for k in range(8):   # fall: right edge at height k
        out.append(('BAND_SLOPE_DN%d' % k, land_column_tile([min(8, k + 1 + (7 - x)) for x in range(8)])))
    return out


# --------------------------------------------------------------------------------------------
# loading
# --------------------------------------------------------------------------------------------

def placeholder_meta(i):
    """crude fallback art (only with strict=False)"""
    p = [[1] * 16 for _ in range(16)]
    for b in range(6):
        if i & (1 << b):
            for y in range(2, 4):
                p[y][2 + b * 2] = 3
    return p


def load_assets(assets_dir, strict=True):
    blocks = []
    for name in sorted(os.listdir(assets_dir)):
        if name.endswith('.txt'):
            blocks.extend(parse_file(os.path.join(assets_dir, name)))

    metas, anims, band, sprites, maptiles = {}, {}, {}, [], []
    title = title_attr = None
    bgpal, objpal, titlepal, dmg = {}, {}, {}, {}

    for b in blocks:
        k = b.kind
        if k == 'meta':
            n = b.args[0]
            if n not in MT_ORDER:
                raise AssetError('%s: unknown metatile %s' % (b.where, n))
            if n in metas:
                raise AssetError('%s: duplicate %s' % (b.where, n))
            pix = b.pixels()
            if len(pix) != 16 or len(pix[0]) != 16:
                raise AssetError('%s: metatile must be 16x16' % b.where)
            pal = b.opts.get('pal')
            pals = b.opts.get('pals')
            pals = pals.split(',') if pals else [pal] * 4
            if len(pals) != 4 or any(p not in PAL_CLASSES for p in pals):
                raise AssetError('%s: bad pal=/pals= for %s' % (b.where, n))
            anim = {}
            if 'anim' in b.opts:
                for ent in b.opts['anim'].split(','):
                    c, a = ent.split(':')
                    if c not in CORNERS:
                        raise AssetError('%s: bad anim corner %s' % (b.where, c))
                    anim[CORNERS.index(c)] = a
            metas[n] = {'pix': pix, 'tiles': split8(pix, b.where), 'pals': pals, 'anim': anim,
                        'where': b.where}
        elif k == 'anim':
            n = b.args[0]
            pix = b.pixels()
            if len(pix) != 8 or len(pix[0]) != 32:
                raise AssetError('%s: @anim must be 32x8 (4 frames)' % b.where)
            anims[n] = split8(pix, b.where)
        elif k == 'band':
            t = split8(b.pixels(), b.where)
            if len(t) != 1:
                raise AssetError('%s: band tile must be 8x8' % b.where)
            band[b.args[0]] = t[0]
        elif k == 'sprite':
            n = b.args[0]
            pix = b.pixels()
            sprites.append((n, len(pix[0]), len(pix), split8(pix, b.where, 'obj16'), pix, b.where))
        elif k == 'maptile':
            ts = split8(b.pixels(), b.where)
            if len(ts) != len(b.args):
                raise AssetError('%s: %d names for %d map tiles' % (b.where, len(b.args), len(ts)))
            maptiles.extend(zip(b.args, ts))
        elif k == 'title':
            title = b
        elif k == 'title_attr':
            title_attr = b
        elif k in ('bgpal', 'objpal'):
            if len(b.args) != 6:
                raise AssetError('%s: @%s PHASE NAME c0 c1 c2 c3' % (b.where, k))
            ph, nm = b.args[0], b.args[1]
            names = PAL_CLASSES if k == 'bgpal' else OBJ_SLOTS
            if ph not in PHASES or nm not in names:
                raise AssetError('%s: bad phase/name %s %s' % (b.where, ph, nm))
            d = bgpal if k == 'bgpal' else objpal
            if (ph, nm) in d:
                raise AssetError('%s: duplicate palette %s %s' % (b.where, ph, nm))
            d[(ph, nm)] = [parse_color(c, b.where) for c in b.args[2:]]
        elif k == 'titlepal':
            n = int(b.args[0])
            titlepal[n] = [parse_color(c, b.where) for c in b.args[1:5]]
        elif k == 'dmg':
            ph = b.args[0]
            dmg[ph] = dict((kk, int(v, 0)) for kk, v in b.opts.items())
        else:
            raise AssetError('%s: unknown block @%s' % (b.where, k))

    def need(cond, msg):
        if not cond:
            if strict:
                raise AssetError(msg)
            return False
        return True

    # ---- anims ------------------------------------------------------------------------------
    for a in ANIM_REQUIRED:
        if not need(a in anims, 'missing @anim %s' % a):
            anims[a] = [solid(1)] * 4
    anim_names = [a for a in ANIM_REQUIRED] + sorted(a for a in anims if a not in ANIM_REQUIRED)

    # ---- world BG tileset ------------------------------------------------------------------
    bg = []            # list of tiles
    bg_label = []
    index = {}         # static tile -> index

    def add_static(t, label):
        if t in index:
            return index[t]
        index[t] = len(bg)
        bg.append(t)
        bg_label.append(label)
        return index[t]

    # tile 0: plain sky / plain colour-1 (safe default for a cleared map)
    for bn in BAND_REQUIRED:
        if not need(bn in band, 'missing @band %s' % bn):
            band[bn] = solid(1) if bn != 'BAND_LAND' else solid(3)
    band_idx = {}
    band_idx['BAND_SKY'] = add_static(band['BAND_SKY'], 'BAND_SKY')
    band_idx['BAND_SKY_TOP'] = add_static(band['BAND_SKY_TOP'], 'BAND_SKY_TOP')

    anim_idx = {}
    for a in anim_names:
        anim_idx[a] = len(bg)
        bg.append(anims[a][0])
        bg_label.append('anim %s' % a)

    mt_tiles, mt_attr = [], []
    for i, n in enumerate(MT_ORDER):
        if not need(n in metas, 'missing metatile %s' % n):
            pix = placeholder_meta(i)
            metas[n] = {'pix': pix, 'tiles': split8(pix, n), 'pals': ['GRASS'] * 4, 'anim': {},
                        'where': n}
        m = metas[n]
        idxs = []
        for c in range(4):
            if c in m['anim']:
                a = m['anim'][c]
                if a not in anim_idx:
                    raise AssetError('%s: unknown anim %s' % (m['where'], a))
                idxs.append(anim_idx[a])
                m['tiles'][c] = anims[a][0]
            else:
                idxs.append(add_static(m['tiles'][c], '%s %s' % (n, CORNERS[c])))
        mt_tiles.append(idxs)
        mt_attr.append([PAL_CLASSES.index(p) for p in m['pals']])

    # the star tiles must be adjacent (BAND_STAR0, BAND_STAR0+1): never dedup them
    band_idx['BAND_STAR0'] = len(bg)
    bg.append(band['BAND_STAR0'])
    bg_label.append('BAND_STAR0')
    bg.append(band['BAND_STAR1'])
    bg_label.append('BAND_STAR1')
    band_idx['BAND_LAND'] = add_static(band['BAND_LAND'], 'BAND_LAND')
    gen = band_generated()
    # ridge / slope groups are indexed as BASE + k: keep each group contiguous
    for g in ('BAND_RIDGE', 'BAND_SLOPE_UP', 'BAND_SLOPE_DN'):
        band_idx[g + '0'] = len(bg)
        for nm, t in gen:
            if nm.startswith(g) and nm[len(g):].isdigit():
                bg.append(t)
                bg_label.append(nm)
    band_tiles = dict(band)
    band_tiles.update(dict(gen))

    if len(bg) > MAX_BG_TILES:
        raise AssetError('world BG tileset has %d tiles (> %d)' % (len(bg), MAX_BG_TILES))

    # ---- sprites ------------------------------------------------------------------------------
    spr_tiles, spr_idx, spr_label, spr_art = [], {}, [], {}
    seen = set()
    for n, w, h, ts, pix, where in sprites:
        if n in seen:
            raise AssetError('%s: duplicate sprite %s' % (where, n))
        seen.add(n)
        if n in SPR_REQUIRED and SPR_REQUIRED[n] != (w, h):
            raise AssetError('%s: %s must be %dx%d, is %dx%d' % ((where, n) + SPR_REQUIRED[n] + (w, h)))
        spr_idx[n] = len(spr_tiles)
        spr_art[n] = pix
        for j, t in enumerate(ts):
            spr_tiles.append(t)
            spr_label.append(n if j == 0 else '')
    for n, (w, h) in SPR_REQUIRED.items():
        if not need(n in spr_idx, 'missing sprite %s' % n):
            spr_idx[n] = len(spr_tiles)
            spr_art[n] = [[3 if (x in (0, w - 1) or y in (0, h - 1)) else 0 for x in range(w)]
                          for y in range(h)]
            ts = split8(spr_art[n], n, 'obj16')
            spr_tiles.extend(ts)
            spr_label.extend([n] + [''] * (len(ts) - 1))
    if spr_idx['SPR_BIRD1'] != spr_idx['SPR_BIRD0'] + 2:
        raise AssetError('SPR_BIRD1 must directly follow SPR_BIRD0')
    if len(spr_tiles) > MAX_SPR_TILES:
        raise AssetError('%d sprite tiles (> %d)' % (len(spr_tiles), MAX_SPR_TILES))

    # ---- title -----------------------------------------------------------------------------
    if title is not None:
        tpix = title.pixels()
        if len(tpix) != 144 or len(tpix[0]) != 160:
            raise AssetError('%s: @title must be 160x144' % title.where)
    else:
        need(False, 'missing @title')
        tpix = [[1] * 160 for _ in range(144)]
    cells = split8(tpix, 'title')
    t_tiles, t_seen, t_map = [], {}, []
    for t in cells:
        if t not in t_seen:
            t_seen[t] = len(t_tiles)
            t_tiles.append(t)
        t_map.append(t_seen[t])
    if len(t_tiles) > MAX_TITLE_TILES:
        raise AssetError('title uses %d unique tiles (> %d)' % (len(t_tiles), MAX_TITLE_TILES))
    if title_attr is not None:
        if title_attr.h != 18 or title_attr.w != 20:
            raise AssetError('%s: @title_attr must be 20x18' % title_attr.where)
        t_attr = []
        for r in title_attr.rows:
            for ch in r:
                if ch not in '01234567':
                    raise AssetError('%s: bad title attr %r' % (title_attr.where, ch))
                t_attr.append(int(ch))
    else:
        need(False, 'missing @title_attr')
        t_attr = [0] * 360
    for n in range(8):
        if not need(n in titlepal, 'missing @titlepal %d' % n):
            titlepal[n] = [(232, 232, 224), (160, 160, 160), (80, 80, 88), (16, 16, 24)]

    # ---- map tiles -------------------------------------------------------------------------
    names = [n for n, _ in maptiles]
    for n in MAP_REQUIRED:
        if not need(n in names, 'missing map tile %s' % n):
            maptiles.append((n, solid(1)))
    if len(set(n for n, _ in maptiles)) != len(maptiles):
        raise AssetError('duplicate map tile name')

    # ---- palettes --------------------------------------------------------------------------
    for ph in PHASES:
        for c in PAL_CLASSES:
            if not need((ph, c) in bgpal, 'missing @bgpal %s %s' % (ph, c)):
                bgpal[(ph, c)] = [(224, 224, 208), (160, 168, 152), (88, 96, 96), (24, 24, 32)]
        for s in OBJ_SLOTS:
            if not need((ph, s) in objpal, 'missing @objpal %s %s' % (ph, s)):
                objpal[(ph, s)] = [(0, 0, 0), (232, 232, 224), (136, 136, 136), (24, 24, 32)]
        if not need(ph in dmg and all(k in dmg[ph] for k in ('bgp', 'obp0', 'obp1')),
                    'missing @dmg %s bgp= obp0= obp1=' % ph):
            dmg[ph] = {'bgp': 0xE4, 'obp0': 0xE0, 'obp1': 0x90}

    return {
        'bg': bg, 'bg_label': bg_label, 'mt_tiles': mt_tiles, 'mt_attr': mt_attr,
        'metas': metas, 'anim_names': anim_names, 'anims': anims, 'anim_idx': anim_idx,
        'band_idx': band_idx, 'band_tiles': band_tiles,
        'spr_tiles': spr_tiles, 'spr_idx': spr_idx, 'spr_label': spr_label, 'spr_art': spr_art,
        'title_pix': tpix, 'title_tiles': t_tiles, 'title_map': t_map, 'title_attr': t_attr,
        'title_pal': [titlepal[n] for n in range(8)],
        'map_tiles': maptiles,
        'bgpal': bgpal, 'objpal': objpal, 'dmg': dmg,
    }


# --------------------------------------------------------------------------------------------
# C output
# --------------------------------------------------------------------------------------------

def c_bytes(data, indent='    ', per_line=16):
    return '\n'.join(indent + ','.join('0x%02X' % b for b in data[i:i + per_line]) + ','
                     for i in range(0, len(data), per_line))


def tile_array(name, size, tiles, labels=None):
    s = ['const uint8_t %s[%s] = {' % (name, size)]
    for i, t in enumerate(tiles):
        lab = labels[i] if labels else ''
        s.append(c_bytes(encode_tile(t)) + ('  /* %d %s */' % (i, lab) if lab else '  /* %d */' % i))
    s.append('};')
    return '\n'.join(s)


HEADER_TOP = '''/* assets.h - OPEN WORLD art: AUTO-GENERATED by tools/gen_assets.py from the assets/ directory.
 * DO NOT EDIT: edit the ASCII sources and run `make assets` (python3 tools/gen_assets.py).
 * Contract: docs/CONTRACTS.md ("Assets").
 *
 * ART RULE: BG colour index 0 is LIGHT (fire, beacon light, stars, glints, glow). Ground is
 * colour 1, detail 2, deep shadow 3. The night palette crushes 1-3 and keeps 0 bright.
 * Sprite colours: 0 transparent, 1 light, 2 mid, 3 dark (for every sprite).
 */
#ifndef OPEN_WORLD_ASSETS_H
#define OPEN_WORLD_ASSETS_H

#include <stdint.h>
#include "world.h"            /* MT_COUNT */
'''


def gen(d, banked=None):
    banked = BANKED if banked is None else banked
    H = [HEADER_TOP]
    A = H.append
    if banked:
        A('/* BANKED: every array in this file lives in the switchable ROM bank BANK(assets)')
        A('   (autobanked). The engine must SWITCH_ROM(BANK(assets)) (and restore its own bank)')
        A('   before reading any of them, or copy what it needs to WRAM first. The #defines are')
        A('   plain constants and need no banking. */')
        A('#ifdef __SDCC')
        A('#include <gb/gb.h>')
        A('BANKREF_EXTERN(assets)')
        A('#endif')
        A('#define ASSETS_BANKED 1')
        A('')
    A('/* ---- world BG tileset (load with set_bkg_data(0, BG_TILE_COUNT, bg_tiles)) ---- */')
    A('#define BG_TILE_COUNT  %d    /* <= 224 */' % len(d['bg']))
    A('extern const uint8_t bg_tiles[];              /* BG_TILE_COUNT * 16 bytes, 2bpp */')
    A('extern const uint8_t mt_tiles[MT_COUNT][4];   /* bg tile index TL, TR, BL, BR */')
    A('extern const uint8_t mt_attr[MT_COUNT][4];    /* CGB attribute: palette class (PAL_*), no flips */')
    A('')
    A('/* Animated BG tiles: every ANIM_PERIOD frames copy anim_frames[i][f] into bg tile')
    A('   anim_tile[i], f = 0..ANIM_FRAMES-1 cycling. Entries: %s. */' %
      ', '.join('%d=%s' % (i, a) for i, a in enumerate(d['anim_names'])))
    A('#define ANIM_COUNT   %d' % len(d['anim_names']))
    A('#define ANIM_FRAMES  4')
    A('#define ANIM_PERIOD  16')
    for i, a in enumerate(d['anim_names']):
        A('#define ANIM_%-8s %d   /* bg tile %d */' % (a, i, d['anim_idx'][a]))
    A('extern const uint8_t anim_tile[ANIM_COUNT];')
    A('extern const uint8_t anim_frames[ANIM_COUNT][ANIM_FRAMES][16];')
    A('')
    bi = d['band_idx']
    A('/* Horizon band (map 0x9C00, 3 rows x 32 columns). Sky = colour 1 (tinted by PAL_SKY),')
    A('   stars = colour 0, far land = colour 3 with a colour-2 skyline rim so ridges stay')
    A('   faintly visible when night crushes sky and land together. */')
    A('#define BAND_SKY_TOP     %d  /* plain sky, row 0 */' % bi['BAND_SKY_TOP'])
    A('#define BAND_SKY         %d  /* plain sky, rows 1-2 (also bg tile 0) */' % bi['BAND_SKY'])
    A('#define BAND_STAR0       %d  /* sky + star (colour 0); variant BAND_STAR0+1 */' % bi['BAND_STAR0'])
    A('#define BAND_LAND        %d  /* solid distant land */' % bi['BAND_LAND'])
    A('#define BAND_RIDGE0      %d  /* +k (k=0..7): land fills the bottom k+1 rows, flat top */' % bi['BAND_RIDGE0'])
    A('#define BAND_SLOPE_UP0   %d  /* +k: rises left->right 1 px/px from height k+1 at the left edge */' % bi['BAND_SLOPE_UP0'])
    A('#define BAND_SLOPE_DN0   %d  /* +k: falls left->right to height k+1 at the right edge */' % bi['BAND_SLOPE_DN0'])
    A('')
    A('/* ---- sprites: 8x16 OBJ tiles (LCDC.2 = 1), load at 0x8000 ---- */')
    A('#define SPR_TILE_COUNT %d    /* <= 128 */' % len(d['spr_tiles']))
    A('extern const uint8_t spr_tiles[];')
    A('/* Player frames are 16x16 = two 8x16 sprites: constant = LEFT half, right half = +2.')
    A('   Side frames face RIGHT (X-flip and swap halves for left). */')
    for n in ['SPR_PL_DOWN0', 'SPR_PL_DOWN1', 'SPR_PL_UP0', 'SPR_PL_UP1', 'SPR_PL_SIDE0',
              'SPR_PL_SIDE1', 'SPR_PL_SIT', 'SPR_PL_SLEEP', 'SPR_PL_GLIDE']:
        A('#define %-20s %d' % (n, d['spr_idx'][n]))
    A('/* single 8x16 sprites (art in the top 8 px for HUD items, which sit on lines 0-8) */')
    for n in ['SPR_PIP_FULL', 'SPR_PIP_EMPTY', 'SPR_ICON_LANTERN', 'SPR_ICON_STONES',
              'SPR_ICON_CLOAK', 'SPR_HINT_A', 'SPR_BAND_BEACON', 'SPR_BAND_BEACON_LIT',
              'SPR_BAND_HEART', 'SPR_BAND_CAIRN', 'SPR_RAIN', 'SPR_SNOW']:
        A('#define %-20s %d' % (n, d['spr_idx'][n]))
    A('''/* Lantern glow: SPR_GLOW0 is the TOP-LEFT QUARTER (16x16) of a 32x32 dithered disc of light
   centred on the player: SPR_GLOW0 = its left 8x16 half, SPR_GLOW0+2 = its right half. Its
   bottom-right corner is the disc centre. Place 8 sprites (player centre = cx, cy):
       TL: (cx-16, cy-16) GLOW0      (cx-8, cy-16) GLOW0+2          no flip
       TR: (cx,    cy-16) GLOW0+2    (cx+8, cy-16) GLOW0            X flip
       BL: (cx-16, cy)    GLOW0      (cx-8, cy)    GLOW0+2          Y flip
       BR: (cx,    cy)    GLOW0+2    (cx+8, cy)    GLOW0            X+Y flip
   Pixels: colour 1 = brightest (around the feet), 2 = the glow, 3 = its faint outer edge (on DMG
   night ground it vanishes, so the ring fades out). Draw with OPAL_GLOW / DMG OBP1, behind the
   player (higher OAM index than the player). */''')
    A('#define %-20s %d' % ('SPR_GLOW0', d['spr_idx']['SPR_GLOW0']))
    A('#define %-20s %d  /* 16x32: TL, TR +2, BL +4, BR +6 */' % ('SPR_WATCHER', d['spr_idx']['SPR_WATCHER']))
    A('#define %-20s %d  /* 2 frames: SPR_BIRD0, SPR_BIRD0+2 (= SPR_BIRD1) */' % ('SPR_BIRD0', d['spr_idx']['SPR_BIRD0']))
    A('#define %-20s %d' % ('SPR_BIRD1', d['spr_idx']['SPR_BIRD1']))
    A('/* map-screen markers (8x16; art centred in the top 8x8, pivot = pixel (3,3)) */')
    for n in ['SPR_MAP_PLAYER', 'SPR_MAP_BEACON', 'SPR_MAP_CAIRN', 'SPR_MAP_HEART']:
        A('#define %-20s %d' % (n, d['spr_idx'][n]))
    extra = [n for n in d['spr_idx'] if n not in SPR_REQUIRED]
    for n in extra:
        A('#define %-20s %d' % (n, d['spr_idx'][n]))
    A('')
    dm = d['dmg']
    A('/* ---- palettes ---- */')
    def mapping(reg):
        return ' '.join('%d->%d' % (i, (reg >> (2 * i)) & 3) for i in range(4))
    A('/* DMG, indexed by PH_DAWN, PH_DAY, PH_DUSK, PH_NIGHT (sound.h). Colour index -> shade')
    A('   (0 white .. 3 black); colour 0 (LIGHT) stays white in every phase:')
    notes = {'dawn': 'haze: nothing black yet, the land lifted and soft',
             'day': 'identity',
             'dusk': 'detail hardens into black silhouette, the sea goes dark',
             'night': 'ground one step above black (legible), forms black, only light shines'}
    for nm in ('bgp', 'obp0', 'obp1'):
        for ph in PHASES:
            A('     %-4s %-5s 0x%02X  %s%s' % (nm.upper(), ph, dm[ph][nm], mapping(dm[ph][nm]),
                                            ('   ' + notes[ph]) if nm == 'bgp' else ''))
    A('   OBP0 = figures (player, band beacon/cairn, icons, map markers); OBP1 = lights')
    A('   (warmth pips, lantern glow, rain/snow, Watchers, lit beacon, heart). */')
    A('extern const uint8_t dmg_bgp[4];')
    A('extern const uint8_t dmg_obp0[4], dmg_obp1[4];')
    A('/* CGB: 8 BG palette classes x 4 colours (RGB555) per phase. Colour 0 is always the light. */')
    A('enum { PAL_GRASS, PAL_FOREST, PAL_WATER, PAL_SAND, PAL_SNOW, PAL_ROCK, PAL_LIGHT, PAL_SKY };')
    A('/* CGB OBJ palette slots (cgb_obj_pal[phase][slot]); DMG palette to use in brackets:')
    A('   OPAL_PLAYER [OBP0] wanderer   OPAL_EMBER [OBP1] warmth pips   OPAL_UI [OBP0] item icons,')
    A('   hint, map markers   OPAL_WEATHER [OBP1] rain, snow, birds [OBP0]   OPAL_WATCHER [OBP1]')
    A('   OPAL_BAND [OBP0] unlit beacon, cairn on the band   OPAL_LIGHT [OBP1] lit beacon, heart')
    A('   OPAL_GLOW [OBP1] lantern glow */')
    A('enum { %s };' % ', '.join('OPAL_' + s for s in OBJ_SLOTS))
    A('#define DMG_OBP_PLAYER  0   /* OBP0 */')
    A('#define DMG_OBP_LIGHT   1   /* OBP1 */')
    A('extern const uint16_t cgb_bg_pal[4][8][4];')
    A('extern const uint16_t cgb_obj_pal[4][8][4];')
    A('')
    A('/* ---- title screen: load title_tiles at bg tile 0; title_map holds tile indices ---- */')
    A('#define TITLE_TILE_COUNT %d' % len(d['title_tiles']))
    A('extern const uint8_t title_tiles[];')
    A('extern const uint8_t title_map[20 * 18];')
    A('extern const uint8_t title_attr[20 * 18];   /* CGB palette 0-7 per cell */')
    A('extern const uint16_t title_pal[8][4];')
    A('')
    A('/* ---- map screen tiles (load at bg tile 0; MAP_T_* are indices into map_tiles) ---- */')
    A('#define MAP_TILE_COUNT %d' % len(d['map_tiles']))
    for i, (n, _) in enumerate(d['map_tiles']):
        A('#define %-16s %d' % (n, i))
    A('extern const uint8_t map_tiles[];')
    A('')
    A('#endif')
    A('')

    C = []
    B = C.append
    B('/* assets.c - AUTO-GENERATED by tools/gen_assets.py from the assets/ directory. DO NOT EDIT. */')
    if banked:
        B('#ifdef __SDCC')
        B('#pragma bank 255')
        B('#endif')
    B('#include "assets.h"')
    if banked:
        B('#ifdef __SDCC')
        B('BANKREF(assets)')
        B('#endif')
    B('')
    B(tile_array('bg_tiles', 'BG_TILE_COUNT * 16', d['bg'], d['bg_label']))
    B('')
    B('const uint8_t mt_tiles[MT_COUNT][4] = {')
    for n, t in zip(MT_ORDER, d['mt_tiles']):
        B('    {%3d,%3d,%3d,%3d}, /* %s */' % (tuple(t) + (n,)))
    B('};')
    B('')
    B('const uint8_t mt_attr[MT_COUNT][4] = {')
    for n, t in zip(MT_ORDER, d['mt_attr']):
        B('    {%d,%d,%d,%d}, /* %s */' % (tuple(t) + (n,)))
    B('};')
    B('')
    B('const uint8_t anim_tile[ANIM_COUNT] = { %s };' %
      ', '.join(str(d['anim_idx'][a]) for a in d['anim_names']))
    B('')
    B('const uint8_t anim_frames[ANIM_COUNT][ANIM_FRAMES][16] = {')
    for a in d['anim_names']:
        B('    { /* %s */' % a)
        for f in d['anims'][a]:
            B('        {' + ','.join('0x%02X' % v for v in encode_tile(f)) + '},')
        B('    },')
    B('};')
    B('')
    B(tile_array('spr_tiles', 'SPR_TILE_COUNT * 16', d['spr_tiles'], d['spr_label']))
    B('')
    for nm in ('bgp', 'obp0', 'obp1'):
        B('const uint8_t dmg_%s[4] = { %s };' % (nm, ', '.join('0x%02X' % d['dmg'][ph][nm] for ph in PHASES)))
    B('')
    for arr, pal, names in (('cgb_bg_pal', d['bgpal'], PAL_CLASSES), ('cgb_obj_pal', d['objpal'], OBJ_SLOTS)):
        B('const uint16_t %s[4][8][4] = {' % arr)
        for ph in PHASES:
            B('    { /* %s */' % ph)
            for n in names:
                B('        {' + ','.join('0x%04X' % rgb555(c) for c in pal[(ph, n)]) + '}, /* %s */' % n)
            B('    },')
        B('};')
        B('')
    B(tile_array('title_tiles', 'TITLE_TILE_COUNT * 16', d['title_tiles']))
    B('')
    for nm, arr in (('title_map', d['title_map']), ('title_attr', d['title_attr'])):
        B('const uint8_t %s[20 * 18] = {' % nm)
        for r in range(18):
            B('    ' + ','.join('%3d' % v for v in arr[r * 20:(r + 1) * 20]) + ',')
        B('};')
        B('')
    B('const uint16_t title_pal[8][4] = {')
    for p in d['title_pal']:
        B('    {' + ','.join('0x%04X' % rgb555(c) for c in p) + '},')
    B('};')
    B('')
    B(tile_array('map_tiles', 'MAP_TILE_COUNT * 16', [t for _, t in d['map_tiles']],
                 [n for n, _ in d['map_tiles']]))
    B('')
    return '\n'.join(H), '\n'.join(C)


def write_outputs(data, out_dir):
    h, c = gen(data)
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    for fn, txt in (('assets.h', h), ('assets.c', c)):
        p = os.path.join(out_dir, fn)
        old = None
        if os.path.exists(p):
            with open(p) as f:
                old = f.read()
        if old != txt:
            with open(p, 'w') as f:
                f.write(txt)


def main(argv=None):
    ap = argparse.ArgumentParser(description='OPEN WORLD asset generator')
    ap.add_argument('--assets', default=os.path.join(ROOT, 'assets'))
    ap.add_argument('--out-dir', default=os.path.join(ROOT, 'src', 'gb'))
    ap.add_argument('--allow-missing', action='store_true', help='fill missing art with placeholders')
    args = ap.parse_args(argv)
    try:
        data = load_assets(args.assets, strict=not args.allow_missing)
    except AssetError as e:
        sys.stderr.write('gen_assets: error: %s\n' % e)
        return 1
    write_outputs(data, args.out_dir)
    print('gen_assets: %d bg tiles, %d anims, %d sprite tiles, title %d tiles, map %d tiles -> %s' %
          (len(data['bg']), len(data['anim_names']), len(data['spr_tiles']),
           len(data['title_tiles']), len(data['map_tiles']), args.out_dir))
    return 0


if __name__ == '__main__':
    sys.exit(main())
