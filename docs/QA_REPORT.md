# OPEN WORLD: QA and playtest report

*QA lead / playtester pass, 2026-09-29. Build under test: HEAD `e1e7a25` (autotiled shores) plus
the core agent's uncommitted `world.c` / `world_gen.c` edits at the time, built in a snapshot copy.
Every run was repeated on the previous build (`c4703ad`) as well. Where the two builds differ,
both numbers are given.*

## TL;DR

- **Every world can be finished with real inputs.** That held for 24 of 24 autopilot runs: 4 seeds, DMG and CGB,
  and 3 pilot styles. It also held for runs that power-cycle after every leg, runs that start at dusk or at night, and runs
  where the seed comes from DIV with no memory writes at all. No softlocks, no collision holes and no desync between the
  streamed land and the host generator.
- **A world is far too short for the design.** A map-knowing runner finishes in **2.3-4.0 min**. A first-time player
  steering only by the band compass finishes in **5.8-13.5 min** walking, or **3.2-6.0 min** running. The design
  targets *"one beacon per 10-20 min session, a world in about an hour"*. The warmth/fire/night loop almost never comes
  into play: the first night starts 7 min in, and only 2 of 24 runs ever needed a fire.
- **The top bugs:**
  1. **DMG running stutter.** The wanderer is frozen by the streamer for 17-27 % of frames, in freezes of up to
     38 frames. The shore autotiling regressed this about 2x. CGB is not affected.
  2. **DMG fog makes the land invisible.**
  3. **Resting by a fire at night gets you killed by Watchers.** It happens on 3 of 4 nights, and every spawn ends in a touch.
  4. **SELECT on the title silently overwrites the save.**
  5. **The Heart is invisible on the DMG map.**

## How it was tested

