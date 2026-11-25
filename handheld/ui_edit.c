/* The BLK screen: a token editor for the 16 blocks. There is no text keyboard on the target devices, so a program
   is built by picking words from a palette (tabs: stack, maths, logic, memory, flow, notes, numbers, registers,
   pulser bits). The program is kept as a list of tokens, and every change recompiles the block at once.
   Port of drawEdit/keyEdit in dev/brick_body.js. */
#include "ui_priv.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- small text helpers */

static int streq_nocase(const char *a, const char *b) {
  for (; *a && *b; a++, b++) if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
  return *a == *b;
}

static int tok_color(const char *t) {
  switch (regview_token_kind(t)) {
    case TOK_OP: return AMB;
    case TOK_REG: return CYN;
    case TOK_NOTE: return GRN;
    default: return FG;
  }
}

/* After these words the display starts a new line, so a program reads like Forth source. */
static int breaks_after(const char *t) {
  static const char *B[] = { "L", "!", "JMP", "JZ", "JNZ", "EXIT", "YIELD", "TRIG", "ROUTE", "W" };
  for (unsigned i = 0; i < sizeof B / sizeof *B; i++) if (streq_nocase(t, B[i])) return 1;
  return 0;
}

/* ---------------------------------------------------------------- the token list <-> block source */

static void tokenise(const char *src, TokenList *out) {
  out->n = 0;
  const char *p = src;
  while (*p && out->n < ED_MAX_TOKS) {
    if (*p == '\\') { while (*p && *p != '\n') p++; continue; }        /* a backslash starts a comment to end of line */
    if (isspace((unsigned char)*p)) { p++; continue; }
    int k = 0;
    while (*p && !isspace((unsigned char)*p)) { if (k < ED_TOK_LEN - 1) out->t[out->n][k++] = *p; p++; }
    out->t[out->n][k] = '\0';
    out->n++;
  }
}

/* Recompile the block from the token list. */
static void commit(Ui *ui) {
  char src[ED_MAX_TOKS * ED_TOK_LEN];
  size_t len = 0;
  src[0] = '\0';
  for (int i = 0; i < ui->toks.n; i++) len += (size_t)snprintf(src + len, sizeof src - len, "%s%s", i ? " " : "", ui->toks.t[i]);
  vm_set_block(&ui->synth->vm, ui->blk, src);
}

static void push_undo(Ui *ui) {
  if (ui->nundo == ED_UNDO) { memmove(&ui->undo[0], &ui->undo[1], sizeof(TokenList) * (ED_UNDO - 1)); ui->nundo--; }
  ui->undo[ui->nundo++] = ui->toks;
}

static void insert_token(Ui *ui, const char *t);

static void flush_lit(Ui *ui) {
  if (ui->lit[0] && strcmp(ui->lit, "-") != 0) { char t[8]; strcpy(t, ui->lit); ui->lit[0] = '\0'; insert_token(ui, t); }
  ui->lit[0] = '\0';
}

static void insert_token(Ui *ui, const char *t) {
  flush_lit(ui);
  if (ui->toks.n >= ED_MAX_TOKS) return;
  push_undo(ui);
  memmove(&ui->toks.t[ui->cur + 1], &ui->toks.t[ui->cur], sizeof(Token) * (size_t)(ui->toks.n - ui->cur));
  snprintf(ui->toks.t[ui->cur], ED_TOK_LEN, "%s", t);
  ui->toks.n++; ui->cur++;
  commit(ui);
}

static void delete_token(Ui *ui) {
  size_t l = strlen(ui->lit);
  if (l) { ui->lit[l - 1] = '\0'; return; }                             /* B first erases a half-typed number */
  if (ui->cur > 0) {
    push_undo(ui);
    memmove(&ui->toks.t[ui->cur - 1], &ui->toks.t[ui->cur], sizeof(Token) * (size_t)(ui->toks.n - ui->cur));
    ui->toks.n--; ui->cur--;
    commit(ui);
  }
}

