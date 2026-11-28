/* The desktop UI's frame: layouts and tiles, focus, tabs, the status bar and the keys that work everywhere.
   What goes inside a tile is in tab_*.c. */
#include "desk.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- the tabs */

typedef struct {
  const char *name;
  void (*draw)(Desk *, View *);
  void (*key)(Desk *, int, int);
  void (*text)(Desk *, int);
  const char *hint;             /* shown in the status bar while the tab is focused */
} TabDef;

static const TabDef TABS[TAB_COUNT] = {
  { "EDITOR",  tab_editor_draw,  tab_editor_key,  tab_editor_text,  "Ctrl+Enter apply  Ctrl+R apply+run  [ ] block  Tab complete  F3 words" },
  { "PATCHES", tab_patches_draw, tab_patches_key, tab_patches_text, "Up/Down pick  [ ] mine/examples  Enter load  Ctrl+S save  Ctrl+D delete  type to filter" },
  { "CTRL",    tab_ctrl_draw,    tab_ctrl_key,    tab_ctrl_text,    "Up/Down pick  Left/Right change (Shift: coarse)  type a number, Enter  Tab units/raw" },
  { "DEVICES", tab_devices_draw, tab_devices_key, tab_devices_text, "[ ] device  Up/Down setting  Left/Right change  Space step/reseed/freeze" },
  { "BUFFER",  tab_buffer_draw,  tab_buffer_key,  tab_buffer_text,  "[ ] tape/voices  Space trigger  S stop  R record  F freeze  C clear" },
  { "PULSERS", tab_pulsers_draw, tab_pulsers_key, tab_pulsers_text, "Left/Right cell  Space toggle  Enter on BPM: adjust  type BPM, Enter  F fire" },
  { "SEQ",     tab_seq_draw,     tab_seq_key,     tab_seq_text,     "Up/Down step  Left/Right value (Shift: snap to note)  Space on/off  type a note" },
  { "MATRIX",  tab_matrix_draw,  tab_matrix_key,  tab_matrix_text,  "Left/Right cell  Enter pick/adjust  type a weight  Backspace delete route" },
  { "KEYS",    tab_keys_draw,    tab_keys_key,    tab_keys_text,    "Z S X D C V... and Q 2 W 3 E... play  Space all off  Up/Down Left/Right settings" },
  { "STICKS",  tab_sticks_draw,  tab_sticks_key,  tab_sticks_text,  "left stick plays  A B X Y fire P0 to P3" },
  { "SCOPE",   tab_scope_draw,   tab_scope_key,   tab_scope_text,   "the output waveform, triggered on a rising edge" },
};

/* ---------------------------------------------------------------- layouts */

/* Four ways to cut the window up, named 1 to 4 like workspaces in a tiling window manager. Rectangles are in thousandths of
   the window (see Frac). Ctrl+Tab cycles through them. Each tile starts on a sensible tab and can be changed with Ctrl+[ ]. */
static void init_layouts(Desk *d) {
  Layout one   = { "LAYOUT 1", 3, { { 0, 0, 580, 1000 }, { 580, 0, 1000, 400 }, { 580, 400, 1000, 1000 } }, { TAB_EDITOR, TAB_SCOPE, TAB_CTRL } };     /* a tall tile and two small ones */
  Layout two   = { "LAYOUT 2", 2, { { 0, 0, 500, 1000 }, { 500, 0, 1000, 1000 } }, { TAB_EDITOR, TAB_CTRL } };                                             /* a 50/50 split */
  Layout three = { "LAYOUT 3", 3, { { 0, 0, 1000, 340 }, { 0, 340, 500, 1000 }, { 500, 340, 1000, 1000 } }, { TAB_SCOPE, TAB_CTRL, TAB_DEVICES } };      /* a wide strip over two halves */
  /* Three equal columns for a big or very wide screen: the editor, the CTRL list, and the scope over the devices. */
  Layout four  = { "LAYOUT 4", 4, { { 0, 0, 340, 1000 }, { 340, 0, 670, 1000 }, { 670, 0, 1000, 400 }, { 670, 400, 1000, 1000 } }, { TAB_EDITOR, TAB_CTRL, TAB_SCOPE, TAB_DEVICES } };
  d->layouts[0] = one; d->layouts[1] = two; d->layouts[2] = three; d->layouts[3] = four;
  Layout hand = { "HAND", 1, { { 0, 0, 1000, 1000 } }, { TAB_PATCHES } };   /* the handheld: one tile, the whole screen */
  d->layouts[LAYOUT_HAND] = hand;
}

static int status_height(const Desk *d) { return GFX_CELL_H * d->scale + 4; }

