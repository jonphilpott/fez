/* The audio side: turns the control side's effective parameters into sound.

   Signal flow (a west-coast style voice with a second, "mod" voice):

     complex osc (sine + a shaped wave, blended) --> wavefolder --> mixer --+
     mod osc ---------------------------------------------------> mixer ----+--> vactrol lowpass gate --> bus
     noise --> tone lowpass ------------------------------------> mixer ----+      ^ gate envelope
     buffer device (MIXBP) ---------------------------------------------- +

     mod osc / noise --> Moog ladder filter --> mod VCA ------------------------> bus
     (voice + mod voice) --> tape delay --> spring (two tanks that feed each other: a stereo pair) --> bus (left, right)
     buffer device (MIXBF) ------------------------------------------------------> bus
     bus --> compressor (one detector for both ears) --> master level --> output (left, right)

   Time structure: sound is made one sample at a time, but parameters only change once per CHUNK of 16 samples
   (0.33 ms): we smooth the registers, convert them to physical units, and ramp gains linearly across the chunk.
   Every 15 chunks (5 ms) the control side ticks. */
#include "fez.h"
#include "scales.h"
#include <math.h>
#include <string.h>

#define TWO_PI 6.28318530717958647692
/* The spring is a stereo pair of tanks. Each is a short loop with a chain of allpass filters (see render_sample). They are tuned
   differently, which is what makes the two ears hear two different chirps instead of one: tank 0 (left) loops in 34 ms, tank 1
   (right) in 41 ms, and its allpass corners and lowpass sit a little higher. SPR_CROSS is how much of each tank's output is fed
   into the other's loop: 0 would be two separate springs, 0.5 would make them one. At 0.3 the loop is still stable at full
   feedback: the mixing matrix [1-x x; x 1-x] has eigenvalues 1 and 1-2x, neither above 1 in size. */
static const int SPR_LEN[SPR_TANKS] = { (int)(0.034 * FEZ_SAMPLE_RATE), (int)(0.041 * FEZ_SAMPLE_RATE) };
static const double SPR_AP_HZ[SPR_TANKS][SPR_ALLPASS] = { { 330, 520, 780, 1100, 1500, 2100, 2800 }, { 406, 640, 960, 1353, 1845, 2583, 3444 } };
static const double SPR_LP_HZ[SPR_TANKS] = { 3200, 3000 };
#define SPR_CROSS 0.3
/* SPRTONE takes the metal out in three ways, each scaled by the knob (0 is the old spring exactly):
   1) diffusers: three short allpass delays in each loop (lengths in samples, about 2.4, 7 and 10.6 ms, no two sharing a
      factor so their echoes never line up). Each one smears every echo into a spread of smaller ones, so the loop stops
      repeating one clean copy of itself every 34 ms, which is what rings like a tin can.
   2) the chirp allpasses get gentler: their Q falls from 2.5 (each one rings at its own pitch) to 0.9.
   3) each loop's length wobbles a little (up to SPR_WOB samples, at slow rates that differ per tank), so the loop's
      resonances keep moving instead of settling on fixed pitches.
   The diffusers delay the sound too, so the main loop is shortened by their total: each tank's loop time stays the same. */
static const int SPR_DIFF_LEN[SPR_TANKS][SPR_DIFF] = { { 113, 337, 509 }, { 127, 373, 563 } };
static const double SPR_WOB_HZ[SPR_TANKS] = { 0.71, 0.93 };
#define SPR_WOB 12.0                    /* samples (0.25 ms) at full SPRTONE: a slight drift, too little to hear as pitch */
#define SPR_Q_METAL 2.5                 /* the chirp allpasses' Q at SPRTONE 0 ... */
#define SPR_Q_SOFT  0.9                 /* ... and at full */
#define SPR_DIFF_G  0.6                 /* the diffusers' gain at full: above about 0.7 they ring themselves */

/* ---------------------------------------------------------------- small helpers */

static double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

/* Aim a ramp at a new value, reaching it by the end of the chunk. */
static void ramp_to(Ramp *r, double target) { r->step = (target - r->cur) / CHUNK; }
static double ramp_next(Ramp *r) { r->cur += r->step; return r->cur; }

