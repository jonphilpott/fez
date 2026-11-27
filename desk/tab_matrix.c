/* The MATRIX tab: the modulation routes.

   A route says "add (weight / 32768) x SOURCE to DESTINATION". The source is one of 34 moving values (LFO0, ENV, CV0..3,
   the pots, the automaton, the Lorenz axes, the sequencer ...); the destination is any parameter register. Both are
   picked from lists grouped and explained like the CTRL tab. Up to 16
   routes. The voice hears the register plus everything routed to it; the register itself never changes.

     #  SOURCE  DEST      WEIGHT  [bar centred on zero]     NOW
     1  LFO0    CUTOFF      4000  ----|=======-------      +812

   NOW is what the route is adding at this moment, in the destination's own units.

   Keys:
     Up/Down        pick a route (the last row, "+", adds one)    Left/Right   move between the three cells
     Enter          on SOURCE or DEST: pick from a list (type to filter, Enter chooses, Esc cancels)
                    on WEIGHT: adjust it (Left/Right step by 256, Shift 4096; Enter or Esc to stop), so the cursor cannot get stuck
                    on "+": add a route, then pick its source and destination
     digits, -      type a weight from anywhere on the WEIGHT cell, Enter to store
     Backspace or Delete   delete the route */
#include "desk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST_MAX 80                     /* the longest pick list: 34 sources, 66 parameters */
#define WEIGHT_STEP 256
#define WEIGHT_COARSE 4096


/* The rows of the screen: every route, plus a "+" row while there is room for another. */
static int row_count(const Desk *d) { return d->synth->nroutes + (d->synth->nroutes < MAX_ROUTES ? 1 : 0); }

static const char *dst_name(const Desk *d, int addr) {
  return addr >= 0 && addr < NUM_REGS && d->synth->param_index[addr] >= 0 ? regview_param_name(d->synth, addr) : "?";
}

/* ---- pick lists. A list is the sources (1..34) or the parameters; each entry is a source number or a register address.
   Both are shown like the CTRL tab: under group headings, with a help line for the highlighted entry. The sources keep
   SRC_TABLE's order and its groups; the parameters are put in address order, so they fall under the same headings as on
   CTRL (REG_GROUPS). */

static int sorted_pars(const Desk *d, int *out) {            /* the parameters in address order (a plain insertion sort) */
  int n = d->npars;
  for (int i = 0; i < n; i++) {
    int a = d->pars[i], k = i;
    while (k > 0 && out[k - 1] > a) { out[k] = out[k - 1]; k--; }
    out[k] = a;
  }
  return n;
}

static int pick_item_count(const Desk *d) { return d->mtx.mode == MTX_PICK_SRC ? SRC_COUNT - 1 : d->npars; }
static int pick_item(const Desk *d, int i) {
  if (d->mtx.mode == MTX_PICK_SRC) return i + 1;
  int pars[LIST_MAX]; sorted_pars(d, pars); return pars[i];
}
static const char *pick_item_name(const Desk *d, int i) { return d->mtx.mode == MTX_PICK_SRC ? SRC_TABLE[i + 1].name : dst_name(d, pick_item(d, i)); }

/* The heading an entry sits under, and what it is for. */
static const char *pick_item_group(const Desk *d, int i) {
  if (d->mtx.mode == MTX_PICK_SRC) return SRC_TABLE[i + 1].group;
  int a = pick_item(d, i), g = 0;
  for (int k = 0; k < REG_GROUP_COUNT; k++) if (REG_GROUPS[k].first <= a) g = k;
  return REG_GROUPS[g].name;
}
static const char *pick_item_help(const Desk *d, int i) {
  return d->mtx.mode == MTX_PICK_SRC ? SRC_TABLE[i + 1].help : regview_help(pick_item_name(d, i));
}

/* The entries matching the typed filter, as indexes into the full list. */
static int pick_filtered(const Desk *d, int *out) {
  int n = 0;
  for (int i = 0; i < pick_item_count(d) && n < LIST_MAX; i++)
    if (desk_contains_nocase(pick_item_name(d, i), d->mtx.filter)) out[n++] = i;
  return n;
}

/* ---------------------------------------------------------------- drawing */

