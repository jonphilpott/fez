/* See filters.h. */
#include "filters.h"
#include "rate.h"           /* FEZ_SAMPLE_RATE */
#include <math.h>

#define PI_D 3.14159265358979323846

double flush_denormal(double x) { return fabs(x) < 1e-20 ? 0.0 : x; }

/* Keep cutoffs below about 0.45 of the sample rate, where the maths for tan() and exp() stays well behaved. */
static double limit_hz(double hz) {
  double top = 0.45 * FEZ_SAMPLE_RATE;
  return hz < 1 ? 1 : hz > top ? top : hz;
}

/* ---------------------------------------------------------------- one-pole */

/* A one-pole lowpass moves a fixed fraction `a` of the remaining distance to the input each sample.
   For cutoff f, a = 1 - e^(-2 pi f / sample rate). */
double onepole_coef(double hz) { return 1.0 - exp(-2.0 * PI_D * limit_hz(hz) / FEZ_SAMPLE_RATE); }

double onepole_run(OnePole *f, double a, double x) {
  f->y += a * (x - f->y);
  f->y = flush_denormal(f->y);
  return f->y;
}

/* ---------------------------------------------------------------- state-variable filter */

/* Two integrators in a loop. g = tan(pi f / sr) "pre-warps" the cutoff so it lands exactly on f even near the
   top of the spectrum; a1..a3 fold the loop's algebra into three constants so each sample needs only a few
   multiplies. q = 0.707 is the flattest lowpass (no peak, so its gain never exceeds 1). */
void svf_coef(SvfCoef *c, double hz, double q) {
  double g = tan(PI_D * limit_hz(hz) / FEZ_SAMPLE_RATE);
  c->k = 1.0 / q;
  c->a1 = 1.0 / (1.0 + g * (g + c->k));
  c->a2 = g * c->a1;
  c->a3 = g * c->a2;
}

/* One step of the filter. Returns lowpass in *lp and bandpass in *bp. */
static void svf_step(Svf *f, const SvfCoef *c, double x, double *lp, double *bp) {
  double v3 = x - f->ic2;
  double v1 = c->a1 * f->ic1 + c->a2 * v3;
  double v2 = f->ic2 + c->a2 * f->ic1 + c->a3 * v3;
  f->ic1 = flush_denormal(2.0 * v1 - f->ic1);
  f->ic2 = flush_denormal(2.0 * v2 - f->ic2);
  *lp = v2; *bp = v1;
}

double svf_lowpass(Svf *f, const SvfCoef *c, double x) { double lp, bp; svf_step(f, c, x, &lp, &bp); return lp; }

/* An allpass passes every frequency at full level but shifts the phase around its centre. Lowpass + highpass
   - k*bandpass ... which simplifies to x - 2k*bandpass. */
double svf_allpass(Svf *f, const SvfCoef *c, double x) { double lp, bp; svf_step(f, c, x, &lp, &bp); return x - 2.0 * c->k * bp; }

/* ---------------------------------------------------------------- ladder */

/* Four one-pole stages in a row, with the output fed back to the input (negative feedback of strength k).
   The feedback is what makes it resonate. Each stage is a "trapezoidal" one-pole: y = G*x + (1-G)*state with
   G = g/(1+g), g = tan(pi f / sr). Because every stage depends on the one before, the whole chain's response
   to the input right now is G^4; the rest comes from the stored states. That lets us solve the feedback loop
   exactly instead of guessing with a one-sample delay (which would detune the resonance). */
void ladder_coef(LadderCoef *c, double hz, double resonance) {
  double g = tan(PI_D * limit_hz(hz) / FEZ_SAMPLE_RATE);
  c->G = g / (1.0 + g);
  c->g1 = 1.0 / (1.0 + g);
  c->k = 4.1 * (resonance < 0 ? 0 : resonance > 1 ? 1 : resonance);
}

double ladder_run(Ladder *f, const LadderCoef *c, double x) {
  double G = c->G, g1 = c->g1;
  /* what the four stages would output with zero input, from their stored states */
  double b1 = f->s[0] * g1, b2 = f->s[1] * g1, b3 = f->s[2] * g1, b4 = f->s[3] * g1;
  double G2 = G * G, G3 = G2 * G, G4 = G2 * G2;
  double rest = G3 * b1 + G2 * b2 + G * b3 + b4;
  /* solve u = x - k * y4 for the stage input u, where y4 = G^4 u + rest */
  double u = (x - c->k * rest) / (1.0 + c->k * G4);
  u = tanh(u);                                       /* saturation: keeps resonance bounded and adds a little warmth */
  double in = u;
  for (int i = 0; i < 4; i++) {                      /* run the four stages and update their states */
    double v = (in - f->s[i]) * G;
    double y = v + f->s[i];
    f->s[i] = flush_denormal(y + v);
    in = y;
  }
  return in;
}
