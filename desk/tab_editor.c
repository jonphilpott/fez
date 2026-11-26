/* The EDITOR tab: a plain text editor for the 16 blocks.

   A block is Forth-like source (at most 128 slots). Each block's text is kept here, apart from the running synth,
   so a half-typed program never changes the sound: Ctrl+Enter compiles the text and swaps it in.
   While you type, the text is also compiled on the side every frame, so the status line always says whether it
   would work, how many slots it uses, and what is wrong.

   Keys (Ctrl and Alt are interchangeable; nothing needs Home, End or the Page keys):
     arrows            move (with Shift: select)         Ctrl+A / Ctrl+E   start / end of line
     Backspace, Delete erase                             Ctrl+K            delete to end of line
     Ctrl+C X V        copy, cut, paste (system clipboard)   Ctrl+Z   undo      Ctrl+Y or Ctrl+Shift+Z   redo
     Ctrl+/            comment or uncomment the line (a backslash starts a comment)
     Tab               complete the word (press again for the next choice); else two spaces
     Ctrl+Enter        apply the block        Ctrl+R   apply it and run it once now
     [  ]  (plain)     previous / next block (also Ctrl+, and Ctrl+.)   Ctrl+1 to 4   toggle which pulsers run this block
     Note: [ and ] are not typed in the editor. They are the "previous / next page" keys of every tab, and no block needs them. */
#include "desk.h"
#include "words.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>          /* strcasecmp: POSIX, and glibc hides it from <string.h> under -std=c99 */

#define MAX_CAND   64
#define POPUP_ROWS 6
#define FLASH_MS 250.0         /* how long a block's cell flashes after it runs */
#define HEADER_ROWS 2          /* screen rows taken by the block grid above the text */

/* ---------------------------------------------------------------- the text and the cursor */

static Editor *ed(Desk *d) { return &d->ed; }
static char *txt(Desk *d) { return d->ed.text[d->ed.blk]; }
static int txt_len(Desk *d) { return (int)strlen(txt(d)); }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

static int line_start(const char *t, int c) { while (c > 0 && t[c - 1] != '\n') c--; return c; }
static int line_end(const char *t, int c) { while (t[c] && t[c] != '\n') c++; return c; }

/* Long lines are wrapped on screen every `width` characters. A screen row is a "chunk" of a line: chunk k covers
   positions [ls + k*width, ls + (k+1)*width). The position just past a line's last character belongs to the chunk
   it would start or continue, so a line has len/width + 1 chunks. */
static int chunk_start(const char *t, int c, int width) {
  int ls = line_start(t, c);
  return ls + (c - ls) / width * width;
}

/* The selection as [a, b), or 0 if there is none. */
static int selection(Desk *d, int *a, int *b) {
  Editor *e = ed(d);
  if (e->anchor < 0 || e->anchor == e->cursor) return 0;
  *a = imin(e->anchor, e->cursor); *b = imax(e->anchor, e->cursor);
  return 1;
}

void desk_reload_editor(Desk *d) {
  Editor *e = ed(d);
  for (int b = 0; b < NUM_BLOCKS; b++) snprintf(e->text[b], BLOCK_SRC_MAX, "%s", d->synth->vm.blocks[b].src);
  e->cursor = 0; e->anchor = -1; e->top = 0; e->nundo = e->nredo = 0; e->typing = 0; e->cycle = 0;
}

/* Every block counts its runs. If the count went up since the last frame, the block ran: remember when, so the
   block grid can flash it. (A count that went down means a patch was loaded: just resync, no flash.) */
void editor_watch_blocks(Desk *d) {
  Editor *e = ed(d);
  for (int b = 0; b < NUM_BLOCKS; b++) {
    int runs = d->synth->vm.blocks[b].runs;
    if (runs > e->seen_runs[b]) e->flash_at[b] = d->now_ms;
    e->seen_runs[b] = runs;
  }
}

/* ---------------------------------------------------------------- undo and redo */

