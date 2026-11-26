/* The FEZ token VM: compiler and interpreter. See vm.h for the big picture.
   It began as a line-by-line port of a JavaScript prototype (retired), so some comments mention "the JS". */
#include "vm.h"
#include "tables.h"
#include "fez.h"      /* fez_scale_mask: SCALE uses the quantiser's scale table */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const OP_NAMES[OP_COUNT] = {
  "DUP", "DROP", "SWAP", "OVER", "ROT", "+", "-", "*", "F*", "/", "*/", "NEG", "ABS", "MIN",
  "MAX", "MOD", "AND", "OR", "XOR", "NOT", "SHL", "SHR", "ROL", "ROR", "=", "<", ">", "U<", "@", "!",
  "L", "JMP", "JZ", "JNZ", "SKIP", "EXIT", "YIELD", "TRIG", "ROUTE", "W", "RND", "ST", "HZ",
  "COLOUR", "?", "SCALE"
};

/* ---------------------------------------------------------------- small integer helpers */

/* Clamp to the 16-bit signed range. Registers and the stack saturate instead of wrapping. */
static int sat(int r) { return r > 32767 ? 32767 : r < -32768 ? -32768 : r; }

/* Wrap any int into the signed 16-bit range (what a 16-bit register would hold). */
static int i16(int x) { x &= 0xffff; return x >= 0x8000 ? x - 0x10000 : x; }

