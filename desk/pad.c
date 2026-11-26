/* The handheld's controls: see pad.h. Everything the buttons can do ends up as a call into the desktop UI
   (desk_key, desk_text), so a fix to a desktop tab is a fix to the handheld too. */
#include "pad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
static int cur_tab(const Pad *p) { return p->d->layouts[p->d->layout].tab[p->d->focus]; }
/* ---------------------------------------------------------------- the analogue sticks */

#define STICK_DEADZONE 4096           /* a stick at rest wanders a little: inside this the pot sits exactly at its centre */

void pad_stick(Pad *p, int axis, int raw) {
  if (axis < 0 || axis > 3) return;
  int v = raw < -32767 ? -32767 : raw;               /* keep -v in range */
  if (axis & 1) v = -v;                              /* SDL's Y is negative upwards; the pot should rise when you push up */
  /* 1) take off the dead zone and stretch what is left to the full range, so the pot leaves its centre smoothly */
  int mag = v < 0 ? -v : v;
  mag = mag <= STICK_DEADZONE ? 0 : (int)((long)(mag - STICK_DEADZONE) * 32767 / (32767 - STICK_DEADZONE));
  v = v < 0 ? -mag : mag;
  /* 2) -32767..32767 becomes the pot's 0..32767, centre 16384 */
  fez_set_pot(p->d->synth, axis, 16384 + v / 2);
}

static int held(const Pad *p, Button b) { return (p->held >> b) & 1; }

/* ---------------------------------------------------------------- the editor's palette of words */

/* A program is built by picking words. Each tab is a list; "NUMS" digits are glued together (typing 1 then 2 makes 12) and
   ENT ends the number; every other word is followed by a space, or a line break after the words that end a statement. */
typedef struct { const char *name; const char *const *items; int n; } PalTab;
#define LIST(a) a, (int)(sizeof a / sizeof *a)
static const char *const P_STK[] = { "DUP", "DROP", "SWAP", "OVER", "ROT", "?" };
static const char *const P_MTH[] = { "+", "-", "*", "F*", "/", "*/", "NEG", "ABS", "MIN", "MAX", "MOD", "ST", "HZ", "COLOUR", "SCALE" };
static const char *const P_LOG[] = { "AND", "OR", "XOR", "NOT", "SHL", "SHR", "ROL", "ROR", "=", "<", ">", "U<" };
static const char *const P_MEM[] = { "@", "!", "TRIG", "ROUTE", "W", "RND" };
static const char *const P_FLW[] = { "L", "JMP", "JZ", "JNZ", "SKIP", "EXIT", "YIELD" };
static const char *const P_NUM[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "ENT", "100", "1000", "4096", "16384", "32767" };
static const char *const P_PLS[] = { "P0", "P1", "P2", "P3" };
static char note_text[60][5];                  /* C1 to B5, built once */
static const char *note_items[60];
static const PalTab PAL[PAD_PAL_TABS] = {
  { "STACK", LIST(P_STK) }, { "MATHS", LIST(P_MTH) }, { "LOGIC", LIST(P_LOG) }, { "MEMORY", LIST(P_MEM) }, { "FLOW", LIST(P_FLW) },
  { "NOTES", note_items, 60 }, { "NUMS", LIST(P_NUM) }, { "REGS", NULL, 0 }, { "PULSERS", LIST(P_PLS) } };
#define PT_NUMS 6
#define PT_REGS 7
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
static const Action A_EDITOR[]  = { { "Run block once", 'r', MOD_CTRL, 0, 0 }, { "Undo", 'z', MOD_CTRL, 0, 0 }, { "Redo", 'y', MOD_CTRL, 0, 0 },
  { "Copy", 'c', MOD_CTRL, 0, 0 }, { "Cut", 'x', MOD_CTRL, 0, 0 }, { "Paste", 'v', MOD_CTRL, 0, 0 }, { "Clear block", 0, 0, 0, 0 },
  { "Runs on P0", '1', MOD_CTRL, 0, 1 }, { "Runs on P1", '2', MOD_CTRL, 0, 1 }, { "Runs on P2", '3', MOD_CTRL, 0, 1 }, { "Runs on P3", '4', MOD_CTRL, 0, 1 } };