static void snap_save(Desk *d, EdSnap *s) { snprintf(s->text, BLOCK_SRC_MAX, "%s", txt(d)); s->cursor = ed(d)->cursor; }
static void snap_load(Desk *d, const EdSnap *s) {
  snprintf(txt(d), BLOCK_SRC_MAX, "%s", s->text);
  ed(d)->cursor = imin(s->cursor, txt_len(d)); ed(d)->anchor = -1;
}

static void stack_push(Desk *d, EdSnap *stack, int *n) {
  if (*n == ED_UNDO) { memmove(&stack[0], &stack[1], sizeof(EdSnap) * (ED_UNDO - 1)); (*n)--; }
  snap_save(d, &stack[(*n)++]);
}

/* Call before every change. Consecutive typed characters share one undo step; anything else gets its own. */
static void begin_edit(Desk *d, int is_typing) {
  Editor *e = ed(d);
  if (!(is_typing && e->typing)) stack_push(d, e->undo, &e->nundo);
  e->typing = is_typing; e->nredo = 0;
}

static void undo(Desk *d) {
  Editor *e = ed(d);
  if (!e->nundo) return;
  stack_push(d, e->redo, &e->nredo);
  snap_load(d, &e->undo[--e->nundo]);
  e->typing = 0;
}

static void redo(Desk *d) {
  Editor *e = ed(d);
  if (!e->nredo) return;
  stack_push(d, e->undo, &e->nundo);
  snap_load(d, &e->redo[--e->nredo]);
  e->typing = 0;
}

/* ---------------------------------------------------------------- editing primitives */

static void delete_range(Desk *d, int a, int b) {
  char *t = txt(d);
  memmove(t + a, t + b, (size_t)(txt_len(d) - b + 1));
  ed(d)->cursor = a; ed(d)->anchor = -1;
}

static int delete_selection(Desk *d) {
  int a, b;
  if (!selection(d, &a, &b)) return 0;
  delete_range(d, a, b);
  return 1;
}

/* Insert n characters at the cursor (replacing the selection). Refuses if the block's text would not fit. */
static void insert_raw(Desk *d, const char *s, int n) {
  delete_selection(d);
  char *t = txt(d);
  int len = txt_len(d), c = ed(d)->cursor;
  if (len + n >= BLOCK_SRC_MAX - 1) { desk_say(d, "BLOCK TEXT IS FULL"); return; }
  memmove(t + c + n, t + c, (size_t)(len - c + 1));
  memcpy(t + c, s, (size_t)n);
  ed(d)->cursor = c + n; ed(d)->anchor = -1;
}

/* ---------------------------------------------------------------- completion */

static int prefix_nocase(const char *word, const char *prefix, int plen) {
  for (int i = 0; i < plen; i++) if (!word[i] || toupper((unsigned char)word[i]) != toupper((unsigned char)prefix[i])) return 0;
  return 1;
}

/* Operations and register names that start with the prefix (case does not matter). A word that is already
   complete is not offered back to you. */
static int build_candidates(const char *prefix, const char **out) {
  int n = 0, plen = (int)strlen(prefix);
  if (plen < 2) return 0;
  for (int i = 0; i < OP_COUNT && n < MAX_CAND; i++)
    if (prefix_nocase(OP_NAMES[i], prefix, plen) && (int)strlen(OP_NAMES[i]) > plen) out[n++] = OP_NAMES[i];
  for (int i = 0; i < NAME_COUNT && n < MAX_CAND; i++)
    if (prefix_nocase(NAME_TABLE[i].name, prefix, plen) && (int)strlen(NAME_TABLE[i].name) > plen) out[n++] = NAME_TABLE[i].name;
  return n;
}

static int in_comment(const char *t, int pos) {
  for (int i = line_start(t, pos); i < pos; i++) if (t[i] == '\\') return 1;
  return 0;
}

/* The word being completed: its prefix, where it starts, and how long it is now. Returns 0 if there is no word
   to complete (a selection, the middle of a word, a comment). When Tab is already cycling, it is the original
   prefix and the candidate inserted last. */