/* The synth's smoothed value of a parameter register (see begin_chunk). */
static double P(const FezSynth *s, int addr) { return s->audio.sm[s->param_index[addr]]; }

/* White noise in -1..1 from a small xorshift generator. */
static double white(Audio *a) {
  a->nz_state ^= a->nz_state << 13; a->nz_state ^= a->nz_state >> 17; a->nz_state ^= a->nz_state << 5;
  return (a->nz_state / 4294967296.0) * 2.0 - 1.0;
}

/* ---------------------------------------------------------------- oscillators
   A naive sawtooth or square jumps instantly, which creates frequencies above half the sample rate that fold
   back down as harsh "aliasing". PolyBLEP smooths the two samples around each jump with a short polynomial,
   which removes most of it for almost no cost. `t` is the phase (0..1), `dt` the phase step per sample. */
static double poly_blep(double t, double dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
  if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
  return 0.0;
}

/* shape 0 sine, 1 triangle, 2 square, 3 saw. All start at zero phase like Web Audio's, except saw/square. */
static double wave(int shape, double ph, double dt) {
  switch (shape) {
    case 0: return sin(TWO_PI * ph);
    case 1: return ph < 0.25 ? 4 * ph : ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4;   /* ponytail: naive triangle; harmonics fall as 1/n^2, so aliasing is mild */
    case 2: { double sq = ph < 0.5 ? 1.0 : -1.0; double ph2 = ph + 0.5; if (ph2 >= 1) ph2 -= 1;
              return sq + poly_blep(ph, dt) - poly_blep(ph2, dt); }
    default: return 2 * ph - 1 - poly_blep(ph, dt);
  }
}

static double advance_phase(double ph, double hz) {
  ph += hz / FEZ_SAMPLE_RATE;
  return ph - floor(ph);                                    /* wrap into 0..1, also for negative frequencies */
}

/* ---------------------------------------------------------------- setup */

void audio_init(Audio *a) {
  memset(a, 0, sizeof *a);
  buf_init(&a->buf);
  a->nz_state = 2463534242u;
  for (int i = 0; i < PARAM_COUNT; i++) {
    double tau = PARAM_TABLE[i].tau;
    a->k_sm[i] = tau > 0 ? 1.0 - exp(-(double)CHUNK / (FEZ_SAMPLE_RATE * tau)) : 1.0;   /* tau 0: no smoothing */
  }
  for (int t = 0; t < SPR_TANKS; t++) {
    for (int i = 0; i < SPR_ALLPASS; i++) svf_coef(&a->spr_ap_c[t][i], SPR_AP_HZ[t][i], SPR_Q_METAL);   /* until the first chunk sets SPRTONE's */
    svf_coef(&a->spr_c[t], SPR_LP_HZ[t], 0.7071);               /* Q 0.7071: flattest lowpass, gain never above 1 */
  }
  a->comp_ka = 1.0 - exp(-1.0 / (0.003 * FEZ_SAMPLE_RATE));   /* compressor attack 3 ms */
  a->comp_kr = 1.0 - exp(-1.0 / (0.2 * FEZ_SAMPLE_RATE));     /* release 200 ms */
  a->dly_len = 0.3 * FEZ_SAMPLE_RATE;
  a->rel = 0.2;                                               /* so the gate maths never divides by zero before the first note */
}

/* ---------------------------------------------------------------- clearing the effects */

/* Empty everything that remembers sound: the delay ring and its feedback lowpass, the spring's ring and filters, the
   compressor's level follower, and the voice's own filters and gate (which also clears any NaN stuck in them). Only state is cleared. Settings (delay length, filter coefficients, freeze) are left alone, because
   the new patch's registers are about to set them. memset on the arrays is safe here: float and double zero is all-bits-zero. */
void audio_clear_fx(Audio *a) {
  memset(a->dly, 0, sizeof a->dly);
  a->dly_damp.y = 0;
  memset(a->spr, 0, sizeof a->spr);                          /* both tanks */
  memset(a->spr_ap, 0, sizeof a->spr_ap);
  memset(a->spr_svf, 0, sizeof a->spr_svf);
  memset(a->spr_dif, 0, sizeof a->spr_dif);
  a->comp_env = 0;
  memset(&a->lpg_lp, 0, sizeof a->lpg_lp);
  memset(&a->nz_svf, 0, sizeof a->nz_svf);
  memset(&a->md_ladder, 0, sizeof a->md_ladder);
  a->yf = a->ys = 0;
  a->buf_prev = 0;
}

