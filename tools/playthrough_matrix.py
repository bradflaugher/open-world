#!/usr/bin/env python3
"""Run the autopilot over seeds x models x modes in parallel and tabulate the results.

    python3 tools/playthrough_matrix.py [--seeds 0x1234,1,0x4e] [--modes omni,explore-walk]
                                        [--rom build/open-world.gb] [--owgen build/owgen]
                                        [--out build/playthrough] [-j 4]

Modes: omni (knows the whole map, runs: the fastest possible world), explore (knows only what
it has seen on screen and heads for the markers on the band, runs), explore-walk (the same,
never holding B: a slow, first-time player). Writes <out>/summary.json and prints a table.
"""
import argparse
import json
import os
import sys
from multiprocessing import Pool

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import autopilot  # noqa: E402

MODES = {'omni': dict(explore=False, walk=False), 'explore': dict(explore=True, walk=False),
         'explore-walk': dict(explore=True, walk=True), 'omni-walk': dict(explore=False, walk=True)}


def run_one(job):
    seed, cgb, mode, rom, owgen, out, shots = job
    sym = os.path.splitext(rom)[0] + '.sym'
    p = autopilot.Pilot(rom, sym, owgen, seed, cgb, out, log=None, shots=shots, **MODES[mode])
    try:
        r = p.play()
    finally:
        p.stop()
    keep = ('seed', 'model', 'mode', 'completed', 'frames', 'minutes', 'whiteouts', 'rests', 'stones_used',
            'watcher_spawns', 'worlds_done', 'new_seed', 'error', 'wall_seconds', 'frame_drops', 'dbg_stalls',
            'dbg_refills')
    s = {k: r.get(k) for k in keep}
    s['mode'] = mode
    s['tag'] = p.tag
    s['legs'] = [{k: L[k] for k in ('name', 'frames', 'minutes', 'whiteouts', 'rests', 'rest_frames', 'stones',
                                    'burns', 'glides', 'stuck', 'replans', 'watcher_evades', 'refused')}
                 for L in r['legs']]
    s['stuck_spots'] = r.get('stuck_spots', [])[:10]
    s['land_mismatch'] = [e for e in r.get('events', []) if e['kind'] == 'land_mismatch'][:3]
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seeds', default='0x1234,0x0001,0x004e')
    ap.add_argument('--modes', default='omni,explore-walk')
    ap.add_argument('--models', default='dmg,cgb')
    ap.add_argument('--rom', default=os.path.join(autopilot.ROOT, 'build', 'open-world.gb'))
    ap.add_argument('--owgen', default=os.path.join(autopilot.ROOT, 'build', 'owgen'))
    ap.add_argument('--out', default=os.path.join(autopilot.ROOT, 'build', 'playthrough'))
    ap.add_argument('--no-shots', action='store_true')
    ap.add_argument('-j', type=int, default=4)
    a = ap.parse_args()
    jobs = [(int(s, 0), m == 'cgb', mode, a.rom, a.owgen, a.out, not a.no_shots)
            for s in a.seeds.split(',') for m in a.models.split(',') for mode in a.modes.split(',')]
    with Pool(a.j) as pool:
        res = pool.map(run_one, jobs, chunksize=1)
    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, 'summary.json')
    old = []
    if os.path.exists(path):
        try:
            old = [r for r in json.load(open(path)) if r['tag'] not in {x['tag'] for x in res}]
        except Exception:
            old = []
    json.dump(old + res, open(path, 'w'), indent=1, default=str)
    print(f"{'tag':28s} {'ok':3s} {'total':>6s} " + ' '.join(f'{n:>8s}' for n in ('start', 'beacon0', 'beacon1',
                                                                               'beacon2', 'heart'))
          + '  whiteouts rests stones watchers stuck')
    for r in res:
        legs = {L['name']: L['minutes'] for L in r['legs']}
        print(f"{r['tag']:28s} {'yes' if r['completed'] else 'NO ':3s} {r['minutes']:6.2f} "
              + ' '.join(f"{legs.get(n, float('nan')):8.2f}" for n in ('start', 'beacon0', 'beacon1', 'beacon2',
                                                                       'heart'))
              + f"  {r['whiteouts']:9d} {r['rests']:5d} {r['stones_used']:6d} {r['watcher_spawns']:8d}"
              f" {sum(L['stuck'] for L in r['legs']):5d}" + (f"  {r['error']}" if r.get('error') else ''))
    return 0 if all(r['completed'] for r in res) else 1


if __name__ == '__main__':
    sys.exit(main())
