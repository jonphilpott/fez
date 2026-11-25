/* Audio tests for the stereo spring. These do not judge how it sounds (only ears can); they catch the failures a machine can see:
   a mono spring that was meant to be stereo, a wide one that is not, loops that grow without bound, NaN. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "fez.h"
#include "test.h"

static FezSynth s;
static float lr[2 * 4800];

/* Set up a plain voice into the spring only: no tape delay echo, the spring wet and its feedback as given. */
static void spring_patch(int sprmix, int sprdec, int sprwid) {
  fez_init(&s, 1);
  fez_reg_write(&s, REG_MIXCX, 20000); fez_reg_write(&s, REG_CUTOFF, 32767); fez_reg_write(&s, REG_FOLD, 0);
  fez_reg_write(&s, REG_FMAMT, 0); fez_reg_write(&s, REG_ATK, 0); fez_reg_write(&s, REG_REL, 24000);
  fez_reg_write(&s, REG_DLYMIX, 0); fez_reg_write(&s, REG_DLYIN, 0);
  fez_reg_write(&s, REG_SPRMIX, sprmix); fez_reg_write(&s, REG_SPRDEC, sprdec); fez_reg_write(&s, REG_SPRWID, sprwid);
}

/* Play a note for `blocks` blocks of 4800 frames; report the largest left-right difference, the peak, and whether anything was NaN. */
static void run(int blocks, double *diff, double *peak, int *bad) {
  *diff = *peak = 0; *bad = 0;
  fez_note_on(&s, 57);
  for (int b = 0; b < blocks; b++) {
    if (b == 4) fez_note_off(&s, 57);                              /* let the spring ring on its own after the note */
    fez_render_stereo(&s, lr, 4800);
    for (int i = 0; i < 4800; i++) {
      double l = lr[2 * i], r = lr[2 * i + 1];
      if (!(l == l) || !(r == r)) *bad = 1;
      if (fabs(l - r) > *diff) *diff = fabs(l - r);
      if (fabs(l) > *peak) *peak = fabs(l);
      if (fabs(r) > *peak) *peak = fabs(r);
    }
  }
}

void test_audio(void) {
  double diff, peak; int bad;

  spring_patch(0, 16000, 32767);                                   /* spring off: the two ears carry the very same signal */
  run(8, &diff, &peak, &bad);
  CHECK(diff == 0 && peak > 0.01, "with the spring off, left equals right");

  spring_patch(20000, 16000, 0);                                   /* spring on, width 0: still mono, exactly */
  run(8, &diff, &peak, &bad);
  CHECK(diff == 0 && peak > 0.01, "width 0 gives a mono spring (left equals right)");

  spring_patch(20000, 16000, 32767);                               /* spring on, width full: two different signals */
  run(8, &diff, &peak, &bad);
  CHECK(diff > 0.005, "full width gives a stereo spring (the ears differ)");

  /* the mono mix is the same sound as the stereo one: fez_render is (left + right) / 2 */
  { static float mono[4800]; static FezSynth t;
    spring_patch(20000, 16000, 32767); fez_note_on(&s, 57);
    t = s;                                                         /* a copy of the whole synth, so both start in the same state */
    fez_render_stereo(&s, lr, 4800); fez_render(&t, mono, 4800);
    double worst = 0; for (int i = 0; i < 4800; i++) { double d = fabs(mono[i] - 0.5 * (lr[2 * i] + lr[2 * i + 1])); if (d > worst) worst = d; }
    CHECK(worst < 1e-6, "fez_render is the mono mix of fez_render_stereo"); }

  /* the loops must stay bounded and finite at their worst settings: feedback full, wet full, width full, a loud note held then released.
     The two tanks feed each other, so this is the case that could run away. */
  spring_patch(32767, 32767, 32767);
  fez_reg_write(&s, REG_MIXCX, 32767);
  run(100, &diff, &peak, &bad);                                    /* 10 seconds */
  CHECK(!bad && peak <= 1.0, "spring at maximum settings stays finite and within full scale");

  /* a patch load empties the spring tails in both tanks. A short release means the voice itself is silent soon after the note
     ends, so anything still heard before the load is the spring ringing, and anything heard after it would be a tail left behind. */
  spring_patch(32767, 32767, 32767); fez_reg_write(&s, REG_REL, 0);
  run(8, &diff, &peak, &bad);
  CHECK(peak > 0.05, "the spring is still ringing after the note has ended");
  fez_load_blank(&s);
  fez_reg_write(&s, REG_SPRMIX, 32767);
  fez_render_stereo(&s, lr, 4800);                                  /* the clear is delivered on the next 5 ms control tick: let it land */
  fez_render_stereo(&s, lr, 4800);
  { double quiet = 0; for (int i = 0; i < 2 * 4800; i++) if (fabs(lr[i]) > quiet) quiet = fabs(lr[i]);
    CHECK(quiet < 0.02, "a patch load empties both spring tanks"); }
}
