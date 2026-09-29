# OPEN WORLD: design

> *The screen is a window, and the world goes on past it.*

A wordless, minimalist, procedurally generated wandering game for the Game Boy (DMG) and
Game Boy Color. It has no dialogue, no story text, no numbers, no combat and no grind.
The only words in the whole cartridge are the title.

## 1. Thesis

The central image is Caspar David Friedrich's *Wanderer above the Sea of Fog*: a small figure
seen from behind, facing something too large to measure. Burke and Kant called this the
**Sublime**. A 160x144 screen can't show it; it can only imply it. So we treat the small screen
as the point. The frame is a window, and the world is what the frame leaves out.

- **Tarkovsky's Zone** sets the mood: a landscape that seems to have intentions. You don't
  conquer it; you cross it carefully.
- **Borges' map as big as the empire** sets the structure: a 65,536 x 65,536-metatile world,
  deterministic from a 16-bit seed. It can never be fully mapped, only held in fragments.
- **Ueda's "design by subtraction" and Japanese *ma*** (meaningful empty space) set the
  method: silence is the medium, and empty ground is composition.
- **Eno's generative ambient** sets the sound: phasing loops of unequal length that never
  repeat exactly, driven by the land.
- It reads like Calvino's *Invisible Cities*: short, dense, slightly unreal episodes.

It is also designed to **respect a grown-up's time**. One beacon fits in one 10-20 minute
session, the game saves at every fire, and a world takes about an hour.

## 2. The world, in one paragraph

You wake beside a cold fire. Far away, on the **horizon band** at the top of the screen, three
dark pillars stand against the sky. Those are the **Beacons**. Walk to one (an old causeway
leads there, if you notice it), get through the ring that guards it, and light it. When all
three are lit, a fourth light, **the Heart**, rises on the horizon. Walk to it and sit. The
world dissolves and a new one is generated, and the cairns you built are still there as
ancient ruins.

## 3. Screen layout (hardware trick #1: two tile maps in one frame)

```
 lines 0-23   HORIZON BAND  BG map 0x9C00, its own SCX = your bearing; a 360-degree panorama
 lines 24-143 THE LAND      BG map 0x9800, a 32x32 streaming ring buffer, world SCX/SCY
```
A STAT/LYC interrupt at line 0 selects map 0x9C00 (LCDC bit 3) and the band's scroll. A second
interrupt at line 24 switches back to 0x9800 and the world scroll. The window layer is free
(used for the map screen). HUD elements are **sprites** on lines 0-8, inside the band:

- **Warmth:** 4 ember pips.
- **Equipped item:** one icon.

**The horizon band** is a 256-px-wide panorama that represents 360 degrees. Its centre is the
direction you are facing, and it turns smoothly when you turn. It contains:

- a sky whose colour follows the time of day, with stars at night;
- a distant mountain silhouette from low-frequency noise of `(seed, bearing)`;
- markers for the **Beacons** (dark pillar when unlit, a column of light when lit), the
  **Heart** (once revealed), and your nearest cairn.

**The band is the compass.** Nothing in the game needs a text hint (the pictograms are in §5). On CGB the band has its
own sky palettes (gradient rows via attributes).

## 4. The world

- **Coordinates.** A metatile is 16x16 px (2x2 BG tiles). Coordinates are
  `uint16_t mx, my`, so the world wraps as a 65,536^2 torus. The start is near
  (32768, 32768).
- **Noise.** Three integer value-noise fields come from a permutation-table hash:
  - elevation (2 octaves: a 16-metatile grid plus a 4-metatile grid);
  - moisture (a 32-metatile grid);
  - strangeness (a 64-metatile grid).
- **Biome classification:** `biome_lut[elev>>4][moist>>4]`.

| Biome | Look | Rule |
|---|---|---|
| Sea | water, animated | impassable |
| Shallows | pale water | impassable unless a **stepping stone** is placed |
| Shore | sand | normal |
| Meadow | grass, tall grass, flowers | normal. Tall grass hides you (stretch). |
| Forest | trees (solid), undergrowth | trees block you. Undergrowth is walkable and slow. |
| Desert | dunes, bones | cold nights (double warmth loss at night) |
| Tundra | snow, pines | double warmth loss |
| Rock | crags, cliffs | impassable, but the **cloak** glides over it |
| Ash / Glass (strangeness high) | charcoal ground, glass shards, monoliths | uncanny. Watchers roam here. |
| Ruins (strangeness overlay) | walls, floors, pillars, statue fragments | environmental story |

