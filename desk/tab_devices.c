/* The DEVICES tab: the virtual machines that make things happen on their own. One page per device, switched with Tab.

   CELLULAR  16 cells in a ring. Each generation a cell becomes 0 or 1 from itself and its two neighbours, by the bits of
             RULE. The picture is the history: the newest generation at the bottom, older ones above it. The automaton
             only moves when something steps it (a block writing CASTEP, or Space here). A "birth" in the TAP column
             can fire a pulser (TRIG).
   LORENZ    three numbers (X, Y, Z) that swirl forever and never repeat. The picture is X across and Z up: the famous
             butterfly. RATE sets the speed, RHO how wild it is; X crossing zero can fire a pulser (TRIG).

   QUANTISER a piano keyboard: the keys lit are the notes of the scale over the root. When ON, the pitch the voice hears
             snaps to the nearest lit key. The PITCH register itself is left alone.
   TURING    a ring of bits that is read, sometimes flipped, and fed back in. The picture is its history, newest at the
             bottom; the bits slide left each step. CHANCE 0 repeats forever, 50 percent is random. It steps when a
             pulser in PULSERS fires, or on Space.

   DELAY     the output tape delay: every sound, including the buffer's return, goes through it (then the spring, then the
             compressor). The picture is the train of echoes it makes: one bar per repeat, each as tall as the feedback
             leaves it. TIME and FEEDBACK are the echo; DAMPING is the lowpass each echo passes through (lower = duller);
             WOBBLE is the tape's wandering speed (it bends the pitch of the echoes); NOISE is tape hiss written into
             the loop, so it builds up with feedback; FREEZE holds the loop's contents for ever; MIX is the wet level.

   KEYS      the computer keyboard as a piano (tracker layout, two rows). Hold a key and the voice sounds; let go and it
             falls over REL. Newest key wins; letting go of it returns to the one before. The picture is the notes you
             played, scrolling left. OCTAVE moves the bottom key (C3 by default); PULSERS are fired on every key press, so
             blocks can react to playing. Registers KBNOTE, KBGATE, KBOCT, KBPUL; matrix source KB.

   Keys:  ] / [  or  Tab / Shift+Tab   next / previous device       Up/Down      choose a setting
          Left/Right        change it (Shift: coarse)    digits, Enter  type a value (not on KEYS: digits are notes there)
          Space or Enter    CELLULAR: one generation     LORENZ: restart from a new point
                            QUANTISER: switch on/off     TURING: one step
          CELLULAR n: random row, c: one cell in the middle     LORENZ r: restart     TURING n: random bits
          KEYS: Space or Enter lets go of every key

   The pictures are recorded here, not in the core: once a frame we note what the devices look like now. If the
   automaton takes several steps within one frame, only the last row is drawn (ponytail: fine at pulser speeds, which
   top out at 10 a second). */
#include "desk.h"
#include "scales.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *name; int addr, lo, hi, step, coarse; } Setting;

static const Setting CA_SETTINGS[] = {
  { "RULE", REG_CARULE, 0, 255, 1, 16 }, { "TAP", REG_CATAP, 0, 15, 1, 4 }, { "TRIG", REG_CATRIG, 0, 4, 1, 1 } };
static const Setting LZ_SETTINGS[] = {
  { "RATE", REG_LZRATE, 0, 32767, 512, 4096 }, { "RHO", REG_LZRHO, 0, 32767, 512, 4096 }, { "TRIG", REG_LZTRIG, 0, 4, 1, 1 } };
static const Setting QT_SETTINGS[] = {
  { "ON", REG_QON, 0, 1, 1, 1 }, { "SCALE", REG_QSCALE, 0, 7, 1, 1 }, { "ROOT", REG_QROOT, 0, 32767, 256, 1792 } };   /* ROOT: a semitone, or a fifth with Shift */
static const Setting TM_SETTINGS[] = {
  { "CHANCE", REG_TMCHANCE, 0, 32767, 512, 4096 }, { "LEN", REG_TMLEN, 2, 16, 1, 4 }, { "PULSERS", REG_TMPUL, 0, 15, 1, 1 } };
static const Setting DLY_SETTINGS[] = {
  { "TIME", REG_DLYTIME, 0, 32767, 128, 2731 }, { "FEEDBACK", REG_DLYFB, 0, 32767, 512, 4096 }, { "DAMPING", REG_DLYLP, 0, 32767, 512, 4096 },
  { "WOBBLE", REG_DLYWOB, 0, 32767, 512, 4096 }, { "NOISE", REG_DLYNZ, 0, 32767, 512, 4096 },
  { "FREEZE", REG_DLYFRZ, 0, 32767, 32767, 32767 },                           /* on at or above half, so one step flips it */
  { "MIX", REG_DLYMIX, 0, 32767, 512, 4096 }, { "INPUT", REG_DLYIN, 0, 32767, 512, 4096 } };
