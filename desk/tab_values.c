/* The CTRL tab: every register in one scrolling list, grouped under headings (MASTER, VOICE, MIXER, DELAY, BUFFER ...).
   Left/Right steps the selected register, typing a digit starts an entry (shown in amber with a cursor); Enter stores it,
   Esc drops it. Tab switches the whole list between two ways of reading it:
     UNITS  parameters in human terms (Hz, ms, note names, percent), with a "now" column where the modulation matrix moves
            the value away from what you set. Left/Right steps are musical (a semitone for PITCH, a fixed fraction of the range
            for the rest). Registers that are not parameters show their plain number.
     RAW    every register as the number a block would read or write, plus the modulated value ("eff") for parameters.
            Left/Right steps are exactly 1 (Shift: 256).
   A typed entry is always the raw number. Read-only registers (ro) refuse writes; Enter on a write-only one (wo) pokes a 1. */
#include "desk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define HELP_ROWS 2

/* Rows available for the list: all but the header and the help lines. */
static int list_rows(const View *v) { return v->rows - 1 - HELP_ROWS; }

/* Keep the selected row near the middle of the list, as far as the ends allow. */
static void scroll_to_selection(ValueList *l, int n, int vis) {
  l->top = fez_clamp(l->sel - vis / 2, 0, n > vis ? n - vis : 0);
}

static void draw_help(View *v, const char *name) {
  char lines[HELP_ROWS + 1][64];
  int n = wrap_text(regview_help(name), v->cols - 2, lines, HELP_ROWS);
  gfx_fill(&v->d->cv, v->x, v->y + (v->rows - HELP_ROWS) * v->rh - 3, v->w, 1, gfx_colour(COL_LINE));
  for (int i = 0; i < n && i < HELP_ROWS; i++) view_text(v, 0, v->rows - HELP_ROWS + i, lines[i], COL_FG);
}

/* Typed characters: digits (and a leading minus, for signed registers) build the entry. */
static void type_char(ValueList *l, int ch, int allow_minus) {
  if ((ch >= '0' && ch <= '9') || (ch == '-' && allow_minus && l->in.len == 0)) entry_add(&l->in, ch, 6);   /* 6 characters: -32768 */
}


static void entry_text(const ValueList *l, char *out, size_t n) { snprintf(out, n, "%s_", l->in.buf); }

/* ---------------------------------------------------------------- the rows: registers under group headings */

/* The headings come from REG_GROUPS (core/regview.c), which the manual's generator reads too. LEVEL is lifted out of the voice
   group into its own MASTER heading at the top, because the volume is the setting everyone reaches for first. */
#define GROUPS REG_GROUPS
#define NGROUPS REG_GROUP_COUNT
#define HEAD_MASTER NGROUPS                     /* heading number of MASTER (it is not in the table) */

/* Row kinds, in d->crow: a value >= 0 is an index into d->regs; a heading is stored as -(heading number + 1). */
static int is_heading(const Desk *d, int row) { return d->crow[row] < 0; }
static const char *heading_name(int h) { return h == HEAD_MASTER ? "MASTER" : GROUPS[h].name; }
const char *ctrl_heading_name(int h) { return heading_name(h); }   /* for other screens that list registers the same way (the handheld editor's palette) */

void ctrl_build_rows(Desk *d) {
  int n = 0, g = 0, level = -1;
  for (int i = 0; i < d->nregs; i++) if (d->regs[i].addr == REG_LEVEL) level = i;
  d->crow[n++] = -(HEAD_MASTER + 1);
  if (level >= 0) d->crow[n++] = level;
  for (int i = 0; i < d->nregs && n < MAX_LIST + 32 - 1; i++) {
    while (g < NGROUPS && d->regs[i].addr >= GROUPS[g].first) d->crow[n++] = -(g++ + 1);   /* a heading before the group's first register */
    if (i != level) d->crow[n++] = i;
  }
  d->ncrow = n;
  d->vl.sel = 1;                                                    /* the first real row (row 0 is the MASTER heading): LEVEL */
}

/* Up/Down: one row (wrapping), or ten with Shift (stopping at the ends). Headings are never selected: the selection steps
   over them in the direction of travel. */
static void move_selection(Desk *d, int dir, int mods) {
  ValueList *l = &d->vl;
  int n = d->ncrow, sel = l->sel;
  entry_clear(&l->in);
  if (mods & MOD_SHIFT) sel = fez_clamp(sel + dir * 10, 0, n - 1);
  else sel = (sel + dir + n) % n;
  while (is_heading(d, sel)) {
    sel += dir;
    if (mods & MOD_SHIFT) { if (sel < 0 || sel >= n) { sel = fez_clamp(sel, 0, n - 1); dir = -dir; } }   /* Shift stops at an end: turn round */
    else sel = (sel + n) % n;                                                                       /* a plain step wraps */
  }
  l->sel = sel;
}

/* ---------------------------------------------------------------- the tab */

/* Read-only in this list means a block cannot write it. LEVEL carries that flag so blocks leave the master volume alone,
   but the person at the keyboard may set it: the volume is the first row of this tab. */
static int locked(const RegEntry *e) { return e->ro && e->addr != REG_LEVEL; }

static int is_param(const FezSynth *s, int a) { return s->param_index[a] >= 0 && a != REG_GATE; }