/* Division that rounds toward minus infinity. C's / truncates toward zero, which differs for negatives. */
static int floor_div(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

/* Modulo with the result in 0..n-1 for positive n (C's % keeps the sign of the dividend). */
static int floor_mod(int a, int n) { return a - floor_div(a, n) * n; }

/* Arithmetic shift right by 15, written as a division so it does not depend on how the compiler shifts
   negative numbers. This is the "* ": a Q15 multiply keeps 15 fraction bits. */
static int shr15(int x) { return floor_div(x, 32768); }

/* ---------------------------------------------------------------- the HZ word */

/* HZ converts tenths of a hertz to an 8.8-semitone pitch, with integer maths only, so every platform agrees.
   Idea: pitch = A4 + 12 * log2(f / 440 Hz), in 8.8 semitones. log2 of an integer = position of its top bit
   (the whole part) + log2 of the remaining mantissa 1.xxx (the fraction, from a 257-entry table with linear
   interpolation). Everything is in Q16 fixed point (65536 = 1.0). */
int hz_to_pitch(int t) {
  if (t < 1) return 0;
  int e = 0;                                          /* 1) top bit position: floor(log2(t)) */
  while ((t >> (e + 1)) > 0) e++;
  int m = e >= 16 ? t >> (e - 16) : t << (16 - e);    /* 2) mantissa scaled to 65536..131071 */
  int i = (m - 65536) >> 8, f = (m - 65536) & 255;    /* 3) table index and interpolation fraction */
  int lg = e * 65536 + HZ_TAB[i] + (((HZ_TAB[i + 1] - HZ_TAB[i]) * f) >> 8);
  /* 4) pitch = 69*256 + 3072 * (lg - log2(4400)); the product needs 64 bits, and we round to nearest */
  long long x = 3072LL * (lg - HZ_K) + 32768;
  long long q = x / 65536;
  if (x % 65536 != 0 && x < 0) q--;                   /* floor, not truncate */
  long long p = 17664 + q;
  return p < 0 ? 0 : p > 32767 ? 32767 : (int)p;
}

/* ---------------------------------------------------------------- compiler */

/* Value of a run of digits in the given base, or -1 if the token has other characters. Big values are held at
   1,000,000 so a long digit string cannot overflow; anything above 32767 is rejected as out of range anyway. */
static long digits(const char *p, int base) {
  long n = 0;
  if (*p == '\0') return -1;
  for (; *p; p++) {
    int d;
    if (*p >= '0' && *p <= '9') d = *p - '0';
    else if (base == 16 && toupper((unsigned char)*p) >= 'A' && toupper((unsigned char)*p) <= 'F') d = toupper((unsigned char)*p) - 'A' + 10;
    else return -1;
    if (d >= base) return -1;
    n = n * base + d;
    if (n > 1000000) n = 1000000;
  }
  return n;
}

static void put_lit(Slot *s, int v) { s->is_op = 0; s->value = (int16_t)v; }
static void put_op(Slot *s, int op) { s->is_op = 1; s->value = (int16_t)op; }

/* Turn one token into one slot. Returns 1, or -1 with a message in err.
   The checks run in this order, as in the JS: operations, a minus sign (an error), decimal, hex, fraction, note name,
   register name. Each token gives exactly one slot. */
static int parse_token(const char *t, Slot out[2], char *err, size_t errsz) {
  char u[64];                                         /* upper-case copy for case-insensitive matching */
  size_t len = strlen(t);
  if (len >= sizeof u) { snprintf(err, errsz, "unknown token: %.60s", t); return -1; }
  for (size_t k = 0; k <= len; k++) u[k] = (char)toupper((unsigned char)t[k]);

  /* 1) operations */
  for (int op = 0; op < OP_COUNT; op++)
    if (strcmp(u, OP_NAMES[op]) == 0) { put_op(&out[0], op); return 1; }

  /* 2) a minus sign is not part of a number. A "-5" that quietly became two slots (5, then NEG) would hide one of them from
     anyone counting slots or using SKIP, so every token is exactly one number or one operation and the sign is a word of its
     own: write 5 NEG. (A bare "-" is the subtract word and was matched above.) */
  if (t[0] == '-' && (digits(t + 1, 10) >= 0 || (t[1] == '.' && digits(t + 2, 10) >= 0))) {
    snprintf(err, errsz, "no negative numbers: write %.50s NEG", t + 1);
    return -1;
  }
  /* 3) decimal integer. Literals are 15 bit (0 to 32767). */
  {
    long n = digits(t, 10);
    if (n >= 0) {
      if (n > 32767) { snprintf(err, errsz, "literal out of range: %.60s", t); return -1; }
      put_lit(&out[0], (int)n);
      return 1;
    }
  }
  /* 4) hex: 0x followed by hex digits (case-insensitive) */
  if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
    long n = digits(t + 2, 16);
    if (n >= 0) {
      if (n > 32767) { snprintf(err, errsz, "literal out of range: %.60s", t); return -1; }
      put_lit(&out[0], (int)n);
      return 1;
    }
  }
  /* 5) fraction of full scale: .5 is 16384 (value * 32768, clamped to 0..32767) */
  if (t[0] == '.' && digits(t + 1, 10) >= 0) {
    double r = floor(strtod(t, NULL) * 32768.0 + 0.5);          /* round half up, as JS Math.round does */
    put_lit(&out[0], r < 0 ? 0 : r > 32767 ? 32767 : (int)r);
    return 1;
  }
  /* 6) note name: letter A-G, optional # or b, octave digit (C4 = 15360). Octave -1 is written C-1.
     The JS pattern accepts a capital B as the accidental too, but only # and lower-case b change the pitch. */
  {
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* A B C D E F G */
    const char *p = t;
    if (toupper((unsigned char)p[0]) >= 'A' && toupper((unsigned char)p[0]) <= 'G') {
      int semis = base[toupper((unsigned char)p[0]) - 'A'];
      p++;
      if (*p == '#' || *p == 'b' || *p == 'B') { if (*p == '#') semis++; else if (*p == 'b') semis--; p++; }
      int oct_neg = 0;
      if (*p == '-') { oct_neg = 1; p++; }
      if (isdigit((unsigned char)p[0]) && p[1] == '\0') {
        int oct = (oct_neg ? -1 : 1) * (p[0] - '0');
        int v = (semis + 12 * (oct + 1)) * 256;
        if (v < 0 || v > 32767) { snprintf(err, errsz, "note out of range: %.60s", t); return -1; }
        put_lit(&out[0], v);
        return 1;
      }
    }
  }
  /* 7) register or other name from the name table */
  for (int k = 0; k < NAME_COUNT; k++)
    if (strcmp(u, NAME_TABLE[k].name) == 0) { put_lit(&out[0], NAME_TABLE[k].addr); return 1; }

  snprintf(err, errsz, "unknown token: %.60s", t);
  return -1;
}

