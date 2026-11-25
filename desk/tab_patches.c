/* The PATCHES tab: load and save patches.

   Two lists, switched with [ and ] (or Left/Right):
     MY PATCHES   your own, one text file each in the patch folder (see userpatch.c). The default list.
     EXAMPLES     the patches built into the program (read-only), with a description and what to listen for
   The program starts from a blank patch (no code, pulsers off), not from an example.

   Keys:
     Up/Down        move (Shift: first or last)         Left/Right   switch list
     Enter          load the selected patch             letters      type to narrow the list; Esc clears
     Ctrl+S         save under a name (from any tab)    Ctrl+D or Delete   delete the selected user patch (asks first)

   The load and save code lives here because a patch is picked, named and confirmed here. The text format is in the core
   (patchfile.c); disk access is in userpatch.c. */
#include "desk.h"
#include "userpatch.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>


static char file_buf[PATCH_TEXT_BUF];

/* ---------------------------------------------------------------- what is in each list */

static const char *list_name(const Desk *d, int sub, int i) { return sub == SUB_EXAMPLES ? PATCH_TABLE[i].name : d->pat.names[i]; }
static int list_size(const Desk *d, int sub) { return sub == SUB_EXAMPLES ? PATCH_COUNT : d->pat.nuser; }

/* The rows that match the filter, as indexes into the full list. Returns how many. */
static int visible_rows(const Desk *d, int *rows, int max) {
  int n = 0, sub = d->pat.sub;
  for (int i = 0; i < list_size(d, sub) && n < max; i++)
    if (desk_contains_nocase(list_name(d, sub, i), d->pat.filter)) rows[n++] = i;
  return n;
}

void desk_patches_refresh(Desk *d) {
  d->pat.nuser = userpatch_list(d->pat.dir, d->pat.names, MAX_USER_PATCHES);
  d->pat.shown[0] = '\0';                                      /* forget the cached header: the file may have changed */
}

/* ---------------------------------------------------------------- loading and saving */

/* Start from nothing: the state of a new, empty patch. */
void desk_patches_loaded_blank(Desk *d) {
  fez_load_blank(d->synth);
  d->pulsers_on = 1;
  snprintf(d->loaded_name, sizeof d->loaded_name, "(blank)"); d->loaded_user = 0;
  d->pat.info.name[0] = d->pat.info.desc[0] = d->pat.info.listen[0] = '\0';
}

/* Take over the example patch's own text as the "current" description, for the next save. */
void desk_patches_loaded_example(Desk *d, int index) {
  const PatchDef *p = &PATCH_TABLE[index];
  fez_load_patch(d->synth, index);
  d->pulsers_on = 1;
  snprintf(d->loaded_name, sizeof d->loaded_name, "%s", p->name); d->loaded_user = 0;
  snprintf(d->pat.info.name, sizeof d->pat.info.name, "%s", p->name);
  snprintf(d->pat.info.desc, sizeof d->pat.info.desc, "%s", p->desc);
  snprintf(d->pat.info.listen, sizeof d->pat.info.listen, "%s", p->listen);
}

static void load_user(Desk *d, const char *name) {
  if (userpatch_read(d->pat.dir, name, file_buf, sizeof file_buf) < 0) { desk_say(d, "CANNOT READ THAT FILE"); return; }
  char err[96];
  int bad = patchfile_read(d->synth, file_buf, &d->pat.info, err, sizeof err);
  if (bad < 0) { desk_say(d, "NOT A FEZ PATCH FILE"); return; }
  d->pulsers_on = 1;
  snprintf(d->loaded_name, sizeof d->loaded_name, "%s", name); d->loaded_user = 1;
  desk_reload_editor(d);
  char m[48];
  if (bad) snprintf(m, sizeof m, "LOADED, %d LINES SKIPPED", bad); else snprintf(m, sizeof m, "LOADED %.36s", name);
  desk_say(d, m);
}

static void do_save(Desk *d) {
  FezSynth *s = d->synth;
  PatchInfo info = d->pat.info;
  snprintf(info.name, sizeof info.name, "%s", d->pat.name);
  /* With "all pulsers off" in force the synth shows every pulser off: save what the pulsers would be. */
  int was[NUM_PULSERS];
  for (int i = 0; i < NUM_PULSERS; i++) { was[i] = s->pulsers[i].on; if (!d->pulsers_on) s->pulsers[i].on = d->pul_was[i]; }
  int n = patchfile_write(s, &info, file_buf, sizeof file_buf);
  for (int i = 0; i < NUM_PULSERS; i++) s->pulsers[i].on = was[i];
  if (n < 0 || userpatch_write(d->pat.dir, d->pat.name, file_buf) != 0) { desk_say(d, "SAVE FAILED"); return; }

  d->pat.info = info;
  snprintf(d->loaded_name, sizeof d->loaded_name, "%s", d->pat.name); d->loaded_user = 1;
  desk_patches_refresh(d);
  d->pat.sub = SUB_MINE; d->pat.flen = 0; d->pat.filter[0] = '\0';
  for (int i = 0; i < d->pat.nuser; i++) if (!strcmp(d->pat.names[i], d->pat.name)) d->pat.sel[SUB_MINE] = i;   /* select the saved patch */
  char m[48]; snprintf(m, sizeof m, "SAVED %.38s", d->pat.name); desk_say(d, m);
}