/* Turn the layout's fractions into pixel rectangles. Neighbouring tiles share an edge value, so there are no gaps. */
static void compute_rects(Desk *d) {
  const Layout *l = &d->layouts[d->layout];
  int area_h = d->cv.h - status_height(d);
  for (int i = 0; i < l->ntiles; i++) {
    d->rect[i].x = d->cv.w * l->rect[i].x0 / 1000;
    d->rect[i].y = area_h * l->rect[i].y0 / 1000;
    d->rect[i].w = d->cv.w * l->rect[i].x1 / 1000 - d->rect[i].x;
    d->rect[i].h = area_h * l->rect[i].y1 / 1000 - d->rect[i].y;
  }
}

/* The handheld: LAYOUT_HAND, and nothing in the keys cycles away from it (Ctrl+Tab wraps within the desktop layouts). */
void desk_set_handheld(Desk *d) { d->handheld = 1; d->layout = LAYOUT_HAND; d->focus = 0; compute_rects(d); }

/* ---------------------------------------------------------------- set-up */

void desk_init(Desk *d, FezSynth *synth) {
  memset(d, 0, sizeof *d);
  d->synth = synth;
  d->scale = 1; d->pulsers_on = 1;
  d->dev.stk_dir = d->dev.stk_note = -1;                  /* the stick: nothing held */
  setup_defaults(&d->setup);
  init_layouts(d);
  d->nregs = regview_list(d->regs, MAX_LIST);
  d->npars = regview_params(d->pars, MAX_LIST);
  ctrl_build_rows(d);
  desk_patches_loaded_blank(d);
  desk_reload_editor(d);
}

void desk_set_scale(Desk *d, int scale) {
  d->scale_user = scale < 0 ? 0 : scale > 4 ? 4 : scale;
  d->scale = d->scale_user ? d->scale_user : (d->cv.w / 480 < 1 ? 1 : d->cv.w / 480 > 4 ? 4 : d->cv.w / 480);
  compute_rects(d);
}

void desk_set_canvas(Desk *d, uint32_t *pixels, int w, int h) {
  d->cv.px = pixels; d->cv.w = w; d->cv.h = h;
  gfx_unclip(&d->cv);
  desk_set_scale(d, d->scale_user);      /* 1024 wide gives scale 2: about 102 columns, as the Pi screen wants */
}

int desk_contains_nocase(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  for (; *hay; hay++) {
    size_t i = 0;
    while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
    if (i == n) return 1;
  }
  return n == 0;
}

void desk_say(Desk *d, const char *msg) { snprintf(d->msg, sizeof d->msg, "%s", msg); d->msg_until = d->now_ms + 1600; }
static void kb_swell_tick(Desk *d, double dt_ms);   /* below, with the keys */
void desk_tick(Desk *d, double dt_ms) {
  d->now_ms += dt_ms; editor_watch_blocks(d); devices_watch(d);
  kb_swell_tick(d, dt_ms);
  if (d->layouts[d->layout].tab[d->focus] != TAB_STICKS) {
    if (d->dev.stk_dir >= 0 || d->dev.stk_note >= 0 || d->dev.stk_swell || d->dev.dpad) tab_devices_stick_release(d);   /* STICKS left the front while playing: let go */
  } else tab_devices_dpad_tick(d, dt_ms);
}
/* The tab's name as the tab bar shows it ("EDITOR", "SEQ", ...). */
const char *desk_tab_name(int tab) { return TABS[tab].name; }

void desk_fire_gate(Desk *d) { fez_reg_write(d->synth, REG_GATE, 1); }

void desk_toggle_pulsers(Desk *d) {
  d->pulsers_on = !d->pulsers_on;
  for (int i = 0; i < NUM_PULSERS; i++) {                     /* going off, remember each pulser's own state; coming back, restore it */
    if (!d->pulsers_on) d->pul_was[i] = (unsigned char)d->synth->pulsers[i].on;
    d->synth->pulsers[i].on = d->pulsers_on && d->pul_was[i];
  }
  desk_say(d, d->pulsers_on ? "PULSERS ON" : "PULSERS OFF");
}

/* ---------------------------------------------------------------- drawing helpers (character cells) */

void view_text(View *v, int col, int row, const char *s, int colour) {
  gfx_text(&v->d->cv, v->x + col * v->cw, v->y + row * v->rh, s, v->d->scale, gfx_colour(colour));
}

void view_text_right(View *v, int col, int width, int row, const char *s, int colour) {
  int len = (int)strlen(s);
  if (len > width) { s += len - width; len = width; }
  view_text(v, col + width - len, row, s, colour);
}

/* A small box with a centred label, in the cell grid: col, row and width in cells. The outline is amber under the cursor. */
void view_cell(View *v, int col, int row, int cols, const char *label, int text_colour, int fill_colour, int cursor) {
  int s = v->d->scale, x = v->x + col * v->cw, y = v->y + row * v->rh, w = cols * v->cw - 2 * s, h = v->rh - 2 * s;
  gfx_fill(&v->d->cv, x, y + s, w, h, gfx_colour(fill_colour));
  gfx_frame(&v->d->cv, x, y + s, w, h, gfx_colour(cursor ? COL_AMB : COL_LINE));
  int len = (int)strlen(label);
  gfx_text(&v->d->cv, x + (w - len * GFX_CELL_W * s) / 2, y + s + (h - 8 * s) / 2, label, s, gfx_colour(text_colour));
}

