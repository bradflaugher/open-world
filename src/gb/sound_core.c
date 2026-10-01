/* sound_core.c - OPEN WORLD generative ambient engine + sfx (GBDK-2020, autobanked; the public
 * API and VBL entry are in sound.c, which maps this bank in.  Host: included by sound.c).
 *
 * Music is not a song but a small ecology, after Eno's tape-loop pieces:
 *
 *   CH3 wave   drone pad: one 32-sample wavetable per biome, a root that now and then walks
 *              to a neighbouring degree (4th / 5th / b7), and breathes between 50% and 25%.
 *              Wave RAM is only rewritten while the channel is stopped (faded to 0%, then
 *              halted by its length counter; the DAC stays on).
 *   CH1 pulse  "tape loops": 6 generative loops of unequal prime lengths (17..71 steps) each
 *              carrying one note of the biome's 8-note modal pool, plus a night "star" loop
 *              and three beacon loops (one per lit beacon: D5, A5, E6 - an open sus chord).
 *              Loops phase against each other, so the music never repeats exactly; notes are
 *              re-picked slowly from an xorshift RNG seeded by ambient_seed ^ biome.
 *              Every pool is pentatonic or suspended: no semitones, no tritones.
 *   CH2 pulse  feedback echo of CH1 (dotted step delay, each tap quieter).
 *   CH4 noise  wind: a software envelope (retrigger per level step) with rare soft gusts and
 *              silence between them; slow surf on the coast; rain = a soft steady shower;
 *              storm = rain with low rumbling gusts.  Footsteps borrow it; the wind then
 *              swells back from silence.
 *
 *   Time of day: dawn = rising two-note cells, day = fullest, dusk = falling cells and
 *   thinner, night = slower, lower, two loops + stars; the drone drops an octave.
 *   Changes from ambient_set() are applied at the next phrase boundary (16 steps); drone
 *   changes swap the wave only after a fade.  Mode changes crossfade via a global attenuation.
 *
 * SFX: register scripts (tools/gen_music.py -> sound_data.h) on two voices with priorities.
 * An sfx owns the channels in its mask (never CH3); the ambient keeps its state for an owned
 * channel but does not touch the hardware; on release the channel is silenced (CH1/CH2,
 * picked up by the next note) or the wind swells back from silence (CH4).
 *
 * No pops.  On the hardware a channel whose DAC is on sits at full DC offset even at volume
 * 0, so anything that moves that offset without a note clicks: switching a DAC on or off
 * (NRx2 = 0 / NR30 = 0), re-routing a channel in NR51, jumping NR50, and big steps of the
 * CH3 volume code.  So: every DAC is switched on once in snd_core_init() and stays on
 * (silence is a fast hardware fade to volume 0, never DAC off); NR51 is written once and
 * never changes (NR50 = 0 is the lowest gain, not silence: a mute ramps NR50 down and fades
 * every channel instead); NR50 moves one step per frame; NR32 one step at a time.
 * And every envelope runs downwards: writing NRx2 with the other direction to a playing
 * channel ("zombie mode") throws its volume to 14-15 until the trigger, a sharp click.
 * PyBoy does not model any of this (it mixes digital volume), so tests/test_sound.c checks
 * the register stream for it instead.
 *
 * Every public function only posts a request; all register work happens in sound_tick()
 * (VBL, once per frame).  Hot path: static uint8_t state, no multiply / divide.
 */
#include <stdint.h>
#ifndef HOST_TEST
#pragma bank 255
#include <gb/gb.h>
BANKREF(sound_core)
#endif
#include "sound.h"
#include "hw_sound.h"
#include "sound_int.h"
#include "world.h"
#include "sound_data.h"


/* ------------------------------------------------------------------ constants */
#define REQ_NONE SND_REQ_NONE
#define N_D4 26                     /* snd_freq index of D4 (MIDI 62) */
#define N_D5 38
#define NLOOP 10                    /* 0..5 generative, 6 star, 7..9 beacons */
#define L_STAR 6
#define L_BEACON 7

/* layers */
#define LAY_LOOPS 0x01
#define LAY_MOTIF 0x02
#define LAY_WIND  0x04

/* deferred work */
#define WK_A     0x01
#define WK_B     0x02
#define WK_LOOPS 0x04

/* crossfade */
#define XF_NONE 0
#define XF_OUT  1
#define XF_IN   2

/* drone state */
#define DS_IDLE  0
#define DS_DOWN  1                  /* fade to 0%, then stop the channel (new wave) */
#define DS_SLIDE 2                  /* step the level towards d_tl */
#define DS_LOAD  3                  /* channel stopped: write wave RAM and trigger */

/* cells */
#define CELL_ONE  0
#define CELL_RISE 1
#define CELL_FALL 2
#define CELL_MIX  3

/* wind flags */
#define WF_WAVES  0x01

/* silence with the DAC kept on: written without a trigger, it only changes the envelope pace
   of a playing channel, which then fades to 0 in under 1/64 s (same direction: no zombie
   glitch); a stopped channel just stores it */
#define ENV_FADE 0x11