| Tool | What it does |
|---|---|
| `tools/autopilot.py` | Plays one whole world in PyBoy with **button inputs only**. The only memory write is `dbg_seed`, poked on the title right before SELECT (the ROM's own hook), so that a seed can be chosen. `--div-seed` makes no write at all: the ROM seeds from DIV, and that is deterministic under emulation (the first boot gives `0x4f86`). Position, state, warmth, time, mods and the Watcher are **read** to close the loop. Terrain comes from `owgen dump`, with the ROM's mods table laid over it. A* runs with the item rules (burn brambles, stepping stones on shallows only, 3-cell glide over `MTF_GLIDE`). The path is followed with the d-pad by pixel-level steering of the foot point, with stuck and streamer-freeze detection. The **night policy** is: keep walking while warmth covers the rest of the night; otherwise walk to the nearest fire, light it, stand in cover if there is any, sit until dawn and step away from Watchers. Screenshots go to `build/playthrough/<run>/`: one every 30 s of game time, one at each milestone, and a `contact.png` sheet. |
| `--explore` | A "first-time player" pilot. It knows only the cells it has seen on screen (11x9). Unknown cells are assumed to be grass, and it heads for the markers on the band. It prefers causeways, is careful with stones and replans when the screen shows an obstacle. `--walk` never holds B. |
| `tools/playthrough_matrix.py` | Runs seeds x models x pilots in parallel and writes `build/playthrough/summary.json`. |
| `tools/qa_edgecases.py` | 13 edge cases, played with real inputs (listed below). Writes `build/playthrough/edge_{dmg,cgb}.json` and screenshots. |
| `tests/test_playthrough.py` | unittest. **It always runs**, because one seed takes about 3 s of wall time per model: DMG and CGB, seed `0x1234`, completion, `worlds_done` +1, a new seed, walking in the new world, zero collision holes, at least 1 burn, stone and glide, and land == host. **`OW_SLOW=1`** adds 3 more seeds on both models (`0x004E` is about half water), the first-time pilot, a power cycle after every leg, and a run through the night. That is about 60 s. `OW_ROM` and `OW_OWGEN` select a build. |

A per-frame hitbox check runs through every run. The wanderer's hitbox ([-5,4] x [-5,0] round the foot) must never
overlap a solid cell of the ROM's own land cache.

## Results

Minutes are game time at 59.73 fps, so they equal real-player minutes. "omni" is a runner who knows the whole map: the fastest
possible world. "explore" is the compass-only pilot. "DMG stall frames" counts frames where the wanderer was held still by
the streamer (`dbg_stalls`).

| seed | model | pilot | done | total min | start | beacon 0 | beacon 1 | beacon 2 | Heart | whiteouts | rests | stones | Watchers | DMG stall frames |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0x0001 | CGB | omni | yes | 2.3 | 0.02 | 0.37 | 0.63 | 0.54 | 0.61 | 0 | 0 | 2 | 0 | 0 (0%) |
| 0x0001 | DMG | omni | yes | 2.9 | 0.02 | 0.42 | 0.80 | 0.70 | 0.80 | 0 | 0 | 2 | 0 | 1904 (18%) |
| 0x0001 | CGB | explore | yes | 3.2 | 0.02 | 0.37 | 1.38 | 0.65 | 0.68 | 0 | 0 | 2 | 0 | 0 (0%) |
| 0x0001 | DMG | explore | yes | 4.0 | 0.02 | 0.42 | 1.82 | 0.81 | 0.82 | 0 | 0 | 2 | 0 | 2580 (18%) |
| 0x0001 | CGB | explore-walk | yes | 5.8 | 0.02 | 0.67 | 2.71 | 1.19 | 1.07 | 0 | 0 | 2 | 0 | 0 (0%) |
| 0x0001 | DMG | explore-walk | yes | 6.0 | 0.02 | 0.67 | 2.77 | 1.22 | 1.14 | 0 | 0 | 2 | 0 | 417 (2%) |
| 0x004e | CGB | omni | yes | 2.5 | 0.02 | 0.40 | 0.64 | 0.54 | 0.70 | 0 | 0 | 2 | 0 | 0 (0%) |
| 0x004e | DMG | omni | yes | 3.3 | 0.02 | 0.48 | 0.84 | 0.71 | 1.06 | 0 | 0 | 2 | 0 | 2782 (24%) |
| 0x004e | CGB | explore | yes | 3.8 | 0.02 | 0.41 | 1.66 | 0.76 | 0.75 | 0 | 0 | 14 | 0 | 0 (0%) |
| 0x004e | DMG | explore | yes | 4.8 | 0.02 | 0.49 | 1.99 | 0.96 | 1.15 | 0 | 0 | 14 | 0 | 3526 (21%) |
| 0x004e | CGB | explore-walk | yes | 6.8 | 0.02 | 0.75 | 3.17 | 1.47 | 1.27 | 0 | 0 | 14 | 0 | 0 (0%) |
| 0x004e | DMG | explore-walk | yes | 7.0 | 0.02 | 0.75 | 3.14 | 1.47 | 1.48 | 0 | 0 | 14 | 1 | 708 (3%) |
| 0x1234 | CGB | omni | yes | 2.9 | 0.02 | 0.38 | 0.73 | 0.59 | 1.03 | 0 | 0 | 2 | 0 | 0 (0%) |
| 0x1234 | DMG | omni | yes | 4.0 | 0.02 | 0.46 | 0.93 | 0.87 | 1.57 | 0 | 0 | 2 | 0 | 3815 (26%) |
| 0x1234 | CGB | explore | yes | 5.0 | 0.02 | 0.39 | 2.40 | 0.74 | 1.25 | 0 | 0 | 2 | 4 | 0 (0%) |
| 0x1234 | DMG | explore | yes | 6.0 | 0.02 | 0.47 | 2.80 | 0.90 | 1.67 | 0 | 0 | 2 | 4 | 3752 (17%) |
| 0x1234 | CGB | explore-walk | yes | 9.4 | 0.02 | 0.72 | 4.61 | 1.41 | 2.52 | 0 | 0 | 3 | 15 | 0 (0%) |
| 0x1234 | DMG | explore-walk | yes | 13.5 | 0.02 | 0.72 | 7.20 | 2.96 | 2.48 | 0 | 1 | 2 | 12 | 1652 (3%) |
| 0xbeef | CGB | omni | yes | 2.7 | 0.02 | 0.31 | 0.61 | 0.61 | 0.96 | 0 | 0 | 2 | 0 | 0 (0%) |
| 0xbeef | DMG | omni | yes | 3.5 | 0.02 | 0.35 | 0.77 | 0.97 | 1.23 | 0 | 0 | 2 | 0 | 2856 (23%) |
| 0xbeef | CGB | explore | yes | 4.9 | 0.02 | 0.31 | 0.81 | 2.65 | 0.98 | 0 | 0 | 14 | 0 | 0 (0%) |
| 0xbeef | DMG | explore | yes | 6.0 | 0.02 | 0.35 | 0.99 | 3.18 | 1.27 | 0 | 0 | 14 | 0 | 3686 (17%) |
| 0xbeef | CGB | explore-walk | yes | 9.7 | 0.02 | 0.57 | 1.57 | 4.30 | 3.10 | 1 | 1 | 14 | 4 | 0 (0%) |
| 0xbeef | DMG | explore-walk | yes | 8.6 | 0.02 | 0.57 | 1.57 | 4.48 | 1.82 | 0 | 0 | 14 | 1 | 637 (2%) |

Extra runs (all completed; in `build/playthrough/*_{powercycle,dusk,night,div}`):

| run | DMG | CGB | notes |
|---|---|---|---|
| power-cycle after every leg (SRAM only, title -> A) | 4.6 min | 3.4 min | Seed, items, beacons, mods, respawn and worlds were identical after each of the 4 cycles. |
| explore-walk, sit at the start fire until dusk first | 11.0 min | 11.0 min | 1 rest each (2600 frames = 43 s real, beside a fire, dusk to dawn). 0 whiteouts. 11 Watchers. |
| omni runner, starting at night (tod 26500) | 5.1 min | 3.9 min | No rest was needed. Four full pips (13 094 frames = 3.6 min of night) outlast a whole run through the dark. |
| `--div-seed` (no memory writes at all) | 4.2 min (seed 0x4f86) | 2.6 min (0x7fb8) | |

- **Whiteouts:** 1 in 30 runs. It happened during a rest at a fire at night, when the 4th Watcher of that rest reached the wanderer.
- **Rests:** 4 in 30 runs, all by walking pilots after dusk.
- **Stones:** the map-knowing pilot always used exactly 2 (the 2-wide shallows ring). The first-time pilot used 2-14, because it
  paves small pools on the way; the pouch refills passively at every lit fire and beacon, so it never ran dry.

## Bugs, prioritized

Repro commands assume a build in `build/`. Add `--rom/--owgen` to point at another build.

### P1

**1. DMG: the wanderer freezes while running (streamer stalls), and it regressed about 2x with the shore autotiling.**
On DMG, 17-27 % of all frames while running are frames where `world_frame` holds the player still because the streamer is at
the edge of its slack (`dbg_stalls`). A world has about 700 freezes. The longest are 22-38 frames (0.4-0.6 s), and about 140 per world are
8 frames or longer, which is a visible hitch. Running sideways is worst: B+left stalls 24.6 % of its frames, B+right 17 %
(numbers from `c4703ad`). Walking stalls 2-3 %. CGB: 0 stalls. On the same inputs, stall frames for seed 0x1234 went from 1541 of 12 025
(`c4703ad`) to 3371-3815 of 14 000 (`e1e7a25`). The core agent's uncommitted edits did not change this (3394). Frame drops: DMG 38-130 per
world, CGB 1-5.
Repro: `python3 tools/autopilot.py --seed 0x1234 --no-shots` and read `result.json`: `dbg_stalls` and `stalls_by_keys`. Or hold
B+Left from the spawn of seed 0x1234 on DMG and watch `dbg_stalls` climb.

**2. DMG: fog makes the land invisible.** `pal.c: fog_tab = {0, 1, 1, 2}` maps shades 1 and 2 to the same shade. The ground and its
detail use only 1 and 2, so a fogged DMG screen is one flat grey. Brambles, shallows, crags, water and the causeway all vanish, and only
sprites and the colour-0 lights remain. Fog covers 3-26 % of play time (14 % on average) and is forced clear only within 40 cells of the start on
day 0. The first fog in seed 0x1234 falls exactly on beacon 0's bramble ring.
Repro: seed 0x1234 DMG, walk north up the causeway to beacon 0 (32805, 32650) at dawn of day 0 (frames about 2000-3600).
Screenshot `build/playthrough/qa/dmg_fog_beacon0.png`, CGB for comparison: `qa/cgb_fog_beacon0.png` (legible but washed out; the HUD pips
almost vanish).
Fix: `fog_tab = {0, 1, 2, 2}`, or cap the lerp at about 60 %.

**3. Watchers kill you while you rest beside a lit fire.** The design's way to skip a night is to sit by a fire, because time runs 8x. With no
input, standing 1 cell from the lit start fire on open ground, 4 nights (about 22 000 frames) gave **3 spawns and 3 touches (whiteouts),
closest 6 px**. That was the same on DMG and CGB and for seeds 0x1234, 0x0001 and 0xBEEF. Every Watcher that spawns next to a sitter reaches it:
a sitter is lit (the glow is on) and not hidden. The pilot's flee-and-return strategy survived 3 Watchers during one rest and fell to the 4th
(`cgb_beef_explore_walk`, `events.log` frame 28 560-32 455).
Also, `wrng` is a fixed `0x7A3D` and never seeded, so the Watcher timing is identical in every world and on every boot.
Repro: `python3 tools/qa_edgecases.py --only night_by_fire` for one night. For several nights, sit by the start fire for 22 000 frames with no input.
Screenshots: `qa/dmg_watcher_at_fire.png`, `qa/cgb_watcher_at_fire.png`.
Fix: no spawn within 4 cells of a warm source (`near_warm`), and no drift toward a wanderer who is sitting by a fire. Seed `wrng`
from the world seed and `vbl_frames`.

### P2

**4. SELECT on the title overwrites the save, with no confirmation.** When a save exists, SELECT starts a new world and `save_write()`
immediately rewrites both copies. The old world is gone. Tallies and `worlds_done` survive, because the title peeked the save.
Repro: light beacon 0 (save), power-cycle, press SELECT on the title. The SRAM seed goes from 0x1234 to 0x66e5 and `beacons_lit` = 0.
In a game with no words, a stray SELECT costs a whole world.
Fix: hold SELECT for about 2 s (the title flame gutters as feedback), or don't write the new world until its first fire is lit.

**5. The Heart is invisible on the DMG map, and faint on CGB.** The map draws the Heart and lit beacons with OBP1 = `0x90` (colour 1
becomes white) on white paper. Repro: light all three beacons, press START. At x=138, y=58 there is nothing
(`qa/dmg_map_heart_invisible.png`). On CGB `OPAL_LIGHT` is pale on the pale paper (`qa/cgb_map_heart_faint.png`). Lit and unlit beacons also look
almost the same on the map.

**6. A next to a lit fire buzzes "no" and shakes the screen.** It did so on 28-30 of 30 presses (`edge_*.json: spam_a`). A player walking up to a fire to
rest is punished. `world.c` even documents `MT_FIRE_LIT (A: sit / rest)`.
Fix: A next to a lit fire or lit beacon makes the wanderer sit at once (skip the 180-frame wait).

**7. On DMG the glide is nearly invisible over crags.** The cloak sprite's greys sit on the rock texture, and the shadow is dark on dark
(`qa/dmg_glide_over_crags.png`, seed 0x0001 beacon 2). This is the best moment of the cloak.
Fix: a colour-0 rim on the glide frames, or OBP1 for the glide sprite.

### P3

8. **Sitting in the cold at night is a 27-second death.** Sitting runs warmth drain at 8x too: from sitting down at night to a whiteout takes 1636
   frames (`cold_whiteout`). There is no signal except the pips. Make the fast-forward happen only near a fire, or make the pips blink when they drain at 8x.
9. **Whiteouts cost almost nothing.** You wake at your last fire at dawn with full warmth and keep everything. After a fire rest, a whiteout
   is effectively a free night skip. A whiteout at a fire is indistinguishable from resting. This compounds #3 and the pacing issue below.
10. **CGB: VBlank ISR overrun.** About once per 10 k frames the autopilot reads a half-updated foot (`pl_mx` already decremented, `pl_sx` not yet
    written). So `world_frame` sometimes straddles the next frame boundary (`torn_read` events, for example `cgb_1234_explore_walk_dusk`
    frame 22 934). It is invisible to the player. It's a hint that the ISR is close to its budget. It is not a collision hole: the next frame is consistent.
11. **The map takes about 600 frames (10 s) to "survey" fully** (`qa` map shots at 60, 240 and 600 frames). That is too slow for a player who hates waiting.
    Draw the visited chunks first (or 3 rings per frame), and animate only the fog.
12. **START during a glide is dropped, not buffered.** This is fine and probably intended; noted only.
13. **The `owgen` header says `layoutraw` prints 18 numbers; it prints 16.** Trivial.

### Checked and clean

- **Collision:** 0 hitbox overlaps with solid cells, checked every frame across more than 500 000 frames of play (30 runs and the edge cases).
- **Water:** running into the sea and shallows from 8 directions: no entry.
- **Land vs host:** the streamed land matched host + mods at spawn, at beacons 0 and 1, and after the map, whiteouts, glides and the ending.
- **The map:** the map pauses time, warmth and Watchers (`tod` does not move). The ambient switches WORLD -> MAP -> WORLD (`snd_cur_mode` 2 -> 3 -> 2).
  Pressing START 20 times quickly always returns to the world with a correct split.
- **The ending:** mashing every button through the ending gives exactly one new world (`worlds_done` 0 -> 1) and a fresh seed. The wanderer is asleep in the new world.
  The new world is walkable, and its start fire is cold.
- **Power cycles:** a power cycle after each leg is exact.
- **Early Heart:** walking to the Heart before the beacons are lit does nothing, as designed. The Heart is reachable with the lantern alone.
- **Split line:** no tearing across 1260 frames of running in 8 directions (frame-to-frame shift consistency).

## Playing it as the client

*The client is 40 and loves FF6, Zelda, Fallout and MGS. He hates wasted time and wants a wordless art piece that feels big.*

**What works.** The CGB palettes are beautiful. Dawn is peach, and night is deep blue with white foam lines and lit fires. The first image
(the sleeping wanderer by a cold fire, with a pillar on the horizon) says everything without words. The band works as a compass. Unlit
beacons are dark pillars. Lit ones are white columns, and there is a small diamond once the Heart is revealed. The hint pictogram (a hand)
appears above every interactable: the fire, shrine, beacon, brambles and shallows when the right item is equipped. So the verbs are
learnable with no text. The new shore foam reads well on both models. Watchers standing in the Ash by day are properly uncanny.

**What doesn't.**
- **Not big.** Each beacon is a 20-60 s run up a causeway. The causeways are straight roads from the start to every gate, so the
  compass barely matters: follow the road, burn two brambles, lay two stones, glide once. A first-timer who walks the whole
  way still finishes in 6-13 min. The world's most interesting mechanic, surviving the night, is skipped by anyone who plays for
  less than 7 minutes.
- **Nothing makes you choose.** Stones refill at every fire and beacon, warmth lasts through most of a night, whiteouts are free, and the Heart
  can be reached with the lantern alone. There is no risk to weigh.
- **DMG readability.** In fog, the land disappears. The glide, over rock, is hard to see.

## Design and pacing recommendations (with numbers)

1. **Longer legs without padding.** Move the beacons from 90-160 to **250-400 metatiles** from the start, and the Heart from 200-260 to **450-600**. At walking
   pace (1 px/frame = 3.7 cells/s) and with the detours measured here (1.4-2.5x the straight line), that is **5-10 min per leg** and **25-40 min per world**.
   Running roughly halves it.
2. **Break the causeways.** Leave gaps of 20-40 cells, and let them bend or end at ruins. Make 30-50 % of each route cross open country, so that the band compass is
   the navigation tool.
3. **Make the night happen.** Shorten the day from 12 to **8 min** (DAY_FRAMES 28 800), so the first dusk falls about 4 min in. A 30-min world then spans
   3-4 nights.
4. **Make a fire stop necessary each night.** Raise the night drain from 5/64 to **7/64** per frame. Four pips then last 2.6 min against a 3.2-min night, so
   each night needs one fire stop. Guarantee a cold fire every **40-60 cells** along the routes.
5. **Make fires safe.** No Watcher spawns within 4 cells of `near_warm`, so sitting by a fire is the safe haven the design promises. Keep Watchers
   dangerous in the open at night and in the Ash.
6. **Give a whiteout a small cost.** Wake with **2 pips instead of 4**, or with an empty stone pouch. It should still be gentle, but no longer free.
7. **Give each leg a find and a gate.** Put the STONES and CLOAK shrines **30-60 cells off the road**, not beside the beacon. Show them on the band as a
   small glint within 40 cells. Then each leg is: see the pillar, notice a glint, detour, return with the tool.
8. **Quick wins:** DMG fog table (#2); A at a fire = sit (#6); the Heart on the map in colour 3 (#5); a glide rim (#7); a hold-to-confirm new world
   (#4); faster map survey (#11).

## Files

- **Tools:** `tools/autopilot.py`, `tools/playthrough_matrix.py`, `tools/qa_edgecases.py`, `tests/test_playthrough.py`.
- **Results:** `build/playthrough/summary.json` (the matrix), `build/playthrough/<run>/{result.json, events.log, contact.png, *.png}`,
  `build/playthrough/edge_{dmg,cgb}.json` and `edge_*/`, and the bug screenshots in `build/playthrough/qa/`.
- **Re-run everything:**
  ```
  make rom build/owgen
  python3 tools/playthrough_matrix.py --seeds 0x1234,0x0001,0x004e,0xbeef --modes omni,explore,explore-walk
  python3 tools/qa_edgecases.py; python3 tools/qa_edgecases.py --cgb
  OW_SLOW=1 python3 -m unittest tests/test_playthrough.py -v
  ```
