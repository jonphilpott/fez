/* Patch file tests: every built-in patch is written out, read back, and must behave exactly like the original. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fez.h"
#include "patchfile.h"
#include "scales.h"
#include "test.h"

/* A FezSynth holds the audio buffers, so it is big: keep these off the stack. */
static FezSynth orig, copy;
static char text1[65536], text2[65536];

/* Same registers, routes, pulsers, knobs, devices and code? */
static int same_setup(const FezSynth *a, const FezSynth *b) {
  if (memcmp(a->mem + REG_PITCH, b->mem + REG_PITCH, (NUM_REGS - REG_PITCH) * sizeof a->mem[0])) return 0;
  if (a->nroutes != b->nroutes || memcmp(a->routes, b->routes, a->nroutes * sizeof a->routes[0])) return 0;
  for (int i = 0; i < NUM_PULSERS; i++)
    if (a->pulsers[i].on != b->pulsers[i].on || a->mem[REG_PRATE0 + i] != b->mem[REG_PRATE0 + i]) return 0;
  if (a->tm.bits != b->tm.bits) return 0;
  if (a->master_level != b->master_level) return 0;
  if (a->ca.rule != b->ca.rule || a->ca.row != b->ca.row || a->ca.tap != b->ca.tap || a->ca.trig != b->ca.trig) return 0;
  if (a->mem[REG_LZRATE] != b->mem[REG_LZRATE] || a->lz.rho != b->lz.rho || a->lz.trig != b->lz.trig) return 0;
  for (int i = 0; i < NUM_BLOCKS; i++)
    if (strcmp(a->vm.blocks[i].src, b->vm.blocks[i].src) || strcmp(a->vm.word_src[i], b->vm.word_src[i])) return 0;
  return 1;
}