/* The pick list on screen, as CTRL draws its registers:

     PICK A DESTINATION                 filter: _
     VOICE ------------------------------------
     PITCH        C4
     CUTOFF       4.2k          <- the highlighted entry
     ...
     ------------------------------------------
     Lowpass gate cutoff, 40 Hz to 14 kHz          <- its help (two lines; a longer one scrolls)
     Up/Down  Enter choose  Esc cancel

   Destinations show their value now (in CTRL's units); sources show what they are doing now, as a bar centred on zero.
   Headings are rows of their own and are never selected: the selection moves over the entries, as before. */
static void draw_weight_bar(Desk *d, View *v, int col, int cols, int row, int w, int live);

static void draw_pick(Desk *d, View *v) {
  FezSynth *s = d->synth;
  int list[LIST_MAX], n = pick_filtered(d, list);
  if (d->mtx.psel >= n) d->mtx.psel = n ? n - 1 : 0;
  int first = d->handheld ? 1 : 2;                           /* the list's first row: under the title and (desktop) the filter line */
  int vis = v->rows - 3 - first;                             /* below it: a rule, two help lines and the keys */
  if (vis < 1) return;

  /* 1) the rows: an entry is its position in the filtered list (>= 0); a heading is -(that entry's position + 1),
        placed before the first entry of each group */
  static int rows[2 * LIST_MAX]; int nr = 0, sel_row = 0;
  for (int k = 0; k < n; k++) {
    if (k == 0 || strcmp(pick_item_group(d, list[k]), pick_item_group(d, list[k - 1])) != 0) rows[nr++] = -(k + 1);
    if (k == d->mtx.psel) sel_row = nr;
    rows[nr++] = k;
  }
  int top = fez_clamp(sel_row - vis / 2, 0, nr > vis ? nr - vis : 0);

  /* 2) title and filter */
  char buf[64];
  view_text(v, 0, 0, d->mtx.mode == MTX_PICK_SRC ? "PICK A SOURCE" : "PICK A DESTINATION", COL_AMB);
  snprintf(buf, sizeof buf, d->mtx.flen ? "filter: %s_" : d->handheld ? "%s" : "filter: _", d->mtx.filter);   /* the empty field looks like one, not like a list entry */
  if (!d->handheld) view_text(v, 0, 1, buf, d->mtx.flen ? COL_CYN : COL_DIM);   /* (the handheld cannot type) */

  /* 3) the list */
  const int VAL_COL = 11;
  int bar_cols = v->cols - VAL_COL - 2;
  if (bar_cols > 20) bar_cols = 20;
  for (int r = 0; r < vis && top + r < nr; r++) {
    int code = rows[top + r], row = first + r;
    if (code < 0) {                                           /* a heading, with a rule along to the edge */
      const char *name = pick_item_group(d, list[-code - 1]);
      int len = (int)strlen(name);
      view_text(v, 0, row, name, COL_MAG);
      gfx_fill(&d->cv, v->x + (len + 1) * v->cw, v->y + row * v->rh + v->rh / 2, v->w - (len + 1) * v->cw - 6, 1, gfx_colour(COL_LINE));
      continue;
    }
    int item = list[code], sel = code == d->mtx.psel;
    if (sel) view_fill(v, 0, row, v->cols - 1, 1, COL_SEL);
    view_text(v, 0, row, pick_item_name(d, item), sel ? COL_AMB : COL_FG);
    if (d->mtx.mode == MTX_PICK_SRC) {                       /* what the source is doing now */
      if (bar_cols >= 4) draw_weight_bar(d, v, VAL_COL, bar_cols, row, fez_matrix_source(s, item + 1), 1);
    } else {                                                  /* the parameter's value now, as CTRL shows it */
      int a = pick_item(d, item), val = regview_read(s, a);
      if (a == REG_MDPITCH && s->mem[REG_MDTRACK] >= 16384) regview_fmt_interval(buf, sizeof buf, val);
      else if (!regview_fmt_grain(buf, sizeof buf, s, a, val)) regview_fmt_param(buf, sizeof buf, a, val);
      view_text(v, VAL_COL, row, buf, sel ? COL_AMB : COL_DIM);
    }
  }
  if (!n) view_text(v, 0, first, "no match", COL_DIM);
  if (nr > vis) view_scrollbar(v, top, vis, nr, first, vis);

  /* 4) the highlighted entry's help under a rule (two lines; a longer one scrolls), then the keys */
  gfx_fill(&d->cv, v->x, v->y + (v->rows - 3) * v->rh - d->scale, v->w, 1, gfx_colour(COL_LINE));   /* just above the help, clear of the last entry's letters */
  if (n) {
    int key = d->mtx.mode * 1000 + list[d->mtx.psel];
    if (key != d->mtx.help_key) { d->mtx.help_key = key; d->mtx.help_since = d->now_ms; }
    static char lines[8][64]; int colours[8];
    int nl = wrap_text(pick_item_help(d, list[d->mtx.psel]), v->cols - 2, lines, 8);
    for (int i = 0; i < nl; i++) colours[i] = COL_FG;
    view_lines_scroll(v, 0, v->rows - 3, 2, lines, colours, nl, d->mtx.help_since);
  }
  view_text(v, 0, v->rows - 1, d->handheld ? "Up/Down  A choose  B cancel" : "Up/Down  type to filter  Enter choose  Esc cancel", COL_DIM);
}

