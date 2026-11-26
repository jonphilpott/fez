/* Offline renderer and sound checker (no SDL, no sound card).
   PATCH is a built-in example, 1..PATCH_COUNT (see tables.c), or a saved patch file (name ending in .fez).
     ./render PATCH SECONDS [out.wav]        render the patch, print the level of each second, optionally save a WAV
     ./render PATCH SECONDS out.wav stress   same, with every feedback control at 100 percent and the filter resonance
                                             at maximum: the level must stay bounded
     ./render all                            quick check of every built-in patch: peak, RMS, NaN, silence
     ./render all FILE.fez ...               the same check of your own patches (give it every .fez file in ~/FEZ/patches)
   The tool exists because the sound cannot be unit-tested (there is no reference to compare it with). It catches the
   failures a machine can see: silence, clipping, NaN and runaway feedback. Whether it SOUNDS good is for ears. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fez.h"
#include "patchfile.h"

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

/* Load a patch: a built-in number (1-based, as typed) or a .fez file. Puts its name in name; returns 0, or -1 if it cannot. */
static int load(const char *arg, char *name, size_t cap) {
  fez_init(&synth, 12345);
  size_t len = strlen(arg);
  if (len < 4 || strcmp(arg + len - 4, ".fez") != 0) {                           /* a built-in example */
    int p = atoi(arg) - 1;
    if (p < 0 || p >= PATCH_COUNT) { fprintf(stderr, "patch 1-%d, or a .fez file\n", PATCH_COUNT); return -1; }
    fez_load_patch(&synth, p);
    snprintf(name, cap, "%s", PATCH_TABLE[p].name);
    return 0;
  }
  static char text[65536];                                                       /* a patch file is a few kilobytes */
  FILE *f = fopen(arg, "rb");
  if (!f) { perror(arg); return -1; }
  size_t n = fread(text, 1, sizeof text - 1, f);
  fclose(f);
  text[n] = '\0';
  PatchInfo info; char err[160];
  int bad = patchfile_read(&synth, text, &info, err, sizeof err);
  if (bad < 0) { fprintf(stderr, "%s: not a FEZ patch file\n", arg); return -1; }
  if (bad > 0) fprintf(stderr, "%s: %d line(s) skipped, first: %s\n", arg, bad, err);
  const char *base = strrchr(arg, '/');                                           /* the file's name, without folder and .fez */
  base = base ? base + 1 : arg;
  snprintf(name, cap, "%.*s", (int)(strlen(base) - 4), base);
  return 0;
}

/* Render the loaded patch and report. Returns the peak of the last second (to judge runaway). */
static double run(const char *name, double seconds, const char *wav, int stress, int verbose) {
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
    for (int i = a; i < b; i++) { double v = fabs(x[i]); if (v > pk) pk = v; s2 += (double)x[i] * x[i]; }
    if (verbose) printf("  second %2d: peak %.3f  rms %.4f\n", sec + 1, pk, sqrt(s2 / (b - a)));
    if (pk > peak_all) peak_all = pk;
    sum2 += s2; last_peak = pk;
  }
  nan = (int)synth.audio.nan_count;                      /* the core replaces NaN with silence, but counts it */
  printf("%-14s peak %.3f  rms %.4f  %s%s%s\n", name, peak_all, sqrt(sum2 / n),
         nan ? "NaN! " : "", peak_all >= 0.999 ? "CLIPPING " : "", sqrt(sum2 / n) < 1e-4 ? "SILENT" : "");
  if (wav) write_wav(wav, x, n);
  free(x);
  return last_peak;
}

int main(int argc, char **argv) {
  char name[64], num[8];
  if (argc >= 2 && strcmp(argv[1], "all") == 0) {
    if (argc == 2)                                                                 /* every built-in example */
      for (int p = 1; p <= PATCH_COUNT; p++) { snprintf(num, sizeof num, "%d", p); if (load(num, name, sizeof name) == 0) run(name, 20, NULL, 0, 0); }
    for (int i = 2; i < argc; i++) if (load(argv[i], name, sizeof name) == 0) run(name, 20, NULL, 0, 0);   /* the files named */
    return 0;
  }
  if (argc < 3) { fprintf(stderr, "usage: render PATCH SECONDS [out.wav [stress]] | render all [FILE.fez ...]\n  PATCH: 1-%d or a .fez file\n", PATCH_COUNT); return 1; }
  if (load(argv[1], name, sizeof name) != 0) return 1;
  run(name, atof(argv[2]), argc > 3 ? argv[3] : NULL, argc > 4, 1);
  return 0;
}