/* ---------------------------------------------------------------- once per control tick (5 ms) */

/* Hand the buffer device its settings from the registers, then deliver the events (trigger, stop, record,
   clear) and the envelope gates the control side queued. */
static void service_events(FezSynth *s) {
  Audio *a = &s->audio;
  BufCfg c;
  c.mode = s->mem[REG_BWMODE];
  c.fb = s->eff[REG_BWFB] / 32767.0;
  c.bin = s->mem[REG_BIN] / 32767.0;
  c.ms = s->mem[REG_BUFMS];
  c.bwlen = s->mem[REG_BWLEN];
  c.win = s->mem[REG_GWIN] / 32767.0;
  c.pitch = s->eff[REG_PITCH];
  for (int i = 0; i < BUF_VOICES; i++) {                     /* voice 0's position, rate and level can be modulated */
    c.v[i].pos   = i == 0 ? s->eff[REG_GPOS0] : s->mem[REG_GPOS0 + i];
    c.v[i].rate  = i == 0 ? s->eff[REG_GRATE0] : s->mem[REG_GRATE0 + i];
    c.v[i].len   = s->mem[REG_GLEN0 + i];
    c.v[i].lvl   = i == 0 ? s->eff[REG_GLVL0] : s->mem[REG_GLVL0 + i];
    c.v[i].flags = s->mem[REG_GFLG0 + i];
  }
  buf_set_cfg(&a->buf, &c);

  if (s->fx_clear) { audio_clear_fx(a); s->fx_clear = 0; }

  BufEvent be;
  while (fez_pop_buf_event(s, &be)) {
    if (be.type == EV_CLEAR) buf_clear(&a->buf);
    else if (be.type == EV_TRIG) buf_trig(&a->buf, be.mask);
    else if (be.type == EV_STOP) buf_stop(&a->buf, be.mask);
    else if (be.type == EV_REC) buf_record(&a->buf);
  }

  /* A gate lights the vactrol's LED. The LED stays on long enough for the photoresistor to open fully:
     ATK sets how fast it opens (time constant ATK/3, held for five of them); REL is used as it falls. */
  GateEvent g;
  while (fez_pop_gate_event(s, &g)) {
    double tau_r = g.atk / 3.0 < 0.0005 ? 0.0005 : g.atk / 3.0;
    a->k_rise = 1.0 - exp(-1.0 / (tau_r * FEZ_SAMPLE_RATE));
    a->led_left = (int)(5.0 * tau_r * FEZ_SAMPLE_RATE) + 1;
    a->rel = g.rel;
  }
}

/* ---------------------------------------------------------------- once per chunk (16 samples) */