static int current_word(Desk *d, char *prefix, int *start, int *cur_len) {
  Editor *e = ed(d);
  if (e->cycle) { snprintf(prefix, 16, "%s", e->cycle_prefix); *start = e->cycle_start; *cur_len = e->cycle_len; return 1; }
  const char *t = txt(d);
  int c = e->cursor, s = c;
  if (e->anchor >= 0 && e->anchor != c) return 0;
  if (t[c] && !isspace((unsigned char)t[c])) return 0;
  while (s > 0 && !isspace((unsigned char)t[s - 1])) s--;
  if (c - s < 1 || c - s > 15 || in_comment(t, s)) return 0;
  memcpy(prefix, t + s, (size_t)(c - s)); prefix[c - s] = '\0';
  *start = s; *cur_len = c - s;
  return 1;
}

/* Tab: replace the word with the next candidate and follow it with a space, ready for the next word. Pressing Tab
   again swaps the word (and its space) for the next candidate. */
static void complete(Desk *d) {
  Editor *e = ed(d);
  char prefix[16]; int start, cur_len;
  const char *cand[MAX_CAND]; int n = 0;
  if (current_word(d, prefix, &start, &cur_len)) n = build_candidates(prefix, cand);
  if (n == 0) { begin_edit(d, 0); insert_raw(d, "  ", 2); return; }       /* nothing to complete: Tab indents */
  int idx = e->cycle ? (e->cycle_idx + 1) % n : 0;
  begin_edit(d, 0);
  delete_range(d, start, start + cur_len);
  insert_raw(d, cand[idx], (int)strlen(cand[idx]));
  int added = (int)strlen(cand[idx]);
  if (txt(d)[e->cursor] != ' ') { insert_raw(d, " ", 1); added++; }    /* no second space if one follows already */
  snprintf(e->cycle_prefix, sizeof e->cycle_prefix, "%s", prefix);
  e->cycle = 1; e->cycle_idx = idx; e->cycle_start = start; e->cycle_len = added;
}

/* ---------------------------------------------------------------- commands */

static void apply_block(Desk *d) {
  Editor *e = ed(d);
  vm_set_block(&d->synth->vm, e->blk, txt(d));
  const Block *b = &d->synth->vm.blocks[e->blk];
  char m[64];
  if (b->c.ok) snprintf(m, sizeof m, "APPLIED B%X  %d/%d slots", e->blk, b->c.count, BLOCK_SLOTS);
  else snprintf(m, sizeof m, "ERROR: %.50s", b->c.err);
  desk_say(d, m);
}

static void switch_block(Desk *d, int delta) {
  Editor *e = ed(d);
  e->blk = (e->blk + delta + NUM_BLOCKS) % NUM_BLOCKS;
  e->cursor = 0; e->anchor = -1; e->top = 0; e->nundo = e->nredo = 0; e->typing = 0; e->cycle = 0;
}

static void copy_selection(Desk *d, int cut) {
  int a, b;
  if (!selection(d, &a, &b)) return;
  char buf[BLOCK_SRC_MAX];
  snprintf(buf, sizeof buf, "%.*s", b - a, txt(d) + a);
  if (d->clip_set) d->clip_set(buf);
  if (cut) { begin_edit(d, 0); delete_range(d, a, b); }
}

static void paste(Desk *d) {
  char raw[BLOCK_SRC_MAX], clean[BLOCK_SRC_MAX];
  if (!d->clip_get || d->clip_get(raw, sizeof raw) <= 0) return;
  int n = 0;
  for (const char *p = raw; *p && n < BLOCK_SRC_MAX - 3; p++) {         /* keep printable ASCII and newlines; a tab becomes two spaces */
    if (*p == '\t') { clean[n++] = ' '; clean[n++] = ' '; }
    else if (*p == '\n' || (*p >= 32 && *p < 127)) clean[n++] = *p;
  }
  if (!n) return;
  begin_edit(d, 0);
  insert_raw(d, clean, n);
}

