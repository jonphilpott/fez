/* The PULSERS tab: the four clocks, what each one runs, and how fast.

   A pulser is a timer. Each time it fires it runs every block that has its bit set, and it can also clock the
   sequencer (SEQ) or start a grain (GRN). Each pulser is a card of three rows:

     P0 [ON ] [=====-----] 120 BPM  [SEQ] [GRN]   <- controls (the dot lights when it fires)
     blocks 0 1 2 3 4 5 6 7 8 9 A B C D E F       <- the blocks it runs (lit = runs)
     running  every 0.50 s  runs B0 B3 SEQ         <- a summary

   A cursor sits on one cell. Up/Down change pulser; Left/Right and Tab/Shift+Tab move between cells.
     Space or Enter   toggle the cell (on/off, SEQ, GRN, or a block)
     Enter on the rate slider   adjust it: Left/Right then change the rate by 5 BPM (Shift: 40), so the cursor
                      cannot get stuck on the slider. Enter or Esc stops adjusting, as does Up/Down or Tab.
     digits           type a rate in BPM (10 to 600) from anywhere, Enter to store, Esc to cancel
     + and -          step the rate by 5          F   fire the pulser now */
#include "desk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { F_ON, F_RATE, F_SEQ, F_GRN, F_BLOCK0, FIELD_COUNT = F_BLOCK0 + NUM_BLOCKS };

#define RATE_MIN 10
#define RATE_MAX 600
#define FIRE_LIGHT_TICKS 40          /* the firing light stays lit for 40 ticks of 5 ms: 200 ms */

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

static void set_rate(Desk *d, int row, int rate) { d->synth->pulsers[row].rate = clampi(rate, RATE_MIN, RATE_MAX); }

/* ---------------------------------------------------------------- drawing */

