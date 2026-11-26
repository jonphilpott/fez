/* Draws the desktop UI to PPM files without a window: ./deskshot PATCH WIDTH HEIGHT OUTPREFIX
   One picture per layout, after sending a few key presses. For checking layouts at a given screen size. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desk.h"

static FezSynth synth;
static Desk desk;
static float ring[2 * SCOPE_RING];
static unsigned head;

static void dump(const char *prefix, const char *name, uint32_t *px, int w, int h) {
  char path[256]; snprintf(path, sizeof path, "%s_%s.ppm", prefix, name);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int i = 0; i < w * h; i++) { unsigned char c[3] = { (unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i] }; fwrite(c, 1, 3, f); }
  fclose(f);
}

int main(int argc, char **argv) {
  int patch = argc > 1 ? atoi(argv[1]) : 1, w = argc > 2 ? atoi(argv[2]) : 1024, h = argc > 3 ? atoi(argv[3]) : 600;
  const char *prefix = argc > 4 ? argv[4] : "desk";
  fez_init(&synth, 1);
  fez_load_patch(&synth, patch - 1);
  desk_init(&desk, &synth);
  desk_patches_loaded_example(&desk, patch - 1); desk_reload_editor(&desk); desk.pat.sub = SUB_EXAMPLES; desk.pat.sel[SUB_EXAMPLES] = patch - 1;
  desk.scope = ring; desk.scope_head = &head;
  uint32_t *px = malloc((size_t)w * h * sizeof *px);
  desk_set_canvas(&desk, px, w, h);
  if (argc > 5) desk.layouts[0].tab[0] = atoi(argv[5]);        /* optional: which tab the first tile shows */

  static float out[2 * 480];
  for (int i = 0; i < 200; i++) {                       /* two seconds of audio, copied into the scope ring as main.c does */
    fez_render_stereo(&synth, out, 480);
    for (int k = 0; k < 480; k++) { unsigned slot = (head + (unsigned)k) & (SCOPE_RING - 1); ring[2 * slot] = out[2 * k]; ring[2 * slot + 1] = out[2 * k + 1]; }
    head += 480;
    desk.peak = 0.3f;
  }
  desk.now_ms = 1000;
  /* FEZ_SHOT_CLEAN=1 leaves the editor as the patch has it (for the manual); otherwise a half-typed word shows the completion popup */
  if (desk.layouts[0].tab[0] == TAB_EDITOR && !getenv("FEZ_SHOT_CLEAN")) { desk.ed.cursor = (int)strlen(desk.ed.text[0]); desk_text(&desk, '\n'); desk_text(&desk, 'C'); desk_text(&desk, 'U'); }   /* a half-typed word, so the completion popup shows */
  desk.ed.last_key_ms = 999;
  printf("scale %d, layout tiles: ", desk.scale);
  for (int l = 0; l < 3; l++) {
    desk_draw(&desk);
    static const char *names[] = { "layout1", "layout2", "layout3" };
    dump(prefix, names[l], px, w, h);
    desk_key(&desk, KEY_TAB, MOD_CTRL);                 /* next layout */
  }
  printf("done\n");
  return 0;
}
