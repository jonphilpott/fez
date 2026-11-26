/* The BUFFER tab: the tape and its eight voices.

   The buffer device is a circular tape of up to 8 seconds. One write head records into it (what it records, and how
   much of the voices' own output is fed back, are settings). Eight read voices play it back: a continuous voice is a
   delay tap or a free-running loop; a voice with a length is a grain, a short windowed snippet played once or looped.

   Two pages, switched with [ and ]:
     TAPE     the recording drawn as a waveform, with the write head (amber) and every voice's read position (numbered
              markers: magenta while playing, dim at the spot it would start), and the tape's settings below
     VOICES   a table of the eight voices: position, rate, length, level and four flags
              (R relative to the head, V reverse, T follows PITCH, L loops)

   Keys, both pages:  Space  start the selected voice     S  stop it      R  record a one-shot     F  freeze or run the head
                      C  empty the tape
   TAPE:    Up/Down choose a setting, Left/Right change it (Shift: coarse), digits then Enter type a raw value
   VOICES:  Up/Down choose a voice, Left/Right move between cells (never changing a value), Enter on a value starts
            adjusting it (Left/Right, Shift coarse; Enter or Esc stops) or type a raw number then Enter; Enter or Space
            on a flag switches it. Values show in friendly units; the bottom line shows the raw register.
   The waveform is read straight from the audio side's buffer once a frame (the app holds the audio lock while drawing). */
#include "desk.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *name; int addr, lo, hi, step, coarse; } Setting;

static const Setting TAPE[] = {
  { "MODE",    REG_BWMODE, 0, 2, 1, 1 },          /* BWMODE */
  { "LENGTH",  REG_BUFMS, 50, 30000, 50, 1000 },   /* BUFMS */
  { "SOURCE",  REG_BWSRC, 0, 3, 1, 1 },          /* BWSRC */
  { "IN",      REG_BIN, 0, 32767, 512, 4096 }, /* BIN */
  { "FEEDBACK",REG_BWFB, 0, 32767, 512, 4096 }, /* BWFB */
  { "WINDOW",  REG_GWIN, 0, 32767, 512, 4096 }, /* GWIN */
  { "ONE-SHOT",REG_BWLEN, 1, 32767, 512, 4096 }, /* BWLEN */
};
#define NTAPE ((int)(sizeof TAPE / sizeof *TAPE))

/* The voice table's cells: register base address, range, steps, and the column where it is drawn. */
typedef struct { const char *name; int base, lo, hi, step, coarse, col, width; } Col;
static const Col COLS[4] = {
  { "POS",  REG_GPOS0, 0, 32767, 256, 2048, 3, 7 },
  { "RATE", REG_GRATE0, 0, 32767, 256, 3072, 11, 8 },       /* a semitone; Shift: an octave */
  { "LEN",  REG_GLEN0, 0, 32767, 50, 500, 20, 6 },
  { "LVL",  REG_GLVL0, 0, 32767, 512, 4096, 27, 5 },
};
static const char FLAG_LABEL[4] = { 'R', 'V', 'T', 'L' };   /* bits 0..3 of GFLG: REL, REV, TRK, LOOP */
#define FLAG_COL 33

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
static int voice_addr(int base, int voice) { return base + voice; }

/* What the voice actually uses: voice 0's position, rate and level are matrix targets, so read their effective values. */
static int voice_value(const FezSynth *s, int c, int voice) {
  int a = voice_addr(COLS[c].base, voice);
  return (voice == 0 && (c == 0 || c == 1 || c == 3)) ? s->eff[a] : s->mem[a];
}

/* ---------------------------------------------------------------- friendly text */

static void fmt_pct(char *out, size_t n, int v) { snprintf(out, n, "%d%%", (int)(v * 100.0 / 32767 + 0.5)); }

