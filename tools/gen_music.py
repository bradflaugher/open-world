#!/usr/bin/env python3
"""gen_music.py - generate src/gb/sound_data.h for the OPEN WORLD sound engine.

The music itself is generative (see src/gb/sound.c); this script only builds the
fixed data: the note -> frequency-register table, the CH3 drone wavetables, the
sfx register scripts and the few composed motifs (title, wake, ending).

    python3 tools/gen_music.py          # rewrites src/gb/sound_data.h

sfx script bytes:  reg(0x10..0x3F), value   -> one APU register write
                   0x40 + n                 -> wait n+1 frames
                   0x00                     -> end (channels are handed back)
motif bytes (triples):  op, arg, wait_frames
                   op < 0x80  : CH1 note (index into snd_freq), arg = NR12 envelope
                   0xF0       : drone note (arg = index; the wave channel sounds an octave lower)
                   0xF3       : CH1 duty (arg = NR11 value)
                   0xF4       : echo feedback decrement (arg)
                   0xF5       : master fade-out, arg = frames per NR50 step
                   0xFD       : jump to triple number arg
                   0xFF       : end of motif
"""
import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src", "gb", "sound_data.h")

MIDI_LO, MIDI_HI = 36, 100          # C2 .. E7 (pulse); wave sounds an octave lower
NAMES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def midi(name):
    """'D4', 'F#5', 'Bb3' -> MIDI number."""
    n = NAMES[name[0]]
    i = 1
    while name[i] in "#b":
        n += 1 if name[i] == "#" else -1
        i += 1
    return n + 12 * (int(name[i:]) + 1)


def idx(name):
    m = midi(name)
    assert MIDI_LO <= m <= MIDI_HI, name
    return m - MIDI_LO


def freq_reg(m):
    hz = 440.0 * 2 ** ((m - 69) / 12.0)
    return int(round(2048 - 131072.0 / hz))


FREQ = [freq_reg(m) for m in range(MIDI_LO, MIDI_HI + 1)]

# ---------------------------------------------------------------- wavetables
WAVES = [
    # name,   [(harmonic, amplitude, phase)]
    ("SOFT",   [(1, 1.0, 0), (2, 0.18, 0.3), (3, 0.05, 0)]),                  # sea / shore
    ("WARM",   [(1, 1.0, 0), (2, 0.45, 0.2), (3, 0.22, 0.5), (4, 0.08, 0)]),   # meadow, title
    ("HOLLOW", [(1, 1.0, 0), (3, 0.33, 0.1), (5, 0.12, 0.4)]),                # forest
    ("REED",   [(1, 0.8, 0), (2, 0.25, 0), (3, 0.45, 0.2), (5, 0.3, 0.5), (7, 0.15, 0)]),  # desert
    ("GLASS",  [(1, 0.45, 0), (4, 0.6, 0.1), (6, 0.45, 0.3), (9, 0.2, 0.6)]),  # tundra: D2 + D4 + A4 + E5
    ("LOW",    [(n, 1.0 / n ** 1.35, 0) for n in range(1, 9)]),               # rock
    ("ASH",    [(1, 1.0, 0), (5, 0.3, 0.2), (7, 0.42, 0.7), (11, 0.22, 0.1)]), # ash / glass: 7th + 11th partials
    ("DARK",   [(1, 1.0, 0), (2, 0.28, 0.4), (3, 0.08, 0), (4, 0.12, 0.2)]),   # ruins
]


def wavetable(harm):
    s = []
    for k in range(32):
        t = 2 * math.pi * k / 32
        s.append(sum(a * math.sin(h * t + 2 * math.pi * p) for h, a, p in harm))
    lo, hi = min(s), max(s)
    q = [int(round((v - lo) / (hi - lo) * 15)) for v in s]
    return [(q[2 * i] << 4) | q[2 * i + 1] for i in range(16)]


