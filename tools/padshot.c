/* Draws every handheld (Brick) screen to a PPM file without a window: ./padshot PATCH OUTPREFIX [W H]
   One picture per screen, then the menu, the palette, the patch-name picker and the help page. Buttons are pressed
   through the Pad, exactly as the real device's would be. For checking the 256x192 layouts. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pad.h"

static int W = 256, H = 192;          /* the picture: the Brick's, unless a size is given */

static FezSynth synth;
static Desk desk;
static Pad pad;
static float ring[2 * SCOPE_RING];
static unsigned head;
static uint32_t px[1024 * 768];

static void dump(const char *prefix, const char *name) {
  desk_tick(&desk, 16); pad_tick(&pad);
  desk_draw(&desk); pad_draw(&pad);
  char path[256]; snprintf(path, sizeof path, "%s_%s.ppm", prefix, name);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int i = 0; i < W * H; i++) { unsigned char c[3] = { (unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i] }; fwrite(c, 1, 3, f); }
  fclose(f);
}

static void tap(Button b) { pad_button(&pad, b, 1); pad_button(&pad, b, 0); desk.now_ms += 1000; }   /* the clock moves on so a repeat never fires */

int main(int argc, char **argv) {
  int patch = argc > 1 ? atoi(argv[1]) : 1;
  const char *prefix = argc > 2 ? argv[2] : "pad";
  if (argc > 4) { W = atoi(argv[3]); H = atoi(argv[4]); }                       /* another screen's picture, e.g. 360 240 */
  if (W < 256 || H < 192 || W * H > 1024 * 768) { fprintf(stderr, "size: at least 256x192, at most 1024x768 pixels\n"); return 1; }
  fez_init(&synth, 1);
  desk_init(&desk, &synth);
  desk_patches_loaded_example(&desk, patch - 1); desk_reload_editor(&desk);
  desk.pat.sub = SUB_EXAMPLES; desk.pat.sel[SUB_EXAMPLES] = patch - 1;
  desk.scope = ring; desk.scope_head = &head;
  snprintf(desk.pat.dir, sizeof desk.pat.dir, "%s/", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");   /* so the Save picker opens */
  desk_set_canvas(&desk, px, W, H);
  desk_set_handheld(&desk);
  pad_init(&pad, &desk);
  fez_reg_write(&synth, REG_SPRWID, 32767);              /* a wide spring, so the two lanes of the scope differ */
  static float out[2 * 480];
  for (int i = 0; i < 200; i++) {
    fez_render_stereo(&synth, out, 480);
    for (int k = 0; k < 480; k++) { unsigned slot = (head + (unsigned)k) & (SCOPE_RING - 1); ring[2 * slot] = out[2 * k]; ring[2 * slot + 1] = out[2 * k + 1]; }
    head += 480; desk.peak = 0.3f;
  }
  desk.cpu = 0.35f; desk.cpu_peak = 0.5f; desk.now_ms = 1000;
  static const char *names[TAB_COUNT] = { "editor", "patches", "ctrl", "devices", "buffer", "pulsers", "seq", "matrix", "keys", "sticks", "scope" };
  desk.layouts[LAYOUT_HAND].tab[0] = TAB_EDITOR;
  for (int i = 0; i < TAB_COUNT - 1; i++) {              /* R1 walks the screens, as on the device (there is no KEYS screen) */
    int t = desk.layouts[LAYOUT_HAND].tab[0];
    if (t == TAB_STICKS) { pad_stick(&pad, 0, 26000); pad_stick(&pad, 1, -26000); }   /* the left stick pushed up and right: degree 1 lit */
    char n[32]; snprintf(n, sizeof n, "%02d_%s", t, names[t]);
    dump(prefix, n);
    if (t == TAB_STICKS) { pad_stick(&pad, 0, 0); pad_stick(&pad, 1, 0); }
    tap(BTN_R1);
  }
  desk.layouts[LAYOUT_HAND].tab[0] = TAB_DEVICES;                  /* every page of the Devices tab (R2 is the next page) */
  for (int p = 1; p < DEV_PAGES; p++) {
    static const char *const pages[] = { "delay", "cellular", "lorenz", "quantiser", "turing" };
    tap(BTN_R2);
    if (p == 1) for (int i = 0; i < 14; i++) { tap(BTN_X); fez_render_stereo(&synth, out, 480); desk_tick(&desk, 16); }   /* step the automaton so the picture has a history */
    char n[32]; snprintf(n, sizeof n, "03%c_%s", 'a' + p, pages[p]);
    dump(prefix, n);
  }
  desk.layouts[LAYOUT_HAND].tab[0] = TAB_EDITOR;
  tap(BTN_A); dump(prefix, "10_palette");                  /* A in the editor opens the palette */
  tap(BTN_R2); tap(BTN_R2); tap(BTN_R2); tap(BTN_R2); tap(BTN_R2); tap(BTN_R2); dump(prefix, "11_palette_notes");
  tap(BTN_R2); tap(BTN_R2); dump(prefix, "11b_palette_regs");
  for (int i = 0; i < 6; i++) tap(BTN_DOWN);
  dump(prefix, "11c_palette_regs_down");
  tap(BTN_B);
  tap(BTN_MENU); for (int i = 0; i < 8; i++) tap(BTN_DOWN); tap(BTN_A);   /* the editor menu's Reference: the palette with help */
  tap(BTN_R2); tap(BTN_R2); for (int i = 0; i < 13; i++) tap(BTN_RIGHT); dump(prefix, "11d_palette_reference");   /* the picture in the manual (it lands on STACK, DROP) */
  tap(BTN_B);
  tap(BTN_MENU); dump(prefix, "12_menu");                 /* opens on the editor's actions */
  synth.mem[REG_BP0] = 0;                                  /* so the two ticks below are the only ones */
  for (int i = 0; i < 9; i++) tap(BTN_DOWN); tap(BTN_A); tap(BTN_DOWN); tap(BTN_DOWN); tap(BTN_A); dump(prefix, "12b_actions");   /* P0 and P2 ticked */
  tap(BTN_RIGHT); dump(prefix, "12c_menu_main");           /* the MENU tab */
  tap(BTN_A); dump(prefix, "13_goto");
  tap(BTN_B); tap(BTN_B);
  desk.layouts[LAYOUT_HAND].tab[0] = TAB_PATCHES; tap(BTN_X); dump(prefix, "14_name");
  tap(BTN_Y);
  tap(BTN_MENU); tap(BTN_RIGHT); tap(BTN_DOWN); tap(BTN_A); dump(prefix, "16_confirm");   /* MENU tab, second row: New blank patch */
  tap(BTN_B);
  tap(BTN_MENU); tap(BTN_RIGHT); for (int i = 0; i < 4; i++) tap(BTN_DOWN); tap(BTN_A); dump(prefix, "15_help");          /* MENU tab, fifth row: Button help */
  printf("done\n");
  return 0;
}
