"""Regenerate the README media in docs/screens/ by playing the real ROM in PyBoy.

    make rom build/owgen && python3 tools/screenshots.py

Writes {cgb,dmg}_{title,day,dusk,night,rain,snow,hint_run,lesson,hint_select,map,beacon,band,
ending}.png (3x) and
gameplay.gif (a short CGB walk from dusk into night, 2x).
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tests'))
from test_rom import (Game, GS_TITLE, GS_WORLD, GS_MAP, GS_ENDING, GS_LESSON, MT, T_DUSK,  # noqa: E402
                      T_NIGHT, T_DAY, WX_CLEAR, WX_RAIN, WX_SNOW, PL_GLIDE, IT_LANTERN, DIRV,
                      host_region, MTF_SOLID)

OUT = os.path.join(ROOT, 'docs', 'screens')
SCALE = 3
SEED = 0x1234


def frame(g, scale=SCALE):
    return g.pb.screen.image.convert('RGB').resize((160 * scale, 144 * scale), 0)


def save(g, name, img=None):
    os.makedirs(OUT, exist_ok=True)
    (img or frame(g)).save(os.path.join(OUT, f'{g.tag}_{name}.png'))
    print('wrote', g.tag, name)


def walkable(g, mt):
    return not (g.pb.memory[g.addr('mt_flags') + mt] & MTF_SOLID)


def find_cell(g, center, want, r=60):
    cx, cy = center
    reg = host_region(SEED, cx - r, cy - r, 2 * r + 1, 2 * r + 1)
    best = None
    for (x, y), m in reg.items():
        if m in want and all(walkable(g, reg.get((x + dx, y + dy), 0)) for dx, dy in ((0, 1), (1, 0), (-1, 0), (0, -1))):
            d = abs(x - cx) + abs(y - cy)
            if best is None or d < best[0]:
                best = (d, (x, y))
    return best and best[1]


def next_to(g, want, center, r=12):
    """teleport onto a walkable cell 4-adjacent to one holding a metatile in `want` (or the
    cell `want` itself when it is a position) and face it"""
    cx, cy = center
    reg = host_region(SEED, cx - r, cy - r, 2 * r + 1, 2 * r + 1)
    targets = [want] if isinstance(want, tuple) else sorted(k for k, v in reg.items() if v in want)
    for t in targets:
        for d, (dx, dy) in DIRV.items():
            a = (t[0] - dx, t[1] - dy)
            if a in reg and walkable(g, reg[a]):
                g.teleport(*a)
                g.face(d)
                return t
    return None


def shoot(cgb):
    g = Game(cgb)
    g.boot_to_title()
    g.run(60)
    save(g, 'title')
    g.set_u16('dbg_seed', SEED)
    g.press('select')
    g.wait(lambda: g.state() == GS_WORLD and g.u8('pal_fade') == 0, 3000)
    g.run(20)
    save(g, 'wake')
    g.wake()
    w = g.world()
    g.face('up')
    g.press('a', after=20)          # light the first fire
    g.set_time(T_DAY + 600)
    g.hold(['down'], 24)
    g.hold(['right'], 30)
    g.run(40)
    save(g, 'day')
    band = g.pb.screen.image.convert('RGB').crop((0, 0, 160, 24)).resize((160 * 5, 24 * 5), 0)
    save(g, 'band', band)
    g.set_time(T_DUSK + 1100)
    g.run(80)
    save(g, 'dusk')
    g.set_time(T_NIGHT + 1100)
    g.hold(['left'], 20)
    g.run(80)
    save(g, 'night')
    g.set_u8('weather', WX_RAIN)
    g.run(60)
    save(g, 'rain')
    snow = find_cell(g, w['start'], {MT['MT_SNOW']}, 90)
    if snow:
        g.teleport(*snow)
        g.set_time(T_DAY + 300)
        g.set_u8('weather', WX_SNOW)
        g.run(90)
        save(g, 'snow')
    g.set_u8('weather', WX_CLEAR)
    # the one-time hold-B: walking a while without running
    g.teleport(*w['start'])
    g.set_time(T_DAY + 300)
    g.pb.button_press('left')
    g.run(170)
    save(g, 'hint_run')
    g.pb.button_release('left')
    # the stones' lesson, mid-way: one stone down, A pressing for the next
    next_to(g, w['shrine'][0], w['shrine'][0])
    g.press('a', after=2)
    g.wait(lambda: g.state() == GS_LESSON, 60)
    g.run(118)
    save(g, 'lesson')
    g.wait(lambda: g.u8('lesson_ready'), 1500)
    g.press('a')
    g.wait(lambda: g.state() == GS_WORLD and g.u8('pal_fade') == 0, 1500)
    # SELECT over the wanderer: the lantern in hand at the shallows, the stones in the pouch
    g.set_u8('equipped', IT_LANTERN)
    next_to(g, {MT['MT_SHALLOW']}, w['beacon'][1])
    g.run(20)
    save(g, 'hint_select')
    b = w['beacon'][0]
    g.teleport(b[0], b[1] + 1)
    g.set_time(T_DUSK + 1500)
    g.face('up')
    g.press('a', after=90)
    save(g, 'beacon')
    g.hold(['down', 'b'], 60)
    g.press('start', after=10)
    g.wait(lambda: g.state() == GS_MAP, 300)
    g.run(150)
    save(g, 'map')
    g.press('start', after=10)
    g.wait(lambda: g.state() == GS_WORLD and g.u8('pal_fade') == 0, 1500)
    g.set_u8('beacons_lit', 7)
    g.set_u8('heart_revealed', 1)
    h = w['heart']
    g.teleport(h[0], h[1] + 1)
    g.face('up')
    g.press('a')
    g.wait(lambda: g.state() == GS_ENDING, 100)
    g.run(200)
    save(g, 'ending')
    g.stop()


def record_gif():
    g = Game(True)
    g.new_world(SEED)
    g.face('up')
    g.press('a', after=10)
    g.set_time(T_DUSK + 1600)
    frames = []

    def grab(n):
        for i in range(n):
            g.pb.tick()
            if i % 2 == 0:
                frames.append(frame(g, 2).quantize(64))

    grab(30)
    for keys, n in ((['down'], 50), (['down', 'right'], 60), (['right', 'b'], 70), (['up', 'right'], 50),
                    (['up'], 40)):
        for k in keys:
            g.pb.button_press(k)
        grab(n)
        for k in keys:
            g.pb.button_release(k)
    g.set_time(T_NIGHT + 1100)
    grab(40)
    for keys, n in ((['left'], 60), (['down', 'left', 'b'], 50)):
        for k in keys:
            g.pb.button_press(k)
        grab(n)
        for k in keys:
            g.pb.button_release(k)
    grab(200)       # stand, then sit
    frames[0].save(os.path.join(OUT, 'gameplay.gif'), save_all=True, append_images=frames[1:],
                   duration=33, loop=0, optimize=True)
    print('wrote gameplay.gif', len(frames), 'frames')
    g.stop()


def main():
    shoot(True)
    shoot(False)
    record_gif()


if __name__ == '__main__':
    main()