# ---------------------------------------------------------------- sfx DSL
NR10, NR11, NR12, NR13, NR14 = 0x10, 0x11, 0x12, 0x13, 0x14
NR21, NR22, NR23, NR24 = 0x16, 0x17, 0x18, 0x19
NR41, NR42, NR43, NR44 = 0x20, 0x21, 0x22, 0x23
DUTY = {12: 0x00, 25: 0x40, 50: 0x80, 75: 0xC0}


class S:
    def __init__(self):
        self.b = []

    def w(self, r, v):
        self.b += [r, v & 0xFF]
        return self

    def wait(self, n):
        while n > 0:
            k = min(n, 190)
            self.b.append(0x40 + k - 1)
            n -= k
        return self

    def p2(self, note, env, duty=50):           # CH2 note, retriggered
        f = FREQ[idx(note)]
        return self.w(NR21, DUTY[duty]).w(NR22, env).w(NR23, f & 0xFF).w(NR24, 0x80 | (f >> 8))

    def p1(self, note, env, duty=50):           # CH1 note, retriggered (sweep off)
        f = FREQ[idx(note)]
        return self.w(NR10, 0).w(NR11, DUTY[duty]).w(NR12, env).w(NR13, f & 0xFF).w(NR14, 0x80 | (f >> 8))

    def f2(self, note):                          # CH2 pitch change, no retrigger
        f = FREQ[idx(note)]
        return self.w(NR23, f & 0xFF).w(NR24, f >> 8)

    def nz(self, env, poly, length=None):        # CH4 noise, retriggered
        if length is None:
            return self.w(NR41, 0).w(NR42, env).w(NR43, poly).w(NR44, 0x80)
        return self.w(NR41, 64 - length).w(NR42, env).w(NR43, poly).w(NR44, 0xC0)

    def poly(self, poly):                        # CH4 colour change, no retrigger
        return self.w(NR43, poly)

    def end(self):
        return self.b + [0]


CH1, CH2, CH4 = 1, 2, 8

# steps: (variants) — tiny, soft, randomised by the driver
STEPS = {
    "STEP_SOFT":  [S().nz(0x22, 0x71).wait(5).end(),
                   S().nz(0x22, 0x62).wait(5).end(),
                   S().nz(0x22, 0x63).wait(5).end()],
    "STEP_SAND":  [S().nz(0x23, 0x31).wait(3).poly(0x41).wait(4).end(),
                   S().nz(0x23, 0x32).wait(3).poly(0x42).wait(4).end(),
                   S().nz(0x13, 0x21).wait(3).poly(0x31).wait(4).end()],
    "STEP_SNOW":  [S().nz(0x31, 0x5D).wait(2).nz(0x21, 0x4C).wait(2).nz(0x11, 0x3D).wait(3).end(),
                   S().nz(0x31, 0x4D).wait(2).nz(0x21, 0x5C).wait(3).end(),
                   S().nz(0x21, 0x5E).wait(2).nz(0x21, 0x4D).wait(2).nz(0x11, 0x3C).wait(3).end()],
    "STEP_STONE": [S().nz(0x20, 0x42, 4).wait(4).end(),
                   S().nz(0x20, 0x43, 4).wait(4).end(),
                   S().nz(0x20, 0x51, 3).wait(4).end()],
}


def sfx_light():
    s = S().nz(0x0A, 0x55).wait(6)                       # breath of air rising
    s.p2("D5", 0x55).wait(2)
    s.nz(0x57, 0x45).wait(6).poly(0x56).wait(6)          # flame catches, settles
    s.p2("A5", 0x46).poly(0x66).wait(10).poly(0x67).wait(40)
    return s.end()


def sfx_burn():
    s = S()
    for env, poly, wt in [(0x61, 0x31, 3), (0x41, 0x51, 2), (0x51, 0x21, 4), (0x62, 0x41, 5),
                          (0x31, 0x31, 3), (0x42, 0x52, 6), (0x21, 0x41, 4), (0x33, 0x61, 8),
                          (0x21, 0x51, 6), (0x22, 0x62, 10)]:
        s.nz(env, poly).wait(wt)
    return s.end()


