/* The F2 setup page's two jobs that need no hardware: the settings survive a write and read back, and MIDI bytes do the
   right thing (notes play the keyboard device, CCs move the pots in each of the three modes). The device code itself
   (midi.c) talks to the operating system and is not built into the tests. */
#include <stdio.h>
#include <string.h>
#include "desk.h"
#include "test.h"

static FezSynth s;
static Desk d;

static void send(const unsigned char *b, int n) { desk_midi_bytes(&d, b, n); }

void test_setup(void) {
  fez_init(&s, 1); desk_init(&d, &s);

  /* 1) the settings as text, and back */
  Setup a; setup_defaults(&a);
  snprintf(a.audio, SETUP_NAME, "MacBook Pro Speakers"); snprintf(a.midi, SETUP_NAME, "Arturia BeatStep");
  a.chan = 3; a.pot[0].cc = 74; a.pot[0].mode = CC_ABS; a.pot[15].cc = 10; a.pot[15].mode = CC_REL2C;
  char text[2048]; setup_write(&a, text, sizeof text);
  Setup b; setup_read(&b, text);
  CHECK(!memcmp(&a, &b, sizeof a), "the setup survives a write and read back (names with spaces too)");
  setup_read(&b, "FEZ SETUP 1\npot 99 5 abs\npot 2 off rel64\nchannel 40\nnonsense\n");
  CHECK(b.pot[2].cc == -1 && b.pot[2].mode == CC_REL64 && b.chan == 16, "bad lines are skipped and numbers kept in range");

  /* 2) notes, with running status (the second note has no status byte) and a clock tick in the middle of a message */
  unsigned char on[] = { 0x90, 60, 100, 64, 100 };
  send(on, 3);  CHECK(s.mem[REG_KBGATE] == 1 && s.mem[REG_KBNOTE] == 60 * 256, "note on plays the keyboard device");
  send(on + 3, 2); CHECK(s.mem[REG_KBNOTE] == 64 * 256, "running status: data bytes alone reuse the last status");
  unsigned char off[] = { 0x80, 64, 0, 0x90, 0xF8, 60, 0 };
  send(off, 7); CHECK(s.mem[REG_KBGATE] == 0, "note off, and note on with velocity 0, let go (a clock byte mid-message is skipped)");

  /* 3) the channel: set to 2, a message on channel 1 is shown but not played */
  d.setup.chan = 2;
  send(on, 3);  CHECK(s.mem[REG_KBGATE] == 0 && strstr(d.midi_last, "not heard"), "another channel is ignored, and the page says so");
  d.setup.chan = 0;

  /* 4) CCs move pots: absolute, and the two kinds of encoder */
  d.setup.pot[0].cc = 74; d.setup.pot[0].mode = CC_ABS;
  d.setup.pot[1].cc = 20; d.setup.pot[1].mode = CC_REL64;
  d.setup.pot[2].cc = 21; d.setup.pot[2].mode = CC_REL2C;
  unsigned char cc[] = { 0xB0, 74, 127, 74, 0 };
  send(cc, 3);  CHECK(fez_reg_read(&s, REG_MP0) == 32767, "ABS: 127 is the top");
  send(cc + 3, 2); CHECK(fez_reg_read(&s, REG_MP0) == 0, "ABS: 0 is the bottom");
  fez_set_pot(&s, 1, 16384); fez_set_pot(&s, 2, 16384);
  unsigned char rel[] = { 0xB0, 20, 66, 20, 63, 21, 3, 21, 127 };
  send(rel, 3); CHECK(fez_reg_read(&s, REG_MP1) == 16384 + 2 * 256, "REL64: 66 is two clicks up");
  send(rel + 3, 2); CHECK(fez_reg_read(&s, REG_MP1) == 16384 + 256, "REL64: 63 is one click down");
  send(rel + 5, 2); CHECK(fez_reg_read(&s, REG_MP2) == 16384 + 3 * 256, "REL2C: 3 is three clicks up");
  send(rel + 7, 2); CHECK(fez_reg_read(&s, REG_MP2) == 16384 + 2 * 256, "REL2C: 127 is one click down");
  CHECK(!strcmp(d.midi_last, "CC 21 = 127  ch 1"), "the page shows the last message");

  /* 5) a gamepad on the desktop: STICKS shows only while one is plugged in; there its left stick plays and A B X Y fire pulsers */
  CHECK(!desk_tab_shown(&d, TAB_STICKS), "no gamepad: no STICKS tab on the desktop");
  d.gamepads = 1;
  CHECK(desk_tab_shown(&d, TAB_STICKS), "a gamepad plugged in shows it");
  desk_gamepad_axis(&d, 0, 32767); CHECK(fez_reg_read(&s, REG_MP0) == 32767, "elsewhere the left stick is a pair of pots");
  desk_gamepad_axis(&d, 0, 0);
  d.layouts[d.layout].tab[d.focus] = TAB_STICKS;
  desk_gamepad_axis(&d, 1, -32767); CHECK(s.mem[REG_KBGATE] == 1 && s.mem[REG_KBNOTE] == 60 * 256, "on STICKS, up plays the root");
  s.vm.pending = 0; desk_gamepad_button(&d, 3); CHECK(s.vm.pending == 8, "Y fires P3");
  d.layouts[d.layout].tab[d.focus] = TAB_EDITOR; desk_tick(&d, 16);
  CHECK(s.mem[REG_KBGATE] == 0, "leaving STICKS lets go of the note");
  desk_gamepad_axis(&d, 1, 0);
  s.vm.pending = 0; desk_gamepad_button(&d, 3); CHECK(s.vm.pending == 0, "elsewhere the buttons do nothing");
  d.gamepads = 0;

  /* 6) the page: F2 opens it, it draws, Esc closes it */
  static uint32_t px[1024 * 600];
  desk_set_canvas(&d, px, 1024, 600);
  desk_key(&d, KEY_F2, 0); CHECK(d.setup_open, "F2 opens the setup page");
  desk_draw(&d);
  desk_key(&d, KEY_ESC, 0); CHECK(!d.setup_open, "Esc closes it");
}