void tab_pulsers_draw(Desk *d, View *v) {
  FezSynth *s = d->synth;
  int visible = (v->rows - 2) / 3;                            /* cards that fit: three rows each, under a header row */
  if (visible < 1) return;
  if (visible > NUM_PULSERS) visible = NUM_PULSERS;
  int first = clampi(d->pul.row - visible / 2, 0, NUM_PULSERS - visible);

  view_text(v, 0, 0, "PULSERS", COL_AMB);
  view_text_right(v, 10, v->cols - 10, 0, d->handheld ? (d->pulsers_on ? "all on" : "all OFF") : d->pulsers_on ? "all on (Ctrl+P)" : "all OFF (Ctrl+P)", d->pulsers_on ? COL_DIM : COL_RED);

  for (int k = 0; k < visible; k++) {
    int p = first + k, r0 = 1 + k * 3, sel = p == d->pul.row;
    char buf[48];
    int on = s->pulsers[p].on, rate = (int)(s->pulsers[p].rate + 0.5);
    int cur_field = sel ? d->pul.field : -1;
    int sc = d->scale;

    if (sel) view_fill(v, 0, r0, v->cols - 1, 3, COL_PNL);

    /* controls row: name and firing light, ON, rate, period, SEQ, GRN */
    snprintf(buf, sizeof buf, "P%d", p);
    view_text(v, 0, r0, buf, sel ? COL_AMB : COL_FG);
    unsigned long since = s->tick_count - s->pulsers[p].last_fire;      /* ticks since it fired (last_fire 0 = never) */
    double light = s->pulsers[p].last_fire && since < FIRE_LIGHT_TICKS ? 1.0 - (double)since / FIRE_LIGHT_TICKS : 0.0;
    gfx_fill(&d->cv, v->x + 3 * v->cw, v->y + r0 * v->rh + 5 * sc, 4 * sc, 4 * sc, gfx_mix(gfx_colour(COL_SEL), gfx_colour(COL_MAG), light));

    view_cell(v, 5, r0, 4, on ? "ON" : "off", on ? COL_GRN : COL_DIM, cur_field == F_ON ? COL_AMBD : COL_PNL, cur_field == F_ON);
    /* the rate: a slider across RATE_MIN..RATE_MAX, then the number (or the number being typed) */
    int adjusting = sel && d->pul.adjusting && cur_field == F_RATE;
    view_cell(v, 10, r0, 18, "", COL_FG, cur_field == F_RATE ? COL_AMBD : COL_PNL, cur_field == F_RATE);
    if (adjusting)                                                   /* a brighter fill: the slider is live */
      gfx_fill(&d->cv, v->x + 10 * v->cw + 1, v->y + r0 * v->rh + sc + 1, 18 * v->cw - 2 * sc - 2, v->rh - 2 * sc - 2, gfx_mix(gfx_colour(COL_AMBD), gfx_colour(COL_AMB), 0.45));
    {
      int bx = v->x + 10 * v->cw + 4 * sc, bw = 9 * v->cw, by = v->y + r0 * v->rh + v->rh / 2 - sc;
      double frac = (double)(rate - RATE_MIN) / (RATE_MAX - RATE_MIN);
      frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
      gfx_fill(&d->cv, bx, by, bw, 2 * sc, gfx_colour(COL_SEL));                              /* the track */
      gfx_fill(&d->cv, bx, by, (int)(frac * bw), 2 * sc, gfx_colour(adjusting ? COL_GRN : on ? COL_AMB : COL_DIM)); /* filled up to the value */
      gfx_fill(&d->cv, bx + (int)(frac * (bw - 3 * sc)), by - 3 * sc, 3 * sc, 8 * sc, gfx_colour(cur_field == F_RATE ? COL_AMB : COL_FG));   /* the handle */
    }
    if (sel && d->pul.editing) snprintf(buf, sizeof buf, "%s_", d->pul.buf); else snprintf(buf, sizeof buf, "%d BPM", rate);
    view_text_right(v, 20, 7, r0, buf, sel && d->pul.editing ? COL_AMB : COL_FG);
    int sq = (s->mem[REG_SQPUL] >> p) & 1, gr = (s->mem[REG_GRPUL] >> p) & 1;
    view_cell(v, 29, r0, 4, "SEQ", sq ? COL_GRN : COL_DIM, cur_field == F_SEQ ? COL_AMBD : COL_PNL, cur_field == F_SEQ);
    view_cell(v, 34, r0, 4, "GRN", gr ? COL_GRN : COL_DIM, cur_field == F_GRN ? COL_AMBD : COL_PNL, cur_field == F_GRN);

    /* blocks row: lit cells are the blocks this pulser runs */
    view_text(v, 0, r0 + 1, "blocks", COL_DIM);
    for (int b = 0; b < NUM_BLOCKS; b++) {
      int runs = (s->mem[REG_BP0 + b] >> p) & 1, here = cur_field == F_BLOCK0 + b;
      char lab[2] = { "0123456789ABCDEF"[b], '\0' };
      view_cell(v, 7 + b * 2, r0 + 1, 2, lab, runs ? COL_BG : COL_DIM, runs ? COL_MAG : (here ? COL_AMBD : COL_PNL), here);
    }

    /* summary row */
    size_t l = (size_t)snprintf(buf, sizeof buf, "%s  every %.2f s  runs", on ? "running" : "stopped", 60.0 / s->pulsers[p].rate);
    int any = 0;
    for (int b = 0; b < NUM_BLOCKS && l < sizeof buf - 6; b++) if ((s->mem[REG_BP0 + b] >> p) & 1) { l += (size_t)snprintf(buf + l, sizeof buf - l, " B%X", b); any = 1; }
    if (sq && l < sizeof buf - 6) { l += (size_t)snprintf(buf + l, sizeof buf - l, " SEQ"); any = 1; }
    if (gr && l < sizeof buf - 6) { l += (size_t)snprintf(buf + l, sizeof buf - l, " GRN"); any = 1; }
    if (!any) snprintf(buf + l, sizeof buf - l, " nothing");
    view_text(v, 0, r0 + 2, buf, COL_DIM);
    gfx_fill(&d->cv, v->x, v->y + (r0 + 3) * v->rh - 2, v->w, 1, gfx_colour(COL_LINE));
  }
}

