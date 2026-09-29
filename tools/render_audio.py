#!/usr/bin/env python3
"""render_audio.py - build a tiny sound-test ROM around src/gb/sound.c, run it headless in
PyBoy (real APU emulation) and render the ambient modes / sfx to WAV, measuring the cost of
sound_tick() on the emulated CPU.

    python3 tools/render_audio.py                  # everything -> build/audio/*.wav
    python3 tools/render_audio.py --only title     # names containing 'title'
    python3 tools/render_audio.py --secs 120       # longer world renders
    python3 tools/render_audio.py --bench          # cycle stats only (no WAVs)

Needs /opt/gbdk and pyboy + numpy.
"""
import argparse
import os
import subprocess
import sys
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "sndtest")
LCC = os.environ.get("LCC", "/opt/gbdk/bin/lcc")

BIOMES = ["sea", "shallow", "shore", "meadow", "forest", "desert", "tundra", "rock", "ash", "ruins"]
PHASES = ["dawn", "day", "dusk", "night"]
WEATHER = ["clear", "rain", "snow", "fog", "storm"]
MODES = ["silent", "title", "world", "map", "ending", "wake"]
SFX = ["step_soft", "step_sand", "step_snow", "step_stone", "light", "burn", "cairn", "stepstone",
       "pickup", "glide", "land", "no", "item", "beacon", "heart", "whiteout", "dawn", "select",
       "map", "thunder", "sit"]

MAIN_C = r"""
#include <gb/gb.h>
#include <stdint.h>
#include "sound.h"
/* mailbox at 0xD800:
   [0] mode req (0xFF none)  [1] sfx req (0xFF none)  [2] biome [3] phase [4] weather
   [5] set flag (1 = call ambient_set)  [6] beacons (0xFF none)  [7] tempo (0xFF none)
   [8..9] last tick cost  [10..11] max  [12..15] sum (units of 16 M-cycles)
   [16..17] seed, [18] seed flag  [19] frame ack counter */
#define MB ((volatile uint8_t *)0xD800)
void main(void)
{
    uint16_t cost, mx = 0; uint32_t sum = 0;
    MB[0] = 0xFF; MB[1] = 0xFF; MB[5] = 0; MB[6] = 0xFF; MB[7] = 0xFF; MB[18] = 0;
    sound_init();
    TAC_REG = 0x06;                    /* 65536 Hz: 1 count = 64 T = 16 M-cycles */
    while (1) {
        wait_vbl_done();
        if (MB[18]) { ambient_seed(MB[16] | ((uint16_t)MB[17] << 8)); MB[18] = 0; }
        if (MB[5]) { ambient_set(MB[2], MB[3], MB[4]); MB[5] = 0; }
        if (MB[6] != 0xFF) { ambient_beacons(MB[6]); MB[6] = 0xFF; }
        if (MB[7] != 0xFF) { ambient_tempo(MB[7]); MB[7] = 0xFF; }
        if (MB[0] != 0xFF) { ambient_mode(MB[0]); MB[0] = 0xFF; }
        if (MB[1] != 0xFF) { sfx_play(MB[1]); MB[1] = 0xFF; }
        disable_interrupts();
        TIMA_REG = 0; IF_REG &= ~TIM_IFLAG;
        sound_tick();
        cost = TIMA_REG;
        if (IF_REG & TIM_IFLAG) cost += 256;
        enable_interrupts();
        if (cost > mx) mx = cost;
        sum += cost;
        MB[8] = cost; MB[9] = cost >> 8; MB[10] = mx; MB[11] = mx >> 8;
        MB[12] = sum; MB[13] = sum >> 8; MB[14] = sum >> 16; MB[15] = sum >> 24;
        MB[19]++;
    }
}
"""


def build_rom():
    os.makedirs(BUILD, exist_ok=True)
    main = os.path.join(BUILD, "main.c")
    with open(main, "w") as f:
        f.write(MAIN_C)
    rom = os.path.join(BUILD, "sndtest.gb")
    src = os.path.join(ROOT, "src", "gb")
    cmd = [LCC, "-I" + src, "-I" + os.path.join(ROOT, "src", "core"), "-Wl-j", "-o", rom, main,
           os.path.join(src, "sound.c")]
    subprocess.check_call(cmd)
    return rom


class Rig:
    def __init__(self, rom):
        from pyboy import PyBoy
        self.pb = PyBoy(rom, window="null", sound_emulated=True, sound_sample_rate=48000)
        self.m = self.pb.memory
        self.pb.tick(400, False, True)
        self.samples = []
        self.costs = []

    def frame(self, n=1, keep=True):
        import numpy as np
        for _ in range(n):
            self.pb.tick(1, False, True)
            if keep:
                self.samples.append(np.array(self.pb.sound.ndarray, copy=True))
            self.costs.append(self.cost())

    def cost(self):
        m = self.m
        return (m[0xD808] | m[0xD809] << 8) * 16

    def mode(self, x):
        self.m[0xD800] = MODES.index(x) if isinstance(x, str) else x

    def sfx(self, x):
        self.m[0xD801] = SFX.index(x) if isinstance(x, str) else x

    def set(self, biome, phase, wx="clear"):
        self.m[0xD802] = BIOMES.index(biome)
        self.m[0xD803] = PHASES.index(phase)
        self.m[0xD804] = WEATHER.index(wx)
        self.m[0xD805] = 1

    def seed(self, s):
        self.m[0xD810] = s & 0xFF
        self.m[0xD811] = s >> 8
        self.m[0xD812] = 1

    def beacons(self, b):
        self.m[0xD806] = b

    def tempo(self, t):
        self.m[0xD807] = t

    def take(self):
        import numpy as np
        a = np.concatenate(self.samples) if self.samples else np.zeros((0, 2), dtype=np.int8)
        self.samples = []
        c = self.costs
        self.costs = []
        return a, c