void desk_patches_begin_save(Desk *d) {
  if (!d->pat.dir[0]) { desk_say(d, "NO SAVE FOLDER"); return; }
  d->pat.mode = PAT_NAMING;
  snprintf(d->pat.name, sizeof d->pat.name, "%s", d->loaded_name);     /* pre-filled: Enter alone re-saves; or retype */
  d->pat.nlen = (int)strlen(d->pat.name);
}

/* ---------------------------------------------------------------- drawing */

void tab_patches_draw(Desk *d, View *v) {
  int rows[MAX_USER_PATCHES > 64 ? MAX_USER_PATCHES : 64];
  int sub = d->pat.sub, n = visible_rows(d, rows, MAX_USER_PATCHES);
  int wide = v->cols >= 60;                                   /* wide: list on the left, text on the right */
  int list_col_w = 24, text_rows = wide ? 0 : 8;
  int top_rows = 2, bottom_rows = 1;                          /* header + filter line above the list; prompt line below */
  int vis = v->rows - top_rows - bottom_rows - text_rows;
  if (vis < 3) vis = 3;
  if (vis > n) vis = n;
  int *sel = &d->pat.sel[sub];
  if (*sel >= n) *sel = n ? n - 1 : 0;
  int top = *sel - vis / 2; top = top < 0 ? 0 : top > n - vis ? n - vis : top;

  /* header: the two lists, the active one lit */
  char hdr[48];   /* "filter: " + up to 31 typed letters + "_" must fit */
  view_fill(v, sub == SUB_MINE ? 0 : 15, 0, sub == SUB_MINE ? 14 : 9, 1, COL_AMBD);
  snprintf(hdr, sizeof hdr, "MY PATCHES %d", d->pat.nuser);
  view_text(v, 0, 0, hdr, sub == SUB_MINE ? COL_AMB : COL_DIM);
  view_text(v, 15, 0, "EXAMPLES", sub == SUB_EXAMPLES ? COL_AMB : COL_DIM);
  if (d->pat.flen) { snprintf(hdr, sizeof hdr, "filter: %s_", d->pat.filter); view_text(v, 0, 1, hdr, COL_CYN); }
  else view_text(v, 0, 1, "type to filter", COL_DIM);

  for (int r = 0; r < vis; r++) {
    int i = rows[top + r], is_sel = top + r == *sel;
    char line[PATCH_NAME_MAX + 4];
    int loaded = d->loaded_user == (sub == SUB_MINE) && !strcmp(list_name(d, sub, i), d->loaded_name);
    snprintf(line, sizeof line, "%s%s", list_name(d, sub, i), loaded ? " *" : "");
    if (is_sel) view_fill(v, 0, top_rows + r, wide ? list_col_w : v->cols - 1, 1, COL_AMBD);
    view_text(v, 0, top_rows + r, line, is_sel ? COL_AMB : COL_FG);
  }
  if (n > vis) view_scrollbar(v, top, vis, n, top_rows, vis);
  if (n == 0) view_text(v, 0, top_rows, sub == SUB_MINE && !d->pat.nuser ? "none yet: Ctrl+S saves one" : "no match", COL_DIM);

  /* the selected patch's text */
  const char *name = ""; const char *desc = ""; const char *listen = "";
  if (n) {
    int i = rows[*sel];
    if (sub == SUB_EXAMPLES) { name = PATCH_TABLE[i].name; desc = PATCH_TABLE[i].desc; listen = PATCH_TABLE[i].listen; }
    else {
      if (strcmp(d->pat.shown, d->pat.names[i]) != 0) {        /* read the file's header once per selection, not every frame */
        PatchInfo *si = &d->pat.shown_info;
        si->name[0] = si->desc[0] = si->listen[0] = '\0';
        if (userpatch_read(d->pat.dir, d->pat.names[i], file_buf, sizeof file_buf) >= 0) patchfile_info(file_buf, si);
        snprintf(d->pat.shown, sizeof d->pat.shown, "%s", d->pat.names[i]);
      }
      name = d->pat.names[i]; desc = d->pat.shown_info.desc; listen = d->pat.shown_info.listen;
    }
  }
  int col = wide ? list_col_w + 2 : 0, row = wide ? 0 : top_rows + vis + 1, width = v->cols - col - 2;
  char lines[24][64], buf[300];
  if (width >= 12 && n) {
    view_text(v, col, row, name, COL_AMB);
    int dl = wrap_text(desc, width, lines, 14);
    for (int i = 0; i < dl && row + 1 + i < v->rows - 1; i++) view_text(v, col, row + 1 + i, lines[i], COL_FG);
    if (listen[0]) {
      snprintf(buf, sizeof buf, "Listen: %s", listen);
      int ll = wrap_text(buf, width, lines, 8);
      for (int i = 0; i < ll && row + 2 + dl + i < v->rows - 1; i++) view_text(v, col, row + 2 + dl + i, lines[i], COL_GRN);
    }
  }

  /* the prompt line at the bottom */
  int pr = v->rows - 1;
  if (d->pat.mode == PAT_NAMING) { snprintf(buf, sizeof buf, "SAVE AS: %s_   Enter saves, Esc cancels", d->pat.name); view_text(v, 0, pr, buf, COL_AMB); }
  else if (d->pat.mode == PAT_CONFIRM_OVERWRITE) { snprintf(buf, sizeof buf, "\"%s\" EXISTS. Enter overwrites, Esc cancels", d->pat.name); view_text(v, 0, pr, buf, COL_RED); }
  else if (d->pat.mode == PAT_CONFIRM_DELETE) { snprintf(buf, sizeof buf, "DELETE \"%s\"? Enter deletes, Esc cancels", d->pat.names[rows[*sel]]); view_text(v, 0, pr, buf, COL_RED); }
}

