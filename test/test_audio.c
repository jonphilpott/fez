/* Audio tests for the stereo spring. These do not judge how it sounds (only ears can); they catch the failures a machine can see:
   a mono spring that was meant to be stereo, a wide one that is not, loops that grow without bound, NaN. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "fez.h"
#include "test.h"
#include "regview.h"
#include "scales.h"

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
      if (s.audio.nan_count) *bad = 1;                              /* the core silences NaN, but counts it */
      if (fabs(l - r) > *diff) *diff = fabs(l - r);
      if (fabs(l) > *peak) *peak = fabs(l);
      if (fabs(r) > *peak) *peak = fabs(r);
    }
  }
}


static double gate_peak_from(int level, int fire, int blocks, int from);
static double gate_peak(int level, int fire, int blocks) { return gate_peak_from(level, fire, blocks, blocks - 2); }
/* The loudest sample in `blocks` blocks of 4800 frames, after the voice has been set up and (optionally) the gate fired once. */
static double gate_peak_from(int level, int fire, int blocks, int from) {
  fez_init(&s, 1);
  fez_load_blank(&s);
  fez_reg_write(&s, REG_MIXCX, 20000); fez_reg_write(&s, REG_CUTOFF, 32767); fez_reg_write(&s, REG_FOLD, 0);
  fez_reg_write(&s, REG_FMAMT, 0); fez_reg_write(&s, REG_DLYMIX, 0); fez_reg_write(&s, REG_SPRMIX, 0);
  fez_reg_write(&s, REG_ATK, 0); fez_reg_write(&s, REG_REL, 4000);          /* a short envelope, so a fired gate has faded before the end */
  fez_reg_write(&s, REG_GATELEVEL, level);
  if (fire) fez_reg_write(&s, REG_GATE, 1);
  double peak = 0;
  for (int b = 0; b < blocks; b++) {
    fez_render_stereo(&s, lr, 4800);
    if (b < from) continue;                                                  /* the last two blocks are measured once any fired envelope is long gone; the first two catch the envelope */
    for (int i = 0; i < 4800; i++) { double l = fabs(lr[2 * i]); if (l > peak) peak = l; }
  }
  return peak;
}