void tab_ctrl_draw(Desk *d, View *v) {
  FezSynth *s = d->synth;
  ValueList *l = &d->vl;
  int n = d->ncrow, vis = list_rows(v);
  if (vis < 1) return;
  scroll_to_selection(l, n, vis);

  int show_now = v->cols >= 40;                                 /* the modulated value, if there is room */
  int valw = 7, bar_cols = v->cols - 9 - (valw + 1) - (show_now ? valw + 1 : 0) - 4;
  if (bar_cols < 4) bar_cols = 4;
  int val_col = 9 + bar_cols + 1, now_col = val_col + valw + 1, mark_col = (show_now ? now_col + valw : val_col + valw) + 1;

  view_text(v, 0, 0, d->ctl_raw ? "RAW" : "UNITS", COL_AMB);
  view_text_right(v, val_col, valw, 0, d->ctl_raw ? "raw" : "base", COL_DIM);
  if (show_now) view_text_right(v, now_col, valw, 0, d->ctl_raw ? "eff" : "now", COL_DIM);

  for (int r = 0; r < vis && l->top + r < n; r++) {
    int i = l->top + r, row = 1 + r, sel = i == l->sel, c = d->crow[i];
    if (c < 0) {                                                /* a group heading, with a rule running along to the edge */
      const char *name = heading_name(-c - 1);
      int len = (int)strlen(name);
      view_text(v, 0, row, name, COL_MAG);
      gfx_fill(&d->cv, v->x + (len + 1) * v->cw, v->y + row * v->rh + v->rh / 2, v->w - (len + 1) * v->cw - 6, 1, gfx_colour(COL_LINE));
      continue;
    }
    const RegEntry *e = &d->regs[c];
    int a = e->addr, par = is_param(s, a), base = regview_read(s, a), eff = par ? s->eff[a] : base;
    char buf[24];
    if (sel) view_fill(v, 0, row, v->cols - 1, 1, COL_SEL);
    view_text(v, 0, row, e->name, sel ? COL_AMB : COL_FG);
    if (!e->wo && !e->is_signed && a < REG_P0)
      view_bar(v, 9, row, bar_cols, base / 32767.0, sel ? COL_AMB : COL_DIM, sel ? COL_BG : COL_SEL);
    if (par && abs(eff - base) > 200) {                         /* a cyan tick where modulation puts the value now */
      int x = v->x + 9 * v->cw + (int)(eff / 32767.0 * (bar_cols * v->cw - 2));
      gfx_fill(&d->cv, x, v->y + row * v->rh + 1, 2, v->rh - 2, gfx_colour(COL_MAG));
    }
    if (e->wo) view_text_right(v, val_col, valw, row, "-", COL_FG);
    else if (sel && l->in.editing) { entry_text(l, buf, sizeof buf); view_text_right(v, val_col, valw, row, buf, COL_AMB); }
    else {
      if (par && !d->ctl_raw) regview_fmt_param(buf, sizeof buf, a, base); else snprintf(buf, sizeof buf, "%d", base);
      view_text_right(v, val_col, valw, row, buf, sel ? COL_AMB : COL_FG);
    }
    if (show_now && par && eff != base) {
      if (!d->ctl_raw) regview_fmt_param(buf, sizeof buf, a, eff); else snprintf(buf, sizeof buf, "%d", eff);
      view_text_right(v, now_col, valw, row, buf, COL_MAG);
    }
    if (locked(e) || e->wo) view_text(v, mark_col, row, e->wo ? "wo" : "ro", COL_DIM);
  }
  view_scrollbar(v, l->top, vis, n, 1, vis);
  draw_help(v, d->regs[d->crow[l->sel]].name);
}

void tab_ctrl_key(Desk *d, int key, int mods) {
  ValueList *l = &d->vl;
  const RegEntry *e = &d->regs[d->crow[l->sel]];
  int a = e->addr;
  switch (key) {
    case KEY_UP:   move_selection(d, -1, mods); break;
    case KEY_DOWN: move_selection(d, 1, mods); break;
    case KEY_TAB:  d->ctl_raw = !d->ctl_raw; break;
    case KEY_LEFT: case KEY_RIGHT: {
      int dir = key == KEY_RIGHT ? 1 : -1;
      entry_clear(&l->in);
      if (locked(e) || e->wo) break;
      if (is_param(d->synth, a) && !d->ctl_raw) regview_pstep(d->synth, a, dir, mods & MOD_SHIFT);     /* a musical step */
      else regview_write(d->synth, a, fez_clamp(regview_read(d->synth, a) + dir * (mods & MOD_SHIFT ? 256 : 1), -32768, 32767));
      break;
    }
    case KEY_ENTER:
      if (l->in.editing) {
        if (locked(e)) desk_say(d, "READ ONLY");
        else regview_write(d->synth, a, fez_clamp(atoi(l->in.buf), -32768, 32767));
        entry_clear(&l->in);
      } else if (e->wo) regview_write(d->synth, a, 1);          /* a write-only register: Enter pokes a 1 (fires it) */
      break;
    case KEY_ESC: entry_clear(&l->in); break;
    case KEY_BACKSPACE: entry_backspace(&l->in); break;
    default: break;
  }
}

void tab_ctrl_text(Desk *d, int ch) { type_char(&d->vl, ch, 1); }
