/* The handheld's controls: see pad.h. Everything the buttons can do ends up as a call into the desktop UI
   (desk_key, desk_text), so a fix to a desktop tab is a fix to the handheld too. */
#include "pad.h"
#include "words.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int cur_tab(const Pad *p) { return p->d->layouts[p->d->layout].tab[p->d->focus]; }
/* The analogue sticks: the same as a gamepad's on the desktop (desk_gamepad_axis in tab_devices.c). */
void pad_stick(Pad *p, int axis, int raw) { desk_gamepad_axis(p->d, axis, raw); }

static int held(const Pad *p, Button b) { return (p->held >> b) & 1; }

/* ---------------------------------------------------------------- the editor's palette of words */

/* A program is built by picking words. Each tab is a list; "NUMS" digits are glued together (typing 1 then 2 makes 12) and
   ENT ends the number; every other word is followed by a space, or a line break after the words that end a statement. */
typedef struct { const char *name; const char *const *items; int n; } PalTab;
#define LIST(a) a, (int)(sizeof a / sizeof *a)
static const char *const P_STK[] = { "DUP", "DROP", "SWAP", "OVER", "ROT", "?" };
static const char *const P_MTH[] = { "+", "-", "*", "F*", "/", "*/", "NEG", "ABS", "MIN", "MAX", "MOD", "ST", "HZ", "COLOUR", "SCALE" };
static const char *const P_LOG[] = { "AND", "OR", "XOR", "NOT", "SHL", "SHR", "ROL", "ROR", "=", "<", ">", "U<" };
static const char *const P_CHC[] = { "RND", "P" };
static const char *const P_MEM[] = { "@", "!", "TRIG", "ROUTE", "W" };
static const char *const P_FLW[] = { "L", "JMP", "JZ", "JNZ", "SKIP", "EXIT", "YIELD" };
static const char *const P_NUM[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "ENT", "100", "1000", "4096", "16384", "32767" };
/* S0..S15 are scratch registers every block shares; P0..P15 are private: each block has its own (the list was once
   called PULSERS, wrongly: P0 is a register, not pulser 0) */
static const char *const P_SCR[] = { "S0", "S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8", "S9", "S10", "S11", "S12", "S13", "S14", "S15",
  "P0", "P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8", "P9", "P10", "P11", "P12", "P13", "P14", "P15" };
static char note_text[60][5];                  /* C1 to B5, built once */
static const char *note_items[60];
static const PalTab PAL[PAD_PAL_TABS] = {
  { "STACK", LIST(P_STK) }, { "MATHS", LIST(P_MTH) }, { "CHANCE", LIST(P_CHC) }, { "LOGIC", LIST(P_LOG) }, { "MEMORY", LIST(P_MEM) }, { "FLOW", LIST(P_FLW) },
  { "NOTES", note_items, 60 }, { "NUMS", LIST(P_NUM) }, { "REGS", NULL, 0 }, { "SCRATCH", LIST(P_SCR) } };
#define PT_NOTES 6
#define PT_NUMS 7
#define PT_REGS 8
#define PAL_COLS 6

static int pal_count(const Pad *p, int tab) { return tab == PT_REGS ? p->ncells : PAL[tab].n; }
static const char *pal_item(const Pad *p, int tab, int i) {
  if (tab == PT_REGS) return p->cells[i].name;
  return PAL[tab].items[i];
}

/* After these words a new line starts, so a program reads like Forth source (the same list the old handheld editor used). */
static int ends_statement(const char *w) {
  static const char *const B[] = { "L", "!", "JMP", "JZ", "JNZ", "EXIT", "YIELD", "TRIG", "ROUTE", "W" };
  for (unsigned i = 0; i < sizeof B / sizeof *B; i++) if (!strcmp(w, B[i])) return 1;
  return 0;
}

static void type_string(Desk *d, const char *s) { for (; *s; s++) desk_text(d, *s); }

/* The editor's text is a plain string with a cursor, but on the handheld it is really a list of words, so an insertion
   must keep the words apart wherever the cursor is: a space before the new word unless one is already there, and a
   separator after it (a line break after the words that end a statement). Without this, a 1 put in front of CASTEP
   would give 1CASTEP.
   `glue` is for digits of a number being built from the NUMS tab: a digit joins the digit before it
   instead of starting a new word, and the cursor stays in front of the separator so the next digit can join too. */
static int blank(char c) { return c == ' ' || c == '\n' || c == '\0'; }

static void insert_token(Pad *p, const char *w, int glue, int newline_after) {
  Desk *d = p->d;
  const char *t = d->ed.text[d->ed.blk];
  char before = d->ed.cursor > 0 ? t[d->ed.cursor - 1] : '\0';
  int joins = glue && before >= '0' && before <= '9';
  if (!blank(before) && !joins) desk_text(d, ' ');                      /* 1) keep it apart from the word before */
  type_string(d, w);                                                   /* 2) the word itself */
  t = d->ed.text[d->ed.blk];
  char after = t[d->ed.cursor];
  if (after == ' ' || after == '\n') { if (!glue) desk_key(d, KEY_RIGHT, 0); return; }   /* 3) a separator is already there: step over it (a digit stays in front of it) */
  desk_text(d, glue ? ' ' : newline_after ? '\n' : ' ');               /*    otherwise make one ... */
  if (glue) desk_key(d, KEY_LEFT, 0);                                   /*    ... and a digit stays in front of it, so the next digit joins */
}

static void insert_word(Pad *p, const char *w) {
  Desk *d = p->d;
  if (p->ptab == PT_NUMS && !strcmp(w, "ENT")) {                        /* ENT ends a number: move on past the separator, making one if need be */
    const char *t = d->ed.text[d->ed.blk];
    char before = d->ed.cursor > 0 ? t[d->ed.cursor - 1] : '\0', after = t[d->ed.cursor];
    if (after == ' ' || after == '\n') desk_key(d, KEY_RIGHT, 0);
    else if (!blank(before)) desk_text(d, ' ');
    return;
  }
  int digit = p->ptab == PT_NUMS && strlen(w) == 1;                     /* a digit builds a number; 100, 1000 ... are whole words */
  insert_token(p, w, digit, p->ptab != PT_NUMS && ends_statement(w));
}

