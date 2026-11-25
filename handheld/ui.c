/* The small-screen UI, part 1: drawing primitives, the frame around every screen (tabs, hints, meter), input
   handling, the menu, and the PAR / REG / PAT / PLAY screens. The code editor (BLK) is in ui_edit.c.
   A port of dev/brick_body.js; the layouts and the behaviour of the buttons follow it. */
#include "ui_priv.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- palette and drawing */


int ui_clamp(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

void ui_px(Ui *ui, int x, int y, int c) { if (x >= 0 && x < UI_W && y >= 0 && y < UI_H) ui->fb[y * UI_W + x] = (unsigned char)c; }

void ui_fill(Ui *ui, int x, int y, int w, int h, int c) {
  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) ui_px(ui, x + i, y + j, c);
}

void ui_rect(Ui *ui, int x, int y, int w, int h, int c) {
  ui_fill(ui, x, y, w, 1, c); ui_fill(ui, x, y + h - 1, w, 1, c);
  ui_fill(ui, x, y, 1, h, c); ui_fill(ui, x + w - 1, y, 1, h, c);
}

/* Each character is a 5x8 bitmap: 8 rows, bit 4 of each row is the leftmost pixel. Characters outside ASCII 32..126
   draw as '?'. */
void ui_text(Ui *ui, int x, int y, const char *s, int c) {
  for (int k = 0; s[k]; k++) {
    int ch = (unsigned char)s[k];
    const unsigned char *g = FONT5[ch >= 32 && ch < 127 ? ch - 32 : 31];
    for (int r = 0; r < 8; r++) for (int b = 0; b < 5; b++) if ((g[r] >> (4 - b)) & 1) ui_px(ui, x + k * 5 + b, y + r, c);
  }
}

void ui_ctext(Ui *ui, int x, int w, int y, const char *s, int c) { ui_text(ui, x + (w - (int)strlen(s) * 5) / 2, y, s, c); }

void ui_bar(Ui *ui, int x, int y, int w, int h, double v, int c, int bg) {
  ui_fill(ui, x, y, w, h, bg);
  v = v < 0 ? 0 : v > 1 ? 1 : v;
  ui_fill(ui, x, y, (int)floor(v * w + 0.5), h, c);
}

int ui_held(const Ui *ui, Button b) { return (ui->held >> b) & 1; }

void ui_say(Ui *ui, const char *msg) { snprintf(ui->msg, sizeof ui->msg, "%s", msg); ui->msg_until = ui->now_ms + 1600; }

/* Left-align s in n characters (cut or padded with spaces), or right-align. The small screens use fixed columns. */
static void pad_right(char *out, const char *s, int n) { snprintf(out, (size_t)n + 1, "%-*.*s", n, n, s); }
static void pad_left(char *out, const char *s, int n) {
  int len = (int)strlen(s);
  if (len >= n) snprintf(out, (size_t)n + 1, "%s", s + len - n); else snprintf(out, (size_t)n + 1, "%*s", n, s);
}

/* ---------------------------------------------------------------- the frame around every screen */

static const char *SCREEN_NAMES[SCR_COUNT] = { "BLK", "PAR", "REG", "PAT", "PLAY" };

static void top_bar(Ui *ui) {
  ui_fill(ui, 0, 0, UI_W, 12, PNL); ui_fill(ui, 0, 11, UI_W, 1, LINE);
  int tw = UI_W / SCR_COUNT;
  for (int i = 0; i < SCR_COUNT; i++) {
    int x = i * tw, on = i == ui->screen;
    if (on) ui_fill(ui, x, 0, tw, 11, AMBD);
    ui_ctext(ui, x, tw, 2, SCREEN_NAMES[i], on ? AMB : DIM);
    if (i == SCR_BLK) for (int b = 0; b < NUM_BLOCKS; b++) if (ui->synth->vm.blocks[b].halted) { ui_fill(ui, x + tw - 4, 2, 2, 2, RED); break; }
  }
}