void test_patchfile(void) {
  PatchInfo info, back;
  char err[160];
  for (int p = 0; p < PATCH_COUNT; p++) {
    fez_init(&orig, 7); fez_load_patch(&orig, p);
    if (p == 3) { orig.tm.bits = 0x1234; orig.mem[REG_QON] = 1; orig.mem[REG_QSCALE] = 5; }          /* quantiser on, and a Turing pattern: both must survive */
    snprintf(info.name, sizeof info.name, "%s", PATCH_TABLE[p].name);
    snprintf(info.desc, sizeof info.desc, "%s", PATCH_TABLE[p].desc);
    snprintf(info.listen, sizeof info.listen, "%s", PATCH_TABLE[p].listen);

    int n1 = patchfile_write(&orig, &info, text1, sizeof text1);
    CHECK(n1 > 0, "patch writes");
    fez_init(&copy, 7);
    int bad = patchfile_read(&copy, text1, &back, err, sizeof err);
    if (bad) printf("  %s: %s\n", info.name, err);
    CHECK(bad == 0, "patch reads back with no skipped lines");
    CHECK(!strcmp(back.name, info.name) && !strcmp(back.desc, info.desc) && !strcmp(back.listen, info.listen), "name, description and listen survive");
    CHECK(same_setup(&orig, &copy), "reloaded setup equals the original");

    int n2 = patchfile_write(&copy, &back, text2, sizeof text2);
    CHECK(n2 == n1 && !strcmp(text1, text2), "writing the reloaded patch gives the same text");

    /* the real proof: run both and compare. Same seed, so the random numbers match. */
    orig.rng = copy.rng = 12345;
    for (int t = 0; t < 600; t++) { fez_tick(&orig, 5.0); fez_tick(&copy, 5.0); }
    CHECK(memcmp(orig.mem + REG_PITCH, copy.mem + REG_PITCH, (NUM_REGS - REG_PITCH) * sizeof orig.mem[0]) == 0, "600 ticks later the registers still match");
  }

  /* the keyboard's settings are saved by name; its live state (KBNOTE, KBGATE) is not */
  {
    fez_init(&orig, 7); fez_load_blank(&orig);
    fez_reg_write(&orig, REG_KBOCT, 5); fez_reg_write(&orig, REG_KBPUL, 3); fez_note_on(&orig, 61);
    snprintf(info.name, sizeof info.name, "keys"); info.desc[0] = info.listen[0] = '\0';
    int n1 = patchfile_write(&orig, &info, text1, sizeof text1);
    CHECK(n1 > 0 && strstr(text1, "reg KBOCT 5") && strstr(text1, "reg KBPUL 3"), "keyboard octave and pulsers are written by name");
    CHECK(!strstr(text1, "KBNOTE") && !strstr(text1, "KBGATE"), "the keyboard's live note and gate are not written");
    fez_init(&copy, 7);
    CHECK(patchfile_read(&copy, text1, &back, err, sizeof err) == 0 && copy.mem[REG_KBOCT] == 5 && copy.mem[REG_KBPUL] == 3 && copy.mem[REG_KBGATE] == 0, "and read back, with no key held");
  }

  /* the blank patch starts the devices from their standard settings */
  fez_load_blank(&copy);
  CHECK(lz_rho(copy.lz.rho) > 27.999 && lz_rho(copy.lz.rho) < 28.001, "blank Lorenz: rho is 28");
  CHECK(copy.mem[REG_LZRATE] == 8000 && copy.lz.trig == 4 && copy.lz.x == 1 && copy.lz.y == 1 && copy.lz.z == 1, "blank Lorenz: default speed, no pulser, start at (1, 1, 1)");
  CHECK(copy.ca.rule == 30 && copy.ca.row == 0x2a54 && copy.ca.trig == 4, "blank automaton: rule 30, the usual seed, no pulser");
  CHECK(copy.nroutes == 0 && !copy.pulsers[0].on, "blank patch has no routes and no running pulsers");

  /* damaged and foreign files */
  /* looping grains that are playing come back playing; continuous voices and stopped grains are not in the mask */
  { static FezSynth a, b2; static char text[65536]; static float out[2 * 480]; PatchInfo pi = { 0 };
    fez_init(&a, 1); fez_load_blank(&a);
    for (int i = 0; i < 8; i++) { fez_reg_write(&a, REG_GLEN0 + i, i < 3 ? 0 : 300); fez_reg_write(&a, REG_GFLG0 + i, 8); fez_reg_write(&a, REG_GLVL0 + i, 10000); }
    fez_reg_write(&a, REG_GTRIG, 255 & ~(1 << 7));            /* every voice but 7 */
    fez_render_stereo(&a, out, 480);
    int n = patchfile_write(&a, &pi, text, sizeof text);
    CHECK(n > 0 && strstr(text, "grains 120\n"), "the playing looping grains (3 to 6) are saved as a mask");
    fez_init(&b2, 1);
    CHECK(patchfile_read(&b2, text, &back, err, sizeof err) == 0, "and the file reads back cleanly");
    fez_render_stereo(&b2, out, 480);
    int on = 0; for (int i = 0; i < 8; i++) on |= b2.audio.buf.vo[i].on << i;
    CHECK(on == 120, "after a load, voices 3 to 6 are playing again, and 7 is not"); }

  CHECK(patchfile_read(&copy, "hello", &back, err, sizeof err) == -1, "not a patch file");
  CHECK(patchfile_read(&copy, "FEZ PATCH 3\nlevel 20000\ncutoff 0\n", &back, err, sizeof err) == 0 && copy.master_level == 20000,
        "an old file's unused cutoff line is accepted quietly");
  CHECK(patchfile_read(&copy, "FEZ PATCH 99\n", &back, err, sizeof err) == -1, "a newer version is refused");
  /* FMRATE (gone in version 3): older files keep the mod oscillator's frequency, as MDPITCH */
  int bad0 = patchfile_read(&copy, "FEZ PATCH 1\nreg FMRATE 21300\n", &back, err, sizeof err);
  CHECK(bad0 == 0 && abs(copy.mem[REG_MDPITCH] - 11512) <= 2, "version 1: FMRATE 21300 (109.8 Hz) becomes MDPITCH just under A2, quietly");
  patchfile_read(&copy, "FEZ PATCH 2\nreg FMRATE 21300\nreg FMAMT 4000\n", &back, err, sizeof err);
  CHECK(abs(copy.mem[REG_MDPITCH] - 11512) <= 2, "version 2, FM heard and the mod voice silent: the FM speed wins");
  patchfile_read(&copy, "FEZ PATCH 2\nreg FMRATE 21300\nreg FMAMT 4000\nreg MDLVL 9000\nreg MDPITCH 9000\n", &back, err, sizeof err);
  CHECK(copy.mem[REG_MDPITCH] == 9000, "version 2, mod voice heard: it keeps its pitch");
  patchfile_read(&copy, "FEZ PATCH 2\nreg FMRATE 21300\n", &back, err, sizeof err);
  CHECK(copy.mem[REG_MDPITCH] == 15360, "version 2, no FM: MDPITCH as written (here the default, C4)");
  /* DLYMIX: a file before version 3 that left it out had the old default, 9000; from version 3 the default is dry */
  CHECK(copy.mem[REG_DLYMIX] == 9000 && copy.mem[REG_SPRMIX] == 8000, "version 2: no DLYMIX or SPRMIX line is the old default");
  patchfile_read(&copy, "FEZ PATCH 2\nreg DLYMIX 100\n", &back, err, sizeof err);
  CHECK(copy.mem[REG_DLYMIX] == 100, "version 2: a DLYMIX line still wins");
  patchfile_read(&copy, "FEZ PATCH 3\n", &back, err, sizeof err);
  CHECK(copy.mem[REG_DLYMIX] == 0 && copy.mem[REG_SPRMIX] == 0, "version 3: no DLYMIX or SPRMIX line is dry");
  CHECK(copy.mem[REG_SPRTONE] == 16384, "version 3: no SPRTONE line is the default, halfway");
  patchfile_read(&copy, "FEZ PATCH 2\n", &back, err, sizeof err);
  CHECK(copy.mem[REG_SPRTONE] == 0, "version 2: the old, metallic spring");
  int bad = patchfile_read(&copy, "FEZ PATCH 1\nname X\nreg NOSUCH 5\nreg CUTOFF 123\nbogus line\n", &back, err, sizeof err);
  CHECK(bad == 2 && copy.mem[REG_CUTOFF] == 123 && !strncmp(err, "line 3", 6), "bad lines are skipped and counted, good ones still load");
  CHECK(patchfile_write(&orig, &info, text1, 20) == -1, "a buffer that is too small is reported");

  /* a file cannot hold what code could not write: the same range checks apply */
  fez_init(&copy, 7);
  patchfile_read(&copy, "FEZ PATCH 1\nreg BUFMS 0\nreg ATK 5\n", &back, err, sizeof err);
  CHECK(copy.mem[REG_BUFMS] == 50, "a file's BUFMS 0 is raised to the smallest length, as a block's write would be");
  CHECK(patchfile_read(&copy, "FEZ PATCH 1\nreg KBGATE 1\n", &back, err, sizeof err) == 1 && copy.mem[REG_KBGATE] == 0, "a read-only register in a file is refused");
  patchfile_read(&copy, "FEZ PATCH 1\nca 30 5 3 -1\nlz 8000 15291 -1\n", &back, err, sizeof err);
  CHECK(copy.ca.trig == 0 && copy.lz.trig == 0, "device trig numbers from a file are kept in range");

  /* a very long line is cut short, not written past the end of a buffer */
  { static char longtext[3000];
    int n = snprintf(longtext, sizeof longtext, "FEZ PATCH 1\ndesc ");
    memset(longtext + n, 'x', 2000); longtext[n + 2000] = '\0';
    patchfile_read(&copy, longtext, &back, err, sizeof err);
    CHECK(strlen(back.desc) == PATCH_TEXT_MAX - 1, "an overlong description is cut to fit"); }

  /* word source longer than a word can hold is refused, so what runs is always what a save would write */
  { static char longword[1000];
    int n = snprintf(longword, sizeof longword, "FEZ PATCH 1\nword 0\n> \\ ");
    memset(longword + n, 'c', 300); n += 300;
    snprintf(longword + n, sizeof longword - n, "\n> 7\n");
    fez_init(&copy, 7);
    CHECK(patchfile_read(&copy, longword, &back, err, sizeof err) >= 1, "word source over the word limit is reported"); }

  /* routes are written and read by source NAME, so renumbering the sources can never change a saved patch */
  fez_init(&orig, 7); fez_load_blank(&orig);
  fez_set_route(&orig, REG_MP9, REG_CUTOFF, 4000, &(unsigned){0});
  fez_set_route(&orig, REG_KBNOTE, REG_FOLD, -2000, &(unsigned){0});
  CHECK(patchfile_write(&orig, &info, text1, sizeof text1) > 0, "a patch with routes writes");
  CHECK(strstr(text1, "route MP9 CUTOFF 4000") && strstr(text1, "route KB FOLD -2000"), "routes are written with the source's name");
  fez_init(&copy, 7);
  CHECK(patchfile_read(&copy, text1, &back, err, sizeof err) == 0 && copy.nroutes == 2 && copy.routes[0].src == orig.routes[0].src && copy.routes[1].src == orig.routes[1].src, "named routes read back to the same sources");
  fez_init(&copy, 7);
  CHECK(patchfile_read(&copy, "FEZ PATCH 1\nroute mp9 cutoff 4000\n", &back, err, sizeof err) == 0 && copy.nroutes == 1, "source and register names are not case sensitive");
  fez_init(&copy, 7);
  CHECK(patchfile_read(&copy, "FEZ PATCH 1\nroute 2 CUTOFF 4000\n", &back, err, sizeof err) == 1 && copy.nroutes == 0, "an old numbered route is skipped and counted, not misread");
  CHECK(patchfile_read(&copy, "FEZ PATCH 1\nroute NOSUCH CUTOFF 4000\n", &back, err, sizeof err) == 1, "an unknown source name is skipped");
}