/* Ctrl+/: put "\ " at the start of the line's text, or take it off if it is there. */
static void toggle_comment(Desk *d) {
  Editor *e = ed(d);
  const char *t = txt(d);
  int ls = line_start(t, e->cursor), p = ls, c = e->cursor;     /* c: where the cursor was, to put it back after the edit */
  while (t[p] == ' ') p++;
  begin_edit(d, 0);
  e->anchor = -1;                       /* acts on the cursor's line only: drop any selection, or insert_raw would replace it */
  if (t[p] == '\\') {
    int n = t[p + 1] == ' ' ? 2 : 1;
    delete_range(d, p, p + n);          /* (this moves the cursor to p) */
    e->cursor = c > p ? imax(p, c - n) : c;
  } else {
    int len = txt_len(d);
    e->cursor = p; insert_raw(d, "\\ ", 2);
    int added = txt_len(d) - len;       /* 2, or 0 if the block was full and insert_raw refused */
    e->cursor = c >= p ? c + added : c;
  }
}

/* ---------------------------------------------------------------- keys */

static void move_to(Desk *d, int pos, int mods, int keep_col) {
  Editor *e = ed(d);
  if (mods & MOD_SHIFT) { if (e->anchor < 0) e->anchor = e->cursor; } else e->anchor = -1;
  e->cursor = pos;
  if (!keep_col) e->want_col = pos - chunk_start(txt(d), pos, imax(e->width, 8));
  e->typing = 0;
}

static void move_vertical(Desk *d, int dir, int mods) {
  const char *t = txt(d);
  Editor *e = ed(d);
  int width = imax(d->ed.width, 8), want = imin(e->want_col, width - 1);
  int c = e->cursor, cs = chunk_start(t, c, width), ls = line_start(t, c), le = line_end(t, c);
  int target;
  if (dir < 0) {
    if (cs > ls) target = cs - width + want;                           /* the chunk above, in the same line */
    else if (ls == 0) target = 0;                                      /* already on the first row */
    else {                                                             /* the last chunk of the previous line */
      int pe = ls - 1, pcs = chunk_start(t, pe, width);
      target = pcs + imin(want, pe - pcs);
    }
  } else {
    if (cs + width <= le) target = cs + width + imin(want, le - (cs + width));   /* the chunk below, in the same line */
    else if (!t[le]) target = le;                                       /* already on the last row */
    else { int nls = le + 1, nle = line_end(t, nls); target = nls + imin(want, nle - nls); }
  }
  move_to(d, target, mods, 1);
}

static void newline(Desk *d) {
  const char *t = txt(d);
  int ls = line_start(t, ed(d)->cursor), ind = 0;
  while (t[ls + ind] == ' ' && ls + ind < ed(d)->cursor) ind++;        /* keep the previous line's indentation */
  char buf[64] = "\n";
  if (ind > 60) ind = 60;
  memset(buf + 1, ' ', (size_t)ind);
  begin_edit(d, 0);
  insert_raw(d, buf, 1 + ind);
  ed(d)->want_col = ind;
}