void ui_bottom_bar(Ui *ui, const char *hint) {
  ui_fill(ui, 0, 180, UI_W, 12, PNL); ui_fill(ui, 0, 180, UI_W, 1, LINE);
  if (ui->msg_until > ui->now_ms) ui_text(ui, 3, 183, ui->msg, AMB); else ui_text(ui, 3, 183, hint, DIM);
  ui_bar(ui, 228, 184, 24, 4, ui->peak * 1.6, ui->peak > 0.9 ? RED : GRN, SEL);   /* output level meter */
  ui_fill(ui, 222, 184, 3, 4, ui->audio_ok ? GRN : RED);
}

/* A thin scrollbar down the right edge of a list: the thumb is the visible part, its position the scroll. */
static void scrollbar(Ui *ui, int top, int vis, int n, int y0, int h) {
  ui_fill(ui, 252, y0, 3, h, SEL);
  if (n <= vis) return;
  int th = h * vis / n; if (th < 6) th = 6;
  int ty = y0 + (int)floor((double)(h - th) * top / (n - vis) + 0.5);
  ui_fill(ui, 252, ty, 3, th, AMB);
}

static void help_line(Ui *ui, const char *t) {
  ui_fill(ui, 0, 167, UI_W, 13, BG); ui_fill(ui, 0, 167, UI_W, 1, LINE);
  char line[50]; snprintf(line, sizeof line, "%.49s", t);
  ui_text(ui, 4, 171, line, FG);
}

/* ---------------------------------------------------------------- PAR: the parameters */

static void draw_par(Ui *ui) {
  FezSynth *s = ui->synth;
  int n = ui->npars, vis = 14, top = ui_clamp(ui->par_sel - 7, 0, n - vis);
  ui_text(ui, 4, 15, "PARAMETERS", AMB); ui_text(ui, 150, 15, "base", DIM); ui_text(ui, 206, 15, "now", DIM);
  for (int r = 0; r < vis; r++) {
    int i = top + r, a = ui->pars[i], y = 26 + r * 10, sel = i == ui->par_sel;
    char buf[24], cell[24];
    ui_fill(ui, 2, y, 248, 10, sel ? SEL : BG);
    pad_right(cell, regview_param_name(s, a), 8);
    ui_text(ui, 6, y + 1, cell, sel ? AMB : FG);
    int base = regview_pget(ui->synth, a), eff = s->eff[a];
    ui_bar(ui, 60, y + 2, 86, 6, base / 32767.0, sel ? AMB : DIM, sel ? BG : SEL);
    if (abs(eff - base) > 200) ui_fill(ui, 60 + (int)floor(eff / 32767.0 * 85 + 0.5), y + 1, 2, 8, CYN);   /* where modulation puts it now */
    regview_fmt_param(buf, sizeof buf, a, base); pad_left(cell, buf, 7);
    ui_text(ui, 150, y + 1, cell, sel ? AMB : FG);
    if (eff != base) { regview_fmt_param(buf, sizeof buf, a, eff); pad_left(cell, buf, 7); ui_text(ui, 206, y + 1, cell, CYN); }
  }
  scrollbar(ui, top, vis, n, 26, vis * 10);
  help_line(ui, regview_help(regview_param_name(s, ui->pars[ui->par_sel])));
  ui_bottom_bar(ui, "A/B change  Y coarse");
}

static void key_par(Ui *ui, Button b) {
  int n = ui->npars, a = ui->pars[ui->par_sel], coarse = ui_held(ui, BTN_Y);
  if (b == BTN_UP) ui->par_sel = (ui->par_sel + n - 1) % n;
  else if (b == BTN_DOWN) ui->par_sel = (ui->par_sel + 1) % n;
  else if (b == BTN_LEFT || b == BTN_B) regview_pstep(ui->synth, a, -1, coarse);
  else if (b == BTN_RIGHT || b == BTN_A) regview_pstep(ui->synth, a, 1, coarse);
}