static void fmt_tape(char *out, size_t n, const FezSynth *s, int addr, int v) {
  static const char *const MODE[3] = { "frozen", "running", "one-shot" };
  static const char *const SRC[4] = { "mixer", "voice", "mod voice", "both" };
  if (addr == REG_BWMODE) snprintf(out, n, "%s", MODE[clampi(v, 0, 2)]);
  else if (addr == REG_BUFMS) snprintf(out, n, "%d ms", v);
  else if (addr == REG_BWSRC) snprintf(out, n, "%s", SRC[clampi(v, 0, 3)]);
  else if (addr == REG_BWLEN) snprintf(out, n, "%.2f s", v / 32767.0 * s->mem[REG_BUFMS] / 1000.0);
  else fmt_pct(out, n, v);
}

/* A voice cell as text, in the unit a person thinks in. Position is milliseconds on the tape. */
static void fmt_cell(char *out, size_t n, const FezSynth *s, int c, int voice) {
  int v = voice_value(s, c, voice);
  switch (c) {
    case 0: snprintf(out, n, "%dms", (int)(v / 32767.0 * s->mem[REG_BUFMS] + 0.5)); break;
    case 1: regview_fmt_note(out, n, v); break;
    case 2: if (v == 0) snprintf(out, n, "cont"); else snprintf(out, n, "%dms", v); break;
    default: fmt_pct(out, n, v); break;
  }
}

/* ---------------------------------------------------------------- the waveform */

/* One column of the picture is the smallest and largest sample among the tape samples that fall in it. */
static void draw_wave(Desk *d, int x0, int y0, int w, int h) {
  const BufEngine *b = &d->synth->audio.buf;
  int len = b->len > 0 ? b->len : 1, mid = y0 + h / 2;
  gfx_fill(&d->cv, x0, y0, w, h, gfx_colour(COL_PNL));
  gfx_fill(&d->cv, x0, mid, w, 1, gfx_colour(COL_SEL));
  for (int col = 0; col < w; col++) {
    int i0 = (int)((long long)col * len / w), i1 = (int)((long long)(col + 1) * len / w);
    if (i1 <= i0) i1 = i0 + 1;
    if (i1 > len) i1 = len;
    float lo = 0, hi = 0;
    for (int i = i0; i < i1; i++) { float x = b->buf[i]; if (x < lo) lo = x; if (x > hi) hi = x; }
    /* full scale (plus or minus 1) reaches the edge of the picture */
    int y_top = mid - (int)(fminf(hi, 1) * (h / 2 - 1)), y_bot = mid - (int)(fmaxf(lo, -1) * (h / 2 - 1));
    gfx_fill(&d->cv, x0 + col, y_top, 1, y_bot - y_top + 1, gfx_colour(COL_CYN));
  }
  gfx_frame(&d->cv, x0, y0, w, h, gfx_colour(COL_LINE));
}

/* Is voice i making sound right now? A grain while its envelope is up, a continuous voice while its level is up. */
static int voice_active(const FezSynth *s, int i) { return s->audio.buf.vo[i].env > 0.001; }

