/* Patch files: a whole synth setup as plain text, and back.

   Pure functions on memory: no files, no malloc, so the same code runs on a desktop, a Pi or a microcontroller.
   The app decides where the text lives (a folder of .fez files on a computer, flash on hardware).

   What is saved: the registers that differ from the defaults, the master volume, the pulsers, the matrix routes,
   the cellular automaton and Lorenz settings, and the source of every block and word. What is not saved: the
   recorded buffer audio, and anything that is only in motion (the sequencer position, the LFO phase, the
   envelope). Loading a file therefore starts those from rest, like loading a built-in patch does.

   File shape (every line is "keyword values"; blank lines and lines starting with # are ignored):

     FEZ PATCH 3
     name Glass Bells
     desc A few words about the patch
     listen What to listen for
     level 20000                  <- the master volume (LEVEL)
     pulser 0 on 1 rate 120       <- pulser number, on (0 or 1), rate in pulses per minute
     reg CUTOFF 12000             <- a register by name (or by address); only those that differ from the default
     route ENV CUTOFF 4000        <- matrix: source name, destination register, weight
     ca 30 5 3 4                  <- rule, row, tap, trig (4 = none)
     lz 8000 15291 4               <- rate, rho (register scale: 15291 is rho 28), trig (4 = none)
     block 3                      <- the source of block 3, each line starting with "> "
     > 1 DROP
     word 2                       <- the same for a word
     > DUP + */

#ifndef FEZ_PATCHFILE_H
#define FEZ_PATCHFILE_H

#include <stddef.h>
#include "fez.h"

#define PATCH_FILE_VERSION 3     /* 2: MDPITCH exists, a version 1 file takes it from FMRATE; 3: DLYMIX and SPRMIX default to 0, not 9000 and 8000, SPRTONE arrives (older files get 0, the old spring), and FMRATE is gone (MDPITCH again tunes the one mod oscillator); all 26 Nov 2026 */
#define PATCH_NAME_MAX 48
#define PATCH_TEXT_MAX 256

typedef struct { char name[PATCH_NAME_MAX]; char desc[PATCH_TEXT_MAX]; char listen[PATCH_TEXT_MAX]; } PatchInfo;

/* Write the synth's setup into out (NUL-terminated). Returns the length, or -1 if cap is too small. */
int patchfile_write(const FezSynth *s, const PatchInfo *info, char *out, size_t cap);

/* Replace the whole machine state with the file's. Lines it cannot understand are skipped and counted.
   Returns the number of skipped lines (0 = clean), or -1 if the text is not a patch file at all.
   If err is not NULL, it receives a message about the first skipped line. The caller must hold the audio lock. */
int patchfile_read(FezSynth *s, const char *text, PatchInfo *info, char *err, size_t errcap);

/* Only the name, description and listen text of a file (to show in a list), without touching any synth. */
void patchfile_info(const char *text, PatchInfo *info);

#endif