/* Compile source text. Everything from a backslash to the end of the line is a comment. On error, c->ok is 0
   and c->err says why. A program longer than BLOCK_SLOTS is always an error (the JS only checks `max_slots`,
   which is never above BLOCK_SLOTS for a block). */
void vm_compile(Compiled *c, const char *src, int max_slots) {
  memset(c, 0, sizeof *c);
  c->err_pos = -1;
  for (int i = 0; i < 16; i++) c->labels[i] = -1;
  char err[128] = "";
  int line = 1, total = 0;                            /* total counts every slot, even past the array */
  int over_pos = -1;                                  /* where the token that took the program past max_slots starts */
  const char *p = src;
  while (*p && !err[0]) {
    if (*p == '\n') { line++; p++; continue; }
    if (*p == '\\') { while (*p && *p != '\n') p++; continue; }       /* comment */
    if (isspace((unsigned char)*p)) { p++; continue; }
    int tstart = (int)(p - src);                                      /* where this token starts, kept for error highlighting */
    char tok[128]; size_t n = 0;                                      /* collect one token */
    while (*p && !isspace((unsigned char)*p) && *p != '\\') { if (n < sizeof tok - 1) tok[n++] = *p; p++; }
    tok[n] = '\0';
    Slot s[2];
    char msg[96];
    int k = parse_token(tok, s, msg, sizeof msg);
    if (k < 0) { snprintf(err, sizeof err, "line %d: %s", line, msg); c->err_pos = tstart; break; }
    for (int j = 0; j < k; j++) {
      if (total < BLOCK_SLOTS) { c->slots[total] = s[j]; c->pos[total] = (uint16_t)tstart; }
      if (total == max_slots && over_pos < 0) over_pos = tstart;
      total++;
    }
  }
  c->count = total;
  if (!err[0] && total <= BLOCK_SLOTS) {
    for (int i = 0; i < total; i++) {                 /* find labels: "n L" marks the slot after the L */
      if (!c->slots[i].is_op || c->slots[i].value != OP_LABEL) continue;
      if (i == 0 || c->slots[i - 1].is_op) { snprintf(err, sizeof err, "L needs a number before it"); c->err_pos = c->pos[i]; break; }
      int n = c->slots[i - 1].value;
      if (n > 15) { snprintf(err, sizeof err, "label must be 0 to 15"); c->err_pos = c->pos[i - 1]; break; }
      if (c->labels[n] >= 0) { snprintf(err, sizeof err, "duplicate label %d", n); c->err_pos = c->pos[i - 1]; break; }
      c->labels[n] = (int16_t)(i + 1);
    }
  }
  if (!err[0] && total > max_slots) { snprintf(err, sizeof err, "too long: %d of %d slots", total, max_slots); c->err_pos = over_pos; }
  strcpy(c->err, err);
  c->ok = err[0] == '\0';
}

/* ---------------------------------------------------------------- interpreter */

enum { EXEC_END = 0, EXEC_EXIT = 1, EXEC_YIELD = 2, EXEC_HALT = -1 };

/* Stop the run, recording why. Used inside exec() only. */
#define HALT(bit) do { *vm->fault |= (bit); NOTE_FAULT(1); return EXEC_HALT; } while (0)
/* Remember which slot of the block's own code caused a fault, for an editor to highlight. Inside a word (depth > 0) the
   caller notes its own W instead, so the slot always points into the block. `force` is for a halt, which replaces an
   earlier note left by a fault that did not stop the block. Uses `here` and `depth` from exec(). */