/* ---------------------------------------------------------------- REG: every register */

static void draw_reg(Ui *ui) {
  FezSynth *s = ui->synth;
  int n = ui->nregs, vis = 14, top = ui_clamp(ui->reg_sel - 7, 0, n - vis);
  ui_text(ui, 4, 15, "REGISTERS", AMB); ui_text(ui, 150, 15, "raw", DIM); ui_text(ui, 196, 15, "eff", DIM);
  for (int r = 0; r < vis; r++) {
    int i = top + r, y = 26 + r * 10, sel = i == ui->reg_sel;
    const RegEntry *e = &ui->regs[i];
    char buf[24], cell[24];
    ui_fill(ui, 2, y, 248, 10, sel ? SEL : BG);
    pad_right(cell, e->name, 8);
    ui_text(ui, 6, y + 1, cell, sel ? AMB : CYN);
    if (e->wo) ui_text(ui, 150, y + 1, "    -", FG);
    else { snprintf(buf, sizeof buf, "%d", regview_read(ui->synth, e->addr)); pad_left(cell, buf, 6); ui_text(ui, 150, y + 1, cell, FG); }
    if (s->param_index[e->addr] >= 0 && e->addr != REG_GATE && !e->wo && s->eff[e->addr] != s->mem[e->addr]) {
      snprintf(buf, sizeof buf, "%d", s->eff[e->addr]); pad_left(cell, buf, 6); ui_text(ui, 196, y + 1, cell, GRN);   /* modulated */
    }
    ui_text(ui, 238, y + 1, e->ro ? "ro" : e->wo ? "wo" : "", DIM);
    if (!e->wo && !e->is_signed && e->addr < REG_P0) ui_bar(ui, 60, y + 3, 80, 4, regview_read(ui->synth, e->addr) / 32767.0, sel ? AMB : DIM, SEL);
  }
  scrollbar(ui, top, vis, n, 26, vis * 10);
  help_line(ui, regview_help(ui->regs[ui->reg_sel].name));
  ui_bottom_bar(ui, "Up/Down pick  A/B +-1  Y x256  X zero");
}

static void key_reg(Ui *ui, Button b) {
  int n = ui->nregs, big = ui_held(ui, BTN_Y);
  const RegEntry *e = &ui->regs[ui->reg_sel];
  if (b == BTN_UP) ui->reg_sel = (ui->reg_sel + n - 1) % n;
  else if (b == BTN_DOWN) ui->reg_sel = (ui->reg_sel + 1) % n;
  else if (b == BTN_LEFT) ui->reg_sel = ui_clamp(ui->reg_sel - 14, 0, n - 1);
  else if (b == BTN_RIGHT) ui->reg_sel = ui_clamp(ui->reg_sel + 14, 0, n - 1);
  else if (e->ro) return;
  else if (e->wo) { if (b == BTN_A) regview_write(ui->synth, e->addr, 1); }                  /* write-only: A pokes a 1 (fires it) */
  else if (b == BTN_X) regview_write(ui->synth, e->addr, 0);
  else if (b == BTN_A || b == BTN_B) regview_write(ui->synth, e->addr, ui_clamp(regview_read(ui->synth, e->addr) + (b == BTN_A ? 1 : -1) * (big ? 256 : 1), -32768, 32767));
}

/* ---------------------------------------------------------------- PAT: the example patches */

/* Break text into lines of at most w characters at spaces. Returns the number of lines. */
static int wrap_text(const char *s, int w, char lines[][48], int max_lines) {
  int n = 0, len = 0;
  char word[48];
  lines[0][0] = '\0';
  while (*s) {
    while (*s == ' ') s++;
    int k = 0;
    while (*s && *s != ' ' && k < 47) word[k++] = *s++;
    word[k] = '\0';
    if (!k) break;
    if (len > 0 && len + 1 + k > w) { if (n + 1 >= max_lines) return n + 1; n++; lines[n][0] = '\0'; len = 0; }
    if (len > 0) { lines[n][len++] = ' '; }
    memcpy(lines[n] + len, word, (size_t)k + 1);
    len += k;
  }
  return len > 0 || n > 0 ? n + 1 : 0;
}

