/* test_sound.c - host-side tests for the OPEN WORLD generative sound engine.
 *
 *   make build/test_sound && ./build/test_sound
 *   (gcc -std=c99 -DHOST_TEST -Isrc/gb -Isrc/core tests/test_sound.c src/gb/sound.c)
 *
 * Every APU register write goes through host_snd_write() (hw_sound.h); the hook below
 * validates each one against the hardware rules and keeps per-test statistics.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sound.h"
#include "hw_sound.h"
#include "world.h"

static int n_pass, n_fail;
#define CHECK(cond, ...) do { if (cond) n_pass++; else { n_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- write monitor ---- */
static unsigned long w_total, w_bad_reg, w_bad_nrx4, w_bad_wave, w_bad_52, w_ch3_retrig, w_wave_loud;
static unsigned long w_music_on_owned, w_sfx_outside, w_sfx_wave;
static unsigned long trig[4], trig_music[4], wave_loads;
static unsigned long tick_writes, max_tick_writes, sum_tick_writes, n_ticks;
static uint8_t sfx_mask_now;
static uint8_t ch3_playing;
static uint32_t log_hash;
static uint16_t ch1_freq_seen[2048 / 16];

static int reg_channel(uint8_t r)
{
    if (r >= 0x10 && r <= 0x14) return 0;
    if (r >= 0x16 && r <= 0x19) return 1;
    if ((r >= 0x1A && r <= 0x1E) || (r >= 0x30 && r <= 0x3F)) return 2;
    if (r >= 0x20 && r <= 0x23) return 3;
    return -1;
}

static void hook(uint8_t r, uint8_t v)
{
    int c = reg_channel(r);
    w_total++;
    tick_writes++;
    log_hash = (log_hash ^ (uint32_t)((r << 8) | v)) * 16777619u;
    if (!((r >= 0x10 && r <= 0x26 && r != 0x15 && r != 0x1F) || (r >= 0x30 && r <= 0x3F)))
        w_bad_reg++;
    if ((r == 0x14 || r == 0x19 || r == 0x1E) && (v & 0x38))
        w_bad_nrx4++;                           /* junk bits / 11-bit overflow */
    if (r == 0x23 && (v & 0x3F))
        w_bad_nrx4++;
    if (r >= 0x30 && r <= 0x3F) {
        if (host_snd_regs[0x1A] & 0x80)
            w_bad_wave++;                       /* wave RAM written with the CH3 DAC on */
        if (r == 0x30)
            wave_loads++;
    }
    if (r == 0x1A) {
        if (!(v & 0x80)) {
            if (ch3_playing && host_snd_regs[0x1C] != 0x00 && host_snd_regs[0x24] != 0)
                w_wave_loud++;                  /* DAC switched off while the drone was audible */
            ch3_playing = 0;
        }
    }
    if (r == 0x1E && (v & 0x80)) {
        if (ch3_playing)
            w_ch3_retrig++;                     /* DMG: retrigger while playing corrupts wave RAM */
        ch3_playing = 1;
    }
    if (r == 0x26 && v != 0x80)
        w_bad_52++;
    if (c >= 0) {
        if ((r == 0x14 || r == 0x19 || r == 0x1E || r == 0x23) && (v & 0x80)) {
            trig[c]++;
            if (host_snd_src == SND_SRC_MUSIC)
                trig_music[c]++;
            if (c == 0 && host_snd_src == SND_SRC_MUSIC) {
                unsigned f = host_snd_regs[0x13] | ((v & 7) << 8);
                ch1_freq_seen[f >> 4] |= (uint16_t)(1u << (f & 15));
            }
        }
        if (host_snd_src == SND_SRC_MUSIC && (sound_debug_owned() & (1 << c)))
            w_music_on_owned++;
        if (host_snd_src == SND_SRC_SFX && !(sfx_mask_now & (1 << c)))
            w_sfx_outside++;
        if (host_snd_src == SND_SRC_SFX && c == 2)
            w_sfx_wave++;
    }
    if (host_snd_src == SND_SRC_SFX && c < 0)
        w_sfx_outside++;
}