void test_audio(void) {
  double diff, peak; int bad;

  /* A one-shot writes over BWLEN of the tape from where the head is, and keeps the rest (it used to empty the whole tape
     and start from the top) */
  { static float buf[2 * 4800];
    fez_init(&s, 1); fez_load_blank(&s);
    fez_reg_write(&s, REG_BUFMS, 100); fez_reg_write(&s, REG_BWSRC, 0); fez_reg_write(&s, REG_MIXCX, 32767);   /* the raw drone */
    for (int i = 0; i < 4; i++) fez_render_stereo(&s, buf, 4800);                          /* 0.4 s: the 0.1 s tape (4800 samples) is full of it */
    fez_render_stereo(&s, buf, 1200);                                                      /* then a quarter more, so the head is not near the start */
    const BufEngine *b = &s.audio.buf;
    int start = b->wp, full = 1;
    for (int i = 0; i < b->len; i++) full &= b->buf[i] != 0;
    fez_reg_write(&s, REG_BIN, 0); fez_reg_write(&s, REG_BWLEN, 16384);                    /* record silence over half the tape */
    fez_reg_write(&s, REG_BWTRIG, 1);
    for (int i = 0; i < 4; i++) fez_render_stereo(&s, buf, 4800);
    int silent = 1, kept = 1, margin = 40;                                                 /* the event lands at the next chunk: allow a few samples */
    for (int k = margin; k < b->len / 2 - margin; k++) silent &= b->buf[(start + k) % b->len] == 0;
    for (int k = b->len / 2 + margin; k < b->len - margin; k++) kept &= b->buf[(start + k) % b->len] != 0;
    CHECK(start != 0 && full && silent && kept, "a one-shot overwrites the stretch after the head and leaves the rest"); }

  /* MDTRACK: off, MDPITCH is the mod oscillator's own pitch; on, an interval above PITCH (C4 none, G4 a fifth) */
  { static float buf[2 * 4800];
    fez_init(&s, 1); fez_load_blank(&s);
    fez_reg_write(&s, REG_PITCH, 69 * 256); fez_reg_write(&s, REG_MDPITCH, 67 * 256);       /* A4; G4 */
    fez_render_stereo(&s, buf, 4800);
    CHECK(fabs(s.audio.f_md - pitch_hz(67 * 256)) < 0.01, "MDTRACK off: the mod oscillator plays MDPITCH itself (G4)");
    fez_reg_write(&s, REG_MDTRACK, 32767);
    for (int i = 0; i < 10; i++) fez_render_stereo(&s, buf, 4800);
    CHECK(fabs(s.audio.f_md - pitch_hz((69 + 7) * 256)) < 0.01, "MDTRACK on: G4 means a fifth above PITCH (A4 to E5)");
    fez_reg_write(&s, REG_PITCH, 57 * 256);                                                  /* A3: the mod oscillator follows */
    for (int i = 0; i < 20; i++) fez_render_stereo(&s, buf, 4800);
    CHECK(fabs(s.audio.f_md - pitch_hz((57 + 7) * 256)) < 0.05, "MDTRACK on: it follows PITCH (A3 to E4)");
    char t[16]; regview_fmt_interval(t, sizeof t, 67 * 256);
    char u[16]; regview_fmt_interval(u, sizeof u, 48 * 256);
    CHECK(!strcmp(t, "+7st") && !strcmp(u, "-12st"), "the interval reads +7st for G4 and -12st for C3"); }

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
  for (int tone = 0; tone <= 32767; tone += 32767) {               /* both ends of SPRTONE: the old spring, and full diffusion */
    spring_patch(32767, 32767, 32767);
    fez_reg_write(&s, REG_MIXCX, 32767); fez_reg_write(&s, REG_SPRTONE, tone);
    run(100, &diff, &peak, &bad);
    CHECK(!bad && peak <= 1.0, tone ? "full SPRTONE at maximum feedback stays finite and within full scale" : "SPRTONE 0 at maximum feedback likewise");
  }

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

  /* The buffer looping back to itself through the gate (MIXBP into the gate, BWSRC taking the gate's output) with BWFB at 0:
     this second feedback path once grew to NaN, which stuck in the gate's filter and silenced the synth until a restart. */
  fez_init(&s, 1);
  fez_load_blank(&s);
  fez_reg_write(&s, REG_MIXCX, 32767); fez_reg_write(&s, REG_GATELEVEL, 32767);
  fez_reg_write(&s, REG_BWSRC, 1); fez_reg_write(&s, REG_BWMODE, 1); fez_reg_write(&s, REG_BIN, 32767); fez_reg_write(&s, REG_BWFB, 0);
  fez_reg_write(&s, REG_MIXBP, 32767);
  fez_reg_write(&s, REG_GLVL0, 32767); fez_reg_write(&s, REG_GPOS0, 300); fez_reg_write(&s, REG_GFLG0, 1);
  fez_reg_write(&s, REG_GLVL1, 32767); fez_reg_write(&s, REG_GPOS1, 500); fez_reg_write(&s, REG_GFLG1, 1);
  for (int b = 0; b < 100; b++) fez_render_stereo(&s, lr, 4800);   /* 10 seconds */
  CHECK(s.audio.nan_count == 0, "the buffer looping through the gate stays finite");

  /* GATELEVEL: the gate is open by this much plus whatever the envelope adds */
  CHECK(gate_peak(0, 0, 12) < 0.001, "GATELEVEL 0 and no envelope: the gate is shut and the voice is silent");
  CHECK(gate_peak(32767, 0, 12) > 0.02, "GATELEVEL full: the gate stays open and the voice drones with no envelope");
  double half = gate_peak(14000, 0, 12), more = gate_peak(30000, 0, 12);
  CHECK(half > 0.001 && more > half, "a higher GATELEVEL is louder (and brighter)");
  CHECK(gate_peak(0, 1, 12) < 0.001, "a fired envelope alone has faded to silence by the end of the test");
  CHECK(gate_peak_from(14000, 1, 12, 0) > gate_peak_from(14000, 0, 12, 0) * 1.3, "the envelope adds to GATELEVEL: a fired gate is louder than the level alone");
}
