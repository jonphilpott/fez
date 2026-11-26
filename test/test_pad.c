/* Handheld controls: buttons in, the right things happen in the desktop UI behind them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pad.h"
#include "test.h"

static FezSynth s;
static Desk d;
static Pad pad;
static uint32_t px[256 * 192];

static void tap(Button b) { pad_button(&pad, b, 1); pad_button(&pad, b, 0); d.now_ms += 1000; }
static int tab(void) { return d.layouts[d.layout].tab[d.focus]; }

static char test_clip[256];
static void test_clip_set(const char *t) { snprintf(test_clip, sizeof test_clip, "%s", t); }
static int test_clip_get(char *buf, int max) { snprintf(buf, (size_t)max, "%s", test_clip); return (int)strlen(buf); }
/* open the editor's Screen actions and choose the row-th action */
static void editor_action(int row) {
  if (pad.menu == -1) tap(BTN_MENU);                                           /* the menu opens on the actions tab */
  for (int i = 0; i < row; i++) tap(BTN_DOWN);
  tap(BTN_A);
}

void test_pad(void) {
  fez_init(&s, 1);
  desk_init(&d, &s);
  snprintf(d.pat.dir, sizeof d.pat.dir, "%s/", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
  desk_set_canvas(&d, px, 256, 192);
  desk_set_handheld(&d);
  pad_init(&pad, &d);
  d.layouts[3].tab[0] = TAB_EDITOR;

  /* the editor's Ctrl+/ with a selection comments the line and keeps the text (it once deleted the selection and left the
     cursor past the end, so the next key moved memory with a negative length) */
  { Editor *e = &d.ed;
    snprintf(e->text[e->blk], BLOCK_SRC_MAX, "100 PITCH !");
    e->cursor = 11; e->anchor = 6;                                             /* "ITCH !" selected */
    tab_editor_key(&d, '/', MOD_CTRL);
    CHECK(!strcmp(e->text[e->blk], "\\ 100 PITCH !") && e->cursor == 13 && e->anchor == -1, "Ctrl+/ with a selection comments the line and keeps the text");
    tab_editor_key(&d, '/', MOD_CTRL);
    CHECK(!strcmp(e->text[e->blk], "100 PITCH !") && e->cursor == 11, "Ctrl+/ again takes the comment off and the cursor stays put");
    e->text[e->blk][0] = '\0'; e->cursor = 0; }

  /* L1 and R1 walk through every screen and wrap round */
  int seen[TAB_COUNT] = { 0 };
  for (int i = 0; i < TAB_COUNT; i++) { seen[tab()] = 1; tap(BTN_R1); }
  int all = 1; for (int i = 0; i < TAB_COUNT; i++) all &= seen[i];
  CHECK(all && tab() == TAB_EDITOR, "R1 visits every screen and comes back");
  tap(BTN_L1); CHECK(tab() == TAB_SCOPE, "L1 from the first screen wraps to the last");

  /* Select mutes, and again unmutes */
  tap(BTN_SELECT); CHECK(d.muted, "Select mutes"); tap(BTN_SELECT); CHECK(!d.muted, "Select unmutes");

  /* the volume buttons move the master level */
  int v0 = s.level_pot; tap(BTN_VOLD); CHECK(s.level_pot < v0, "Vol- lowers the level"); tap(BTN_VOLU); CHECK(s.level_pot == v0, "Vol+ raises it again");

  /* PLAY: A B X Y fire the four pulsers */
  d.layouts[3].tab[0] = TAB_PLAY;
  s.vm.pending = 0; tap(BTN_A); CHECK(s.vm.pending == 1, "A fires P0");
  s.vm.pending = 0; tap(BTN_B); CHECK(s.vm.pending == 2, "B fires P1");
  s.vm.pending = 0; tap(BTN_X); CHECK(s.vm.pending == 4, "X fires P2");
  s.vm.pending = 0; tap(BTN_Y); CHECK(s.vm.pending == 8, "Y fires P3");

  /* the menu opens and closes, and swallows buttons while open */
  tap(BTN_MENU); CHECK(pad.menu != -1, "Menu opens the menu");
  s.vm.pending = 0; tap(BTN_DOWN); tap(BTN_B); CHECK(s.vm.pending == 0 && pad.menu == -1, "B closes the menu and does not fire a pulser");

  /* the editor palette: A opens it, A inserts a word into the block's text, B closes it */
  d.layouts[3].tab[0] = TAB_EDITOR; desk_reload_editor(&d);
  vm_set_block(&s.vm, d.ed.blk, ""); desk_reload_editor(&d);
  tap(BTN_A); CHECK(pad.pal, "A in the editor opens the palette");
  tap(BTN_A); CHECK(strncmp(d.ed.text[d.ed.blk], "DUP ", 4) == 0, "A in the palette types the highlighted word and a space");
  tap(BTN_Y); CHECK(d.ed.text[d.ed.blk][0] == '\0', "Y in the palette deletes the last word");
  tap(BTN_B); CHECK(!pad.pal, "B closes the palette");

  /* inserting keeps words apart wherever the cursor is: a 1 put in front of CASTEP must not give 1CASTEP */
  vm_set_block(&s.vm, d.ed.blk, "CASTEP"); desk_reload_editor(&d);
  tap(BTN_A); for (int i = 0; i < 6; i++) tap(BTN_R2); tap(BTN_RIGHT);          /* palette, NUMS tab, the item "1" */
  tap(BTN_A); CHECK(!strcmp(d.ed.text[d.ed.blk], "1 CASTEP"), "a digit inserted before a word gets a space after it");
  tap(BTN_RIGHT); tap(BTN_A); CHECK(!strcmp(d.ed.text[d.ed.blk], "12 CASTEP"), "the next digit joins the number");
  tap(BTN_B); tap(BTN_LEFT); tap(BTN_LEFT); tap(BTN_LEFT);                      /* close the palette, cursor to the start */
  desk_key(&d, KEY_LEFT, 0); desk_key(&d, KEY_LEFT, 0); desk_key(&d, KEY_LEFT, 0); desk_key(&d, KEY_LEFT, 0);
  vm_set_block(&s.vm, d.ed.blk, "A B"); desk_reload_editor(&d); desk_key(&d, KEY_RIGHT, 0);   /* between A and the space */
  tap(BTN_A); pad.ptab = 0; pad.psel = 0; tap(BTN_A);                            /* DUP */
  CHECK(!strcmp(d.ed.text[d.ed.blk], "A DUP B"), "a word inserted mid-line gets one space either side, not two");
  tap(BTN_B);
  vm_set_block(&s.vm, d.ed.blk, ""); desk_reload_editor(&d);
  tap(BTN_A); tap(BTN_A); tap(BTN_A);                                           /* DUP DUP at the end of an empty block */
  CHECK(!strcmp(d.ed.text[d.ed.blk], "DUP DUP "), "words appended at the end are separated, with a space after the last");
  tap(BTN_B);

  /* the palette's register tab is grouped like CTRL: headings take rows of their own, and Up/Down skip them */
  tap(BTN_A); pad.ptab = 7; pad.psel = 0;
  CHECK(pad.nheads > 5 && pad.ncells > 100 && pad.cells[0].row == 1, "the REGS palette has headings, and the first register sits below the first heading");
  int r0 = pad.cells[pad.psel].row; tap(BTN_DOWN);
  CHECK(pad.cells[pad.psel].row > r0, "Down moves to a later row of registers");
  for (int i = 0; i < 60; i++) tap(BTN_DOWN);
  for (int i = 0; i < 60; i++) tap(BTN_UP);
  CHECK(pad.cells[pad.psel].row <= r0 + 1, "Up retraces the same rows (headings are skipped both ways)");
  tap(BTN_B);

  /* the editor's "Runs on P#" checkboxes change the block's pulser mask and leave the menu open */
  d.layouts[3].tab[0] = TAB_EDITOR; s.mem[REG_BP0 + d.ed.blk] = 0;
  tap(BTN_MENU); CHECK(pad.menu == 2, "the menu opens on this screen's actions when it has some");
  tap(BTN_RIGHT); CHECK(pad.menu == 0, "Right switches to the MENU tab");
  tap(BTN_L1); CHECK(pad.menu == 2, "L1 switches back to the actions");
  tap(BTN_R1); tap(BTN_LEFT); CHECK(pad.menu == 2, "R1 and Left switch tabs too");
  tap(BTN_RIGHT); tap(BTN_A); CHECK(pad.menu == 1, "Go to screen opens its list");
  tap(BTN_B); CHECK(pad.menu == 0, "B in the list goes back to the MENU tab");
  tap(BTN_LEFT);
  for (int i = 0; i < 7; i++) tap(BTN_DOWN);                                    /* past Run, Undo, Redo, Copy, Cut, Paste, Clear: now on "Runs on P0" */
  tap(BTN_A); CHECK(pad.menu == 2 && (s.mem[REG_BP0 + d.ed.blk] & 1), "A on Runs on P0 sets the bit and keeps the menu open");
  tap(BTN_DOWN); tap(BTN_DOWN); tap(BTN_A); CHECK(pad.menu == 2 && s.mem[REG_BP0 + d.ed.blk] == 5, "P2 can be added without closing the menu");
  tap(BTN_UP); tap(BTN_UP); tap(BTN_A); CHECK(s.mem[REG_BP0 + d.ed.blk] == 4, "A again clears the bit");
  tap(BTN_B); tap(BTN_B); CHECK(pad.menu == -1, "B backs out");

  /* Start fires the gate */
  s.env.phase = ENV_IDLE; tap(BTN_START); CHECK(s.env.phase == ENV_ATT, "Start fires the gate (the envelope starts its attack)");

  /* naming a patch with the letter picker, then saving */
  d.layouts[3].tab[0] = TAB_PATCHES;
  tap(BTN_X); CHECK(d.pat.mode == PAT_NAMING, "X on the Patches screen starts a save");
  d.pat.nlen = 0; d.pat.name[0] = '\0';
  tap(BTN_A); tap(BTN_DOWN); tap(BTN_A);                          /* "a" then "b" */
  CHECK(strcmp(d.pat.name, "ab") == 0, "the letter picker adds a, then b");
  tap(BTN_B); CHECK(strcmp(d.pat.name, "a") == 0, "B removes the last letter");
  tap(BTN_Y); CHECK(d.pat.mode == PAT_NORMAL, "Y cancels the name");

  /* every screen draws without trouble, with every overlay */
  for (int t = 0; t < TAB_COUNT; t++) { d.layouts[3].tab[0] = t; tap(BTN_MENU); desk_draw(&d); pad_draw(&pad); tap(BTN_B); desk_draw(&d); pad_draw(&pad); }

  /* the Devices screen has five pages on the handheld: R2 walks round them and never reaches the KEYS page */
  d.layouts[3].tab[0] = TAB_DEVICES; d.dev.sub = DEV_DELAY;
  int pages_seen = 0;
  for (int i = 0; i < 12; i++) { tap(BTN_R2); CHECK(d.dev.sub != DEV_KEYS, "R2 never reaches the KEYS page on the handheld"); pages_seen |= 1 << d.dev.sub; }
  CHECK(pages_seen == (1 << DEV_COUNT) - 1 - (1 << DEV_KEYS), "R2 visits every other page");
  d.dev.sub = DEV_DELAY; tap(BTN_L2); CHECK(d.dev.sub == DEV_TURING, "L2 from the first page wraps to the last one the handheld has");

  /* the sticks are modulation pots MP0..MP3 */
  CHECK(fez_reg_read(&s, REG_MP0) == 16384 && fez_reg_read(&s, REG_MP3) == 16384, "the pots start at the sticks' centre");
  pad_stick(&pad, 0, 0);       CHECK(fez_reg_read(&s, REG_MP0) == 16384, "a stick at rest is at the centre");
  pad_stick(&pad, 0, 3000);    CHECK(fez_reg_read(&s, REG_MP0) == 16384, "a small push is inside the dead zone");
  pad_stick(&pad, 0, 32767);   CHECK(fez_reg_read(&s, REG_MP0) == 32767, "full right is the top of the pot");
  pad_stick(&pad, 0, -32768);  CHECK(fez_reg_read(&s, REG_MP0) == 1, "full left is the bottom of the pot (the dead zone stretch leaves 1)");
  pad_stick(&pad, 1, -32768);  CHECK(fez_reg_read(&s, REG_MP1) == 32767, "pushing up (SDL's negative Y) raises the pot");
  pad_stick(&pad, 3, 20000);   CHECK(fez_reg_read(&s, REG_MP3) < 16384, "pushing the right stick down lowers MP3");
  pad_stick(&pad, 2, 6000);    CHECK(fez_reg_read(&s, REG_MP2) > 16384 && fez_reg_read(&s, REG_MP2) < 18000, "just outside the dead zone the pot moves only a little");
  pad_stick(&pad, 9, 32767);   CHECK(fez_reg_read(&s, REG_MP2) < 18000, "a bad axis is ignored");
  fez_load_patch(&s, 2);       CHECK(fez_reg_read(&s, REG_MP0) == 1 && fez_reg_read(&s, REG_MP1) == 32767, "loading a patch leaves the pots where they were");

  /* the editor on the handheld: select with Y + D-pad, then copy, cut and paste from the Screen actions menu */
  {
    d.clip_set = test_clip_set; d.clip_get = test_clip_get;
    d.layouts[3].tab[0] = TAB_EDITOR; d.ed.blk = 0;
    if (pad.menu != -1) tap(BTN_B);
    snprintf(d.ed.text[0], sizeof d.ed.text[0], "alpha beta");
    d.ed.cursor = 10; d.ed.anchor = -1;
    for (int k = 0; k < 4; k++) { pad_button(&pad, BTN_Y, 1); pad_button(&pad, BTN_LEFT, 1); pad_button(&pad, BTN_LEFT, 0); pad_button(&pad, BTN_Y, 0); }
    CHECK(d.ed.anchor == 10 && d.ed.cursor == 6, "holding Y with the D-pad selects text");
    editor_action(3);                                                           /* Copy */
    CHECK(strcmp(test_clip, "beta") == 0 && strcmp(d.ed.text[0], "alpha beta") == 0, "Copy puts the selection on the clipboard and leaves the text");
    d.ed.cursor = 0; d.ed.anchor = -1;
    editor_action(5);                                                           /* Paste */
    CHECK(strcmp(d.ed.text[0], "betaalpha beta") == 0, "Paste inserts the clipboard at the cursor");
    snprintf(d.ed.text[0], sizeof d.ed.text[0], "one two");
    d.ed.cursor = 7; d.ed.anchor = 4;
    editor_action(4);                                                           /* Cut */
    CHECK(strcmp(test_clip, "two") == 0 && strcmp(d.ed.text[0], "one ") == 0, "Cut takes the selection out and puts it on the clipboard");
    snprintf(d.ed.text[0], sizeof d.ed.text[0], "alpha beta");
    d.ed.cursor = 10; d.ed.anchor = 6;
    tap(BTN_B);
    CHECK(strcmp(d.ed.text[0], "alpha ") == 0, "B with a selection deletes just the selection, not the word before it");
    snprintf(d.ed.text[0], sizeof d.ed.text[0], "alpha beta");
    d.ed.cursor = 10; d.ed.anchor = -1;
    tap(BTN_B);
    CHECK(strcmp(d.ed.text[0], "alpha ") == 0, "B with no selection still deletes the last word");
  }

  /* New blank patch: asks first, and changes nothing until the answer is yes */
  {
    if (pad.menu != -1) tap(BTN_B);
    d.layouts[3].tab[0] = TAB_EDITOR;
    desk_patches_loaded_example(&d, 0); desk_reload_editor(&d);
    int before = s.mem[REG_FOLD];
    CHECK(before == 6000 && strcmp(d.loaded_name, "Glass Bells") == 0, "Glass Bells is loaded for the test (FOLD 6000)");
    tap(BTN_MENU); tap(BTN_RIGHT); tap(BTN_DOWN); tap(BTN_A);                    /* the MENU tab's second row: New blank patch */
    CHECK(tab() == TAB_PATCHES && d.pat.mode == PAT_CONFIRM_NEW, "New blank patch goes to the Patches screen and asks");
    pad_draw(&pad);                                                             /* the question box draws (the handheld shows a dialog, not a text line) */
    CHECK(strcmp(d.loaded_name, "Glass Bells") == 0, "nothing is discarded before the answer");
    tap(BTN_B);
    CHECK(d.pat.mode == PAT_NORMAL && strcmp(d.loaded_name, "Glass Bells") == 0, "B says no and keeps the patch");
    desk_key(&d, 'n', MOD_CTRL);
    CHECK(d.pat.mode == PAT_CONFIRM_NEW, "Ctrl+N asks too");
    tap(BTN_A);
    CHECK(d.pat.mode == PAT_NORMAL && strcmp(d.loaded_name, "(blank)") == 0 && d.ed.text[0][0] == '\0', "A says yes: a blank patch with empty blocks");
    CHECK(s.mem[REG_FOLD] != before && !s.pulsers[0].on, "the registers are back to the defaults and the pulsers are off");
  }
}