void edit_open_block(Ui *ui, int b) {
  ui->blk = (b + NUM_BLOCKS) % NUM_BLOCKS;
  tokenise(ui->synth->vm.blocks[ui->blk].src, &ui->toks);
  ui->cur = ui->toks.n; ui->lit[0] = '\0'; ui->nundo = 0; ui->top = 0;
}

void edit_run(Ui *ui) { flush_lit(ui); vm_run_block(&ui->synth->vm, ui->blk); ui_say(ui, "RAN BLOCK"); }
void edit_undo(Ui *ui) {
  if (!ui->nundo) return;
  ui->toks = ui->undo[--ui->nundo];
  if (ui->cur > ui->toks.n) ui->cur = ui->toks.n;
  ui->lit[0] = '\0'; commit(ui);
}
void edit_copy(Ui *ui) { ui->clip = ui->toks; ui_say(ui, "COPIED"); }
void edit_paste(Ui *ui) {
  flush_lit(ui);
  if (!ui->clip.n || ui->toks.n + ui->clip.n > ED_MAX_TOKS) return;
  push_undo(ui);
  memmove(&ui->toks.t[ui->cur + ui->clip.n], &ui->toks.t[ui->cur], sizeof(Token) * (size_t)(ui->toks.n - ui->cur));
  memcpy(&ui->toks.t[ui->cur], ui->clip.t, sizeof(Token) * (size_t)ui->clip.n);
  ui->toks.n += ui->clip.n; ui->cur += ui->clip.n;
  commit(ui);
}
void edit_clear(Ui *ui) { push_undo(ui); ui->toks.n = 0; ui->cur = 0; commit(ui); }

/* ---------------------------------------------------------------- the palette */

typedef struct { const char *name; int cols; const char *const *items; int n; } Tab;

#define LIST(a) a, (int)(sizeof a / sizeof *a)
static const char *const T_STK[] = { "DUP", "DROP", "SWAP", "OVER", "ROT", "?" };
static const char *const T_MTH[] = { "+", "-", "*", "/", "*/", "NEG", "ABS", "MIN", "MAX", "MOD", "ST", "HZ", "COLOUR" };
static const char *const T_LOG[] = { "AND", "OR", "XOR", "NOT", "SHL", "SHR", "=", "<", ">", "U<" };
static const char *const T_MEM[] = { "@", "!", "TRIG", "ROUTE", "W", "RND" };
static const char *const T_FLW[] = { "L", "JMP", "JZ", "JNZ", "SKIP", "EXIT", "YIELD" };
static const char *const T_NUM[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "-", "ENT", "100", "1000", "4096", "16384", "32767" };
static const char *const T_PLS[] = { "P0", "P1", "P2", "P3" };

enum { TAB_NOTE = 5, TAB_NUM = 6, TAB_REG = 7, TAB_PLS = 8, TAB_COUNT = 9 };

/* The NOTE and REG tabs are filled in at start-up (their lists are built from tables), so these two are rewritten
   in edit_tab(). */
static Tab edit_tab(Ui *ui, int i) {
  switch (i) {
    case 0: return (Tab){ "STK", 6, LIST(T_STK) };
    case 1: return (Tab){ "MTH", 6, LIST(T_MTH) };
    case 2: return (Tab){ "LOG", 6, LIST(T_LOG) };
    case 3: return (Tab){ "MEM", 6, LIST(T_MEM) };
    case 4: return (Tab){ "FLW", 6, LIST(T_FLW) };
    case TAB_NOTE: return (Tab){ "NOTE", 12, ui->note_items, 60 };
    case TAB_NUM: return (Tab){ "NUM", 6, LIST(T_NUM) };
    case TAB_REG: return (Tab){ "REG", 6, ui->reg_items, ui->nreg_items };
    default: return (Tab){ "PLS", 4, LIST(T_PLS) };
  }
}