static int seen_ch1(unsigned f) { return (ch1_freq_seen[f >> 4] >> (f & 15)) & 1; }

static void reset_stats(void)
{
    w_total = w_bad_reg = w_bad_nrx4 = w_bad_wave = w_bad_52 = w_ch3_retrig = w_wave_loud = 0;
    w_music_on_owned = w_sfx_outside = w_sfx_wave = 0;
    memset(trig, 0, sizeof trig);
    memset(trig_music, 0, sizeof trig_music);
    memset(ch1_freq_seen, 0, sizeof ch1_freq_seen);
    wave_loads = 0;
}

static void check_clean(const char *what)
{
    CHECK(w_bad_reg == 0, "%s: %lu writes to invalid registers", what, w_bad_reg);
    CHECK(w_bad_nrx4 == 0, "%s: %lu bad NRx4 values", what, w_bad_nrx4);
    CHECK(w_bad_wave == 0, "%s: %lu wave RAM writes with DAC on", what, w_bad_wave);
    CHECK(w_ch3_retrig == 0, "%s: %lu CH3 retriggers while playing", what, w_ch3_retrig);
    CHECK(w_bad_52 == 0, "%s: %lu bad NR52 writes", what, w_bad_52);
    CHECK(w_music_on_owned == 0, "%s: %lu music writes to sfx-owned channels", what, w_music_on_owned);
    CHECK(w_sfx_outside == 0, "%s: %lu sfx writes outside mask", what, w_sfx_outside);
    CHECK(w_sfx_wave == 0, "%s: %lu sfx writes to the wave channel", what, w_sfx_wave);
}

static void tick(void)
{
    tick_writes = 0;
    sound_tick();
    if (tick_writes > max_tick_writes)
        max_tick_writes = tick_writes;
    sum_tick_writes += tick_writes;
    n_ticks++;
}

static void ticks(int n)
{
    while (n-- > 0)
        tick();
}

static void fresh(void)
{
    memset(host_snd_regs, 0, sizeof host_snd_regs);
    ch3_playing = 0;
    sound_init();
    reset_stats();
}

static int run_sfx_until_done(int limit)
{
    int f = 0;
    while (f < limit) {
        tick();
        f++;
        if (!sfx_playing())
            break;
    }
    return f;
}

static const char *mode_names[NUM_AMB] = { "SILENT", "TITLE", "WORLD", "MAP", "ENDING", "WAKE" };

/* freq register for a MIDI note, like tools/gen_music.py */
static unsigned note_reg(int midi)
{
    static const unsigned tab[] = { /* D5 A5 E6 F#5 */ 1825, 1899, 1949, 1871 };
    switch (midi) {
    case 74: return tab[0];
    case 81: return tab[1];
    case 88: return tab[2];
    case 78: return tab[3];
    }
    return 0;
}

/* ------------------------------------------------------------------ tests */
static void test_init(void)
{
    fresh();
    CHECK(host_snd_regs[0x26] == 0x80, "NR52 on");
    CHECK(host_snd_regs[0x24] == 0x77, "NR50 = 0x77");
    CHECK(host_snd_regs[0x25] == 0xFF, "NR51 = 0xFF");
    CHECK(sound_debug_mode() == AMB_SILENT, "silent after init");
    CHECK(!sfx_playing(), "no sfx after init");
    ticks(120);
    CHECK(w_total == 0, "silent mode writes nothing (%lu)", w_total);
    check_clean("idle");
}

static void test_world_all(void)
{
    uint8_t b, ph, wx;
    char what[64];
    for (b = 0; b < B_COUNT; b++)
        for (ph = 0; ph < 4; ph++)
            for (wx = 0; wx <= WX_STORM; wx++) {
                fresh();
                ambient_seed((uint16_t)(0x1111 * (b + 1) + ph));
                ambient_set(b, ph, wx);
                ambient_mode(AMB_WORLD);
                ticks(60 * 50);
                sprintf(what, "world b%u ph%u wx%u", b, ph, wx);
                check_clean(what);
                CHECK(sound_debug_mode() == AMB_WORLD, "%s: mode", what);
                CHECK(trig[0] >= 3, "%s: CH1 phrases (%lu)", what, trig[0]);
                CHECK(trig[1] >= 1, "%s: CH2 echoes (%lu)", what, trig[1]);
                CHECK(trig[2] == 1, "%s: drone triggered once (%lu)", what, trig[2]);
                CHECK(trig[3] >= 5, "%s: wind moves (%lu)", what, trig[3]);
                CHECK(host_snd_regs[0x1A] == 0x80 && host_snd_regs[0x1C] != 0, "%s: drone audible", what);
                CHECK(trig[0] < 60 * 50 / 8, "%s: CH1 not frantic (%lu)", what, trig[0]);
            }
}