static void draw_tape_page(Desk *d, View *v) {
  FezSynth *s = d->synth;
  const BufEngine *b = &s->audio.buf;
  int sc = d->scale, len = b->len > 0 ? b->len : 1;
  int ph = clampi(v->rows - 2 - 1 - NTAPE - 3, 3, 14);          /* picture rows: what is left after the header, ruler, settings, help */
  int x0 = v->x + sc, w = v->w - 2 * sc, y0 = v->y + 2 * v->rh, h = ph * v->rh;
  char buf[64];

  draw_wave(d, x0, y0, w, h);
  /* voices: a numbered marker per voice that has a level. Bright and moving while playing; dim where it would start. */
  for (int i = 0; i < BUF_VOICES; i++) {
    if (voice_value(s, 3, i) <= 0) continue;
    int active = voice_active(s, i), flags = s->mem[REG_GFLG0 + i];
    double pos = b->vo[i].pos;
    if (!active) {                                              /* where a trigger would start it */
      double off = voice_value(s, 0, i) / 32767.0 * len;
      pos = flags & 1 ? b->wp - off : off;
      if (pos < 0) pos += len;
    }
    int x = x0 + (int)(pos / len * (w - 1));
    int colour = active ? COL_MAG : COL_DIM;
    gfx_fill(&d->cv, x, y0, 1, h, gfx_colour(colour));
    snprintf(buf, sizeof buf, "%d", i);
    gfx_text(&d->cv, x - GFX_CELL_W * sc / 2, y0 - v->rh + sc, buf, sc, gfx_colour(i == d->bf.voice ? COL_AMB : colour));
  }
  int hx = x0 + (int)((double)b->wp / len * (w - 1));           /* the write head */
  if (b->state != 0) gfx_fill(&d->cv, hx, y0, 2 * sc, h, gfx_colour(COL_AMB));
  else gfx_fill(&d->cv, hx, y0, sc, h, gfx_colour(COL_DIM));
  /* ruler: the tape's length, and the head's state */
  int ry = 2 + ph;
  snprintf(buf, sizeof buf, "0"); view_text(v, 0, ry, buf, COL_DIM);
  snprintf(buf, sizeof buf, "%d ms", s->mem[REG_BUFMS]); view_text_right(v, 0, v->cols - 1, ry, buf, COL_DIM);
  static const char *const STATE[3] = { "frozen", "running", "recording" };
  snprintf(buf, sizeof buf, "head %s  voice %d selected", STATE[clampi(s->bw.state, 0, 2)], d->bf.voice);
  view_text(v, 6, ry, buf, s->bw.state == 2 ? COL_RED : COL_DIM);

  /* settings */
  int first = 3 + ph, vis = v->rows - first - 2;
  if (vis < 1) vis = 1;
  if (vis > NTAPE) vis = NTAPE;
  int top = clampi(d->bf.set - vis / 2, 0, NTAPE - vis);
  for (int r = 0; r < vis; r++) {
    int i = top + r, sel = i == d->bf.set, val = fez_reg_read(s, TAPE[i].addr);
    char text[32];
    if (sel) view_fill(v, 0, first + r, v->cols - 1, 1, COL_AMBD);
    view_text(v, 0, first + r, TAPE[i].name, sel ? COL_AMB : COL_FG);
    if (sel && d->bf.mode == BFM_TYPE) snprintf(buf, sizeof buf, "%s_", d->bf.buf); else snprintf(buf, sizeof buf, "%d", val);
    view_text_right(v, 9, 6, first + r, buf, sel && d->bf.mode == BFM_TYPE ? COL_AMB : COL_FG);
    fmt_tape(text, sizeof text, s, TAPE[i].addr, val);
    view_text(v, 17, first + r, text, COL_DIM);
  }
  const char *name = NULL;
  for (int i = 0; i < NAME_COUNT; i++) if (NAME_TABLE[i].addr == TAPE[d->bf.set].addr) { name = NAME_TABLE[i].name; break; }
  if (name) view_text(v, 0, v->rows - 1, regview_help(name), COL_DIM);
}

/* ---------------------------------------------------------------- the voice table */