static const char *const DEVICE_NAMES[DEV_COUNT] = { "CELLULAR", "LORENZ", "QUANTISER", "TURING", "DELAY", "KEYS" };

static const Setting KB_SETTINGS[] = {
  { "OCTAVE", REG_KBOCT, 0, 8, 1, 1 }, { "PULSERS", REG_KBPUL, 0, 15, 1, 1 } };

static const Setting *settings_of(int sub, int *n) {
  static const Setting *const TABLE[DEV_COUNT] = { CA_SETTINGS, LZ_SETTINGS, QT_SETTINGS, TM_SETTINGS, DLY_SETTINGS, KB_SETTINGS };
  /* counted from the tables themselves, so adding a setting to a table cannot leave a stale number here */
  static const int COUNT[DEV_COUNT] = { (int)(sizeof CA_SETTINGS / sizeof CA_SETTINGS[0]), (int)(sizeof LZ_SETTINGS / sizeof LZ_SETTINGS[0]),
    (int)(sizeof QT_SETTINGS / sizeof QT_SETTINGS[0]), (int)(sizeof TM_SETTINGS / sizeof TM_SETTINGS[0]), (int)(sizeof DLY_SETTINGS / sizeof DLY_SETTINGS[0]), (int)(sizeof KB_SETTINGS / sizeof KB_SETTINGS[0]) };
  *n = COUNT[sub];
  return TABLE[sub];
}

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

/* ---------------------------------------------------------------- recording (once a frame) */

/* Add a row to a history if it differs from the last one; the oldest falls off when full. */
static void push_history(uint16_t *hist, int *n, uint16_t row) {
  if (*n > 0 && hist[*n - 1] == row) return;
  if (*n == CA_HIST) { memmove(hist, hist + 1, (CA_HIST - 1) * sizeof row); (*n)--; }
  hist[(*n)++] = row;
}

void devices_watch(Desk *d) {
  FezSynth *s = d->synth;
  push_history(d->dev.ca_hist, &d->dev.ca_n, (uint16_t)s->ca.row);
  push_history(d->dev.tm_hist, &d->dev.tm_n, (uint16_t)s->tm.bits);
  d->dev.lz_x[d->dev.lz_head] = fez_reg_read(s, REG_LZX);
  d->dev.lz_z[d->dev.lz_head] = fez_reg_read(s, REG_LZZ);
  d->dev.lz_head = (d->dev.lz_head + 1) % LZ_HIST;
  if (d->dev.lz_n < LZ_HIST) d->dev.lz_n++;
  /* the keyboard's trace: the note held this frame, or -1. Shifting 480 bytes a frame is nothing. */
  if (d->dev.kb_n == KB_HIST) { memmove(d->dev.kb_hist, d->dev.kb_hist + 1, KB_HIST - 1); d->dev.kb_n--; }
  d->dev.kb_hist[d->dev.kb_n++] = (int8_t)(s->mem[REG_KBGATE] ? s->mem[REG_KBNOTE] / 256 : -1);
}

/* ---------------------------------------------------------------- drawing */

static void trig_name(char *out, size_t n, int v) { if (v >= 0 && v < NUM_PULSERS) snprintf(out, n, "P%d", v); else snprintf(out, n, "off"); }

