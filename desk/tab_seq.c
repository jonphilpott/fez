/* The SEQ tab: the eight-step sequencer.

   Each step holds a pitch and can be switched on or off. Every clock (a pulser with its SEQ bit set, or you)
   moves to the next enabled step inside the loop length and then, by the drive bits, writes that step's pitch to
   PITCH and/or fires the envelope. The registers behind it are SQ0..SQ7 (the pitches), SQEN, SQLEN, SQDRV.

   One row per step: its number, an ON cell, the note, a bar, and the raw register value (8.8 semitones: C4 is 15360).

   Keys:
     Up/Down         pick a step
     Left/Right      move its value in fine steps (1/16 semitone); Shift snaps to the next note below or above
     Space           switch the step on or off
     a to g          start typing a note: a letter, then # or b, then an octave number (c#3, bb2, g); a bare
                     letter keeps the step's octave. Enter stores it, Esc cancels.
     Enter           clock the sequencer once now        R   go back to the start (the next clock plays step 1)
     + and -         steps in the loop (1 to 8)          P   clock writes PITCH on/off     T   clock fires the gate on/off */
#include "desk.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define NSTEPS 8
#define STEP0  REG_SQ0           /* the first step register; SQ1.. follow */
#define PITCH_MIN 6144           /* the range the steps accept: note 24 to note 102 */
#define PITCH_MAX 26112
#define DISPLAY_LOW  36          /* the bars span notes 36 to 84 */
#define DISPLAY_SPAN 48

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

static void cancel_entry(Desk *d) { d->seq.editing = 0; d->seq.len = 0; d->seq.buf[0] = '\0'; }

/* ---------------------------------------------------------------- drawing */

void tab_seq_draw(Desk *d, View *v) {
  FezSynth *s = d->synth;
  int en = s->mem[REG_SQEN], len = clampi(s->mem[REG_SQLEN], 1, NSTEPS), drv = s->mem[REG_SQDRV];
  int sc = d->scale;
  char buf[96];

  /* 1. title, and who is clocking */
  view_text(v, 0, 0, "STEP SEQUENCER", COL_AMB);
  char who[64] = ""; size_t wl = 0;
  for (int p = 0; p < NUM_PULSERS; p++)
    if (((s->mem[REG_SQPUL] >> p) & 1) && s->pulsers[p].on) wl += (size_t)snprintf(who + wl, sizeof who - wl, "%sP%d %d BPM", wl ? ", " : "", p, (int)(s->pulsers[p].rate + 0.5));
  if (wl) { snprintf(buf, sizeof buf, "clocked by %s", who); view_text_right(v, 15, v->cols - 15, 0, buf, COL_GRN); }
  else view_text_right(v, 15, v->cols - 15, 0, "no clock: set SEQ on a pulser", COL_RED);

  /* 2. one row per step. Rows are two text rows tall when the tile is tall enough. */
  int srows = (v->rows - 4) / NSTEPS;
  srows = srows < 1 ? 1 : srows > 2 ? 2 : srows;
  const int COL_ON = 3, COL_NOTE = 8, COL_BAR = 15, VAL_W = 6;
  int bar_cols = v->cols - COL_BAR - VAL_W - 2;
  for (int i = 0; i < NSTEPS; i++) {
    int row = 1 + i * srows, y = v->y + row * v->rh, h = srows * v->rh - sc, x = v->x, w = v->w - 2 * sc;
    int cur = s->sq_count > 0 && s->sq_cur == i, sel = i == d->seq.sel;
    int on = (en >> i) & 1, dim = i >= len, value = s->mem[STEP0 + i];
    int ty = y + (h - 8 * sc) / 2;                              /* text, centred in the row */

    gfx_fill(&d->cv, x, y, w, h, gfx_colour(cur ? COL_AMBD : COL_PNL));
    if (sel || cur) gfx_frame(&d->cv, x, y, w, h, gfx_colour(sel ? COL_CYN : COL_AMB));

    snprintf(buf, sizeof buf, "%d", i + 1);
    gfx_text(&d->cv, x + 2 * v->cw - (int)strlen(buf) * GFX_CELL_W * sc, ty, buf, sc, gfx_colour(dim ? COL_DIM : cur ? COL_AMB : COL_FG));

    /* ON cell: a full text row high, so the glyphs fit inside it */
    const char *label = dim ? "--" : on ? "ON" : "off";
    int cx = x + COL_ON * v->cw, cw = 4 * v->cw - 2 * sc, cy = y + (h - v->rh) / 2;
    if (!dim) gfx_fill(&d->cv, cx, cy, cw, v->rh, gfx_colour(on ? COL_GRN : COL_SEL));
    gfx_text(&d->cv, cx + (cw - (int)strlen(label) * GFX_CELL_W * sc) / 2, cy + (v->rh - 8 * sc) / 2, label, sc, gfx_colour(dim ? COL_DIM : on ? COL_BG : COL_DIM));

    /* the note (or the one being typed) */
    if (sel && d->seq.editing) { snprintf(buf, sizeof buf, "%s_", d->seq.buf); gfx_text(&d->cv, x + COL_NOTE * v->cw, ty, buf, sc, gfx_colour(COL_AMB)); }
    else {
      char name[24];
      regview_fmt_note(name, sizeof name, value);
      gfx_text(&d->cv, x + COL_NOTE * v->cw, ty, name, sc, gfx_colour(dim ? COL_DIM : COL_FG));
    }

    /* the bar: the note's position within the display range, then the raw value at the right */
    if (bar_cols >= 4) {
      int bx = x + COL_BAR * v->cw, bw = bar_cols * v->cw, bh = h / 2, by = y + (h - bh) / 2;
      double fr = (value / 256.0 - DISPLAY_LOW) / DISPLAY_SPAN;
      fr = fr < 0 ? 0 : fr > 1 ? 1 : fr;
      gfx_fill(&d->cv, bx, by, bw, bh, gfx_colour(COL_SEL));
      gfx_fill(&d->cv, bx, by, (int)(fr * bw + 0.5), bh, gfx_colour(dim || !on ? COL_DIM : cur ? COL_AMB : COL_CYN));
    }
    snprintf(buf, sizeof buf, "%d", value);
    gfx_text(&d->cv, x + (v->cols - 1) * v->cw - (int)strlen(buf) * GFX_CELL_W * sc, ty, buf, sc, gfx_colour(dim ? COL_DIM : COL_FG));
  }

  /* 3. the loop settings */
  int r = 1 + NSTEPS * srows + 1;
  if (r < v->rows) {
    snprintf(buf, sizeof buf, "steps %d   clock does: %s%s%s", len, drv & 1 ? "PITCH " : "", drv & 2 ? "GATE" : "", drv ? "" : "nothing (code reads SQOUT)");
    view_text(v, 0, r, buf, COL_FG);
  }
  if (r + 1 < v->rows) view_text(v, 0, r + 1, "P pitch  T gate  +/- steps  R restart  Enter clock", COL_DIM);
}