/* A weight bar centred on zero: it fills right (magenta, when live) for positive, left (red) for negative. */
static void draw_weight_bar(Desk *d, View *v, int col, int cols, int row, int w, int live) {
  int sc = d->scale, x = v->x + col * v->cw, wpx = cols * v->cw - 2 * sc, mid = x + wpx / 2, y = v->y + row * v->rh + v->rh / 2 - 2 * sc;
  int len = (int)((double)w / 32768.0 * (wpx / 2));
  gfx_fill(&d->cv, x, y, wpx, 4 * sc, gfx_colour(COL_SEL));
  if (len >= 0) gfx_fill(&d->cv, mid, y, len, 4 * sc, gfx_colour(live ? COL_MAG : COL_DIM));
  else gfx_fill(&d->cv, mid + len, y, -len, 4 * sc, gfx_colour(live ? COL_RED : COL_DIM));
  gfx_fill(&d->cv, mid, y - sc, sc, 6 * sc, gfx_colour(COL_FG));                              /* the zero mark */
}

void tab_matrix_draw(Desk *d, View *v) {
  FezSynth *s = d->synth;
  if (d->mtx.mode == MTX_PICK_SRC || d->mtx.mode == MTX_PICK_DST) { draw_pick(d, v); return; }

  int total = row_count(d), vis = v->rows - 2 - 2;                 /* title and column heads above; two help lines below */
  if (vis < 3) vis = 3;
  if (vis > total) vis = total;
  int top = fez_clamp(d->mtx.row - vis / 2, 0, total - vis);
  char buf[48];

  snprintf(buf, sizeof buf, "%d of %d routes", s->nroutes, MAX_ROUTES);
  view_text(v, 0, 0, "MODULATION MATRIX", COL_AMB);
  view_text_right(v, 18, v->cols - 18, 0, buf, COL_DIM);
  view_text(v, 1, 1, "#", COL_DIM);                                 /* column heads, lined up with the cells below */
  view_text(v, 3, 1, "SOURCE", COL_DIM);
  view_text(v, 11, 1, "DEST", COL_DIM);
  view_text_right(v, 20, 6, 1, "WEIGHT", COL_DIM);
  view_text_right(v, v->cols - 8, 7, 1, "NOW", COL_DIM);

  const int C_SRC = 3, C_DST = 11, C_W = 20, W_WIDTH = 6, BAR_COL = 27;
  int bar_cols = v->cols - 8 - BAR_COL - 1;
  for (int r = 0; r < vis; r++) {
    int i = top + r, row = 2 + r, sel = i == d->mtx.row;
    if (sel) view_fill(v, 0, row, v->cols - 1, 1, COL_PNL);
    if (i >= s->nroutes) { view_text(v, 3, row, "+ add a route", sel ? COL_AMB : COL_DIM); continue; }

    const RouteDef *rt = &s->routes[i];
    int cur = sel ? d->mtx.cell : -1;
    snprintf(buf, sizeof buf, "%2d", i + 1);
    view_text(v, 0, row, buf, COL_DIM);
    if (cur == 0) view_fill(v, C_SRC, row, 7, 1, COL_AMBD);
    view_text(v, C_SRC, row, SRC_TABLE[fez_clamp(rt->src, 0, SRC_COUNT - 1)].name, cur == 0 ? COL_AMB : COL_FG);
    if (cur == 1) view_fill(v, C_DST, row, 8, 1, COL_AMBD);
    view_text(v, C_DST, row, dst_name(d, rt->dst), cur == 1 ? COL_AMB : COL_FG);

    int adjusting = cur == 2 && d->mtx.mode == MTX_ADJUST, typing = cur == 2 && d->mtx.mode == MTX_TYPE;
    if (cur == 2) view_fill(v, C_W, row, W_WIDTH + 1, 1, adjusting ? COL_GRN : COL_AMBD);
    if (typing) snprintf(buf, sizeof buf, "%s_", d->mtx.in.buf); else snprintf(buf, sizeof buf, "%d", rt->w);
    view_text_right(v, C_W, W_WIDTH, row, buf, adjusting ? COL_BG : cur == 2 ? COL_AMB : COL_FG);

    int now = fez_round((double)rt->w * fez_matrix_source(s, rt->src) / 32768.0);
    if (bar_cols >= 4) draw_weight_bar(d, v, BAR_COL, bar_cols, row, rt->w, rt->src != 0);
    snprintf(buf, sizeof buf, "%+d", now);
    view_text_right(v, v->cols - 8, 7, row, buf, now == 0 ? COL_DIM : now > 0 ? COL_GRN : COL_RED);
  }
  if (total > vis) view_scrollbar(v, top, vis, total, 2, vis);

  /* two help lines: what the destination is, and what the cell under the cursor does */
  if (d->mtx.row < s->nroutes && v->rows >= 6) {
    view_text(v, 0, v->rows - 2, regview_help(dst_name(d, s->routes[d->mtx.row].dst)), COL_DIM);
    static const char *const CELL_HELP[3] = { "Enter: pick a source (what moves the target)", "Enter: pick the parameter it moves",
                                              "Enter adjusts, or type a number. 32768 = full swing" };
    static const char *const PAD_HELP[3] = { "A: pick a source (what moves the target)", "A: pick the parameter it moves",
                                             "A adjusts. 32768 = full swing" };
    view_text(v, 0, v->rows - 1, d->mtx.mode == MTX_ADJUST ? (d->handheld ? "ADJUSTING: Left/Right, Y coarse, A done" : "ADJUSTING: Left/Right, Shift coarse, Enter done")
                                                           : (d->handheld ? PAD_HELP : CELL_HELP)[d->mtx.cell], COL_DIM);
  } else if (v->rows >= 6) view_text(v, 0, v->rows - 1, d->handheld ? "A adds a route" : "Enter adds a route", COL_DIM);
}