/* Delete back to the start of the previous word (and any blanks just before the cursor). */
static void delete_word(Desk *d) {
  if (d->ed.anchor >= 0 && d->ed.anchor != d->ed.cursor) { desk_key(d, KEY_BACKSPACE, 0); return; }   /* a selection (held Y and the D-pad): delete just that */
  const char *t = d->ed.text[d->ed.blk];
  int c = d->ed.cursor;
  while (c > 0 && (t[c - 1] == ' ' || t[c - 1] == '\n')) { desk_key(d, KEY_BACKSPACE, 0); c--; }
  while (c > 0 && t[c - 1] != ' ' && t[c - 1] != '\n') { desk_key(d, KEY_BACKSPACE, 0); c--; }
}

/* ---------------------------------------------------------------- menu */

enum { MENU_CLOSED = -1, MENU_MAIN = 0, MENU_GOTO = 1, MENU_ACTIONS = 2 };

/* What a tab can do that a plain button does not reach: each is a key the desktop tab already understands. */
typedef struct { const char *label; int key, mods, text, check; } Action;   /* text: send the key as typed text, not a key press. check: a checkbox for "this block runs on pulser (key - '1')": it shows its state, and the menu stays open after A */
static const Action A_EDITOR[]  = { { "Apply and run", 'r', MOD_CTRL, 0, 0 }, { "Undo", 'z', MOD_CTRL, 0, 0 }, { "Redo", 'y', MOD_CTRL, 0, 0 },
  { "Copy", 'c', MOD_CTRL, 0, 0 }, { "Cut", 'x', MOD_CTRL, 0, 0 }, { "Paste", 'v', MOD_CTRL, 0, 0 }, { "Clear block", 0, 0, 0, 0 }, { "Reference", KEY_F3, 0, 0, 0 },
  { "Runs on P0", '1', MOD_CTRL, 0, 1 }, { "Runs on P1", '2', MOD_CTRL, 0, 1 }, { "Runs on P2", '3', MOD_CTRL, 0, 1 }, { "Runs on P3", '4', MOD_CTRL, 0, 1 } };
static const Action A_PATCHES[] = { { "Save patch", 's', MOD_CTRL, 0, 0 }, { "Delete patch", 'd', MOD_CTRL, 0, 0 }, { "New blank patch", 'n', MOD_CTRL, 0, 0 }, { "Other list", '[', 0, 1, 0 } };
static const Action A_CTRL[]    = { { "Units / raw", KEY_TAB, 0, 0, 0 } };
static const Action A_DEVICES[] = { { "Next page", ']', 0, 1, 0 }, { "Previous page", '[', 0, 1, 0 }, { "Cellular: new row", 'n', 0, 1, 0 }, { "Cellular: single cell", 'c', 0, 1, 0 }, { "Lorenz: reset", 'r', 0, 1, 0 }, { "Turing: new bits", 'n', 0, 1, 0 } };
static const Action A_BUFFER[]  = { { "Trigger voice", ' ', 0, 1, 0 }, { "Stop voice", 's', 0, 1, 0 }, { "Record one-shot", 'r', 0, 1, 0 }, { "Freeze / run head", 'f', 0, 1, 0 }, { "Clear tape", 'c', 0, 1, 0 } };
static const Action A_PULSERS[] = { { "Fire pulser", 'f', 0, 1, 0 }, { "Rate up", '+', 0, 1, 0 }, { "Rate down", '-', 0, 1, 0 } };
static const Action A_SEQ[]     = { { "Clock one step", KEY_ENTER, 0, 0, 0 }, { "Length up", '+', 0, 1, 0 }, { "Length down", '-', 0, 1, 0 }, { "Pitch drive", 'p', 0, 1, 0 }, { "Trigger drive", 't', 0, 1, 0 }, { "Restart", 'r', 0, 1, 0 } };
static const Action A_MATRIX[]  = { { "Delete route", KEY_BACKSPACE, 0, 0, 0 } };
static const Action A_STICKS[]  = { { "Play / swell", ' ', 0, 1, 0 } };
#define ACT(a) a, (int)(sizeof a / sizeof *a)
static const struct { const Action *a; int n; } ACTIONS[TAB_COUNT] = {
  [TAB_EDITOR] = { ACT(A_EDITOR) }, [TAB_PATCHES] = { ACT(A_PATCHES) }, [TAB_CTRL] = { ACT(A_CTRL) }, [TAB_DEVICES] = { ACT(A_DEVICES) },
  [TAB_BUFFER] = { ACT(A_BUFFER) }, [TAB_PULSERS] = { ACT(A_PULSERS) }, [TAB_SEQ] = { ACT(A_SEQ) }, [TAB_MATRIX] = { ACT(A_MATRIX) },
  [TAB_KEYS] = { NULL, 0 }, [TAB_STICKS] = { ACT(A_STICKS) }, [TAB_SCOPE] = { NULL, 0 } };

static const char *const TAB_NAMES[TAB_COUNT] = { "Editor", "Patches", "Control", "Devices", "Buffer", "Pulsers", "Sequencer", "Matrix", "Keys", "Sticks", "Scope" };

/* Go to screen lists the screens this machine has (no KEYS on the handheld): row r of the list is tab goto_tab(r). */
static int goto_count(const Pad *p) { int n = 0; for (int t = 0; t < TAB_COUNT; t++) n += desk_tab_shown(p->d, t); return n; }
static int goto_tab(const Pad *p, int row) { for (int t = 0; t < TAB_COUNT; t++) if (desk_tab_shown(p->d, t) && row-- == 0) return t; return TAB_EDITOR; }
static int goto_row(const Pad *p, int tab) { int r = 0; for (int t = 0; t < tab; t++) r += desk_tab_shown(p->d, t); return r; }

/* The MENU tab's rows. (The other tab, ACTIONS, lists what the current screen can do; see ACTIONS above.) */
enum { MM_GOTO, MM_NEW, MM_RECORD, MM_PULSERS, MM_HELP, MM_QUIT, MM_CLOSE, MM_COUNT };
static const char *main_label(const Pad *p, int row) {
  switch (row) {
    case MM_GOTO: return "Go to screen";
    case MM_NEW: return "New blank patch";
    case MM_RECORD: return p->d->recording ? "Stop recording" : "Record to WAV";
    case MM_PULSERS: return p->d->pulsers_on ? "Pulsers: switch off" : "Pulsers: switch on";
    case MM_HELP: return "Button help";
    case MM_QUIT: return "Quit";
    default: return "Close";
  }
}