static void draw_pat(Ui *ui) {
  ui_text(ui, 4, 15, "PATCHES", AMB);
  for (int i = 0; i < PATCH_COUNT; i++) {
    int y = 26 + i * 12, sel = i == ui->pat_sel;
    char line[40]; snprintf(line, sizeof line, "%d %s", i + 1, PATCH_TABLE[i].name);
    ui_fill(ui, 2, y, 100, 12, sel ? AMBD : BG);
    ui_text(ui, 6, y + 2, line, sel ? AMB : FG);
  }
  const PatchDef *p = &PATCH_TABLE[ui->pat_sel];
  char lines[16][48], listen[300];
  ui_fill(ui, 106, 14, 148, 164, PNL);
  ui_text(ui, 110, 18, p->name, AMB);
  int dl = wrap_text(p->desc, 28, lines, 12);
  for (int i = 0; i < dl && i < 12; i++) ui_text(ui, 110, 28 + i * 8, lines[i], FG);
  snprintf(listen, sizeof listen, "Listen: %s", p->listen);
  int ll = wrap_text(listen, 28, lines, 8);
  for (int i = 0; i < ll && i < 8; i++) ui_text(ui, 110, 36 + dl * 8 + i * 8, lines[i], GRN);
  ui_bottom_bar(ui, "A load");
}

static void key_pat(Ui *ui, Button b) {
  if (b == BTN_UP) ui->pat_sel = (ui->pat_sel + PATCH_COUNT - 1) % PATCH_COUNT;
  else if (b == BTN_DOWN) ui->pat_sel = (ui->pat_sel + 1) % PATCH_COUNT;
  else if (b == BTN_A) {
    fez_load_patch(ui->synth, ui->pat_sel);
    ui->patch = ui->pat_sel; ui->pulsers_on = 1;
    edit_open_block(ui, 0);
    ui->screen = SCR_BLK;
    char m[40]; snprintf(m, sizeof m, "LOADED %s", PATCH_TABLE[ui->pat_sel].name); ui_say(ui, m);
  }
  else if (b == BTN_X) ui_say(ui, "SAVING USER PATCHES: NOT YET");
}

/* ---------------------------------------------------------------- PLAY: notes from the computer keyboard */

void ui_play_note(Ui *ui, int semitone) {
  int pitch = (12 * (ui->octave + 1) + semitone) * 256;               /* 8.8 semitones: MIDI note x 256 */
  ui->last_pitch = pitch;
  fez_reg_write(ui->synth, REG_PITCH, pitch);                                /* PITCH */
  fez_reg_write(ui->synth, REG_GATE, 1);                                    /* GATE: fire the envelope */
}

void ui_octave(Ui *ui, int delta) {
  ui->octave = ui_clamp(ui->octave + delta, 0, 8);
  char m[40]; snprintf(m, sizeof m, "OCTAVE %d", ui->octave); ui_say(ui, m);
}

void ui_toggle_pulsers(Ui *ui) {
  ui->pulsers_on = !ui->pulsers_on;
  for (int i = 0; i < NUM_PULSERS; i++) ui->synth->pulsers[i].on = ui->pulsers_on && PATCH_TABLE[ui->patch].pulsers[i].on;
  ui_say(ui, ui->pulsers_on ? "PULSERS ON" : "PULSERS OFF");
}