/* The settings list of the page at (col, row), `width` cells wide. */
static void draw_settings(Desk *d, View *v, int col, int row, int width) {
  int n; const Setting *st = settings_of(d->dev.sub, &n);
  for (int i = 0; i < n; i++) {
    int sel = i == d->dev.sel[d->dev.sub], val = fez_reg_read(d->synth, st[i].addr);
    char buf[48], extra[32] = "";
    if (sel) view_fill(v, col, row + i, width, 1, COL_AMBD);
    view_text(v, col, row + i, st[i].name, sel ? COL_AMB : COL_FG);
    if (sel && d->dev.editing) snprintf(buf, sizeof buf, "%s_", d->dev.buf); else snprintf(buf, sizeof buf, "%d", val);
    view_text_right(v, col + 8, 6, row + i, buf, sel && d->dev.editing ? COL_AMB : COL_FG);
    if (st[i].addr == REG_CATRIG || st[i].addr == REG_LZTRIG) trig_name(extra, sizeof extra, val);
    else if (st[i].addr == REG_CARULE) for (int b = 7; b >= 0; b--) extra[7 - b] = (char)('0' + ((val >> b) & 1)), extra[8] = '\0';
    else if (st[i].addr == REG_LZRHO) snprintf(extra, sizeof extra, "rho %.1f", lz_rho(val));
    else if (st[i].addr == REG_LZRATE) snprintf(extra, sizeof extra, "%.1f/s", lz_units_per_sec(val));
    else if (st[i].addr == REG_QON) snprintf(extra, sizeof extra, "%s", val ? "on" : "off");
    else if (st[i].addr == REG_QSCALE) snprintf(extra, sizeof extra, "%s", fez_scale_name(val));
    else if (st[i].addr == REG_QROOT) regview_fmt_note(extra, sizeof extra, val);
    else if (st[i].addr == REG_TMCHANCE) snprintf(extra, sizeof extra, "flip %d%%", (int)(val * 100.0 / 32768 + 0.5));
    else if (st[i].addr >= REG_DLYTIME && st[i].addr <= REG_DLYIN) regview_fmt_param(extra, sizeof extra, st[i].addr, val);      /* the tape delay's registers */
    else if (st[i].addr == REG_KBOCT) snprintf(extra, sizeof extra, "C%d to B%d", val, val + 1);     /* the two rows of keys */
    else if (st[i].addr == REG_TMPUL || st[i].addr == REG_KBPUL) {
      size_t l = 0; extra[0] = '\0';
      for (int p = 0; p < NUM_PULSERS; p++) if ((val >> p) & 1) l += (size_t)snprintf(extra + l, sizeof extra - l, "%sP%d", l ? " " : "", p);
      if (!l) snprintf(extra, sizeof extra, "none");
    }
    view_text(v, col + 15, row + i, extra, COL_DIM);
  }
}

/* A picture of a row of 16 bits over time: newest generation at the bottom. Bit 0 is drawn at the right, like a number.
   live_mask says which bits are in use (others are drawn as dark columns); mark_col, if not negative, shades one bit's column. */
static void draw_history(Desk *d, View *v, const uint16_t *hist, int n, int prows, int live_mask, int mark_col) {
  int cs = v->rh, x0 = v->x, y0 = v->y + 2 * v->rh, rows = prows < CA_HIST ? prows : CA_HIST, sc = d->scale;
  gfx_fill(&d->cv, x0, y0, 16 * cs, rows * cs, gfx_colour(COL_PNL));
  for (int c = 0; c < 16; c++)
    if (!((live_mask >> c) & 1)) gfx_fill(&d->cv, x0 + (15 - c) * cs, y0, cs, rows * cs, gfx_colour(COL_BG));
  if (mark_col >= 0) gfx_fill(&d->cv, x0 + (15 - mark_col) * cs, y0, cs, rows * cs, gfx_colour(COL_SEL));
  for (int r = 0; r < rows && r < n; r++) {
    int gen = n - 1 - r, y = y0 + (rows - 1 - r) * cs, bits = hist[gen];
    for (int c = 0; c < 16; c++)
      if (((bits >> c) & 1) && ((live_mask >> c) & 1))
        gfx_fill(&d->cv, x0 + (15 - c) * cs + sc, y + sc, cs - 2 * sc, cs - 2 * sc, gfx_colour(r == 0 ? COL_AMB : COL_CYN));
  }
  gfx_frame(&d->cv, x0, y0, 16 * cs, rows * cs, gfx_colour(COL_LINE));
}

static void draw_cellular(Desk *d, View *v, int pane_w, int prows, int sx, int sy, int swidth) {
  FezSynth *s = d->synth;
  (void)pane_w;
  draw_history(d, v, d->dev.ca_hist, d->dev.ca_n, prows, 0xFFFF, s->ca.tap);        /* the watched column is shaded */
  draw_settings(d, v, sx, sy, swidth);
  char buf[64];
  int alive = fez_reg_read(s, REG_CACOUNT), watched = fez_reg_read(s, REG_CABIT);
  snprintf(buf, sizeof buf, "alive %d of 16", alive); view_text(v, sx, sy + 4, buf, COL_FG);
  snprintf(buf, sizeof buf, "watching cell %d: %s", s->ca.tap, watched ? "alive" : "dead"); view_text(v, sx, sy + 5, buf, COL_FG);
  if (sy + 7 < v->rows) view_text(v, sx, sy + 7, "Space: step once", COL_DIM);
  if (sy + 8 < v->rows) view_text(v, sx, sy + 8, "n: random row   c: one cell", COL_DIM);
}