void view_fill(View *v, int col, int row, int cols, int rows, int colour) {
  gfx_fill(&v->d->cv, v->x + col * v->cw, v->y + row * v->rh, cols * v->cw, rows * v->rh, gfx_colour(colour));
}

/* A horizontal bar one text row high (with a little air above and below). value is 0..1. */
void view_bar(View *v, int col, int row, int cols, double value, int fg, int bg) {
  int s = v->d->scale, x = v->x + col * v->cw, y = v->y + row * v->rh + 2 * s, w = cols * v->cw, h = v->rh - 4 * s;
  value = value < 0 ? 0 : value > 1 ? 1 : value;
  gfx_fill(&v->d->cv, x, y, w, h, gfx_colour(bg));
  gfx_fill(&v->d->cv, x, y, (int)(value * w + 0.5), h, gfx_colour(fg));
}

/* A thin scrollbar on the right edge of the view, next to `rows` rows that show `visible` of `total` items. */
void view_scrollbar(View *v, int top, int visible, int total, int row, int rows) {
  int x = v->x + v->w - 4, y = v->y + row * v->rh, h = rows * v->rh;
  gfx_fill(&v->d->cv, x, y, 4, h, gfx_colour(COL_SEL));
  if (total <= visible) return;
  int th = h * visible / total; if (th < 8) th = 8;
  int ty = y + (h - th) * top / (total - visible);
  gfx_fill(&v->d->cv, x, ty, 4, th, gfx_colour(COL_AMB));
}

/* Text too long for its space scrolls by itself, so nothing has to be cut off: 3 s still at the top (time to start
   reading), then up one line every 1.2 s, 3 s still at the end, and back to the top. The time is d->now_ms, so the
   drawing needs no state of its own: the caller only says when this text first showed. */
#define SCROLL_HOLD_MS 3000.0
#define SCROLL_LINE_MS 1200.0
void view_lines_scroll(View *v, int col, int row, int rows, char lines[][64], const int *colours, int n, double since_ms) {
  if (rows <= 0) return;
  int extra = n - rows, top = 0;                     /* extra: how many lines do not fit */
  if (extra > 0) {
    double cycle = 2 * SCROLL_HOLD_MS + extra * SCROLL_LINE_MS;
    double t = fmod(v->d->now_ms - since_ms, cycle);
    if (t < 0) t = 0;
    top = t < SCROLL_HOLD_MS ? 0 : (int)((t - SCROLL_HOLD_MS) / SCROLL_LINE_MS);
    if (top > extra) top = extra;
  }
  for (int i = 0; i < rows && top + i < n; i++) view_text(v, col, row + i, lines[top + i], colours[top + i]);
}

/* Break text into lines of at most `width` characters at spaces. Returns the number of lines (at most max_lines). */
int wrap_text(const char *s, int width, char lines[][64], int max_lines) {
  int n = 0, len = 0;
  if (width > 63) width = 63;
  if (max_lines > 0) lines[0][0] = '\0';
  while (*s && max_lines > 0) {
    while (*s == ' ') s++;
    char word[64]; int k = 0;
    while (*s && *s != ' ' && k < 63) word[k++] = *s++;
    word[k] = '\0';
    if (!k) break;
    if (len > 0 && len + 1 + k > width) { if (n + 1 >= max_lines) return n + 1; n++; lines[n][0] = '\0'; len = 0; }
    if (len > 0) lines[n][len++] = ' ';
    if (len + k > 63) k = 63 - len;
    memcpy(lines[n] + len, word, (size_t)k); len += k; lines[n][len] = '\0';
  }
  return len > 0 || n > 0 ? n + 1 : 0;
}

/* ---------------------------------------------------------------- drawing */

/* The handheld's picture for a screen of screen_w x screen_h pixels. Pixel text only looks right at a whole-number scale (at
   2.5x some pixels come out twice as wide as others), so: the largest whole scale that still leaves at least 256x192, the
   size the handheld screens were laid out for, and then a picture of screen / scale, so the screen is filled rather than
   bordered. The screens are drawn in proportion, so a picture larger than 256x192 just gets more room. Examples: the Brick,
   1024x768: 4x, 256x192. 720x480: 2x, 360x240. 640x480: 2x, 320x240. 320x240: 1x, 320x240. A screen smaller than 256x192
   still gets 256x192 at 1x (cropped by SDL; no such device is known). */