/* ---------------------------------------------------------------- keys */

static void toggle_field(Desk *d) {
  FezSynth *s = d->synth;
  int p = d->pul.row, f = d->pul.field;
  if (f == F_ON || f == F_RATE) s->pulsers[p].on = !s->pulsers[p].on;
  else if (f == F_SEQ) s->mem[REG_SQPUL] ^= (int16_t)(1 << p);
  else if (f == F_GRN) s->mem[REG_GRPUL] ^= (int16_t)(1 << p);
  else s->mem[REG_BP0 + f - F_BLOCK0] ^= (int16_t)(1 << p);
}

static void step_rate(Desk *d, int dir, int coarse) {
  int p = d->pul.row;
  set_rate(d, p, (int)(d->synth->pulsers[p].rate + 0.5) + dir * (coarse ? 40 : 5));
}

static void stop_entry(Desk *d) { d->pul.editing = 0; d->pul.len = 0; d->pul.adjusting = 0; }

static void move_field(Desk *d, int dir) {
  d->pul.field = (d->pul.field + dir + FIELD_COUNT) % FIELD_COUNT;
  stop_entry(d);
}

void tab_pulsers_key(Desk *d, int key, int mods) {
  int p = d->pul.row;
  switch (key) {
    case KEY_UP:    d->pul.row = (p + NUM_PULSERS - 1) % NUM_PULSERS; stop_entry(d); break;
    case KEY_DOWN:  d->pul.row = (p + 1) % NUM_PULSERS; stop_entry(d); break;
    case KEY_LEFT: case KEY_RIGHT: {
      int dir = key == KEY_RIGHT ? 1 : -1;
      if (d->pul.adjusting) step_rate(d, dir, mods & MOD_SHIFT);       /* only while the slider is being adjusted */
      else move_field(d, dir);
      break;
    }
    case KEY_TAB: move_field(d, mods & MOD_SHIFT ? -1 : 1); break;
    case KEY_ENTER:
      if (d->pul.editing) { set_rate(d, p, atoi(d->pul.buf)); stop_entry(d); }                 /* store a typed rate */
      else if (d->pul.field == F_RATE) {                                                     /* start or stop adjusting the slider */
        d->pul.adjusting = !d->pul.adjusting;
        desk_say(d, d->pul.adjusting ? (d->handheld ? "ADJUSTING: Left/Right, A when done" : "ADJUSTING: Left/Right change, Enter when done") : "SLIDER RELEASED");
      } else toggle_field(d);
      break;
    case KEY_ESC: stop_entry(d); break;
    case KEY_BACKSPACE:
      if (d->pul.len > 0) d->pul.buf[--d->pul.len] = '\0';
      if (d->pul.len == 0) d->pul.editing = 0;
      break;
    default: break;
  }
}

void tab_pulsers_text(Desk *d, int ch) {
  int p = d->pul.row;
  if (ch >= '0' && ch <= '9') {                               /* typing a number goes to the rate, wherever the cursor is */
    if (d->pul.len < 3) { d->pul.buf[d->pul.len++] = (char)ch; d->pul.buf[d->pul.len] = '\0'; d->pul.editing = 1; d->pul.adjusting = 0; d->pul.field = F_RATE; }
  } else if (ch == ' ') {
    if (d->pul.editing) return;                               /* the space key does not end an entry; Enter does */
    toggle_field(d);
  } else if (ch == '+' || ch == '=') step_rate(d, 1, 0);
  else if (ch == '-' || ch == '_') step_rate(d, -1, 0);
  else if (ch == 'f' || ch == 'F') d->synth->vm.pending |= 1u << p;           /* fire on the next tick, as TRIG does */
}