static void begin_chunk(FezSynth *s) {
  Audio *a = &s->audio;
  if (a->chunks_to_tick == 0) {
    fez_tick(s, 1000.0 * TICK_CHUNKS * CHUNK / FEZ_SAMPLE_RATE);            /* one tick's worth of time: 5 ms */
    service_events(s);
    a->chunks_to_tick = TICK_CHUNKS;
  }
  a->chunks_to_tick--;

  /* 1) smooth every parameter register toward its effective value (register + matrix) */
  for (int i = 0; i < PARAM_COUNT; i++) {
    int addr = PARAM_TABLE[i].addr;
    double target = addr == REG_GATE ? 0 : s->eff[addr];
    if (!a->primed) a->sm[i] = target;
    else a->sm[i] += (target - a->sm[i]) * a->k_sm[i];
  }
  const double FULL = 32767.0, HALF_PI = TWO_PI / 4;

  /* 2) oscillators */
  a->f_cx = pitch_hz(P(s, REG_PITCH));
  /* MDTRACK on: MDPITCH is an interval above the voice's pitch as heard (after glide, matrix and quantiser), C4 being none */
  a->f_md = pitch_hz(P(s, REG_MDTRACK) >= 16384 ? P(s, REG_PITCH) + P(s, REG_MDPITCH) - 15360 : P(s, REG_MDPITCH));
  a->fm_depth = fm_depth_hz(P(s, REG_FMAMT));
  a->cx_shape = cx_shape(s->eff[REG_CXWAVE]);
  a->md_shape = md_shape(s->eff[REG_MDWAVE]);

  /* 3) gains. Blends are "equal power": cos and sin of an angle, so the loudness stays steady across the blend. */
  static const double cx_scale[3] = { 1.0, 0.55, 0.75 };                       /* level-match the shapes (by ear): triangle, square, saw */
  static const double md_scale[4] = { 1.0, 1.0, 0.5, 0.65 };
  double bl = P(s, REG_CXBLEND) / FULL * HALF_PI;
  double mdb = P(s, REG_MDNZ) / FULL * HALF_PI;
  double md_cut = cut_hz(P(s, REG_MDCUT));
  /* White noise through a low cutoff carries little energy, so lift it as the cutoff falls to keep sweeps level. */
  double lift = clampd(sqrt(8000.0 / md_cut), 1.0, 6.0);
  /* Each ramped gain and the value it heads for this chunk, side by side, so the two cannot get out of step.
     The constants 0.9 (noise), 1.7 (spring) and 1.8 (master) are loudness matches set by ear when the voice was built;
     change them by listening, not by calculation. */
  struct { Ramp *ramp; double next; } gains[] = {
    { &a->g_sine,  cos(bl) },                                       /* complex oscillator: the sine ... */
    { &a->g_shape, sin(bl) * cx_scale[a->cx_shape] },               /* ... and the shaped wave, blended */
    { &a->pre,     0.25 + 0.75 * P(s, REG_FOLD) / FULL },           /* drive into the folder */
    { &a->mix_cx,  P(s, REG_MIXCX) / FULL },                        /* mixer: complex, mod, noise */
    { &a->mix_md,  P(s, REG_MIXMD) / FULL },
    { &a->mix_nz,  P(s, REG_MIXNZ) / FULL },
    { &a->md_osc_g, cos(mdb) * md_scale[a->md_shape] },             /* mod voice: oscillator ... */
    { &a->md_nz_g, sin(mdb) * 0.9 * lift },                         /* ... and noise */
    { &a->md_vca,  pow(P(s, REG_MDLVL) / FULL, 1.5) },              /* mod voice VCA */
    { &a->master,  pow(P(s, REG_LEVEL) / FULL, 1.5) * 1.8 },        /* master level */
    { &a->out_bf,  P(s, REG_MIXBF) / FULL },                        /* buffer out: to the bus ... */
    { &a->out_bp,  P(s, REG_MIXBP) / FULL },                        /* ... and to the gate */
    { &a->dly_dry, cos(P(s, REG_DLYMIX) / FULL * HALF_PI) },        /* delay mix, equal power: dry ... */
    { &a->dly_wet, sin(P(s, REG_DLYMIX) / FULL * HALF_PI) },        /* ... and wet, so the level holds across the knob */
    { &a->spr_wet, P(s, REG_SPRMIX) / FULL * 1.7 },                 /* spring wet */
    { &a->spr_wid, P(s, REG_SPRWID) / FULL },                       /* spring stereo width: 0 mono, 1 wide */
    { &a->dly_in,  pow(P(s, REG_DLYIN) / FULL, 2.0) },              /* sound written into the delay loop: squared, so the quiet end has room */
  };
  for (size_t i = 0; i < sizeof gains / sizeof gains[0]; i++) {
    if (!a->primed) gains[i].ramp->cur = gains[i].next;             /* first chunk: start there, do not fade in */
    ramp_to(gains[i].ramp, gains[i].next);
  }

  /* 4) filters */
  svf_coef(&a->nz_c, fmin(nz_hz(P(s, REG_NZTONE)), 20000.0), 0.5);
  ladder_coef(&a->md_c, md_cut, P(s, REG_MDRES) / FULL);
  a->lpg_ln_ratio = log(cut_hz(P(s, REG_CUTOFF)) / 40.0);

  /* 5) effects */
  a->dly_len = clampd(dly_sec(P(s, REG_DLYTIME)) * FEZ_SAMPLE_RATE, 1.0, DLY_SAMPLES - 2.0);
  a->dly_fb_g = dly_fb(P(s, REG_DLYFB));
  a->dly_damp_a = onepole_coef(dly_lp_hz(P(s, REG_DLYLP)));           /* tape delay: where each echo loses its treble */
  { double w = P(s, REG_DLYWOB) / FULL, n = P(s, REG_DLYNZ) / FULL;
    a->dly_wob = w * w;                                          /* squared: the first part of the knob is subtle */
    a->dly_nz_g = n * n * 0.05; }                                /* at most 5 percent of full scale: audible hiss, not a wall */
  a->dly_frz_target = P(s, REG_DLYFRZ) >= 16384 ? 1.0 : 0.0;
  a->spr_fb_g = spring_fb(P(s, REG_SPRDEC));
  a->spr_tone = P(s, REG_SPRTONE) / FULL;                          /* see SPR_DIFF_LEN: how far from the old metallic spring */
  a->spr_dif_g = SPR_DIFF_G * a->spr_tone;
  for (int t = 0; t < SPR_TANKS; t++)                              /* 14 filters retuned once a chunk: cheap, and the Q glides with the knob */
    for (int i = 0; i < SPR_ALLPASS; i++) svf_coef(&a->spr_ap_c[t][i], SPR_AP_HZ[t][i], SPR_Q_METAL + (SPR_Q_SOFT - SPR_Q_METAL) * a->spr_tone);

  a->primed = 1;
}

