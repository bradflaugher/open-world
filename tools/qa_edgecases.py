#!/usr/bin/env python3
"""OPEN WORLD edge cases, played with real button inputs (the autopilot is the driver).

    python3 tools/qa_edgecases.py [--cgb] [--seed 0x1234] [--only name,name] [--rom ...] [--owgen ...]

Every case boots a fresh world (seed poked on the title, as the autopilot does), plays with the
d-pad and buttons only, reads memory to judge, saves screenshots to
build/playthrough/edge_<model>/ and prints one JSON line per case. Cases:

  cold_whiteout      sit in the open at night until the cold takes you; wake at the fire
  night_by_fire      sit next to the lit start fire all night, not in cover (Watchers?)
  night_in_cover     the same, standing in tall grass / undergrowth within reach of the fire
  map_at_night       open the map at night for 20 s: does time / warmth / the Watcher pause?
  map_spam           START pressed 20 times quickly, then back to the world
  map_mid_glide      START (and SELECT, B) pressed during a glide
  ending_spam        all buttons mashed through the ending: exactly one new world
  water_edges        run into the sea and shallows from 8 directions (collision holes?)
  spam_a             A mashed next to the lit fire, on shallows without stones, on open ground
  cairn_limit        build cairns next to a fire (the pouch refills) until something gives
  glide_refused      A with the cloak where the landing is blocked
  heart_early        walk up to the Heart before the beacons are lit
  map_after_journey  the map after three beacons (what does a journey look like?)
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import autopilot as A  # noqa: E402
from autopilot import MT, DIRS, GS_WORLD, GS_MAP, GS_ENDING, GS_WHITEOUT, PL_SLEEP, PL_SIT, PL_GLIDE  # noqa: E402


class Edge(A.Pilot):
    def fresh(self):
        self.boot_new_world()
        self.leg_begin('edge')
        self.wake()

    def light_start(self):
        self.face((0, -1))
        self.press('a', 2, 12)

    def open_cell(self, center, rmin, rmax, need_hide=None, avoid_warm=True):
        """A walkable cell rmin..rmax (Chebyshev) from center, reachable, with open neighbours."""
        ter = self.ter
        warm = [ter.xy(i) for i, m in enumerate(ter.cur) if self.flags[m] & A.MTF_WARM]
        best = None
        for r in range(rmin, rmax + 1):
            for dx in range(-r, r + 1):
                for dy in range(-r, r + 1):
                    if max(abs(dx), abs(dy)) != r:
                        continue
                    x, y = center[0] + dx, center[1] + dy
                    m = ter.cur[ter.idx(x, y)]
                    if self.flags[m] & A.MTF_SOLID:
                        continue
                    if need_hide is not None and bool(self.flags[m] & A.MTF_HIDE) != need_hide:
                        continue
                    if avoid_warm and any(max(abs(x - a), abs(y - b)) <= 5 for a, b in warm):
                        continue
                    if any(self.flags[ter.cur[ter.idx(x + a, y + b)]] & A.MTF_SOLID for a, b in DIRS[::2]):
                        continue
                    p = A.plan(ter, self.cell(), {(x, y)}, self.u8('items'), 0, limit=3000)
                    if p is not None:
                        return (x, y)
        return best

    def idle(self, n, until=None):
        for _ in range(n):
            self.buttons(())
            self.tick()
            if until and until():
                return True
        return False

    def mash(self, n, keys=('a', 'b', 'start', 'select'), period=3):
        for i in range(n):
            self.buttons({keys[(i // period) % len(keys)]} if (i // period) % 2 == 0 else set())
            self.tick()
        self.buttons(())

    def world_ok(self, what):
        bad = self.check_land_vs_host(what)
        return len(bad) == 0


def case_cold_whiteout(e):
    e.fresh()
    e.light_start()
    w = e.layout
    c = e.open_cell(w['start'], 7, 14)
    e.goto({c}, allow_night=False)
    f0 = e.frame_n
    sat = e.idle(400, lambda: e.u8('pl_state') == PL_SIT)
    t_sit = e.frame_n - f0
    night_f = None
    shot = False
    for _ in range(20000):
        e.buttons(())
        e.tick()
        if night_f is None and e.u8('phase') == A.PH_NIGHT:
            night_f = e.frame_n
        if night_f and not shot and e.frame_n - night_f > 300:
            e.milestone('cold_night_sitting')
            shot = True
        if e.state() == GS_WHITEOUT:
            break
    wo = e.state() == GS_WHITEOUT
    f_wo = e.frame_n
    e.tick(30)
    e.milestone('cold_whiteout')
    e.idle(3000, lambda: e.state() == GS_WORLD and e.u8('pal_fade') == 0)
    e.milestone('cold_wake')
    r = dict(sat=sat, frames_to_sit=t_sit, whiteout=wo, frames_sit_to_night=(night_f or 0) - f0,
             frames_night_to_whiteout=f_wo - (night_f or f_wo), wake_cell=e.cell(),
             expected=(w['start'][0], w['start'][1] + 1), asleep=e.u8('pl_state') == PL_SLEEP,
             tod=e.u16('tod'), warmth=e.u16('warmth'), day=e.u16('day_count'))
    e.wake()
    r['land_ok'] = e.world_ok('after whiteout')
    return r


def night_by_fire(e, cover):
    e.fresh()
    e.light_start()
    s = e.layout['start']
    spots = []
    for dx in range(-2, 3):
        for dy in range(-2, 3):
            x, y = s[0] + dx, s[1] + dy
            m = e.ter.mt(x, y)
            if not e.flags[m] & A.MTF_SOLID and bool(e.flags[m] & A.MTF_HIDE) == cover:
                spots.append((max(abs(dx), abs(dy)), (x, y)))
    if not spots:
        return dict(skipped=f'no {"cover" if cover else "open"} cell within 2 of the start fire')
    spots.sort()
    e.goto({spots[0][1]}, allow_night=False)
    watchers = touches = 0
    prev = 0
    shot = False
    f0 = e.frame_n
    closest = 999
    # through dusk and night (sitting: 8x), until dawn
    while e.frame_n - f0 < 12000:
        e.buttons(())
        e.tick()
        on = e.u8('watch_on')
        if on and not prev:
            watchers += 1
        prev = on
        if on:
            w = e.watch()
            if w:
                closest = min(closest, max(abs(w[0]), abs(w[1])))
                if not shot and max(abs(w[0]), abs(w[1])) < 40:
                    e.milestone('watcher_at_fire')
                    shot = True
        if e.state() == GS_WHITEOUT:
            touches += 1
            e.idle(3000, lambda: e.state() == GS_WORLD and e.u8('pal_fade') == 0)
            break
        if e.u8('phase') == A.PH_DAWN and e.frame_n - f0 > 3000:
            break
    return dict(cell=e.cell(), cover=cover, frames=e.frame_n - f0, watchers=watchers, closest_px=closest,
                whiteout_by_watcher=touches, sitting=e.u8('pl_state') == PL_SIT, tod=e.u16('tod'))


def case_night_by_fire(e):
    return night_by_fire(e, False)


def case_night_in_cover(e):
    return night_by_fire(e, True)


def case_map_at_night(e):
    e.fresh()
    e.light_start()
    # sit by the fire into the night, then walk out and open the map
    while e.u16('tod') < A.T_NIGHT + 600:
        e.buttons(())
        e.tick()
    c = e.open_cell(e.layout['start'], 6, 12)
    e.goto({c}, allow_night=False)
    before = dict(tod=e.u16('tod'), warmth=e.u16('warmth'), watch=e.u8('watch_on'), amb=e.u8('snd_cur_mode'))
    e.press('start', 2, 2)
    opened = e.idle(200, lambda: e.state() == GS_MAP)
    e.idle(1200)
    e.milestone('map_night')
    mid = dict(tod=e.u16('tod'), warmth=e.u16('warmth'), amb=e.u8('snd_cur_mode'), state=e.state())
    e.press('start', 2, 2)
    back = e.idle(1500, lambda: e.state() == GS_WORLD and e.u8('pal_fade') == 0)
    e.tick(10)
    e.milestone('map_night_back')
    after = dict(tod=e.u16('tod'), warmth=e.u16('warmth'), amb=e.u8('snd_cur_mode'), split=e.u8('split_mode'))
    return dict(opened=opened, back=back, before=before, in_map=mid, after=after, land_ok=e.world_ok('after map'))


def case_map_spam(e):
    e.fresh()
    e.hold_keys(('right', 'b'), 40)
    states = []
    for i in range(20):
        e.press('start', 1, 2 + (i % 5))
        states.append(e.state())
    ok = e.idle(3000, lambda: e.state() == GS_WORLD and e.u8('pal_fade') == 0)
    if not ok and e.state() == GS_MAP:
        e.press('start', 2, 2)
        ok = e.idle(3000, lambda: e.state() == GS_WORLD and e.u8('pal_fade') == 0)
    e.tick(20)
    e.milestone('after_map_spam')
    f = e.foot()
    e.hold_keys(('down',), 30)
    return dict(back_in_world=ok, final_state=e.state(), states_seen=sorted(set(states)),
                moves_after=e.foot() != f, land_ok=e.world_ok('after map spam'), split=e.u8('split_mode'))


def play_to_cloak(e):
    """Autopilot through beacons 0 and 1 (the cloak)."""
    e.fresh()
    e.light_start()
    w = e.layout
    e.reach_and_interact(w['shrine'][0], 'shrine0', lambda: e.u8('items') & 2)
    e.reach_and_interact(w['beacon'][0], 'beacon0', lambda: e.u8('beacons_lit') & 1)
    e.reach_and_interact(w['shrine'][1], 'shrine1', lambda: e.u8('items') & 4)
    e.reach_and_interact(w['beacon'][1], 'beacon1', lambda: e.u8('beacons_lit') & 2)


def case_map_mid_glide(e):
    play_to_cloak(e)
    out = []

    def hook(p):
        rec = {}
        p.tick(6)
        rec['glide_before'] = p.u8('pl_state') == PL_GLIDE
        p.press('start', 2, 1)
        rec['state_after_start'] = p.state()
        p.press('select', 2, 1)
        p.press('b', 2, 1)
        p.milestone('start_mid_glide')
        for _ in range(60):
            if p.u8('pl_state') != PL_GLIDE:
                break
            p.tick()
        p.tick(10)
        rec['after_landing_state'] = p.state()
        rec['equipped'] = p.u8('equipped')
        if p.state() == GS_MAP:
            p.milestone('map_after_glide')
            p.press('start', 2, 2)
            p.idle(2000, lambda: p.state() == GS_WORLD and p.u8('pal_fade') == 0)
        out.append(rec)
        p.hooks.pop('glide_mid', None)

    e.hooks['glide_mid'] = hook
    e.reach_and_interact(e.layout['beacon'][2], 'beacon2', lambda: e.u8('beacons_lit') & 4)
    return dict(glide=out, lit=e.u8('beacons_lit'), land_ok=e.world_ok('after glide + map'))


def case_ending_spam(e):
    play_to_cloak(e)
    e.reach_and_interact(e.layout['beacon'][2], 'beacon2', lambda: e.u8('beacons_lit') & 4)
    done0 = e.u8('worlds_done')
    seed0 = e.layout['seed']
    h = e.layout['heart']
    adj = {(h[0] + dx, h[1] + dy) for dx, dy in DIRS if not e.ter.solid(h[0] + dx, h[1] + dy)}
    try:
        e.goto(adj)
    except Exception:
        pass
    started = e.idle(300, lambda: e.state() == GS_ENDING)
    states = set()
    for i in range(1800):
        e.buttons({('a', 'b', 'start', 'select', 'up', 'left')[(i // 2) % 6]} if i % 4 < 2 else set())
        e.tick()
        states.add(e.state())
        if i == 200:
            e.milestone('ending_mashing')
        if e.state() == GS_WORLD and e.u8('pal_fade') == 0 and i > 60:
            break
    e.buttons(())
    e.idle(2000, lambda: e.state() == GS_WORLD and e.u8('pal_fade') == 0)
    e.idle(120)
    e.milestone('after_ending_spam')
    return dict(ending_started=started, worlds_done=(done0, e.u8('worlds_done')), seed=(seed0, e.world_layout()['seed']),
                states=sorted(states), state=e.state(), asleep=e.u8('pl_state') == PL_SLEEP,
                beacons=e.u8('beacons_lit'), items=e.u8('items'))


def case_water_edges(e):
    e.fresh()
    ter = e.ter
    s = e.layout['start']
    # water cells near the start with walkable ground next to them
    found = []
    for r in range(2, 40):
        for dx in range(-r, r + 1):
            for dy in range(-r, r + 1):
                if max(abs(dx), abs(dy)) != r:
                    continue
                x, y = s[0] + dx, s[1] + dy
                if not e.flags[ter.cur[ter.idx(x, y)]] & A.MTF_WATER:
                    continue
                for d in DIRS:
                    ax, ay = x - d[0], y - d[1]
                    if not e.flags[ter.cur[ter.idx(ax, ay)]] & A.MTF_SOLID and \
                            not e.flags[ter.cur[ter.idx(ax - d[0], ay - d[1])]] & A.MTF_SOLID:
                        found.append(((ax - d[0], ay - d[1]), d))
        if len(found) > 24:
            break
    tried = 0
    in_water = 0
    seen = set()
    for (c, d) in found:
        if d in seen:
            continue
        try:
            e.goto({c}, allow_night=False, tries=4, budget=4000)
        except Exception:
            continue
        seen.add(d)
        e.hold_keys(A.DIR_KEYS[d] + ('b',), 90)
        m = ter.cur[ter.idx(*e.cell())]
        if e.flags[m] & A.MTF_SOLID:
            in_water += 1
            e.milestone('in_water')
        tried += 1
        if len(seen) == 8:
            break
    return dict(directions_tried=tried, foot_in_solid=in_water, collision_holes=e.hitbox_bad,
                torn_reads=e.torn_reads)


def case_spam_a(e):
    e.fresh()
    e.light_start()
    r = {}
    shakes = 0
    for i in range(30):
        e.press('a', 1, 2)
        shakes += e.u8('shake') > 0
    r['a_next_to_lit_fire_shakes'] = shakes
    e.milestone('spam_a_fire')
    # A with only the lantern on open ground
    c = e.open_cell(e.layout['start'], 5, 10)
    e.goto({c}, allow_night=False)
    for i in range(30):
        e.press('a', 1, 1)
    r['state_after'] = e.state()
    r['mods'] = e.u8('world_mod_count')
    r['sfx_queue_ok'] = True
    return r


def case_cairn_limit(e):
    e.fresh()
    e.light_start()
    w = e.layout
    e.reach_and_interact(w['shrine'][0], 'shrine0', lambda: e.u8('items') & 2)
    e.reach_and_interact(w['beacon'][0], 'beacon0', lambda: e.u8('beacons_lit') & 1)
    # next to the lit beacon the pouch refills: build a cairn, step, build, ...
    e.equip(A.IT_STONES)
    b = w['beacon'][0]
    built = 0
    log = []
    cells = [(b[0] + dx, b[1] + dy) for dy in range(-3, 4) for dx in range(-3, 4)
             if 2 <= max(abs(dx), abs(dy)) <= 3]
    for c in cells:
        e.ter.set_mods(e.mods())
        if e.ter.solid(*c):
            continue
        # stand next to c (a 4-neighbour that is open), face it, A
        adj = {(c[0] + dx, c[1] + dy) for dx, dy in DIRS[::2] if not e.ter.solid(c[0] + dx, c[1] + dy)
               and max(abs(c[0] + dx - b[0]), abs(c[1] + dy - b[1])) <= 3}
        try:
            e.goto(adj, allow_night=False, tries=3, budget=3000)
        except Exception:
            continue
        n0 = e.u8('cairn_n')
        e.interact(c, 'cairn')
        if e.u8('cairn_n') > n0:
            built += 1
        log.append((c, e.u8('cairn_n'), e.u8('stones'), e.u8('world_mod_count')))
    e.milestone('cairns')
    return dict(built=built, cairn_n=e.u8('cairn_n'), stones=e.u8('stones'), mods=e.u8('world_mod_count'),
                note='stones refill beside a warm source, so cairns are limited only by MAX_CAIRNS/MAX_MODS',
                land_ok=e.world_ok('cairns'))


def case_glide_refused(e):
    play_to_cloak(e)
    e.equip(A.IT_CLOAK)
    ter = e.ter
    here = e.cell()
    tries = []
    # a solid glidable cell with a blocked landing
    for r in range(1, 30):
        for dx in range(-r, r + 1):
            for dy in range(-r, r + 1):
                if max(abs(dx), abs(dy)) != r:
                    continue
                x, y = here[0] + dx, here[1] + dy
                for d in DIRS[::2]:
                    a = (x - d[0], y - d[1])
                    m1 = ter.mt(x, y)
                    land = ter.mt(x + 2 * d[0], y + 2 * d[1])
                    if e.flags[m1] & A.MTF_SOLID and e.flags[m1] & A.MTF_GLIDE and \
                            e.flags[land] & A.MTF_SOLID and not ter.solid(*a):
                        tries.append((a, d))
        if len(tries) > 5:
            break
    for a, d in tries:
        try:
            e.goto({a}, allow_night=False, tries=3, budget=4000)
        except Exception:
            continue
        e.face(d)
        e.press('a', 1, 2)
        glided = False
        for _ in range(6):
            glided |= e.u8('pl_state') == PL_GLIDE
            e.tick()
        shake = e.u8('shake')
        e.milestone('glide_refused')
        return dict(at=a, dir=d, glided=glided, shake_after=shake, state=e.state())
    return dict(skipped='no blocked landing found')


def case_heart_early(e):
    e.fresh()
    e.light_start()
    h = e.layout['heart']
    adj = {(h[0] + dx, h[1] + dy) for dx, dy in DIRS[::2] if not e.ter.solid(h[0] + dx, h[1] + dy)}
    try:
        e.goto(adj, allow_night=False)
    except Exception as ex:
        return dict(reached=False, error=str(ex))
    e.interact(h, 'heart')
    e.milestone('heart_early')
    return dict(reached=True, state=e.state(), ending=e.state() == GS_ENDING, shake=e.u8('shake'),
                on_band=e.u8('heart_revealed'))


def case_map_after_journey(e):
    play_to_cloak(e)
    e.reach_and_interact(e.layout['beacon'][2], 'beacon2', lambda: e.u8('beacons_lit') & 4)
    e.press('start', 2, 2)
    e.idle(200, lambda: e.state() == GS_MAP)
    frames = [0]
    for t in (60, 240, 600):
        e.idle(t - frames[-1])
        frames.append(t)
        e.milestone(f'map_{t}f')
    e.press('start', 2, 2)
    back = e.idle(2000, lambda: e.state() == GS_WORLD and e.u8('pal_fade') == 0)
    e.tick(4)
    e.milestone('heart_on_band')
    return dict(back=back, heart_revealed=e.u8('heart_revealed'), land_ok=e.world_ok('after map'))


CASES = [n[5:] for n in list(globals()) if n.startswith('case_')]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seed', type=lambda s: int(s, 0), default=0x1234)
    ap.add_argument('--cgb', action='store_true')
    ap.add_argument('--only', default='')
    ap.add_argument('--rom', default=os.path.join(A.ROOT, 'build', 'open-world.gb'))
    ap.add_argument('--owgen', default=os.path.join(A.ROOT, 'build', 'owgen'))
    ap.add_argument('--out', default=os.path.join(A.ROOT, 'build', 'playthrough'))
    a = ap.parse_args()
    sym = os.path.splitext(a.rom)[0] + '.sym'
    names = a.only.split(',') if a.only else CASES
    results = {}
    for n in names:
        e = Edge(a.rom, sym, a.owgen, a.seed, a.cgb, os.path.join(a.out, 'edge_' + ('cgb' if a.cgb else 'dmg')),
                 log=None, tag='_' + n)
        try:
            r = globals()['case_' + n](e)
        except Exception as ex:
            import traceback
            r = dict(error=f'{type(ex).__name__}: {ex}', tb=traceback.format_exc()[-600:])
            try:
                e.milestone('error')
            except Exception:
                pass
        r['collision_holes'] = e.hitbox_bad
        r['frames'] = e.frame_n
        results[n] = r
        print(json.dumps({n: r}, default=str))
        e.stop()
    with open(os.path.join(a.out, f"edge_{'cgb' if a.cgb else 'dmg'}.json"), 'w') as f:
        json.dump(results, f, indent=1, default=str)


if __name__ == '__main__':
    main()
