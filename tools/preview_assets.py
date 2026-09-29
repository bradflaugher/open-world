#!/usr/bin/env python3
"""Render PNG previews of the OPEN WORLD art into build/preview/.

  metatiles_<pal>.png      every metatile (1x and 4x), DMG and each CGB phase
  biomes_<pal>.png         a small tiled landscape per biome
  band_<pal>.png           the 256-px horizon panorama with markers
  sprites_<pal>.png        every sprite (4x)
  title_dmg.png / title_cgb.png
  mock_<pal>.png           160x144 mock screenshot: band + land + player (+ glow at night)
  mock_sheet.png           all mock screenshots side by side (2x)

<pal> is dmg_<phase> or cgb_<phase> for phase in dawn/day/dusk/night. Every image is written
at 1x (true Game Boy pixels) and, as *_x3.png, scaled 3x for viewing.

Requires Pillow. Usage: python3 tools/preview_assets.py [--out build/preview]
"""

import argparse
import math
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_assets as G  # noqa: E402

DMG_SHADES = [(224, 228, 214), (152, 158, 146), (78, 84, 82), (20, 22, 24)]
SHEET_BG = (40, 40, 44)
PHASES = G.PHASES


def c555(c):
    v = G.rgb555(c)
    r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
    return tuple((x << 3) | (x >> 2) for x in (r, g, b))


def dmg_map(reg):
    return [DMG_SHADES[(reg >> (2 * i)) & 3] for i in range(4)]


class Pal(object):
    """Colour lookup for one rendering mode."""

    def __init__(self, data, mode, phase):
        self.mode, self.phase = mode, phase
        dm = data['dmg'][phase]
        if mode == 'dmg':
            bg = dmg_map(dm['bgp'])
            self.bg = [bg] * 8
            o0, o1 = dmg_map(dm['obp0']), dmg_map(dm['obp1'])
            # OBJ slot -> DMG OBP (see assets.h)
            use1 = {'EMBER', 'WEATHER', 'WATCHER', 'LIGHT', 'GLOW'}
            self.obj = [o1 if s in use1 else o0 for s in G.OBJ_SLOTS]
        else:
            self.bg = [[c555(c) for c in data['bgpal'][(phase, n)]] for n in G.PAL_CLASSES]
            self.obj = [[c555(c) for c in data['objpal'][(phase, s)]] for s in G.OBJ_SLOTS]

    @property
    def name(self):
        return '%s_%s' % (self.mode, self.phase)


def all_pals(data):
    return [Pal(data, m, ph) for m in ('dmg', 'cgb') for ph in PHASES]


def put_tile(img, tile, x, y, pal, transparent=False, flipx=False, flipy=False):
    W, H = img.size
    for ty in range(len(tile)):
        for tx in range(len(tile[0])):
            sx = len(tile[0]) - 1 - tx if flipx else tx
            sy = len(tile) - 1 - ty if flipy else ty
            v = tile[sy][sx]
            if transparent and v == 0:
                continue
            px, py = x + tx, y + ty
            if 0 <= px < W and 0 <= py < H:
                img.putpixel((px, py), pal[v])


def save(img, path, scale=3):
    img.save(path)
    root, ext = os.path.splitext(path)
    img.resize((img.width * scale, img.height * scale), Image.NEAREST).save(root + '_x3' + ext)


# ---------------------------------------------------------------------------- metatile drawing

def mt_quarters(data, m, frame=0):
    """the 4 tiles of metatile m (animated quarters at `frame`)"""
    out = []
    for c, idx in enumerate(data['mt_tiles'][m]):
        tile = data['bg'][idx]
        for a, ai in data['anim_idx'].items():
            if ai == idx:
                tile = data['anims'][a][frame % 4]
        out.append(tile)
    return out


def draw_mt(img, data, m, x, y, pal, frame=0):
    q = mt_quarters(data, m, frame)
    for c in range(4):
        p = pal.bg[data['mt_attr'][m][c]]
        put_tile(img, q[c], x + (c & 1) * 8, y + (c >> 1) * 8, p)


MT = dict((n, i) for i, n in enumerate(G.MT_ORDER))


