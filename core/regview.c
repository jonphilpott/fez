#include "regview.h"
#include "scales.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- lists */

const RegGroup REG_GROUPS[] = {
  { "VOICE", REG_PITCH },      { "CV", REG_CV0 },         { "MIXER", REG_MIXCX },
  { "MOD VOICE", REG_MDWAVE }, { "DELAY", REG_DLYTIME },  { "SLEW", REG_SLT0 },        { "SCRATCH", REG_S0 },
  { "CELLULAR", REG_CARULE },  { "LORENZ", REG_LZRATE },  { "SEQUENCER", REG_SQSTEP }, { "BUFFER", REG_MIXBF },
  { "GRAINS", REG_GPOS0 },     { "PULSERS", REG_BP0 },    { "QUANTISER", REG_QON },    { "TURING", REG_TMPUL },
  { "KEYBOARD", REG_KBNOTE }, { "POTS", REG_MP0 } };
const int REG_GROUP_COUNT = (int)(sizeof REG_GROUPS / sizeof REG_GROUPS[0]);

int regview_list(RegEntry *out, int max) {
  /* NAME_TABLE is in the JS's definition order. A stable insertion sort by address keeps equal addresses in order. */
  int n = 0;
  for (int i = 0; i < NAME_COUNT && n < max; i++) {
    int a = NAME_TABLE[i].addr;
    if (a < REG_PITCH || (a >= REG_P0 && a <= REG_P15)) continue;
    RegEntry e;
    e.name = NAME_TABLE[i].name; e.addr = a;
    e.ro = a == REG_LEVEL || (a >= REG_MP0 && a <= REG_MP15) || (a >= REG_SLO0 && a <= REG_SLO3) || (a >= REG_CACOUNT && a <= REG_CADENS) || a == REG_CABIT || (a >= REG_LZX && a <= REG_LZZ) || a == REG_SQOUT || a == REG_BWPOS || a == REG_BWSTAT || a == REG_TMOUT || a == REG_KBNOTE || a == REG_KBGATE;
    e.is_signed = (a >= REG_CV0 && a <= REG_CV3) || (a >= REG_S0 && a <= REG_S15);
    e.wo = a == REG_CASTEP || a == REG_LZSEED || a == REG_GATE || a == REG_SQCLK || a == REG_BWTRIG || a == REG_GTRIG || a == REG_GSTOP || a == REG_TMSTEP;
    int k = n++;
    while (k > 0 && out[k - 1].addr > a) { out[k] = out[k - 1]; k--; }
    out[k] = e;
  }
  return n;
}

int regview_params(int *out, int max) {
  int n = 0;
  for (int i = 0; i < PARAM_COUNT && n < max; i++) if (PARAM_TABLE[i].addr != REG_GATE) out[n++] = PARAM_TABLE[i].addr;
  return n;
}

const char *regview_param_name(const FezSynth *s, int addr) { return PARAM_TABLE[s->param_index[addr]].name; }

/* ---------------------------------------------------------------- formatting */

static void fmt_hz(char *o, size_t n, double f) {
  if (f >= 1000) snprintf(o, n, f >= 10000 ? "%.0fk" : "%.1fk", f / 1000);
  else if (f >= 100) snprintf(o, n, "%.0fHz", floor(f + 0.5));
  else snprintf(o, n, "%.1fHz", f);
}
static void fmt_sec(char *o, size_t n, double s) { if (s < 1) snprintf(o, n, "%.0fms", floor(s * 1000 + 0.5)); else snprintf(o, n, "%.1fs", s); }

void regview_fmt_pct(char *o, size_t n, int v) { snprintf(o, n, "%.0f%%", floor(v / 32767.0 * 100 + 0.5)); }

