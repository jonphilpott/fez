/* The audio side: turns the control side's effective parameters into sound.

   Signal flow (a Buchla/Music Easel style voice with a second, "mod" voice):

     complex osc (sine + a shaped wave, blended) --> wavefolder --> mixer --+
     mod osc ---------------------------------------------------> mixer ----+--> vactrol lowpass gate --> bus
     noise --> tone lowpass ------------------------------------> mixer ----+      ^ gate envelope
     buffer device (MIXBP) ---------------------------------------------- +

     mod osc / noise --> Moog ladder filter --> mod VCA ------------------------> bus
     (voice + mod voice) --> delay and spring, wet returns ----------------------> bus
     buffer device (MIXBF) ------------------------------------------------------> bus
     bus --> compressor --> master level --> output

   Time structure: sound is made one sample at a time, but parameters only change once per CHUNK of 16 samples
   (0.33 ms): we smooth the registers, convert them to physical units, and ramp gains linearly across the chunk.
   Every 15 chunks (5 ms) the control side ticks. */
#include "fez.h"
#include "scales.h"
#include <math.h>
#include <string.h>

#define TWO_PI 6.28318530717958647692
#define SPR_DELAY ((int)(0.034 * FEZ_SAMPLE_RATE))   /* the spring's loop length: 34 ms */

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
  static const double ap_hz[SPR_ALLPASS] = { 330, 520, 780, 1100, 1500, 2100, 2800 };
  for (int i = 0; i < SPR_ALLPASS; i++) svf_coef(&a->spr_ap_c[i], ap_hz[i], 2.5);
  svf_coef(&a->spr_c, 3200, 0.7071);                          /* Q 0.7071: flattest lowpass, gain never above 1 */
  a->comp_ka = 1.0 - exp(-1.0 / (0.003 * FEZ_SAMPLE_RATE));   /* compressor attack 3 ms */
  a->comp_kr = 1.0 - exp(-1.0 / (0.2 * FEZ_SAMPLE_RATE));     /* release 200 ms */
  a->dly_len = 0.3 * FEZ_SAMPLE_RATE;
  a->rel = 0.2;                                               /* so the gate maths never divides by zero before the first note */
}

/* ---------------------------------------------------------------- clearing the effects */

/* Empty everything that remembers sound: the delay ring and its feedback lowpass, the spring's ring and filters, and the
   compressor's level follower. Only state is cleared. Settings (delay length, filter coefficients, freeze) are left alone, because
   the new patch's registers are about to set them. memset on the arrays is safe here: float and double zero is all-bits-zero. */
