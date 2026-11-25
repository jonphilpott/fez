/* The desktop UI: a window split into tiles, each showing one tab (patches, parameters, registers, scope ...).
   It is built for a keyboard, and for a 1024x600 screen first (a Raspberry Pi LCD): at text scale 2 that is
   about 102 columns by 33 rows. The code here draws into a Canvas and takes key events; it knows nothing about SDL.

   Keys (Ctrl or Alt works for every Ctrl chord, because some systems keep Ctrl+arrows for themselves):
     Ctrl+arrows       move focus between tiles
     Ctrl+[  Ctrl+]    previous / next tab in the focused tile
     Ctrl+Tab          next layout
     Ctrl+=  Ctrl+-    bigger / smaller text
     In the editor: Ctrl+Enter applies the block, Ctrl+R runs it once, Ctrl+, Ctrl+. change block (see tab_editor.c)
     Ctrl+G            fire the envelope     Ctrl+P  pulsers on/off     Ctrl+F  full screen     Ctrl+Q  quit
   No Home, End, Insert or Page keys are needed anywhere: Shift+Up/Down moves ten rows instead.

   Threading: call every function with the audio callback locked out. */
#ifndef FEZ_DESK_H
#define FEZ_DESK_H

#include "fez.h"
#include "regview.h"
#include "palette.h"
#include "gfx.h"
#include "patchfile.h"

/* Keys the platform layer reports. Printable characters arrive separately, through desk_text(). Letters and symbols
   that are part of a Ctrl/Alt chord arrive in desk_key() as their lower-case ASCII code. */
enum { KEY_NONE = 0, KEY_UP = 256, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_TAB, KEY_DELETE };
enum { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4 };

enum { TAB_EDITOR, TAB_PATCHES, TAB_PULSERS, TAB_SEQ, TAB_MATRIX, TAB_DEVICES, TAB_BUFFER, TAB_CTRL, TAB_SCOPE, TAB_COUNT };

#define MAX_TILES  4
#define MAX_LIST   256
#define SCOPE_RING 16384         /* samples kept for the scope; a power of two so indexes can wrap with a mask */

/* A rectangle in thousandths of the window, so a layout works at any window size. */
typedef struct { int x0, y0, x1, y1; } Frac;

typedef struct {
  const char *name;
  int ntiles;
  Frac rect[MAX_TILES];
  int tab[MAX_TILES];            /* the tab each tile shows; changed by the user and remembered per layout */
} Layout;

/* The block editor's state. Each of the 16 blocks has its own text, which only reaches the running synth when the
   user applies it (Ctrl+Enter), so a half-typed program never changes the sound. */
#define ED_UNDO 40
typedef struct { char text[BLOCK_SRC_MAX]; int cursor; } EdSnap;
typedef struct {
  char text[NUM_BLOCKS][BLOCK_SRC_MAX];
  int blk;                       /* block being edited */
  int cursor, anchor;            /* cursor position (a character index); anchor is the other end of a selection, or -1 */
  int want_col;                  /* column Up/Down try to keep */
  int top;                       /* first visible screen row (long lines wrap) */
  int width;                     /* characters per screen row, set when drawing; Up/Down need it */
  EdSnap undo[ED_UNDO]; int nundo;
  EdSnap redo[ED_UNDO]; int nredo;
  int typing;                    /* the last edit was typing, so the next character joins the same undo step */
  int cycle, cycle_idx, cycle_start, cycle_len; char cycle_prefix[16];   /* Tab is cycling through completions */
  double last_key_ms;            /* keeps the cursor solid while you type */
  int seen_runs[NUM_BLOCKS];     /* each block's run count at the last look, to notice when it runs */
  double flash_at[NUM_BLOCKS];   /* when each block last ran (now_ms), for the flash in the block grid */
} Editor;

/* A scrolling list of numbers you can step with Left/Right or type into (used by the parameter and register tabs). */
typedef struct { int sel, top; int editing; char buf[12]; int len; } ValueList;

enum { BFP_TAPE, BFP_VOICES, BFP_COUNT };                      /* the BUFFER tab's pages */
enum { BFM_NORMAL, BFM_ADJUST, BFM_TYPE };                      /* what the keys do on a voice cell */
enum { DEV_CELLULAR, DEV_LORENZ, DEV_QUANTISER, DEV_TURING, DEV_DELAY, DEV_KEYS, DEV_COUNT };                  /* the DEVICES tab's pages */
#define CA_HIST 64                                        /* generations of the automaton kept for the picture */
#define KB_HIST 480                                       /* frames of the keyboard's note trace (about 8 s) */
#define LZ_HIST 400                                       /* points of the Lorenz trace */
enum { MTX_NORMAL, MTX_PICK_SRC, MTX_PICK_DST, MTX_ADJUST, MTX_TYPE };   /* the MATRIX tab's modes */
enum { SUB_MINE, SUB_EXAMPLES };               /* the Patches tab's two lists, in tab order */
enum { PAT_NORMAL, PAT_NAMING, PAT_CONFIRM_DELETE, PAT_CONFIRM_OVERWRITE };
#define MAX_USER_PATCHES 128

