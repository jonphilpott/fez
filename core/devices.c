/* Virtual devices on the control side: the cellular automaton, the Lorenz attractor and the step sequencer.
   Each device exposes registers (read and write functions here, called from the register file in regs.c) and can
   fire pulsers by setting bits in vm.pending; the next tick runs whatever is aimed at those pulsers. */
#include "fez.h"
#include "scales.h"
#include <math.h>

/* ---------------------------------------------------------------- cellular automaton
   A row of 16 cells in a ring. Each generation, a cell's new state comes from itself and its two neighbours:
   the three bits (left, me, right) form a number 0..7, and bit n of the RULE register says what that pattern
   becomes. RULE 30 is the classic chaotic one; RULE 90 draws triangles. */

static int popcount16(int x) { int n = 0; x &= 0xffff; while (x) { n += x & 1; x >>= 1; } return n; }

/* Registers hold signed 16-bit values; a 16-bit row with bit 15 set reads back negative. */
static int as_i16(int x) { x &= 0xffff; return x >= 0x8000 ? x - 0x10000 : x; }

int fez_ca_read(const FezSynth *s, int addr) {
  switch (addr) {
    case REG_CARULE: return s->ca.rule;
    case REG_CAROW: return as_i16(s->ca.row);
    case REG_CACOUNT: return popcount16(s->ca.row);                                   /* live cells */
    case REG_CADENS: { int d = popcount16(s->ca.row) * 2048; return d > 32767 ? 32767 : d; }   /* density: a matrix source */
    case REG_CATAP: return s->ca.tap;
    case REG_CABIT: return (s->ca.row >> s->ca.tap) & 1;                            /* state of the watched cell */
    case REG_CATRIG: return s->ca.trig;
    default:  return 0;                                                       /* CASTEP reads as 0 */
  }
}

static void ca_step(FezSynth *s, int generations) {
  for (int g = 0; g < generations; g++) {
    int r = s->ca.row, next = 0;
    for (int i = 0; i < 16; i++) {
      int left = (r >> ((i + 15) % 16)) & 1, mid = (r >> i) & 1, right = (r >> ((i + 1) % 16)) & 1;
      if ((s->ca.rule >> (left * 4 + mid * 2 + right)) & 1) next |= 1 << i;
    }
    int before = (r >> s->ca.tap) & 1, after = (next >> s->ca.tap) & 1;
    if (before == 0 && after == 1 && s->ca.trig < 4)                          /* a "birth" at the watched cell */
      s->vm.pending |= 1u << s->ca.trig;
    s->ca.row = next;
  }
}

void fez_ca_write(FezSynth *s, int addr, int v, unsigned *fault) {
  switch (addr) {
    case REG_CARULE: s->ca.rule = fez_clamp(v, 0, 255); break;
    case REG_CAROW: s->ca.row = v & 0xffff; break;
    case REG_CASTEP: ca_step(s, fez_clamp(v, 0, 16)); break;
    case REG_CATAP: s->ca.tap = fez_clamp(v, 0, 15); break;
    case REG_CATRIG: s->ca.trig = fez_clamp(v, 0, 4); break;
    default:  *fault |= FAULT_ADDR;                                           /* CACOUNT, CADENS, CABIT are read-only */
  }
}

/* ---------------------------------------------------------------- Lorenz attractor
   Three coupled equations that never repeat:  dx = sigma (y - x),  dy = x (rho - z) - y,  dz = x y - beta z.
   We integrate with the midpoint method (RK2): take half a step to see where the slope is heading, then use
   that slope for the whole step. It is cheap, and accurate enough for a modulation source. */

#define LZ_SIGMA 10.0
#define LZ_BETA  (8.0 / 3.0)

static int lz_out(double v, double scale, double offset) {
  int r = fez_round((v - offset) * scale);
  return fez_clamp(r, -32768, 32767);
}

int fez_lz_read(const FezSynth *s, int addr) {
  switch (addr) {
    case REG_LZRATE: return s->lz.rate;
    case REG_LZRHO: return s->lz.rho;
    case REG_LZX: return lz_out(s->lz.x, 1600.0, 0.0);
    case REG_LZY: return lz_out(s->lz.y, 1200.0, 0.0);
    case REG_LZZ: return lz_out(s->lz.z, 1300.0, 25.0);
    case REG_LZTRIG: return s->lz.trig;
    default:  return 0;                                                       /* LZSEED reads as 0 */
  }
}

void fez_lz_write(FezSynth *s, int addr, int v, unsigned *fault) {
  switch (addr) {
    case REG_LZRATE: s->lz.rate = fez_clamp(v, 0, 32767); break;
    case REG_LZRHO: s->lz.rho = fez_clamp(v, 0, 32767); break;
    case REG_LZTRIG: s->lz.trig = fez_clamp(v, 0, 4); break;
    case REG_LZSEED: s->lz.x = 0.1 + (v & 0x7fff) / 3277.0; s->lz.y = 1; s->lz.z = 1; break;   /* restart from a new point */
    default:  *fault |= FAULT_ADDR;                                           /* X, Y, Z are read-only */
  }
}

static double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

