/* Test runner: builds to ./test_core (make test). Each test_* function lives in its own file. */
#include <stdio.h>
#include <string.h>
#include "tables.h"
#include "reg_names.h"
#include "fez.h"
#include "vm.h"
#include "words.h"
#include "test.h"

int test_fails = 0;

void test_tables(void) {
  CHECK(PARAM_COUNT > 30, "param table has entries");
  CHECK(PATCH_COUNT == 13, "thirteen example patches");
  CHECK(strcmp(PATCH_TABLE[0].name, "Glass Bells") == 0, "first patch is Glass Bells");
  for (int i = 0; i < HELP_COUNT; i++)
    CHECK(strlen(HELP_TABLE[i].text) <= 49, "help line fits the Brick screen (49 characters)");
  int found = 0;
  for (int i = 0; i < PARAM_COUNT; i++) if (strcmp(PARAM_TABLE[i].name, "MDRES") == 0 && PARAM_TABLE[i].addr == REG_MDRES) found = 1;
  CHECK(found, "the table and the generated REG_MDRES agree on where MDRES lives");
  CHECK(SRC_COUNT == NUM_SOURCES, "the matrix source table and NUM_SOURCES agree");
  /* the register map: no two parameters share an address, the complex oscillator's wave and blend sit in the voice run
     (between PITCH and GATE), and numbered families are contiguous */
  for (int i = 0; i < PARAM_COUNT; i++)
    for (int j = i + 1; j < PARAM_COUNT; j++)
      CHECK(PARAM_TABLE[i].addr != PARAM_TABLE[j].addr, "two parameters share an address");
  CHECK(REG_CXWAVE > REG_PITCH && REG_CXBLEND < REG_GATE && REG_GATE < REG_CV0, "CXWAVE and CXBLEND belong to the voice run");
  CHECK(REG_GATELEVEL == REG_GATE + 1 && REG_CV0 == REG_GATELEVEL + 1, "GATELEVEL sits in the voice run, between GATE and CV0");
  CHECK(REG_CV3 == REG_CV0 + 3 && REG_MP15 == REG_MP0 + 15, "numbered families are contiguous");
  CHECK(!strcmp(SRC_NAMES[7], "MP0") && !strcmp(SRC_NAMES[22], "MP15") && !strcmp(SRC_NAMES[23], "CADENS") && !strcmp(SRC_NAMES[33], "KB"), "the sixteen pots are matrix sources 7..22, in one run");
  CHECK(HZ_TAB[0] == 0 && HZ_TAB[256] == 65536, "hz table runs from log2(1) to log2(2) in Q16");
  /* the word table describes every operation, in the same order as the compiler's list, with every field filled in */
  CHECK(PARAM_COUNT <= PARAM_MAX, "every parameter has a smoothing slot in the audio side (PARAM_MAX in audio.h)");
  CHECK(WORD_COUNT == OP_COUNT, "the word table has one entry per operation");
  for (int i = 0; i < WORD_COUNT && i < OP_COUNT; i++) {
    CHECK(strcmp(WORD_TABLE[i].name, OP_NAMES[i]) == 0, "the word table runs in the same order as OP_NAMES");
    CHECK(WORD_TABLE[i].stack[0] == '(' && WORD_TABLE[i].text[0] != '\0', "every word has a stack picture and a description");
  }
}

int main(void) {
  test_tables();
  test_vm();
  test_patchfile();
  test_audio();
  test_pad();
  if (test_fails) { printf("%d test(s) failed\n", test_fails); return 1; }
  printf("all tests ok\n");
  return 0;
}
