/* The handheld's controls. The Brick has a D-pad, A B X Y, four shoulder buttons, Start, Select and Menu, and no
   keyboard. A Pad sits in front of the desktop UI (desk.c) and turns those buttons into the keys the desktop tabs
   already understand, so the handheld shows exactly the screens the desktop shows, at 256x192. It adds the three
   things a keyboard did that buttons cannot: a menu, a palette of words for the code editor (a program is built by
   picking words, not typing them) and a letter picker for naming a patch.

   The button map, in short:
     L1 / R1    previous / next screen            L2 / R2   previous / next page inside a screen
     D-pad      move                              Y held    coarse steps, or select (Shift)
     A / B / X  per screen: see tab_hint() in pad.c, and the menu's Help page
     Start      fire the gate                     Select    mute
     Menu       the menu                          Vol +/-   master volume
   Call every function with the audio callback locked out, like the desk's own. */
#ifndef FEZ_PAD_H
#define FEZ_PAD_H

#include "desk.h"

typedef enum {
  BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_X, BTN_Y,
  BTN_L1, BTN_R1, BTN_L2, BTN_R2, BTN_START, BTN_SELECT, BTN_MENU, BTN_VOLU, BTN_VOLD, BTN_COUNT
} Button;

#define PAD_PAL_TABS 10                /* tabs in the editor's palette of words */

typedef struct Pad {
  Desk *d;
  unsigned held;                       /* bit n is set while button n is down */
  double repeat_at[BTN_COUNT];         /* when a held button next repeats (now_ms) */
  int menu, msel, menu_top;            /* menu level (-1 closed, 0 main, 1 go to screen, 2 tab actions), the highlighted row, first row shown */
  int help, help_top;                  /* the button help page is open; its scroll */
  int pal, ref;                        /* the editor's palette of words is open; ref: opened as Reference (taller, with each word's help) */
  int ptab, psel;                      /* palette tab, and the highlighted item in it */
  char find[12];                       /* the desktop palette: the letters typed so far to find a word (empty: none) */
  int help_key; double help_since;     /* which word's help is shown (list * 1000 + item), and since when: a long one scrolls from the top */
  int letter;                          /* the patch-name letter picker: index into the letters */
  /* The palette's REGS tab lists registers under the same group headings as the CTRL screen, laid out on a grid PAL_COLS wide:
     each register has a row and a column, and each heading takes a row of its own. Built once from CTRL's own list. */
  struct { char name[12]; int row, col; } cells[MAX_LIST];
  int ncells;
  struct { int row; const char *name; } heads[40];
  int nheads, reg_rows;                /* headings, and the number of grid rows in all */
} Pad;

void pad_init(Pad *p, Desk *d);
void pad_button(Pad *p, Button b, int down);   /* a button went down (1) or up (0) */
/* An analogue stick moved. axis: 0 left X, 1 left Y, 2 right X, 3 right Y (SDL's numbering); raw: -32768..32767.
   Each axis is modulation pot MP0..MP3: centre is 16384, a push one way raises it, the other lowers it (up is up). */
void pad_stick(Pad *p, int axis, int raw);
void pad_tick(Pad *p);                         /* key repeat for held buttons; call once a frame, after desk_tick */
void pad_draw(Pad *p);                         /* after desk_draw: the menu, palette, picker and help page, and the status hint */

/* The same palette of words on the desktop: F3 in the editor opens it over the window. pad_palette_init builds the lists
   (pad_init does it too, for the handheld); the desktop calls nothing else of the pad. pad_desk_key and pad_desk_text take
   a key or a typed letter first and return 1 if the palette used it (0: give it to desk_key or desk_text as usual). */
void pad_palette_init(Pad *p, Desk *d);
int pad_desk_key(Pad *p, int key, int mods);
int pad_desk_text(Pad *p, int c);
void pad_desk_draw(Pad *p);                    /* after desk_draw, when the palette is open */

#endif