static void draw_play(Ui *ui) {
  ui_text(ui, 4, 15, "PLAY", AMB);
  char buf[48], note[16];
  regview_fmt_note(note, sizeof note, ui->last_pitch);
  ui_text(ui, 4, 26, "Keys play notes on this screen.", FG);
  snprintf(buf, sizeof buf, "Octave %d    last note %s", ui->octave, ui->last_pitch ? note : "-");
  ui_text(ui, 4, 38, buf, CYN);

  /* A picture of the keys. White keys are semitones 0,2,4,5,7,9,11,12,14,16 on A S D F G H J K L ;
     black keys sit between them on W E T Y U O P. */
  static const int white[10] = { 0, 2, 4, 5, 7, 9, 11, 12, 14, 16 };
  static const char *wlabel[10] = { "A", "S", "D", "F", "G", "H", "J", "K", "L", ";" };
  static const int black[7] = { 1, 3, 6, 8, 10, 13, 15 }, bafter[7] = { 0, 1, 3, 4, 5, 7, 8 };
  static const char *blabel[7] = { "W", "E", "T", "Y", "U", "O", "P" };
  int last = ui->last_pitch ? ui->last_pitch / 256 - 12 * (ui->octave + 1) : -99;
  int x0 = 58, y0 = 64, kw = 14;
  for (int i = 0; i < 10; i++) {
    ui_fill(ui, x0 + i * kw, y0, kw - 1, 44, white[i] == last ? AMB : FG);
    ui_ctext(ui, x0 + i * kw, kw - 1, y0 + 34, wlabel[i], BG);
  }
  for (int i = 0; i < 7; i++) {
    int x = x0 + bafter[i] * kw + kw - 4;
    ui_fill(ui, x, y0, 9, 26, black[i] == last ? AMB : BG);
    ui_rect(ui, x, y0, 9, 26, DIM);
    ui_ctext(ui, x, 9, y0 + 16, blabel[i], black[i] == last ? BG : FG);
  }
  ui_text(ui, 4, 122, "Left/Right  octave", DIM);
  ui_text(ui, 4, 132, "Enter  fire the envelope", DIM);
  ui_text(ui, 4, 142, "Tab  pulsers on/off (patch plays itself)", DIM);
  ui_text(ui, 4, 152, "[ and ]  change screen", DIM);
  ui_bottom_bar(ui, ui->pulsers_on ? "pulsers ON  Tab to silence the patch" : "pulsers OFF  Tab to restore");
}

static void key_play(Ui *ui, Button b) {
  if (b == BTN_LEFT) ui_octave(ui, -1);
  else if (b == BTN_RIGHT) ui_octave(ui, 1);
  else if (b == BTN_A) fez_reg_write(ui->synth, REG_GATE, 1);
  else if (b == BTN_X) ui_toggle_pulsers(ui);
}

/* ---------------------------------------------------------------- the menu */

static const char *MENU[] = { "Fire gate", "Run block", "Undo", "Copy block", "Paste block", "Clear block", "Clear halts", "Close" };
#define MENU_N ((int)(sizeof MENU / sizeof *MENU))

static void draw_menu(Ui *ui) {
  ui_fill(ui, 60, 30, 136, 124, PNL); ui_rect(ui, 60, 30, 136, 124, AMB); ui_text(ui, 66, 34, "MENU", AMB);
  for (int i = 0; i < MENU_N; i++) {
    int y = 46 + i * 10, sel = i == ui->menu;
    if (sel) ui_fill(ui, 62, y - 1, 132, 10, AMBD);
    ui_text(ui, 68, y, MENU[i], sel ? AMB : FG);
  }
}

static void key_menu(Ui *ui, Button b) {
  if (b == BTN_UP) ui->menu = (ui->menu + MENU_N - 1) % MENU_N;
  else if (b == BTN_DOWN) ui->menu = (ui->menu + 1) % MENU_N;
  else if (b == BTN_B || b == BTN_MENU) ui->menu = -1;
  else if (b == BTN_A) {
    int m = ui->menu; ui->menu = -1;
    if (m == 0) fez_reg_write(ui->synth, REG_GATE, 1);
    else if (m == 1) edit_run(ui);
    else if (m == 2) edit_undo(ui);
    else if (m == 3) edit_copy(ui);
    else if (m == 4) edit_paste(ui);
    else if (m == 5) edit_clear(ui);
    else if (m == 6) for (int i = 0; i < NUM_BLOCKS; i++) { ui->synth->vm.blocks[i].halted = 0; ui->synth->vm.blocks[i].fault = 0; }
  }
}