void desk_hand_size(int screen_w, int screen_h, int *w, int *h, int *scale) {
  int k = screen_w / 256 < screen_h / 192 ? screen_w / 256 : screen_h / 192;
  if (k < 1) k = 1;
  *scale = k;
  *w = screen_w / k < 256 ? 256 : screen_w / k;
  *h = screen_h / k < 192 ? 192 : screen_h / k;
}

/* KEYS plays the computer keyboard as a piano, which a handheld does not have; STICKS plays a gamepad's left stick: always
   on the handheld, and on the desktop while a gamepad is plugged in. */
int desk_tab_shown(const Desk *d, int t) { return t == TAB_KEYS ? !d->handheld : t == TAB_STICKS ? d->handheld || d->gamepads > 0 : 1; }

static void draw_tile(Desk *d, int i) {
  Canvas *cv = &d->cv;
  int tab = d->layouts[d->layout].tab[i], focused = i == d->focus;
  int x = d->rect[i].x, y = d->rect[i].y, w = d->rect[i].w, h = d->rect[i].h;
  int s = d->scale, th = GFX_CELL_H * s + 3;                       /* title bar height */

  gfx_clip(cv, x, y, w, h);
  gfx_fill(cv, x, y, w, h, gfx_colour(COL_BG));

  /* 1. the title bar: the tabs that fit, always including the showing one. When some are hidden, "<" and ">" mark which side.
     Each tab is its name plus one cell of padding. We start with the showing tab and add its neighbours (right first, then
     left) for as long as the bar has room. */
  gfx_fill(cv, x, y, w, th, gfx_colour(COL_PNL));
  int list[TAB_COUNT], n = 0, at = 0;                              /* the tabs this machine has, and where the showing one is among them */
  for (int t = 0; t < TAB_COUNT; t++) if (desk_tab_shown(d, t) || t == tab) { if (t == tab) at = n; list[n++] = t; }   /* the showing tab always (a gamepad unplugged while STICKS shows) */
  int cw = GFX_CELL_W * s, room = (w - 8) / cw;                    /* the bar's width in cells */
  int first = at, last = at, used = (int)strlen(TABS[tab].name) + 1;
  for (int grew = 1; grew;) {
    grew = 0;
    if (last + 1 < n && used + (int)strlen(TABS[list[last + 1]].name) + 1 <= room - (first > 0 ? 1 : 0)) { last++; used += (int)strlen(TABS[list[last]].name) + 1; grew = 1; }
    if (first > 0 && used + (int)strlen(TABS[list[first - 1]].name) + 1 <= room - (last + 1 < n ? 1 : 0)) { first--; used += (int)strlen(TABS[list[first]].name) + 1; grew = 1; }
  }
  int px = x + 4;
  if (first > 0) { gfx_text(cv, px, y + 2, "<", s, gfx_colour(COL_DIM)); px += cw; }
  for (int i = first; i <= last; i++) {
    int t = list[i], len = (int)strlen(TABS[t].name), tw = (len + 1) * cw;
    if (t == tab) gfx_fill(cv, px, y + 1, tw, th - 1, gfx_colour(focused ? COL_AMBD : COL_SEL));
    gfx_text(cv, px + cw / 2, y + 2, TABS[t].name, s, gfx_colour(t == tab ? (focused ? COL_AMB : COL_FG) : COL_DIM));
    px += tw;
  }
  if (last + 1 < n) gfx_text(cv, px, y + 2, ">", s, gfx_colour(COL_DIM));
  gfx_fill(cv, x, y + th - 1, w, 1, gfx_colour(focused ? COL_AMB : COL_LINE));   /* the line under the tabs: amber on the focused tile */

  /* 2. the tab's own content, in a view inside the tile */
  View v;
  v.d = d; v.focused = focused;
  v.cw = GFX_CELL_W * s; v.rh = GFX_CELL_H * s;
  v.x = x + 5; v.y = y + th + 3; v.w = w - 10; v.h = h - th - 6;
  v.cols = v.w / v.cw; v.rows = v.h / v.rh;
  gfx_clip(cv, v.x - 1, v.y - 1, v.w + 2, v.h + 2);
  TABS[tab].draw(d, &v);

  /* 3. the border. The focused tile gets a thick amber frame (2 pixels, more at larger text scales); the others a thin dark
     one, so the focused tile is plain at a glance. The content starts 5 pixels in, so a frame up to 4 thick never covers it. */
  gfx_clip(cv, x, y, w, h);
  if (focused) {
    int t = s < 2 ? 2 : s > 4 ? 4 : s;
    for (int k = 0; k < t; k++) gfx_frame(cv, x + k, y + k, w - 2 * k, h - 2 * k, gfx_colour(COL_AMB));
  } else gfx_frame(cv, x, y, w, h, gfx_colour(COL_SEL));
}