static const Action A_PATCHES[] = { { "Save patch", 's', MOD_CTRL, 0, 0 }, { "Delete patch", 'd', MOD_CTRL, 0, 0 }, { "New blank patch", 'n', MOD_CTRL, 0, 0 }, { "Other list", '[', 0, 1, 0 } };
static const Action A_CTRL[]    = { { "Units / raw", KEY_TAB, 0, 0, 0 } };
static const Action A_DEVICES[] = { { "Next page", ']', 0, 1, 0 }, { "Previous page", '[', 0, 1, 0 }, { "Cellular: new row", 'n', 0, 1, 0 }, { "Cellular: single cell", 'c', 0, 1, 0 }, { "Lorenz: reset", 'r', 0, 1, 0 }, { "Turing: new bits", 'n', 0, 1, 0 } };
static const Action A_BUFFER[]  = { { "Trigger voice", ' ', 0, 1, 0 }, { "Stop voice", 's', 0, 1, 0 }, { "Record one-shot", 'r', 0, 1, 0 }, { "Freeze / run head", 'f', 0, 1, 0 }, { "Clear tape", 'c', 0, 1, 0 } };
static const Action A_PULSERS[] = { { "Fire pulser", 'f', 0, 1, 0 }, { "Rate up", '+', 0, 1, 0 }, { "Rate down", '-', 0, 1, 0 } };
static const Action A_SEQ[]     = { { "Clock one step", KEY_ENTER, 0, 0, 0 }, { "Length up", '+', 0, 1, 0 }, { "Length down", '-', 0, 1, 0 }, { "Pitch drive", 'p', 0, 1, 0 }, { "Trigger drive", 't', 0, 1, 0 }, { "Restart", 'r', 0, 1, 0 } };
static const Action A_MATRIX[]  = { { "Delete route", KEY_BACKSPACE, 0, 0, 0 } };
static const Action A_PLAY[]    = { { "Clock sequencer", 's', 0, 1, 0 } };
#define ACT(a) a, (int)(sizeof a / sizeof *a)
static const struct { const Action *a; int n; } ACTIONS[TAB_COUNT] = {
  [TAB_EDITOR] = { ACT(A_EDITOR) }, [TAB_PATCHES] = { ACT(A_PATCHES) }, [TAB_CTRL] = { ACT(A_CTRL) }, [TAB_DEVICES] = { ACT(A_DEVICES) },
  [TAB_BUFFER] = { ACT(A_BUFFER) }, [TAB_PULSERS] = { ACT(A_PULSERS) }, [TAB_SEQ] = { ACT(A_SEQ) }, [TAB_MATRIX] = { ACT(A_MATRIX) },
  [TAB_PLAY] = { ACT(A_PLAY) }, [TAB_SCOPE] = { NULL, 0 } };

