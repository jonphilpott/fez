/* Drawing helpers and the editor's entry points, shared by ui.c and ui_edit.c. Not for apps. */
#ifndef FEZ_UI_PRIV_H
#define FEZ_UI_PRIV_H

#include "ui.h"
#include "palette.h"

/* Palette indices: the picture stores these, the platform layer maps them to colours (UI_PAL). */
enum { BG = COL_BG, FG = COL_FG, DIM = COL_DIM, AMB = COL_AMB, CYN = COL_CYN, RED = COL_RED, GRN = COL_GRN, PNL = COL_PNL, SEL = COL_SEL, AMBD = COL_AMBD, LINE = COL_LINE };
#define UI_PAL FEZ_PALETTE

int  ui_clamp(int x, int lo, int hi);
void ui_fill(Ui *ui, int x, int y, int w, int h, int c);
void ui_rect(Ui *ui, int x, int y, int w, int h, int c);
void ui_px(Ui *ui, int x, int y, int c);
void ui_text(Ui *ui, int x, int y, const char *s, int c);          /* 5x8 font, 5 pixels per character */
void ui_ctext(Ui *ui, int x, int w, int y, const char *s, int c);  /* centred in the box x..x+w */
void ui_bar(Ui *ui, int x, int y, int w, int h, double v, int c, int bg);
void ui_say(Ui *ui, const char *msg);                              /* show a message for about 1.6 s */
void ui_bottom_bar(Ui *ui, const char *hint);
int  ui_held(const Ui *ui, Button b);

/* the code editor (ui_edit.c) */
void edit_open_block(Ui *ui, int b);
void edit_build_palettes(Ui *ui);
void edit_draw(Ui *ui);
void edit_key(Ui *ui, Button b);
void edit_run(Ui *ui);
void edit_undo(Ui *ui);
void edit_copy(Ui *ui);
void edit_paste(Ui *ui);
void edit_clear(Ui *ui);

#endif