void edit_build_palettes(Ui *ui) {
  static const char *NN[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
  for (int o = 0; o < 5; o++) for (int n = 0; n < 12; n++) {
    snprintf(ui->note_text[o * 12 + n], sizeof ui->note_text[0], "%s%d", NN[n], o + 2);
    ui->note_items[o * 12 + n] = ui->note_text[o * 12 + n];
  }
  /* every register from 100 up, by address (ui->regs is already sorted) */
  ui->nreg_items = 0;
  for (int i = 0; i < ui->nregs; i++) {
    const char *nm = ui->regs[i].name;
    if (nm[0] == 'P' && isdigit((unsigned char)nm[1])) continue;        /* P0..P15 are per-block privates, not shared registers */
    ui->reg_items[ui->nreg_items++] = nm;
  }
}

static void palette_insert(Ui *ui, const char *s) {
  if (ui->pal_tab == TAB_PLS) { ui->synth->mem[REG_BP0 + ui->blk] ^= (int16_t)(1 << (s[1] - '0')); return; }   /* a block setting, not a word */
  if (ui->pal_tab == TAB_NUM) {
    size_t l = strlen(ui->lit);
    if (isdigit((unsigned char)s[0]) && !s[1]) { if (l < 6) { ui->lit[l] = s[0]; ui->lit[l + 1] = '\0'; } return; }
    if (s[0] == '.' && !s[1]) { if (!strchr(ui->lit, '.') && l < 6) { ui->lit[l] = '.'; ui->lit[l + 1] = '\0'; } return; }
    if (s[0] == '-' && !s[1]) { if (!l) strcpy(ui->lit, "-"); return; }
    if (strcmp(s, "ENT") == 0) { flush_lit(ui); return; }
  }
  insert_token(ui, s);
}

/* ---------------------------------------------------------------- layout of the code area */

#define CODE_COLS 50
#define CODE_ROWS 11

/* One displayed item: a token, or the number being typed (is_lit). */
typedef struct { const char *text; int tok, is_lit, line, col; } Item;

/* Place tokens on lines of CODE_COLS characters. Returns the number of items; *nlines gets the line count. */
static int layout(Ui *ui, Item *items, int *nlines) {
  int n = 0, line = 0, col = 0;
  for (int i = 0; i <= ui->toks.n; i++) {
    for (int pass = 0; pass < 2; pass++) {
      Item it;
      if (pass == 0) { if (!(ui->lit[0] && i == ui->cur)) continue; it = (Item){ ui->lit, i, 1, 0, 0 }; }
      else { if (i == ui->toks.n) continue; it = (Item){ ui->toks.t[i], i, 0, 0, 0 }; }
      int w = (int)strlen(it.text) + 1;
      if (col + w > CODE_COLS && col > 0) { line++; col = 0; }
      it.line = line; it.col = col; items[n++] = it; col += w;
      if (!it.is_lit && breaks_after(it.text)) { line++; col = 0; }
    }
  }
  *nlines = line + 1;
  return n;
}

/* ---------------------------------------------------------------- drawing */

static int blink_on(const Ui *ui) { return ui->now_ms - ui->last_key_ms < 500 || ((long)(ui->now_ms / 500)) % 2 == 0; }

void edit_draw(Ui *ui) {
  FezSynth *s = ui->synth;
  const Block *blk = &s->vm.blocks[ui->blk];
  char buf[160];

  /* 1. the strip of 16 blocks: number, how full it is, a dot if a pulser runs it */
  for (int b = 0; b < NUM_BLOCKS; b++) {
    const Block *bb = &s->vm.blocks[b];
    int x = 4 + b * 15, sel = b == ui->blk;
    ui_fill(ui, x, 14, 14, 10, sel ? AMBD : PNL);
    snprintf(buf, sizeof buf, "%X", b);
    ui_text(ui, x + 4, 15, buf, sel ? AMB : bb->halted ? RED : bb->c.count ? FG : DIM);
    ui_fill(ui, x + 1, 22, bb->c.count * 12 / BLOCK_SLOTS, 1, bb->c.ok ? (sel ? AMB : DIM) : RED);
    if (s->mem[REG_BP0 + b]) ui_px(ui, x + 12, 15, CYN);
  }

  /* 2. the code, with the cursor */
  Item items[ED_MAX_TOKS + 1]; int nlines;
  int ni = layout(ui, items, &nlines);
  int cl = nlines - 1, cc = 0, found = 0;
  for (int k = 0; k < ni && !found; k++) {
    if (items[k].is_lit) { cl = items[k].line; cc = items[k].col + (int)strlen(items[k].text); found = 1; }
    else if (items[k].tok == ui->cur) { cl = items[k].line; cc = items[k].col; found = 1; }
  }
  if (!found) {                                                           /* the cursor is after the last token */
    for (int k = 0; k < ni; k++) if (items[k].line == cl) cc = items[k].col + (int)strlen(items[k].text) + 1;
  }
  if (cl < ui->top) ui->top = cl;
  if (cl > ui->top + CODE_ROWS - 1) ui->top = cl - (CODE_ROWS - 1);
  for (int k = 0; k < ni; k++) {
    int r = items[k].line - ui->top;
    if (r < 0 || r >= CODE_ROWS) continue;
    ui_text(ui, 3 + items[k].col * 5, 26 + r * 8, items[k].text, items[k].is_lit ? FG : tok_color(items[k].text));
  }
  if (ui->toks.n == 0 && !ui->lit[0]) ui_text(ui, 3, 26, "empty block  pick words below, A inserts", DIM);
  if (blink_on(ui)) ui_fill(ui, 3 + cc * 5, 26 + (cl - ui->top) * 8 + 7, 5, 1, ui->focus_code ? AMB : DIM);

  /* 3. the status line: a fault, a compile error, or the block's size and which pulsers run it */
  int c = FG;
  if (blk->halted) { snprintf(buf, sizeof buf, "B%X HALT %s", ui->blk, vm_fault_name(blk->fault)); c = RED; }
  else if (!blk->c.ok) { snprintf(buf, sizeof buf, "B%X ERR %.40s", ui->blk, blk->c.err); c = RED; }
  else {
    size_t l = (size_t)snprintf(buf, sizeof buf, "B%X %d/%d slots  peak %d/%d  runs on", ui->blk, blk->c.count, BLOCK_SLOTS, blk->peak, STEP_CAP);
    int any = 0;
    for (int p = 0; p < 4; p++) if ((s->mem[REG_BP0 + ui->blk] >> p) & 1) { l += (size_t)snprintf(buf + l, sizeof buf - l, " P%d", p); any = 1; }
    if (!any) l += (size_t)snprintf(buf + l, sizeof buf - l, " none");
    if (ui->lit[0]) snprintf(buf + l, sizeof buf - l, "   >%s", ui->lit);
  }
  ui_fill(ui, 0, 114, UI_W, 8, SEL);
  buf[50] = '\0';                                                        /* the line is 50 characters wide */
  ui_text(ui, 3, 114, buf, c);

  /* 4. the palette: tabs along the top, a grid of words below */
  Tab tab = edit_tab(ui, ui->pal_tab);
  int tw = UI_W / TAB_COUNT;
  for (int i = 0; i < TAB_COUNT; i++) {
    int on = i == ui->pal_tab;
    Tab t = edit_tab(ui, i);
    ui_fill(ui, i * tw, 123, tw - 1, 10, on ? (ui->focus_code ? SEL : AMBD) : PNL);
    ui_ctext(ui, i * tw, tw, 124, t.name, i == TAB_PLS ? CYN : on ? AMB : DIM);
  }
  int cols = tab.cols, cw = 250 / cols, rows = 5, nr = (tab.n + cols - 1) / cols;
  ui->pal_item = ui_clamp(ui->pal_item, 0, tab.n - 1);
  int sr = ui->pal_item / cols, off = ui_clamp(sr - 2, 0, nr > rows ? nr - rows : 0);
  for (int r = 0; r < rows; r++) for (int k = 0; k < cols; k++) {
    int i = (off + r) * cols + k;
    if (i >= tab.n) continue;
    const char *it = tab.items[i];
    int x = 3 + k * cw, y = 135 + r * 9, sel = i == ui->pal_item && !ui->focus_code;
    if (sel) ui_fill(ui, x, y, cw - 1, 9, AMBD); else if (i == ui->pal_item) ui_fill(ui, x, y, cw - 1, 9, SEL);
    if (ui->pal_tab == TAB_PLS) {
      int on = (s->mem[REG_BP0 + ui->blk] >> (it[1] - '0')) & 1;
      snprintf(buf, sizeof buf, "%s %s", it, on ? "on" : "off");
      ui_ctext(ui, x, cw - 1, y + 1, buf, on ? (sel ? FG : GRN) : DIM);
      continue;
    }
    int col = ui->pal_tab == TAB_NOTE ? GRN : ui->pal_tab == TAB_REG ? CYN : ui->pal_tab == TAB_NUM ? FG : AMB;
    ui_ctext(ui, x, cw - 1, y + 1, it, sel ? FG : col);
  }
  if (nr > rows) { ui_fill(ui, 253, 135, 2, 45, SEL); ui_fill(ui, 253, 135 + off * 45 / nr, 2, rows * 45 / nr, DIM); }

  ui_bottom_bar(ui, !ui->focus_code
    ? (ui->pal_tab == TAB_PLS ? "A toggle pulser bit  B del  X code  Y run" : "A insert  B del  X code  Y run  L2/R2 tab")
    : "A palette  B del  X pal  Y run  L2/R2 blk");
}

/* ---------------------------------------------------------------- keys */

void edit_key(Ui *ui, Button b) {
  Tab tab = edit_tab(ui, ui->pal_tab);
  int cols = tab.cols, n = tab.n;
  if (b == BTN_Y) { edit_run(ui); return; }
  if (b == BTN_X) { ui->focus_code = !ui->focus_code; return; }
  if (b == BTN_B) { delete_token(ui); return; }
  if (b == BTN_L2 || b == BTN_R2) {
    int d = b == BTN_R2 ? 1 : -1;
    if (!ui->focus_code) { ui->pal_tab = (ui->pal_tab + d + TAB_COUNT) % TAB_COUNT; ui->pal_item = 0; }
    else { flush_lit(ui); edit_open_block(ui, ui->blk + d); }
    return;
  }
  if (!ui->focus_code) {
    if (b == BTN_A) palette_insert(ui, tab.items[ui->pal_item]);
    else if (b == BTN_LEFT) ui->pal_item = (ui->pal_item + n - 1) % n;
    else if (b == BTN_RIGHT) ui->pal_item = (ui->pal_item + 1) % n;
    else if (b == BTN_UP) { if (ui->pal_item - cols >= 0) ui->pal_item -= cols; }
    else if (b == BTN_DOWN) { if (ui->pal_item + cols < n) ui->pal_item += cols; }
    return;
  }
  if (b == BTN_A) { ui->focus_code = 0; return; }
  flush_lit(ui);
  if (b == BTN_LEFT) { if (ui->cur > 0) ui->cur--; }
  else if (b == BTN_RIGHT) { if (ui->cur < ui->toks.n) ui->cur++; }
  else if (b == BTN_UP || b == BTN_DOWN) {
    Item items[ED_MAX_TOKS + 1]; int nlines;
    int ni = layout(ui, items, &nlines);
    int li = nlines - 1, col = 0;
    int found = 0;
    for (int k = 0; k < ni; k++) if (items[k].tok == ui->cur) { li = items[k].line; col = items[k].col; found = 1; break; }
    if (!found) for (int k = 0; k < ni; k++) if (items[k].line == li) col = items[k].col + (int)strlen(items[k].text) + 1;
    int tl = li + (b == BTN_UP ? -1 : 1);
    if (tl < 0 || tl >= nlines) return;
    int best = -1, bd = 99;
    for (int k = 0; k < ni; k++) if (items[k].line == tl) { int dd = abs(items[k].col - col); if (dd < bd) { bd = dd; best = k; } }
    if (best >= 0) ui->cur = items[best].tok;
  }
}