static void test_density(void)
{
    /* night must be sparser than day; tempo(1) must be quicker */
    unsigned long day, night, fastn;
    uint8_t b;
    for (b = 0; b < B_COUNT; b++) {
        fresh(); ambient_seed(77); ambient_set(b, PH_DAY, WX_CLEAR); ambient_mode(AMB_WORLD);
        ticks(60 * 120); day = trig[0];
        fresh(); ambient_seed(77); ambient_set(b, PH_NIGHT, WX_CLEAR); ambient_mode(AMB_WORLD);
        ticks(60 * 120); night = trig[0];
        fresh(); ambient_seed(77); ambient_set(b, PH_DAY, WX_CLEAR); ambient_mode(AMB_WORLD);
        ambient_tempo(1);
        ticks(60 * 120); fastn = trig[0];
        CHECK(night * 4 < day * 3, "biome %u: night sparser (%lu vs day %lu)", b, night, day);
        CHECK(fastn > day, "biome %u: tempo(1) quicker (%lu vs %lu)", b, fastn, day);
        CHECK(night >= 4, "biome %u: night not dead (%lu)", b, night);
    }
}

static void test_modes(void)
{
    uint8_t m;
    for (m = 0; m < NUM_AMB; m++) {
        fresh();
        ambient_set(B_MEADOW, m == AMB_WAKE ? PH_DAWN : PH_DAY, WX_CLEAR);
        ambient_mode(m);
        CHECK(sound_debug_mode() == m, "%s: debug mode right after request", mode_names[m]);
        ticks(60 * 30);
        check_clean(mode_names[m]);
        if (m == AMB_SILENT) {
            CHECK(w_total == 0, "SILENT: no writes (%lu)", w_total);
            continue;
        }
        CHECK(trig[0] >= 3, "%s: CH1 notes (%lu)", mode_names[m], trig[0]);
        CHECK(trig[2] >= 1, "%s: drone (%lu)", mode_names[m], trig[2]);
        CHECK(trig[3] >= 1, "%s: wind (%lu)", mode_names[m], trig[3]);
        if (m == AMB_WAKE)
            CHECK(sound_debug_mode() == AMB_WORLD, "WAKE flows into WORLD (%u)", sound_debug_mode());
        else
            CHECK(sound_debug_mode() == m, "%s: mode held (%u)", mode_names[m], sound_debug_mode());
    }
    /* MAP: muffled mix, CH1 dropped, restored on return */
    fresh();
    ambient_set(B_FOREST, PH_DAY, WX_CLEAR);
    ambient_mode(AMB_WORLD);
    ticks(600);
    ambient_mode(AMB_MAP);
    tick();
    CHECK((host_snd_regs[0x25] & 0x11) == 0, "MAP drops CH1 (NR51 %02x)", host_snd_regs[0x25]);
    CHECK(host_snd_regs[0x24] < 0x77, "MAP lowers NR50 (%02x)", host_snd_regs[0x24]);
    ticks(600);
    CHECK(trig[1] > 0 && trig[2] > 0, "MAP keeps echo + drone");
    CHECK((host_snd_regs[0x25] & 0x11) == 0, "MAP keeps CH1 dropped (NR51 %02x)", host_snd_regs[0x25]);
    sfx_mask_now = 8;
    sfx_play(SFX_MAP);
    run_sfx_until_done(200);
    sfx_mask_now = 0;
    ambient_mode(AMB_WORLD);
    tick();
    CHECK(host_snd_regs[0x24] == 0x77, "WORLD restores NR50 (%02x)", host_snd_regs[0x24]);
    ticks(600);
    CHECK((host_snd_regs[0x25] & 0x11) != 0, "WORLD restores CH1 (NR51 %02x)", host_snd_regs[0x25]);
    check_clean("map");
    /* mute */
    sound_mute_all(1);
    tick();
    CHECK(host_snd_regs[0x25] == 0, "mute: NR51 = 0");
    ticks(100);
    CHECK(host_snd_regs[0x25] == 0, "mute holds while music runs");
    sound_mute_all(0);
    tick();
    CHECK(host_snd_regs[0x25] != 0, "unmute restores NR51");
}

