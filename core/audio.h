/* State of the audio side: the voice, the effects and the buffer device. Only audio.c touches these fields; they
   live in a struct (rather than static variables) so the whole synth is one block of memory the caller owns. */
#ifndef FEZ_AUDIO_H
#define FEZ_AUDIO_H

#include <stdint.h>
#include "buf.h"
#include "filters.h"

#define CHUNK        16                 /* samples per control update of the audio parameters (0.33 ms) */
#define TICK_CHUNKS  15                 /* chunks per control tick: 15 x 16 = 240 samples = 5 ms */
#define PARAM_MAX    48                 /* room for every parameter register */
#define DLY_SAMPLES  (FEZ_SAMPLE_RATE * 12 + 8)      /* longest delay time is 12 s (2.3 MB as floats) */
#define SPR_SAMPLES  2048               /* each spring tank's loop is 34 or 41 ms (1632 and 1968 samples) */
#define SPR_ALLPASS  7
#define SPR_TANKS    2                  /* the spring is a stereo pair: tank 0 feeds the left ear, tank 1 the right */

/* A gain that moves linearly from where it is to a new target over one chunk, so a parameter change never
   makes a step (a step in gain is a click). */
typedef struct { double cur, step; } Ramp;

typedef struct {
  /* ---- parameters: each register value smoothed over time (a one-pole, time constant from PARAM_TABLE) ---- */
  double sm[PARAM_MAX];
  double k_sm[PARAM_MAX];               /* per-chunk smoothing coefficient for each parameter */
  int primed;                           /* the smoothers have been set to their first values */
  int chunk_pos, chunks_to_tick;        /* position in the current chunk; chunks until the next control tick */

  /* ---- oscillators ---- */
  double ph_cx, ph_md;                  /* phases, 0..1 */
  uint32_t nz_state;                    /* white-noise generator (separate from the synth's, which must stay deterministic) */
  double f_cx, f_md, fm_depth;          /* base frequencies in Hz, FM depth in Hz (per chunk) */
  int cx_shape, md_shape;

  /* ---- gains (ramped) ---- */
  Ramp g_sine, g_shape, pre, mix_cx, mix_md, mix_nz, md_osc_g, md_nz_g, md_vca, master, out_bf, out_bp, dly_dry, dly_wet, spr_wet, spr_wid, dly_in;
  double dly_fb_g, spr_fb_g;            /* feedback gains (per chunk) */

  /* ---- noise tone, mod-voice ladder, lowpass gate ---- */
  Svf nz_svf; SvfCoef nz_c;
  Ladder md_ladder; LadderCoef md_c;
  double lpg_ln_ratio;                  /* ln(cutoff ceiling / 40 Hz); the gate's cutoff = 40 Hz * e^(y * this) */
  OnePole lpg_lp;

  /* ---- the vactrol: a photoresistor lit by an LED. y is how open the gate is, 0..1. ---- */
  double yf, ys;                        /* fast and slow parts of the photoresistor's response */
  int led_left;                         /* samples the LED stays on */
  double k_rise, rel;                   /* rise coefficient (per sample); release time of the last gate, seconds */

  /* ---- effects ---- */
  float dly[DLY_SAMPLES]; int dly_w; double dly_len;        /* delay line, write index, current delay in samples */
  OnePole dly_damp; double dly_damp_a;                      /* the tape delay's feedback lowpass and its coefficient */
  double dly_wob, dly_nz_g, dly_frz_target;                  /* per chunk: wobble depth (0..1), hiss gain, freeze (0 or 1) */
  double dly_frz;                                            /* freeze, glided to its target so engaging it does not click */
  double dly_rp;                                             /* where the tape was read last (in samples; wobble included), for the display */
  double ph_wow, ph_flt, drift;                              /* the tape's wobble: slow wow, fast flutter, random drift */
  float spr[SPR_TANKS][SPR_SAMPLES]; int spr_w;               /* the two spring tanks' rings, and the one write index they share */
  Svf spr_ap[SPR_TANKS][SPR_ALLPASS]; SvfCoef spr_ap_c[SPR_TANKS][SPR_ALLPASS];
  Svf spr_svf[SPR_TANKS]; SvfCoef spr_c[SPR_TANKS];
  double comp_env, comp_ka, comp_kr;    /* compressor level follower and its attack/release coefficients (one detector for both ears) */

  /* ---- buffer device ---- */
  BufEngine buf;
  double buf_prev;                      /* its output one sample ago (used to break the loop through the gate) */

  unsigned long nan_count;              /* samples where the output was NaN and was replaced by silence (a bug if ever above 0) */
} Audio;

void audio_init(Audio *a);
void audio_clear_fx(Audio *a);        /* empty the delay, spring and compressor (state only, not settings) */

#endif