static void draw_voices_page(Desk *d, View *v) {
  FezSynth *s = d->synth;
  char buf[48];
  view_text(v, 1, 1, "#", COL_DIM);
  for (int c = 0; c < 4; c++) view_text_right(v, COLS[c].col, COLS[c].width, 1, COLS[c].name, COL_DIM);
  view_text(v, FLAG_COL, 1, "R V T L", COL_DIM);
  for (int i = 0; i < BUF_VOICES; i++) {
    int row = 2 + i, sel = i == d->bf.voice;
    if (row >= v->rows - 2) break;
    if (sel) view_fill(v, 0, row, v->cols - 1, 1, COL_PNL);
    snprintf(buf, sizeof buf, "%d", i);
    view_text(v, 1, row, buf, voice_active(s, i) ? COL_MAG : sel ? COL_AMB : COL_FG);
    for (int c = 0; c < 4; c++) {
      int cur = sel && d->bf.cell == c, adjusting = cur && d->bf.mode == BFM_ADJUST, typing = cur && d->bf.mode == BFM_TYPE;
      if (cur) view_fill(v, COLS[c].col, row, COLS[c].width, 1, adjusting ? COL_GRN : COL_AMBD);
      if (typing) snprintf(buf, sizeof buf, "%s_", d->bf.buf); else fmt_cell(buf, sizeof buf, s, c, i);
      view_text_right(v, COLS[c].col, COLS[c].width, row, buf, adjusting ? COL_BG : typing ? COL_AMB : cur ? COL_AMB : COL_FG);
    }
    for (int f = 0; f < 4; f++) {
      int on = (s->mem[REG_GFLG0 + i] >> f) & 1, cur = sel && d->bf.cell == 4 + f;
      char lab[2] = { FLAG_LABEL[f], '\0' };
      view_cell(v, FLAG_COL + 2 * f, row, 2, lab, on ? COL_BG : COL_DIM, on ? COL_GRN : (cur ? COL_AMBD : COL_PNL), cur);
    }
  }
  /* the bottom lines: the raw register under the cursor, and what the flags mean */
  int a = d->bf.cell < 4 ? voice_addr(COLS[d->bf.cell].base, d->bf.voice) : 490 + d->bf.voice;
  const char *name = NULL;
  for (int i = 0; i < NAME_COUNT; i++) if (NAME_TABLE[i].addr == a) { name = NAME_TABLE[i].name; break; }
  if (name && d->handheld) view_text(v, 0, v->rows - 2, regview_help(name), COL_DIM);          /* no room for "NAME = value" beside the help on 49 columns */
  else if (name) { snprintf(buf, sizeof buf, "%s = %d", name, fez_reg_read(s, a)); view_text(v, 0, v->rows - 2, buf, COL_FG); view_text(v, 16, v->rows - 2, regview_help(name), COL_DIM); }
  view_text(v, 0, v->rows - 1, d->handheld ? "R relative  V reverse  T track pitch  L loop" : "R relative to head   V reverse   T follows PITCH   L loops", COL_DIM);
}

void tab_buffer_draw(Desk *d, View *v) {
  int page = d->bf.page;
  int col = 0;
  static const char *const NAMES[BFP_COUNT] = { "TAPE", "VOICES" };
  for (int i = 0; i < BFP_COUNT; i++) {
    int len = (int)strlen(NAMES[i]);
    if (i == page) view_fill(v, col, 0, len + 1, 1, COL_AMBD);
    view_text(v, col, 0, NAMES[i], i == page ? COL_AMB : COL_DIM);
    col += len + 2;
  }
  if (v->rows < 8) return;
  if (page == BFP_TAPE) draw_tape_page(d, v); else draw_voices_page(d, v);
}

/* ---------------------------------------------------------------- keys */

static void stop_entry(Desk *d) { d->bf.mode = BFM_NORMAL; d->bf.len = 0; d->bf.buf[0] = '\0'; }

/* The register and limits of whatever is under the cursor, or 0 if it is not a number (a flag). */
static int current_target(const Desk *d, int *addr, int *lo, int *hi, int *step, int *coarse) {
  if (d->bf.page == BFP_TAPE) { const Setting *t = &TAPE[d->bf.set]; *addr = t->addr; *lo = t->lo; *hi = t->hi; *step = t->step; *coarse = t->coarse; return 1; }
  if (d->bf.cell >= 4) return 0;
  const Col *c = &COLS[d->bf.cell];
  *addr = voice_addr(c->base, d->bf.voice); *lo = c->lo; *hi = c->hi; *step = c->step; *coarse = c->coarse;
  return 1;
}

static void change(Desk *d, int dir, int coarse) {
  int addr, lo, hi, step, big;
  if (!current_target(d, &addr, &lo, &hi, &step, &big)) return;
  int v = fez_reg_read(d->synth, addr) + dir * (coarse ? big : step);
  fez_reg_write(d->synth, addr, clampi(v, lo, hi));
}

static void toggle_flag(Desk *d) {
  int a = 490 + d->bf.voice;
  fez_reg_write(d->synth, a, d->synth->mem[a] ^ (1 << (d->bf.cell - 4)));
}