void tab_editor_key(Desk *d, int key, int mods) {
  Editor *e = ed(d);
  e->last_key_ms = d->now_ms;
  if (!(key == KEY_TAB && !(mods & (MOD_CTRL | MOD_ALT)))) e->cycle = 0;

  if (mods & (MOD_CTRL | MOD_ALT)) {                         /* chords */
    const char *t = txt(d);
    switch (key) {
      case 'a': move_to(d, line_start(t, e->cursor), mods, 0); break;
      case 'e': move_to(d, line_end(t, e->cursor), mods, 0); break;
      case 'z': if (mods & MOD_SHIFT) redo(d); else undo(d); break;
      case 'y': redo(d); break;
      case 'c': copy_selection(d, 0); break;
      case 'x': copy_selection(d, 1); break;
      case 'v': paste(d); break;
      case 'k': {                                            /* delete to the end of the line (or the line break itself) */
        int le = line_end(t, e->cursor);
        begin_edit(d, 0);
        delete_range(d, e->cursor, t[le] && le == e->cursor ? le + 1 : le);
        break;
      }
      case '/': toggle_comment(d); break;
      case KEY_ENTER: apply_block(d); break;
      case 'r':                                               /* apply, then run once: one key to hear what you typed */
        apply_block(d);
        if (d->synth->vm.blocks[e->blk].c.ok) { vm_run_block(&d->synth->vm, e->blk); desk_say(d, "APPLIED AND RAN"); }
        break;                                                /* a compile error: apply_block has shown it, and nothing runs */
      case ',': switch_block(d, -1); break;
      case '.': switch_block(d, 1); break;
      case '1': case '2': case '3': case '4':
        d->synth->mem[REG_BP0 + e->blk] ^= (int16_t)(1 << (key - '1'));
        break;
      default: break;
    }
    return;
  }

  int c = e->cursor, len = txt_len(d);
  switch (key) {
    case KEY_LEFT:  move_to(d, c > 0 ? c - 1 : 0, mods, 0); break;
    case KEY_RIGHT: move_to(d, c < len ? c + 1 : len, mods, 0); break;
    case KEY_UP:    move_vertical(d, -1, mods); break;
    case KEY_DOWN:  move_vertical(d, 1, mods); break;
    case KEY_ENTER: newline(d); break;
    case KEY_BACKSPACE:
      begin_edit(d, 0);
      if (!delete_selection(d) && c > 0) delete_range(d, c - 1, c);
      e->want_col = e->cursor - chunk_start(txt(d), e->cursor, imax(e->width, 8));
      break;
    case KEY_DELETE:
      begin_edit(d, 0);
      if (!delete_selection(d) && c < len) delete_range(d, c, c + 1);
      break;
    case KEY_TAB: complete(d); e->want_col = e->cursor - chunk_start(txt(d), e->cursor, imax(e->width, 8)); break;
    case KEY_ESC: e->anchor = -1; break;
    default: break;
  }
}

void tab_editor_text(Desk *d, int ch) {
  Editor *e = ed(d);
  if (ch == '[' || ch == ']') { switch_block(d, ch == ']' ? 1 : -1); return; }   /* the same keys that change a sub-page in every tab; so a bracket cannot be typed here */
  e->last_key_ms = d->now_ms; e->cycle = 0;
  char c = (char)ch;
  begin_edit(d, 1);
  insert_raw(d, &c, 1);
  e->want_col = e->cursor - chunk_start(txt(d), e->cursor, imax(e->width, 8));
}

/* ---------------------------------------------------------------- drawing */

/* Colour for every character of one line: operations amber, registers cyan, notes green, comments dim. */
static void colour_line(const char *t, int ls, int le, unsigned char *out, int max) {
  int i = ls;
  while (i < le) {
    int k = i - ls;
    if (t[i] == ' ') { if (k < max) out[k] = COL_FG; i++; continue; }
    int j = i;
    if (t[i] == '\\') {                                       /* a comment runs to the end of the line */
      for (; j < le; j++) if (j - ls < max) out[j - ls] = COL_DIM;
      return;
    }
    char tok[32]; int n = 0;
    for (; j < le && t[j] != ' '; j++) if (n < 31) tok[n++] = t[j];
    tok[n] = '\0';
    int kind = regview_token_kind(tok);
    int col = kind == TOK_OP ? COL_AMB : kind == TOK_REG ? COL_CYN : kind == TOK_NOTE ? COL_GRN : COL_FG;
    for (; i < j; i++) if (i - ls < max) out[i - ls] = (unsigned char)col;
  }
}

/* The word the cursor is in (or just after), copied into out. Returns 0 if the cursor is on a space, or in a comment
   (a backslash earlier on the same line starts one). */