static const char *const TAB_NAMES[TAB_COUNT] = { "Editor", "Patches", "Control", "Devices", "Buffer", "Pulsers", "Sequencer", "Matrix", "Play", "Scope" };

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
  if (p->menu == MENU_GOTO) return TAB_COUNT;
  if (p->menu == MENU_ACTIONS) return ACTIONS[cur_tab(p)].n ? ACTIONS[cur_tab(p)].n : 1;     /* one row saying there is nothing, so the list is never empty */
  return MM_COUNT;
}
static const char *menu_label(const Pad *p, int row) {
  if (p->menu == MENU_GOTO) return TAB_NAMES[row];
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
    if (p->menu == MENU_GOTO) { d->layouts[d->layout].tab[d->focus] = row; p->pal = 0; p->menu = MENU_CLOSED; }
    else if (p->menu == MENU_ACTIONS) {
      if (!ACTIONS[cur_tab(p)].n) return;
      const Action *a = &ACTIONS[cur_tab(p)].a[row];
      if (!a->check) p->menu = MENU_CLOSED;                                /* a checkbox changes in place; everything else closes the menu */
      run_action(p, a);
    }
    else switch (row) {
      case MM_GOTO: p->menu = MENU_GOTO; p->msel = cur_tab(p); break;
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

/* Up/Down move one letter, Left/Right five. A adds the letter, B removes the last one, X saves, Y gives up. */
static void name_key(Pad *p, Button b) {
  Desk *d = p->d;
  if (b == BTN_UP) p->letter = (p->letter + NLETTERS - 1) % NLETTERS;
  else if (b == BTN_DOWN) p->letter = (p->letter + 1) % NLETTERS;
  else if (b == BTN_LEFT) p->letter = (p->letter + NLETTERS - 5) % NLETTERS;
  else if (b == BTN_RIGHT) p->letter = (p->letter + 5) % NLETTERS;
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
  if (b == BTN_UP) p->help_top = clampi(p->help_top - 1, 0, HELP_N - HELP_ROWS);
  else if (b == BTN_DOWN) p->help_top = clampi(p->help_top + 1, 0, HELP_N - HELP_ROWS);
  else if (b == BTN_B || b == BTN_MENU || b == BTN_A || b == BTN_START) p->help = 0;
}

/* ---------------------------------------------------------------- the buttons on an ordinary screen */

static void say_volume(Pad *p) {
  char m[48], v[16];
  regview_fmt_pct(v, sizeof v, p->d->synth->level_pot);
  snprintf(m, sizeof m, "VOLUME %s", v);
  desk_say(p->d, m);
}

/* A, B and X are the three buttons whose job depends on the screen; the others do the same everywhere. */
static void screen_key(Pad *p, Button b) {
  Desk *d = p->d;
  int tab = cur_tab(p), shift = held(p, BTN_Y) && tab != TAB_PLAY ? MOD_SHIFT : 0;
  switch (b) {
    case BTN_UP:    desk_key(d, KEY_UP, shift); return;
    case BTN_DOWN:  desk_key(d, KEY_DOWN, shift); return;
    case BTN_LEFT:  desk_key(d, KEY_LEFT, shift); return;
    case BTN_RIGHT: desk_key(d, KEY_RIGHT, shift); return;
    case BTN_L2:    desk_text(d, '['); return;
    case BTN_R2:    desk_text(d, ']'); return;
    default: break;
  }
  int overlay_q = tab == TAB_PATCHES && d->pat.mode != PAT_NORMAL;   /* a question (overwrite? delete?) is open: A says yes, B no */
  switch (tab) {
    case TAB_EDITOR:
      if (b == BTN_A) { p->pal = 1; p->psel = 0; }
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
    case TAB_PLAY:
      if (b >= BTN_A && b <= BTN_Y) desk_text(d, '1' + (b - BTN_A));            /* A B X Y: pulsers 0 to 3 */
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
    d->synth->level_pot = clampi(d->synth->level_pot + (b == BTN_VOLU ? 1 : -1) * 1500, 0, 32767);
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
  memset(p, 0, sizeof *p);
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

/* The d-pad repeats when held; so do A and B on the Control screen, where they step a value. */
static int repeatable(const Pad *p, Button b) {
  if (p->menu != MENU_CLOSED || p->help || p->pal) return b == BTN_UP || b == BTN_DOWN || b == BTN_LEFT || b == BTN_RIGHT;
  if (b == BTN_UP || b == BTN_DOWN || b == BTN_LEFT || b == BTN_RIGHT) return 1;
  return cur_tab(p) == TAB_CTRL && (b == BTN_A || b == BTN_B);
}

void pad_button(Pad *p, Button b, int down) {
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
  if (cur_tab(p) == TAB_PATCHES && d->pat.mode == PAT_NAMING) return "Up/Dn A add B del X ok Y no";
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
    case TAB_PLAY:    return "A B X Y fire pulsers P0 to P3";
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
  p->menu_top = clampi(p->msel - vis / 2, 0, n - vis);
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
  }
  for (int i = 0; i < vis; i++) {
    int row = p->menu_top + i, yy = y + 14 + i * rh, sel = row == p->msel;
    if (sel) gfx_fill(cv, x + 2, yy - 1, w - 4, rh, gfx_colour(COL_AMBD));
    gfx_text(cv, x + 8, yy, menu_label(p, row), 1, gfx_colour(sel ? COL_AMB : COL_FG));
  }
}

static void draw_palette(Pad *p) {
  Desk *d = p->d; Canvas *cv = &d->cv;
  int n = pal_count(p, p->ptab), rows = 4, cw = 41, rh = 10, x = 4, h = rows * rh + 24, y = cv->h - 13 - h, w = cv->w - 8;
  panel(d, x, y, w, h);
  char t[64]; snprintf(t, sizeof t, "%s  (L2/R2 tab)", PAL[p->ptab].name);
  gfx_text(cv, x + 4, y + 3, t, 1, gfx_colour(COL_MAG));
  if (p->ptab == PT_REGS) {                                 /* grouped under CTRL's headings: each cell and heading has its own grid row */
    int sel_row = p->cells[p->psel].row, top = sel_row - rows / 2;
    if (top > p->reg_rows - rows) top = p->reg_rows - rows;
    if (top < 0) top = 0;
    for (int i = 0; i < p->nheads; i++) {
      int r = p->heads[i].row - top;
      if (r < 0 || r >= rows) continue;
      int hy = y + 16 + r * rh, len = (int)strlen(p->heads[i].name);
      gfx_text(cv, x + 4, hy, p->heads[i].name, 1, gfx_colour(COL_MAG));
      gfx_fill(cv, x + 4 + (len + 1) * GFX_CELL_W, hy + 4, w - 12 - (len + 1) * GFX_CELL_W, 1, gfx_colour(COL_LINE));
    }
    for (int i = 0; i < p->ncells; i++) {
      int r = p->cells[i].row - top;
      if (r < 0 || r >= rows) continue;
      int cx = x + 4 + p->cells[i].col * cw, cy = y + 16 + r * rh, sel = i == p->psel;
      if (sel) gfx_fill(cv, cx - 1, cy - 1, cw - 1, rh, gfx_colour(COL_AMBD));
      gfx_text(cv, cx, cy, p->cells[i].name, 1, gfx_colour(sel ? COL_AMB : COL_FG));
    }
    return;
  }
  int top = (p->psel / PAL_COLS - rows / 2) < 0 ? 0 : p->psel / PAL_COLS - rows / 2;
  int total_rows = (n + PAL_COLS - 1) / PAL_COLS;
  if (top > total_rows - rows) top = total_rows > rows ? total_rows - rows : 0;
  for (int i = top * PAL_COLS; i < n && i < (top + rows) * PAL_COLS; i++) {
    int cx = x + 4 + (i % PAL_COLS) * cw, cy = y + 16 + (i / PAL_COLS - top) * rh, sel = i == p->psel;
    if (sel) gfx_fill(cv, cx - 1, cy - 1, cw - 1, rh, gfx_colour(COL_AMBD));
    gfx_text(cv, cx, cy, pal_item(p, p->ptab, i), 1, gfx_colour(sel ? COL_AMB : COL_FG));
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
  else if (p->pal && cur_tab(p) == TAB_EDITOR) draw_palette(p);
  if (p->menu != MENU_CLOSED) draw_menu(p);
}
