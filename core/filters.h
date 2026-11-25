/* The filters. Three kinds, each a few lines, with no tables and no allocation.

   one-pole   6 dB per octave lowpass. The vactrol lowpass gate and the delay damper use it.
   svf        2-pole state-variable filter (a "topology-preserving transform" design by Zavalishin). Stays
              stable while its cutoff is modulated. Gives lowpass or allpass. Noise tone and spring use it.
   ladder     4-pole, 24 dB per octave, Moog style, with resonance and gentle saturation. The mod voice uses it.

   Every coefficient function takes a frequency in hertz; coefficients are computed once per chunk of samples,
   not per sample, except where noted. */
#ifndef FEZ_FILTERS_H
#define FEZ_FILTERS_H

/* ---- one-pole lowpass ---- */
typedef struct { double y; } OnePole;
double onepole_coef(double hz);                          /* a in y += a * (x - y) */
double onepole_run(OnePole *f, double a, double x);

/* ---- state-variable filter ---- */
typedef struct { double ic1, ic2; } Svf;                 /* the two integrator states */
typedef struct { double a1, a2, a3, k; } SvfCoef;        /* k = 1/Q: damping */
void   svf_coef(SvfCoef *c, double hz, double q);
double svf_lowpass(Svf *f, const SvfCoef *c, double x);
double svf_allpass(Svf *f, const SvfCoef *c, double x);

/* ---- Moog-style ladder ---- */
typedef struct { double s[4]; } Ladder;                  /* one state per pole */
typedef struct { double G, g1, k; } LadderCoef;
/* resonance 0..1 maps to feedback 0..4.1. Feedback 4 is where the loop would sustain by itself, so the top of
   the range self-oscillates; the input saturation keeps the level bounded. */
void   ladder_coef(LadderCoef *c, double hz, double resonance);
double ladder_run(Ladder *f, const LadderCoef *c, double x);

/* Replace tiny numbers with zero. Filters in feedback loops decay toward zero forever, and the processor slows
   to a crawl on the "denormal" numbers near it. */
double flush_denormal(double x);

#endif