typedef struct Desk {
  FezSynth *synth;
  Canvas cv;
  int scale, scale_user;         /* text scale now; and the user's own choice (0 = pick from the window size) */

  Layout layouts[3];
  int layout, focus;             /* current layout, and the focused tile within it */
  struct { int x, y, w, h; } rect[MAX_TILES];   /* the tiles in pixels, from the layout and the window size */

  char msg[160]; double msg_until;
  double now_ms;
  float peak;                    /* recent output level 0..1, for the meter (set by the platform layer) */
  int pulsers_on;                /* whether the pulsers run (Ctrl+P) */
  int rec_toggle;                /* Ctrl+T sets this; main.c starts or stops the recording (it owns the file) and clears it */
  int recording; double rec_secs;/* set by main.c while a take runs: the status bar shows REC and the time */
  volatile int muted;            /* global mute (Shift+Esc): the app fades the output to silence; the synth keeps running. Read by the audio thread, hence volatile. */
  unsigned char pul_was[NUM_PULSERS];   /* each pulser's own on/off, kept while "all off" is in force */
  char loaded_name[PATCH_NAME_MAX]; int loaded_user;   /* the patch last loaded or saved, and whether it is a user patch (not an example) */
  int kb_key_note[128];           /* the keyboard device: the note each computer key started, plus one (0 means the key is up) */
  int quit;                      /* set when the user asks to quit */

  /* the audio thread's copy of the output, for the scope */
  const float *scope;
  const volatile unsigned *scope_head;      /* total samples written so far */

  /* tab state */
  struct {                                   /* PATCHES */
    int sub;                                 /* SUB_MINE or SUB_EXAMPLES */
    int sel[2];                              /* selected row in each list (index into the filtered list) */
    char filter[32]; int flen;               /* typed letters that narrow the list */
    int mode;                                /* PAT_NORMAL, PAT_NAMING, PAT_CONFIRM_DELETE, PAT_CONFIRM_OVERWRITE */
    char name[PATCH_NAME_MAX]; int nlen;     /* the name being typed for a save */
    char dir[512];                           /* where user patches live ("" = nowhere: saving is off) */
    char names[MAX_USER_PATCHES][PATCH_NAME_MAX]; int nuser;
    PatchInfo info;                          /* description and listen text of the loaded patch, kept for the next save */
    char shown[PATCH_NAME_MAX]; PatchInfo shown_info;   /* the selected user patch's header, read from disk once, not every frame */
  } pat;
  RegEntry regs[MAX_LIST]; int nregs;
  int pars[MAX_LIST];      int npars;
  ValueList vl;                              /* CTRL: the selection and any number being typed */
  int crow[MAX_LIST + 32]; int ncrow;        /* CTRL's rows: a register index, or a group heading as -(n+1) */
  int ctl_raw;                               /* CTRL shows raw numbers (1) or friendly units (0) */
  Editor ed;
  struct { int sel; int editing; char buf[8]; int len; } seq;            /* SEQ: the selected step, and a note being typed */
  struct {                                   /* BUFFER */
    int page;                                /* BFP_TAPE or BFP_VOICES */
    int set;                                 /* TAPE: the setting under the cursor */
    int voice, cell;                         /* VOICES: the row, and the cell in it (0 pos, 1 rate, 2 length, 3 level, 4 to 7 the flags) */
    int mode; char buf[8]; int len;          /* adjusting a value, or typing one */
  } bf;
  struct {                                   /* DEVICES */
    int sub;                                 /* which device page */
    int sel[DEV_COUNT];                      /* the setting under the cursor on each page */
    int editing; char buf[8]; int len;       /* a value being typed */
    uint16_t ca_hist[CA_HIST]; int ca_n;     /* the automaton's rows as they were seen, oldest first */
    uint16_t tm_hist[CA_HIST]; int tm_n;     /* the same for the Turing machine's bits */
    int lz_x[LZ_HIST], lz_z[LZ_HIST]; int lz_n, lz_head;   /* the Lorenz trace: a ring of (X, Z) register values */
    int8_t kb_hist[KB_HIST]; int kb_n;       /* the keyboard's note trace, oldest first: the note held that frame, or -1 */
  } dev;
  struct {                                   /* MATRIX */
    int row, cell, mode;                     /* the route under the cursor; its cell (0 source, 1 destination, 2 weight); what the keys do now */
    int adding;                              /* a new route is being built: Esc while picking removes it */
    char filter[24]; int flen, psel;         /* pick lists: typed filter, and the highlighted row of the filtered list */
    char buf[8]; int len;                    /* a weight being typed */
  } mtx;
  struct { int row, field; int editing; char buf[8]; int len; int adjusting; } pul;   /* PULSERS: the cell under the cursor, and a rate being typed */

  /* the system clipboard, supplied by the platform layer (may be NULL) */
  void (*clip_set)(const char *text);
  int  (*clip_get)(char *buf, int max);      /* copies the clipboard text into buf; returns its length */
} Desk;