/* ---------------------------------------------------------------- keys */

static void stop_entry(Desk *d) { d->mtx.mode = MTX_NORMAL; entry_clear(&d->mtx.in); }

static void delete_route(Desk *d, int i) {
  FezSynth *s = d->synth;
  for (int k = i; k < s->nroutes - 1; k++) s->routes[k] = s->routes[k + 1];
  s->nroutes--;
  d->mtx.row = fez_clamp(d->mtx.row, 0, row_count(d) - 1);
}

static void open_pick(Desk *d, int mode) {
  d->mtx.mode = mode; d->mtx.flen = 0; d->mtx.filter[0] = '\0'; d->mtx.psel = 0;
  int list[LIST_MAX], n = pick_filtered(d, list);
  const RouteDef *rt = &d->synth->routes[d->mtx.row];
  int now = mode == MTX_PICK_SRC ? rt->src : rt->dst;                  /* start on the current choice */
  for (int k = 0; k < n; k++) if (pick_item(d, list[k]) == now) d->mtx.psel = k;
}

static void pick_key(Desk *d, int key) {
  FezSynth *s = d->synth;
  int list[LIST_MAX], n = pick_filtered(d, list);
  switch (key) {
    case KEY_UP:   if (n) d->mtx.psel = (d->mtx.psel + n - 1) % n; break;
    case KEY_DOWN: if (n) d->mtx.psel = (d->mtx.psel + 1) % n; break;
    case KEY_BACKSPACE: if (d->mtx.flen > 0) { d->mtx.filter[--d->mtx.flen] = '\0'; d->mtx.psel = 0; } break;
    case KEY_ESC:
      d->mtx.mode = MTX_NORMAL;
      if (d->mtx.adding) { delete_route(d, d->mtx.row); d->mtx.adding = 0; }                 /* give up on the new route */
      break;
    case KEY_ENTER:
      if (!n) break;
      if (d->mtx.mode == MTX_PICK_SRC) {
        s->routes[d->mtx.row].src = pick_item(d, list[d->mtx.psel]);
        if (d->mtx.adding) open_pick(d, MTX_PICK_DST); else d->mtx.mode = MTX_NORMAL;     /* a new route goes on to its destination */
      } else {
        s->routes[d->mtx.row].dst = pick_item(d, list[d->mtx.psel]);
        d->mtx.mode = MTX_NORMAL;
        if (d->mtx.adding) { d->mtx.adding = 0; d->mtx.cell = 2; }                         /* ...then lands on the weight */
      }
      break;
    default: break;
  }
}

