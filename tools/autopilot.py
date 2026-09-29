#!/usr/bin/env python3
"""OPEN WORLD autopilot: plays one whole world in PyBoy with real button inputs only.

    python3 tools/autopilot.py --seed 0x1234 [--cgb] [--rom build/open-world.gb]
                               [--owgen build/owgen] [--out build/playthrough] [--max-frames N]

What it does, as a player would:
  boot -> title -> SELECT (new world) -> wake -> light the start fire ->
  beacon 0: burn a way through the brambles, take the STONES, light it ->
  beacon 1: lay stepping stones over the shallows, take the CLOAK, light it ->
  beacon 2: glide over the crags, light it ->
  the Heart: walk up to it; the world ends and a new one starts (worlds_done must go up).

Inputs only. The only write to the game's memory is `dbg_seed`, poked once on the title
screen right before SELECT (the ROM's own test hook), so a run is reproducible for a given
seed. Everything else is read-only: the player's position, state, warmth, time, the mods
table and the Watcher, which close the control loop, as a player's eyes would.

Pathfinding: the host generator (`owgen dump`) gives the terrain of the whole region; the
ROM's mods table (read from WRAM) is laid over it. A* runs over metatiles with the item
rules (lantern burns brambles, stones cross shallows, the cloak glides 3 cells over
MTF_GLIDE cells), and the path is followed with the d-pad, closing the loop on the real
pixel position of the wanderer's foot every frame (sub-metatile steering, collision
sliding, slow ground, stuck detection and replanning).

Night: warmth drains at night away from fire. The pilot keeps walking while its warmth
covers the rest of the night; otherwise it walks to the nearest fire (lighting it if it
is cold), stands in cover next to it if there is any, lets the wanderer sit (time runs 8x)
until dawn, and steps away from Watchers.

Output (in --out/<model>_<seed>/): result.json (metrics per leg), events.log, periodic
screenshots (every 30 s of game time) and milestone screenshots, and contact.png.
"""
import argparse
import heapq
import io
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

GS_BOOT, GS_TITLE, GS_WORLD, GS_MAP, GS_ENDING, GS_WHITEOUT, GS_LESSON = range(7)
PL_SLEEP, PL_STAND, PL_WALK, PL_SIT, PL_GLIDE = range(5)
PH_DAWN, PH_DAY, PH_DUSK, PH_NIGHT = range(4)
PHASE_NAMES = ('dawn', 'day', 'dusk', 'night')
WX_NAMES = ('clear', 'rain', 'snow', 'fog', 'storm')
BIOME_NAMES = ('sea', 'shallow', 'shore', 'meadow', 'forest', 'desert', 'tundra', 'rock', 'ash', 'ruins')
IT_LANTERN, IT_STONES, IT_CLOAK = range(3)
T_DAY, T_DUSK, T_NIGHT, DAY_FRAMES = 4320, 21600, 25920, 43200
MTF_SOLID, MTF_WATER, MTF_HIDE, MTF_SLOW, MTF_COLD, MTF_WARM, MTF_INTERACT, MTF_GLIDE = (
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80)
SRAM_SIZE = 8192
FPS = 59.7275          # DMG / CGB frame rate
SHOT_EVERY = 1800      # 30 s of game time at 60 fps

# 8 directions, N first, clockwise (as the ROM's D_N .. D_NW)
DIRS = [(0, -1), (1, -1), (1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1)]
DIR_KEYS = {(0, -1): ('up',), (1, -1): ('up', 'right'), (1, 0): ('right',), (1, 1): ('down', 'right'),
            (0, 1): ('down',), (-1, 1): ('down', 'left'), (-1, 0): ('left',), (-1, -1): ('up', 'left')}
FACE_OF = {d: i for i, d in enumerate(DIRS)}


def mt_enum():
    with open(os.path.join(ROOT, 'src', 'core', 'world.h')) as f:
        src = f.read()
    i = src.index('MT_SEA')
    body = src[src.rindex('{', 0, i) + 1:src.index('}', i)]
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    names = [re.sub(r'=.*', '', n).strip() for n in body.split(',')]
    return {n: k for k, n in enumerate(n for n in names if n and n != 'MT_COUNT')}


MT = mt_enum()


class Stuck(Exception):
    pass


class Interrupt(Exception):
    """The plan being followed is no longer valid (whiteout, night, a Watcher, a detour)."""

    def __init__(self, why):
        super().__init__(why)
        self.why = why


# --------------------------------------------------------------------------------- terrain
class Terrain:
    """Base metatiles of a rectangular region from the host generator + the ROM's mods."""

    def __init__(self, owgen, seed, x0, y0, w, h, flags):
        self.x0, self.y0, self.w, self.h = x0, y0, w, h
        out = subprocess.check_output([owgen, 'dump', str(seed), str(x0), str(y0), str(w), str(h)], text=True)
        rows = out.split('\n')[:h]
        self.base = bytearray()
        for r in rows:
            self.base += bytes.fromhex(r.strip())
        assert len(self.base) == w * h, 'owgen dump size'
        self.cur = bytearray(self.base)
        self.mods = {}
        self.flags = flags
        # explore mode: the pilot only knows what it has seen; unknown cells look like grass
        self.explore = False
        self.known = bytearray(w * h)
        self.view = bytearray([MT['MT_GRASS']]) * (w * h)

    def arr(self):
        return self.view if self.explore else self.cur

    def mark(self, x0, y0, w, h):
        """Cells seen on screen: now known. Returns the newly known indices."""
        new = []
        for y in range(y0, y0 + h):
            yy = y - self.y0
            if not 0 <= yy < self.h:
                continue
            for x in range(x0, x0 + w):
                xx = x - self.x0
                if 0 <= xx < self.w:
                    i = yy * self.w + xx
                    if not self.known[i]:
                        self.known[i] = 1
                        self.view[i] = self.cur[i]
                        new.append(i)
        return new

    def idx(self, x, y):
        x -= self.x0
        y -= self.y0
        if 0 <= x < self.w and 0 <= y < self.h:
            return y * self.w + x
        return -1

    def xy(self, i):
        return self.x0 + i % self.w, self.y0 + i // self.w

    def mt(self, x, y):
        i = self.idx(x, y)
        return self.arr()[i] if i >= 0 else MT['MT_SEA']

    def set_mods(self, mods):
        for (x, y) in self.mods:
            i = self.idx(x, y)
            if i >= 0:
                self.cur[i] = self.base[i]
                if self.known[i]:
                    self.view[i] = self.base[i]
        self.mods = dict(mods)
        for (x, y), m in mods.items():
            i = self.idx(x, y)
            if i >= 0:
                self.cur[i] = m
                self.known[i] = 1
                self.view[i] = m

    def solid(self, x, y):
        return self.flags[self.mt(x, y)] & MTF_SOLID


# --------------------------------------------------------------------------------- planning
COST_ORTH, COST_DIAG = 8, 11          # frames per cell at run speed (2 px/frame; 1.5 diagonally)
COST_BURN, COST_STONE, COST_GLIDE = 70, 50, 60
STONE_PENALTY = 60