/* ---------------------------------------------------------------- one sample */

/* One sample of the whole synth, as a left and a right value. Everything up to the spring is mono; the spring makes it stereo. */
static void render_sample(FezSynth *s, double *out_l, double *out_r) {
  Audio *a = &s->audio;

  /* ---- sources ---- */
  double dt_md = fabs(a->f_md) / FEZ_SAMPLE_RATE;
  /* The mod oscillator, as in west-coast synthesis: one oscillator that is both the FM source (it moves the complex oscillator's
     pitch, by FMAMT) and the mod voice's sound. MDPITCH tunes it like PITCH. */
  double m = wave(a->md_shape, a->ph_md, dt_md);
  a->ph_md = advance_phase(a->ph_md, a->f_md);
  double zraw = white(a);
  double nz = svf_lowpass(&a->nz_svf, &a->nz_c, zraw * 0.7);       /* noise through its tone control (0.7: a level set by ear) */

  /* complex oscillator: a sine plus a shaped wave on the same phase, FM'd by the mod oscillator */
  double f = a->f_cx + m * a->fm_depth;
  double dt_cx = fabs(f) / FEZ_SAMPLE_RATE;
  double sine = sin(TWO_PI * a->ph_cx);
  double shaped = wave(a->cx_shape + 1, a->ph_cx, dt_cx);           /* cx_shape counts from triangle, wave() from sine: hence + 1 */
  a->ph_cx = advance_phase(a->ph_cx, f);
  double cx = ramp_next(&a->g_sine) * sine + ramp_next(&a->g_shape) * shaped;
  /* wavefolder: sin(2 pi x) bends the wave back on itself when it gets loud, adding harmonics (the "fold") */
  double folded = sin(TWO_PI * ramp_next(&a->pre) * cx);

  /* ---- mixer ---- */
  double mix_cx = folded * ramp_next(&a->mix_cx);
  double mix_md = m * ramp_next(&a->mix_md);
  double mix_nz = nz * ramp_next(&a->mix_nz);
  double mix = mix_cx + mix_md + mix_nz;

  /* ---- lowpass gate: a vactrol. y is how open it is. ---- */
  double drive = 0;
  if (a->led_left > 0) { drive = 1; a->led_left--; }
  if (s->gate_hold) drive = 1;                                     /* a keyboard key is down: the LED stays on until it is let go, then REL takes over */
  if (drive > 0) {                                                 /* LED on: the photoresistor opens quickly */
    a->yf += (1 - a->yf) * a->k_rise;
    a->ys += (1 - a->ys) * a->k_rise * 0.3;
  } else {
    /* LED off: it closes, and the lower it gets the slower it goes (tau grows as y falls). That long tail is
       the vactrol's signature. A second, slower part adds the photoresistor's "memory". */
    double tau_f = (a->rel / 3.0) * (0.4 + 1.6 * (1.0 - a->yf));
    a->yf -= a->yf * (1.0 - exp(-1.0 / (tau_f * FEZ_SAMPLE_RATE)));
    a->ys -= a->ys * (1.0 - exp(-1.0 / (a->rel * 0.8 * FEZ_SAMPLE_RATE)));
    a->yf = flush_denormal(a->yf); a->ys = flush_denormal(a->ys);
  }
  double y = 0.7 * a->yf + 0.3 * a->ys;
  /* GATELEVEL holds the gate open by a fixed amount; the envelope (the vactrol above) adds to it. 0 is the old behaviour, full is a
     gate that is always wide open, so the voice drones. Both limits are 0 and 1, the vactrol's own range. */
  y = clampd(y + P(s, REG_GATELEVEL) / 32767.0, 0.0, 1.0);
  /* one gate state drives both the level and the cutoff, as in a west-coast lowpass gate: closing the gate also dulls the sound */
  double gate_hz = 40.0 * exp(y * a->lpg_ln_ratio);
  double lpg_in = mix + a->buf_prev * ramp_next(&a->out_bp);
  double vca = onepole_run(&a->lpg_lp, onepole_coef(gate_hz), lpg_in) * y;

  /* ---- mod voice: oscillator and noise through a Moog ladder, its own VCA ---- */
  double md_src = m * ramp_next(&a->md_osc_g) + zraw * ramp_next(&a->md_nz_g);
  double md = ladder_run(&a->md_ladder, &a->md_c, md_src) * ramp_next(&a->md_vca);

  /* ---- buffer device. Its input is chosen by BWSRC; its output returns one sample late. ---- */
  int src = s->mem[REG_BWSRC];
  double buf_in = src == 0 ? mix : src == 1 ? vca : src == 2 ? md : vca + md;
  double buf_out = buf_process(&a->buf, buf_in);
  a->buf_prev = buf_out;

  /* ---- effects, in series: the voice, the mod voice and the buffer's return all go through the tape delay, then the
     spring, then the compressor. (The buffer's own input is chosen by BWSRC above, before any of this, so no loop
     runs through the effects.) ---- */
  double dry = vca + md + buf_out * ramp_next(&a->out_bf);

  /* 1) TAPE DELAY: a line with, in its feedback loop, a lowpass (each echo duller), a trace of hiss, a soft clip,
     and a wobbling read point. Everything in the loop has a gain of at most 1, so even 100 percent feedback holds. */

  /* the tape's speed wanders: a slow wow (0.6 Hz), a faster flutter (6.3 Hz) and a random drift (about 1 Hz, which is
     white noise through a one-pole lowpass). Together they move the read point a few ms, which bends the pitch.
     ponytail: sines and one noise lowpass; a real tape's motion is messier, add more terms if it sounds too regular. */
  a->ph_wow = advance_phase(a->ph_wow, 0.6);
  a->ph_flt = advance_phase(a->ph_flt, 6.3);
  a->drift += (white(a) - a->drift) * 1.05e-4;
  double drift = clampd(a->drift * 200.0, -1.0, 1.0);
  double wob = a->dly_wob * (240.0 * sin(TWO_PI * a->ph_wow) + 12.0 * sin(TWO_PI * a->ph_flt) + 100.0 * drift);   /* in samples */

  /* freeze glides (about 10 ms) so engaging it does not click */
  a->dly_frz += (a->dly_frz_target - a->dly_frz) * 0.002;
  if (fabs(a->dly_frz - a->dly_frz_target) < 1e-9) a->dly_frz = a->dly_frz_target;   /* arrive exactly: the glide alone never quite gets there */
  double frz = a->dly_frz;

  /* read between two samples (linear interpolation); the time itself also glides (DLYTIME is smoothed), so changing
     it bends the echoes like a tape machine whose speed you turn */
  double len = clampd(a->dly_len + wob, 1.0, DLY_SAMPLES - 2.0);
  /* Reading between two samples averages them, which dulls the sound a little on every trip round the loop. That is
     right for an echo but would make a frozen loop slowly fade, so as freeze engages the read point slides onto a
     whole sample, where the read is an exact copy. */
  len -= frz * (len - floor(len + 0.5));
  double rp = a->dly_w - len;
  if (rp < 0) rp += DLY_SAMPLES;
  a->dly_rp = rp;                                    /* remembered only so the screen can draw the read head */
  int i0 = (int)rp;
  if (i0 >= DLY_SAMPLES) i0 -= DLY_SAMPLES;          /* a tiny negative rp plus DLY_SAMPLES can round up to exactly DLY_SAMPLES */
  int i1 = i0 + 1 >= DLY_SAMPLES ? 0 : i0 + 1;
  double fr = rp - i0;
  double d_out = a->dly[i0] * (1 - fr) + a->dly[i1] * fr;
  double lp = onepole_run(&a->dly_damp, a->dly_damp_a, d_out);

  /* Frozen means: the loop is a perfect copy of itself. No input, no hiss, no lowpass, feedback exactly 1, no soft clip
     (the soft clip is what makes ordinary echoes die away, so a frozen loop would fade without this). Each part below is
     blended by frz rather than switched, so the change glides. */
  double damped = d_out + (lp - d_out) * (1.0 - frz);
  double g = a->dly_fb_g + (1.0 - a->dly_fb_g) * frz;
  double loop = tanh(damped * g) * (1.0 - frz) + damped * g * frz;
  double hiss = white(a) * a->dly_nz_g * (1.0 - frz);
  a->dly[a->dly_w] = (float)flush_denormal(dry * ramp_next(&a->dly_in) * (1.0 - frz) + hiss + loop);   /* DLYIN scales only what is written into the loop */
  if (++a->dly_w >= DLY_SAMPLES) a->dly_w = 0;
  /* DLYMIX is a true dry/wet crossfade: 0 is the dry sound alone, 1 is the loop alone, so with the loop's input
     closed (DLYIN 0) and MIX at full you hear only what the tape holds. The two gains are cos and sin of the same angle
     (an "equal power" crossfade), so halfway is not 3 dB quieter than either end, as a straight-line crossfade would be. */
  double stage1 = dry * ramp_next(&a->dly_dry) + damped * ramp_next(&a->dly_wet);

  /* 2) SPRING, as a stereo pair. Each tank is a short loop with a chain of allpass filters, which delay low frequencies more
     than high ones, so each echo is a "chirp" like a real spring tank. Both tanks are fed by the delay stage's output, but
     they are tuned differently (see SPR_LEN) and each is also fed a little of the other, so the two ears get related but
     different reverb. Steps: (a) read both tanks, (b) write both loops, (c) blend the two outputs by the width control. */
  double tank[SPR_TANKS];
  for (int t = 0; t < SPR_TANKS; t++) {                                       /* (a) */
    /* read the loop SPR_LEN back, less what the diffusers add, plus the slow wobble (between two samples, so it glides) */
    a->ph_spr[t] = advance_phase(a->ph_spr[t], SPR_WOB_HZ[t]);
    double back = SPR_LEN[t] - SPR_DIFF_LEN[t][0] - SPR_DIFF_LEN[t][1] - SPR_DIFF_LEN[t][2]
                + SPR_WOB * a->spr_tone * (1.0 + sin(TWO_PI * a->ph_spr[t]));          /* 0 to 2 x SPR_WOB: never reads ahead */
    double rp = a->spr_w - back;
    if (rp < 0) rp += SPR_SAMPLES;
    int i0 = (int)rp; double fr = rp - i0;
    if (i0 >= SPR_SAMPLES) i0 -= SPR_SAMPLES;
    int i1 = i0 + 1 >= SPR_SAMPLES ? 0 : i0 + 1;
    double x = a->spr[t][i0] * (1 - fr) + a->spr[t][i1] * fr;
    /* the diffusers: Schroeder allpasses. Each delays by its length D and mixes in -g of its input and +g of its own
       output, which spreads an echo out without colouring it (every frequency passes at the same level). At g 0 each is a
       plain delay of D, which is why the read above is shortened by the three D: at SPRTONE 0 the loop is the old one. */
    for (int k = 0; k < SPR_DIFF; k++) {
      float *ring = a->spr_dif[t][k];
      double delayed = ring[(a->spr_dif_w - SPR_DIFF_LEN[t][k] + SPR_DIFF_MAX) % SPR_DIFF_MAX];
      double y = delayed - a->spr_dif_g * x;
      ring[a->spr_dif_w] = (float)flush_denormal(x + a->spr_dif_g * y);
      x = y;
    }
    for (int i = 0; i < SPR_ALLPASS; i++) x = svf_allpass(&a->spr_ap[t][i], &a->spr_ap_c[t][i], x);
    tank[t] = svf_lowpass(&a->spr_svf[t], &a->spr_c[t], x);
  }
  for (int t = 0; t < SPR_TANKS; t++) {                                       /* (b) the loop is the tank's own output mixed with the other's, soft clipped */
    double fed = (1.0 - SPR_CROSS) * tank[t] + SPR_CROSS * tank[1 - t];
    a->spr[t][a->spr_w] = (float)flush_denormal(stage1 + tanh(fed * a->spr_fb_g));
  }
  if (++a->spr_w >= SPR_SAMPLES) a->spr_w = 0;
  if (++a->spr_dif_w >= SPR_DIFF_MAX) a->spr_dif_w = 0;
  /* (c) width by mid and side: mid is what the tanks share, side is how they differ. Width 0 keeps only the mid, so both ears
     hear the same signal (mono); full width gives each ear its own tank. This changes smoothly, with no retuning clicks. */
  double wet_g = ramp_next(&a->spr_wet), wid = ramp_next(&a->spr_wid);
  double mid = 0.5 * (tank[0] + tank[1]), side = 0.5 * (tank[0] - tank[1]) * wid;
  double bus_l = stage1 + (mid + side) * wet_g;
  double bus_r = stage1 + (mid - side) * wet_g;

  /* ---- compressor, master ---- */
  /* compressor: follow the louder ear; above -10 dB (0.316) reduce it so every 8 dB in gives 1 dB out. One detector and one
     gain for both ears, so the compressor never moves the stereo image. */
  double lvl = fmax(fabs(bus_l), fabs(bus_r));
  a->comp_env += (lvl - a->comp_env) * (lvl > a->comp_env ? a->comp_ka : a->comp_kr);
  const double thr = 0.3162;
  double gain = a->comp_env > thr ? pow(a->comp_env / thr, 1.0 / 8.0 - 1.0) : 1.0;
  double out_gain = gain * ramp_next(&a->master);
  double l = bus_l * out_gain, r = bus_r * out_gain;

  /* safety: never output NaN, never exceed full scale (a final hard limit protects speakers and ears) */
  if (!(l == l) || !(r == r)) { a->nan_count++; l = r = 0; }    /* counted, so tests and tools can still see that it happened */
  *out_l = clampd(l, -1.0, 1.0);
  *out_r = clampd(r, -1.0, 1.0);
}

/* Fill `out` with interleaved stereo: left, right, left, right ... (2 * frames floats). */
void fez_render_stereo(FezSynth *s, float *out, int frames) {
  Audio *a = &s->audio;
  for (int i = 0; i < frames; i++) {
    if (a->chunk_pos == 0) begin_chunk(s);
    double l, r;
    render_sample(s, &l, &r);
    out[2 * i] = (float)l; out[2 * i + 1] = (float)r;
    if (++a->chunk_pos == CHUNK) a->chunk_pos = 0;
  }
}

/* The same sound mixed down to one channel, (left + right) / 2: for the tools and tests that want a mono signal.
   It renders stereo in short pieces and mixes them, so there is one render loop. */
void fez_render(FezSynth *s, float *out, int frames) {
  float lr[2 * 256];
  while (frames > 0) {
    int n = frames < 256 ? frames : 256;
    fez_render_stereo(s, lr, n);
    for (int i = 0; i < n; i++) out[i] = 0.5f * (lr[2 * i] + lr[2 * i + 1]);
    out += n; frames -= n;
  }
}