static void test_transitions(void)
{
    uint8_t i;
    int f;
    unsigned long loads;
    /* biome change: applied at a phrase boundary, drone wave swapped with the DAC off after a fade */
    fresh();
    ambient_seed(4242);
    ambient_set(B_MEADOW, PH_DAY, WX_CLEAR);
    ambient_mode(AMB_WORLD);
    ticks(900);
    loads = wave_loads;
    ambient_set(B_ASH, PH_DAY, WX_CLEAR);
    for (f = 0; f < 60 * 12 && wave_loads == loads; f++)
        tick();
    CHECK(wave_loads == loads + 1, "biome change reloads the wave (%lu)", wave_loads - loads);
    CHECK(f > 2, "biome change waits for the phrase boundary (%d frames)", f);
    CHECK(f < 60 * 8, "biome change applied within 8 s (%d frames)", f);
    CHECK(w_wave_loud == 0, "drone faded before DAC off (%lu)", w_wave_loud);
    ticks(300);
    CHECK(host_snd_regs[0x1C] != 0, "new drone faded in (%02x)", host_snd_regs[0x1C]);
    check_clean("biome change");
    /* walk through every biome / phase repeatedly */
    for (i = 0; i < 80; i++) {
        ambient_set((uint8_t)((i * 7) % B_COUNT), (uint8_t)((i / 3) & 3), (uint8_t)((i / 5) % 5));
        ticks(97 + (i & 3) * 71);
    }
    check_clean("biome walk");
    CHECK(w_wave_loud == 0, "biome walk: drone always faded before DAC off (%lu)", w_wave_loud);
    /* mode crossfades title -> world -> title */
    fresh();
    ambient_mode(AMB_TITLE);
    ticks(600);
    ambient_mode(AMB_WORLD);
    CHECK(sound_debug_mode() == AMB_WORLD, "debug mode reports the requested mode");
    ticks(20);
    ambient_mode(AMB_TITLE);                    /* retarget mid-fade */
    ticks(600);
    CHECK(sound_debug_mode() == AMB_TITLE, "retargeted fade ends in TITLE");
    ambient_mode(AMB_SILENT);
    ticks(120);
    CHECK(!(host_snd_regs[0x1A] & 0x80) && host_snd_regs[0x21] == 0, "SILENT: drone + wind off");
    {
        unsigned long before = w_total;
        ticks(300);
        CHECK(w_total == before, "SILENT: quiet (%lu writes)", w_total - before);
    }
    check_clean("crossfades");
    /* wake + world requested in the same frame: the figure still plays */
    fresh();
    ambient_set(B_MEADOW, PH_DAWN, WX_CLEAR);
    ambient_mode(AMB_WAKE);
    ambient_mode(AMB_WORLD);
    ticks(30);
    CHECK(sound_debug_mode() == AMB_WAKE, "WAKE kept when WORLD follows at once");
    ticks(60 * 8);
    CHECK(sound_debug_mode() == AMB_WORLD, "then WORLD");
    check_clean("wake");
}