/* A pitch in 8.8 semitones as a note name with cents, e.g. C4 or A4+12. */
void regview_fmt_note(char *o, size_t n, int p) {
  static const char *NN[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
  double m = p / 256.0;
  int note = (int)floor(m + 0.5), cents = (int)floor((m - note) * 100 + 0.5);
  int idx = ((note % 12) + 12) % 12, oct = (int)floor(note / 12.0) - 1;
  if (cents == 0) snprintf(o, n, "%s%d", NN[idx], oct); else snprintf(o, n, "%s%d%s%d", NN[idx], oct, cents > 0 ? "+" : "", cents);
}

void regview_fmt_param(char *o, size_t n, int a, int v) {
  static const char *CX[3] = { "TRI", "SQR", "SAW" }, *MD[4] = { "SIN", "TRI", "SQR", "SAW" };
  switch (a) {
    case REG_PITCH: regview_fmt_note(o, n, v); break;
    case REG_CUTOFF: case REG_MDCUT: fmt_hz(o, n, cut_hz(v)); break;
    case REG_ATK: fmt_sec(o, n, atk_sec(v)); break;
    case REG_REL: fmt_sec(o, n, rel_sec(v)); break;
    case REG_LFORATE: fmt_hz(o, n, lfo_hz(v)); break;
    case REG_FMRATE: fmt_hz(o, n, fm_hz(v)); break;
    case REG_FMAMT: snprintf(o, n, "%.0fHz", floor(fm_depth_hz(v) + 0.5)); break;
    case REG_NZTONE: fmt_hz(o, n, fmin(nz_hz(v), 20000)); break;
    case REG_CXWAVE: snprintf(o, n, "%s", CX[cx_shape(v)]); break;
    case REG_MDWAVE: snprintf(o, n, "%s", MD[md_shape(v)]); break;
    case REG_DLYTIME: fmt_sec(o, n, dly_sec(v)); break;
    case REG_DLYFB: snprintf(o, n, "%.0f%%", floor(dly_fb(v) * 100 + 0.5)); break;
    case REG_DLYLP: fmt_hz(o, n, dly_lp_hz(v)); break;
    case REG_DLYFRZ: snprintf(o, n, "%s", v >= 16384 ? "FROZEN" : "off"); break;
    default: regview_fmt_pct(o, n, v);
  }
}

/* The one-line help for a register: an exact match, or a numbered family ("CV#" covers CV0..CV3). */
const char *regview_help(const char *name) {
  for (int i = 0; i < HELP_COUNT; i++) if (strcmp(HELP_TABLE[i].name, name) == 0) return HELP_TABLE[i].text;
  static char family[16], text[64];
  int len = (int)strlen(name), d = len;
  while (d > 0 && name[d - 1] >= '0' && name[d - 1] <= '9') d--;
  if (d == len || d + 1 > 14) return "";
  snprintf(family, sizeof family, "%.*s#", d, name);
  for (int i = 0; i < HELP_COUNT; i++) if (strcmp(HELP_TABLE[i].name, family) == 0) {
    const char *src = HELP_TABLE[i].text; size_t k = 0;
    for (; *src && k < sizeof text - 8; src++) { if (*src == '#') k += (size_t)snprintf(text + k, sizeof text - k, "%s", name + d); else text[k++] = *src; }
    text[k] = '\0';
    return text;
  }
  return "";
}

/* ---------------------------------------------------------------- access */

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

int regview_read(FezSynth *s, int a) {
  if (a == REG_LEVEL) return s->level_pot;
  if (s->param_index[a] >= 0 && a != REG_GATE) return s->mem[a];
  return fez_reg_read(s, a);
}

void regview_write(FezSynth *s, int a, int v) {
  if (a == REG_LEVEL) s->level_pot = clampi(v, 0, 32767); else fez_reg_write(s, a, v);
}

int regview_pget(FezSynth *s, int a) { return a == REG_LEVEL ? s->level_pot : s->mem[a]; }

void regview_pset(FezSynth *s, int a, int v) {
  v = clampi(v, 0, 32767);
  if (a == REG_LEVEL) s->level_pot = v; else s->mem[a] = (int16_t)v;
}

/* A register that selects one of a few shapes (CXWAVE: 3, MDWAVE: 4) is a switch, not a knob: the value is cut into equal bands
   (cx_shape and md_shape in scales.c do the cutting). One step moves to the next shape, so a press never lands inside the same band.
   Returns the number of choices, or 0 for an ordinary register. */
static int switch_choices(int a) { return a == REG_CXWAVE ? 3 : a == REG_MDWAVE ? 4 : 0; }

void regview_pstep(FezSynth *s, int a, int dir, int coarse) {
  int n = switch_choices(a);
  if (n) {
    int c = clampi(regview_pget(s, a) * n / 32768, 0, n - 1) + dir;        /* the band the value is in now, then one band along */
    c = clampi(c, 0, n - 1);
    regview_pset(s, a, (2 * c + 1) * 32768 / (2 * n));                      /* the middle of the new band */
    return;
  }
  regview_pset(s, a, regview_pget(s, a) + dir * (a == REG_PITCH ? 256 * (coarse ? 12 : 1) : coarse ? 4096 : 512));
}

/* ---------------------------------------------------------------- source words */

static int streq_nocase(const char *a, const char *b) {
  for (; *a && *b; a++, b++) if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
  return *a == *b;
}

/* "C4", "F#3", "Bb-1": a note name the compiler accepts. */
static int is_note(const char *t) {
  if (!(toupper((unsigned char)t[0]) >= 'A' && toupper((unsigned char)t[0]) <= 'G')) return 0;
  int i = 1;
  if (t[i] == '#' || t[i] == 'b') i++;
  if (t[i] == '-') i++;
  return isdigit((unsigned char)t[i]) && t[i + 1] == '\0';
}

static int is_number(const char *t) {
  int i = 0, digits = 0;
  for (; t[i]; i++) { if (isdigit((unsigned char)t[i])) digits++; else if (t[i] != '.') return 0; }
  return digits > 0;
}

int regview_token_kind(const char *t) {
  for (int i = 0; i < OP_COUNT; i++) if (streq_nocase(t, OP_NAMES[i])) return TOK_OP;
  for (int i = 0; i < NAME_COUNT; i++) if (streq_nocase(t, NAME_TABLE[i].name)) return TOK_REG;
  if (is_note(t)) return TOK_NOTE;
  if (is_number(t)) return TOK_NUM;
  return TOK_OTHER;
}

int regview_parse_note(const char *t, int default_octave, int *pitch) {
  static const int PITCH_CLASS[7] = { 9, 11, 0, 2, 4, 5, 7 };         /* A B C D E F G, in semitones above C */
  int c = toupper((unsigned char)t[0]);
  if (c < 'A' || c > 'G') return 0;
  int pc = PITCH_CLASS[c - 'A'], i = 1;
  if (t[i] == '#') { pc++; i++; }
  else if (t[i] == 'b') { pc--; i++; }                                 /* a lower-case b after the letter is a flat */
  int octave = default_octave;
  if (t[i]) {
    int neg = t[i] == '-', digits = 0, v = 0;
    if (neg) i++;
    for (; isdigit((unsigned char)t[i]) && digits < 2; i++, digits++) v = v * 10 + (t[i] - '0');
    if (!digits || t[i]) return 0;
    octave = neg ? -v : v;
  }
  int midi = (octave + 1) * 12 + pc;
  if (midi < 0 || midi > 127) return 0;
  *pitch = midi * 256;
  return 1;
}
