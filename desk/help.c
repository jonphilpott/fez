/* The F1 help screen: a full-window page that lists the keys for someone who has never used the app. It draws over the tiles
   (the synth keeps running underneath) and swallows every key except the ones that close or scroll it, so a stray press
   cannot change a patch while the page is open.

   The text lives in two plain tables, one per column. A line with no key is a heading. Descriptions are kept short (about 31
   characters) so a column is 48 characters wide. In a window too narrow for two columns, both tables are shown one after the
   other and Up/Down scroll the page. */
#include "desk.h"
#include <string.h>

typedef struct { const char *key, *text; } HelpLine;

/* ---- the left column: getting around and making sound ---- */
static const HelpLine LEFT[] = {
  { NULL, "MOVING AROUND" },
  { "Ctrl+Arrows",   "jump to the tile that way" },
  { "Ctrl+[  Ctrl+]", "previous / next tab" },
  { "[  ]",          "previous / next page in a tab" },
  { "Ctrl+Tab",      "next window layout (there are 3)" },
  { "Up / Down",     "pick a row" },
  { "Left / Right",  "change it (Shift: big steps)" },
  { "0-9, Enter",    "type an exact number, store it" },
  { "Esc",           "cancel what you are typing" },
  { "Ctrl+=  Ctrl+-", "text bigger / smaller" },
  { "Ctrl+F",        "full screen on / off" },
  { "Ctrl+Q",        "quit" },
  { "F2",            "setup: audio output, MIDI, pots" },
  { "Alt",           "works like Ctrl, everywhere" },
  { "", "" },
  { NULL, "MAKING SOUND" },
  { "Ctrl+G",        "play one note (fire the gate)" },
  { "Ctrl+P",        "pulsers on / off" },
  { "Shift+Esc",     "mute / unmute (a quick fade)" },
  { "CTRL tab",      "top row LEVEL: master volume" },
  { "KEYS tab",      "the keyboard plays notes" },
  { "  low row",     "Z S X D C V G B H N J M" },
  { "  high row",    "Q 2 W 3 E R 5 T 6 Y 7 U" },
  { "  Space",       "let go of every key" },
  { "", "" },
  { NULL, "BUFFER TAB" },
  { "Space  S",      "start / stop the picked voice" },
  { "R  F",          "record a one-shot / freeze" },
  { "C",             "empty the tape" },
};

/* ---- the right column: files, recording, the editor ---- */
static const HelpLine RIGHT[] = {
  { NULL, "PATCHES AND RECORDING" },
  { "PATCHES tab",   "Enter loads the picked patch" },
  { "Left / Right",  "MY PATCHES / EXAMPLES" },
  { "letters",       "type to filter the list" },
  { "Ctrl+S",        "save under a name (any tab)" },
  { "Ctrl+N",        "new blank patch (asks first)" },
  { "Ctrl+D",        "delete the picked patch (asks)" },
  { "Ctrl+T",        "start / stop recording to WAV" },
  { "patch files",   "~/FEZ/patches" },
  { "recordings",    "~/FEZ/recordings" },
  { "", "" },
  { NULL, "THE EDITOR" },
  { "Ctrl+Enter",    "apply the code" },
  { "Ctrl+R",        "apply this block and run it once" },
  { "Tab",           "complete the word" },
  { "F3",            "palette: find words, registers" },
  { "[  ]",          "previous / next block" },
  { "Ctrl+Z  Ctrl+Y", "undo / redo" },
  { "Ctrl+C X V",    "copy / cut / paste" },
  { "",              "brackets page, they are not text" },
  { "", "" },
  { NULL, "THE STATUS BAR" },
  { "CPU",           "audio load; the tick is the" },
  { "",              "worst of the last 3 seconds" },
  { "CLIP",          "output reached full scale" },
  { "MUTED  REC",    "shown there while on" },
  { "tab keys",      "the keys of the tab in front;" },
  { "",              "the patch name gives way to them" },
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define COL_WIDTH 48          /* characters in one column: 2 x 48 + 2 fits the 1024-wide window at the default text size */
#define KEY_WIDTH 16          /* characters before the description */

/* One line at (col, row) of the view. */
static void draw_line(View *v, const HelpLine *l, int col, int row) {
  if (!l->key) { view_text(v, col, row, l->text, COL_MAG); return; }               /* a heading */
  view_text(v, col, row, l->key, COL_CYN);
  view_text(v, col + KEY_WIDTH, row, l->text, COL_FG);
}

void desk_help_draw(Desk *d) {
  Canvas *cv = &d->cv;
  int s = d->scale, status = GFX_CELL_H * s + 4;
  View v;
  v.d = d; v.focused = 1;
  v.cw = GFX_CELL_W * s; v.rh = GFX_CELL_H * s;
  v.x = 8 * s; v.y = 4 * s; v.w = cv->w - 16 * s; v.h = cv->h - status - 8 * s;
  v.cols = v.w / v.cw; v.rows = v.h / v.rh;

  gfx_unclip(cv);
  gfx_fill(cv, 0, 0, cv->w, cv->h - status, gfx_colour(COL_BG));              /* cover the tiles, leave the status bar */
  gfx_frame(cv, 0, 0, cv->w, cv->h - status, gfx_colour(COL_AMB));
  gfx_clip(cv, 0, 0, cv->w, cv->h - status);

  view_text(&v, 0, 0, "FEZ HELP", COL_AMB);
  view_text(&v, 9, 0, "version " FEZ_VERSION, COL_DIM);
  view_text_right(&v, 10, v.cols - 10, 0, "F1 or Esc: close", COL_DIM);
  gfx_fill(cv, v.x, v.y + v.rh - 2 * s, v.w, 1, gfx_colour(COL_LINE));

  int top = 2, avail = v.rows - top;                                          /* rows under the title */
  int nl = COUNT(LEFT), nr = COUNT(RIGHT);
  if (v.cols >= 2 * COL_WIDTH + 2) {                                          /* wide: two columns side by side */
    for (int i = 0; i < nl && i < avail; i++) draw_line(&v, &LEFT[i], 0, top + i);
    for (int i = 0; i < nr && i < avail; i++) draw_line(&v, &RIGHT[i], COL_WIDTH + 2, top + i);
  } else {                                                                    /* narrow: one long page, scrolled with Up/Down */
    int total = nl + 1 + nr;
    int max_top = total > avail ? total - avail : 0;
    if (d->help_top > max_top) d->help_top = max_top;
    for (int r = 0; r < avail; r++) {
      int i = d->help_top + r;
      if (i >= total) break;
      if (i < nl) draw_line(&v, &LEFT[i], 0, top + r);
      else if (i > nl) draw_line(&v, &RIGHT[i - nl - 1], 0, top + r);
    }
    if (max_top > 0) view_text_right(&v, 0, v.cols, v.rows - 1, "Up / Down: scroll", COL_DIM);
  }
  gfx_unclip(cv);
}

/* Returns 1 if the page took the key. While it is open it takes all of them. */
int desk_help_key(Desk *d, int key) {
  if (key == KEY_F1 || key == KEY_ESC) { d->help = 0; return 1; }
  if (key == KEY_UP && d->help_top > 0) d->help_top--;
  else if (key == KEY_DOWN) d->help_top++;                                    /* the draw routine clamps it at the end */
  return 1;
}