static void step_weight(Desk *d, int dir, int coarse) {
  RouteDef *rt = &d->synth->routes[d->mtx.row];
  rt->w = fez_clamp(rt->w + dir * (coarse ? WEIGHT_COARSE : WEIGHT_STEP), -32768, 32767);
}

void tab_matrix_key(Desk *d, int key, int mods) {
  FezSynth *s = d->synth;
  if (d->mtx.mode == MTX_PICK_SRC || d->mtx.mode == MTX_PICK_DST) { pick_key(d, key); return; }
  d->mtx.row = fez_clamp(d->mtx.row, 0, row_count(d) - 1);
  int on_route = d->mtx.row < s->nroutes;

  switch (key) {
    case KEY_UP: case KEY_DOWN: {
      int n = row_count(d);
      d->mtx.row = (d->mtx.row + n + (key == KEY_DOWN ? 1 : -1)) % n;
      stop_entry(d);
      break;
    }
    case KEY_LEFT: case KEY_RIGHT: {
      int dir = key == KEY_RIGHT ? 1 : -1;
      if (d->mtx.mode == MTX_ADJUST && on_route) step_weight(d, dir, mods & MOD_SHIFT);   /* only while adjusting */
      else { d->mtx.cell = fez_clamp(d->mtx.cell + dir, 0, 2); stop_entry(d); }
      break;
    }
    case KEY_ENTER:
      if (!on_route) {                                                  /* the "+" row: make a route and choose its source */
        s->routes[s->nroutes].src = 1; s->routes[s->nroutes].dst = d->pars[0]; s->routes[s->nroutes].w = 0;
        s->nroutes++;
        d->mtx.adding = 1; d->mtx.cell = 0;
        open_pick(d, MTX_PICK_SRC);
      }
      else if (d->mtx.mode == MTX_TYPE) { s->routes[d->mtx.row].w = fez_clamp(atoi(d->mtx.in.buf), -32768, 32767); stop_entry(d); }
      else if (d->mtx.cell == 0) open_pick(d, MTX_PICK_SRC);
      else if (d->mtx.cell == 1) open_pick(d, MTX_PICK_DST);
      else {                                                            /* weight: start or stop adjusting */
        d->mtx.mode = d->mtx.mode == MTX_ADJUST ? MTX_NORMAL : MTX_ADJUST;
        desk_say(d, d->mtx.mode == MTX_ADJUST ? (d->handheld ? "ADJUSTING: Left/Right, A when done" : "ADJUSTING: Left/Right change, Enter when done") : "WEIGHT RELEASED");
      }
      break;
    case KEY_ESC: stop_entry(d); break;
    case KEY_BACKSPACE:
      if (d->mtx.mode == MTX_TYPE) { entry_backspace(&d->mtx.in); if (!d->mtx.in.editing) stop_entry(d); }
      else if (on_route) { delete_route(d, d->mtx.row); stop_entry(d); desk_say(d, "ROUTE DELETED"); }
      break;
    case KEY_DELETE:
      if (on_route) { delete_route(d, d->mtx.row); stop_entry(d); desk_say(d, "ROUTE DELETED"); }
      break;
    default: break;
  }
}

/* Typed characters: the pick list's filter, or a weight (digits and a leading minus) on the WEIGHT cell. */
void tab_matrix_text(Desk *d, int ch) {
  if (d->mtx.mode == MTX_PICK_SRC || d->mtx.mode == MTX_PICK_DST) {
    if (ch >= 32 && ch <= 126 && d->mtx.flen < (int)sizeof d->mtx.filter - 1) { d->mtx.filter[d->mtx.flen++] = (char)ch; d->mtx.filter[d->mtx.flen] = '\0'; d->mtx.psel = 0; }
    return;
  }
  if (d->mtx.row >= d->synth->nroutes || d->mtx.cell != 2) return;
  int minus = ch == '-' && d->mtx.in.len == 0;
  if ((ch >= '0' && ch <= '9') || minus) {
    if (d->mtx.mode != MTX_TYPE) entry_clear(&d->mtx.in);
    entry_add(&d->mtx.in, ch, 6); d->mtx.mode = MTX_TYPE;            /* 6 characters: weights run -32768..32767 */
  }
}