/* ---------------------------------------------------------------- keys */

static void set_sub(Desk *d, int sub) {
  d->pat.sub = sub; d->pat.flen = 0; d->pat.filter[0] = '\0';
  if (sub == SUB_MINE) desk_patches_refresh(d);
}

void tab_patches_key(Desk *d, int key, int mods) {
  int rows[MAX_USER_PATCHES > 64 ? MAX_USER_PATCHES : 64];
  int sub = d->pat.sub, n = visible_rows(d, rows, MAX_USER_PATCHES);
  int *sel = &d->pat.sel[sub];

  /* a question is open: only Enter, Esc and (for a name) Backspace mean anything */
  if (d->pat.mode != PAT_NORMAL) {
    if (key == KEY_ESC) { d->pat.mode = PAT_NORMAL; return; }
    if (key == KEY_BACKSPACE && d->pat.mode == PAT_NAMING && d->pat.nlen > 0) d->pat.name[--d->pat.nlen] = '\0';
    if (key != KEY_ENTER) return;
    if (d->pat.mode == PAT_NAMING) {
      if (!d->pat.nlen) return;
      if (userpatch_exists(d->pat.dir, d->pat.name) && !(d->loaded_user && !strcmp(d->pat.name, d->loaded_name))) { d->pat.mode = PAT_CONFIRM_OVERWRITE; return; }
      d->pat.mode = PAT_NORMAL; do_save(d);
    } else if (d->pat.mode == PAT_CONFIRM_OVERWRITE) { d->pat.mode = PAT_NORMAL; do_save(d); }
    else {                                                     /* delete */
      d->pat.mode = PAT_NORMAL;
      if (n && userpatch_delete(d->pat.dir, d->pat.names[rows[*sel]]) == 0) { desk_say(d, "DELETED"); desk_patches_refresh(d); }
      else desk_say(d, "DELETE FAILED");
    }
    return;
  }

  switch (key) {
    case KEY_UP:    if (n) *sel = (mods & MOD_SHIFT) ? 0 : (*sel + n - 1) % n; break;
    case KEY_DOWN:  if (n) *sel = (mods & MOD_SHIFT) ? n - 1 : (*sel + 1) % n; break;
    case KEY_LEFT:  case KEY_RIGHT: set_sub(d, !sub); break;
    case KEY_ESC:   d->pat.flen = 0; d->pat.filter[0] = '\0'; break;
    case KEY_BACKSPACE: if (d->pat.flen > 0) d->pat.filter[--d->pat.flen] = '\0'; break;
    case KEY_ENTER:
      if (!n) break;
      if (sub == SUB_EXAMPLES) {
        desk_patches_loaded_example(d, rows[*sel]);
        desk_reload_editor(d);
        char m[48]; snprintf(m, sizeof m, "LOADED %.36s", PATCH_TABLE[rows[*sel]].name); desk_say(d, m);
      } else load_user(d, d->pat.names[rows[*sel]]);
      break;
    case KEY_DELETE: case 'd':                                 /* 'd' arrives with Ctrl held: plain letters come through the text path */
      if (key == KEY_DELETE || (mods & MOD_CTRL)) { if (sub == SUB_MINE && n) d->pat.mode = PAT_CONFIRM_DELETE; }
      break;
    default: break;
  }
}

/* Letters narrow the list; or, while a name is being typed, build the name. */
void tab_patches_text(Desk *d, int ch) {
  if ((ch == '[' || ch == ']') && d->pat.mode == PAT_NORMAL) { set_sub(d, !d->pat.sub); return; }   /* two lists: either bracket switches */
  if (d->pat.mode == PAT_NAMING) {
    if (userpatch_name_char_ok(ch) && d->pat.nlen < PATCH_NAME_MAX - 8) { d->pat.name[d->pat.nlen++] = (char)ch; d->pat.name[d->pat.nlen] = '\0'; }
    return;
  }
  if (d->pat.mode != PAT_NORMAL || ch < 32 || ch > 126) return;
  if (d->pat.flen < (int)sizeof d->pat.filter - 1) { d->pat.filter[d->pat.flen++] = (char)ch; d->pat.filter[d->pat.flen] = '\0'; }
  d->pat.sel[d->pat.sub] = 0;
}