void tab_buffer_key(Desk *d, int key, int mods) {
  FezSynth *s = d->synth;
  int tape = d->bf.page == BFP_TAPE;
  switch (key) {
    case KEY_UP: case KEY_DOWN: {
      int dir = key == KEY_DOWN ? 1 : -1;
      if (tape) d->bf.set = (d->bf.set + dir + NTAPE) % NTAPE;
      else d->bf.voice = (d->bf.voice + dir + BUF_VOICES) % BUF_VOICES;
      stop_entry(d);
      break;
    }
    case KEY_LEFT: case KEY_RIGHT: {
      int dir = key == KEY_RIGHT ? 1 : -1;
      if (tape) { change(d, dir, mods & MOD_SHIFT); stop_entry(d); }                    /* the tape page has one column: Left/Right change the value */
      else if (d->bf.mode == BFM_ADJUST) change(d, dir, mods & MOD_SHIFT);              /* the table: only while adjusting */
      else { d->bf.cell = clampi(d->bf.cell + dir, 0, 7); stop_entry(d); }
      break;
    }
    case KEY_ENTER: {
      int addr, lo, hi, step, big;
      if (d->bf.mode == BFM_TYPE && current_target(d, &addr, &lo, &hi, &step, &big)) { fez_reg_write(s, addr, clampi(atoi(d->bf.buf), lo, hi)); stop_entry(d); }
      else if (!tape && d->bf.cell >= 4) toggle_flag(d);
      else if (!tape) { d->bf.mode = d->bf.mode == BFM_ADJUST ? BFM_NORMAL : BFM_ADJUST; desk_say(d, d->bf.mode == BFM_ADJUST ? (d->handheld ? "ADJUSTING: Left/Right, A when done" : "ADJUSTING: Left/Right, Enter when done") : "RELEASED"); }
      break;
    }
    case KEY_ESC: stop_entry(d); break;
    case KEY_BACKSPACE: if (d->bf.len > 0) d->bf.buf[--d->bf.len] = '\0'; if (d->bf.len == 0 && d->bf.mode == BFM_TYPE) d->bf.mode = BFM_NORMAL; break;
    case KEY_TAB: d->bf.page = !d->bf.page; stop_entry(d); break;
    default: break;
  }
}

void tab_buffer_text(Desk *d, int ch) {
  FezSynth *s = d->synth;
  if (ch == '[' || ch == ']') { d->bf.page = (d->bf.page + (ch == ']' ? 1 : BFP_COUNT - 1)) % BFP_COUNT; stop_entry(d); return; }
  if (ch >= '0' && ch <= '9') {                                                          /* a raw number for the cell under the cursor */
    if (d->bf.page == BFP_VOICES && d->bf.cell >= 4) return;
    if (d->bf.mode != BFM_TYPE) { d->bf.len = 0; d->bf.buf[0] = '\0'; }
    if (d->bf.len < 5) { d->bf.buf[d->bf.len++] = (char)ch; d->bf.buf[d->bf.len] = '\0'; d->bf.mode = BFM_TYPE; }
    return;
  }
  if (d->bf.mode == BFM_TYPE) return;
  switch (ch) {
    case ' ':
      if (d->bf.page == BFP_VOICES && d->bf.cell >= 4) { toggle_flag(d); break; }       /* on a flag, Space is the toggle, as on the Pulsers tab */
      fez_reg_write(s, REG_GTRIG, 1 << d->bf.voice); break;                                   /* GTRIG */
    case 's': case 'S': fez_reg_write(s, REG_GSTOP, 1 << d->bf.voice); break;                  /* GSTOP */
    case 'r': case 'R': fez_reg_write(s, REG_BWTRIG, 1); desk_say(d, "RECORDING ONE-SHOT"); break;   /* BWTRIG */
    case 'f': case 'F': fez_reg_write(s, REG_BWMODE, s->mem[REG_BWMODE] == 0 ? 1 : 0); desk_say(d, s->mem[REG_BWMODE] ? "HEAD RUNNING" : "HEAD FROZEN"); break;
    case 'c': case 'C': fez_clear_buffer(s); desk_say(d, "TAPE CLEARED"); break;
    default: break;
  }
}