/* ------------------------------------------------------------------ biome tables */
/* 8-note pools, ascending, as snd_freq indices (N_D4 + semitones) */
#define P(x) (N_D4 + (x))
static const uint8_t b_pool[B_COUNT][8] = {
    { P(-5), P(0), P(5), P(7), P(10), P(12), P(17), P(19) },   /* SEA     suspended 4ths  */
    { P(-5), P(0), P(5), P(7), P(12), P(14), P(17), P(19) },   /* SHALLOW                 */
    { P(-5), P(0), P(2), P(5), P(7), P(12), P(14), P(19) },    /* SHORE                   */
    { P(0), P(2), P(4), P(7), P(9), P(12), P(14), P(16) },     /* MEADOW  major pent.     */
    { P(-2), P(0), P(2), P(5), P(7), P(10), P(12), P(14) },    /* FOREST  sus pent. (C D E G A) */
    { P(0), P(3), P(5), P(7), P(10), P(12), P(15), P(17) },    /* DESERT  minor pent.     */
    { P(0), P(7), P(12), P(14), P(19), P(21), P(24), P(26) },  /* TUNDRA  open 5ths, high */
    { P(-7), P(-5), P(-2), P(0), P(2), P(5), P(7), P(12) },    /* ROCK    low sus         */
    { P(0), P(2), P(7), P(9), P(12), P(14), P(19), P(21) },    /* ASH     5ths, no third  */
    { P(-7), P(-2), P(0), P(3), P(5), P(7), P(10), P(12) },    /* RUINS   minor pent.     */
};
/* drone: wave, 4 weighted notes (index; CH3 sounds an octave lower), level code (1=100%..3=25%) */
static const uint8_t b_wave[B_COUNT] = { WV_SOFT, WV_SOFT, WV_SOFT, WV_WARM, WV_HOLLOW, WV_REED,
                                         WV_GLASS, WV_LOW, WV_ASH, WV_DARK };
static const uint8_t b_drone[B_COUNT][4] = {
    { P(0), P(0), P(5), P(0) },     /* D  G         */
    { P(0), P(5), P(0), P(7) },     /* D  G  A      */
    { P(0), P(0), P(5), P(7) },
    { P(0), P(0), P(7), P(-5) },    /* D  A  A-     */
    { P(0), P(0), P(-2), P(5) },    /* D  C  G      */
    { P(0), P(0), P(7), P(-2) },    /* D  A  C      */
    { P(-12), P(-12), P(-12), P(-5) }, /* low D, A  */
    { P(-12), P(-12), P(-5), P(-7) },  /* D2 A2 G2   */
    { P(0), P(0), P(7), P(5) },     /* D  A  G      */
    { P(0), P(0), P(-2), P(5) },    /* D  C  G      */
};
static const uint8_t b_dlev[B_COUNT]  = { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
static const uint8_t b_pset[B_COUNT]  = { 1, 1, 1, 0, 0, 2, 2, 2, 2, 2 };
static const uint8_t b_skip[B_COUNT]  = { 96, 88, 80, 72, 88, 100, 96, 72, 72, 128 };
static const uint8_t b_vol[B_COUNT]   = { 8, 8, 8, 9, 9, 8, 6, 9, 8, 8 };
static const uint8_t b_pace[B_COUNT]  = { 7, 7, 7, 7, 6, 6, 5, 7, 7, 7 };
static const uint8_t b_duty[B_COUNT]  = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x40, 0x80, 0x80, 0x80, 0x80 };
static const uint8_t b_slow[B_COUNT]  = { 4, 4, 3, 0, 1, 2, 2, 1, 2, 3 };
/* wind: calm, gust, rise, fall, gust probability, calm colour, gust colour, flags.
   Calm is 0 (silence) on land: a constant noise floor is what static sounds like.  The
   colours are dark (shift 5-7): a low whoosh, not a hiss. */
static const uint8_t b_wind[B_COUNT][8] = {
    { 0, 3, 22, 34, 255, 0x67, 0x56, WF_WAVES },
    { 0, 2, 20, 30, 255, 0x67, 0x56, WF_WAVES },
    { 0, 3, 24, 36, 255, 0x67, 0x56, WF_WAVES },
    { 0, 2, 16, 22,  70, 0x66, 0x56, 0 },
    { 0, 2, 14, 18,  80, 0x56, 0x55, 0 },
    { 0, 3, 16, 22,  90, 0x66, 0x56, 0 },
    { 0, 3, 12, 18, 120, 0x56, 0x46, 0 },
    { 0, 3, 14, 20,  90, 0x67, 0x57, 0 },
    { 0, 2, 20, 22,  60, 0x77, 0x67, 0 },
    { 0, 2, 18, 22,  60, 0x67, 0x57, 0 },
};
static const uint8_t wind_title[8] = { 0, 2, 20, 24, 60, 0x66, 0x56, 0 };
static const uint8_t wind_rain[8]  = { 1, 2, 24, 30, 50, 0x46, 0x45, 0 };
static const uint8_t wind_storm[8] = { 1, 3, 18, 26, 70, 0x45, 0x76, 0 };

static const uint8_t pset_per[3][6] = {
    { 17, 19, 23, 29, 31, 37 },
    { 17, 23, 29, 37, 43, 53 },
    { 23, 31, 41, 53, 61, 71 },
};
static const uint8_t other_per[4] = { 47, 41, 59, 71 };  /* star, beacons */
/* phases: dawn, day, dusk, night */
/* one pulse for every phase: night does not slow down (a slower tempo read as the game
 * lagging), it thins out (ph_mask, ph_skip) and sinks (octave down, quieter) instead */
static const uint8_t ph_step[4] = { 16, 16, 16, 16 };
static const uint8_t ph_skip[4] = { 16, 0, 24, 72 };
static const uint8_t ph_mask[4] = { 0x1F, 0x3F, 0x0F, 0x03 };
static const uint8_t ph_vdown[4] = { 1, 0, 1, 2 };
static const uint8_t ph_cell[4] = { CELL_RISE, CELL_MIX, CELL_FALL, CELL_ONE };