static void draw_status(Desk *d) {
  Canvas *cv = &d->cv;
  int s = d->scale, sh = status_height(d), y = cv->h - sh;
  gfx_unclip(cv);
  gfx_fill(cv, 0, y, cv->w, sh, gfx_colour(COL_PNL));
  gfx_fill(cv, 0, y, cv->w, 1, gfx_colour(COL_LINE));

  int meter_w = (d->handheld ? 9 : 14) * GFX_CELL_W * s;                     /* the load meter sits at the right edge, the CLIP flag just left of it */
  int clip_w = 6 * GFX_CELL_W * s;
  /* NAN: the sound went bad (a number that is not a number) and the core silenced it. That is a bug in the synth or a
     runaway patch, never normal, so it is flagged in red for 3 s after each new one. The core counts them; we watch the count. */
  if (d->synth->audio.nan_count != d->nan_seen) { d->nan_seen = d->synth->audio.nan_count; d->nan_until = d->now_ms + 3000; }
  int nan = d->nan_until > d->now_ms;
  int flags_w = (nan && d->clip ? 2 : 1) * (clip_w + 4);                    /* room for CLIP; for NAN too only while both show (the handheld is narrow) */
  const char *text = d->msg_until > d->now_ms ? d->msg : d->setup_open ? "Up/Down pick  Enter use device  Left/Right or type CC  Space mode  F2 close"
                   : d->hint_override ? d->hint_override : TABS[d->layouts[d->layout].tab[d->focus]].hint;

  /* The head: layout, patch name, pulsers. The tab's key hints matter more (they are the only reminder of a tab's letter
     keys), so when both do not fit, the head goes and the hint gets the whole bar. */
  char head[96];
  if (d->handheld) snprintf(head, sizeof head, " ");                           /* 256 pixels leave no room for the name: the button hints get the space (the Patches and Play screens show the rest) */
  else snprintf(head, sizeof head, " %s  %s  pulsers %s ", d->layouts[d->layout].name, d->loaded_name, d->pulsers_on ? "on" : "off");
  int markers = (d->muted ? 8 : 0) + (d->recording ? 11 : 0);                 /* cells the MUTED and REC markers take */
  if ((int)(strlen(head) + strlen(text) + markers) * GFX_CELL_W * s > cv->w - meter_w - flags_w - 4) snprintf(head, sizeof head, " ");
  int hx = (int)strlen(head) * GFX_CELL_W * s;
  gfx_text(cv, 0, y + 3, head, s, gfx_colour(COL_FG));
  if (d->muted) {                                        /* a solid red MUTED marker right after the name, so it cannot be missed */
    int mw = 7 * GFX_CELL_W * s;
    gfx_fill(cv, hx, y + 1, mw, sh - 2, gfx_colour(COL_RED));
    gfx_text(cv, hx + GFX_CELL_W * s / 2, y + 3, "MUTED", s, gfx_colour(COL_BG));
    hx += mw + GFX_CELL_W * s;
  }

  if (d->recording) {                                    /* a red REC marker with the elapsed time */
    char rec[32]; int secs = (int)d->rec_secs;
    snprintf(rec, sizeof rec, "REC %d:%02d", secs / 60, secs % 60);
    int rw = ((int)strlen(rec) + 1) * GFX_CELL_W * s;
    gfx_fill(cv, hx, y + 1, rw, sh - 2, gfx_colour(COL_RED));
    gfx_text(cv, hx + GFX_CELL_W * s / 2, y + 3, rec, s, gfx_colour(COL_BG));
    hx += rw + GFX_CELL_W * s;
  }

  gfx_clip(cv, hx, y, cv->w - hx - meter_w - flags_w - 4, sh);
  gfx_text(cv, hx, y + 3, text, s, gfx_colour(d->msg_until > d->now_ms ? COL_AMB : COL_DIM));
  gfx_unclip(cv);
  /* The load bar: how much of real time the audio callback uses. Full bar = 100 percent = the point where the audio crackles.
     The bar is the recent average; a thin marker shows the worst reading of the last few seconds plus the time the UI
     held the audio lock (main.c; drawing does not hold it), which is what predicts a dropout. Colour: green below 70 percent, amber to 90, red above. */
  int mx = cv->w - meter_w - 4;
  double avg = d->cpu > 1 ? 1 : d->cpu, pk = d->cpu_peak > 1 ? 1 : d->cpu_peak;
  gfx_fill(cv, mx, y + 5, meter_w, sh - 10, gfx_colour(COL_SEL));
  gfx_fill(cv, mx, y + 5, (int)(avg * meter_w), sh - 10, gfx_colour(d->cpu > 0.9 ? COL_RED : d->cpu > 0.7 ? COL_AMB : COL_GRN));
  const char *label = "CPU";                               /* inside the bar, at its right end (the peak marker is rarely there): dark over the fill, light over the empty track */
  for (int i = 0; i < 3; i++) {
    int lx = mx + meter_w - GFX_CELL_W * s * (4 - i), covered = (int)(avg * meter_w) > lx - mx + GFX_CELL_W * s / 2;
    char ch[2] = { label[i], 0 };
    gfx_text(cv, lx, y + 3, ch, s, gfx_colour(covered ? COL_BG : COL_FG));
  }
  int px = mx + (int)(pk * (meter_w - 2 * s));
  gfx_fill(cv, px, y + 3, 2 * s, sh - 6, gfx_colour(d->cpu_peak > 0.9 ? COL_RED : COL_FG));
  if (d->clip) {                                           /* CLIP: the output reached full scale in the last second */
    int cx = mx - clip_w - 4;
    gfx_fill(cv, cx, y + 1, clip_w, sh - 2, gfx_colour(COL_RED));
    gfx_text(cv, cx + GFX_CELL_W * s / 2 + s, y + 3, "CLIP", s, gfx_colour(COL_BG));
  }
  if (nan) {                                               /* NAN takes CLIP's place, or sits left of it when both show */
    int nx = mx - (d->clip ? 2 : 1) * (clip_w + 4);
    gfx_fill(cv, nx, y + 1, clip_w, sh - 2, gfx_colour(COL_RED));
    gfx_text(cv, nx + GFX_CELL_W * s, y + 3, "NAN", s, gfx_colour(COL_BG));
  }
}

