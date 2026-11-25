/* The small-screen UI: a 256x192 pixel picture, drawn by plain C into an array, driven by gamepad-style buttons.
   It is a port of the TrimUI Brick page (dev/brick_body.js). Nothing here knows about SDL, windows or keyboards:
   the platform layer turns its own input into Button events, calls ui_tick/ui_draw, and copies `fb` to the screen.
   That is what lets the same code run on the desktop, on the Brick and on a Raspberry Pi.

   The picture is an array of palette indices (see PAL in ui.c), one byte per pixel.

   Threading: call everything from one thread with the audio callback locked out; the UI reads and writes the
   synth directly. */
#ifndef FEZ_UI_H
#define FEZ_UI_H

#include "fez.h"
#include "regview.h"

#define UI_W 256
#define UI_H 192

typedef enum {
  BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_X, BTN_Y,
  BTN_L1, BTN_R1, BTN_L2, BTN_R2, BTN_START, BTN_SELECT, BTN_MENU, BTN_VOLU, BTN_VOLD, BTN_COUNT
} Button;

/* Screens, in the order L1/R1 cycles through them. */
enum { SCR_BLK, SCR_PAR, SCR_REG, SCR_PAT, SCR_PLAY, SCR_COUNT };

#define ED_MAX_TOKS 128           /* a block holds at most BLOCK_SLOTS slots, and a token is at least one slot */
#define ED_TOK_LEN  12
#define ED_UNDO     40
#define MAX_LIST    256           /* longest register/parameter/palette list */

typedef char Token[ED_TOK_LEN];

typedef struct { Token t[ED_MAX_TOKS]; int n; } TokenList;

typedef struct Ui {
  FezSynth *synth;
  unsigned char fb[UI_W * UI_H];

  /* set by the platform layer */
  float peak;                       /* recent output level 0..1, for the meter */
  int audio_ok;                     /* 1 if audio is running (the dot in the corner) */

  /* time and input */
  double now_ms;
  double last_key_ms;               /* when a button last went down (keeps the cursor solid while typing) */
  unsigned held;                    /* bit n set while button n is down */
  double repeat_at[BTN_COUNT];      /* when a held button next repeats */

  /* chrome */
  int screen;
  char msg[40]; double msg_until;   /* a message that replaces the key hints for a moment */
  int menu;                         /* -1 closed, else the highlighted item */

  /* BLK: the code editor */
  int blk;                          /* block being edited */
  TokenList toks; int cur;          /* its tokens, and the cursor position (between tokens) */
  char lit[8];                      /* a number being typed, not yet inserted */
  int focus_code;                   /* 1: cursor in the code, 0: in the palette */
  int top;                          /* first visible line */
  int pal_tab, pal_item;
  TokenList undo[ED_UNDO]; int nundo;
  TokenList clip;

  /* PAR, REG, PAT */
  int par_sel, reg_sel, pat_sel, patch;      /* patch: the last one loaded */
  RegEntry regs[MAX_LIST]; int nregs;
  int pars[MAX_LIST]; int npars;            /* addresses of the parameter registers, in table order, without GATE */

  /* PLAY */
  int octave, last_pitch, pulsers_on;

  /* palette contents that have to be built at start-up */
  const char *reg_items[MAX_LIST]; int nreg_items;
  char note_text[60][5]; const char *note_items[60];
} Ui;

void ui_init(Ui *ui, FezSynth *synth);
void ui_button(Ui *ui, Button b, int down);   /* a button went down (1) or up (0) */
void ui_tick(Ui *ui, double dt_ms);           /* advance time: key repeat, cursor blink, message timeout */
void ui_draw(Ui *ui);                         /* redraw `fb` */

/* PLAY screen support, for platforms with a keyboard: play a note (semitones above C, 0 to 16, in the current octave). */
void ui_play_note(Ui *ui, int semitone);
void ui_octave(Ui *ui, int delta);
void ui_toggle_pulsers(Ui *ui);

#endif