static int word_under_cursor(const char *t, int c, char *out, int cap) {
  int ls = line_start(t, c);
  for (int i = ls; i < c; i++) if (t[i] == '\\') return 0;
  int a = c, b = c;
  while (a > ls && !isspace((unsigned char)t[a - 1])) a--;
  while (t[b] && !isspace((unsigned char)t[b])) b++;
  if (a == b || b - a >= cap) return 0;
  memcpy(out, t + a, (size_t)(b - a)); out[b - a] = '\0';
  return 1;
}

/* A line about the word under the cursor, for the status line: an operation's stack picture ("SCALE ( index scale --
   pitch )", from core/words.c) or a register's one-line help (from tables.c). Returns 0 if it is neither. */
static int word_help(const char *t, int c, char *out, int cap) {
  char w[32];
  if (!word_under_cursor(t, c, w, sizeof w)) return 0;
  for (int i = 0; i < WORD_COUNT; i++)
    if (!strcasecmp(w, WORD_TABLE[i].name)) { snprintf(out, (size_t)cap, "%s %s", WORD_TABLE[i].name, WORD_TABLE[i].stack); return 1; }
  for (int i = 0; i < NAME_COUNT; i++)
    if (!strcasecmp(w, NAME_TABLE[i].name)) { snprintf(out, (size_t)cap, "%s: %s", NAME_TABLE[i].name, regview_help(NAME_TABLE[i].name)); return 1; }
  return 0;
}