/* The "quit?" dialog: a centred box over whatever is showing, at the text size of the rest of the screen. */
static void draw_quit_dialog(Desk *d) {
  Canvas *cv = &d->cv;
  int s = d->scale, cw = GFX_CELL_W * s, ch = GFX_CELL_H * s;
  const char *title = "QUIT FEZ?";
  const char *hint = "Enter or Ctrl+Q  quit        Esc  stay";
  int w = ((int)strlen(hint) + 4) * cw, h = ch * 4;
  if (w > cv->w) w = cv->w;                                   /* a very small window: the text is cut, not the box */
  int x = (cv->w - w) / 2, y = (cv->h - h) / 2;
  gfx_fill(cv, x, y, w, h, gfx_colour(COL_BG));
  gfx_frame(cv, x, y, w, h, gfx_colour(COL_AMB));
  gfx_text(cv, x + 2 * cw, y + ch, title, s, gfx_colour(COL_RED));
  gfx_text(cv, x + 2 * cw, y + ch * 5 / 2, hint, s, gfx_colour(COL_AMB));
}

void desk_draw(Desk *d) {
  gfx_unclip(&d->cv);
  gfx_fill(&d->cv, 0, 0, d->cv.w, d->cv.h, gfx_colour(COL_BG));
  const Layout *l = &d->layouts[d->layout];
  if (d->focus >= l->ntiles) d->focus = 0;
  for (int i = 0; i < l->ntiles; i++) draw_tile(d, i);
  if (d->help) desk_help_draw(d);                         /* F1: the help page covers the tiles */
  else if (d->setup_open) desk_setup_draw(d);             /* F2: so does the setup page */
  draw_status(d);
  if (d->quit_ask) draw_quit_dialog(d);                   /* Ctrl+Q: on top of everything */
}

/* ---------------------------------------------------------------- keys */

/* Move focus to the nearest tile in a direction. "Nearest" is measured between tile centres, counting sideways
   distance double so that Ctrl+Right prefers the tile straight across over one far above or below. */
static void move_focus(Desk *d, int dx, int dy) {
  int n = d->layouts[d->layout].ntiles, best = -1, best_cost = 0;
  int fx = d->rect[d->focus].x + d->rect[d->focus].w / 2, fy = d->rect[d->focus].y + d->rect[d->focus].h / 2;
  for (int i = 0; i < n; i++) {
    if (i == d->focus) continue;
    int cx = d->rect[i].x + d->rect[i].w / 2 - fx, cy = d->rect[i].y + d->rect[i].h / 2 - fy;
    int along = dx ? cx * dx : cy * dy, across = dx ? cy : cx;
    if (along <= 0) continue;                                  /* not in that direction */
    if (across < 0) across = -across;
    int cost = along + 2 * across;
    if (best < 0 || cost < best_cost) { best = i; best_cost = cost; }
  }
  if (best >= 0) d->focus = best;
}

/* ---------------------------------------------------------------- the keyboard device's keys */

/* The tracker layout, the one trackers and many DAWs use for "musical typing": two rows of piano. The bottom row's white
   keys are Z X C V B N M with the black keys S D G H J above; the next row up repeats it an octave higher (Q W E R T Y U
   with 2 3 5 6 7). A key's position in these strings is its distance in semitones above the bottom C. The characters are
   physical positions (the app converts scancodes), so it works on any keyboard layout. */
const char KB_KEYS_LOW[13]  = "zsxdcvgbhnjm";
const char KB_KEYS_HIGH[13] = "q2w3er5t6y7u";

static int kb_semitones(int ch) {
  for (int i = 0; i < 12; i++) { if (KB_KEYS_LOW[i] == ch) return i; if (KB_KEYS_HIGH[i] == ch) return 12 + i; }
  return -1;
}