/* ---------------------------------------------------------------- keys */

/* Left/Right move the raw pitch value (8.8 semitones, so a step of 16 is 6 cents). Shift snaps to a note instead: the
   next semitone strictly above or below, which is the neighbour if the step already sits on a note. */
#define FINE_STEP 16
static void nudge_step(Desk *d, int dir, int snap) {
  int i = d->seq.sel, v = d->synth->mem[STEP0 + i];
  if (snap) {
    int semis = v / 256, rest = v % 256;                       /* whole semitones, and how far past them (never negative: v >= 0) */
    v = dir > 0 ? (semis + 1) * 256 : (rest ? semis : semis - 1) * 256;
  } else v += dir * FINE_STEP;
  d->synth->mem[STEP0 + i] = (int16_t)clampi(v, PITCH_MIN, PITCH_MAX);
}

static void commit_entry(Desk *d) {
  int i = d->seq.sel, pitch;
  int cur_octave = d->synth->mem[STEP0 + i] / 3072 - 1;       /* 3072 = an octave in 8.8 semitones; C4 = 15360 -> octave 4 */
  if (regview_parse_note(d->seq.buf, cur_octave, &pitch)) d->synth->mem[STEP0 + i] = (int16_t)clampi(pitch, PITCH_MIN, PITCH_MAX);
  else desk_say(d, "NOT A NOTE (try c#3)");
  cancel_entry(d);
}

void tab_seq_key(Desk *d, int key, int mods) {
  FezSynth *s = d->synth;
  switch (key) {
    case KEY_UP:    cancel_entry(d); d->seq.sel = (d->seq.sel + NSTEPS - 1) % NSTEPS; break;
    case KEY_DOWN:  cancel_entry(d); d->seq.sel = (d->seq.sel + 1) % NSTEPS; break;
    case KEY_LEFT:  cancel_entry(d); nudge_step(d, -1, mods & MOD_SHIFT); break;    /* Shift: snap to a note */
    case KEY_RIGHT: cancel_entry(d); nudge_step(d, 1, mods & MOD_SHIFT); break;
    case KEY_ENTER:
      if (d->seq.editing) commit_entry(d); else fez_seq_clock(s);
      break;
    case KEY_ESC: cancel_entry(d); break;
    case KEY_BACKSPACE:
      if (d->seq.len > 0) d->seq.buf[--d->seq.len] = '\0';
      if (d->seq.len == 0) d->seq.editing = 0;
      break;
    default: break;
  }
}

void tab_seq_text(Desk *d, int ch) {
  FezSynth *s = d->synth;
  if (d->seq.editing) {                                        /* while typing a note, letters belong to the note */
    if (d->seq.len < 5 && (isalnum(ch) || ch == '#' || ch == '-')) { d->seq.buf[d->seq.len++] = (char)ch; d->seq.buf[d->seq.len] = '\0'; }
    return;
  }
  int lower = tolower(ch);
  if (lower >= 'a' && lower <= 'g') { d->seq.editing = 1; d->seq.buf[0] = (char)lower; d->seq.buf[1] = '\0'; d->seq.len = 1; return; }
  switch (lower) {
    case ' ': s->mem[REG_SQEN] ^= (int16_t)(1 << d->seq.sel); break;
    case '+': case '=': s->mem[REG_SQLEN] = (int16_t)clampi(s->mem[REG_SQLEN] + 1, 1, NSTEPS); break;
    case '-': case '_': s->mem[REG_SQLEN] = (int16_t)clampi(s->mem[REG_SQLEN] - 1, 1, NSTEPS); break;
    case 'p': s->mem[REG_SQDRV] ^= 1; break;
    case 't': s->mem[REG_SQDRV] ^= 2; break;
    case 'r': s->sq_cur = NSTEPS - 1; desk_say(d, "RESTARTED"); break;   /* the next clock lands on step 1 */
    default: break;
  }
}