#define NOTE_FAULT(force) do { if (depth == 0 && vm->cur && (force || vm->cur->fault_slot < 0)) vm->cur->fault_slot = here; } while (0)
/* Pop into a variable, or halt on underflow. */
#define POP(var) do { if (vm->sp == 0) HALT(FAULT_UNDER); (var) = vm->stack[--vm->sp]; } while (0)
/* Push a value, or halt on overflow. */
#define PUSH(val) do { if (vm->sp >= STACK_DEPTH) HALT(FAULT_OVER); vm->stack[vm->sp++] = (val); } while (0)

/* Run one compiled program from slot `ip`. Words run through the same function one level deeper, sharing the
   stack and the step counter. Returns EXEC_END, EXEC_EXIT, EXEC_YIELD or EXEC_HALT. */
static int exec(Vm *vm, const Compiled *c, int ip, int b, int depth) {
  while (ip < c->count) {
    int here = ip;                                    /* the slot being run, for NOTE_FAULT */
    unsigned f0 = *vm->fault;                         /* to tell whether this instruction added a fault */
    if (++vm->steps > STEP_CAP) HALT(FAULT_CAP);
    Slot s = c->slots[ip++];
    if (!s.is_op) { PUSH(s.value); continue; }
    int a, x, d;                                      /* a = top of stack, x = next, d = third */
    switch (s.value) {
      case OP_DUP:  POP(a); PUSH(a); PUSH(a); break;
      case OP_DROP: POP(a); break;
      case OP_SWAP: POP(a); POP(x); PUSH(a); PUSH(x); break;
      case OP_OVER: POP(a); POP(x); PUSH(x); PUSH(a); PUSH(x); break;
      case OP_SELECT: POP(a); POP(x); POP(d); PUSH(a == 0 ? d : x); break;   /* ( d x flag -- d if 0, else x ) */
      case OP_ROT:  POP(a); POP(x); POP(d); PUSH(x); PUSH(a); PUSH(d); break;
      case OP_ADD:  POP(a); POP(x); PUSH(sat(x + a)); break;
      case OP_SUB:  POP(a); POP(x); PUSH(sat(x - a)); break;
      case OP_MUL:  POP(a); POP(x); PUSH(sat(x * a)); break;                 /* whole numbers, sticking at the ends like + and - */
      case OP_FMUL: POP(a); POP(x); PUSH(sat(shr15(x * a))); break;          /* fractions: 32768 is 1.0 (what the sound wants) */
      case OP_DIV:  POP(a); POP(x); if (a == 0) { *vm->fault |= FAULT_DIV0; PUSH(0); } else PUSH(sat(x / a)); break;
      case OP_MULDIV: POP(d); POP(a); POP(x);
        if (d == 0) { *vm->fault |= FAULT_DIV0; PUSH(0); } else PUSH(sat((x * a) / d)); break;
      case OP_MOD:  POP(a); POP(x);
        if (a == 0) { *vm->fault |= FAULT_DIV0; PUSH(0); } else PUSH(a > 0 ? floor_mod(x, a) : x % a); break;
      case OP_NEG:  POP(a); PUSH(sat(-a)); break;
      case OP_ABS:  POP(a); PUSH(sat(a < 0 ? -a : a)); break;
      case OP_MIN:  POP(a); POP(x); PUSH(a < x ? a : x); break;
      case OP_MAX:  POP(a); POP(x); PUSH(a > x ? a : x); break;
      case OP_AND:  POP(a); POP(x); PUSH(i16(x & a)); break;
      case OP_OR:   POP(a); POP(x); PUSH(i16(x | a)); break;
      case OP_XOR:  POP(a); POP(x); PUSH(i16(x ^ a)); break;
      case OP_NOT:  POP(a); PUSH(i16(~a)); break;
      case OP_SHL:  POP(a); POP(x); PUSH(sat(x * (1 << (a & 15)))); break;
      case OP_SHR:  POP(a); POP(x); PUSH(i16((x & 0xffff) >> (a & 15))); break;   /* logical shift on 16 bits */
      /* Rotate: the bits that fall off one end come back in at the other. Done on the unsigned 16 bits; n is taken mod 16,
         and n = 0 is handled apart because shifting a 16-bit value by 16 would be wrong. */
      case OP_ROL:  { POP(a); POP(x); unsigned u = (unsigned)x & 0xffff, n = (unsigned)a & 15; PUSH(i16(n ? (u << n) | (u >> (16 - n)) : u)); break; }
      case OP_ROR:  { POP(a); POP(x); unsigned u = (unsigned)x & 0xffff, n = (unsigned)a & 15; PUSH(i16(n ? (u >> n) | (u << (16 - n)) : u)); break; }
      case OP_EQ:   POP(a); POP(x); PUSH(x == a); break;
      case OP_LT:   POP(a); POP(x); PUSH(x < a); break;
      case OP_GT:   POP(a); POP(x); PUSH(x > a); break;
      case OP_ULT:  POP(a); POP(x); PUSH((x & 0xffff) < (a & 0xffff)); break;
      case OP_FETCH: POP(a); PUSH(vm->host.read(vm->host.ctx, a, b, vm->fault)); break;
      case OP_STORE: POP(a); POP(x); vm->host.write(vm->host.ctx, a, x, b, vm->fault); break;
      case OP_LABEL: POP(a); break;                   /* a label marker only consumes its number */
      case OP_JMP:  POP(a); if (a < 0 || a > 15 || c->labels[a] < 0) HALT(FAULT_LABEL); ip = c->labels[a]; break;
      case OP_JZ:   POP(a); POP(x);
        if (x == 0) { if (a < 0 || a > 15 || c->labels[a] < 0) HALT(FAULT_LABEL); ip = c->labels[a]; } break;
      case OP_JNZ:  POP(a); POP(x);
        if (x != 0) { if (a < 0 || a > 15 || c->labels[a] < 0) HALT(FAULT_LABEL); ip = c->labels[a]; } break;
      case OP_SKIP: POP(a); if (a == 0) ip++; break;
      case OP_EXIT: return EXEC_EXIT;
      case OP_YIELD: if (depth > 0) HALT(FAULT_CTX); vm->yield_ip = ip; return EXEC_YIELD;
      case OP_TRIG: POP(a); if (a >= 0 && a < 4) vm->pending |= 1u << a; else *vm->fault |= FAULT_ADDR; break;
      case OP_ROUTE: POP(a); POP(x); POP(d); vm->host.route(vm->host.ctx, d, x, a, vm->fault); break;
      case OP_ST:   POP(a); PUSH(sat(a * 256)); break;
      case OP_HZ:   POP(a); PUSH(hz_to_pitch(a)); break;
      case OP_COLOUR: {                               /* ( index colour -- pitch above the root, 8.8 semitones ) */
        POP(a); POP(x);
        int col = floor_mod(a, 16), n = floor_mod(x, 5), oct = floor_div(x, 5);
        int j = n == 0 ? 0 : n == 1 ? (col >> 2) + 1 : n == 2 ? 5 : n == 3 ? 7 : (col & 3) + 8;
        PUSH(sat(oct * 3072 + JUST88[j]));
        break;
      }
      case OP_SCALE: {                                /* ( index scale -- pitch above the root, 8.8 semitones ) */
        /* The scale number is the quantiser's SCALE (0 to 7). Its mask has one bit per allowed semitone, so
           "degree k" means "the k-th set bit". The degrees repeat every octave: index n walks past the last
           set bit into the next octave (3072 = 12 semitones in 8.8). */
        POP(a); POP(x);
        int mask = fez_scale_mask(floor_mod(a, 8)), n = 0;        /* wrap 0..7 like COLOUR wraps its col */
        for (int b2 = 0; b2 < 12; b2++) n += (mask >> b2) & 1;      /* notes per octave (never 0: every mask has bits) */
        int oct = floor_div(x, n), k = floor_mod(x, n), semi = 0;
        for (int b2 = 0; b2 < 12; b2++) if ((mask >> b2) & 1) { if (k-- == 0) { semi = b2; break; } }
        PUSH(sat(oct * 3072 + semi * 256));
        break;
      }
      case OP_RND:  POP(a); PUSH(a > 0 ? (int)floor(vm->host.rnd(vm->host.ctx) * a) : 0); break;
      case OP_WORD: {
        POP(a);
        if (depth > 0) HALT(FAULT_CTX);
        if (a < 0 || a >= NUM_WORDS || !vm->words[a].ok || vm->words[a].count == 0) { *vm->fault |= FAULT_WORD; break; }
        int r = exec(vm, &vm->words[a], 0, b, depth + 1);
        if (*vm->fault != f0) NOTE_FAULT(r == EXEC_HALT);      /* the fault was in the word: point at the W that called it */
        if (r == EXEC_HALT) return EXEC_HALT;
        break;
      }
      default: break;
    }
    if (*vm->fault != f0) NOTE_FAULT(0);              /* a fault that did not stop the block (divide by zero, bad address) */
  }
  return EXEC_END;
}