def plan(ter, start, goals, items, stones=12, avoid=None, limit=None, heuristic=True):
    """A* over metatiles. start (x, y); goals: set of (x, y). Returns [(x, y, kind)] where
    kind is how the cell is entered: 'walk', 'burn' (a bramble, from the previous cell),
    'stone' (shallows) or 'glide' (3 cells in one direction). None if unreachable."""
    W, H = ter.w, ter.h
    cur, fl = ter.arr(), ter.flags
    lantern = items & (1 << IT_LANTERN)
    has_stones = (items & (1 << IT_STONES)) and stones > 0
    cloak = items & (1 << IT_CLOAK)
    BR, SH, ROAD = MT['MT_BRAMBLE'], MT['MT_SHALLOW'], MT['MT_ROAD']
    road_bias = ter.explore
    s = ter.idx(*start)
    gidx = {ter.idx(*g) for g in goals} - {-1}
    if s < 0 or not gidx:
        return None
    gx = [i % W for i in gidx]
    gy = [i // W for i in gidx]
    single = len(gidx) <= 16
    avoid = avoid or {}

    def h(i):
        if not heuristic:
            return 0
        x, y = i % W, i // W
        if single:
            best = 1 << 30
            for a, b in zip(gx, gy):
                dx, dy = abs(a - x), abs(b - y)
                v = COST_ORTH * max(dx, dy) + (COST_DIAG - COST_ORTH) * min(dx, dy)
                if v < best:
                    best = v
            return best
        dx = max(0, min(gx) - x, x - max(gx))
        dy = max(0, min(gy) - y, y - max(gy))
        return COST_ORTH * max(dx, dy) + (COST_DIAG - COST_ORTH) * min(dx, dy)

    def walkable(i):
        return not (fl[cur[i]] & MTF_SOLID)

    g = {s: 0}
    came = {s: (None, 'walk')}
    openq = [(h(s), 0, s)]
    while openq:
        f, gc, i = heapq.heappop(openq)
        if gc > g.get(i, 1 << 30):
            continue
        if i in gidx:
            path = []
            while i is not None:
                p, k = came[i]
                path.append((i % W + ter.x0, i // W + ter.y0, k))
                i = p
            path.reverse()
            return path[1:]
        if limit and gc > limit:
            continue
        x, y = i % W, i // W
        slow = fl[cur[i]] & MTF_SLOW
        for d, (dx, dy) in enumerate(DIRS):
            nx, ny = x + dx, y + dy
            if not (0 <= nx < W and 0 <= ny < H):
                continue
            j = ny * W + nx
            diag = dx and dy
            m = cur[j]
            kind = None
            if not (fl[m] & MTF_SOLID):
                if diag and not (walkable(y * W + nx) and walkable(ny * W + x)):
                    kind = None
                else:
                    kind = 'walk'
                    c = COST_DIAG if diag else COST_ORTH
                    if slow:
                        c *= 2
                    if m == ROAD and road_bias:
                        c -= 2          # a first-time player follows the causeway
            elif not diag and m == BR and lantern:
                kind, c = 'burn', COST_ORTH + COST_BURN
            elif not diag and m == SH and has_stones:
                kind, c = 'stone', COST_ORTH + COST_STONE + STONE_PENALTY * (3 if ter.explore else 1)
            if kind:
                c += avoid.get(j, 0)
                ng = gc + c
                if ng < g.get(j, 1 << 30):
                    g[j] = ng
                    came[j] = (i, kind)
                    heapq.heappush(openq, (ng + h(j), ng, j))
            # glide: cells 1 and 2 must be glidable or open, cell 3 open
            if cloak:
                ok = True
                for k in (1, 2):
                    ax, ay = x + dx * k, y + dy * k
                    if not (0 <= ax < W and 0 <= ay < H):
                        ok = False
                        break
                    mm = cur[ay * W + ax]
                    if not (fl[mm] & MTF_GLIDE) and (fl[mm] & MTF_SOLID):
                        ok = False
                        break
                if not ok:
                    continue
                lx, ly = x + 3 * dx, y + 3 * dy
                if not (0 <= lx < W and 0 <= ly < H):
                    continue
                j = ly * W + lx
                if fl[cur[j]] & MTF_SOLID:
                    continue
                # only useful glides (over something solid): keeps the search small
                if not any(fl[cur[(y + dy * k) * W + x + dx * k]] & MTF_SOLID for k in (1, 2)):
                    continue
                c = COST_GLIDE + (15 if diag else 0) + avoid.get(j, 0)
                ng = gc + c
                if ng < g.get(j, 1 << 30):
                    g[j] = ng
                    came[j] = (i, 'glide')
                    heapq.heappush(openq, (ng + h(j), ng, j))
    return None


def dist_field(ter, sources, items):
    """Dijkstra (walking only, with the lantern's and stones' shortcuts ignored) from the
    source cells; returns {idx: frames}."""
    W, H = ter.w, ter.h
    cur, fl = ter.arr(), ter.flags
    dist = {}
    q = []
    for (x, y) in sources:
        i = ter.idx(x, y)
        if i >= 0 and not (fl[cur[i]] & MTF_SOLID):
            dist[i] = 0
            q.append((0, i))
    heapq.heapify(q)
    while q:
        d, i = heapq.heappop(q)
        if d > dist.get(i, 1 << 30):
            continue
        x, y = i % W, i // W
        for dx, dy in DIRS:
            nx, ny = x + dx, y + dy
            if not (0 <= nx < W and 0 <= ny < H):
                continue
            j = ny * W + nx
            if fl[cur[j]] & MTF_SOLID:
                continue
            if dx and dy and ((fl[cur[y * W + nx]] & MTF_SOLID) or (fl[cur[ny * W + x]] & MTF_SOLID)):
                continue
            nd = d + (COST_DIAG if dx and dy else COST_ORTH)
            if nd < dist.get(j, 1 << 30):
                dist[j] = nd
                heapq.heappush(q, (nd, j))
    return dist


# --------------------------------------------------------------------------------- the pilot
class Pilot:
    def __init__(self, rom, sym, owgen, seed, cgb, out, sram=None, max_frames=500000, log=print,
                 shots=True, seed_mode='poke', explore=False, walk=False, tag=''):
        from pyboy import PyBoy
        self.rom, self.sym, self.owgen = rom, sym, owgen
        self.seed, self.cgb = seed, cgb
        self.explore = explore
        self.run_ok = not walk
        self.tag = f"{'cgb' if cgb else 'dmg'}_{seed:04x}" + ('_explore' if explore else '') + ('_walk' if walk else '') + tag
        self.out = os.path.join(out, self.tag)
        self.shots = shots
        os.makedirs(self.out, exist_ok=True)
        if shots:
            for f in os.listdir(self.out):
                if f.endswith('.png'):
                    os.remove(os.path.join(self.out, f))
        self.ram = io.BytesIO(sram if sram is not None else bytes(SRAM_SIZE))
        self.pb = PyBoy(rom, window='null', cgb=cgb, symbols=sym, sound_emulated=False, ram_file=self.ram)
        self.pb.set_emulation_speed(0)
        self.mem = self.pb.memory
        self.sym_cache = {}
        self.held = set()
        self.frame_n = 0
        self.max_frames = max_frames
        self.logf = open(os.path.join(self.out, 'events.log'), 'w')
        self.echo = log
        self.shot_list = []
        self.seed_mode = seed_mode
        self.ter = None
        self.fire_dist = None
        self.fire_dist_key = None
        self.leg = None
        self.legs = []
        self.samples = []            # (frame, phase, weather, biome, amb mode, warmth)
        self.events = []
        self.stuck_spots = []
        self.watcher_spawns = 0
        self.watcher_near = 0
        self._watch_prev = 0
        self.flags = None
        self.last_rest_frame = -99999
        self.stuck_block = set()
        self.hooks = {}
        self.trace = []
        self.stall_stats = {} if self.has('dbg_stalls') else None
        self.avoid_watch = None

    # ---- memory
    def addr(self, name):
        a = self.sym_cache.get(name)
        if a is None:
            a = self.pb.symbol_lookup('_' + name)[1]
            self.sym_cache[name] = a
        return a

    def u8(self, name, off=0):
        return self.mem[self.addr(name) + off]

    def u16(self, name, off=0):
        a = self.addr(name) + off
        return self.mem[a] | self.mem[a + 1] << 8

    def has(self, name):
        try:
            self.addr(name)
            return True
        except Exception:
            return False

    def state(self):
        return self.u8('game_state')

    def foot(self):
        """Foot point in world pixels."""
        return self.u16('pl_mx') * 16 + self.u8('pl_sx'), self.u16('pl_my') * 16 + self.u8('pl_sy')

    def cell(self):
        return self.u16('pl_mx'), self.u16('pl_my')

    def mods(self):
        n = self.u8('world_mod_count')
        a = self.addr('world_mods')
        m = self.mem
        return {(m[a + i * 5] | m[a + i * 5 + 1] << 8, m[a + i * 5 + 2] | m[a + i * 5 + 3] << 8): m[a + i * 5 + 4]
                for i in range(n)}

    def land(self, x, y):
        """The metatile the ROM has streamed for (x, y), or None outside its window."""
        x0, y0 = self.u16('land_x0'), self.u16('land_y0')
        if 0 <= (x - x0) & 0xFFFF < 15 and 0 <= (y - y0) & 0xFFFF < 15:
            return self.u8('land_cache', ((y & 15) << 4) | (x & 15))
        return None

    def world_layout(self):
        a = self.addr('world')
        w = [self.mem[a + i] | self.mem[a + i + 1] << 8 for i in range(0, 34, 2)]
        return {'seed': w[0], 'start': (w[1], w[2]),
                'beacon': [(w[3 + 2 * i], w[4 + 2 * i]) for i in range(3)],
                'shrine': [(w[9 + 2 * i], w[10 + 2 * i]) for i in range(3)],
                'heart': (w[15], w[16])}

    # ---- logging
    def log(self, msg):
        line = f'[{self.frame_n:7d} {self.frame_n / FPS / 60:6.2f}m] {msg}'
        self.logf.write(line + '\n')
        self.logf.flush()
        if self.echo:
            self.echo(f'{self.tag} {line}')

    def event(self, kind, **kw):
        kw.update(kind=kind, frame=self.frame_n, leg=self.leg['name'] if self.leg else None)
        self.events.append(kw)
        self.log(f'{kind} ' + ' '.join(f'{k}={v}' for k, v in kw.items() if k not in ('kind', 'frame', 'leg')))

    # ---- time and input
    def buttons(self, want):
        want = set(want)
        for b in self.held - want:
            self.pb.button_release(b)
        for b in want - self.held:
            self.pb.button_press(b)
        self.held = want

    def tick(self, n=1, render=False):
        for _ in range(n):
            self.frame_n += 1
            snap = self.shots and self.frame_n % SHOT_EVERY == 0
            self.pb.tick(1, render or snap, False)
            if snap:
                self.save_shot(f'{self.frame_n // SHOT_EVERY * 30 // 60:03d}m{self.frame_n // SHOT_EVERY * 30 % 60:02d}s', periodic=True)
            if self.frame_n % 600 == 0:
                self.sample()
            if self.check_hitbox:
                self.hitbox_check()
            if self.frame_n > self.max_frames:
                raise RuntimeError(f'frame budget exhausted ({self.max_frames})')

    check_hitbox = True
    hitbox_bad = 0
    torn_reads = 0
    hole_pending = None

    def hitbox_check(self):
        """The wanderer's hitbox ([-5, 4] x [-5, 0] round the foot) must never overlap a solid
        cell of the ROM's own land cache while walking; and the land cache must agree with the
        host generator + mods under the wanderer (streaming desync)."""
        m = self.mem
        if m[self.addr('game_state')] != GS_WORLD or m[self.addr('pl_state')] not in (PL_STAND, PL_WALK, PL_SIT):
            return
        mx, my = self.u16('pl_mx'), self.u16('pl_my')
        sx, sy = m[self.addr('pl_sx')], m[self.addr('pl_sy')]
        self.trace.append((self.frame_n, mx, my, sx, sy, '+'.join(sorted(self.held))))
        if len(self.trace) > 12:
            self.trace.pop(0)
        cells = {(mx + ((sx - 5) >> 4), my + ((sy - 5) >> 4)), (mx + ((sx + 4) >> 4), my + ((sy - 5) >> 4)),
                 (mx + ((sx - 5) >> 4), my + (sy >> 4)), (mx + ((sx + 4) >> 4), my + (sy >> 4))}
        if self.hole_pending:
            # a position read torn by a late VBlank ISR (pl_mx updated, pl_sx not yet) jumps by
            # ~16 px and comes back the next frame: only report holes that are really there
            f0, (px, py), info = self.hole_pending
            self.hole_pending = None
            fx, fy = mx * 16 + sx, my * 16 + sy
            pv = self.trace[-3] if len(self.trace) >= 3 else None
            if pv and abs(fx - (pv[1] * 16 + pv[3])) <= 4 and abs(fy - (pv[2] * 16 + pv[4])) <= 4 and \
                    (abs(px - fx) > 6 or abs(py - fy) > 6):
                self.torn_reads += 1
                if self.torn_reads <= 5:
                    self.event('torn_read', at=f0, read=info['foot'], next=(mx, my, sx, sy))
            else:
                self.hitbox_bad += 1
                if self.hitbox_bad <= 5:
                    self.event('collision_hole', **info)
                    self.milestone('collision_hole')
        for c in cells:
            v = self.land(*c)
            if v is not None and self.flags[v] & MTF_SOLID:
                if not self.hole_pending:
                    self.hole_pending = (self.frame_n, (mx * 16 + sx, my * 16 + sy), dict(
                        cell=c, mt=v, foot=(mx, my, sx, sy), state=m[self.addr('pl_state')], trace=list(self.trace),
                        around=[[self.land(c[0] + i, c[1] + j) for i in (-1, 0, 1, 2)] for j in (-1, 0, 1, 2)]))
                return
        return
        for c in cells:
            v = self.land(*c)
            if v is not None and self.flags[v] & MTF_SOLID:
                self.hitbox_bad += 1
                if self.hitbox_bad <= 5:
                    self.event('collision_hole', cell=c, mt=v, foot=(mx, my, sx, sy), state=m[self.addr('pl_state')],
                               trace=list(self.trace),
                               around=[[self.land(c[0] + i, c[1] + j) for i in (-1, 0, 1, 2)] for j in (-1, 0, 1, 2)])
                    self.milestone('collision_hole')
                return

    def milestone(self, label):
        """Render one frame and keep it as a milestone screenshot."""
        self.pb.tick(1, True, False)
        self.frame_n += 1
        if self.shots:
            self.save_shot(label, periodic=False)

    def save_shot(self, label, periodic):
        name = f'{self.frame_n:07d}_{label}.png'
        path = os.path.join(self.out, name)
        self.pb.screen.image.save(path)
        self.shot_list.append((self.frame_n, label, path, periodic))

    def press(self, button, hold=2, after=2):
        self.buttons(self.held | {button})
        self.tick(hold)
        self.buttons(self.held - {button})
        self.tick(after)

    def sample(self):
        if self.state() != GS_WORLD:
            return
        amb = self.u8('snd_cur_mode') if self.has('snd_cur_mode') else -1
        self.samples.append((self.frame_n, self.u8('phase'), self.u8('weather'), self.u8('biome_here'), amb,
                             self.u16('warmth'), self.u16('tod')))

    # ---- boot
    def boot_new_world(self):
        for _ in range(3000):
            if self.state() == GS_TITLE:
                break
            self.tick()
        assert self.state() == GS_TITLE, 'never reached the title'
        self.tick(60)
        self.milestone('title')
        if self.seed_mode == 'poke':
            a = self.addr('dbg_seed')
            self.mem[a] = self.seed & 0xFF          # the one write: the ROM's own seed hook
            self.mem[a + 1] = self.seed >> 8
        self.press('select')
        for _ in range(4000):
            if self.state() == GS_WORLD and self.u8('pal_fade') == 0:
                break
            self.tick()
        assert self.state() == GS_WORLD, 'no world after SELECT'
        w = self.world_layout()
        if self.seed_mode == 'poke':
            assert w['seed'] == self.seed, f"seed {w['seed']:#x} != {self.seed:#x}"
        else:
            self.seed = w['seed']
        self.layout = w
        self.log(f'world seed {w["seed"]:#06x} layout {w}')
        self.flags = bytes(self.mem[self.addr('mt_flags') + i] for i in range(len(MT)))
        self.load_terrain()

    def load_terrain(self):
        w = self.layout
        pts = [w['start'], w['heart']] + w['beacon'] + w['shrine']
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        m = 56
        x0, y0 = min(xs) - m, min(ys) - m
        self.ter = Terrain(self.owgen, w['seed'], x0, y0, max(xs) + m - x0, max(ys) + m - y0, self.flags)
        self.ter.explore = self.explore
        self.ter.set_mods(self.mods())
        self.log(f'terrain {self.ter.w}x{self.ter.h} at ({x0},{y0})')

    def check_land_vs_host(self, what):
        """The ROM's streamed window must equal host + mods (a desync would break the pilot)."""
        x0, y0 = self.u16('land_x0'), self.u16('land_y0')
        self.ter.set_mods(self.mods())
        bad = []
        for j in range(15):
            for i in range(15):
                x, y = x0 + i, y0 + j
                r = self.land(x, y)
                i = self.ter.idx(x, y)
                h = self.ter.cur[i] if i >= 0 else None
                if r != h and i >= 0:
                    bad.append((x, y, r, h))
        if bad:
            self.event('land_mismatch', where=what, n=len(bad), first=bad[:3])
        return bad

    # ---- leg bookkeeping
    def leg_begin(self, name):
        self.leg = {'name': name, 'f0': self.frame_n, 'whiteouts': 0, 'rests': 0, 'rest_frames': 0,
                    'stones': 0, 'burns': 0, 'glides': 0, 'stuck': 0, 'replans': 0, 'watcher_evades': 0,
                    'refused': 0}
        self.log(f'=== leg {name}')

    def leg_end(self):
        L = self.leg
        L['frames'] = self.frame_n - L['f0']
        L['minutes'] = round(L['frames'] / FPS / 60, 2)
        self.legs.append(L)
        self.log(f"=== leg {L['name']} done: {L['frames']} frames = {L['minutes']} min; whiteouts "
                 f"{L['whiteouts']} rests {L['rests']} stones {L['stones']} stuck {L['stuck']}")
        self.leg = None

    # ---- world checks each frame
    def watch(self):
        """Watcher position relative to the foot (px), or None."""
        on = self.u8('watch_on')
        if on and not self._watch_prev:
            self.watcher_spawns += 1
            self.event('watcher', phase=PHASE_NAMES[self.u8('phase') & 3], biome=self.u8('biome_here'))
        self._watch_prev = on
        if not on:
            return None
        fx, fy = self.foot()
        wx = self.u16('watch_mx') * 16 + self.u8('watch_sx')
        wy = self.u16('watch_my') * 16 + self.u8('watch_sy')
        return wx - fx, wy - fy, self.u16('watch_mx'), self.u16('watch_my')

    def guard(self, allow_night=True):
        """Raise Interrupt when something needs attention. Called every frame of travel."""
        st = self.state()
        if st == GS_WHITEOUT or (st == GS_WORLD and self.u8('pl_state') == PL_SLEEP):
            raise Interrupt('whiteout')
        if st != GS_WORLD:
            raise Interrupt(f'state{st}')
        w = self.watch()
        if w is not None:
            rx, ry, wmx, wmy = w
            if abs(rx) < 72 and abs(ry) < 72 and (wmx, wmy) != self.avoid_watch:
                raise Interrupt('watcher')
        if allow_night and self.frame_n % 120 == 0 and self.need_rest():
            raise Interrupt('night')

    # ---- night
    def drain_rate(self):
        """Warmth lost per frame at night here (the ROM's warmth_tick, /64)."""
        b = self.u8('biome_here')
        r = 10 if b in (5, 6) else 5
        m = self.land(*self.cell())
        if m is not None and self.flags[m] & MTF_COLD:
            r += 5
        wx = self.u8('weather')
        if wx in (1, 4):
            r += 3
        if wx == 2:
            r += 5
        return r / 64.0

    def fire_sources(self):
        F = {MT['MT_FIRE_COLD'], MT['MT_FIRE_LIT'], MT['MT_BEACON_LIT']}
        cur, ter = self.ter.arr(), self.ter
        out = []
        for i, m in enumerate(cur):
            if m in F:
                out.append(ter.xy(i))
        return out

    def fire_distance(self):
        key = (tuple(sorted(self.ter.mods.items())), self.u8('items'))
        if self.fire_dist_key != key:
            srcs = []
            for (x, y) in self.fire_sources():
                for dx in range(-2, 3):
                    for dy in range(-2, 3):
                        srcs.append((x + dx, y + dy))
            self.fire_dist = dist_field(self.ter, srcs, self.u8('items'))
            self.fire_dist_key = key
        return self.fire_dist.get(self.ter.idx(*self.cell()), 1 << 20)

    def need_rest(self):
        ph = self.u8('phase')
        tod = self.u16('tod')
        if ph not in (PH_DUSK, PH_NIGHT):
            return False
        if self.u8('near_warm'):
            return self.frame_n - self.last_rest_frame > 600 and ph == PH_NIGHT and \
                self.u16('warmth') < 900 or (ph == PH_DUSK and tod > T_DUSK + 2500)
        self.ter.set_mods(self.mods())
        rate = max(self.drain_rate(), 5 / 64.0) * 1.15
        grace = max(0, T_NIGHT - tod) if ph == PH_DUSK else 0
        warm_frames = grace + self.u16('warmth') / rate
        night_left = DAY_FRAMES - max(tod, T_NIGHT) + grace
        if warm_frames > night_left * 1.1 + 300:
            return False                      # walking on survives the night
        d = self.fire_distance()
        if d >= 1 << 20:
            return False                      # no fire we know of: press on
        return warm_frames < d * 1.4 + 900

    def rest(self):
        """Walk to the nearest fire (light it if cold), stand in cover beside it, sit until dawn."""
        L = self.leg
        L['rests'] += 1
        f0 = self.frame_n
        self.last_rest_frame = self.frame_n
        self.ter.set_mods(self.mods())
        self.event('rest_begin', tod=self.u16('tod'), warmth=self.u16('warmth'), cell=self.cell())
        # nearest fire by path cost
        here = self.cell()
        best = None
        for (x, y) in self.fire_sources():
            if max(abs(x - here[0]), abs(y - here[1])) > 120:
                continue
            adj = {(x + dx, y + dy) for dx, dy in DIRS[::2] if not self.ter.solid(x + dx, y + dy)}
            if not adj:
                continue
            p = plan(self.ter, here, adj, self.u8('items'), self.u8('stones'), limit=4000)
            if p is not None:
                c = sum(COST_ORTH for _ in p)
                if best is None or c < best[0]:
                    best = (c, (x, y), adj)
        if best is None:
            self.event('rest_nofire')
            return
        _, fire, adj = best
        self.goto(adj, interact=fire if self.ter.mt(*fire) == MT['MT_FIRE_COLD'] else None, allow_night=False)
        self.ter.set_mods(self.mods())
        if self.ter.mt(*fire) == MT['MT_FIRE_COLD']:
            self.interact(fire, 'light fire')
            self.milestone(f'fire_{fire[0]}_{fire[1]}')
        # a resting cell in cover (tall grass / undergrowth) within 2 of the fire
        spots = []
        for dx in range(-2, 3):
            for dy in range(-2, 3):
                x, y = fire[0] + dx, fire[1] + dy
                m = self.ter.mt(x, y)
                if not self.flags[m] & MTF_SOLID:
                    spots.append((0 if self.flags[m] & MTF_HIDE else 1, max(abs(dx), abs(dy)), (x, y)))
        spots.sort()
        if spots and spots[0][2] != self.cell():
            self.goto({spots[0][2]}, allow_night=False)
        hidden = spots and self.flags[self.ter.mt(*spots[0][2])] & MTF_HIDE
        self.event('rest_sit', fire=fire, cover=bool(hidden), tod=self.u16('tod'))
        shot = False
        while True:
            self.buttons(())
            self.tick()
            st = self.state()
            if st == GS_WHITEOUT or self.u8('pl_state') == PL_SLEEP:
                self.handle_whiteout()
                break
            ph = self.u8('phase')
            if ph in (PH_DAWN, PH_DAY) and self.u16('tod') < T_DUSK - 600:
                break
            if not shot and self.u8('pl_state') == PL_SIT and ph == PH_NIGHT:
                self.milestone('rest_night')
                shot = True
            w = self.watch()
            if w is not None and abs(w[0]) < 56 and abs(w[1]) < 56 and not hidden:
                self.evade(w)
                if self.cell() != spots[0][2]:
                    try:
                        self.goto({spots[0][2]}, allow_night=False)
                    except Interrupt:
                        pass
        L['rest_frames'] += self.frame_n - f0
        self.event('rest_end', frames=self.frame_n - f0, tod=self.u16('tod'), warmth=self.u16('warmth'))

    def evade(self, w):
        """Step away from a Watcher until it has gone (they vanish when far behind)."""
        self.leg['watcher_evades'] += 1
        rx, ry, wmx, wmy = w
        self.event('evade', rel=(rx, ry))
        here = self.cell()
        best = None
        for r in range(8, 13):
            for dx in range(-r, r + 1):
                for dy in (-r, r):
                    for (x, y) in ((here[0] + dx, here[1] + dy), (here[0] + dy, here[1] + dx)):
                        if self.ter.solid(x, y):
                            continue
                        score = max(abs(x - wmx) * 16 / 150.0, abs(y - wmy) * 16 / 110.0)
                        if best is None or score > best[0]:
                            best = (score, (x, y))
        if best is None:
            return
        avoid = self.watch_avoid(wmx, wmy)
        p = plan(self.ter, here, {best[1]}, self.u8('items'), 0, avoid=avoid)
        if p:
            try:
                self.follow(p, allow_night=False, watch=False)
            except (Interrupt, Stuck):
                pass
        for _ in range(600):
            if not self.u8('watch_on') or self.state() != GS_WORLD:
                break
            self.buttons(())
            self.tick()

    def watch_avoid(self, wmx, wmy):
        avoid = {}
        for dx in range(-3, 4):
            for dy in range(-3, 4):
                i = self.ter.idx(wmx + dx, wmy + dy)
                if i >= 0:
                    avoid[i] = 4000 if max(abs(dx), abs(dy)) <= 2 else 400
        return avoid

    def handle_whiteout(self):
        self.leg['whiteouts'] += 1
        self.event('whiteout', warmth=self.u16('warmth'), tod=self.u16('tod'), cell=self.cell())
        self.buttons(())
        for _ in range(3000):
            if self.state() == GS_WORLD and self.u8('pal_fade') == 0 and self.u8('pl_state') == PL_SLEEP:
                break
            self.tick()
        self.milestone('whiteout_wake')
        self.wake()

    def wake(self):
        if self.u8('pl_state') != PL_SLEEP:
            return
        self.tick(45)
        for _ in range(10):
            self.press('a', 2, 4)
            if self.u8('pl_state') != PL_SLEEP:
                break
        assert self.u8('pl_state') != PL_SLEEP, 'could not wake'

    # ---- movement
    def steer(self, tx, ty, tol, run=True, watch=True, allow_night=True, limit=900):
        """Drive the foot to pixel (tx, ty) within tol. Raises Stuck / Interrupt."""
        last = self.foot()
        still = 0
        limit = max(limit, 3 * (abs(tx - last[0]) + abs(ty - last[1])))
        self._stall_seen = self.u16('dbg_stalls') if self.stall_stats is not None else 0
        for _ in range(limit):
            fx, fy = self.foot()
            ex, ey = tx - fx, ty - fy
            if abs(ex) <= tol and abs(ey) <= tol:
                return
            keys = set()
            if ex > 1 or (ex > 0 and tol == 0):
                keys.add('right')
            elif ex < -1 or (ex < 0 and tol == 0):
                keys.add('left')
            if ey > 1 or (ey > 0 and tol == 0):
                keys.add('down')
            elif ey < -1 or (ey < 0 and tol == 0):
                keys.add('up')
            if not keys:
                # inside +-1 but outside tol 0: nudge the larger axis
                keys.add(('right' if ex > 0 else 'left') if abs(ex) >= abs(ey) else ('down' if ey > 0 else 'up'))
            if run and self.run_ok and max(abs(ex), abs(ey)) > 5:
                keys.add('b')
            if still > 3 and len(keys - {'b'}) == 2:
                # diagonal blocked on one side: go one axis at a time (as a player would)
                ax = [k for k in keys if k in ('left', 'right')]
                ay = [k for k in keys if k in ('up', 'down')]
                keys = set((ax if (still // 4) % 2 else ay) + (['b'] if 'b' in keys else []))
            self.buttons(keys)
            st0 = self.u16('dbg_stalls') if self.stall_stats is not None else 0
            self.tick()
            if self.stall_stats is not None:
                k = '+'.join(sorted(keys))
                a = self.stall_stats.setdefault(k, [0, 0])
                a[0] += 1
                a[1] += (self.u16('dbg_stalls') - st0) & 0xFFFF
            if self.ter.explore and self.frame_n % 4 == 0:
                self.observe()
            if watch:
                self.guard(allow_night)
            elif self.state() != GS_WORLD or self.u8('pl_state') == PL_SLEEP:
                raise Interrupt('whiteout')
            p = self.foot()
            if p == last and self.stall_stats is not None and self.u16('dbg_stalls') != self._stall_seen:
                self._stall_seen = self.u16('dbg_stalls')      # frozen by the streamer, not by a wall
            elif p == last:
                still += 1
                if still > 24:
                    raise Stuck((fx, fy, ex, ey))
            else:
                still = 0
                last = p
        raise Stuck(('limit', self.foot(), tx, ty))

    def observe(self):
        """Explore mode: learn the cells on screen; replan if they break the current path."""
        cx, cy = self.u16('cam_mx'), self.u16('cam_my')
        new = self.ter.mark(cx, cy, 11, 9)
        if not new or not self.path_cur:
            return
        new = set(new)
        ter, fl = self.ter, self.flags
        path, i0 = self.path_cur, self.path_i
        prev = self.cell() if i0 == 0 else path[i0 - 1][:2]
        for (x, y, kind) in path[i0:]:
            cells = [(x, y)]
            dx, dy = x - prev[0], y - prev[1]
            if kind == 'glide':
                sx, sy = (dx > 0) - (dx < 0), (dy > 0) - (dy < 0)
                cells += [(prev[0] + sx, prev[1] + sy), (prev[0] + 2 * sx, prev[1] + 2 * sy)]
            elif dx and dy:
                cells += [(prev[0] + dx, prev[1]), (prev[0], prev[1] + dy)]
            if any(ter.idx(*c) in new for c in cells):
                m = ter.mt(x, y)
                bad = False
                if kind == 'walk':
                    bad = any(fl[ter.mt(*c)] & MTF_SOLID for c in cells)
                elif kind == 'burn':
                    bad = m != MT['MT_BRAMBLE']
                elif kind == 'stone':
                    bad = m != MT['MT_SHALLOW']
                else:
                    bad = bool(fl[m] & MTF_SOLID) or any(
                        not (fl[ter.mt(*c)] & MTF_GLIDE) and fl[ter.mt(*c)] & MTF_SOLID for c in cells[1:])
                if bad:
                    raise Interrupt('replan')
            prev = (x, y)

    def observe_all(self):
        self.ter.mark(self.u16('cam_mx'), self.u16('cam_my'), 11, 9)

    path_cur = None
    path_i = 0

    @staticmethod
    def center(x, y):
        return x * 16 + 8, y * 16 + 11

    def face(self, d):
        want = FACE_OF[d]
        for _ in range(6):
            if self.u8('pl_face') == want:
                break
            self.buttons(DIR_KEYS[d])
            self.tick()
        self.buttons(())
        self.tick(2)
        return self.u8('pl_face') == want

    def equip(self, it):
        for _ in range(4):
            if self.u8('equipped') == it:
                return True
            self.press('select', 2, 3)
        return self.u8('equipped') == it

    def interact(self, target, what):
        """Face the target from a 4-neighbour and press A."""
        cx, cy = self.cell()
        d = (target[0] - cx, target[1] - cy)
        if d not in FACE_OF:
            d = ((d[0] > 0) - (d[0] < 0), (d[1] > 0) - (d[1] < 0))
        self.steer(*self.center(cx, cy), tol=2, run=False, watch=False)
        self.face(d)
        before = self.mods().get(target)
        self.press('a', 2, 12)
        after = self.mods().get(target)
        self.event('interact', what=what, target=target, before=before, after=after)
        self.close_lesson()
        return after != before

    def close_lesson(self):
        """The first time an item is taken, a short wordless lesson plays. It closes with A
        once it has played through (a bobbing A appears in the corner)."""
        for _ in range(10):
            if self.state() == GS_LESSON:
                break
            self.tick()
        if self.state() != GS_LESSON:
            return
        f0 = self.frame_n
        for _ in range(3000):
            if self.u8('lesson_ready'):
                break
            self.tick()
        self.press('a', 2, 2)
        for _ in range(3000):
            if self.state() == GS_WORLD and self.u8('pal_fade') == 0:
                break
            self.tick()
        assert self.state() == GS_WORLD, f'the lesson did not close: state {self.state()} ready {self.u8("lesson_ready")} held {self.held} fade {self.u8("pal_fade")}'
        self.event('lesson', frames=self.frame_n - f0)

    def follow(self, path, allow_night=True, watch=True):
        """Follow a plan from plan(). Straight runs of walking are merged; special cells
        (burn, stone, glide) are done from the centre of the cell before them."""
        i = 0
        n = len(path)
        self.path_cur = path
        while i < n:
            self.path_i = i
            x, y, kind = path[i]
            if kind == 'walk':
                # extend the straight run
                cx, cy = self.cell()
                px, py = (path[i - 1][0], path[i - 1][1]) if i else (cx, cy)
                d = (x - px, y - py)
                j = i
                while j + 1 < n and path[j + 1][2] == 'walk' and \
                        (path[j + 1][0] - path[j][0], path[j + 1][1] - path[j][1]) == d:
                    j += 1
                ex, ey = path[j][0], path[j][1]
                final = j + 1 >= n
                special = not final and path[j + 1][2] != 'walk'
                tol = 1 if special else (2 if final else 3)
                self.steer(*self.center(ex, ey), tol=tol, watch=watch, allow_night=allow_night)
                i = j + 1
                continue
            cx, cy = self.cell()
            if kind == 'burn':
                self.leg['burns'] += 1
                self.equip(IT_LANTERN)
                self.steer(*self.center(cx, cy), tol=1, run=False, watch=watch, allow_night=False)
                self.face((x - cx, y - cy))
                self.press('a', 2, 2)
                for _ in range(90):
                    self.tick()
                    if self.land(x, y) == MT['MT_ASH']:
                        break
                ok = self.land(x, y) == MT['MT_ASH']
                self.event('burn', cell=(x, y), ok=ok)
                if not ok:
                    self.leg['refused'] += 1
                    raise Interrupt('burn failed')
                self.ter.set_mods(self.mods())
                self.steer(*self.center(x, y), tol=2, run=False, watch=watch, allow_night=False)
                i += 1
                continue
            if kind == 'stone':
                self.equip(IT_STONES)
                self.steer(*self.center(cx, cy), tol=1, run=False, watch=watch, allow_night=False)
                self.face((x - cx, y - cy))
                self.press('a', 2, 4)
                ok = self.land(x, y) == MT['MT_STEPSTONE']
                self.event('stone', cell=(x, y), ok=ok, left=self.u8('stones'))
                if not ok:
                    self.leg['refused'] += 1
                    raise Interrupt('stone failed')
                self.leg['stones'] += 1
                self.tick(3)
                self.ter.set_mods(self.mods())
                self.steer(*self.center(x, y), tol=2, run=False, watch=watch, allow_night=False)
                i += 1
                continue
            if kind == 'glide':
                self.equip(IT_CLOAK)
                self.steer(*self.center(cx, cy), tol=1, run=False, watch=watch, allow_night=False)
                d = ((x - cx) // 3, (y - cy) // 3)
                self.face(d)
                self.press('a', 1, 0)
                started = False
                for _ in range(8):
                    if self.u8('pl_state') == PL_GLIDE:
                        started = True
                        break
                    self.tick()
                if not started:
                    self.leg['refused'] += 1
                    self.event('glide_refused', frm=(cx, cy), to=(x, y), face=self.u8('pl_face'))
                    raise Interrupt('glide refused')
                if self.hooks.get('glide_mid'):
                    self.hooks['glide_mid'](self)
                if self.leg['glides'] == 0:
                    self.tick(12)
                    self.milestone('glide')
                for _ in range(60):
                    if self.u8('pl_state') != PL_GLIDE:
                        break
                    self.tick()
                self.leg['glides'] += 1
                self.event('glide', frm=(cx, cy), to=(x, y), landed=self.cell())
                if self.cell() != (x, y):
                    raise Interrupt('glide landed elsewhere')
                i += 1
                continue
            raise ValueError(kind)

    def goto(self, goals, interact=None, allow_night=True, tries=60, budget=90000):
        """Walk until the foot's cell is in goals (replanning as needed)."""
        goals = set(goals)
        f0 = self.frame_n
        attempt = fails = 0
        while fails < tries and self.frame_n - f0 < budget:
            attempt += 1
            self.ter.set_mods(self.mods())
            here = self.cell()
            if here in goals:
                self.buttons(())
                return True
            avoid = {}
            w = self.watch()
            if w is not None:
                avoid = self.watch_avoid(w[2], w[3])
                self.avoid_watch = (w[2], w[3])
            else:
                self.avoid_watch = None
            for (sx, sy) in self.stuck_block:
                i = self.ter.idx(sx, sy)
                if i >= 0:
                    avoid[i] = avoid.get(i, 0) + 300
            if self.ter.explore:
                self.observe_all()
            p = plan(self.ter, here, goals, self.u8('items'), self.u8('stones'), avoid=avoid)
            if p is None and self.u8('items') & (1 << IT_STONES) and self.u8('stones') < 12 and not self.refilling:
                self.event('refill_trip', stones=self.u8('stones'))
                self.refilling = True
                try:
                    self.refill()
                finally:
                    self.refilling = False
                fails += 1
                continue
            if p is None:
                fails += 1
                self.event('no_path', frm=here, goals=sorted(goals)[:4], items=self.u8('items'))
                # nudge and retry (e.g. standing on a cell the plan thinks is solid)
                self.buttons(())
                self.tick(30)
                continue
            if attempt:
                self.leg['replans'] += 1
            try:
                self.follow(p, allow_night=allow_night)
            except Interrupt as e:
                self.buttons(())
                if e.why == 'whiteout':
                    self.handle_whiteout()
                elif e.why == 'night':
                    self.rest()
                elif e.why == 'replan':
                    self.leg['explore_replans'] = self.leg.get('explore_replans', 0) + 1
                elif e.why == 'watcher':
                    w = self.watch()
                    if w is not None and abs(w[0]) < 40 and abs(w[1]) < 40:
                        self.evade(w)
                else:
                    fails += 1
                    self.event('interrupt', why=e.why, cell=self.cell())
                continue
            except Stuck as e:
                fails += 1
                self.leg['stuck'] += 1
                c = self.cell()
                self.stuck_spots.append((self.frame_n, c, self.foot(), str(e.args[0])))
                self.stuck_block.add(tuple(p[0][:2]) if p else c)
                self.event('stuck', cell=c, foot=self.foot(), info=e.args[0], next=p[:2] if p else None)
                self.buttons(())
                self.tick(4)
                continue
            if self.cell() in goals:
                self.buttons(())
                return True
        raise RuntimeError(f'goto {sorted(goals)[:3]} failed ({fails} failures, {self.frame_n - f0} frames)')

    refilling = False

    def refill(self):
        """Out of stones: walk back to the nearest fire we know, where the pouch refills."""
        here = self.cell()
        best = None
        for (x, y) in self.fire_sources():
            ring = {(x + dx, y + dy) for dx in range(-2, 3) for dy in range(-2, 3)
                    if not self.ter.solid(x + dx, y + dy)}
            p = plan(self.ter, here, ring, self.u8('items'), 0, limit=20000)
            if p is not None and (best is None or len(p) < best[0]):
                best = (len(p), (x, y), ring)
        if best is None:
            self.event('refill_nofire')
            return
        _, fire, ring = best
        self.goto(ring, allow_night=False)
        if self.ter.mt(*fire) == MT['MT_FIRE_COLD']:
            self.goto({(fire[0] + dx, fire[1] + dy) for dx, dy in DIRS[::2]
                       if not self.ter.solid(fire[0] + dx, fire[1] + dy)}, allow_night=False)
            self.interact(fire, 'light fire (refill)')
        for _ in range(60):
            if self.u8('stones') >= 12:
                break
            self.buttons(())
            self.tick()
        self.event('refilled', stones=self.u8('stones'), fire=fire)

    def reach_and_interact(self, target, what, check):
        """Walk next to target (a 4-neighbour), face it, press A until check() holds."""
        for attempt in range(6):
            self.ter.set_mods(self.mods())
            adj = {(target[0] + dx, target[1] + dy) for dx, dy in DIRS[::2]
                   if not self.ter.solid(target[0] + dx, target[1] + dy)}
            self.goto(adj)
            self.interact(target, what)
            if check():
                return True
            if self.state() == GS_WHITEOUT or self.u8('pl_state') == PL_SLEEP:
                self.handle_whiteout()
        raise RuntimeError(f'{what}: no effect')

    # ---- the whole world
    def power_cycle(self):
        """Pull the plug: keep only the battery SRAM, boot a fresh Game Boy with it and continue
        from the title (A). Checks the saved state came back."""
        before = {'seed': self.world_layout()['seed'], 'items': self.u8('items'), 'beacons': self.u8('beacons_lit'),
                  'mods': self.mods(), 'worlds': self.u8('worlds_done'), 'respawn': (self.u16('respawn_x'),
                                                                                      self.u16('respawn_y'))}
        sram = self.sram()
        self.event('power_off', saves=self.u8('dbg_saves'), cell=self.cell())
        self.buttons(())
        self.pb.stop(save=False)
        from pyboy import PyBoy
        self.ram = io.BytesIO(sram)
        self.pb = PyBoy(self.rom, window='null', cgb=self.cgb, symbols=self.sym, sound_emulated=False,
                        ram_file=self.ram)
        self.pb.set_emulation_speed(0)
        self.mem = self.pb.memory
        self.held = set()
        for _ in range(3000):
            if self.state() == GS_TITLE:
                break
            self.tick()
        self.tick(60)
        self.milestone('title_continue')
        self.press('a')
        for _ in range(4000):
            if self.state() == GS_WORLD and self.u8('pal_fade') == 0:
                break
            self.tick()
        after = {'seed': self.world_layout()['seed'], 'items': self.u8('items'), 'beacons': self.u8('beacons_lit'),
                 'mods': self.mods(), 'worlds': self.u8('worlds_done'), 'respawn': (self.u16('respawn_x'),
                                                                                     self.u16('respawn_y'))}
        diff = {k: (before[k], after[k]) for k in before if before[k] != after[k]}
        self.event('power_on', diff=diff, cell=self.cell(), state=self.u8('pl_state'))
        self.milestone('continued')
        self.power_diffs.append(diff)
        self.wake()
        self.ter.set_mods(self.mods())

    def sit_until(self, tod_min):
        """Idle beside the start fire until the clock passes tod_min (sitting runs time 8x)."""
        f0 = self.frame_n
        while self.u16('tod') < tod_min:
            self.buttons(())
            self.tick()
            if self.state() != GS_WORLD or self.u8('pl_state') == PL_SLEEP:
                self.handle_whiteout()
        self.event('waited', frames=self.frame_n - f0, tod=self.u16('tod'))

    def play(self, power_cycle_after=(), start_tod=None):
        t0 = time.time()
        self.power_diffs = []
        result = {'seed': self.seed, 'model': 'cgb' if self.cgb else 'dmg', 'completed': False,
                  'mode': ('explore' if self.explore else 'omniscient') + ('+walk' if not self.run_ok else '')}
        try:
            self.boot_new_world()
            w = self.layout
            result['seed'] = w['seed']
            result['layout'] = w
            self.check_land_vs_host('spawn')
            self.milestone('wake')
            # leg 0: wake and light the start fire
            self.leg_begin('start')
            self.wake()
            self.face((0, -1))
            self.press('a', 2, 12)
            assert self.mods().get(w['start']) == MT['MT_FIRE_LIT'], 'start fire not lit'
            self.tick(20)
            self.milestone('start_fire')
            if start_tod:
                self.sit_until(start_tod)
                self.milestone('evening')
            self.leg_end()
            if 'start' in power_cycle_after:
                self.power_cycle()
            # leg 1: beacon 0 (brambles; STONES)
            self.leg_begin('beacon0')
            self.reach_and_interact(w['shrine'][0], 'shrine0',
                                    lambda: self.u8('items') & (1 << IT_STONES))
            self.milestone('stones')
            self.reach_and_interact(w['beacon'][0], 'beacon0', lambda: self.u8('beacons_lit') & 1)
            self.tick(30)
            self.milestone('beacon0_lit')
            self.check_land_vs_host('beacon0')
            self.leg_end()
            if 'beacon0' in power_cycle_after:
                self.power_cycle()
            # leg 2: beacon 1 (shallows; CLOAK)
            self.leg_begin('beacon1')
            self.reach_and_interact(w['shrine'][1], 'shrine1',
                                    lambda: self.u8('items') & (1 << IT_CLOAK))
            self.milestone('cloak')
            self.reach_and_interact(w['beacon'][1], 'beacon1', lambda: self.u8('beacons_lit') & 2)
            self.tick(30)
            self.milestone('beacon1_lit')
            self.check_land_vs_host('beacon1')
            self.leg_end()
            if 'beacon1' in power_cycle_after:
                self.power_cycle()
            # leg 3: beacon 2 (crags)
            self.leg_begin('beacon2')
            self.reach_and_interact(w['beacon'][2], 'beacon2', lambda: self.u8('beacons_lit') & 4)
            self.tick(30)
            self.milestone('beacon2_lit')
            assert self.u8('heart_revealed'), 'heart not revealed after three beacons'
            self.leg_end()
            if 'beacon2' in power_cycle_after:
                self.power_cycle()
                assert self.u8('heart_revealed'), 'heart not revealed after continuing'
            # leg 4: the Heart
            self.leg_begin('heart')
            h = w['heart']
            adj = {(h[0] + dx, h[1] + dy) for dx, dy in DIRS if not self.ter.solid(h[0] + dx, h[1] + dy)}
            done0 = self.u8('worlds_done')
            try:
                self.goto(adj)
            except (RuntimeError, Interrupt):
                if self.state() != GS_ENDING:
                    raise
            ok = False
            for _ in range(600):
                if self.state() == GS_ENDING:
                    ok = True
                    break
                if self.hooks.get('ending'):
                    break
                self.buttons(())
                self.tick()
            if self.hooks.get('ending'):
                self.hooks['ending'](self)
                ok = True
            assert ok, 'the ending did not start at the Heart'
            if self.state() == GS_ENDING:
                self.tick(150)
                self.milestone('ending')
            for _ in range(6000):
                if self.state() == GS_WORLD and self.u8('pal_fade') == 0:
                    break
                self.tick()
            self.leg_end()
            self.tick(40)
            self.milestone('new_world')
            w2 = self.world_layout()
            result['worlds_done'] = self.u8('worlds_done')
            result['new_seed'] = w2['seed']
            assert self.state() == GS_WORLD, 'no new world'
            assert self.u8('worlds_done') == done0 + 1, f"worlds_done {done0} -> {self.u8('worlds_done')}"
            assert w2['seed'] != w['seed'], 'same seed after the ending'
            assert self.u8('beacons_lit') == 0
            # the new world must be playable: wake and walk a little
            self.wake()
            f = self.foot()
            self.hold_keys(('down',), 30)
            self.hold_keys(('left',), 30)
            self.hold_keys(('right',), 30)
            result['new_world_moved'] = self.foot() != f
            self.tick(4)
            self.milestone('new_world_walk')
            result['completed'] = True
        except Exception as e:     # keep the metrics of a failed run
            import traceback
            result['error'] = f'{type(e).__name__}: {e}'
            self.log('FAILED ' + traceback.format_exc())
            self.milestone('failure')
            if self.leg:
                self.leg_end()
        result['frames'] = self.frame_n
        result['minutes'] = round(self.frame_n / FPS / 60, 2)
        result['legs'] = self.legs
        result['whiteouts'] = sum(L['whiteouts'] for L in self.legs)
        result['rests'] = sum(L['rests'] for L in self.legs)
        result['stones_used'] = sum(L['stones'] for L in self.legs)
        result['watcher_spawns'] = self.watcher_spawns
        result['stuck_spots'] = self.stuck_spots
        result['collision_holes'] = self.hitbox_bad
        result['torn_reads'] = self.torn_reads
        result['power_cycle_diffs'] = self.power_diffs
        result['frame_drops'] = self.u16('dbg_frame_drops') if self.has('dbg_frame_drops') else None
        for k in ('dbg_stalls', 'dbg_refills', 'dbg_saves'):
            result[k] = (self.u16(k) if k == 'dbg_stalls' else self.u8(k)) if self.has(k) else None
        result['stalls_by_keys'] = self.stall_stats
        result['wall_seconds'] = round(time.time() - t0, 1)
        result['events'] = self.events
        result['samples'] = self.samples
        with open(os.path.join(self.out, 'result.json'), 'w') as f:
            json.dump(result, f, indent=1, default=str)
        if self.shots:
            contact_sheet(self.shot_list, os.path.join(self.out, 'contact.png'), self.tag)
        return result

    def hold_keys(self, keys, n):
        self.buttons(keys)
        self.tick(n)
        self.buttons(())

    def sram(self):
        return bytes(self.mem[0, 0xA000 + i] for i in range(SRAM_SIZE))

    def stop(self):
        self.buttons(())
        self.pb.stop(save=False)
        self.logf.close()


def contact_sheet(shots, path, title, cols=8):
    from PIL import Image, ImageDraw
    if not shots:
        return
    tw, th, lh = 160, 144, 12
    rows = (len(shots) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * tw, rows * (th + lh) + 16), (24, 24, 24))
    d = ImageDraw.Draw(sheet)
    d.text((4, 2), title, fill=(230, 230, 230))
    for k, (f, label, p, periodic) in enumerate(shots):
        im = Image.open(p).convert('RGB')
        x, y = (k % cols) * tw, 16 + (k // cols) * (th + lh)
        sheet.paste(im, (x, y))
        d.text((x + 2, y + th), f'{f / FPS / 60:5.1f}m {label}'[:26],
               fill=(160, 160, 160) if periodic else (255, 220, 120))
    sheet.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--seed', type=lambda s: int(s, 0), default=0x1234)
    ap.add_argument('--cgb', action='store_true')
    ap.add_argument('--rom', default=os.path.join(ROOT, 'build', 'open-world.gb'))
    ap.add_argument('--sym', default=None)
    ap.add_argument('--owgen', default=os.path.join(ROOT, 'build', 'owgen'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'playthrough'))
    ap.add_argument('--max-frames', type=int, default=500000)
    ap.add_argument('--no-shots', action='store_true')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--walk', action='store_true', help='never hold B (walk at 1 px/frame)')
    ap.add_argument('--power-cycle', default='', help='comma list of legs after which to power-cycle and continue')
    ap.add_argument('--start-tod', type=int, default=None,
                    help='sit by the start fire until this time of day first (e.g. 21600 = dusk)')
    ap.add_argument('--tag', default='', help='suffix for the output folder')
    ap.add_argument('--div-seed', action='store_true',
                    help='no memory write at all: take the seed the ROM draws from DIV when SELECT is pressed')
    ap.add_argument('--explore', action='store_true',
                    help='compass-only: know only the cells seen on screen (a first-time player)')
    a = ap.parse_args()
    sym = a.sym or os.path.splitext(a.rom)[0] + '.sym'
    p = Pilot(a.rom, sym, a.owgen, a.seed, a.cgb, a.out, max_frames=a.max_frames,
              log=None if a.quiet else print, shots=not a.no_shots, explore=a.explore,
              walk=a.walk, tag=a.tag, seed_mode='div' if a.div_seed else 'poke')
    r = p.play(power_cycle_after=tuple(x for x in a.power_cycle.split(',') if x), start_tod=a.start_tod)
    p.stop()
    summary = {k: r[k] for k in ('seed', 'model', 'completed', 'minutes', 'whiteouts', 'rests', 'stones_used',
                                 'watcher_spawns', 'wall_seconds') if k in r}
    summary['legs'] = [(L['name'], L['minutes'], L['whiteouts'], L['rests']) for L in r['legs']]
    if 'error' in r:
        summary['error'] = r['error']
    print(json.dumps(summary))
    return 0 if r['completed'] else 1


if __name__ == '__main__':
    sys.exit(main())