/* One octave of piano keys. A key is lit if its note is in the scale over the root. The key the voice is playing now
   (after quantising) gets an amber dot. */
static void draw_keyboard(Desk *d, int x0, int y0, int w, int h, int mask, int root_pc, int heard_pc) {
  static const int WHITE[7] = { 0, 2, 4, 5, 7, 9, 11 }, BLACK_AFTER[5] = { 0, 1, 3, 4, 5 }, BLACK_PC[5] = { 1, 3, 6, 8, 10 };
  static const char *const LABEL[7] = { "C", "D", "E", "F", "G", "A", "B" };
  int sc = d->scale, kw = w / 7, bw = kw * 6 / 10, bh = h * 6 / 10;
  for (int i = 0; i < 7; i++) {
    int lit = (mask >> ((WHITE[i] - root_pc + 12) % 12)) & 1, x = x0 + i * kw;
    gfx_fill(&d->cv, x + sc, y0, kw - 2 * sc, h, gfx_colour(lit ? COL_CYN : COL_SEL));
    if (WHITE[i] == root_pc) gfx_frame(&d->cv, x + sc, y0, kw - 2 * sc, h, gfx_colour(COL_AMB));
    gfx_text(&d->cv, x + (kw - GFX_CELL_W * sc) / 2, y0 + h - 10 * sc, LABEL[i], sc, gfx_colour(lit ? COL_BG : COL_DIM));
    if (WHITE[i] == heard_pc) gfx_fill(&d->cv, x + kw / 2 - 2 * sc, y0 + h - 16 * sc, 4 * sc, 4 * sc, gfx_colour(COL_AMB));
  }
  for (int i = 0; i < 5; i++) {
    int lit = (mask >> ((BLACK_PC[i] - root_pc + 12) % 12)) & 1, x = x0 + (BLACK_AFTER[i] + 1) * kw - bw / 2;
    gfx_fill(&d->cv, x, y0, bw, bh, gfx_colour(COL_BG));
    if (lit) gfx_fill(&d->cv, x + sc, y0, bw - 2 * sc, bh - sc, gfx_mix(gfx_colour(COL_BG), gfx_colour(COL_CYN), 0.7));
    if (BLACK_PC[i] == root_pc) gfx_frame(&d->cv, x, y0, bw, bh, gfx_colour(COL_AMB));
    if (BLACK_PC[i] == heard_pc) gfx_fill(&d->cv, x + bw / 2 - 2 * sc, y0 + bh - 6 * sc, 4 * sc, 4 * sc, gfx_colour(COL_AMB));
  }
}

static void draw_quantiser(Desk *d, View *v, int pane_w, int prows, int sx, int sy, int swidth) {
  FezSynth *s = d->synth;
  int h = prows * v->rh < 7 * v->rh ? prows * v->rh : 7 * v->rh;
  int root_pc = ((s->mem[REG_QROOT] + 128) / 256) % 12, heard = s->mem[REG_QON] ? ((s->eff[REG_PITCH] + 128) / 256) % 12 : -1;
  draw_keyboard(d, v->x, v->y + 2 * v->rh, pane_w, h, fez_scale_mask(s->mem[REG_QSCALE]), root_pc, heard);
  draw_settings(d, v, sx, sy, swidth);
  char buf[48], name[16];
  regview_fmt_note(name, sizeof name, s->mem[REG_PITCH]);
  snprintf(buf, sizeof buf, "PITCH register  %s", name); view_text(v, sx, sy + 4, buf, COL_FG);
  if (s->mem[REG_QON]) { regview_fmt_note(name, sizeof name, s->eff[REG_PITCH]); snprintf(buf, sizeof buf, "voice hears     %s", name); view_text(v, sx, sy + 5, buf, COL_GRN); }
  else view_text(v, sx, sy + 5, "voice hears     the register as it is", COL_DIM);
  if (sy + 7 < v->rows) view_text(v, sx, sy + 7, "Space: on or off", COL_DIM);
  if (sy + 8 < v->rows) view_text(v, sx, sy + 8, "amber frame: the root", COL_DIM);
}