/* The rows in the open menu level, as a count. */
static int menu_rows(const Pad *p) {
  if (p->menu == MENU_GOTO) return goto_count(p);
  if (p->menu == MENU_ACTIONS) return ACTIONS[cur_tab(p)].n ? ACTIONS[cur_tab(p)].n : 1;     /* one row saying there is nothing, so the list is never empty */
  return MM_COUNT;
}
static const char *menu_label(const Pad *p, int row) {
  if (p->menu == MENU_GOTO) return TAB_NAMES[goto_tab(p, row)];
  if (p->menu == MENU_ACTIONS) {
    if (!ACTIONS[cur_tab(p)].n) return "(none on this screen)";
    const Action *a = &ACTIONS[cur_tab(p)].a[row];
    if (!a->check) return a->label;
    static char box[40];                                                  /* "[x] Runs on P0": the block's pulser mask bit, read live */
    int on = (p->d->synth->mem[REG_BP0 + p->d->ed.blk] >> (a->key - '1')) & 1;
    snprintf(box, sizeof box, "[%c] %s", on ? 'x' : ' ', a->label);
    return box;
  }
  return main_label(p, row);
}

static void run_action(Pad *p, const Action *a) {
  Desk *d = p->d;
  if (!a->key) { vm_set_block(&d->synth->vm, d->ed.blk, ""); desk_reload_editor(d); desk_say(d, "BLOCK CLEARED"); return; }   /* Clear block */
  if (a->key == KEY_F3) { p->pal = 1; p->ref = 1; p->psel = 0; return; }   /* Reference: the palette with each word's help, as F3 gives on the desktop */
  if (a->text) desk_text(d, a->key); else desk_key(d, a->key, a->mods);
}

static void menu_key(Pad *p, Button b) {
  Desk *d = p->d;
  int n = menu_rows(p);
  if (b == BTN_UP) p->msel = (p->msel + n - 1) % n;
  else if (b == BTN_DOWN) p->msel = (p->msel + 1) % n;
  else if ((b == BTN_LEFT || b == BTN_RIGHT || b == BTN_L1 || b == BTN_R1) && p->menu != MENU_GOTO) {     /* the two tabs: ACTIONS and MENU */
    p->menu = p->menu == MENU_MAIN ? MENU_ACTIONS : MENU_MAIN; p->msel = 0;
  }
  else if (b == BTN_B || b == BTN_MENU) { if (p->menu == MENU_GOTO) { p->menu = MENU_MAIN; p->msel = MM_GOTO; } else p->menu = MENU_CLOSED; }
  else if (b == BTN_A) {
    int row = p->msel;
    if (p->menu == MENU_GOTO) { d->layouts[d->layout].tab[d->focus] = goto_tab(p, row); p->pal = 0; p->menu = MENU_CLOSED; }
    else if (p->menu == MENU_ACTIONS) {
      if (!ACTIONS[cur_tab(p)].n) return;
      const Action *a = &ACTIONS[cur_tab(p)].a[row];
      if (!a->check) p->menu = MENU_CLOSED;                                /* a checkbox changes in place; everything else closes the menu */
      run_action(p, a);
    }
    else switch (row) {
      case MM_GOTO: p->menu = MENU_GOTO; p->msel = goto_row(p, cur_tab(p)); break;
      case MM_NEW: d->layouts[d->layout].tab[d->focus] = TAB_PATCHES; desk_patches_begin_new(d); p->pal = 0; p->menu = MENU_CLOSED; break;   /* shows the question on the Patches screen */
      case MM_RECORD: d->rec_toggle = 1; p->menu = MENU_CLOSED; break;
      case MM_PULSERS: desk_toggle_pulsers(d); p->menu = MENU_CLOSED; break;
      case MM_HELP: p->menu = MENU_CLOSED; p->help = 1; p->help_top = 0; break;
      case MM_QUIT: d->quit = 1; break;
      default: p->menu = MENU_CLOSED; break;
    }
  }
}

/* ---------------------------------------------------------------- patch names: a letter picker */