static void test_beacons_ending(void)
{
    int f;
    unsigned long before;
    /* rock by day: the pool has neither A5 nor E6, so those come only from beacons */
    fresh();
    ambient_seed(9);
    ambient_set(B_ROCK, PH_DAY, WX_CLEAR);
    ambient_mode(AMB_WORLD);
    ticks(60 * 60);
    CHECK(!seen_ch1(note_reg(81)) && !seen_ch1(note_reg(88)), "no beacon tones unlit");
    reset_stats();
    ambient_beacons(1);
    ticks(60 * 60);
    CHECK(seen_ch1(note_reg(74)), "beacon 0 adds D5");
    CHECK(!seen_ch1(note_reg(81)), "beacon 1 unlit: no A5");
    ambient_beacons(7);
    reset_stats();
    ticks(60 * 60);
    CHECK(seen_ch1(note_reg(74)) && seen_ch1(note_reg(81)) && seen_ch1(note_reg(88)),
          "all beacons: D5 A5 E6 recur");
    check_clean("beacons");
    /* ending: blooms (F#5 arrives), fades NR50 to 0, then silence */
    ambient_mode(AMB_ENDING);
    reset_stats();
    for (f = 0; f < 60 * 90; f++) {
        tick();
        if (f > 60 && !(host_snd_regs[0x1A] & 0x80) && host_snd_regs[0x24] == 0)
            break;
    }
    CHECK(f < 60 * 90, "ending finishes within 90 s (%d)", f);
    CHECK(f > 60 * 20, "ending takes its time (%d frames)", f);
    CHECK(seen_ch1(note_reg(78)), "ending resolves to the third (F#5)");
    CHECK(seen_ch1(note_reg(74)) && seen_ch1(note_reg(81)) && seen_ch1(note_reg(88)), "ending uses the beacon tones");
    CHECK(sound_debug_mode() == AMB_ENDING, "ending mode held");
    before = w_total;
    ticks(600);
    CHECK(w_total == before, "after the ending: silence (%lu writes)", w_total - before);
    check_clean("ending");
    ambient_mode(AMB_TITLE);
    ticks(300);
    CHECK(host_snd_regs[0x24] == 0x77, "title after ending restores NR50");
    CHECK(trig[0] > 0 && (host_snd_regs[0x1A] & 0x80), "title after ending plays");
    check_clean("after ending");
}