static void draw_turing(Desk *d, View *v, int pane_w, int prows, int sx, int sy, int swidth) {
  FezSynth *s = d->synth;
  int len = s->mem[REG_TMLEN] < 2 ? 2 : s->mem[REG_TMLEN] > 16 ? 16 : s->mem[REG_TMLEN];
  (void)pane_w;
  draw_history(d, v, d->dev.tm_hist, d->dev.tm_n, prows, (1 << len) - 1, -1);        /* bits past the loop length are dark */
  draw_settings(d, v, sx, sy, swidth);
  char buf[32];
  int out = fez_reg_read(s, REG_TMOUT);
  snprintf(buf, sizeof buf, "bits %04X", s->tm.bits & 0xffff); view_text(v, sx, sy + 4, buf, COL_FG);
  view_text(v, sx, sy + 5, "out", COL_DIM);
  view_bar(v, sx + 4, sy + 5, swidth - 12 > 4 ? swidth - 12 : 4, out / 32767.0, COL_CYN, COL_SEL);
  snprintf(buf, sizeof buf, "%d", out); view_text_right(v, sx + swidth - 7, 6, sy + 5, buf, COL_FG);
  if (sy + 7 < v->rows) view_text(v, sx, sy + 7, "Space: step once", COL_DIM);
  if (sy + 8 < v->rows) view_text(v, sx, sy + 8, "n: random bits", COL_DIM);
}

/* The tape loop, live. The delay line is a ring of 1.5 s of audio; the picture is that ring, drawn left to right, so what
   you see is what the loop holds right now. The write head (amber) sweeps across laying down new sound; the read head
   (magenta) trails it by the delay time, and the shaded band between them is the echo on its way round. Wobble makes the
   read head waver, turning TIME slides it, and hiss shows as fuzz on the flat line. Frozen, the write head stops (grey),
   the frame goes amber and the picture holds still. Like the buffer page, it reads the audio side's memory directly
   (the app holds the audio lock while drawing). */
static void draw_delay(Desk *d, View *v, int pane_w, int prows) {
  const Audio *a = &d->synth->audio;
  int x0 = v->x + d->scale, y0 = v->y + 2 * v->rh, w = pane_w - d->scale, h = prows * v->rh, sc = d->scale, mid = y0 + h / 2;
  int frozen = d->synth->eff[REG_DLYFRZ] >= 16384;
  gfx_fill(&d->cv, x0, y0, w, h, gfx_colour(COL_PNL));
  /* 1) the loop span: from the read head forward to the write head, wrapping round the end of the ring */
  int wx = (int)((double)a->dly_w / DLY_SAMPLES * (w - 1)), rx = (int)(a->dly_rp / DLY_SAMPLES * (w - 1));
  if (rx <= wx) gfx_fill(&d->cv, x0 + rx, y0, wx - rx + 1, h, gfx_colour(COL_SEL));
  else { gfx_fill(&d->cv, x0 + rx, y0, w - rx, h, gfx_colour(COL_SEL)); gfx_fill(&d->cv, x0, y0, wx + 1, h, gfx_colour(COL_SEL)); }
  /* 2) the audio: each column shows the smallest and largest sample among the ring samples that fall in it */
  gfx_fill(&d->cv, x0, mid, w, 1, gfx_colour(COL_LINE));
  for (int col = 0; col < w; col++) {
    int i0 = (int)((long long)col * DLY_SAMPLES / w), i1 = (int)((long long)(col + 1) * DLY_SAMPLES / w);
    float lo = 0, hi = 0;
    for (int i = i0; i < i1 && i < DLY_SAMPLES; i++) { float x = a->dly[i]; if (x < lo) lo = x; if (x > hi) hi = x; }
    int y_top = mid - (int)(fminf(hi, 1) * (h / 2 - 1)), y_bot = mid - (int)(fmaxf(lo, -1) * (h / 2 - 1));
    gfx_fill(&d->cv, x0 + col, y_top, 1, y_bot - y_top + 1, gfx_colour(COL_CYN));
  }
  /* 3) the heads */
  gfx_fill(&d->cv, x0 + rx, y0, sc, h, gfx_colour(COL_MAG));
  gfx_fill(&d->cv, x0 + wx, y0, 2 * sc, h, gfx_colour(frozen ? COL_DIM : COL_AMB));
  gfx_frame(&d->cv, x0, y0, w, h, gfx_colour(frozen ? COL_AMB : COL_LINE));
  /* 4) the ruler under the picture */
  char buf[48];
  view_text(v, 0, 2 + prows, "0", COL_DIM);
  snprintf(buf, sizeof buf, "%.1f s", (double)DLY_SAMPLES / FEZ_SAMPLE_RATE); view_text_right(v, 0, v->cols - 1, 2 + prows, buf, COL_DIM);
  if (v->cols >= 48 || frozen) view_text(v, 6, 2 + prows, frozen ? "frozen" : "write head amber, read head magenta", frozen ? COL_AMB : COL_DIM);   /* the caption only if it fits */
}