void tab_editor_draw(Desk *d, View *v) {
  Editor *e = ed(d);
  const char *t = txt(d);
  int vis_rows = v->rows - HEADER_ROWS - 2, vis_cols = v->cols;
  if (vis_rows < 1 || vis_cols < 8) return;
  const Block *blk = &d->synth->vm.blocks[e->blk];
  int len = txt_len(d);
  e->cursor = imin(e->cursor, len);

  /* 1. the grid of 16 blocks, one bordered cell each. The block being edited is filled amber; a square in the top
        right corner means it has changes you have not applied, one in the bottom right that a pulser runs it. */
  {
    int sc = d->scale, pitch = (v->cols / NUM_BLOCKS >= 3 ? 3 : 2) * v->cw;
    int bh = v->rh + 4 * sc, by = v->y + (HEADER_ROWS * v->rh - bh) / 2, bw = pitch - 2 * sc, mark = 2 * sc;
    for (int b = 0; b < NUM_BLOCKS; b++) {
      const Block *bb = &d->synth->vm.blocks[b];
      int sel = b == e->blk, dirty = strcmp(e->text[b], bb->src) != 0, x = v->x + b * pitch;
      /* a block that just ran flashes magenta and fades back over a quarter of a second */
      double age = d->now_ms - e->flash_at[b], flash = e->flash_at[b] > 0 && age < FLASH_MS ? 1.0 - age / FLASH_MS : 0.0;
      gfx_fill(&d->cv, x, by, bw, bh, gfx_mix(gfx_colour(sel ? COL_AMBD : COL_PNL), gfx_colour(COL_MAG), flash * 0.7));
      gfx_frame(&d->cv, x, by, bw, bh, gfx_mix(gfx_colour(sel ? COL_AMB : bb->halted ? COL_RED : COL_LINE), gfx_colour(COL_MAG), flash));
      char label[2] = { "0123456789ABCDEF"[b], '\0' };
      int tc = sel ? COL_AMB : bb->halted ? COL_RED : bb->c.count ? COL_FG : COL_DIM;
      gfx_text(&d->cv, x + (bw - GFX_CELL_W * sc) / 2, by + (bh - 8 * sc) / 2, label, sc, gfx_colour(tc));
      if (dirty) gfx_fill(&d->cv, x + bw - mark - sc - 1, by + sc + 1, mark, mark, gfx_colour(COL_AMB));
      if (d->synth->mem[REG_BP0 + b]) gfx_fill(&d->cv, x + bw - mark - sc - 1, by + bh - mark - sc - 1, mark, mark, gfx_colour(COL_CYN));
    }
  }

  /* 2. where the cursor is on screen. Lines wrap every `vis_cols` characters (see chunk_start). */
  e->width = vis_cols;
  int cur_row = 0;                                            /* screen rows above the cursor */
  for (int ls = 0; ls <= e->cursor; ) {
    int le = line_end(t, ls);
    if (e->cursor <= le) { cur_row += (e->cursor - ls) / vis_cols; break; }
    cur_row += (le - ls) / vis_cols + 1;
    ls = le + 1;
  }
  int cur_col = (e->cursor - line_start(t, e->cursor)) % vis_cols;
  if (cur_row < e->top) e->top = cur_row;
  if (cur_row >= e->top + vis_rows) e->top = cur_row - vis_rows + 1;

  /* 3. the text, one screen row (chunk) at a time. First find the word to flag, if any: the token that will not compile, or
        (when the text is as applied) the one that halted the block. */
  Compiled tmp;
  vm_compile(&tmp, t, BLOCK_SLOTS);                           /* a trial compile: nothing changes until the user applies */
  int dirty = strcmp(t, blk->src) != 0;
  int bad_s = -1, bad_e = -1;
  if (!tmp.ok) bad_s = tmp.err_pos;
  else if (blk->halted && !dirty && blk->fault_slot >= 0 && blk->fault_slot < blk->c.count) bad_s = blk->c.pos[blk->fault_slot];
  if (bad_s >= 0) { bad_e = bad_s; while (t[bad_e] && !isspace((unsigned char)t[bad_e]) && t[bad_e] != '\\') bad_e++; }
  int sa = 0, sb = 0, has_sel = selection(d, &sa, &sb);
  int row = 0, ls = 0;
  for (;;) {
    int le = line_end(t, ls);
    unsigned char colours[BLOCK_SRC_MAX];
    memset(colours, COL_FG, sizeof colours);
    colour_line(t, ls, le, colours, BLOCK_SRC_MAX);
    for (int cs = ls; ; cs += vis_cols, row++) {
      int sr = row - e->top;
      if (sr >= 0 && sr < vis_rows)
        for (int x = 0; x < vis_cols && cs + x < le; x++) {
          int ci = cs + x;
          if (has_sel && ci >= sa && ci < sb) view_fill(v, x, HEADER_ROWS + sr, 1, 1, COL_SEL);
          int bad = ci >= bad_s && ci < bad_e;                 /* the flagged word: red block, dark letters */
          if (bad) view_fill(v, x, HEADER_ROWS + sr, 1, 1, COL_RED);
          char ch[2] = { t[ci], '\0' };
          view_text(v, x, HEADER_ROWS + sr, ch, bad ? COL_BG : colours[ci - ls]);
        }
      if (cs + vis_cols > le) { row++; break; }               /* that was the line's last chunk */
    }
    if (!t[le] || row >= e->top + vis_rows) break;
    ls = le + 1;
  }

  /* 4. the cursor: a block that blinks. It stays solid for half a second after each key so you never lose it while
        typing. In an unfocused tile it is an outline. */
  int cx = cur_col, cy = cur_row - e->top;
  if (cy >= 0 && cy < vis_rows && (d->now_ms - e->last_key_ms < 500 || ((long)(d->now_ms / 500)) % 2 == 0)) {
    int x = v->x + cx * v->cw, y = v->y + (HEADER_ROWS + cy) * v->rh;
    if (v->focused) {
      gfx_fill(&d->cv, x, y, v->cw, v->rh, gfx_colour(COL_AMB));
      if (t[e->cursor] && t[e->cursor] != '\n') {             /* draw the character under the block in the background colour */
        char ch[2] = { t[e->cursor], '\0' };
        view_text(v, cx, HEADER_ROWS + cy, ch, COL_BG);
      }
    } else gfx_frame(&d->cv, x, y, v->cw, v->rh, gfx_colour(COL_DIM));
  }

  /* 5. the completion popup, under the word being typed (above it if there is no room below) */
  char prefix[16]; int wstart, wlen;
  const char *cand[MAX_CAND];
  if (v->focused && current_word(d, prefix, &wstart, &wlen)) {
    int n = build_candidates(prefix, cand);
    if (n > 0 && !(e->cycle && n == 1)) {         /* with one choice, accepting it closes the popup */
      int shown = imin(n, POPUP_ROWS), next = e->cycle ? e->cycle_idx : 0;   /* highlight the word now in the editor (before the first Tab, the one Tab will insert) */
      int first = next >= shown ? next - shown + 1 : 0, width = 0;
      for (int i = 0; i < n; i++) width = imax(width, (int)strlen(cand[i]));
      width += 2;
      int px = cx - wlen, py = cy + 1;
      if (py + shown > vis_rows) py = cy - shown;
      if (px + width > vis_cols) px = vis_cols - width;
      if (px < 0) px = 0;
      if (py >= 0) {
        view_fill(v, px, HEADER_ROWS + py, width, shown, COL_PNL);
        gfx_frame(&d->cv, v->x + px * v->cw - 1, v->y + (HEADER_ROWS + py) * v->rh - 1, width * v->cw + 2, shown * v->rh + 2, gfx_colour(COL_AMB));
        for (int i = 0; i < shown; i++) {
          int ci = first + i;
          if (ci == next) view_fill(v, px, HEADER_ROWS + py + i, width, 1, COL_AMBD);
          view_text(v, px + 1, HEADER_ROWS + py + i, cand[ci], ci == next ? COL_AMB : COL_FG);
        }
      }
    }
  }

  /* 6. status: size and state on one line, the problem (or a reminder) on the next */
  char line[128];
  snprintf(line, sizeof line, "B%X  %d/%d slots  %s", e->blk, tmp.ok ? tmp.count : blk->c.count, BLOCK_SLOTS, dirty ? "CHANGED" : "applied");
  gfx_fill(&d->cv, v->x, v->y + (v->rows - 2) * v->rh - 3, v->w, 1, gfx_colour(COL_LINE));
  view_text(v, 0, v->rows - 2, line, dirty ? COL_AMB : COL_DIM);
  /* the pulsers that run this block: four boxes at the right, lit magenta for each pulser that fires it (Ctrl+1 to 4 toggle) */
  for (int p = 0; p < NUM_PULSERS; p++) {
    int runs = (d->synth->mem[REG_BP0 + e->blk] >> p) & 1;
    char lab[3] = { 'P', (char)('0' + p), '\0' };
    view_cell(v, v->cols - 4 * NUM_PULSERS - 1 + 4 * p, v->rows - 2, 3, lab, runs ? COL_BG : COL_DIM, runs ? COL_MAG : COL_PNL, 0);
  }
  /* the second line, most important first: a halt, a compile error, a fault that did not stop the block, then help */
  char why[80];
  vm_fault_text(blk, why, sizeof why);                       /* e.g. "! needs 2, stack had 1" */
  if (blk->halted) {
    snprintf(line, sizeof line, "HALTED: %s", why[0] ? why : vm_fault_name(blk->fault));
    if ((int)strlen(line) + 19 <= v->cols) strcat(line, " (apply to restart)");   /* only where it fits (not on the handheld) */
    view_text(v, 0, v->rows - 1, line, COL_RED);
  }
  else if (!tmp.ok) { snprintf(line, sizeof line, "ERR %.100s", tmp.err); view_text(v, 0, v->rows - 1, line, COL_RED); }
  else if (why[0] && !dirty) view_text(v, 0, v->rows - 1, why, COL_AMB);   /* it ran on, but something went wrong (until re-applied) */
  else if (word_help(t, e->cursor, line, sizeof line)) view_text(v, 0, v->rows - 1, line, COL_CYN);   /* what the word under the cursor does */
  else view_text(v, 0, v->rows - 1, d->handheld ? (dirty ? "X applies this block" : "Menu: apply and run") : dirty ? "Ctrl+Enter applies, Ctrl+R applies and runs" : "Ctrl+R runs it once", COL_DIM);
}