def write_wav(path, a, rate=48000):
    import numpy as np
    x = a.astype(np.float64)
    x -= x.mean(axis=0)          # remove DC (the GB mixer idles off-centre)
    pcm = np.clip(x * 256, -32768, 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())


def stats(a):
    import numpy as np
    if len(a) == 0:
        return "empty"
    x = a.astype(np.float64)
    x -= x.mean(axis=0)
    rms = np.sqrt((x ** 2).mean())
    peak = np.abs(x).max()
    lr = np.sqrt((x ** 2).mean(axis=0))
    return "rms %5.1f (L %4.1f R %4.1f)  peak %5.1f" % (rms, lr[0], lr[1], peak)


def cstats(c):
    c = sorted(c)
    n = len(c)
    return "tick avg %4d med %4d p99 %4d max %4d" % (sum(c) // n, c[n // 2], c[n * 99 // 100], c[-1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default=None)
    ap.add_argument("--secs", type=float, default=40)
    ap.add_argument("--bench", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    rom = build_rom()
    out = os.path.join(ROOT, "build", "audio")
    os.makedirs(out, exist_ok=True)
    rig = Rig(rom)
    keep = not args.bench
    allc = []

    def want(name):
        return args.only is None or args.only in name

    def emit(name, extra=""):
        a, c = rig.take()
        allc.extend(c)
        if keep:
            write_wav(os.path.join(out, name + ".wav"), a)
        top = sorted(range(len(c)), key=lambda i: -c[i])[:3]
        print("%-28s %s  %s %s" % (name, cstats(c), "" if args.bench else stats(a), extra))
        if args.verbose:
            print("    worst frames:", ", ".join("#%d=%d" % (i, c[i]) for i in top))

    def silence():
        rig.mode("silent")
        rig.frame(60, keep=False)
        rig.take()

    rig.seed(0x5EED)
    rig.frame(2, keep=False)
    if want("title"):
        rig.mode("title")
        rig.frame(int(60 * 45))
        emit("title")
        silence()
    for b in BIOMES:
        for p in PHASES:
            name = "world_%s_%s" % (b, p)
            if not want(name):
                continue
            rig.set(b, p)
            rig.mode("world")
            rig.frame(int(60 * args.secs))
            emit(name)
            silence()
    for wx in ["rain", "storm", "snow", "fog"]:
        name = "weather_%s" % wx
        if not want(name):
            continue
        rig.set("tundra" if wx == "snow" else "meadow", "day", wx)
        rig.mode("world")
        rig.frame(int(60 * 30))
        emit(name)
        silence()
    if want("journey"):
        # walk across biomes through a day, with footsteps, beacons, a map peek, sitting
        rig.set("meadow", "dawn")
        rig.mode("wake")
        rig.frame(60 * 8)
        rig.set("meadow", "day")
        for i in range(60 * 20):
            if i % 18 == 0:
                rig.sfx("step_soft")
            rig.frame(1)
        rig.beacons(1)
        rig.set("forest", "day")
        for i in range(60 * 20):
            if i % 18 == 0:
                rig.sfx("step_soft")
            rig.frame(1)
        rig.mode("map")
        rig.sfx("map")
        rig.frame(60 * 6)
        rig.mode("world")
        rig.sfx("map")
        rig.set("desert", "dusk")
        for i in range(60 * 20):
            if i % 20 == 0:
                rig.sfx("step_sand")
            rig.frame(1)
        rig.beacons(3)
        rig.set("desert", "night")
        rig.sfx("sit")
        rig.tempo(1)
        rig.frame(60 * 20)
        rig.tempo(0)
        rig.set("desert", "dawn")
        rig.frame(60 * 15)
        emit("journey")
        silence()
    if want("wake"):
        rig.set("meadow", "dawn")
        rig.mode("wake")
        rig.frame(60 * 20)
        emit("wake")
        silence()
    if want("beacons"):
        rig.set("meadow", "night")
        rig.beacons(7)
        rig.mode("world")
        rig.frame(60 * 40)
        emit("beacons_night")
        rig.beacons(0)
        silence()
    if want("ending"):
        rig.set("meadow", "day")
        rig.beacons(7)
        rig.mode("world")
        rig.frame(60 * 6, keep=False)
        rig.take()
        rig.mode("ending")
        rig.frame(60 * 45)
        emit("ending")
        rig.beacons(0)
        silence()
    for i, s in enumerate(SFX):
        name = "sfx_%02d_%s" % (i, s)
        if not want(name):
            continue
        n = 6 if s.startswith("step") else 1
        for k in range(n):
            rig.sfx(s)
            rig.frame(18)
        rig.frame(150)
        emit(name)
    if want("steps_over_wind"):
        rig.set("tundra", "day", "snow")
        rig.mode("world")
        rig.frame(60, keep=False)
        rig.take()
        for i in range(60 * 12):
            if i % 18 == 0:
                rig.sfx("step_snow")
            rig.frame(1)
        emit("steps_over_wind")
        silence()
    if allc:
        allc.sort()
        print("ALL: %s  (%.1f%% of a 17556 M-cycle frame at max)" %
              (cstats(allc), allc[-1] * 100.0 / 17556))


if __name__ == "__main__":
    main()