static const char LETTERS[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.";
#define NLETTERS ((int)(sizeof LETTERS - 1))

/* Left/Right move one letter (the picker is drawn as a row, so they move the way it looks), Up/Down jump five.
   A adds the letter, B removes the last one, X saves, Y gives up. */
static void name_key(Pad *p, Button b) {
  Desk *d = p->d;
  if (b == BTN_LEFT) p->letter = (p->letter + NLETTERS - 1) % NLETTERS;
  else if (b == BTN_RIGHT) p->letter = (p->letter + 1) % NLETTERS;
  else if (b == BTN_UP) p->letter = (p->letter + NLETTERS - 5) % NLETTERS;
  else if (b == BTN_DOWN) p->letter = (p->letter + 5) % NLETTERS;
  else if (b == BTN_A) desk_text(d, LETTERS[p->letter]);
  else if (b == BTN_B) desk_key(d, KEY_BACKSPACE, 0);
  else if (b == BTN_X) desk_key(d, KEY_ENTER, 0);
  else if (b == BTN_Y) desk_key(d, KEY_ESC, 0);
}

/* ---------------------------------------------------------------- palette keys */

static void palette_key(Pad *p, Button b) {
  Desk *d = p->d;
  int n = pal_count(p, p->ptab);
  if (b == BTN_LEFT) p->psel = (p->psel + n - 1) % n;
  else if (b == BTN_RIGHT) p->psel = (p->psel + 1) % n;
  else if (b == BTN_UP || b == BTN_DOWN) {
    int dir = b == BTN_DOWN ? 1 : -1;
    if (p->ptab == PT_REGS) {                         /* the register grid has headings, so rows are not simply n / PAL_COLS: find the nearest cell in the next row up or down */
      int row = p->cells[p->psel].row, col = p->cells[p->psel].col;
      for (int r = row + dir; r >= 0 && r < p->reg_rows; r += dir) {
        int best = -1, bestd = 0;
        for (int i = 0; i < p->ncells; i++)
          if (p->cells[i].row == r) { int dd = abs(p->cells[i].col - col); if (best < 0 || dd < bestd) { best = i; bestd = dd; } }
        if (best >= 0) { p->psel = best; break; }     /* (a heading row has no cells, so it is skipped) */
      }
    } else if (dir < 0 ? p->psel - PAL_COLS >= 0 : p->psel + PAL_COLS < n) p->psel += dir * PAL_COLS;
  }
  else if (b == BTN_L2 || b == BTN_R2) { p->ptab = (p->ptab + (b == BTN_R2 ? 1 : PAD_PAL_TABS - 1)) % PAD_PAL_TABS; p->psel = 0; }
  else if (b == BTN_A) insert_word(p, pal_item(p, p->ptab, p->psel));
  else if (b == BTN_B) p->pal = 0;
  else if (b == BTN_X) desk_key(d, KEY_ENTER, MOD_CTRL);           /* apply the block */
  else if (b == BTN_Y) delete_word(d);
}

/* ---------------------------------------------------------------- the button help page */

static const char *const HELP_LINES[] = {
  "BUTTONS",
  "L1 / R1    previous / next screen",
  "L2 / R2    previous / next page",
  "D-pad      move, and change values",
  "Y (held)   take bigger steps / select",
  "A B X      the main actions (shown",
  "           at the bottom of the screen)",
  "Start      fire the gate (play a note)",
  "Select     mute / unmute",
  "Menu       the menu. Two tabs (Left/Right or",
  "           L1/R1): this screen's actions, and",
  "           new patch, go to, record, help, quit",
  "Vol + / -  master volume",
  "Sticks     modulation pots MP0 to MP3",
  "",
  "SCREENS",
  "Editor     build programs from words",
  "Patches    load, save (X), delete (B)",
  "Control    every register, in units",
  "Devices    cellular, Lorenz, quantiser,",
  "           Turing and delay",
  "Buffer     the tape and its voices",
  "Pulsers    four timers that run blocks",
  "Sequencer  eight steps",
  "Matrix     modulation routes",
  "Play       A B X Y fire the pulsers",
  "Scope      the output waveform",
};
#define HELP_N ((int)(sizeof HELP_LINES / sizeof *HELP_LINES))
#define HELP_ROWS 15

static void help_key(Pad *p, Button b) {
  if (b == BTN_UP) p->help_top = fez_clamp(p->help_top - 1, 0, HELP_N - HELP_ROWS);
  else if (b == BTN_DOWN) p->help_top = fez_clamp(p->help_top + 1, 0, HELP_N - HELP_ROWS);
  else if (b == BTN_B || b == BTN_MENU || b == BTN_A || b == BTN_START) p->help = 0;
}

/* ---------------------------------------------------------------- the buttons on an ordinary screen */

static void say_volume(Pad *p) {
  char m[48], v[16];
  regview_fmt_pct(v, sizeof v, p->d->synth->master_level);
  snprintf(m, sizeof m, "VOLUME %s", v);
  desk_say(p->d, m);
}

/* A, B and X are the three buttons whose job depends on the screen; the others do the same everywhere. */
static void screen_key(Pad *p, Button b) {
  Desk *d = p->d;
  int tab = cur_tab(p), shift = held(p, BTN_Y) && tab != TAB_STICKS ? MOD_SHIFT : 0;   /* on STICKS, Y is a pulser */
  switch (b) {
    case BTN_UP:    desk_key(d, KEY_UP, shift); return;
    case BTN_DOWN:  desk_key(d, KEY_DOWN, shift); return;
    case BTN_LEFT:  desk_key(d, KEY_LEFT, shift); return;
    case BTN_RIGHT: desk_key(d, KEY_RIGHT, shift); return;
    case BTN_L2:    if (tab != TAB_STICKS) desk_text(d, '['); return;   /* STICKS has no pages: R2 is held for its settings */
    case BTN_R2:    if (tab != TAB_STICKS) desk_text(d, ']'); return;
    default: break;
  }
  int overlay_q = tab == TAB_PATCHES && d->pat.mode != PAT_NORMAL;   /* a question (overwrite? delete?) is open: A says yes, B no */
  switch (tab) {
    case TAB_EDITOR:
      if (b == BTN_A) { p->pal = 1; p->ref = 0; p->psel = 0; }
      else if (b == BTN_B) delete_word(d);
      else if (b == BTN_X) desk_key(d, KEY_ENTER, MOD_CTRL);
      break;
    case TAB_PATCHES:
      if (b == BTN_A) desk_key(d, KEY_ENTER, 0);
      else if (b == BTN_B) desk_key(d, overlay_q ? KEY_ESC : 'd', overlay_q ? 0 : MOD_CTRL);
      else if (b == BTN_X) desk_key(d, 's', MOD_CTRL);
      break;
    case TAB_CTRL:
      if (b == BTN_A) { desk_key(d, KEY_RIGHT, shift); desk_key(d, KEY_ENTER, 0); }   /* step up; on a write-only register Enter fires it */
      else if (b == BTN_B) desk_key(d, KEY_LEFT, shift);
      else if (b == BTN_X) desk_key(d, KEY_TAB, 0);
      break;
    case TAB_MATRIX:
      if (b == BTN_A) desk_key(d, KEY_ENTER, 0);
      else if (b == BTN_B) desk_key(d, KEY_ESC, 0);
      else if (b == BTN_X) desk_key(d, KEY_BACKSPACE, 0);
      break;
    case TAB_STICKS:
      if (b >= BTN_A && b <= BTN_Y) d->synth->vm.pending |= 1u << (b - BTN_A);  /* A B X Y: pulsers 0 to 3, on the next tick, as TRIG does */
      break;
    case TAB_SCOPE: break;
    default:                                                                     /* devices, buffer, pulsers, sequencer */
      if (b == BTN_A) desk_key(d, KEY_ENTER, 0);
      else if (b == BTN_B) desk_key(d, KEY_ESC, 0);
      else if (b == BTN_X) desk_text(d, ' ');
      break;
  }
}

static void press(Pad *p, Button b) {
  Desk *d = p->d;
  if (p->help) { help_key(p, b); return; }
  if (p->menu != MENU_CLOSED) { menu_key(p, b); return; }
  if (b == BTN_MENU) { p->menu = ACTIONS[cur_tab(p)].n ? MENU_ACTIONS : MENU_MAIN; p->msel = 0; return; }     /* opens on this screen's actions, if it has any */
  if (b == BTN_SELECT) { d->muted = !d->muted; return; }
  if (b == BTN_VOLU || b == BTN_VOLD) {
    d->synth->master_level = fez_clamp(d->synth->master_level + (b == BTN_VOLU ? 1 : -1) * 1500, 0, 32767);
    say_volume(p);
    return;
  }
  if (b == BTN_L1 || b == BTN_R1) {
    p->pal = 0;
    if (d->pat.mode == PAT_NAMING) desk_key(d, KEY_ESC, 0);                    /* leaving the Patches tab drops an unfinished name */
    desk_key(d, b == BTN_R1 ? ']' : '[', MOD_CTRL);
    return;
  }
  if (cur_tab(p) == TAB_PATCHES && d->pat.mode == PAT_NAMING) { name_key(p, b); return; }
  if (p->pal && cur_tab(p) == TAB_EDITOR) { palette_key(p, b); return; }
  if (b == BTN_START) { desk_fire_gate(d); return; }
  screen_key(p, b);
}

static const char *tab_hint(const Pad *p);

/* ---------------------------------------------------------------- set-up, repeat, input */

void pad_init(Pad *p, Desk *d) {
  for (int i = 0; i < 4; i++) fez_set_pot(d->synth, i, 16384);      /* the sticks start at rest, which sends no event until they move */
  pad_palette_init(p, d);
}

void pad_palette_init(Pad *p, Desk *d) {
  memset(p, 0, sizeof *p);
  p->help_key = -1;
  p->d = d; p->menu = MENU_CLOSED;
  static const char *const NN[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
  for (int i = 0; i < 60; i++) { snprintf(note_text[i], sizeof note_text[i], "%s%d", NN[i % 12], 1 + i / 12); note_items[i] = note_text[i]; }
  int row = 0, col = 0;                                     /* lay the registers out under CTRL's headings */
  for (int i = 0; i < d->ncrow; i++) {
    int c = d->crow[i];
    if (c < 0) {                                            /* a heading: a row of its own */
      if (col > 0) { row++; col = 0; }
      if (p->nheads < 40) { p->heads[p->nheads].row = row; p->heads[p->nheads].name = ctrl_heading_name(-c - 1); p->nheads++; }
      row++;
    } else if (p->ncells < MAX_LIST) {
      snprintf(p->cells[p->ncells].name, sizeof p->cells[0].name, "%.11s", d->regs[c].name);
      p->cells[p->ncells].row = row; p->cells[p->ncells].col = col; p->ncells++;
      if (++col == PAL_COLS) { col = 0; row++; }
    }
  }
  p->reg_rows = row + (col > 0);
}

/* On the STICKS screen, with nothing open over it and R2 not held, the D-pad plays (desk_gamepad_dpad) instead of moving. */
static int dpad_plays(const Pad *p, Button b) {
  if (b != BTN_UP && b != BTN_DOWN && b != BTN_LEFT && b != BTN_RIGHT) return 0;
  return cur_tab(p) == TAB_STICKS && p->menu == MENU_CLOSED && !p->help && !p->pal && !held(p, BTN_R2);
}

/* The d-pad repeats when held; so do A and B on the Control screen, where they step a value. */
static int repeatable(const Pad *p, Button b) {
  if (p->menu != MENU_CLOSED || p->help || p->pal) return b == BTN_UP || b == BTN_DOWN || b == BTN_LEFT || b == BTN_RIGHT;
  if (b == BTN_UP || b == BTN_DOWN || b == BTN_LEFT || b == BTN_RIGHT) return !dpad_plays(p, b);   /* a held note does not repeat */
  return cur_tab(p) == TAB_CTRL && (b == BTN_A || b == BTN_B);
}

void pad_button(Pad *p, Button b, int down) {
  if (dpad_plays(p, b) || (!down && cur_tab(p) == TAB_STICKS && (p->d->dev.dpad & (b == BTN_UP ? DPAD_UP : b == BTN_RIGHT ? DPAD_RIGHT : b == BTN_DOWN ? DPAD_DOWN : b == BTN_LEFT ? DPAD_LEFT : 0)))) {
    int bit = b == BTN_UP ? DPAD_UP : b == BTN_RIGHT ? DPAD_RIGHT : b == BTN_DOWN ? DPAD_DOWN : DPAD_LEFT;
    if (down) p->held |= 1u << b; else p->held &= ~(1u << b);
    desk_gamepad_dpad(p->d, bit, down);                      /* (a release always reaches it, even with R2 held by then, or the note would hang) */
    return;
  }
  if (!down) { p->held &= ~(1u << b); return; }
  if (held(p, b)) return;                                    /* the operating system's own key repeat: ignore, we do our own */
  p->held |= 1u << b;
  press(p, b);
  p->repeat_at[b] = p->d->now_ms + 380;
}

void pad_tick(Pad *p) {
  p->d->hint_override = tab_hint(p);                        /* the status bar's button hints for whatever is on screen now */
  for (int b = 0; b < BTN_COUNT; b++)
    if (held(p, (Button)b) && repeatable(p, (Button)b) && p->d->now_ms >= p->repeat_at[b]) { press(p, (Button)b); p->repeat_at[b] = p->d->now_ms + 70; }
}

/* ---------------------------------------------------------------- drawing the overlays */

static const char *tab_hint(const Pad *p) {
  const Desk *d = p->d;
  if (p->help) return "Up/Down scroll  B close";
  if (p->menu == MENU_GOTO) return "Up/Down  A choose  B back";
  if (p->menu != MENU_CLOSED) return "L/R tab  A choose  B close";
  if (cur_tab(p) == TAB_PATCHES && d->pat.mode == PAT_NAMING) return "L/R A add B del X ok Y no";
  if (cur_tab(p) == TAB_PATCHES && d->pat.mode != PAT_NORMAL) return "A yes  B no";
  if (p->pal && cur_tab(p) == TAB_EDITOR) return "A insert B close X apply Y del";
  switch (cur_tab(p)) {
    case TAB_EDITOR:  return "A words  B del  X apply  Y select";
    case TAB_PATCHES: return "A load  X save  B delete  L2 list";
    case TAB_CTRL:    return "A/B +/-  Y coarse  X units";
    case TAB_DEVICES:                       /* X is the page's main action (see action() in tab_devices.c) */
      switch (d->dev.sub) {
        case DEV_CELLULAR: case DEV_TURING: return "A act  X step  L2/R2 page";
        case DEV_QUANTISER:                 return "A act  X on/off  L2/R2 page";
        case DEV_DELAY:                     return "A act  X freeze  L2/R2 page";
        default:                            return "A act  X restart  L2/R2 page";   /* Lorenz */
      }
    case TAB_BUFFER:  return "A adjust  X trigger  L2/R2 page";
    case TAB_PULSERS: return "A edit  X toggle  Y coarse";
    case TAB_SEQ:     return "A clock  X on/off  Y snap";
    case TAB_MATRIX:  return "A pick  B back  X delete  Y coarse";
    case TAB_STICKS:  return "Dpad play R2+Dpad set ABXY P0-3";
    default:          return "Menu: more";
  }
}

/* A framed panel on the dark background. */
static void panel(Desk *d, int x, int y, int w, int h) {
  gfx_fill(&d->cv, x, y, w, h, gfx_colour(COL_BG));
  gfx_frame(&d->cv, x, y, w, h, gfx_colour(COL_AMB));
}

static void draw_menu(Pad *p) {
  Desk *d = p->d; Canvas *cv = &d->cv;
  int n = menu_rows(p), vis = n < 12 ? n : 12, rh = 10, w = 150, h = vis * rh + 16, x = (cv->w - w) / 2, y = 8;
  p->menu_top = fez_clamp(p->msel - vis / 2, 0, n - vis);
  panel(d, x, y, w, h);
  if (p->menu == MENU_GOTO) gfx_text(cv, x + 6, y + 4, "GO TO SCREEN", 1, gfx_colour(COL_MAG));
  else {                                                                  /* two tabs; the open one is lit */
    /* drawn like the main tab bar: the open tab sits on a filled block, the other is dim. The first tab carries the screen's own name. */
    const char *names[2] = { desk_tab_name(cur_tab(p)), "MENU" };
    int tx = x + 4;
    for (int t = 0; t < 2; t++) {
      int on = t == (p->menu == MENU_ACTIONS ? 0 : 1), tw = ((int)strlen(names[t]) + 1) * GFX_CELL_W;
      if (on) gfx_fill(cv, tx, y + 2, tw, 10, gfx_colour(COL_AMBD));
      gfx_text(cv, tx + GFX_CELL_W / 2, y + 4, names[t], 1, gfx_colour(on ? COL_AMB : COL_DIM));
      tx += tw + 4;
    }
    if (p->menu == MENU_MAIN) {                                             /* on the MENU tab: which FEZ this is, at the right */
      const char *v = "v" FEZ_VERSION;
      gfx_text(cv, x + w - 4 - (int)strlen(v) * GFX_CELL_W, y + 4, v, 1, gfx_colour(COL_DIM));
    }
  }
  for (int i = 0; i < vis; i++) {
    int row = p->menu_top + i, yy = y + 14 + i * rh, sel = row == p->msel;
    if (sel) gfx_fill(cv, x + 2, yy - 1, w - 4, rh, gfx_colour(COL_AMBD));
    gfx_text(cv, x + 8, yy, menu_label(p, row), 1, gfx_colour(sel ? COL_AMB : COL_FG));
  }
}

/* One word's help for the desktop palette: a word of the language gives its stack picture and what it does (words.c), a
   register its line from HELP_TABLE, a note its pitch; the NUMS list explains itself. The same tables feed the manual. */
static void word_help(const Pad *p, int tab, int i, char *out, size_t n) {
  const char *w = pal_item(p, tab, i);
  out[0] = 0;
  if (tab == PT_NUMS) {
    if (!strcmp(w, "ENT")) snprintf(out, n, "ENT ends the number being built, so the next digit starts a new one.");
    else if (strlen(w) == 1) snprintf(out, n, "The digit %s. Digits put in one after another join into one number; ENT ends it.", w);
    else snprintf(out, n, "The number %s.", w);
    return;
  }
  if (tab == PT_NOTES) {                                         /* NOTES: C1 is MIDI note 24, and a pitch is 256 to a semitone */
    snprintf(out, n, "%s: a pitch, %d (MIDI note %d times 256). Store it in PITCH to play it, or add ST semitones to it.", w, (24 + i) * 256, 24 + i);
    return;
  }
  if (tab == PAD_PAL_TABS - 1) {                            /* SCRATCH */
    if (w[0] == 'S') snprintf(out, n, "%s: a scratch register. Every block sees the same S0 to S15, so blocks can leave each other numbers here.", w);
    else snprintf(out, n, "%s: a private register. Each block has its own P0 to P15, for a number kept between its runs (a step counter, say).", w);
    return;
  }
  for (int k = 0; k < WORD_COUNT; k++)
    if (!strcmp(WORD_TABLE[k].name, w)) { snprintf(out, n, "%s %s  %s", w, WORD_TABLE[k].stack, WORD_TABLE[k].text); return; }
  for (int k = 0; k < HELP_COUNT; k++)
    if (!strcmp(HELP_TABLE[k].name, w)) { snprintf(out, n, "%s: %s", w, HELP_TABLE[k].text); return; }
}

/* s is the text scale (1 on the handheld, the desktop's own size there) and rows the number of rows of words shown.
   The handheld draws it full width over its status bar; the desktop over the bottom of the editor's tile (its status
   lines included), with a line of keys at the bottom. */
static void draw_palette(Pad *p, int s, int rows) {
  Desk *d = p->d; Canvas *cv = &d->cv;
  int desk = !d->handheld, n = pal_count(p, p->ptab), cw = 41 * s, rh = 10 * s;
  int help = desk ? 3 : p->ref ? 5 : 0;                     /* lines for the highlighted word's help: the desktop, and the handheld's Reference */
  int h = rows * rh + 24 * s + (help ? (help + 1) * rh : 0); /* (+1: the line of keys under the help) */
  int x = 4, w = cv->w - 8, y = cv->h - 13 - h;
  if (desk) {                                               /* at the bottom of the tile, or at its top if the cursor is down there */
    x = d->rect[d->focus].x; w = d->rect[d->focus].w; y = d->rect[d->focus].y + d->rect[d->focus].h - h;
    if (d->ed.cur_y >= 0 && d->ed.cur_y + rh > y) y = d->rect[d->focus].y;
  }
  panel(d, x, y, w, h);
  char t[64];
  if (!desk) snprintf(t, sizeof t, "%s  (L2/R2 tab)", PAL[p->ptab].name);
  else snprintf(t, sizeof t, "%s %d/%d", PAL[p->ptab].name, p->ptab + 1, PAD_PAL_TABS);
  gfx_text(cv, x + 4 * s, y + 3 * s, t, s, gfx_colour(COL_MAG));
  if (desk) {                                               /* the letters typed so far, on the right of the title */
    char f[32]; snprintf(f, sizeof f, "find: %s_", p->find);
    gfx_text(cv, x + w - 4 * s - (int)strlen(f) * GFX_CELL_W * s, y + 3 * s, f, s, gfx_colour(p->find[0] ? COL_AMB : COL_DIM));
  }
  if (help) {                                               /* the keys along the bottom */
    gfx_text(cv, x + 4 * s, y + h - rh, desk ? "type: find  Tab: list  Enter: put in  Esc: close" : "A put in  L2/R2 list  B close", s, gfx_colour(COL_DIM));
    /* the highlighted word's help, wrapped onto its lines over the key line, under a rule; a longer one scrolls
       (view_lines_scroll, as on the PATCHES tab), from the top each time another word is highlighted */
    char text[512]; word_help(p, p->ptab, p->psel, text, sizeof text);
    int key = p->ptab * 1000 + p->psel, hy = y + h - (help + 1) * rh;
    if (key != p->help_key) { p->help_key = key; p->help_since = d->now_ms; }
    gfx_fill(cv, x + 4 * s, hy - 3 * s, w - 8 * s, 1, gfx_colour(COL_LINE));
    View hv = { d, x + 4 * s, hy, w - 8 * s, help * rh, 0, help, GFX_CELL_W * s, rh, 1 };
    hv.cols = hv.w / hv.cw;
    static char lines[24][64]; int colours[24];
    int nl = wrap_text(text, hv.cols, lines, 24);
    for (int i = 0; i < nl; i++) colours[i] = i == 0 ? COL_CYN : COL_FG;
    view_lines_scroll(&hv, 0, 0, help, lines, colours, nl, p->help_since);
  }
  if (p->ptab == PT_REGS) {                                 /* grouped under CTRL's headings: each cell and heading has its own grid row */
    int sel_row = p->cells[p->psel].row, top = sel_row - rows / 2;
    if (top > p->reg_rows - rows) top = p->reg_rows - rows;
    if (top < 0) top = 0;
    for (int i = 0; i < p->nheads; i++) {
      int r = p->heads[i].row - top;
      if (r < 0 || r >= rows) continue;
      int hy = y + 16 * s + r * rh, len = (int)strlen(p->heads[i].name);
      gfx_text(cv, x + 4 * s, hy, p->heads[i].name, s, gfx_colour(COL_MAG));
      gfx_fill(cv, x + 4 * s + (len + 1) * GFX_CELL_W * s, hy + 4 * s, w - 12 * s - (len + 1) * GFX_CELL_W * s, 1, gfx_colour(COL_LINE));
    }
    for (int i = 0; i < p->ncells; i++) {
      int r = p->cells[i].row - top;
      if (r < 0 || r >= rows) continue;
      int cx = x + 4 * s + p->cells[i].col * cw, cy = y + 16 * s + r * rh, sel = i == p->psel;
      if (sel) gfx_fill(cv, cx - 1, cy - 1, cw - 1, rh, gfx_colour(COL_AMBD));
      gfx_text(cv, cx, cy, p->cells[i].name, s, gfx_colour(sel ? COL_AMB : COL_FG));
    }
    return;
  }
  int top = (p->psel / PAL_COLS - rows / 2) < 0 ? 0 : p->psel / PAL_COLS - rows / 2;
  int total_rows = (n + PAL_COLS - 1) / PAL_COLS;
  if (top > total_rows - rows) top = total_rows > rows ? total_rows - rows : 0;
  for (int i = top * PAL_COLS; i < n && i < (top + rows) * PAL_COLS; i++) {
    int cx = x + 4 * s + (i % PAL_COLS) * cw, cy = y + 16 * s + (i / PAL_COLS - top) * rh, sel = i == p->psel;
    if (sel) gfx_fill(cv, cx - 1, cy - 1, cw - 1, rh, gfx_colour(COL_AMBD));
    gfx_text(cv, cx, cy, pal_item(p, p->ptab, i), s, gfx_colour(sel ? COL_AMB : COL_FG));
  }
}

static void draw_name(Pad *p) {
  Desk *d = p->d; Canvas *cv = &d->cv;
  int w = cv->w - 8, h = 46, x = 4, y = cv->h - 13 - h;
  panel(d, x, y, w, h);
  gfx_text(cv, x + 4, y + 3, "SAVE PATCH AS", 1, gfx_colour(COL_MAG));
  char buf[PATCH_NAME_MAX + 2];
  snprintf(buf, sizeof buf, "%s_", d->pat.name);
  gfx_text(cv, x + 4, y + 15, buf, 1, gfx_colour(COL_AMB));
  /* the picker: the letter under the cursor in the middle, its neighbours either side */
  for (int k = -8; k <= 8; k++) {
    int i = (p->letter + k + NLETTERS * 4) % NLETTERS;
    char c[2] = { LETTERS[i] == ' ' ? '_' : LETTERS[i], 0 };
    gfx_text(cv, x + w / 2 - 2 + k * 10, y + 30, c, 1, gfx_colour(k == 0 ? COL_AMB : COL_DIM));
  }
  gfx_frame(cv, x + w / 2 - 5, y + 28, 11, 11, gfx_colour(COL_AMB));
}

static void draw_help(Pad *p) {
  Desk *d = p->d; Canvas *cv = &d->cv;
  panel(d, 4, 2, cv->w - 8, cv->h - 13 - 4);
  for (int i = 0; i < HELP_ROWS && p->help_top + i < HELP_N; i++) {
    const char *l = HELP_LINES[p->help_top + i];
    int head = l[0] >= 'A' && l[0] <= 'Z' && l[1] >= 'A' && l[1] <= 'Z';          /* a line that starts with two capitals is a heading */
    gfx_text(cv, 10, 8 + i * 10, l, 1, gfx_colour(head ? COL_MAG : COL_FG));
  }
}

/* A question with two answers, in the middle of the screen: "Start a blank patch?" and the like. A is yes and B is no, as the hint line says. */
static void draw_confirm(Pad *p) {
  Desk *d = p->d; Canvas *cv = &d->cv;
  const char *title = "", *line1 = "", *line2 = "";
  char buf[PATCH_NAME_MAX + 8];
  switch (d->pat.mode) {
    case PAT_CONFIRM_NEW:       title = "START A BLANK PATCH?"; line1 = "This patch will be discarded."; line2 = "Unsaved changes are lost."; break;
    case PAT_CONFIRM_DELETE:    title = "DELETE PATCH?"; snprintf(buf, sizeof buf, "\"%.30s\"", d->pat.name); line1 = buf; line2 = "It cannot be brought back."; break;
    case PAT_CONFIRM_OVERWRITE: title = "REPLACE PATCH?"; snprintf(buf, sizeof buf, "\"%.30s\"", d->pat.name); line1 = buf; line2 = "A patch with that name exists."; break;
    default: return;
  }
  int w = 192, h = 64, x = (cv->w - w) / 2, y = (cv->h - h) / 2;
  panel(d, x, y, w, h);
  gfx_text(cv, x + 8, y + 6, title, 1, gfx_colour(COL_RED));
  gfx_text(cv, x + 8, y + 20, line1, 1, gfx_colour(COL_FG));
  gfx_text(cv, x + 8, y + 30, line2, 1, gfx_colour(COL_DIM));
  /* the two answers, drawn as the buttons they are */
  gfx_fill(cv, x + 12, y + 44, 80, 14, gfx_colour(COL_AMBD)); gfx_text(cv, x + 20, y + 47, "A  Yes", 1, gfx_colour(COL_AMB));
  gfx_frame(cv, x + 100, y + 44, 80, 14, gfx_colour(COL_DIM)); gfx_text(cv, x + 108, y + 47, "B  No", 1, gfx_colour(COL_FG));
}

void pad_draw(Pad *p) {
  Desk *d = p->d;
  gfx_unclip(&d->cv);
  if (p->help) draw_help(p);
  else if (cur_tab(p) == TAB_PATCHES && d->pat.mode == PAT_NAMING) draw_name(p);
  else if (cur_tab(p) == TAB_PATCHES && d->pat.mode != PAT_NORMAL) draw_confirm(p);
  else if (p->pal && cur_tab(p) == TAB_EDITOR) draw_palette(p, 1, p->ref ? 7 : 4);   /* Reference: taller, with the help */
  if (p->menu != MENU_CLOSED) draw_menu(p);
}

/* ---------------------------------------------------------------- the palette on the desktop */

/* Find the first word that starts with the letters typed so far: in the open list first, then in the others in order,
   so typing CU finds CUTOFF in REGS and COL finds COLOUR in MATHS. Upper and lower case are the same. Returns 0 if none. */
static int starts_with(const char *w, const char *f) {
  for (; *f; f++, w++) if (toupper((unsigned char)*w) != toupper((unsigned char)*f)) return 0;
  return 1;
}
static int find_word(Pad *p) {
  for (int k = 0; k < PAD_PAL_TABS; k++) {
    int tab = (p->ptab + k) % PAD_PAL_TABS;
    for (int i = 0; i < pal_count(p, tab); i++)
      if (starts_with(pal_item(p, tab, i), p->find)) { p->ptab = tab; p->psel = i; return 1; }
  }
  return 0;
}

int pad_desk_key(Pad *p, int key, int mods) {
  Desk *d = p->d;
  if (d->help || d->setup_open || cur_tab(p) != TAB_EDITOR) { p->pal = 0; return 0; }   /* the palette belongs to the editor */
  if (key == KEY_F3) { p->pal = !p->pal; p->find[0] = 0; return 1; }                 /* F3 opens and closes it */
  if (!p->pal) return 0;
  if (mods & (MOD_CTRL | MOD_ALT)) return 0;                     /* Ctrl+Enter applies, Ctrl+Z undoes ... as usual */
  if (key == KEY_ESC && (mods & MOD_SHIFT)) return 0;            /* Shift+Esc still mutes */
  if (key == KEY_F1 || key == KEY_F2) return 0;
  size_t nf = strlen(p->find);
  switch (key) {
    case KEY_ESC:   p->pal = 0; break;
    case KEY_UP:    palette_key(p, BTN_UP); break;
    case KEY_DOWN:  palette_key(p, BTN_DOWN); break;
    case KEY_LEFT:  palette_key(p, BTN_LEFT); break;
    case KEY_RIGHT: palette_key(p, BTN_RIGHT); break;
    case KEY_TAB:   palette_key(p, (mods & MOD_SHIFT) ? BTN_L2 : BTN_R2); break;   /* the next list (Shift: the one before) */
    case KEY_ENTER: palette_key(p, BTN_A); break;                                  /* put the word in at the cursor */
    case KEY_BACKSPACE:                                          /* takes back a typed letter; with none, deletes the word before the cursor */
      if (nf > 0) { p->find[nf - 1] = 0; if (nf > 1) find_word(p); }
      else palette_key(p, BTN_Y);
      return 1;
    default: return 1;                                           /* any other key does nothing while it is open */
  }
  p->find[0] = 0;                                                /* moving or putting a word in starts a new search */
  return 1;
}

int pad_desk_text(Pad *p, int c) {
  if (!p->pal || cur_tab(p) != TAB_EDITOR) return 0;
  if (c == '[' || c == ']') { palette_key(p, c == ']' ? BTN_R2 : BTN_L2); p->find[0] = 0; return 1; }   /* as on the handheld's L2 / R2 */
  if (c == ' ') return 1;
  size_t nf = strlen(p->find);
  if (nf + 1 < sizeof p->find) {
    p->find[nf] = (char)toupper(c); p->find[nf + 1] = 0;
    if (!find_word(p)) p->find[nf] = 0;                          /* no word starts so: the letter is not taken */
  }
  return 1;
}

void pad_desk_draw(Pad *p) {
  if (!p->pal || cur_tab(p) != TAB_EDITOR || p->d->help || p->d->setup_open) return;
  gfx_unclip(&p->d->cv);
  draw_palette(p, p->d->scale, 6);
}