def sheet_metatiles(data, pal):
    cols = 12
    rows = (len(G.MT_ORDER) + cols - 1) // cols
    img = Image.new('RGB', (cols * 20 + 4, rows * 20 + 4), SHEET_BG)
    for i in range(len(G.MT_ORDER)):
        draw_mt(img, data, i, 4 + (i % cols) * 20, 4 + (i // cols) * 20, pal)
    return img


# ---------------------------------------------------------------------------- landscapes

def hsh(x, y, s=0):
    h = (x * 374761393 + y * 668265263 + s * 2246822519) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return (h ^ (h >> 16)) & 0xFF


def vnoise(x, y, cell, s):
    gx, gy = x // cell, y // cell
    fx, fy = (x % cell) / cell, (y % cell) / cell
    a, b = hsh(gx, gy, s), hsh(gx + 1, gy, s)
    c, d = hsh(gx, gy + 1, s), hsh(gx + 1, gy + 1, s)
    top = a + (b - a) * fx
    bot = c + (d - c) * fx
    return top + (bot - top) * fy


BIOMES = {
    # name: function(x, y) -> metatile name
    'sea': lambda x, y, n, r: 'MT_SEA_GLINT' if r < 10 else 'MT_SEA',
    'shore': lambda x, y, n, r: ('MT_SHALLOW' if y < 2 else 'MT_SAND' if n < 150 or r > 20
                                 else 'MT_DUNE'),
    'meadow': lambda x, y, n, r: ('MT_GRASS_TALL' if n > 170 else 'MT_FLOWERS' if r < 12
                                  else 'MT_GRASS'),
    'forest': lambda x, y, n, r: ('MT_TREE' if n > 110 and r > 60 else 'MT_UNDERGROWTH'
                                  if n > 90 else 'MT_GRASS'),
    'desert': lambda x, y, n, r: 'MT_BONES' if r < 6 else 'MT_DUNE' if n > 120 else 'MT_SAND',
    'tundra': lambda x, y, n, r: 'MT_PINE' if n > 140 and r > 90 else 'MT_SNOW',
    'rock': lambda x, y, n, r: ('MT_ROCK_PEAK' if n > 170 else 'MT_ROCK' if n > 100 else
                                'MT_GRASS'),
    'ash': lambda x, y, n, r: ('MT_MONOLITH' if r < 5 else 'MT_GLASS' if n > 150 and r < 80
                               else 'MT_ASH'),
    'ruins': lambda x, y, n, r: ('MT_RUIN_WALL' if y == 1 and 1 < x < 7 and r > 40 else
                                 'MT_PILLAR' if r < 14 else 'MT_RUIN_FLOOR' if n > 100
                                 else 'MT_GRASS'),
}


def biome_grid(name, w=10, h=7):
    f = BIOMES[name]
    return [[MT[f(x, y, vnoise(x, y, 3, len(name)), hsh(x, y, 7))] for x in range(w)]
            for y in range(h)]


def draw_grid(img, data, grid, x0, y0, pal, frame=0):
    for y, row in enumerate(grid):
        for x, m in enumerate(row):
            draw_mt(img, data, m, x0 + x * 16, y0 + y * 16, pal, frame)


def sheet_biomes(data, pal):
    names = list(BIOMES)
    cols = 3
    w, h = 160, 112
    img = Image.new('RGB', (cols * (w + 4) + 4, ((len(names) + cols - 1) // cols) * (h + 4) + 4),
                    SHEET_BG)
    for i, n in enumerate(names):
        draw_grid(img, data, biome_grid(n), 4 + (i % cols) * (w + 4), 4 + (i // cols) * (h + 4),
                  pal)
    return img


# ---------------------------------------------------------------------------- band

def band_heights(seed=1):
    """height in px (0..20) of the far land for each of 32 band columns x 8 px"""
    hs = []
    for x in range(256):
        v = vnoise(x, 0, 64, seed) * 0.6 + vnoise(x, 0, 16, seed + 3) * 0.4
        hs.append(int(2 + v / 255.0 * 14))
    return hs


def band_strip(data, pal, night, markers=True):
    """the full 256x24 panorama, built from the real band tiles and a height per column"""
    bi = data['band_idx']
    bg = data['bg']
    sky = pal.bg[G.PAL_CLASSES.index('SKY')]
    hs = band_heights()
    img = Image.new('RGB', (256, 24))
    for col in range(32):
        hL = hs[col * 8]
        hR = hs[(col * 8 + 7) % 256]
        for row in range(3):
            base = (2 - row) * 8          # land height at the bottom of this tile row
            h = (hL + hR) // 2
            d = hR - hL
            if h <= base:
                t = bi['BAND_SKY_TOP'] if row == 0 else bi['BAND_SKY']
                if night and hsh(col, row, 5) < 60:
                    t = bi['BAND_STAR0'] + (hsh(col, row, 9) & 1)
            elif h >= base + 8:
                t = bi['BAND_LAND']
            else:
                k = h - base - 1
                if d >= 4:
                    t = bi['BAND_SLOPE_UP0'] + max(0, min(7, k - 3))
                elif d <= -4:
                    t = bi['BAND_SLOPE_DN0'] + max(0, min(7, k - 3))
                else:
                    t = bi['BAND_RIDGE0'] + k
            put_tile(img, bg[t], col * 8, row * 8, sky)
    if markers:
        spr = data['spr_art']
        o = dict((s, i) for i, s in enumerate(G.OBJ_SLOTS))
        for name, x, slot in (('SPR_BAND_BEACON', 40, 'BAND'), ('SPR_BAND_BEACON_LIT', 130, 'LIGHT'),
                              ('SPR_BAND_CAIRN', 98, 'BAND'), ('SPR_BAND_HEART', 205, 'LIGHT')):
            put_tile(img, spr[name], x, 8, pal.obj[o[slot]], transparent=True)
    return img


def draw_band(img, data, y0, pal, night, markers=True, scx=0):
    strip = band_strip(data, pal, night, markers)
    for x in range(img.width):
        for y in range(24):
            img.putpixel((x, y0 + y), strip.getpixel(((x + scx) % 256, y)))


def sheet_band(data, pal, night):
    img = Image.new('RGB', (256, 24), SHEET_BG)
    draw_band(img, data, 0, pal, night)
    return img


# ---------------------------------------------------------------------------- sprites

def sheet_sprites(data, pal):
    o = dict((s, i) for i, s in enumerate(G.OBJ_SLOTS))
    slot = {'SPR_PIP': 'EMBER', 'SPR_ICON': 'UI', 'SPR_HINT': 'UI', 'SPR_BAND_BEACON_LIT': 'LIGHT',
            'SPR_BAND_HEART': 'LIGHT', 'SPR_BAND': 'BAND', 'SPR_RAIN': 'WEATHER',
            'SPR_SNOW': 'WEATHER', 'SPR_GLOW': 'GLOW', 'SPR_WATCHER': 'WATCHER',
            'SPR_BIRD': 'WEATHER', 'SPR_MAP': 'UI', 'SPR_PL': 'PLAYER'}
    names = list(data['spr_art'])
    ground = pal.bg[0][1]
    img = Image.new('RGB', (len(names) * 20 + 4, 40), ground)
    x = 4
    for n in names:
        art = data['spr_art'][n]
        s = next(v for k, v in sorted(slot.items(), key=lambda kv: -len(kv[0])) if n.startswith(k))
        put_tile(img, art, x, 4, pal.obj[o[s]], transparent=True)
        x += len(art[0]) + 4
    return img.crop((0, 0, x, 40))


# ---------------------------------------------------------------------------- title

def title_img(data, cgb):
    img = Image.new('RGB', (160, 144))
    tp = [[c555(c) for c in p] for p in data['title_pal']]
    dm = dmg_map(0xE4)
    for i, ti in enumerate(data['title_map']):
        x, y = (i % 20) * 8, (i // 20) * 8
        put_tile(img, data['title_tiles'][ti], x, y, tp[data['title_attr'][i]] if cgb else dm)
    return img


# ---------------------------------------------------------------------------- mock screenshot

MOCK = [
    # 10 x 8 metatiles (the bottom one is half visible): a shore, a meadow, a fire, a causeway
    'SEA SEA SEA SEA_GLINT SEA SEA SEA SEA SEA SEA_GLINT',
    'SEA SHALLOW SHALLOW SEA SEA SHALLOW SHALLOW SEA SEA SEA',
    'SAND SAND SHALLOW SHALLOW SHALLOW SAND SAND SHALLOW SHALLOW SHALLOW',
    'GRASS SAND SAND SAND SAND ROAD SAND SAND SAND SAND',
    'TREE GRASS GRASS FLOWERS GRASS ROAD GRASS GRASS_TALL GRASS_TALL TREE',
    'TREE TREE GRASS GRASS FIRE_LIT ROAD GRASS GRASS GRASS_TALL TREE',
    'UNDERGROWTH TREE GRASS GRASS GRASS ROAD GRASS MONOLITH GRASS GRASS',
    'TREE UNDERGROWTH GRASS CAIRN_OLD GRASS ROAD GRASS GRASS FLOWERS GRASS',
]


def mock_screen(data, pal, frame=0):
    night = pal.phase == 'night'
    img = Image.new('RGB', (160, 144))
    grid = [[MT['MT_' + n] for n in row.split()] for row in MOCK]
    land = Image.new('RGB', (160, 128))
    draw_grid(land, data, grid, 0, 0, pal, frame)
    img.paste(land.crop((0, 4, 160, 124)), (0, 24))
    draw_band(img, data, 0, pal, night, scx=60)
    o = dict((s, i) for i, s in enumerate(G.OBJ_SLOTS))
    art = data['spr_art']
    # HUD: 4 pips + item icon on lines 0-8
    for i in range(4):
        put_tile(img, art['SPR_PIP_FULL' if i < 3 else 'SPR_PIP_EMPTY'], 4 + i * 8, 0,
                 pal.obj[o['EMBER']], transparent=True)
    put_tile(img, art['SPR_ICON_LANTERN'], 148, 0, pal.obj[o['UI']], transparent=True)
    # player standing on the grass right of the fire
    px, py = 96, 24 + 5 * 16 - 4 - 2
    cx, cy = px + 8, py + 8
    if night:
        g = art['SPR_GLOW0']
        gp = pal.obj[o['GLOW']]
        put_tile(img, g, cx - 16, cy - 16, gp, True)
        put_tile(img, g, cx, cy - 16, gp, True, flipx=True)
        put_tile(img, g, cx - 16, cy, gp, True, flipy=True)
        put_tile(img, g, cx, cy, gp, True, flipx=True, flipy=True)
    put_tile(img, art['SPR_PL_DOWN0'], px, py, pal.obj[o['PLAYER']], transparent=True)
    if night:
        put_tile(img, art['SPR_WATCHER'], 118, 24 + 6 * 16 - 20, pal.obj[o['WATCHER']], True)
    else:
        put_tile(img, art['SPR_BIRD0'], 30, 40, pal.obj[o['WEATHER']], True)
    return img


# ---------------------------------------------------------------------------- real world

class WPos(__import__('ctypes').Structure):
    _fields_ = [('x', __import__('ctypes').c_uint16), ('y', __import__('ctypes').c_uint16)]


def load_world(out_dir):
    """Build src/core as a host shared library (needs gcc) and return a ctypes handle, or None."""
    import ctypes
    import glob
    import subprocess
    srcs = sorted(glob.glob(os.path.join(G.ROOT, 'src', 'core', '*.c')))
    if not srcs:
        return None
    lib = os.path.join(out_dir, 'libworld.so')
    try:
        subprocess.check_call(['gcc', '-std=c99', '-O2', '-shared', '-fPIC',
                               '-I' + os.path.join(G.ROOT, 'src', 'core'), '-o', lib] + srcs,
                              stderr=subprocess.DEVNULL)
        w = ctypes.CDLL(lib)
    except Exception:
        return None
    w.world_mt.restype = ctypes.c_uint8
    w.world_mt.argtypes = [ctypes.c_uint16, ctypes.c_uint16]

    class Layout(ctypes.Structure):
        _fields_ = [('seed', ctypes.c_uint16), ('start', WPos), ('beacon', WPos * 3),
                    ('shrine', WPos * 3), ('heart', WPos)]
    w.layout = Layout.in_dll(w, 'world')
    return w


def world_grid(w, cx, cy, cols, rows):
    return [[w.world_mt((cx + x) & 0xFFFF, (cy + y) & 0xFFFF) for x in range(cols)]
            for y in range(rows)]


def world_mock(data, pal, w, mx, my, frame=2):
    """a real 160x144 screen: band + the land around (mx, my) with the player in the middle"""
    night = pal.phase == 'night'
    img = Image.new('RGB', (160, 144))
    land = Image.new('RGB', (160, 128))
    draw_grid(land, data, world_grid(w, mx - 5, my - 4, 10, 8), 0, 0, pal, frame)
    img.paste(land.crop((0, 4, 160, 124)), (0, 24))
    draw_band(img, data, 0, pal, night, scx=60)
    o = dict((s, i) for i, s in enumerate(G.OBJ_SLOTS))
    art = data['spr_art']
    for i in range(4):
        put_tile(img, art['SPR_PIP_FULL' if i < 3 else 'SPR_PIP_EMPTY'], 4 + i * 8, 0,
                 pal.obj[o['EMBER']], transparent=True)
    put_tile(img, art['SPR_ICON_LANTERN'], 148, 0, pal.obj[o['UI']], transparent=True)
    px, py = 80 - 8, 24 + 60 - 8
    cx, cy = px + 8, py + 8
    if night:
        g = art['SPR_GLOW0']
        gp = pal.obj[o['GLOW']]
        put_tile(img, g, cx - 16, cy - 16, gp, True)
        put_tile(img, g, cx, cy - 16, gp, True, flipx=True)
        put_tile(img, g, cx - 16, cy, gp, True, flipy=True)
        put_tile(img, g, cx, cy, gp, True, flipx=True, flipy=True)
    put_tile(img, art['SPR_PL_DOWN0'], px, py, pal.obj[o['PLAYER']], transparent=True)
    return img


def world_sheets(data, out_dir):
    w = load_world(out_dir)
    if w is None:
        print('preview: world core not available, skipping real-world renders')
        return
    w.world_init(1)
    L = w.layout
    spots = [('start', L.start.x, L.start.y + 1)] + \
        [('beacon%d' % i, L.beacon[i].x, L.beacon[i].y + 2) for i in range(3)] + \
        [('heart', L.heart.x, L.heart.y + 2)]
    for pal in all_pals(data):
        if pal.mode == 'dmg' and pal.phase in ('dawn', 'dusk'):
            continue
        # a 48x40 metatile region around the start
        big = Image.new('RGB', (48 * 16, 40 * 16))
        draw_grid(big, data, world_grid(w, L.start.x - 24, L.start.y - 20, 48, 40), 0, 0, pal, 2)
        save(big, os.path.join(out_dir, 'world_%s.png' % pal.name), 1)
        sheet = Image.new('RGB', (len(spots) * 164 + 4, 152), SHEET_BG)
        for i, (nm, x, y) in enumerate(spots):
            sheet.paste(world_mock(data, pal, w, x, y), (4 + i * 164, 4))
        save(sheet, os.path.join(out_dir, 'world_mock_%s.png' % pal.name), 2)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(G.ROOT, 'build', 'preview'))
    args = ap.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)
    data = G.load_assets(os.path.join(G.ROOT, 'assets'), strict=False)
    mocks = []
    for pal in all_pals(data):
        night = pal.phase == 'night'
        save(sheet_metatiles(data, pal), os.path.join(args.out, 'metatiles_%s.png' % pal.name), 4)
        save(sheet_biomes(data, pal), os.path.join(args.out, 'biomes_%s.png' % pal.name), 2)
        save(sheet_band(data, pal, night), os.path.join(args.out, 'band_%s.png' % pal.name), 3)
        save(sheet_sprites(data, pal), os.path.join(args.out, 'sprites_%s.png' % pal.name), 4)
        m = mock_screen(data, pal)
        save(m, os.path.join(args.out, 'mock_%s.png' % pal.name), 3)
        mocks.append(m)
    sheet = Image.new('RGB', (4 * 164 + 4, 2 * 148 + 4), SHEET_BG)
    for i, m in enumerate(mocks):
        sheet.paste(m, (4 + (i % 4) * 164, 4 + (i // 4) * 148))
    save(sheet, os.path.join(args.out, 'mock_sheet.png'), 2)
    save(title_img(data, False), os.path.join(args.out, 'title_dmg.png'), 3)
    save(title_img(data, True), os.path.join(args.out, 'title_cgb.png'), 3)
    world_sheets(data, args.out)
    print('preview: wrote %s' % args.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
