/* Draws UI screens to PPM files without a window, for checking layouts: ./uishot PATCH OUTPREFIX
   Writes OUTPREFIX_blk.ppm, _par, _reg, _pat, _play (each 256x192, scaled 3x). */
#include <stdio.h>
#include <stdlib.h>
#include "ui_priv.h"

static FezSynth synth;
static Ui ui;

static void dump(const char *prefix, const char *name) {
  char path[256]; snprintf(path, sizeof path, "%s_%s.ppm", prefix, name);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", UI_W * 3, UI_H * 3);
  for (int y = 0; y < UI_H * 3; y++) for (int x = 0; x < UI_W * 3; x++) fwrite(UI_PAL[ui.fb[(y / 3) * UI_W + x / 3]], 1, 3, f);
  fclose(f);
}

int main(int argc, char **argv) {
  int patch = argc > 1 ? atoi(argv[1]) : 1;
  const char *prefix = argc > 2 ? argv[2] : "shot";
  fez_init(&synth, 1);
  fez_load_patch(&synth, patch - 1);
  ui_init(&ui, &synth);
  static float out[4800];
  for (int i = 0; i < 100; i++) fez_render(&synth, out, 480);     /* let the patch run for a second so values move */
  ui.now_ms = 1000;
  static const char *names[] = { "blk", "par", "reg", "pat", "play" };
  for (int s = 0; s < SCR_COUNT; s++) { ui.screen = s; ui_draw(&ui); dump(prefix, names[s]); }
  return 0;
}