static void test_sfx(void)
{
    uint8_t i, bg;
    static const uint8_t mask_expect_no_wave = 0x0B;
    for (bg = 0; bg < 3; bg++) {
        for (i = 0; i < NUM_SFX; i++) {
            int f;
            char what[48];
            fresh();
            if (bg == 1) {
                ambient_set(B_TUNDRA, PH_DAY, WX_STORM);
                ambient_mode(AMB_WORLD);
                ticks(400);
            } else if (bg == 2) {
                ambient_mode(AMB_TITLE);
                ticks(333);
            }
            reset_stats();
            sfx_mask_now = mask_expect_no_wave;
            sfx_play(i);
            CHECK(sfx_playing(), "sfx %u pending", i);
            tick();
            CHECK(sound_debug_owned() != 0, "sfx %u owns channels", i);
            CHECK(!(sound_debug_owned() & 4), "sfx %u never takes the wave channel", i);
            sfx_mask_now = sound_debug_owned();
            f = run_sfx_until_done(600);
            sprintf(what, "sfx %u bg%u", i, bg);
            CHECK(f < 250, "%s: finished (%d frames)", what, f);
            CHECK(sound_debug_owned() == 0, "%s: channels released", what);
            if (sfx_mask_now & 1)
                CHECK(host_snd_regs[0x10] == 0, "%s: sweep cleared", what);
            check_clean(what);
            if (bg == 0) {
                CHECK(host_snd_regs[0x12] == 0 || !(sfx_mask_now & 1), "%s: CH1 silent after", what);
                CHECK(host_snd_regs[0x17] == 0 || !(sfx_mask_now & 2), "%s: CH2 silent after", what);
                CHECK(host_snd_regs[0x21] == 0 || !(sfx_mask_now & 8), "%s: CH4 silent after", what);
            } else {
                unsigned long t3 = trig_music[3], t0 = trig_music[0];
                if (sfx_mask_now & 8) {
                    /* the wind (or rain) comes straight back */
                    CHECK(host_snd_regs[0x21] != 0 || bg == 2, "%s: wind restored (NR42 %02x)", what, host_snd_regs[0x21]);
                }
                ticks(900);
                CHECK(trig_music[0] > t0, "%s: music resumes on CH1", what);
                if (bg == 1)
                    CHECK(trig_music[3] > t3, "%s: wind resumes", what);
                check_clean(what);
            }
            sfx_mask_now = 0;
        }
    }
    /* footsteps: tiny, soft, varied */
    {
        uint8_t s, seen_poly[256];
        for (s = SFX_STEP_SOFT; s <= SFX_STEP_STONE; s++) {
            int k, f, distinct = 0, maxvol = 0;
            fresh();
            memset(seen_poly, 0, sizeof seen_poly);
            for (k = 0; k < 40; k++) {
                sfx_mask_now = 8;
                sfx_play(s);
                tick();
                if (!seen_poly[host_snd_regs[0x22]]++)
                    distinct++;
                if ((host_snd_regs[0x21] >> 4) > maxvol)
                    maxvol = host_snd_regs[0x21] >> 4;
                f = run_sfx_until_done(60);
                CHECK(f <= 10, "step %u short (%d frames)", s, f);
            }
            CHECK(distinct >= 2, "step %u varies (%d colours)", s, distinct);
            CHECK(maxvol <= 3, "step %u soft (vol %d)", s, maxvol);
            check_clean("steps");
        }
    }
    /* priorities: a footstep never cuts a beacon; the beacon cuts a glide */
    fresh();
    ambient_set(B_MEADOW, PH_DAY, WX_CLEAR);
    ambient_mode(AMB_WORLD);
    ticks(300);
    sfx_mask_now = 0x0B;
    sfx_play(SFX_GLIDE);
    ticks(3);
    CHECK(sound_debug_owned() == 8, "glide owns CH4 (%02x)", sound_debug_owned());
    sfx_play(SFX_BEACON);
    tick();
    CHECK(sound_debug_owned() == 0x0B, "beacon takes CH1+CH2+CH4 (%02x)", sound_debug_owned());
    sfx_play(SFX_STEP_SOFT);
    ticks(2);
    CHECK(sound_debug_owned() == 0x0B, "step does not cut the beacon (%02x)", sound_debug_owned());
    run_sfx_until_done(600);
    CHECK(sound_debug_owned() == 0, "all released");
    /* two voices layered: glide (CH4) + select (CH2) */
    sfx_play(SFX_GLIDE);
    sfx_play(SFX_SELECT);
    ticks(2);
    CHECK(sound_debug_owned() == 0x0A, "glide + select layered (%02x)", sound_debug_owned());
    run_sfx_until_done(600);
    /* step spam settles */
    {
        int k;
        for (k = 0; k < 200; k++) { sfx_play((uint8_t)(k & 3)); ticks(3); }
        CHECK(run_sfx_until_done(100) < 12, "step spam settles");
        for (k = 0; k < 20; k++) sfx_play(SFX_SELECT);   /* queue overflow: dropped, no harm */
        CHECK(run_sfx_until_done(300) < 300, "queue overflow drains");
    }
    sfx_mask_now = 0;
    check_clean("priorities");
    sfx_play(NUM_SFX);
    sfx_play(255);
    CHECK(!sfx_playing(), "invalid sfx ignored");
}

static uint32_t run_script(uint16_t seed, int frames)
{
    int f;
    unsigned r = 99;
    fresh();
    log_hash = 2166136261u;
    ambient_seed(seed);
    ambient_set(B_SHORE, PH_DUSK, WX_CLEAR);
    ambient_mode(AMB_WORLD);
    for (f = 0; f < frames; f++) {
        r = r * 1103515245u + 12345u;
        if (((r >> 16) & 511) == 7)
            ambient_set((uint8_t)((r >> 8) % B_COUNT), (uint8_t)((r >> 4) & 3), 0);
        if (f % 20 == 0)
            sfx_play(SFX_STEP_SAND);
        tick();
    }
    return log_hash;
}

static void test_determinism(void)
{
    uint32_t a = run_script(0xBEEF, 20000);
    uint32_t b = run_script(0xBEEF, 20000);
    uint32_t c = run_script(0xBEF0, 20000);
    CHECK(a == b, "same seed -> identical register stream");
    CHECK(a != c, "different seed -> different music");
}