/* The keyboard: a scrolling trace of the notes you play, newest at the right edge, with a line at every C. Held notes are
   drawn as short bars, one per frame, so a long hold is a long line and a gap is silence. Under the settings, the computer
   keys drawn as a piano, with the ones held right now lit. */
static void draw_keys(Desk *d, View *v, int pane_w, int prows, int sx, int sy, int swidth) {
  FezSynth *s = d->synth;
  int x0 = v->x + d->scale, y0 = v->y + 2 * v->rh, w = pane_w - d->scale, h = prows * v->rh, sc = d->scale;
  int base = (fez_reg_read(s, REG_KBOCT) + 1) * 12;                 /* the bottom key's MIDI note */
  int lo = base - 12 < 0 ? 0 : base - 12, span = 48;                  /* four octaves shown: one below the keys, the two the keys cover, one above */
  gfx_fill(&d->cv, x0, y0, w, h, gfx_colour(COL_PNL));
  for (int n = lo; n < lo + span; n++) {                              /* a line at every C, labelled */
    if (n % 12) continue;
    int y = y0 + h - 1 - (n - lo) * (h - 1) / span;
    gfx_fill(&d->cv, x0, y, w, 1, gfx_colour(COL_SEL));
    char lab[16]; snprintf(lab, sizeof lab, "C%d", n / 12 - 1);
    gfx_text(&d->cv, x0 + 2 * sc, y - GFX_CELL_H * sc - sc, lab, sc, gfx_colour(COL_DIM));
  }
  int colw = w / KB_HIST + 1;
  for (int k = 0; k < d->dev.kb_n; k++) {
    int note = d->dev.kb_hist[k];
    if (note < 0) continue;
    if (note < lo) note = lo;
    if (note >= lo + span) note = lo + span - 1;
    int x = x0 + w - 1 - (d->dev.kb_n - 1 - k) * (w - 1) / (KB_HIST - 1);
    int y = y0 + h - 1 - (note - lo) * (h - 1) / span;
    gfx_fill(&d->cv, x - colw + 1, y - sc, colw, 2 * sc, gfx_colour(k == d->dev.kb_n - 1 ? COL_AMB : COL_MAG));
  }
  gfx_frame(&d->cv, x0, y0, w, h, gfx_colour(s->mem[REG_KBGATE] ? COL_AMB : COL_LINE));
  view_text(v, 0, 2 + prows, "older", COL_DIM);
  view_text_right(v, 0, v->cols - 1, 2 + prows, "now", COL_DIM);
  draw_settings(d, v, sx, sy, swidth);

  /* the keys, as a little piano. Black keys sit between the white ones above them, so the rows line up like the real thing.
     The white keys are semitones 0 2 4 5 7 9 11 of an octave and the black ones 1 3 6 8 10 (the gaps are E-F and B-C). */
  static const int WHITE[7] = { 0, 2, 4, 5, 7, 9, 11 };
  static const int BLACK[5] = { 1, 3, 6, 8, 10 }, BLACK_COL[5] = { 1, 3, 7, 9, 11 };
  int ly = sy + 3, lx = 2;                                                     /* under the settings, one line of space */
  for (int row = 1; row >= 0; row--) {                                         /* the upper octave first, as it sits above on the keyboard */
    const char *keys = row ? KB_KEYS_HIGH : KB_KEYS_LOW;
    int top = ly + (1 - row) * 2;                                              /* each octave is two lines: black keys, then white */
    for (int i = 0; i < 7; i++) {
      char ch = keys[WHITE[i]], t[2] = { (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch), 0 };
      view_text(v, lx + 2 * i, top + 1, t, d->kb_key_note[(int)ch] ? COL_MAG : COL_FG);
    }
    for (int i = 0; i < 5; i++) {
      char ch = keys[BLACK[i]], t[2] = { (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch), 0 };
      view_text(v, lx + BLACK_COL[i], top, t, d->kb_key_note[(int)ch] ? COL_MAG : COL_DIM);
    }
    char name[24]; int oct = fez_reg_read(s, REG_KBOCT) + row;
    snprintf(name, sizeof name, "C%d to B%d", oct, oct);
    view_text(v, lx + 16, top + 1, name, COL_DIM);
  }
}