void fez_lz_advance(FezSynth *s, double dt_ms) {
  double rho = lz_rho(s->lz.rho);
  double sim_t = (dt_ms / 1000.0) * lz_units_per_sec(s->lz.rate);             /* simulated time this tick */
  int n = (int)ceil(sim_t / 0.005);                                           /* sub-steps of at most 0.005 */
  if (n < 1) n = 1;
  if (n > 400) n = 400;
  double h = sim_t / n;
  double x0 = s->lz.x;
  for (int i = 0; i < n; i++) {
    double x = s->lz.x, y = s->lz.y, z = s->lz.z;
    /* slope at the start (k1), then at the midpoint (k2). Written out one term at a time in the same order as
       the JS, so the two give identical results even though floating point is not associative. */
    double k1x = LZ_SIGMA * (y - x), k1y = x * (rho - z) - y, k1z = x * y - LZ_BETA * z;
    double mx = x + 0.5 * h * k1x, my = y + 0.5 * h * k1y, mz = z + 0.5 * h * k1z;
    double k2x = LZ_SIGMA * (my - mx), k2y = mx * (rho - mz) - my, k2z = mx * my - LZ_BETA * mz;
    s->lz.x = clampd(x + h * k2x, -100, 100);
    s->lz.y = clampd(y + h * k2y, -100, 100);
    s->lz.z = clampd(z + h * k2z, -100, 200);
  }
  /* X crossing zero fires the chosen pulser */
  if (s->lz.trig < 4 && ((x0 < 0) != (s->lz.x < 0)) && x0 != 0) s->vm.pending |= 1u << s->lz.trig;
}

/* ---------------------------------------------------------------- quantiser
   Snaps a pitch to the nearest allowed note of a scale. A scale is a 12-bit mask: bit k set means "k semitones above
   the root is allowed". We walk down from the pitch to the nearest allowed semitone and up to the nearest one above;
   the closer wins and a tie goes down. Integers only, so the JS and C agree exactly. */

static const int QT_SCALES[8] = { 0xFFF, 0xAB5, 0x5AD, 0x6AD, 0x295, 0x4A9, 0x4E9, 0x555 };
   /* chromatic, major, natural minor, dorian, major pentatonic, minor pentatonic, blues, whole tone */

int fez_scale_mask(int scale) { return QT_SCALES[fez_clamp(scale, 0, 7)]; }
const char *fez_scale_name(int scale) {
  static const char *const NAMES[8] = { "chromatic", "major", "minor", "dorian", "pent major", "pent minor", "blues", "whole tone" };
  return NAMES[fez_clamp(scale, 0, 7)];
}

/* Floor division for negative numbers too (C's / truncates toward zero). */
static int floor_div(int a, int b) { int q = a / b; return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q; }

int fez_quantise(int p, int scale, int root) {
  int mask = QT_SCALES[fez_clamp(scale, 0, 7)];
  int root_pc = floor_div(root + 128, 256) % 12;
  int lo = floor_div(p, 256), hi = -floor_div(-p, 256);                     /* hi = ceil(p / 256) */
  for (int k = 0; k < 12 && !((mask >> (((lo - root_pc) % 12 + 12) % 12)) & 1); k++) lo--;
  for (int k = 0; k < 12 && !((mask >> (((hi - root_pc) % 12 + 12) % 12)) & 1); k++) hi++;
  int m = (p - lo * 256 <= hi * 256 - p) ? lo : hi;
  return fez_clamp(m * 256, 0, 127 * 256);
}

/* ---------------------------------------------------------------- Turing machine
   A ring of TMLEN bits. A step takes the oldest bit, flips it with probability TMCHANCE / 32768, and pushes it in as
   the newest. Chance 0 repeats the pattern forever; 16384 makes it random. One random number is used per step, always,
   so the JS and C stay in step. */

void fez_tm_step(FezSynth *s) {
  int len = fez_clamp(s->mem[REG_TMLEN], 2, 16), mask = (1 << len) - 1, bits = s->tm.bits & mask;
  int nb = (bits >> (len - 1)) & 1;
  double r = s->vm.host.rnd(s->vm.host.ctx);
  if (r * 32768 < s->mem[REG_TMCHANCE]) nb ^= 1;
  s->tm.bits = ((bits << 1) | nb) & mask;
}

/* The output: the newest 8 bits (all of them if the loop is shorter), scaled to 0..32767. */
int fez_tm_out(const FezSynth *s) {
  int len = fez_clamp(s->mem[REG_TMLEN], 2, 16), m = len < 8 ? len : 8, top = (1 << m) - 1;
  return fez_round(((double)(s->tm.bits & top) * 32767) / top);
}

/* ---------------------------------------------------------------- step sequencer
   Eight steps, each holding a pitch. A clock moves to the next enabled step inside the loop length, then (by the
   SQDRV bits) writes that pitch to PITCH and/or fires the envelope. */
void fez_seq_clock(FezSynth *s) {
  int len = fez_clamp(s->mem[REG_SQLEN], 1, 8), en = s->mem[REG_SQEN] & 255;
  int n = s->sq_cur, found = 0;
  for (int k = 0; k < len; k++) {
    n = (n + 1 >= len) ? 0 : n + 1;
    if ((en >> n) & 1) { found = 1; break; }
  }
  if (!found) return;                                                         /* every step disabled: stay put */
  s->sq_cur = n; s->sq_count++;
  int drv = s->mem[REG_SQDRV];
  if (drv & 1) s->mem[REG_PITCH] = (int16_t)fez_clamp(s->mem[REG_SQ0 + n], 0, 32767);   /* bit 0: write the step's pitch */
  if (drv & 2) fez_fire_gate(s);                                              /* bit 1: fire the envelope */
}