**Points of interest**, one hash roll per 16x16-metatile cell:
- **campfire** (cold until lit): warmth, respawn point and save point;
- **monolith**;
- **ruin** fragment;
- **ancient cairn** (from earlier worlds);
- **"story" vignette:** a table set for two, a road ending in the sea, a giant stone hand.

**Set pieces** override the noise:
- **Beacons 0-2** are placed at bearings 0 / 120 / 240 degrees (plus seed jitter), 90-160
  metatiles from the start.
- **The Heart** sits at 450-600 metatiles from the start.
- Each beacon has a **clear plateau** of radius 3 and a **gate ring** at radius 4-5:
  - Beacon 0: a ring of **brambles**. The lantern burns them. It holds the STONES shrine.
  - Beacon 1: a ring of **shallows** (an island). Stepping stones cross it. It holds the
    CLOAK shrine.
  - Beacon 2: a ring of **crags** (a plateau). The cloak glides over it.
- **Causeways:** ancient roads, 1 wide, from the start to each beacon's gate and to the
  Heart. They are fragmentary: road is forced wherever the line crosses sea, rock or trees
  (bridges and passes, so every set piece is reachable; the host tests prove it with a BFS),
  but on open land only about 30% of it shows. It is a buried road you find and follow. A
  cold fire stands beside it every 64 metatiles.

**Mods.** A small table of `(mx, my, metatile)` overrides holds world state: lit fires, lit
beacons, taken shrines, placed cairns, stepping stones and burnt brambles. It is saved to
SRAM. It has 96 slots, so when fewer than 24 are free the main loop lets the farthest edit
that is easy to make again (more than 20 cells away) return to the land: a lit fire burns down
(never the one you wake at), a stepping stone sinks, burnt thorns grow back. Beacons, shrines
and cairns are never touched. This way a long world can't fill the table and stop you lighting
fires.

## 5. Player and verbs

- **D-pad:** walk in 8 directions at 1 px/frame. **B held:** run at 2 px/frame.
- **A:** use the equipped item. **SELECT:** cycle items. **START:** map.
- **Stand still for about 3 s** and the wanderer sits. Time runs 8x while you sit by a fire.
  This skips nights without grind, and it is where the quiet moments happen. Away from a fire,
  A stands up *and* acts, so a pause never swallows a press.