static void draw_lorenz(Desk *d, View *v, int pane_w, int prows, int sx, int sy, int swidth) {
  FezSynth *s = d->synth;
  int x0 = v->x, y0 = v->y + 2 * v->rh, w = pane_w, h = prows * v->rh, sc = d->scale;
  gfx_fill(&d->cv, x0, y0, w, h, gfx_colour(COL_PNL));
  gfx_fill(&d->cv, x0 + w / 2, y0, 1, h, gfx_colour(COL_SEL));                 /* the zero axes */
  gfx_fill(&d->cv, x0, y0 + h / 2, w, 1, gfx_colour(COL_SEL));
  /* the trace, oldest to newest, fading in. Register values -32768..32767 fill the box. */
  int px = 0, py = 0;
  for (int k = 0; k < d->dev.lz_n; k++) {
    int i = (d->dev.lz_head - d->dev.lz_n + k + LZ_HIST) % LZ_HIST;
    int x = x0 + (int)((d->dev.lz_x[i] + 32768) / 65536.0 * (w - 1)), y = y0 + h - 1 - (int)((d->dev.lz_z[i] + 32768) / 65536.0 * (h - 1));
    if (k > 0) gfx_line(&d->cv, px, py, x, y, gfx_mix(gfx_colour(COL_SEL), gfx_colour(COL_CYN), (double)k / d->dev.lz_n));
    px = x; py = y;
  }
  if (d->dev.lz_n) gfx_fill(&d->cv, px - sc, py - sc, 3 * sc, 3 * sc, gfx_colour(COL_AMB));    /* the present */
  gfx_frame(&d->cv, x0, y0, w, h, gfx_colour(COL_LINE));

  draw_settings(d, v, sx, sy, swidth);
  static const char *const AXIS[3] = { "X", "Y", "Z" };
  for (int a = 0; a < 3; a++) {
    int val = fez_reg_read(s, REG_LZX + a);
    char buf[16]; snprintf(buf, sizeof buf, "%d", val);
    view_text(v, sx, sy + 4 + a, AXIS[a], COL_DIM);
    view_bar(v, sx + 2, sy + 4 + a, swidth - 10 > 4 ? swidth - 10 : 4, (val + 32768) / 65535.0, COL_CYN, COL_SEL);
    view_text_right(v, sx + swidth - 7, 6, sy + 4 + a, buf, COL_FG);
  }
  if (sy + 8 < v->rows) view_text(v, sx, sy + 8, "Space or r: restart", COL_DIM);
}

void tab_devices_draw(Desk *d, View *v) {
  int sub = d->dev.sub;
  /* the page names along the top, the showing one lit */
  int col = 0;
  for (int i = 0; i < DEV_COUNT; i++) {
    int len = (int)strlen(DEVICE_NAMES[i]);
    if (i == sub) view_fill(v, col, 0, len + 1, 1, COL_AMBD);
    view_text(v, col, 0, DEVICE_NAMES[i], i == sub ? COL_AMB : COL_DIM);
    col += len + 2;
  }

  /* Wide tiles: the picture on the left, the settings beside it. Narrow tiles: the settings go under the picture. */
  int top_only = sub == DEV_DELAY || sub == DEV_KEYS;                        /* these pictures are always on top, full width */
  int pane_w = 16 * v->rh, wide = v->w >= pane_w + 26 * v->cw && !top_only;
  int prows, sx, sy, swidth;
  if (wide) { prows = v->rows - 2; sx = (pane_w + v->cw - 1) / v->cw + 2; sy = 2; swidth = v->cols - sx - 1; }
  else { int n; settings_of(sub, &n); prows = v->rows - 2 - (n + (sub == DEV_DELAY ? 2 : sub == DEV_KEYS ? 6 : 5)); if (prows < 3) prows = 3;      /* the settings, live values and hints need n + 5 rows under the picture */ sx = 0; sy = 2 + prows + 1; swidth = v->cols - 1; }
  if (prows < 3) return;
  if (sub == DEV_CELLULAR) draw_cellular(d, v, pane_w, prows, sx, sy, swidth);
  else if (sub == DEV_LORENZ) draw_lorenz(d, v, wide ? pane_w : v->w - 2, prows, sx, sy, swidth);
  else if (sub == DEV_QUANTISER) draw_quantiser(d, v, wide ? pane_w : v->w - 2, prows, sx, sy, swidth);
  else if (sub == DEV_DELAY) { draw_delay(d, v, v->w - 2, prows); draw_settings(d, v, sx, sy, swidth); }
  else if (sub == DEV_KEYS) draw_keys(d, v, v->w - 2, prows, sx, sy, swidth);
  else draw_turing(d, v, pane_w, prows, sx, sy, swidth);
}