def sfx_cairn():
    s = S().nz(0x30, 0x53, 5).p2("D4", 0x51, 12).wait(8)
    s.nz(0x20, 0x52, 4).p2("A3", 0x31, 12).wait(10)
    return s.end()


def sfx_stepstone():
    s = S().p2("A4", 0x63, 50).wait(1)
    for n in ["C5", "E5", "G5", "A5"]:
        s.f2(n).wait(1)
    s.nz(0x33, 0x32).wait(4).nz(0x23, 0x42).wait(20)
    return s.end()


def sfx_pickup():
    return S().nz(0x21, 0x51).p2("E5", 0x42, 25).wait(4).p2("A5", 0x43, 25).wait(16).end()


def sfx_glide():
    s = S()
    for v, p in [(1, 0x55), (2, 0x54), (3, 0x44), (4, 0x43), (4, 0x43), (3, 0x44), (2, 0x54), (1, 0x55)]:
        s.nz(v << 4, p).wait(5)
    return s.nz(0x13, 0x56).wait(4).end()


def sfx_land():
    return S().nz(0x43, 0x72).wait(3).poly(0x74).wait(12).end()


def sfx_no():
    return S().p2("D3", 0x42, 25).wait(7).p2("C#3", 0x42, 25).wait(10).end()


def sfx_item():
    s = S().p1("D4", 0x67, 50).p2("A4", 0x77, 50).wait(10)
    s.p2("D5", 0x77).wait(10).p2("E5", 0x77).wait(10)
    s.p1("D5", 0x67, 25).p2("A5", 0x87).wait(64)
    return s.end()


def sfx_beacon():
    s = S().nz(0x0C, 0x66).wait(18)                       # the column of light draws breath
    s.nz(0x57, 0x55).p1("D3", 0x97, 50).p2("A3", 0x87, 50).wait(12)
    s.poly(0x66).wait(10).poly(0x77).wait(8)
    s.p2("D5", 0x77).wait(24).p2("A5", 0x67).wait(6)
    s.p1("D4", 0x57, 50).wait(18).p2("E6", 0x57).wait(60)
    return s.end()


def sfx_heart():
    s = S()
    seq = [("p1", "D5", 0x77), ("p2", "F#5", 0x77), ("p1", "A5", 0x67), ("p2", "D6", 0x57),
           ("p1", "E6", 0x47), ("p2", "A5", 0x47)]
    for ch, n, env in seq:
        if ch == "p1":
            s.p1(n, env, 50)
        else:
            s.p2(n, env, 25)
        s.wait(14)
    return s.wait(60).end()


