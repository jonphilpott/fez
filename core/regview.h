/* How registers and parameters are listed, formatted and edited, shared by every screen (handheld and desktop).
   Pure functions on a FezSynth: no drawing, no platform code. */
#ifndef FEZ_REGVIEW_H
#define FEZ_REGVIEW_H

#include <stddef.h>
#include "fez.h"

typedef struct { const char *name; int addr; int ro, wo, is_signed; } RegEntry;

/* Every register a person can see (address 100 up, no per-block P0..P15), sorted by address. Returns the count. */
int regview_list(RegEntry *out, int max);
/* Addresses of the parameter registers in table order, without GATE. Returns the count. */
int regview_params(int *out, int max);
const char *regview_param_name(const FezSynth *s, int addr);

/* A parameter's value in its own units: hertz, seconds, percent, a wave name, a note. */
void regview_fmt_param(char *out, size_t n, int addr, int value);
void regview_fmt_pct(char *out, size_t n, int value);
void regview_fmt_note(char *out, size_t n, int pitch);

/* One line of help (49 characters at most) for a register name; handles families such as CV# for CV0..CV3. */
const char *regview_help(const char *name);

/* Raw register access as a screen wants it: LEVEL is the panel pot, and writes skip the VM's fault rules. */
int  regview_read(FezSynth *s, int addr);
void regview_write(FezSynth *s, int addr, int value);
/* Parameters hold 0..32767. A step is a semitone (an octave when coarse) for PITCH and 512 (4096 coarse) otherwise. */
int  regview_pget(FezSynth *s, int addr);
void regview_pset(FezSynth *s, int addr, int value);
void regview_pstep(FezSynth *s, int addr, int dir, int coarse);

/* A note name such as "C4", "f#3", "Bb2" or "g" as a pitch in 8.8 semitones (MIDI note x 256, C4 = 15360). A name with
   no octave number takes default_octave. Returns 1 and sets *pitch, or 0 if the text is not a note. */
int regview_parse_note(const char *text, int default_octave, int *pitch);

/* What a word in block source is, for colouring and completion. */
enum { TOK_OTHER, TOK_OP, TOK_REG, TOK_NOTE, TOK_NUM };
int regview_token_kind(const char *token);

#endif