/* ---------------------------------------------------------------- keys */

static void stop_entry(Desk *d) { d->dev.editing = 0; d->dev.len = 0; d->dev.buf[0] = '\0'; }

/* The page's main action: step the automaton or the Turing machine, switch the quantiser or the freeze, or restart the attractor. */
static void action(Desk *d) {
  if (d->dev.sub == DEV_CELLULAR) fez_reg_write(d->synth, REG_CASTEP, 1);
  else if (d->dev.sub == DEV_QUANTISER) fez_reg_write(d->synth, REG_QON, !fez_reg_read(d->synth, REG_QON));
  else if (d->dev.sub == DEV_TURING) fez_reg_write(d->synth, REG_TMSTEP, 1);
  else if (d->dev.sub == DEV_KEYS) desk_all_notes_off(d);                              /* a stuck note, or just tidying up */
  else if (d->dev.sub == DEV_DELAY) fez_reg_write(d->synth, REG_DLYFRZ, fez_reg_read(d->synth, REG_DLYFRZ) >= 16384 ? 0 : 32767);   /* freeze on or off */
  else fez_reg_write(d->synth, REG_LZSEED, rand() & 0x7fff);                         /* ponytail: libc rand, not the synth's generator: a UI gesture needs no replay */
}

void tab_devices_key(Desk *d, int key, int mods) {
  int n; const Setting *st = settings_of(d->dev.sub, &n);
  int *sel = &d->dev.sel[d->dev.sub];
  switch (key) {
    case KEY_TAB: d->dev.sub = (d->dev.sub + (mods & MOD_SHIFT ? DEV_COUNT - 1 : 1)) % DEV_COUNT; stop_entry(d); break;
    case KEY_UP:   *sel = (*sel + n - 1) % n; stop_entry(d); break;
    case KEY_DOWN: *sel = (*sel + 1) % n; stop_entry(d); break;
    case KEY_LEFT: case KEY_RIGHT: {
      const Setting *t = &st[*sel];
      int dir = key == KEY_RIGHT ? 1 : -1, v = fez_reg_read(d->synth, t->addr);
      fez_reg_write(d->synth, t->addr, clampi(v + dir * ((mods & MOD_SHIFT) ? t->coarse : t->step), t->lo, t->hi));
      stop_entry(d);
      break;
    }
    case KEY_ENTER:
      if (d->dev.editing) { fez_reg_write(d->synth, st[*sel].addr, clampi(atoi(d->dev.buf), st[*sel].lo, st[*sel].hi)); stop_entry(d); }
      else action(d);
      break;
    case KEY_ESC: stop_entry(d); break;
    case KEY_BACKSPACE:
      if (d->dev.len > 0) d->dev.buf[--d->dev.len] = '\0';
      if (d->dev.len == 0) d->dev.editing = 0;
      break;
    default: break;
  }
}

void tab_devices_text(Desk *d, int ch) {
  if (ch == '[' || ch == ']') { d->dev.sub = (d->dev.sub + (ch == ']' ? 1 : DEV_COUNT - 1)) % DEV_COUNT; stop_entry(d); return; }
  if (d->dev.sub == DEV_KEYS && ch != ' ') return;      /* on the KEYS page the letters and digits are notes (played from key events), not typing */
  if (ch >= '0' && ch <= '9') {
    if (d->dev.len < 5) { d->dev.buf[d->dev.len++] = (char)ch; d->dev.buf[d->dev.len] = '\0'; d->dev.editing = 1; }
  } else if (ch == ' ') { if (!d->dev.editing) action(d); }
  else if (d->dev.sub == DEV_CELLULAR && (ch == 'n' || ch == 'N')) fez_reg_write(d->synth, REG_CAROW, rand() & 0xffff);
  else if (d->dev.sub == DEV_CELLULAR && (ch == 'c' || ch == 'C')) fez_reg_write(d->synth, REG_CAROW, 1 << 8);
  else if (d->dev.sub == DEV_LORENZ && (ch == 'r' || ch == 'R')) action(d);
  else if (d->dev.sub == DEV_TURING && (ch == 'n' || ch == 'N')) fez_reg_write(d->synth, REG_TMBITS, (int16_t)(rand() & ((1 << fez_reg_read(d->synth, REG_TMLEN)) - 1)));   /* only the bits the loop uses. ponytail: libc rand, as above */
}