void audio_clear_fx(Audio *a) {
  memset(a->dly, 0, sizeof a->dly);
  a->dly_damp.y = 0;
  memset(a->spr, 0, sizeof a->spr);
  memset(a->spr_ap, 0, sizeof a->spr_ap);
  memset(&a->spr_svf, 0, sizeof a->spr_svf);
  a->comp_env = 0;
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
  if (a->chunks_to_tick == 0) { fez_tick(s, 5.0); service_events(s); a->chunks_to_tick = TICK_CHUNKS; }
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
  a->f_md = fm_hz(P(s, REG_FMRATE));
  a->fm_depth = fm_depth_hz(P(s, REG_FMAMT));
  a->cx_shape = cx_shape(s->eff[REG_CXWAVE]);
  a->md_shape = md_shape(s->eff[REG_MDWAVE]);

  /* 3) gains. Blends are "equal power": cos and sin of an angle, so the loudness stays steady across the blend. */
  static const double cx_scale[3] = { 1.0, 0.55, 0.75 };                       /* level-match the shapes */
  static const double md_scale[4] = { 1.0, 1.0, 0.5, 0.65 };
  double bl = P(s, REG_CXBLEND) / FULL * HALF_PI;
  double mdb = P(s, REG_MDNZ) / FULL * HALF_PI;
  double md_cut = cut_hz(P(s, REG_MDCUT));
  /* White noise through a low cutoff carries little energy, so lift it as the cutoff falls to keep sweeps level. */
  double lift = clampd(sqrt(8000.0 / md_cut), 1.0, 6.0);
  double next[15] = {
    cos(bl), sin(bl) * cx_scale[a->cx_shape],                                  /* g_sine, g_shape */
    0.25 + 0.75 * P(s, REG_FOLD) / FULL,                                            /* pre: drive into the folder */
    P(s, REG_MIXCX) / FULL, P(s, REG_MIXMD) / FULL, P(s, REG_MIXNZ) / FULL,                      /* mixer: complex, mod, noise */
    cos(mdb) * md_scale[a->md_shape], sin(mdb) * 0.9 * lift,                   /* mod voice: oscillator, noise */
    pow(P(s, REG_MDLVL) / FULL, 1.5),                                                /* mod voice VCA */
    pow(P(s, REG_LEVEL) / FULL, 1.5) * 1.8,                                          /* master level */
    P(s, REG_MIXBF) / FULL, P(s, REG_MIXBP) / FULL,                                        /* buffer out: to bus, to the gate */
    P(s, REG_DLYMIX) / FULL, P(s, REG_SPRMIX) / FULL * 1.4,                                  /* delay wet, spring wet */
    pow(P(s, REG_DLYIN) / FULL, 2.0)       /* how much sound is written into the delay loop: squared, so the quiet end has room */
  };
  Ramp *r[15] = { &a->g_sine, &a->g_shape, &a->pre, &a->mix_cx, &a->mix_md, &a->mix_nz, &a->md_osc_g, &a->md_nz_g,
                  &a->md_vca, &a->master, &a->out_bf, &a->out_bp, &a->dly_wet, &a->spr_wet, &a->dly_in };
  for (int i = 0; i < 15; i++) {
    if (!a->primed) r[i]->cur = next[i];                                       /* first chunk: start there, do not fade in */
    ramp_to(r[i], next[i]);
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

  a->primed = 1;
}

/* ---------------------------------------------------------------- one sample */

static double render_sample(FezSynth *s) {
  Audio *a = &s->audio;

  /* ---- sources ---- */
  double dt_md = fabs(a->f_md) / FEZ_SAMPLE_RATE;
  double m = wave(a->md_shape, a->ph_md, dt_md);                  /* mod oscillator, also the FM source */
  a->ph_md = advance_phase(a->ph_md, a->f_md);
  double zraw = white(a);
  double nz = svf_lowpass(&a->nz_svf, &a->nz_c, zraw * 0.7);       /* noise through its tone control (0.7: as the JS buffer) */

  /* complex oscillator: a sine plus a shaped wave on the same phase, FM'd by the mod oscillator */
  double f = a->f_cx + m * a->fm_depth;
  double dt_cx = fabs(f) / FEZ_SAMPLE_RATE;
  double sine = sin(TWO_PI * a->ph_cx);
  double shaped = a->cx_shape == 0 ? wave(1, a->ph_cx, dt_cx) : a->cx_shape == 1 ? wave(2, a->ph_cx, dt_cx) : wave(3, a->ph_cx, dt_cx);
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
  /* one gate state drives both the level and the cutoff, like a Buchla 292: closing the gate also dulls the sound */
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
  int i0 = (int)rp, i1 = i0 + 1 >= DLY_SAMPLES ? 0 : i0 + 1;
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
     closed (DLYIN 0) and MIX at full you hear only what the tape holds.
     ponytail: a linear crossfade dips about 3 dB near half; an equal-power curve would hold the level if that bothers. */
  double wet = ramp_next(&a->dly_wet);
  double stage1 = dry * (1.0 - wet) + damped * wet;

  /* 2) SPRING: a short loop (34 ms) with a chain of allpass filters, which delay low frequencies more than high
     ones, so each echo is a "chirp" like a real spring tank. It is fed by the delay stage's output. */
  double sp = a->spr[(a->spr_w - SPR_DELAY + SPR_SAMPLES) % SPR_SAMPLES];
  for (int i = 0; i < SPR_ALLPASS; i++) sp = svf_allpass(&a->spr_ap[i], &a->spr_ap_c[i], sp);
  double sp_lp = svf_lowpass(&a->spr_svf, &a->spr_c, sp);
  a->spr[a->spr_w] = (float)flush_denormal(stage1 + tanh(sp_lp * a->spr_fb_g));
  if (++a->spr_w >= SPR_SAMPLES) a->spr_w = 0;
  double stage2 = stage1 + sp_lp * ramp_next(&a->spr_wet);

  /* ---- bus, compressor, master ---- */
  double bus = stage2;
  /* compressor: follow the level; above -10 dB (0.316) reduce it so every 8 dB in gives 1 dB out */
  double lvl = fabs(bus);
  a->comp_env += (lvl - a->comp_env) * (lvl > a->comp_env ? a->comp_ka : a->comp_kr);
  const double thr = 0.3162;
  double gain = a->comp_env > thr ? pow(a->comp_env / thr, 1.0 / 8.0 - 1.0) : 1.0;
  double out = bus * gain * ramp_next(&a->master);

  /* safety: never output NaN, never exceed full scale (a final hard limit protects speakers and ears) */
  if (!(out == out)) out = 0;
  return clampd(out, -1.0, 1.0);
}

void fez_render(FezSynth *s, float *out, int frames) {
  Audio *a = &s->audio;
  for (int i = 0; i < frames; i++) {
    if (a->chunk_pos == 0) begin_chunk(s);
    out[i] = (float)render_sample(s);
    if (++a->chunk_pos == CHUNK) a->chunk_pos = 0;
  }
}