def sfx_whiteout():
    s = S()
    s.p2("A5", 0x57, 12)
    for v in range(1, 8):
        s.nz(v << 4, 0x21 if v < 5 else 0x31).wait(3)
        if v in (2, 4, 6):
            s.f2(["G#5", "F#5", "E5"][v // 2 - 1])
    s.nz(0x77, 0x31)
    for n in ["D5", "C#5", "B4", "A4"]:
        s.wait(6).f2(n)
    return s.wait(40).end()


def sfx_dawn():
    return S().p2("D5", 0x47).wait(20).p2("E5", 0x47).wait(20).p2("A5", 0x57).wait(56).end()


def sfx_select():
    return S().p2("A5", 0x31, 25).wait(6).end()


def sfx_map():
    s = S()
    for env, poly, wt in [(0x31, 0x21, 3), (0x21, 0x11, 2), (0x31, 0x22, 4), (0x21, 0x12, 6)]:
        s.nz(env, poly).wait(wt)
    return s.end()


def sfx_thunder():
    s = S().nz(0x81, 0x31).wait(3)
    s.nz(0x87, 0x75).wait(10).poly(0x76).wait(12).poly(0x77).wait(14).poly(0x87).wait(20)
    s.nz(0x57, 0x86).wait(20).poly(0x97).wait(40)
    return s.end()


def sfx_sit():
    return S().nz(0x22, 0x72).wait(4).nz(0x12, 0x53).wait(8).end()


# order = sound.h enum; (name, script, mask, priority)
SFX = [
    ("STEP_SOFT", None, CH4, 0),
    ("STEP_SAND", None, CH4, 0),
    ("STEP_SNOW", None, CH4, 0),
    ("STEP_STONE", None, CH4, 0),
    ("LIGHT", sfx_light(), CH2 | CH4, 2),
    ("BURN", sfx_burn(), CH4, 2),
    ("CAIRN", sfx_cairn(), CH2 | CH4, 2),
    ("STEPSTONE", sfx_stepstone(), CH2 | CH4, 2),
    ("PICKUP", sfx_pickup(), CH2 | CH4, 1),
    ("GLIDE", sfx_glide(), CH4, 2),
    ("LAND", sfx_land(), CH4, 1),
    ("NO", sfx_no(), CH2, 1),
    ("ITEM", sfx_item(), CH1 | CH2, 3),
    ("BEACON", sfx_beacon(), CH1 | CH2 | CH4, 3),
    ("HEART", sfx_heart(), CH1 | CH2, 3),
    ("WHITEOUT", sfx_whiteout(), CH2 | CH4, 3),
    ("DAWN", sfx_dawn(), CH2, 2),
    ("SELECT", sfx_select(), CH2, 1),
    ("MAP", sfx_map(), CH4, 1),
    ("THUNDER", sfx_thunder(), CH4, 2),
    ("SIT", sfx_sit(), CH4, 1),
]

# ---------------------------------------------------------------- motifs
def T(*triples):
    out = []
    for t in triples:
        op, a, w = t
        if isinstance(op, str):
            op = idx(op)
        out += [op, a, w]
    return out


def drone(n, w=0):
    return (0xF0, idx(n), w)


# Title: "A D E ... F# E" over D, answered by "A D B . A" (then ". G") over G.
TITLE = T(
    (0xF3, 0x80, 0), (0xF4, 2, 0),
    drone("D4", 40),
    ("A4", 0x77, 24), ("D5", 0x77, 24), ("E5", 0x87, 76), ("F#5", 0x57, 22), ("E5", 0x47, 150),
    drone("G3", 30),
    ("A4", 0x77, 24), ("D5", 0x77, 24), ("B4", 0x87, 76), ("A4", 0x67, 170),
    drone("D4", 60),
    ("A4", 0x77, 24), ("D5", 0x77, 24), ("E5", 0x87, 76), ("F#5", 0x57, 22), ("E5", 0x47, 150),
    drone("G3", 30),
    ("A4", 0x77, 24), ("D5", 0x77, 24), ("B4", 0x87, 76), ("A4", 0x57, 40), ("G4", 0x57, 150),
    drone("D4", 20), ("D6", 0x37, 150),
    (0xFD, 3, 0),
)

# Wake: a soft rising figure (the drone is already fading in).
WAKE = T(
    (0xF3, 0x80, 0), (0xF4, 2, 20),
    ("D4", 0x47, 18), ("E4", 0x57, 18), ("A4", 0x57, 18), ("D5", 0x67, 36), ("E5", 0x47, 90),
    (0xFF, 0, 0),
)

# Ending: the three beacon tones, then the withheld third (F#) arrives and the
# chord blooms, thickens, holds, and the whole world fades out.
_bloom = []
for rep, env in enumerate([0x37, 0x47, 0x57, 0x67]):
    for n in ["D5", "A5", "F#5", "E6"]:
        _bloom.append((n, env, 22 - 3 * rep))
_cloud = []
for rep, env in enumerate([0x77, 0x77, 0x67, 0x57, 0x47]):
    for n in ["D4", "A4", "D5", "F#5", "A5", "E6"]:
        _cloud.append((n, env, 11))
ENDING = T(
    (0xF3, 0x80, 0), (0xF4, 2, 0),
    drone("D4", 60),
    ("D5", 0x57, 70), ("A5", 0x57, 70), ("E6", 0x47, 100),
    ("F#5", 0x67, 120),
    *_bloom,
    drone("D3", 0),
    *_cloud,
    (0xF4, 1, 0),
    ("D5", 0x97, 30), ("F#5", 0x77, 50), ("A4", 0x67, 60),
    (0xF5, 36, 0),
    ("D6", 0x37, 250),
    (0xFF, 0, 0),
)


# ---------------------------------------------------------------- emit
def carr(vals, per=16):
    lines = []
    for i in range(0, len(vals), per):
        lines.append("    " + ", ".join("0x%02X" % v for v in vals[i:i + per]) + ",")
    return "\n".join(lines)


def main():
    o = []
    o.append("/* sound_data.h - GENERATED by tools/gen_music.py; do not edit. Included by sound.c only. */")
    o.append("#ifndef SOUND_DATA_H\n#define SOUND_DATA_H\n")
    o.append("#define SND_NOTE_LO_MIDI %d" % MIDI_LO)
    o.append("#define SND_NUM_NOTES %d" % len(FREQ))
    o.append("static const uint16_t snd_freq[SND_NUM_NOTES] = {")
    for i in range(0, len(FREQ), 8):
        o.append("    " + ", ".join("%4d" % f for f in FREQ[i:i + 8]) + ",")
    o.append("};\n")
    o.append("enum { " + ", ".join("WV_" + n for n, _ in WAVES) + ", NUM_WAVES };")
    o.append("static const uint8_t snd_waves[NUM_WAVES][16] = {")
    for n, h in WAVES:
        o.append("    { " + ", ".join("0x%02X" % b for b in wavetable(h)) + " }, /* %s */" % n)
    o.append("};\n")
    total = 0
    for name, var in STEPS.items():
        for k, sc in enumerate(var):
            o.append("static const uint8_t sfx_%s_%d[] = {\n%s\n};" % (name.lower(), k, carr(sc)))
            total += len(sc)
    for name, sc, m, p in SFX:
        if sc is not None:
            o.append("static const uint8_t sfx_%s[] = {\n%s\n};" % (name.lower(), carr(sc)))
            total += len(sc)
    o.append("#define SND_STEP_VARIANTS 3")
    o.append("static const uint8_t * const snd_step_var[4][SND_STEP_VARIANTS] = {")
    for name in STEPS:
        o.append("    { " + ", ".join("sfx_%s_%d" % (name.lower(), k) for k in range(3)) + " },")
    o.append("};")
    o.append("static const uint8_t * const snd_sfx_data[%d] = {" % len(SFX))
    for name, sc, m, p in SFX:
        o.append("    %s, /* SFX_%s */" % ("sfx_%s_0" % name.lower() if sc is None else "sfx_" + name.lower(), name))
    o.append("};")
    o.append("static const uint8_t snd_sfx_mask[%d] = { %s };" % (len(SFX), ", ".join(str(m) for _, _, m, _ in SFX)))
    o.append("static const uint8_t snd_sfx_prio[%d] = { %s };\n" % (len(SFX), ", ".join(str(p) for _, _, _, p in SFX)))
    for name, mo in [("title", TITLE), ("wake", WAKE), ("ending", ENDING)]:
        o.append("static const uint8_t mo_%s[] = {\n%s\n};" % (name, carr(mo, 15)))
        total += len(mo)
    o.append("\n#define SND_DATA_BYTES %d\n" % (total + 2 * len(FREQ) + 16 * len(WAVES)))
    o.append("#endif")
    with open(OUT, "w") as f:
        f.write("\n".join(o) + "\n")
    print("wrote %s (%d bytes of data)" % (OUT, total + 2 * len(FREQ) + 16 * len(WAVES)))


if __name__ == "__main__":
    main()
