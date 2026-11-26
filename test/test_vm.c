/* Tests for the language itself: what the compiler accepts, and the rule that every token is one slot. */
#include <stdio.h>
#include <string.h>
#include "fez.h"
#include "reg_names.h"
#include "vm.h"
#include "test.h"

static Compiled c;

/* Compile some source; returns 1 if it compiled. The result is left in c. */
static int compiles(const char *src) { vm_compile(&c, src, BLOCK_SLOTS); return c.ok; }

/* Run source as block 0 on a fresh synth and read back a scratch register. */
static int run_and_read(const char *src, int reg) {
  static FezSynth s;
  fez_init(&s, 1);
  vm_set_block(&s.vm, 0, src);
  fez_reg_write(&s, REG_RUN, 0);
  fez_tick(&s, 20);
  return fez_reg_read(&s, reg);
}

void test_vm(void) {
  /* SCALE: degree 0 is the root, degrees wrap into the next octave, negatives go below */
  CHECK(run_and_read("2 1 SCALE PITCH !", REG_PITCH) == 1024, "major scale degree 2 is the third, 4 semitones up");
  CHECK(run_and_read("7 1 SCALE PITCH !", REG_PITCH) == 3072, "major scale degree 7 is an octave up");
  CHECK(run_and_read("5 4 SCALE PITCH !", REG_PITCH) == 3072, "pentatonic degree 5 is an octave up");
  CHECK(run_and_read("1 NEG 1 SCALE 15360 + PITCH !", REG_PITCH) == 15104, "major scale degree -1 is the B below C4");
  CHECK(run_and_read("13 0 SCALE PITCH !", REG_PITCH) == 3328, "chromatic degree 13 is an octave and a semitone up");
  CHECK(run_and_read("3 9 SCALE PITCH !", REG_PITCH) == 1280, "scale 9 wraps to 1 (major): degree 3 is the fourth");
  /* error positions: an editor can flag the word that failed (compile errors, and the slot that halted a running block) */
  {
    const char *src = "1 2 FOO 3";
    CHECK(!compiles(src) && c.err_pos == (int)(strstr(src, "FOO") - src), "a compile error says where the bad token starts");
    src = "\\ a comment\n10 20 +\n5 -7";
    CHECK(!compiles(src) && c.err_pos == (int)(strstr(src, "-7") - src), "the position counts past comments and lines");
    src = "1 2 + 3 4";
    CHECK(compiles(src) && c.pos[0] == 0 && c.pos[2] == 4 && c.pos[4] == 8, "each slot remembers where its token starts");
    static FezSynth s;
    fez_init(&s, 1);
    src = "10 0 / 5 DROP DROP DROP";                                /* a divide by zero (carries on), then a real halt */
    vm_set_block(&s.vm, 0, src);
    CHECK(s.vm.blocks[0].fault_slot == -1, "a fresh block has no fault slot");
    vm_run_block(&s.vm, 0);
    CHECK(s.vm.blocks[0].halted && s.vm.blocks[0].c.pos[s.vm.blocks[0].fault_slot] == (int)(strrchr(src, 'D') - src),
          "a halt points at the instruction that halted, not at the earlier divide by zero");
    vm_set_word(&s.vm, 0, "DROP");
    vm_set_block(&s.vm, 0, "0 W");
    vm_run_block(&s.vm, 0);
    CHECK(s.vm.blocks[0].halted && s.vm.blocks[0].c.pos[s.vm.blocks[0].fault_slot] == 2, "a fault inside a word points at the W that called it");
    vm_set_word(&s.vm, 0, "");
  }
  /* a number has no sign: the sign is the word NEG, so a token is never two slots */
  CHECK(!compiles("1 -5 +"), "a negative literal is refused");
  CHECK(strstr(c.err, "5 NEG") != NULL, "the error says to write 5 NEG");
  CHECK(strstr(c.err, "line 1") != NULL, "the error says which line");
  CHECK(!compiles("-.5"), "a negative fraction is refused");
  CHECK(strstr(c.err, ".5 NEG") != NULL, "the error says to write .5 NEG");
  CHECK(compiles("5 NEG") && c.count == 2, "5 NEG is two tokens and two slots");
  CHECK(compiles("3 2 -") && c.count == 3, "a bare minus is the subtract word");

  /* every kind of token is exactly one slot: a number, hex, a fraction, a note name, a register name, a word */
  CHECK(compiles("1 0x10 .5 C4 PITCH DUP ST") && c.count == 7, "seven tokens of seven kinds make seven slots");
  CHECK(compiles("C-1 G9") && c.count == 2, "a note name with a minus in the octave is still one token");

  /* literals stop at 15 bits, with a message that says so */
  CHECK(compiles("32767") && c.count == 1, "32767 is the largest literal");
  CHECK(!compiles("32768") && strstr(c.err, "out of range") != NULL, "32768 is out of range");

  /* SKIP skips exactly one token */
  CHECK(run_and_read("7 0 SKIP 5 NEG S0 !", REG_S0) == -7, "SKIP with a 0 flag skips the 5 only; NEG then acts on the 7");
  CHECK(run_and_read("7 1 SKIP 5 NEG S0 !", REG_S0) == -5, "SKIP with a 1 flag skips nothing");

  /* two multiplies: * for whole numbers, F* for fractions (32768 is 1.0) */
  CHECK(run_and_read("6 7 * S0 !", REG_S0) == 42, "* multiplies whole numbers");
  CHECK(run_and_read("300 300 * S0 !", REG_S0) == 32767, "* sticks at the top like +");
  CHECK(run_and_read("300 NEG 300 * S0 !", REG_S0) == -32768, "* sticks at the bottom");
  CHECK(run_and_read(".5 .5 F* S0 !", REG_S0) == 8192, "F* multiplies fractions");
  CHECK(run_and_read("20000 .5 F* S0 !", REG_S0) == 10000, "F* scales a number by a half");
  CHECK(run_and_read("6 7 F* S0 !", REG_S0) == 0, "F* of two small whole numbers is 0");
  /* rotate: bits that fall off one end come back at the other, and none are lost */
  CHECK(run_and_read("1 4 ROL S0 !", REG_S0) == 16, "ROL moves a bit left");
  CHECK(run_and_read("0x4000 1 ROL S0 !", REG_S0) == -32768, "ROL moves a bit into the sign position");
  CHECK(run_and_read("1 NEG 1 ROL S0 !", REG_S0) == -1, "ROL of all ones is all ones");
  CHECK(run_and_read("0x4001 1 ROL S0 !", REG_S0) == -32766, "ROL of 0x4001 leaves 0x8002 (-32766)");
  CHECK(run_and_read("1 1 ROR S0 !", REG_S0) == -32768, "ROR wraps the bottom bit round to the top");
  CHECK(run_and_read("1 NEG 1 ROR S0 !", REG_S0) == -1, "ROR of all ones is all ones");
  CHECK(run_and_read("0x1234 0 ROL S0 !", REG_S0) == 0x1234, "ROL by 0 changes nothing");
  CHECK(run_and_read("0x1234 16 ROR S0 !", REG_S0) == 0x1234, "ROL and ROR use the low four bits of n: 16 is 0");
  CHECK(run_and_read("0x1234 5 ROL 5 ROR S0 !", REG_S0) == 0x1234, "ROR undoes ROL");
  CHECK(run_and_read("0x1234 3 ROL S1 ! 0x1234 13 ROR S0 !", REG_S0) == run_and_read("0x1234 3 ROL S0 !", REG_S0), "ROL n is ROR 16-n");
  /* sixteen modulation pots: set by the host, read by blocks, usable as matrix sources */
  {
    static FezSynth s;
    fez_init(&s, 1);
    fez_load_blank(&s);                       /* no routes and plain registers, so the sums below are exact */
    fez_set_pot(&s, 0, 1000); fez_set_pot(&s, 15, 32767); fez_set_pot(&s, 7, 99999); fez_set_pot(&s, 3, -5);
    CHECK(fez_reg_read(&s, REG_MP0) == 1000 && fez_reg_read(&s, REG_MP15) == 32767, "the first and last pot read back");
    CHECK(fez_reg_read(&s, REG_MP7) == 32767 && fez_reg_read(&s, REG_MP3) == 0, "a pot value is clamped to 0..32767");
    CHECK(fez_matrix_source(&s, 7) == 1000 && fez_matrix_source(&s, 22) == 32767, "MP0 is source 7 and MP15 is source 22");
    vm_set_block(&s.vm, 0, "MP15 @ S0 ! 5 MP12 !");
    fez_reg_write(&s, REG_RUN, 0); fez_tick(&s, 20);
    CHECK(fez_reg_read(&s, REG_S0) == 32767, "a block can read a pot");
    CHECK(s.vm.blocks[0].fault != 0, "a block cannot write a pot");
    vm_set_block(&s.vm, 0, "MP12 CUTOFF 16384 ROUTE MP5 CUTOFF 0 ROUTE");
    fez_reg_write(&s, REG_RUN, 0); fez_tick(&s, 20);
    fez_set_pot(&s, 12, 32767);
    fez_tick(&s, 20);
    CHECK(s.eff[REG_CUTOFF] == fez_reg_read(&s, REG_CUTOFF) + 16384, "a route from MP12 adds half its value to the cutoff");
  }

  /* numbers stick at the ends and never wrap (the manual's examples) */
  CHECK(run_and_read("30000 5000 + S0 !", REG_S0) == 32767, "a sum past the top sticks at 32767");
  CHECK(run_and_read("30000 NEG 5000 - S0 !", REG_S0) == -32768, "a difference past the bottom sticks at -32768");
  CHECK(run_and_read("30000 5000 + 5000 - S0 !", REG_S0) == 27767, "the check happens at every step");
  CHECK(run_and_read("30000 5000 - 5000 + S0 !", REG_S0) == 30000, "subtracting first keeps the value");
  CHECK(run_and_read("32767 NEG 1 - NEG S0 !", REG_S0) == 32767, "NEG of -32768 sticks at 32767");
  CHECK(run_and_read("1 NEG 4 SHR S0 !", REG_S0) == 4095, "SHR works on the bits and does not stick");
}