int desk_note_key(Desk *d, int ch, int down) {
  if (ch < 0 || ch >= 128) return 0;
  if (!down) {                                                  /* a key up is always honoured, even if the page was left meanwhile, or the note would hang */
    int k = d->kb_key_note[ch];
    if (!k) return 0;
    if (k > 0) fez_note_off(d->synth, k - 1);                   /* a swell key (negative) only stops holding the swell open */
    d->kb_key_note[ch] = 0;
    return 1;
  }
  if (d->help || d->setup_open || d->quit_ask) return 0;        /* a page or the quit dialog is open: no notes */
  int semi = kb_semitones(ch);
  const Layout *l = &d->layouts[d->layout];
  if (semi < 0 || l->tab[d->focus] != TAB_KEYS) return 0;   /* only the KEYS tab plays */
  if (d->kb_key_note[ch]) return 1;                             /* key repeat: the key is already down */

  /* 1) the pitch (8.8) this key plays */
  int base = (fez_reg_read(d->synth, REG_KBOCT) + 1) * 12;     /* KBOCT 3: the bottom key is MIDI 48, C3 */
  int pitch;
  if (fez_reg_read(d->synth, REG_KBTYPE)) {
    /* Colour mode: the white keys are steps of the colour, in order up the keyboard (Z X C V B N M are degrees 0 to 6,
       Q W E R T Y U go on from 7), above KBROOT (any note; KBOCT is for chromatic mode only). The black keys are silent. A colour is just-intoned, so
       its notes fall between the keys: step 3 presses the nearest whole note, then sets PITCH exactly, as STICKS does. */
    static const int WHITE_INDEX[12] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };   /* semitone in the octave -> white key 0..6 */
    int w = WHITE_INDEX[semi % 12];
    if (w < 0) return 1;                                         /* a black key: taken (it is not typing), but silent */
    pitch = fez_reg_read(d->synth, REG_KBROOT) + fez_colour_degree(fez_reg_read(d->synth, REG_KBCOL), (semi / 12) * 7 + w);
  } else pitch = (base + semi) * 256;
  pitch = fez_clamp(pitch, 0, 127 * 256);
  int note = (pitch + 128) / 256;

  /* 2) swell mode: the key only moves PITCH, with no attack; kb_swell_tick opens GATELEVEL while any such key is held.
        The key is remembered as a negative number, so its key up knows there is no note to let go of. */
  if (fez_reg_read(d->synth, REG_KBMODE)) {
    fez_reg_write(d->synth, REG_PITCH, pitch);
    d->kb_key_note[ch] = -(note + 1);
    return 1;
  }

  /* 3) play mode: a note through the keyboard device (the attack, KBNOTE, KBPUL), then the exact pitch */
  fez_note_on(d->synth, note);
  fez_reg_write(d->synth, REG_PITCH, pitch);
  d->kb_key_note[ch] = note + 1;                                /* remembered, so the key up lets go of this note even if KBOCT changes meanwhile */
  return 1;
}

/* KEYS in swell mode: GATELEVEL opens over KBSWELL ms while any swell key is held, and closes over the same time once none
   is. It runs on every tick, whichever tab is in front: key ups always arrive (or losing focus lets go of them all), so a
   swell left open closes by itself. */
static void kb_swell_tick(Desk *d, double dt_ms) {
  int held = 0;
  for (int i = 0; i < 128 && !held; i++) held = d->kb_key_note[i] < 0;
  if (!held && d->kb_swell <= 0) return;                        /* closed and nothing held: leave GATELEVEL alone */
  double step = dt_ms / fez_reg_read(d->synth, REG_KBSWELL);
  d->kb_swell += held ? step : -step;
  d->kb_swell = d->kb_swell < 0 ? 0 : d->kb_swell > 1 ? 1 : d->kb_swell;
  fez_reg_write(d->synth, REG_GATELEVEL, (int)lround(d->kb_swell * 32767));
}

void desk_all_notes_off(Desk *d) {
  fez_all_notes_off(d->synth);
  memset(d->kb_key_note, 0, sizeof d->kb_key_note);
}