/* ---------------------------------------------------------------- input */

static void press(Ui *ui, Button b) {
  ui->last_key_ms = ui->now_ms;
  if (ui->menu >= 0) { key_menu(ui, b); return; }
  if (b == BTN_MENU) { ui->menu = 0; return; }
  if (b == BTN_START) { fez_reg_write(ui->synth, REG_GATE, 1); return; }
  if (b == BTN_VOLU || b == BTN_VOLD) {
    ui->synth->level_pot = ui_clamp(ui->synth->level_pot + (b == BTN_VOLU ? 1 : -1) * 1500, 0, 32767);
    char m[40], v[16]; regview_fmt_pct(v, sizeof v, ui->synth->level_pot); snprintf(m, sizeof m, "VOLUME %s", v); ui_say(ui, m);
    return;
  }
  if (b == BTN_SELECT) return;                                     /* (the macro faders are not ported yet) */
  if (b == BTN_L1 || b == BTN_R1) { ui->screen = (ui->screen + (b == BTN_R1 ? 1 : SCR_COUNT - 1)) % SCR_COUNT; return; }
  switch (ui->screen) {
    case SCR_BLK: edit_key(ui, b); break;
    case SCR_PAR: key_par(ui, b); break;
    case SCR_REG: key_reg(ui, b); break;
    case SCR_PAT: key_pat(ui, b); break;
    case SCR_PLAY: key_play(ui, b); break;
  }
}

/* Directions repeat when held. So do A and B where they change a value (not in the editor, where A inserts a word,
   and not in the patch list, where A loads). */
static int repeatable(const Ui *ui, Button b) {
  if (b == BTN_UP || b == BTN_DOWN || b == BTN_LEFT || b == BTN_RIGHT) return 1;
  if ((b == BTN_A || b == BTN_B) && ui->screen != SCR_PAT && ui->screen != SCR_PLAY)
    return ui->screen != SCR_BLK || ui_held(ui, BTN_SELECT) || b == BTN_B;
  return 0;
}

void ui_button(Ui *ui, Button b, int down) {
  if (!down) { ui->held &= ~(1u << b); return; }
  if (ui_held(ui, b)) return;                                      /* ignore the operating system's own repeat */
  ui->held |= 1u << b;
  press(ui, b);
  ui->repeat_at[b] = ui->now_ms + 380;
}

void ui_tick(Ui *ui, double dt_ms) {
  ui->now_ms += dt_ms;
  for (int b = 0; b < BTN_COUNT; b++)
    if (ui_held(ui, (Button)b) && repeatable(ui, (Button)b) && ui->now_ms >= ui->repeat_at[b]) { press(ui, (Button)b); ui->repeat_at[b] = ui->now_ms + 70; }
}

/* ---------------------------------------------------------------- set-up and drawing */

void ui_init(Ui *ui, FezSynth *synth) {
  memset(ui, 0, sizeof *ui);
  ui->synth = synth;
  ui->menu = -1; ui->octave = 4; ui->pulsers_on = 1; ui->focus_code = 0;

  ui->nregs = regview_list(ui->regs, MAX_LIST);
  ui->npars = regview_params(ui->pars, MAX_LIST);

  edit_build_palettes(ui);
  edit_open_block(ui, 0);
  ui->focus_code = 0;
}

void ui_draw(Ui *ui) {
  memset(ui->fb, BG, sizeof ui->fb);
  top_bar(ui);
  switch (ui->screen) {
    case SCR_BLK: edit_draw(ui); break;
    case SCR_PAR: draw_par(ui); break;
    case SCR_REG: draw_reg(ui); break;
    case SCR_PAT: draw_pat(ui); break;
    case SCR_PLAY: draw_play(ui); break;
  }
  if (ui->menu >= 0) draw_menu(ui);
}
