/* The register file: what happens when a block (or a UI, or MIDI) reads or writes an address.
   Most registers are plain stored numbers with a legal range. A few have side effects (GATE fires the
   envelope, SQCLK clocks the sequencer, CASTEP steps the automaton) and a few are read-only.
   The address map is core/reg_names.h; names and defaults come from the tables in tables.c. */
#include "fez.h"
#include <math.h>

int fez_clamp(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

/* JS Math.round: halves round up (-0.5 gives 0). C's round() rounds halves away from zero, so it differs. */
int fez_round(double x) { return (int)floor(x + 0.5); }

/* Division that rounds toward minus infinity. C's / truncates toward zero, which differs for negatives: -7 / 2 is -3 in C,
   but -4 here, so steps of a negative pitch line up with the positive ones. */
int fez_floor_div(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

/* Wrap any int into the signed 16-bit range: what a 16-bit register would hold after the same bit operations. */
int fez_i16(int x) { x &= 0xffff; return x >= 0x8000 ? x - 0x10000 : x; }

static int in(int a, int lo, int hi) { return a >= lo && a <= hi; }

/* ---------------------------------------------------------------- queues to the audio side */

void fez_push_buf_event(FezSynth *s, int type, int mask) {
  if (s->buf_q_count >= BUF_Q_SIZE) return;                  /* full: drop. The audio side drains every tick, so this is a bug guard */
  BufEvent *e = &s->buf_q[(s->buf_q_head + s->buf_q_count++) % BUF_Q_SIZE];
  e->type = type; e->mask = mask;
}

int fez_pop_buf_event(FezSynth *s, BufEvent *out) {
  if (s->buf_q_count == 0) return 0;
  *out = s->buf_q[s->buf_q_head];
  s->buf_q_head = (s->buf_q_head + 1) % BUF_Q_SIZE;
  s->buf_q_count--;
  return 1;
}

int fez_pop_gate_event(FezSynth *s, GateEvent *out) {
  if (s->gate_q_count == 0) return 0;
  *out = s->gate_q[s->gate_q_head];
  s->gate_q_head = (s->gate_q_head + 1) % GATE_Q_SIZE;
  s->gate_q_count--;
  return 1;
}

/* ---------------------------------------------------------------- reading */

int fez_read(FezSynth *s, int a, int b, unsigned *fault) {
  if (a < 0 || a >= NUM_REGS) { *fault |= FAULT_ADDR; return 0; }
  if (s->param_index[a] >= 0 && a != REG_GATE) return s->mem[a];       /* every parameter (GATE is special) */
  if (a == REG_GATE) return s->env.phase == ENV_IDLE ? 0 : 1;          /* GATE reads 1 while the envelope runs */
  if (in(a, REG_CARULE, REG_CATRIG)) return fez_ca_read(s, a);
  if (in(a, REG_LZRATE, REG_LZSEED)) return fez_lz_read(s, a);
  if (a == REG_BWPOS) { double p = s->bw.pos < 0 ? 0 : s->bw.pos > 1 ? 1 : s->bw.pos; return fez_round(p * 32767); }   /* BWPOS */
  if (a == REG_BWSTAT) return s->bw.state;                               /* BWSTAT */
  if (in(a, REG_BWMODE, REG_BIN) && a != REG_BWTRIG) return s->mem[a];
  if (in(a, REG_GPOS0, REG_GFLG7)) return s->mem[a];
  if (a == REG_GWIN) return s->mem[a];
  if (in(a, REG_BP0, REG_BP15) || a == REG_SQPUL || a == REG_GRPUL || a == REG_TMPUL || a == REG_KBPUL) return s->mem[a];   /* pulser masks */
  if (in(a, REG_KBNOTE, REG_KBOCT)) return s->mem[a];                                                      /* keyboard: note, gate, octave */
  if (in(a, REG_QON, REG_QROOT) || a == REG_TMCHANCE || a == REG_TMLEN) return s->mem[a];                  /* quantiser, and the Turing machine's settings */
  if (a == REG_TMBITS) return (int16_t)(s->tm.bits & 0xffff);                        /* TMBITS reads signed, like CAROW */
  if (a == REG_TMOUT) return fez_tm_out(s);
  if (a == REG_SQSTEP) return fez_clamp(s->sq_cur, 0, 7);
  if (a == REG_SQOUT) return s->mem[REG_SQ0 + fez_clamp(s->sq_cur, 0, 7)];  /* SQOUT: the current step's value */
  if (in(a, REG_SQEN, REG_SQDRV) || in(a, REG_SQ0, REG_SQ7)) return s->mem[a];
  if (in(a, REG_CV0, REG_CV3) || in(a, REG_MP0, REG_MP15) || in(a, REG_SLT0, REG_SLT3) || in(a, REG_SLO0, REG_SLO3) || in(a, REG_S0, REG_S15)) return s->mem[a];
  if (in(a, REG_P0, REG_P15)) return s->priv[b][a - REG_P0];                /* P0..P15 belong to the running block */
  *fault |= FAULT_ADDR;
  return 0;
}

/* ---------------------------------------------------------------- the buffer device's registers */

/* Returns 1 if the address belongs to the buffer device (handled, even if only to flag an error). */
static int buf_write(FezSynth *s, int a, int v, unsigned *fault) {
  if (a == REG_BWMODE) { s->mem[a] = (int16_t)fez_clamp(v, 0, 2); s->bw.state = s->mem[a] == 1 ? 1 : 0; return 1; }   /* BWMODE */
  if (a == REG_BWSRC) { s->mem[a] = (int16_t)fez_clamp(v, 0, 3); return 1; }                  /* BWSRC */
  if (a == REG_BWLEN) { s->mem[a] = (int16_t)fez_clamp(v, 1, 32767); return 1; }              /* BWLEN */
  if (a == REG_BWTRIG) {                                                                       /* BWTRIG: start a one-shot recording */
    if (v != 0) { s->bw.pos = 0; s->bw.rec = 0; s->bw.state = 2; fez_push_buf_event(s, EV_REC, 0); }
    return 1;
  }
  if (a == REG_BUFMS) { s->mem[a] = (int16_t)fez_clamp(v, 50, 30000); return 1; }              /* BUFMS */
  if (a == REG_BWPOS || a == REG_BWSTAT) { *fault |= FAULT_ADDR; return 1; }                         /* read-only */
  if (a == REG_BIN) { s->mem[a] = (int16_t)fez_clamp(v, 0, 32767); return 1; }              /* BIN */
  if (in(a, REG_GPOS1, REG_GPOS7) || in(a, REG_GRATE1, REG_GRATE7) || in(a, REG_GLEN0, REG_GLEN7) || in(a, REG_GLVL1, REG_GLVL7)) { s->mem[a] = (int16_t)fez_clamp(v, 0, 32767); return 1; }
  if (in(a, REG_GFLG0, REG_GFLG7)) { s->mem[a] = (int16_t)fez_clamp(v, 0, 15); return 1; }          /* GFLG: four flag bits */
  if (a == REG_GTRIG) { if (v & 255) fez_push_buf_event(s, EV_TRIG, v & 255); return 1; }     /* GTRIG */
  if (a == REG_GSTOP) { if (v & 255) fez_push_buf_event(s, EV_STOP, v & 255); return 1; }     /* GSTOP */
  if (a == REG_GWIN) { s->mem[a] = (int16_t)fez_clamp(v, 0, 32767); return 1; }              /* GWIN */
  return 0;
}

/* ---------------------------------------------------------------- writing */

void fez_write(FezSynth *s, int a, int v, int b, unsigned *fault) {
  v = fez_clamp(v, -32768, 32767);
  if (a < 0 || a >= NUM_REGS) { *fault |= FAULT_ADDR; return; }
  if (in(a, REG_CARULE, REG_CATRIG)) { fez_ca_write(s, a, v, fault); return; }
  if (in(a, REG_LZRATE, REG_LZSEED)) { fez_lz_write(s, a, v, fault); return; }
  if (buf_write(s, a, v, fault)) return;
  if (in(a, REG_BP0, REG_BP15) || a == REG_SQPUL || a == REG_GRPUL || a == REG_TMPUL || a == REG_KBPUL) { s->mem[a] = (int16_t)fez_clamp(v, 0, 15); return; }   /* pulser masks: 4 bits, one per pulser */
  if (a == REG_KBNOTE || a == REG_KBGATE) { *fault |= FAULT_ADDR; return; }                         /* keyboard state: read only */
  if (a == REG_KBOCT) { s->mem[a] = (int16_t)fez_clamp(v, 0, 8); return; }                        /* KBOCT */
  if (a == REG_QON) { s->mem[a] = (int16_t)fez_clamp(v, 0, 1); return; }                     /* QON */
  if (a == REG_QSCALE) { s->mem[a] = (int16_t)fez_clamp(v, 0, 7); return; }                     /* QSCALE */
  if (a == REG_QROOT || a == REG_TMCHANCE) { s->mem[a] = (int16_t)fez_clamp(v, 0, 32767); return; }     /* QROOT, TMCHANCE */
  if (a == REG_TMLEN) { s->mem[a] = (int16_t)fez_clamp(v, 2, 16); return; }                    /* TMLEN */
  if (a == REG_TMSTEP) { if (v != 0) fez_tm_step(s); return; }                                  /* TMSTEP */
  if (a == REG_TMBITS) { s->tm.bits = v & 0xffff; return; }                                     /* TMBITS */
  if (a == REG_RUN) { if (v >= 0 && v < 16) s->sw_run |= 1u << v; else *fault |= FAULT_ADDR; return; }   /* RUN */
  if (a == REG_SQSTEP) { s->sq_cur = fez_clamp(v, 0, 7); return; }
  if (a == REG_SQCLK) { if (v != 0) fez_seq_clock(s); return; }
  if (a == REG_SQEN) { s->mem[a] = (int16_t)fez_clamp(v, 0, 255); return; }
  if (a == REG_SQLEN) { s->mem[a] = (int16_t)fez_clamp(v, 1, 8); return; }
  if (a == REG_SQDRV) { s->mem[a] = (int16_t)fez_clamp(v, 0, 3); return; }
  if (in(a, REG_SQ0, REG_SQ7)) { s->mem[a] = (int16_t)fez_clamp(v, 0, 32767); return; }
  if (s->param_index[a] >= 0) {
    if (a == REG_LEVEL) return;                                         /* LEVEL is owned by the knob */
    if (a == REG_GATE) { if (v != 0) fez_fire_gate(s); return; }       /* GATE */
    s->mem[a] = (int16_t)fez_clamp(v, 0, 32767);
    return;
  }
  if (in(a, REG_CV0, REG_CV3) || in(a, REG_S0, REG_S15)) { s->mem[a] = (int16_t)v; return; }           /* CV0..CV3 and scratch hold signed values */
  if (in(a, REG_SLT0, REG_SLT3)) { s->mem[a] = (int16_t)fez_clamp(v, 0, 32767); return; }         /* SLT0..SLT3 */
  if (in(a, REG_P0, REG_P15)) { s->priv[b][a - REG_P0] = (int16_t)v; return; }
  *fault |= FAULT_ADDR;                                           /* read-only registers and unmapped addresses */
}

/* ---------------------------------------------------------------- modulation matrix routing (the ROUTE word) */

void fez_set_route(FezSynth *s, int sr, int d, int w, unsigned *fault) {
  /* Translate a source register address into the matrix's own source number: its row in SRC_TABLE (0 if it is not a source). */
  int src = 0;
  for (int i = 1; i < SRC_COUNT; i++) if (SRC_TABLE[i].addr == sr) src = i;
  if (!src || d < 0 || d >= NUM_REGS || s->param_index[d] < 0 || d == REG_GATE) { *fault |= FAULT_ADDR; return; }
  for (int i = 0; i < s->nroutes; i++)
    if (s->routes[i].src == src && s->routes[i].dst == d) { s->routes[i].w = w; return; }   /* same pair: update the weight */
  if (s->nroutes < MAX_ROUTES) { s->routes[s->nroutes].src = src; s->routes[s->nroutes].dst = d; s->routes[s->nroutes].w = w; s->nroutes++; }
  else *fault |= FAULT_ADDR;                                      /* matrix full */
}

/* ---------------------------------------------------------------- public register access (UI, MIDI, tests) */

int fez_reg_read(FezSynth *s, int addr) { unsigned ignore = 0; return fez_read(s, addr, 0, &ignore); }
void fez_set_pot(FezSynth *s, int pot, int value) {
  s->mem[REG_MP0 + fez_clamp(pot, 0, 15)] = (int16_t)fez_clamp(value, 0, 32767);
}

void fez_reg_write(FezSynth *s, int addr, int value) { unsigned ignore = 0; fez_write(s, addr, value, 0, &ignore); }