void desk_key(Desk *d, int key, int mods) {
  Layout *l = &d->layouts[d->layout];
  int *tab = &l->tab[d->focus];
  /* PANIC: Shift+Esc toggles the global mute from any tab, even in the middle of typing a number. Plain Esc is left alone,
     because most tabs use it to cancel an entry. */
  if (key == KEY_ESC && (mods & MOD_SHIFT)) { d->muted = !d->muted; return; }
  if (d->quit_ask) {                                                            /* the quit dialog takes every key: Enter or Ctrl+Q again says yes, the rest say no */
    if (key == KEY_ENTER || (key == 'q' && (mods & (MOD_CTRL | MOD_ALT)))) d->quit = 1;
    d->quit_ask = 0;
    return;
  }
  if (key == KEY_F2 && !d->help && !d->handheld && !d->setup_open) { desk_setup_open(d); return; }   /* F2 opens the setup page ... */
  if (d->setup_open) { desk_setup_key(d, key, mods); return; }                  /* ... which takes every key until F2 or Esc closes it */
  if (key == KEY_F1 && !d->help) { d->help = 1; d->help_top = 0; return; }       /* F1 opens the help page ... */
  if (d->help) { desk_help_key(d, key); return; }                               /* ... which takes every key until F1 or Esc closes it */
  if (mods & (MOD_CTRL | MOD_ALT)) {
    switch (key) {
      case KEY_LEFT:  move_focus(d, -1, 0); return;
      case KEY_RIGHT: move_focus(d, 1, 0); return;
      case KEY_UP:    move_focus(d, 0, -1); return;
      case KEY_DOWN:  move_focus(d, 0, 1); return;
      case '[': do *tab = (*tab + TAB_COUNT - 1) % TAB_COUNT; while (!desk_tab_shown(d, *tab)); return;
      case ']': do *tab = (*tab + 1) % TAB_COUNT; while (!desk_tab_shown(d, *tab)); return;
      case KEY_TAB:
        d->layout = (d->layout + 1) % DESK_LAYOUTS;
        d->focus = d->focus < d->layouts[d->layout].ntiles ? d->focus : 0;
        compute_rects(d);
        desk_say(d, d->layouts[d->layout].name);
        return;
      case '=': case '+': desk_set_scale(d, d->scale + 1); return;
      case '-': desk_set_scale(d, d->scale > 1 ? d->scale - 1 : 1); return;     /* never 0: desk_set_scale reads 0 as "automatic" */
      case 't': d->rec_toggle = 1; return;                      /* record a take of the output: main.c does the work (Ctrl+R is the editor's run) */
      case 'g': desk_fire_gate(d); return;
      case 'p': desk_toggle_pulsers(d); return;
      case 'n': *tab = TAB_PATCHES; desk_patches_begin_new(d); return;       /* new blank patch: asks first, in the Patches tab */
      case 's': *tab = TAB_PATCHES; desk_patches_begin_save(d); return;     /* save: shows the name prompt in the Patches tab */
      case 'q': d->quit_ask = 1; return;                         /* ask first: a stray Ctrl+Q must not end a performance (on the USB image it powers off) */
      default: break;                                            /* other chords go to the tab (the editor has several) */
    }
  }
  TABS[*tab].key(d, key, mods);
}

void desk_text(Desk *d, int ch) {
  if (d->help || d->quit_ask) return;                           /* a page or the quit dialog is open: typing does nothing */
  if (d->setup_open) { desk_setup_text(d, ch); return; }
  TABS[d->layouts[d->layout].tab[d->focus]].text(d, ch);
}

/* One column of a waveform picture: the smallest and largest of the samples x[i0] .. x[i1 - 1] (0 if there are none).
   The pictures must be quick, for the frame rate on a small ARM board: a column of a 30 s buffer covers thousands of
   samples, and looking at all of them every frame is millions of reads. So at most 64 evenly spaced
   samples are looked at. ponytail: a short peak between them can be missed; keep a per-column peak if that shows. */
void desk_wave_column(const float *x, int i0, int i1, float *lo, float *hi) {
  int step = (i1 - i0) / 64 + 1;
  *lo = 0; *hi = 0;
  for (int i = i0; i < i1; i += step) { if (x[i] < *lo) *lo = x[i]; if (x[i] > *hi) *hi = x[i]; }
}

/* ---------------------------------------------------------------- typed entries (see Entry in desk.h) */

void entry_clear(Entry *e) { e->len = 0; e->buf[0] = '\0'; e->editing = 0; }

/* max is the longest entry the cell takes, e.g. 5 for 0..32767 or 6 for -32768. It is capped by the buffer's size. */
void entry_add(Entry *e, int ch, int max) {
  if (max > (int)sizeof e->buf - 1) max = (int)sizeof e->buf - 1;
  if (e->len >= max) return;
  e->buf[e->len++] = (char)ch; e->buf[e->len] = '\0';
  e->editing = 1;
}

void entry_backspace(Entry *e) {
  if (e->len > 0) e->buf[--e->len] = '\0';
  if (e->len == 0) e->editing = 0;
}

/* The names of a tab's pages along its top row, the one showing lit in amber. */
void view_pages(View *v, const char *const names[], int n, int showing) {
  int col = 0;
  for (int i = 0; i < n; i++) {
    int len = (int)strlen(names[i]);
    if (i == showing) view_fill(v, col, 0, len + 1, 1, COL_AMBD);
    view_text(v, col, 0, names[i], i == showing ? COL_AMB : COL_DIM);
    col += len + 2;
  }
}