- **LANTERN** (you have it from the start):
  - A on a cold fire or beacon: light it.
  - A on brambles: burn them.
  - At night the lantern is always lit: a sprite-dither **glow** around the player
    (hardware trick #2, see below).
- **STONES** (an endless pouch):
  - A on land: build a cairn. It shows on the map and on the band, and carries into the
    next world.
  - A on shallows: place a stepping stone.
  - Up to 32 of your cairns stand at once. Building another takes down the oldest.
- **CLOAK:** A: glide forward over anything the cloak can pass (rock, water, thorns). It lands
  3 metatiles away, or 4 or 2 when that is the nearest passable ground. The crag ring is thicker
  at an angle than straight on. On a diagonal it also tries the two straight directions, the
  blocked side first. If nothing fits, the glide is refused with a "no" sound and a shake. An A
  pressed in the last few frames of a glide is kept for the landing, so glides chain.
- **Showing, not telling.** The game never explains itself in words, but it should not depend
  on the player pressing every button on every tile either. The only letters in the world are
  the names of the buttons, drawn on the buttons:
  - **A over the target.** A finger on a button marked A bobs over whatever A would act on
    with the item in hand.
  - **SELECT over the wanderer.** When the item in hand can't act on what you face but another
    one you carry could (lantern at the shallows, stones at the crags...), the SELECT pill and
    its label float over the wanderer instead.
  - **Hold B, once.** After a few seconds of walking without ever running, a finger holds B
    over the wanderer's head. The first run retires it for good.
  - **A lesson per item, once.** Taking the stones or the cloak from its shrine opens a short
    wordless scene: a diorama made from the world's own tiles, where the wanderer uses the new
    item with the same A pictogram (stepping stones and a cairn; a glide over crags and water),
    then SELECT cycles the items you carry, the one in hand marked. After one play-through a
    bobbing A in the corner closes it. The lessons are saved, so they never repeat, not even in
    later worlds.
- **Warmth:** 4 pips.
  - It drains at night away from fire, twice as fast in tundra and desert nights.
  - It refills beside a lit fire or beacon, and slowly by day.
  - At 0 the screen whites out and you wake at your last lit fire (dawn). You keep
    everything.

## 6. Time and weather

- **Day length:** 8 real minutes. The phases are dawn, day, dusk and night.
- **DMG palette rule (the core of the art direction):** colour index 0 is reserved for
  **light**: fire, beacons, stars, water glints, lamp glow. The ground mostly uses index 1.
  The night BGP maps indices 1, 2 and 3 to near-black and keeps 0 bright. At night the land
  goes dark and **only light stays light**.
  - Day: `0xE4`
  - Dusk: `0xF9`-ish
  - Night: `0xFC` (0 stays white)
- **Water reads as lying below the land.** The shallows ripple (two quarters of each cell are
  an animated tile of drifting wavelets, dense enough that on a DMG, where they share the
  ground's shade, the texture alone says "water"). The autotiled coast is drawn the way a bank
  looks from above: a dark wet line along the land, the foam breaking just off it, and the
  sea shelving away from the shallows through a dither. (A bright rim on the land's edge, as it
  first was, reads as a lit ridge: the coast looked like snowy plateaus.)
- **CGB:** each palette class (grass, forest, water, sand, snow, rock/ash, light, sky) lerps
  between four times of day. The palette is uploaded in VBlank.
- **Weather** is deterministic per region per day: rain streaks (sprites), snow in tundra,
  and fog (the palette softens toward grey: a veil, never a white-out). Storms bring an
  occasional single lightning flash.

## 7. Watchers (stretch)

Tall, still, silent figures. They appear at night and in the Ash/Glass. They slowly turn to
face your light, and at close range they drift toward you. Their touch whites you out, the
same as losing all warmth. Hiding in forest undergrowth or tall grass breaks their line of
sight. They are never explained.

## 8. Map (START)

A map at 1 px per chunk (1 chunk = 16x16 metatiles), covering about +/-1024 metatiles around
the start. Only **visited chunks** are drawn, with biome shading. Everything
else is fog. Beacons, the Heart (once revealed), cairns and the player are sprites. It renders
progressively ("surveying"), with the LCD on.

## 9. Sound

A generative ambient engine:
- **CH3 (wave):** a drone for each biome. It breathes: 5-10 s of tone, a fade, 2-4 s of rest.
  One unbroken, unchanging tone is what a crashed Game Boy sounds like.
- **CH1:** sparse pentatonic phrases from an LFSR, in the biome's mode.
- **CH2:** an echo of CH1.
- **CH4:** wind, gusting.

It changes with time of day (night is slower and lower). There are footsteps for each
terrain and sfx for lighting, building, gliding, whiteout, and dawn. Each lit beacon adds a
tone to a dawn motif, and at the Heart the tones resolve together.

## 10. Save (battery SRAM, MBC5)

Two copies of a checksummed save block hold:
- seed, position, time, items, beacon bits and warmth;
- the mods table;
- the cairns list;
- the visited-chunk bitmap (128x128 bits = 2 KB);
- worlds completed;
- the hints already shown (version 3 appends this one byte, so a version 2 save still loads).

It saves automatically when you light a fire, light a beacon, or build a cairn.

## 11. Title

The words "OPEN WORLD", a small wanderer from behind, and a flame with **tally marks** for
worlds completed.
- **A:** continue.
- **SELECT:** new world, seeded from DIV at the moment you press.
- The seed is shown as a 4-glyph rune sigil.

## 12. The ending of a world

1. At the Heart, the wanderer sits.
2. The band turns bright.
3. All beacon tones resolve.
4. The screen fades to the lightest shade.
5. A new seed begins.

Your cairns carry into the new world as ancient cairns, placed at the same offsets from the
start.

## Module map

```
src/core/   portable C (SDCC + gcc): hash/noise, biomes, set pieces, mods, reachability
src/gb/     GBDK front-end: ISRs, streaming ring, band, sprites, game states, save, sound
assets/     ASCII art (tiles, metatiles, sprites, palettes) -> tools/gen_assets.py -> src/gb/assets.c
tools/      gen_assets.py, owgen.c (host CLI: render a region / stats / reachability)
tests/      test_core.c, test_sound.c, test_assets.py, test_rom.py (PyBoy, DMG + CGB)
```