static const uint8_t star_pool[8] = { P(19), P(21), P(24), P(26), P(31), P(24), P(19), P(26) };
static const uint8_t btone[3] = { P(12), P(19), P(26) };          /* D5 A5 E6 */
static const uint8_t bit8[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
static const uint8_t nr32_tab[5] = { 0x20, 0x20, 0x40, 0x60, 0x00 };
static const uint8_t datt[5] = { 0, 0, 1, 2, 4 };
static const uint8_t var3[4] = { 0, 1, 2, 1 };
static const uint8_t loop_stagger[NLOOP] = { 3, 9, 6, 14, 11, 19, 30, 24, 37, 50 };

/* ------------------------------------------------------------------ state */
/* requests (main thread -> tick) */

static uint8_t fc;                        /* frame counter (wraps) */
uint8_t snd_cur_mode, snd_xf, snd_xf_next, snd_owned, snd_vo_on;
static uint8_t xf_t, att;
static uint8_t lay, muffle, muted, fast;
static uint8_t m_lvl, mf_rate, mf_t;      /* master level (NR50 target), master fade */
static uint8_t n50;                       /* the level NR50 is at (ramps to m_lvl) */
static uint16_t rng, wrng;

/* committed world params */
static uint8_t c_biome, c_phase, c_wx, c_seeded;
static const uint8_t *pool;
static uint8_t g_mask, g_star, g_vol, g_pace, g_duty, g_skip, g_cell, g_fb, g_night;
static uint8_t step_len, step_t, step_ctr, echo_d, fired, work;
static uint8_t bmask;

/* loops */
static uint8_t lp_cnt[NLOOP], lp_per[NLOOP], lp_pi[6];
static uint8_t lp_stale, last_pick;
static uint8_t pn_t, pn_n, pn_v;

/* echo queue */
static uint8_t eq_n[8], eq_v[8], eq_due[8], eq_pace[8], eq_duty[8];
static uint8_t eq_h, eq_t;

/* drone */
static uint8_t d_on, d_wave, d_note, d_code, d_st, d_t, d_tw, d_tn, d_tl, d_last, d_olv;
/* breathing: in the world the drone swells for 5-10 s, sinks one step (to 25%) for 2-4 s, and
   swells back, and it walks between degrees.  An unbroken tone that never changes is what a
   crashed Game Boy sounds like; a fade to silence and back is four pops (see the top). */
static uint8_t d_br, d_bt, d_blv, d_rng;

/* wind */
static uint8_t w_on, w_lvl, w_tgt, w_tmr, w_rate, w_frate, w_out;
static uint8_t wp_calm, wp_gust, wp_rise, wp_fall, wp_prob, wp_ccol, wp_gcol, wp_flags;

/* motif */
static const uint8_t *mo_p, *mo_base;
static uint8_t mo_t, mo_duty;

/* sfx voices */
static const uint8_t *vo_p[2];
static uint8_t vo_wait[2], vo_mask[2], vo_prio[2];

/* ------------------------------------------------------------------ helpers */
static uint8_t rnd(void)
{
    uint16_t x = rng;
    x ^= x << 7;
    x ^= x >> 9;
    x ^= x << 8;
    rng = x;
    return (uint8_t)(x ^ (x >> 8));
}

static uint8_t wrnd(void)
{
    uint16_t x = wrng;
    x ^= x << 7;
    x ^= x >> 9;
    x ^= x << 8;
    wrng = x;
    return (uint8_t)(x ^ (x >> 8));
}

static uint8_t atten(uint8_t v)
{
    switch (att) {
    case 0: return v;
    case 1: return (uint8_t)(v - (v >> 2));
    case 2: return (uint8_t)(v >> 1);
    case 3: return (uint8_t)(v >> 2);
    default: return 0;
    }
}

/* NR50 follows m_lvl (0 when muted) one step per frame */
static void nr50_frame(void)
{
    uint8_t t = muted ? 0 : m_lvl;
    if (n50 == t)
        return;
    if (n50 < t)
        n50++;
    else
        n50--;
    SND_W(SND_NR50, (uint8_t)(n50 | (n50 << 4)));
}

/* ------------------------------------------------------------------ drone (CH3) */
/* write the drone level (d_code, attenuated; 0% while muted), one NR32 step per call */
static void drone_out(void)
{
    uint8_t c = (uint8_t)(d_code + datt[att]);
    uint8_t v;
    if (c > 4 || muted)
        c = 4;
    if (c > (uint8_t)(d_olv + 1))
        c = (uint8_t)(d_olv + 1);
    else if ((uint8_t)(c + 1) < d_olv)
        c = (uint8_t)(d_olv - 1);
    v = nr32_tab[c];
    if (v != d_last && d_on) {
        d_last = v;
        d_olv = c;
        SND_W(SND_NR32, v);
    }
}

/* Stop the (already 0%) channel without touching the DAC: the length counter halts it within
   1/256 s, after which wave RAM may be written on every model.  DAC off would pop. */
static void drone_stop(void)
{
    SND_W(SND_NR32, 0x00);
    d_last = 0x00;
    d_olv = 4;
    SND_W(SND_NR31, 0xFF);
    SND_W(SND_NR34, 0x40);                  /* length on, no trigger */
    d_on = 0;
}

/* only with the channel stopped (drone_stop() at least a frame ago, or never triggered) */
static void drone_load(void)
{
    const uint8_t *s = snd_waves[d_wave];
    uint16_t f = snd_freq[d_note];
    uint8_t i;
    for (i = 0; i < 16; i++)
        SND_W(SND_WAVE + i, s[i]);
    SND_W(SND_NR31, 0x00);
    SND_W(SND_NR32, 0x00);                  /* start at 0%: the fade-in brings it up */
    d_last = 0x00;
    d_olv = 4;
    SND_W(SND_NR33, (uint8_t)f);
    SND_W(SND_NR34, (uint8_t)(0x80 | (f >> 8)));   /* trigger once; length off */
    d_on = 1;
}

static void drone_pitch(void)
{
    uint16_t f = snd_freq[d_note];
    SND_W(SND_NR33, (uint8_t)f);
    SND_W(SND_NR34, (uint8_t)(f >> 8));      /* no trigger: glitch-free pitch change */
}

/* fade the drone out (one step at a time; the channel keeps running at 0%) */
static void drone_kill(void)
{
    d_br = 0;
    d_tl = 4;
    if (d_st == DS_IDLE && d_code != 4) {
        d_st = DS_SLIDE;
        d_t = 1;
    }
}

static void drone_request(uint8_t w, uint8_t n, uint8_t lvl)
{
    d_blv = lvl;
    if (d_br && lvl < 4)                    /* breathing out: one step quieter */
        lvl++;
    d_tw = w;
    d_tn = n;
    d_tl = lvl;
    if (d_st == DS_DOWN || d_st == DS_LOAD)
        return;                             /* a swap is under way: it takes the new targets */
    if (!d_on) {
        d_st = DS_LOAD;
        d_t = 2;                            /* next frame: keeps commit + load apart */
        return;
    }
    if (w != d_wave) {
        d_st = DS_DOWN;
        d_t = 2;
        return;
    }
    if (n != d_note) {                      /* a new degree: change pitch in place */
        d_note = n;
        drone_pitch();
    }
    if (lvl != d_code && d_st == DS_IDLE) {
        d_st = DS_SLIDE;
        d_t = 2;
    }
}

static uint8_t drone_rnd(void)
{
    uint8_t x = d_rng;
    x ^= (uint8_t)(x << 3);
    x ^= (uint8_t)(x >> 5);
    x ^= (uint8_t)(x << 4);
    d_rng = x ? x : 0x5D;
    return d_rng;
}

static void drone_breathe(void)
{
    if (fc & 7)
        return;
    if (d_bt) {
        d_bt--;
        return;
    }
    if (d_blv >= 4)
        return;                             /* faded out: nothing to breathe */
    if (!d_br) {                            /* breathe out: one step quieter */
        d_br = 1;
        d_tl = (uint8_t)(d_blv + 1);
        d_bt = (uint8_t)(16 + (drone_rnd() & 15));      /* 2.1-4.1 s */
    } else {                                /* breathe in */
        d_br = 0;
        d_tl = d_blv;
        d_bt = (uint8_t)(40 + (drone_rnd() & 31));      /* 5.3-9.5 s */
    }
    d_st = DS_SLIDE;
    d_t = 1;
}

static void drone_frame(void)
{
    if (d_st == DS_IDLE) {
        if ((lay & LAY_LOOPS) && d_on)
            drone_breathe();
        return;
    }
    if (--d_t)
        return;
    switch (d_st) {
    case DS_DOWN:
        if (d_on && (uint8_t)(d_code + datt[att]) < 4) {
            d_code++;
            drone_out();
            d_t = 12;
        } else {
            if (d_on)
                drone_stop();
            d_code = 4;
            d_st = DS_LOAD;
            d_t = 2;
        }
        break;
    case DS_LOAD:
        d_wave = d_tw;
        d_note = d_tn;
        drone_load();
        d_code = 4;
        d_st = DS_SLIDE;
        d_t = 12;
        break;
    default: /* DS_SLIDE */
        if (d_code > d_tl)
            d_code--;
        else if (d_code < d_tl)
            d_code++;
        drone_out();
        if (d_code == d_tl)
            d_st = DS_IDLE;
        else
            d_t = 16;
        break;
    }
}

/* ------------------------------------------------------------------ wind (CH4) */
static void wind_out(void)
{
    uint8_t l, col;
    if (snd_owned & 8)
        return;
    l = (w_on && !muted) ? atten(w_lvl) : 0;
    if (!l) {
        if (w_out) {
            w_out = 0;
            SND_W(SND_NR42, ENV_FADE);      /* fades out by itself, DAC kept on */
        }
        return;
    }
    col = (w_lvl > (uint8_t)(wp_calm + 1)) ? wp_gcol : wp_ccol;
    w_out = l;
    SND_W(SND_NR42, (uint8_t)(l << 4));
    SND_W(SND_NR43, col);
    SND_W(SND_NR44, 0x80);
}

static void wind_profile(const uint8_t *p)
{
    wp_calm = p[0];
    wp_gust = p[1];
    wp_rise = p[2];
    wp_fall = p[3];
    wp_prob = p[4];
    wp_ccol = p[5];
    wp_gcol = p[6];
    wp_flags = p[7];
    w_frate = wp_fall;
    if (!w_on) {
        w_on = 1;
        w_lvl = 0;
        w_tgt = wp_calm;
        w_rate = wp_rise;
        w_tmr = 1;
    } else if (w_tgt > wp_gust || w_tgt < wp_calm) {
        w_tgt = wp_calm;
        w_rate = wp_fall;
    }
}

static void wind_off(void)
{
    w_on = 0;
    wind_out();
}

static void wind_frame(void)
{
    uint8_t r, g;
    if (--w_tmr)
        return;
    if (w_lvl != w_tgt) {
        if (w_lvl < w_tgt)
            w_lvl++;
        else
            w_lvl--;
        wind_out();
        w_tmr = w_rate;
        return;
    }
    r = wrnd();
    if (w_lvl > wp_calm) {                  /* at a peak: hold, then fall back */
        w_tgt = wp_calm;
        w_rate = w_frate;
        w_tmr = (uint8_t)(1 + ((wp_flags & WF_WAVES) ? 8 : (r & 31)));
        return;
    }
    if (w_lvl < wp_calm) {
        w_tgt = wp_calm;
        w_rate = wp_rise;
        w_tmr = 1;
        return;
    }
    if (r < wp_prob) {                      /* gust / wave */
        g = (uint8_t)(wp_calm + 1 + (wrnd() & 3));
        if (g > wp_gust)
            g = wp_gust;
        w_tgt = g;
        w_rate = wp_rise;
        w_frate = wp_fall;
        w_tmr = (uint8_t)((wp_flags & WF_WAVES) ? 30 + (r & 63) : 1);   /* surf: rest between waves */
    } else {                                /* calm */
        w_tmr = (uint8_t)(24 + (r & 127));
    }
}

/* ------------------------------------------------------------------ notes + echo */
static void echo_push(uint8_t n, uint8_t v, uint8_t pace, uint8_t duty)
{
    uint8_t h = eq_h, nh = (uint8_t)((h + 1) & 7);
    if (v < (uint8_t)(g_fb + 2) || nh == eq_t)
        return;
    eq_n[h] = n;
    eq_v[h] = (uint8_t)(v - g_fb);
    eq_due[h] = (uint8_t)(fc + echo_d);
    eq_pace[h] = pace;
    eq_duty[h] = duty;
    eq_h = nh;
}

static void ch1_note(uint8_t n, uint8_t v, uint8_t pace, uint8_t duty)
{
    uint16_t f;
    v = atten(v);
    if (!v || muffle || muted || (snd_owned & 1))
        return;                             /* the map: the melody holds its breath */
    f = snd_freq[n];
    SND_W(SND_NR11, duty);
    SND_W(SND_NR12, (uint8_t)((v << 4) | pace));
    SND_W(SND_NR13, (uint8_t)f);
    SND_W(SND_NR14, (uint8_t)(0x80 | (f >> 8)));
    echo_push(n, v, pace, duty);
    fired = 1;
}

static void echo_frame(void)
{
    uint8_t i = eq_t, v;
    uint16_t f;
    if ((uint8_t)(fc - eq_due[i]) & 0x80)
        return;                             /* head not due yet */
    eq_t = (uint8_t)((i + 1) & 7);
    v = eq_v[i];
    if (!(snd_owned & 2) && !muted) {
        f = snd_freq[eq_n[i]];              /* in tune: a detune of one step is ~30 cents up high */
        SND_W(SND_NR21, eq_duty[i]);
        SND_W(SND_NR22, (uint8_t)((v << 4) | eq_pace[i]));
        SND_W(SND_NR23, (uint8_t)f);
        SND_W(SND_NR24, (uint8_t)(0x80 | (f >> 8)));
    }
    echo_push(eq_n[i], v, eq_pace[i], eq_duty[i]);   /* feedback */
}

/* fade to 0 with the DAC kept on (NRx2 = 0 would switch the DAC off: a pop) */
static void ch1_silence(void)
{
    SND_W(SND_NR12, ENV_FADE);
}

static void ch2_silence(void)
{
    SND_W(SND_NR22, ENV_FADE);
}

static void silence_pulses(void)
{
    if (!(snd_owned & 1))
        ch1_silence();
    if (!(snd_owned & 2))
        ch2_silence();
    eq_h = eq_t = 0;
    pn_t = 0;
}

/* ------------------------------------------------------------------ world */
static void calc_step(void)
{
    uint8_t s = (uint8_t)(ph_step[c_phase] + b_slow[c_biome]);
    if (c_wx == WX_SNOW)
        s += 2;
    if (fast)
        s = (uint8_t)(s - (s >> 2) - (s >> 3));
    step_len = s;
    echo_d = (uint8_t)(s + (s >> 1));
    if (step_t > s)
        step_t = s;
}

/* Commit the requested biome / phase / weather.  Split in two halves that run in
 * different frames (see WK_*), so no single sound_tick() gets expensive. */
static void commit_a(void)
{
    uint8_t b = snd_p_biome, ph = snd_p_phase, wx = snd_p_wx, i, v;
    const uint8_t *per;
    if (b >= B_COUNT)
        b = B_MEADOW;
    ph &= 3;
    if (wx > WX_STORM)
        wx = WX_CLEAR;
    snd_p_dirty = 0;
    if (b != c_biome || !c_seeded) {
        uint16_t s = (uint16_t)(snd_seed ^ ((uint16_t)b << 8) ^ (uint16_t)(b << 3) ^ 0x6D2Bu);
        rng = s ? s : 0x1234;
        lp_stale = 0x3F;
        c_seeded = 1;
    }
    if (ph == PH_DAWN && c_phase == PH_NIGHT) {  /* dawn motif: lit beacons ring in turn */
        lp_cnt[L_BEACON] = 4;
        lp_cnt[L_BEACON + 1] = 10;
        lp_cnt[L_BEACON + 2] = 16;
    }
    c_biome = b;
    c_phase = ph;
    c_wx = wx;
    pool = b_pool[b];
    per = pset_per[b_pset[b]];
    for (i = 0; i < 6; i++) {
        v = per[i];
        lp_per[i] = v;
        if (lp_cnt[i] > v)
            lp_cnt[i] = v;
    }
    v = ph_mask[ph];
    if (wx == WX_STORM)
        v &= 0x0F;
    g_mask = v;
    g_night = (ph == PH_NIGHT);
    g_star = g_night || b == B_TUNDRA;
    g_cell = ph_cell[ph];
    g_fb = (wx == WX_FOG) ? 1 : 2;
}

static void commit_b(void)
{
    uint8_t b = c_biome, wx = c_wx, v, dn;
    v = (uint8_t)(b_vol[b] - ph_vdown[c_phase]);
    if (wx == WX_RAIN || wx == WX_STORM || wx == WX_FOG)
        v--;
    g_vol = v < 4 ? 4 : v;
    g_pace = b_pace[b];
    g_duty = b_duty[b];
    v = (uint8_t)(b_skip[b] + ph_skip[c_phase]);
    g_skip = v < b_skip[b] ? 255 : v;
    calc_step();
    /* drone */
    dn = b_drone[b][0];
    if (g_night && dn >= N_D4)
        dn -= 12;
    drone_request(b_wave[b], dn, b_dlev[b]);
    /* wind / weather */
    if (wx == WX_RAIN)
        wind_profile(wind_rain);
    else if (wx == WX_STORM)
        wind_profile(wind_storm);
    else {
        wind_profile(b_wind[b]);
        if (wx == WX_SNOW) {
            wp_ccol += 0x10;
            wp_gcol += 0x10;
            if (wp_gust > 2)
                wp_gust--;
        } else if (wx == WX_FOG) {
            wp_calm = 0;
            wp_gust = 1;
            wp_prob = 40;
        }
        if (g_night) {
            if (wp_gust < 3)
                wp_gust++;
            wp_prob = (uint8_t)(wp_prob > 200 ? 255 : wp_prob + 40);
        }
    }
}

static void loops_start(void)
{
    uint8_t i, r = rnd();
    for (i = 0; i < NLOOP; i++)
        lp_cnt[i] = (uint8_t)(loop_stagger[i] + (r & 7));
    lp_stale = 0x3F;
    step_t = 1;
    step_ctr = 0;
    pn_t = 0;
    lay |= LAY_LOOPS;
}

/* deferred heavy work, one item per frame */
static void do_work(void)
{
    uint8_t w = work;
    if (w & WK_A) {
        work = (uint8_t)(w & ~WK_A);
        commit_a();
    } else if (w & WK_B) {
        work = (uint8_t)(w & ~WK_B);
        commit_b();
    } else {
        work = 0;
        loops_start();
    }
}

static void play_or_pend(uint8_t n, uint8_t v)
{
    if (fired) {                            /* one CH1 note per step: the other waits */
        if (!pn_t) {
            pn_t = 1;
            pn_n = n;
            pn_v = v;
        }
        return;
    }
    ch1_note(n, v, g_pace, g_duty);
}

static void loop_fire(uint8_t i)
{
    uint8_t n, pi, c, r, v;
    if (i < 6) {
        if (!(g_mask & bit8[i]))
            return;
        r = rnd();
        if ((lp_stale & bit8[i]) || r < (g_night ? 40 : 12)) {  /* slow evolution: re-pick */
            pi = (uint8_t)(rnd() & 7);
            if (pi == last_pick)
                pi = (uint8_t)((pi + 3) & 7);   /* spread the loops over the pool */
            last_pick = pi;
            if ((g_night || i < 3) && pi > 5)
                pi -= 3;                    /* colour tones only on the long loops, never at night */
            lp_pi[i] = pi;
            lp_stale &= (uint8_t)~bit8[i];
        }
        if (rnd() < g_skip)
            return;
        pi = lp_pi[i];
        n = pool[pi];
        if (g_night && n > N_D5)
            n -= 12;
        v = (uint8_t)(g_vol + (r & 1));
        play_or_pend(n, v);
        c = g_cell;
        if (c == CELL_MIX) {
            c = (uint8_t)((r >> 1) & 3);
            if (c == CELL_MIX)
                c = CELL_ONE;
        }
        if (!pn_t && (r & 0x10)) {
            if (c == CELL_RISE && pi < 7)
                n = pool[pi + 1];
            else if (c == CELL_FALL && pi)
                n = pool[pi - 1];
            else
                return;
            if (g_night && n > N_D5)
                n -= 12;
            pn_t = 2;
            pn_n = n;
            pn_v = (uint8_t)(v - 1);
        }
    } else if (i == L_STAR) {
        if (!g_star)
            return;
        r = rnd();
        if (r < 64)
            return;
        if (!fired)
            ch1_note(star_pool[r & 7], 3, 3, 0x40);
    } else {
        i -= L_BEACON;
        if (!(bmask & bit8[i]))
            return;
        if (fired) {
            lp_cnt[i + L_BEACON] = 1;       /* ring next step instead */
            return;
        }
        ch1_note(btone[i], 5, 7, 0x80);
    }
}

static void world_step(void)
{
    uint8_t i;
    fired = 0;
    for (i = 0; i < NLOOP; i++) {
        if (--lp_cnt[i] == 0) {
            lp_cnt[i] = lp_per[i];
            loop_fire(i);
        }
    }
    if (pn_t && --pn_t == 0) {
        if (fired)
            pn_t = 1;
        else if (pn_v)
            ch1_note(pn_n, pn_v, g_pace, g_duty);
    }
    if (++step_ctr == 16) {                 /* phrase boundary */
        step_ctr = 0;
        if (snd_p_dirty) {
            work |= WK_A | WK_B;
        } else {
            i = rnd();
            if (i < 96) {                   /* the drone walks to another degree */
                uint8_t dn = b_drone[c_biome][i & 3];
                if (g_night && dn >= N_D4)
                    dn -= 12;
                drone_request(b_wave[c_biome], dn, b_dlev[c_biome]);
            }
        }
    }
}

/* ------------------------------------------------------------------ motifs */
static void motif_start(const uint8_t *m)
{
    mo_base = m;
    mo_p = m;
    mo_t = 1;
    mo_duty = 0x80;
}

static void motif_done(void)
{
    if (snd_cur_mode == AMB_WAKE) {             /* the wake figure flows into the world */
        snd_cur_mode = AMB_WORLD;
        work |= WK_A | WK_B | WK_LOOPS;
    }
}

static void motif_frame(void)
{
    uint8_t op, a, w;
    if (--mo_t)
        return;
    for (;;) {
        op = mo_p[0];
        a = mo_p[1];
        w = mo_p[2];
        mo_p += 3;
        if (op < 0x80) {
            ch1_note(op, (uint8_t)(a >> 4), (uint8_t)(a & 7), mo_duty);
        } else if (op == 0xF0) {
            drone_request(d_tw, a, d_blv);
        } else if (op == 0xF3) {
            mo_duty = a;
        } else if (op == 0xF4) {
            g_fb = a;
        } else if (op == 0xF5) {
            mf_rate = a;
            mf_t = a;
        } else if (op == 0xFD) {
            mo_p = mo_base + (uint8_t)(a + a + a);
            continue;
        } else {
            mo_p = 0;
            lay &= (uint8_t)~LAY_MOTIF;
            motif_done();
            return;
        }
        if (w) {
            mo_t = w;
            return;
        }
    }
}

/* ------------------------------------------------------------------ modes */
static void switch_mode(uint8_t m)
{
    if (d_br) {                             /* a new mode starts with the drone breathing in */
        d_br = 0;
        if (d_blv < 4)
            d_tl = d_blv;
    }
    d_bt = 40;
    snd_cur_mode = m;
    lay = 0;
    work = 0;
    mo_p = 0;
    mf_rate = 0;
    muffle = 0;
    m_lvl = 7;
    if (att >= 4 || m == AMB_SILENT) {
        silence_pulses();
        d_code = 4;                         /* drone and wind are silent: ramp from zero */
        w_lvl = 0;
    }
    g_fb = 2;
    switch (m) {
    case AMB_TITLE:
        motif_start(mo_title);
        drone_request(WV_WARM, N_D4, 2);
        wind_profile(wind_title);
        lay = LAY_MOTIF | LAY_WIND;
        echo_d = 36;
        break;
    case AMB_WORLD:
    case AMB_MAP:
        work = WK_A | WK_B | WK_LOOPS;
        muffle = (m == AMB_MAP);
        lay = LAY_WIND;
        break;
    case AMB_ENDING:
        motif_start(mo_ending);
        drone_request(WV_WARM, N_D4, 2);
        wind_profile(wind_title);
        lay = LAY_MOTIF | LAY_WIND;
        echo_d = 33;
        break;
    case AMB_WAKE:
        work = WK_A | WK_B;
        motif_start(mo_wake);
        lay = LAY_MOTIF | LAY_WIND;
        break;
    default: /* AMB_SILENT */
        drone_kill();
        wind_off();
        break;
    }
}

/* After a switch from silence: the world fades in; the composed motifs start at full
 * voice (their drone and wind still ramp up by themselves). */
static void fade_in(void)
{
    uint8_t m = snd_cur_mode;
    if (m == AMB_SILENT) {
        snd_xf = XF_NONE;
    } else if (m == AMB_TITLE || m == AMB_WAKE || m == AMB_ENDING) {
        snd_xf = XF_NONE;
        att = 0;
    } else {
        snd_xf = XF_IN;
        xf_t = 20;
    }
}

static void request_mode(uint8_t m)
{
    if (m >= NUM_AMB)
        return;
    if (snd_xf == XF_OUT) {
        snd_xf_next = m;                        /* retarget the running fade */
        return;
    }
    if (m == snd_cur_mode)
        return;
    if (snd_cur_mode == AMB_WAKE && m == AMB_WORLD)
        return;                             /* already on its way */
    if ((snd_cur_mode == AMB_WORLD && m == AMB_MAP) || (snd_cur_mode == AMB_MAP && m == AMB_WORLD)) {
        snd_cur_mode = m;
        muffle = (m == AMB_MAP);
        return;
    }
    if ((snd_cur_mode == AMB_WORLD || snd_cur_mode == AMB_MAP) && m == AMB_ENDING) {
        switch_mode(m);                     /* bloom out of the world, no gap */
        return;
    }
    if (snd_cur_mode == AMB_SILENT || att >= 4 || (snd_cur_mode == AMB_ENDING && !lay)) {
        att = 4;
        switch_mode(m);
        fade_in();
        return;
    }
    snd_xf = XF_OUT;
    xf_t = 1;
    snd_xf_next = m;
}

static void att_apply(void)
{
    drone_out();
    if (w_on)
        wind_out();
}

static void xfade_frame(void)
{
    if (--xf_t)
        return;
    if (snd_xf == XF_OUT) {
        xf_t = 5;
        if (att < 4) {
            att++;
            att_apply();
        } else {
            switch_mode(snd_xf_next);
            fade_in();
        }
    } else {
        xf_t = 20;
        if (att) {
            att--;
            att_apply();
        } else {
            snd_xf = XF_NONE;
        }
    }
}

static void master_frame(void)
{
    if (--mf_t)
        return;
    mf_t = mf_rate;
    if (m_lvl) {
        m_lvl--;                            /* nr50_frame() follows */
    } else {                                /* the world has dissolved */
        mf_rate = 0;
        lay = 0;
        mo_p = 0;
        silence_pulses();
        drone_kill();
        wind_off();
    }
}

/* ------------------------------------------------------------------ sfx */
static void release(uint8_t m)
{
    snd_owned &= (uint8_t)~m;
    if (m & 1) {
        SND_W(SND_NR10, 0x00);
        ch1_silence();
    }
    if (m & 2)
        ch2_silence();
    if (m & 8) {
        w_lvl = 0;                          /* the wind swells back from silence */
        w_tmr = 1;
        w_out = 0xFF;
        wind_out();
    }
}

static void voice_stop(uint8_t i, uint8_t keep)
{
    uint8_t m = (uint8_t)(vo_mask[i] & ~keep);
    vo_p[i] = 0;
    snd_vo_on &= (uint8_t)~(i + 1);
    release(m);
}

static void sfx_start(uint8_t id)
{
    uint8_t m = snd_sfx_mask[id];
    uint8_t pr = snd_sfx_prio[id];
    uint8_t i;
    for (i = 0; i < 2; i++)
        if (vo_p[i] && (vo_mask[i] & m) && vo_prio[i] > pr)
            return;                         /* busy with something more important */
    for (i = 0; i < 2; i++)
        if (vo_p[i] && (vo_mask[i] & m))
            voice_stop(i, m);
    i = 0;
    if (vo_p[0]) {
        i = 1;
        if (vo_p[1]) {
            if (vo_prio[0] <= vo_prio[1])
                i = 0;
            voice_stop(i, 0);
        }
    }
    if (id <= SFX_STEP_STONE)
        vo_p[i] = snd_step_var[id][var3[wrnd() & 3]];
    else
        vo_p[i] = snd_sfx_data[id];
    vo_wait[i] = 0;
    vo_mask[i] = m;
    vo_prio[i] = pr;
    snd_vo_on |= (uint8_t)(i + 1);
    snd_owned |= m;
}

static const uint8_t *vp;                /* script pointer (a global: cheaper on SDCC) */

static void voice_run(uint8_t i)
{
    uint8_t b;
    if (vo_wait[i]) {
        vo_wait[i]--;
        return;
    }
    vp = vo_p[i];
    SND_SRC(SND_SRC_SFX);
    for (;;) {
        b = *vp++;
        if (b >= 0x40) {
            vo_wait[i] = (uint8_t)(b - 0x40);
            vo_p[i] = vp;
            break;
        }
        if (!b) {
            SND_SRC(SND_SRC_MUSIC);
            voice_stop(i, 0);
            return;
        }
        SND_W(b, *vp);
        vp++;
    }
    SND_SRC(SND_SRC_MUSIC);
}

void snd_core_init(void)
{
    uint8_t i;
    SND_SRC(SND_SRC_MUSIC);
    SND_W(SND_NR52, 0x00);                  /* power-cycle: every channel stopped (the boot */
    SND_W(SND_NR52, 0x80);                  /* chime may still be playing on CH1) */
    SND_W(SND_NR50, 0x77);
    SND_W(SND_NR51, 0xFF);
    /* every DAC on, once, at volume 0: from here on nothing switches one off (see the top) */
    SND_W(SND_NR10, 0x00);
    SND_W(SND_NR12, ENV_FADE);
    SND_W(SND_NR22, ENV_FADE);
    SND_W(SND_NR32, 0x00);
    SND_W(SND_NR30, 0x80);
    SND_W(SND_NR42, ENV_FADE);
    n50 = 7;
    snd_req_mode = REQ_NONE;
    snd_req_mute = 0;
    snd_req_fast = 0;
    snd_req_bm = 0;
    snd_rq_head = snd_rq_tail = 0;
    vo_p[0] = vo_p[1] = 0;
    snd_owned = 0;
    snd_vo_on = 0;
    snd_req_seed = 0;
    fc = 0;
    snd_cur_mode = AMB_SILENT;
    snd_xf = XF_NONE;
    att = 0;
    lay = 0;
    muffle = muted = fast = 0;
    m_lvl = 7;
    mf_rate = 0;
    rng = 0x1234;
    wrng = 0xACE1;
    snd_p_biome = B_MEADOW;
    snd_p_phase = PH_DAY;
    snd_p_wx = WX_CLEAR;
    snd_p_dirty = 1;
    c_biome = B_MEADOW;
    c_phase = PH_DAY;
    c_wx = WX_CLEAR;
    c_seeded = 0;
    pool = b_pool[B_MEADOW];
    bmask = 0;
    g_mask = 0;
    g_fb = 2;
    step_len = 16;
    echo_d = 24;
    eq_h = eq_t = 0;
    pn_t = 0;
    for (i = 0; i < NLOOP; i++) {
        lp_cnt[i] = 1;
        lp_per[i] = 16;
    }
    for (i = 0; i < 4; i++)
        lp_per[L_STAR + i] = other_per[i];
    work = 0;
    d_on = 0;
    d_st = DS_IDLE;
    d_code = 4;
    d_last = 0xFF;
    d_olv = 4;
    d_wave = 0xFF;
    d_br = 0;
    d_blv = 4;
    d_bt = 40;
    d_rng = 0x5D;
    w_on = 0;
    w_out = 0;
    w_lvl = 0;
    mo_p = 0;
}

void snd_core_tick(void)
{
    uint8_t r;
    fc++;

    /* ---- requests ---- */
    if (snd_req_seed) {
        snd_req_seed = 0;
        c_seeded = 0;
    }
    r = snd_req_mode;
    if (r != REQ_NONE) {
        snd_req_mode = REQ_NONE;
        request_mode(r);
    }
    r = snd_req_mute;
    if (r != muted) {
        muted = r;                          /* NR51 stays: everything fades instead */
        if (r) {
            if (vo_p[0])
                voice_stop(0, 0);
            if (vo_p[1])
                voice_stop(1, 0);
            silence_pulses();
        }
        w_out = 0xFF;
        wind_out();
    }
    nr50_frame();
    if (d_on && !(fc & 7))
        drone_out();                        /* mute / unmute: the drone follows a step at a time */
    r = snd_req_fast;
    if (r != fast) {
        fast = r;
        if (lay & LAY_LOOPS)
            calc_step();
    }
    r = snd_req_bm;
    if (r != bmask) {
        uint8_t nw = (uint8_t)(r & ~bmask);
        bmask = r;
        if (nw & 1) lp_cnt[L_BEACON] = 12;
        if (nw & 2) lp_cnt[L_BEACON + 1] = 12;
        if (nw & 4) lp_cnt[L_BEACON + 2] = 12;
    }
    /* ---- ambient ---- */
    r = 0;
    if (lay & LAY_LOOPS) {
        if (--step_t == 0) {
            step_t = step_len;
            r = 1;
        }
    }
    /* one sfx start per frame, and not on a music step (a frame later is inaudible): a
       burst of requests never lands on a single tick */
    if (muted)
        snd_rq_tail = snd_rq_head;          /* no sfx while muted */
    else if (snd_rq_tail != snd_rq_head && !r) {
        sfx_start(snd_rq[snd_rq_tail]);
        snd_rq_tail = (uint8_t)((snd_rq_tail + 1) & 3);
    }
    if (work && !r)                         /* deferred work never shares a tick with a step */
        do_work();
    if (snd_xf)
        xfade_frame();
    if (r)
        world_step();
    if (lay & LAY_MOTIF)
        motif_frame();
    if (eq_h != eq_t)
        echo_frame();
    if (d_on | d_st)
        drone_frame();
    if (w_on)
        wind_frame();
    if (mf_rate)
        master_frame();

    /* ---- sfx ---- */
    if (vo_p[0])
        voice_run(0);
    if (vo_p[1])
        voice_run(1);
}