/* A drawing area for one tab: pixel box, and the same box in character cells. */
typedef struct {
  Desk *d;
  int x, y, w, h;
  int cols, rows;
  int cw, rh;                    /* cell size in pixels */
  int focused;
} View;

void desk_init(Desk *d, FezSynth *synth);
void desk_set_canvas(Desk *d, uint32_t *pixels, int w, int h);   /* (re)size: call at start and whenever the window changes */
void desk_set_scale(Desk *d, int scale);                          /* 0 = automatic */
void desk_key(Desk *d, int key, int mods);
void desk_text(Desk *d, int ch);                                  /* a typed printable character */
void desk_tick(Desk *d, double dt_ms);
void desk_draw(Desk *d);
void desk_say(Desk *d, const char *msg);
void desk_fire_gate(Desk *d);
void desk_toggle_pulsers(Desk *d);
void desk_reload_editor(Desk *d);
void desk_patches_refresh(Desk *d);          /* re-read the user patch folder */
void desk_patches_begin_save(Desk *d);       /* Ctrl+S: ask for a name */
void desk_patches_loaded_blank(Desk *d);     /* start from a blank patch */
void desk_patches_loaded_example(Desk *d, int index);   /* load an example patch into the synth and the UI */
void editor_watch_blocks(Desk *d);           /* once per frame: note which blocks have run since the last frame */            /* after a patch load: take every block's source from the synth */

/* drawing helpers in character cells (desk.c) */
void view_text(View *v, int col, int row, const char *s, int colour);
void view_text_right(View *v, int col, int width, int row, const char *s, int colour);
void view_cell(View *v, int col, int row, int cols, const char *label, int text_colour, int fill_colour, int cursor);
void view_fill(View *v, int col, int row, int cols, int rows, int colour);
void view_bar(View *v, int col, int row, int cols, double value, int fg, int bg);
void view_scrollbar(View *v, int top, int visible, int total, int row, int rows);
int  desk_contains_nocase(const char *hay, const char *needle);   /* does hay contain needle, ignoring case (an empty needle matches) */
int  wrap_text(const char *s, int width, char lines[][64], int max_lines);   /* returns the number of lines */

/* tabs (one file each) */
void tab_editor_draw(Desk *d, View *v);   void tab_editor_key(Desk *d, int key, int mods);   void tab_editor_text(Desk *d, int ch);
void tab_patches_draw(Desk *d, View *v);  void tab_patches_key(Desk *d, int key, int mods);  void tab_patches_text(Desk *d, int ch);
void tab_pulsers_draw(Desk *d, View *v);  void tab_pulsers_key(Desk *d, int key, int mods);  void tab_pulsers_text(Desk *d, int ch);
void tab_devices_draw(Desk *d, View *v);  void tab_devices_key(Desk *d, int key, int mods);  void tab_devices_text(Desk *d, int ch);
extern const char KB_KEYS_LOW[13], KB_KEYS_HIGH[13];   /* the keyboard device's two rows of keys, in semitone order */
int  desk_note_key(Desk *d, int ch, int down);   /* a physical key (lower-case US letter or digit) went down or up: 1 if the keyboard device took it */
void desk_all_notes_off(Desk *d);            /* let go of every key (also when the window loses focus) */
void devices_watch(Desk *d);                 /* once per frame: record what the devices are doing, for their pictures */
void tab_buffer_draw(Desk *d, View *v);   void tab_buffer_key(Desk *d, int key, int mods);   void tab_buffer_text(Desk *d, int ch);
void tab_matrix_draw(Desk *d, View *v);   void tab_matrix_key(Desk *d, int key, int mods);   void tab_matrix_text(Desk *d, int ch);
void tab_seq_draw(Desk *d, View *v);      void tab_seq_key(Desk *d, int key, int mods);      void tab_seq_text(Desk *d, int ch);
void tab_ctrl_draw(Desk *d, View *v);     void tab_ctrl_key(Desk *d, int key, int mods);     void tab_ctrl_text(Desk *d, int ch);
void ctrl_build_rows(Desk *d);            /* once, after d->regs is filled: the CTRL list with its headings */
void tab_scope_draw(Desk *d, View *v);    void tab_scope_key(Desk *d, int key, int mods);    void tab_scope_text(Desk *d, int ch);

#endif