static void test_never_repeats(void)
{
    /* the CH1 note stream over 10 minutes should not be periodic with a short period */
    static uint8_t notes[4000];
    int n = 0, f, p, best = 0;
    fresh();
    ambient_seed(31337);
    ambient_set(B_MEADOW, PH_DAY, WX_CLEAR);
    ambient_mode(AMB_WORLD);
    for (f = 0; f < 60 * 600 && n < 4000; f++) {
        unsigned long t = trig_music[0];
        tick();
        if (trig_music[0] != t)
            notes[n++] = host_snd_regs[0x13];
    }
    for (p = 1; p < 200; p++) {
        int i, same = 0;
        for (i = 0; i + p < n; i++)
            same += notes[i] == notes[i + p];
        if (same * 100 / (n - p) > best)
            best = same * 100 / (n - p);
    }
    CHECK(n > 200, "10 minutes of phrases (%d notes)", n);
    CHECK(best < 60, "no short loop: best self-match %d%%", best);
}

static void test_stress(void)
{
    int f;
    unsigned int seed = 12345;
    fresh();
    for (f = 0; f < 200000; f++) {
        unsigned int r;
        seed = seed * 1103515245u + 12345u;
        r = (seed >> 16) & 0x7FFF;
        if (r % 211 == 0) ambient_mode((uint8_t)(r % (NUM_AMB + 2)));
        if (r % 13 == 0) sfx_play((uint8_t)(r % (NUM_SFX + 3)));
        if (r % 97 == 0) ambient_set((uint8_t)(r % (B_COUNT + 2)), (uint8_t)(r % 5), (uint8_t)(r % 7));
        if (r % 307 == 0) ambient_beacons((uint8_t)r);
        if (r % 401 == 0) ambient_tempo((uint8_t)(r & 1));
        if (r % 509 == 0) sound_mute_all((uint8_t)((r >> 2) & 1));
        if (r % 1901 == 0) ambient_seed((uint16_t)r);
        sfx_mask_now = 0x0B;
        tick();
    }
    sfx_mask_now = 0;
    CHECK(w_bad_reg == 0 && w_bad_nrx4 == 0 && w_bad_wave == 0 && w_ch3_retrig == 0, "stress: register rules hold");
    CHECK(w_music_on_owned == 0, "stress: music never writes owned channels");
    CHECK(w_sfx_outside == 0 && w_sfx_wave == 0, "stress: sfx stay in CH1/CH2/CH4");
    sound_mute_all(0);
    CHECK(run_sfx_until_done(600) < 600, "stress: sfx drain");
    ambient_mode(AMB_WORLD);
    ambient_set(B_MEADOW, PH_DAY, WX_CLEAR);
    reset_stats();
    ticks(60 * 20);
    CHECK(trig[0] > 0 && trig[2] <= 1 && (host_snd_regs[0x1A] & 0x80), "stress: world plays afterwards");
    check_clean("after stress");
}

static void test_cost(void)
{
    /* cycle proxy: register writes per tick (the real cost is measured in PyBoy by
       tools/render_audio.py --bench) */
    uint8_t b;
    max_tick_writes = sum_tick_writes = n_ticks = 0;
    for (b = 0; b < B_COUNT; b++) {
        fresh();
        ambient_set(b, PH_DAY, WX_RAIN);
        ambient_mode(AMB_WORLD);
        ticks(3000);
    }
    printf("cost proxy: %.2f writes/tick avg, %lu max\n",
           (double)sum_tick_writes / n_ticks, max_tick_writes);
    CHECK(max_tick_writes <= 40, "at most 40 writes in one tick (%lu)", max_tick_writes);
    CHECK(sum_tick_writes < n_ticks * 3, "avg < 3 writes per tick");
}

int main(void)
{
    host_snd_hook = hook;
    test_init();
    test_world_all();
    test_density();
    test_modes();
    test_transitions();
    test_beacons_ending();
    test_sfx();
    test_determinism();
    test_never_repeats();
    test_stress();
    test_cost();
    printf("test_sound: %d passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