void vm_init(Vm *vm, VmHost host) {
  memset(vm, 0, sizeof *vm);
  vm->host = host;
  for (int i = 0; i < NUM_WORDS; i++) vm_compile(&vm->words[i], "", WORD_SLOTS);
  for (int i = 0; i < NUM_BLOCKS; i++) vm_set_block(vm, i, "");
}

void vm_set_block(Vm *vm, int b, const char *src) {
  Block *blk = &vm->blocks[b];
  strncpy(blk->src, src, BLOCK_SRC_MAX - 1);
  blk->src[BLOCK_SRC_MAX - 1] = '\0';
  vm_compile(&blk->c, blk->src, BLOCK_SLOTS);      /* compile what was kept, so the code that runs is the code a save writes */
  blk->halted = 0; blk->resume = -1; blk->fault = 0; blk->fault_slot = -1; blk->peak = 0;
}

const char *vm_fault_name(unsigned fault) {
  static const struct { unsigned bit; const char *name; } names[] = {
    { FAULT_UNDER, "stack underflow" }, { FAULT_OVER, "stack overflow" }, { FAULT_DIV0, "divide by zero" },
    { FAULT_ADDR, "bad address" }, { FAULT_LABEL, "bad label" }, { FAULT_CAP, "step cap" },
    { FAULT_WORD, "empty word" }, { FAULT_CTX, "not allowed in a word" } };
  for (unsigned i = 0; i < sizeof names / sizeof *names; i++) if (fault & names[i].bit) return names[i].name;
  return "";
}

void vm_set_word(Vm *vm, int w, const char *src) {
  strncpy(vm->word_src[w], src, WORD_SRC_MAX - 1);
  vm->word_src[w][WORD_SRC_MAX - 1] = '\0';
  vm_compile(&vm->words[w], vm->word_src[w], WORD_SLOTS);  /* compile what was kept (see vm_set_block) */
}

/* Run block b once. A halted or empty block does nothing. After YIELD the next run continues where it stopped. */
void vm_run_block(Vm *vm, int b) {
  Block *blk = &vm->blocks[b];
  if (blk->halted || !blk->c.ok || blk->c.count == 0) return;
  vm->fault = &blk->fault;
  vm->cur = blk;
  vm->steps = 0; vm->sp = 0;
  int ip = 0;
  if (blk->resume >= 0) { ip = blk->resume; blk->resume = -1; }
  int r = exec(vm, &blk->c, ip, b, 0);
  if (r == EXEC_YIELD) blk->resume = vm->yield_ip;
  else if (r == EXEC_HALT) { blk->halted = 1; blk->resume = -1; }
  blk->steps = vm->steps;
  if (vm->steps > blk->peak) blk->peak = vm->steps;
  blk->runs++;
}
