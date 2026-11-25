/* Offline renderer and sound checker (no SDL, no sound card).
     ./render PATCH SECONDS [out.wav]        render patch 1..12, print the level of each second, optionally save a WAV
     ./render PATCH SECONDS out.wav stress   same, with every feedback control at 100 percent and the filter resonance
                                             at maximum: the level must stay bounded
     ./render all                            quick check of every patch: peak, RMS, NaN, silence
   The tool exists because the sound cannot be unit-tested (there is no reference to compare it with). It catches the
   failures a machine can see: silence, clipping, NaN and runaway feedback. Whether it SOUNDS good is for ears. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fez.h"

static FezSynth synth;

static void write_wav(const char *path, const float *x, int n) {
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); return; }
  int bytes = n * 2, rate = FEZ_SAMPLE_RATE;
  unsigned char h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 1,0, 0,0,0,0, 0,0,0,0, 2,0, 16,0, 'd','a','t','a', 0,0,0,0 };
  int riff = 36 + bytes, brate = rate * 2;
  memcpy(h + 4, &riff, 4); memcpy(h + 24, &rate, 4); memcpy(h + 28, &brate, 4); memcpy(h + 40, &bytes, 4);   /* little-endian hosts */
  fwrite(h, 1, 44, f);
  for (int i = 0; i < n; i++) { int v = (int)lrintf(fmaxf(-1, fminf(1, x[i])) * 32767); short s = (short)v; fwrite(&s, 2, 1, f); }
  fclose(f);
}

/* Render and report. Returns the peak of the last second (to judge runaway). */
static double run(int patch, double seconds, const char *wav, int stress, int verbose) {
  fez_init(&synth, 12345);
  fez_load_patch(&synth, patch);
  if (stress) {
    static const int regs[] = { REG_DLYFB, REG_SPRDEC, REG_BWFB, REG_MDRES, REG_DLYMIX, REG_SPRMIX, REG_MIXBP, REG_MIXBF };
    for (size_t i = 0; i < sizeof regs / sizeof *regs; i++) fez_reg_write(&synth, regs[i], 32767);
    fez_reg_write(&synth, REG_MDLVL, 20000);                                       /* MDLVL: let the mod voice sound */
    fez_reg_write(&synth, REG_MIXNZ, 8000);                                        /* MIXNZ: feed the filters some noise */
    fez_reg_write(&synth, REG_GATE, 1);
  }
  int n = (int)(seconds * FEZ_SAMPLE_RATE);
  float *x = malloc(sizeof(float) * (size_t)n);
  fez_render(&synth, x, n);
  int nan = 0; double peak_all = 0, last_peak = 0, sum2 = 0;
  for (int sec = 0; sec * FEZ_SAMPLE_RATE < n; sec++) {
    double pk = 0, s2 = 0; int a = sec * FEZ_SAMPLE_RATE, b = a + FEZ_SAMPLE_RATE < n ? a + FEZ_SAMPLE_RATE : n;
    for (int i = a; i < b; i++) { if (x[i] != x[i]) nan++; double v = fabs(x[i]); if (v > pk) pk = v; s2 += (double)x[i] * x[i]; }
    if (verbose) printf("  second %2d: peak %.3f  rms %.4f\n", sec + 1, pk, sqrt(s2 / (b - a)));
    if (pk > peak_all) peak_all = pk;
    sum2 += s2; last_peak = pk;
  }
  printf("%-14s peak %.3f  rms %.4f  %s%s%s\n", PATCH_TABLE[patch].name, peak_all, sqrt(sum2 / n),
         nan ? "NaN! " : "", peak_all >= 0.999 ? "CLIPPING " : "", sqrt(sum2 / n) < 1e-4 ? "SILENT" : "");
  if (wav) write_wav(wav, x, n);
  free(x);
  return last_peak;
}

int main(int argc, char **argv) {
  if (argc >= 2 && strcmp(argv[1], "all") == 0) {
    for (int p = 0; p < PATCH_COUNT; p++) run(p, 20, NULL, 0, 0);
    return 0;
  }
  if (argc < 3) { fprintf(stderr, "usage: render PATCH(1-12) SECONDS [out.wav [stress]] | render all\n"); return 1; }
  int p = atoi(argv[1]) - 1;
  if (p < 0 || p >= PATCH_COUNT) { fprintf(stderr, "patch 1-%d\n", PATCH_COUNT); return 1; }
  run(p, atof(argv[2]), argc > 3 ? argv[3] : NULL, argc > 4, 1);
  return 0;
}
