/* The control side of the synth: loading a patch, the clock (pulsers and ticks), the modulation matrix and the
   slow modulators (LFO, slew generators, control envelope). (Ported from the JavaScript prototype.) */
#include "fez.h"
#include "scales.h"
#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------- VM host callbacks
   The VM calls these to touch the synth. `ctx` is the FezSynth that owns the Vm. (So a FezSynth must not be
   copied after fez_init: the pointer inside would still point at the original.) */

static int host_read(void *ctx, int addr, int block, unsigned *fault) { return fez_read(ctx, addr, block, fault); }
static void host_write(void *ctx, int addr, int v, int block, unsigned *fault) { fez_write(ctx, addr, v, block, fault); }
static void host_route(void *ctx, int src, int dst, int w, unsigned *fault) { fez_set_route(ctx, src, dst, w, fault); }

/* xorshift32: three shifts and xors give a decent random stream from one 32-bit number. Small, fast, and the same
   on every platform, which is what the tests need. (Not for cryptography.) */
static double host_rnd(void *ctx) {
  FezSynth *s = ctx;
  s->rng ^= s->rng << 13; s->rng ^= s->rng >> 17; s->rng ^= s->rng << 5;
  return s->rng / 4294967296.0;
}

/* ---------------------------------------------------------------- loading */

void fez_init(FezSynth *s, uint32_t seed) {
  memset(s, 0, sizeof *s);
  s->rng = seed ? seed : 1;                                   /* xorshift must never hold 0 */
  memset(s->param_index, -1, sizeof s->param_index);
  for (int i = 0; i < PARAM_COUNT; i++) s->param_index[PARAM_TABLE[i].addr] = (int8_t)i;
  VmHost host = { s, host_read, host_write, host_route, host_rnd };
  vm_init(&s->vm, host);
  audio_init(&s->audio);
  fez_load_patch(s, 0);
}

/* Registers that every patch starts with (the buffer device and the sequencer), before the patch's own presets.
   Public because a saved patch lists only the registers that differ from these. `mem` must be all zero on entry. */
void fez_default_registers(int16_t mem[NUM_REGS]) {
  static const int seq_default[8] = { 15360, 15872, 16384, 17152, 17664, 17152, 16384, 15872 };   /* C4 D4 E4 G4 A4 G4 E4 D4 */
  for (int i = 0; i < PARAM_COUNT; i++) mem[PARAM_TABLE[i].addr] = (int16_t)PARAM_TABLE[i].def;
  mem[REG_QSCALE] = 1; mem[REG_QROOT] = 15360;                             /* QSCALE major, QROOT C4 */
  mem[REG_TMCHANCE] = 4096; mem[REG_TMLEN] = 16;                             /* TMCHANCE about 12 percent, TMLEN 16 */
  mem[REG_KBOCT] = 3;                                                        /* the keyboard's bottom key is C3 */
  mem[REG_SQEN] = 255; mem[REG_SQLEN] = 8; mem[REG_SQDRV] = 3;                 /* SQEN all on, SQLEN 8, SQDRV pitch and gate */
  for (int i = 0; i < 8; i++) mem[REG_SQ0 + i] = (int16_t)seq_default[i];
  mem[REG_BWMODE] = 1; mem[REG_BWSRC] = 0; mem[REG_BWLEN] = 32767; mem[REG_BUFMS] = 2000; mem[REG_BIN] = 32767; mem[REG_GWIN] = 16000;
  for (int i = 1; i < 8; i++) mem[REG_GRATE0 + i] = 15360;           /* every grain voice starts at unity speed */
}

/* Everything a patch load resets, whatever patch comes next: registers to their defaults (the pots excepted), every block
   and word empty, every moving part at rest, the queues empty, and the buffer and effects told to clear. The two loaders
   below start here and then add their own content. */
static void reset_machine(FezSynth *s) {
  /* 1) registers. The pots are the controls themselves (knobs, sticks, MIDI), not part of a patch, so they stay put. */
  int16_t pots[16];
  memcpy(pots, s->mem + REG_MP0, sizeof pots);
  memset(s->mem, 0, sizeof s->mem);
  memset(s->priv, 0, sizeof s->priv);
  fez_default_registers(s->mem);
  memcpy(s->mem + REG_MP0, pots, sizeof pots);
  memset(s->eff, 0, sizeof s->eff);

  /* 2) code: every block and word empty */
  for (int i = 0; i < NUM_BLOCKS; i++) { vm_set_block(&s->vm, i, ""); s->vm.blocks[i].runs = 0; s->vm.blocks[i].steps = 0; }
  for (int i = 0; i < NUM_WORDS; i++) vm_set_word(&s->vm, i, "");
  s->vm.pending = 0;
  s->sw_run = 0;

  /* 3) pulsers off, at a usual rate; the matrix empty */
  for (int i = 0; i < NUM_PULSERS; i++) { s->pulsers[i].on = 0; s->pulsers[i].rate = 120; s->pulsers[i].acc = 0; s->pulsers[i].last_fire = 0; }
  s->nroutes = 0;

  /* 4) everything that moves, at rest */
  memset(s->slew, 0, sizeof s->slew);
  s->tm.bits = 0x5ac3;
  s->kb_n = 0; s->gate_hold = 0;                              /* a new patch lets go of every key */
  s->sq_cur = 7; s->sq_count = 0;                             /* so the first clock lands on step 0 */
  s->bw.pos = 0; s->bw.rec = 0;
  s->grn_next = 0;
  s->env.v = 0; s->env.phase = ENV_IDLE; s->env.atk = 0.01; s->env.rel = 0.2;
  s->lfo_phase = 0; s->lfo_val = 0;
  s->lz.x = 1; s->lz.y = 1; s->lz.z = 1;
  s->tick_count = 0;

  /* 5) queues: a new patch starts with an empty buffer, so the last patch's audio cannot play back */
  s->buf_q_head = s->buf_q_count = 0; s->gate_q_head = s->gate_q_count = 0;
  fez_push_buf_event(s, EV_CLEAR, 0);
  s->fx_clear = 1;                            /* and the effects start cold too: no tail from the last patch */
}

void fez_load_patch(FezSynth *s, int index) {
  const PatchDef *p = &PATCH_TABLE[fez_clamp(index, 0, PATCH_COUNT - 1)];
  reset_machine(s);

  /* 1) the patch's register presets (raw writes, no range checks: the table is written by hand and trusted) */
  for (int i = 0; i < PATCH_MAX_PRESETS && p->presets[i].addr != 0; i++) s->mem[p->presets[i].addr] = (int16_t)p->presets[i].value;

  /* 2) code */
  for (int i = 0; i < NUM_BLOCKS; i++) vm_set_block(&s->vm, i, p->blocks[i]);
  for (int i = 0; i < NUM_WORDS; i++) vm_set_word(&s->vm, i, p->words[i]);

  /* 3) pulsers. The patch says which block each pulser runs; the registers store it the other way round,
     as a 4-bit mask of pulsers per block (BP0..BP15), plus one mask each for the sequencer (SQPUL) and the grains (GRPUL). */
  for (int i = 0; i < NUM_PULSERS; i++) {
    s->pulsers[i].on = p->pulsers[i].on; s->pulsers[i].rate = p->pulsers[i].rate;
    int blk = p->pulsers[i].block;
    if (blk < 16) s->mem[REG_BP0 + blk] |= (int16_t)(1 << i);
    else if (blk == 16) s->mem[REG_SQPUL] |= (int16_t)(1 << i);
    else if (blk == 17) s->mem[REG_GRPUL] |= (int16_t)(1 << i);
  }

  /* 4) matrix, knobs, devices */
  s->nroutes = 0;
  for (int i = 0; i < p->nroutes; i++) {                       /* sources are named in the table; the live matrix holds their numbers */
    int src = fez_source_index(p->routes[i].src);
    if (src < 1) continue;                                      /* a misspelt name: test_core catches it, so this never happens */
    s->routes[s->nroutes].src = src; s->routes[s->nroutes].dst = p->routes[i].dst; s->routes[s->nroutes].w = p->routes[i].w; s->nroutes++;
  }
  s->level_pot = p->level; s->cutoff_add = p->cutoff;
  s->ca.rule = p->ca.rule; s->ca.row = p->ca.row; s->ca.tap = p->ca.tap; s->ca.trig = p->ca.trig;
  s->lz.rate = p->lz.rate; s->lz.rho = p->lz.rho; s->lz.trig = p->lz.trig;
  s->bw.state = s->mem[REG_BWMODE] == 1 ? 1 : 0;

  /* 5) setup blocks run once (they fill scratch registers with scales, chords and so on) */
  for (int i = 0; i < p->nsetup; i++) vm_run_block(&s->vm, p->setup[i]);
  fez_compute_eff(s);
}

/* A blank patch: default registers, no code, no matrix routes, every pulser off, the level knob at half. The starting
   point for a new patch, and for reading a patch file (a file only lists what differs). */
void fez_load_blank(FezSynth *s) {
  reset_machine(s);
  s->level_pot = 16384; s->cutoff_add = 0;
  /* The devices start from their standard settings. Automaton: rule 30, the usual seed row, watching cell 0, no pulser
     (trig 4). Lorenz: sigma 10 and beta 8/3 are fixed in the equations; rho 28 is the classic chaotic setting (register
     15291 is 27.9995 on the scale of lz_rho, the nearest the 15-bit register can get); start point (1, 1, 1); no pulser. */
  s->ca.rule = 30; s->ca.row = 0x2a54; s->ca.tap = 0; s->ca.trig = 4;
  s->lz.rate = 8000; s->lz.rho = 15291; s->lz.trig = 4;
  s->bw.state = s->mem[REG_BWMODE] == 1 ? 1 : 0;
  fez_compute_eff(s);
}

void fez_clear_buffer(FezSynth *s) { fez_push_buf_event(s, EV_CLEAR, 0); }

/* ---------------------------------------------------------------- envelope gate */

void fez_fire_gate(FezSynth *s) {
  double atk = atk_sec(s->mem[REG_ATK]), rel = rel_sec(s->mem[REG_REL]);
  if (s->gate_q_count < GATE_Q_SIZE) {
    GateEvent *g = &s->gate_q[(s->gate_q_head + s->gate_q_count++) % GATE_Q_SIZE];
    g->atk = atk; g->rel = rel; g->v0 = s->env.v;
  }
  s->env.phase = ENV_ATT; s->env.atk = atk; s->env.rel = rel;
}

/* ---------------------------------------------------------------- the keyboard device */

/* The newest held key sets the pitch. PITCH and KBNOTE are written together: PITCH is what you hear, KBNOTE is what a
   block or the matrix can read afterwards (PITCH may be changed by a block, KBNOTE only by playing). */
static void kb_set_note(FezSynth *s, int note) {
  s->mem[REG_PITCH] = (int16_t)(note * 256);
  s->mem[REG_KBNOTE] = (int16_t)(note * 256);
}

void fez_note_on(FezSynth *s, int note) {
  note = fez_clamp(note, 0, 127);
  int n = 0;
  for (int i = 0; i < s->kb_n; i++) if (s->kb_held[i] != note) s->kb_held[n++] = s->kb_held[i];     /* pressing a held key again moves it to the top */
  s->kb_n = n;
  if (s->kb_n == KB_MAX_HELD) { memmove(s->kb_held, s->kb_held + 1, (KB_MAX_HELD - 1) * sizeof s->kb_held[0]); s->kb_n--; }   /* forget the oldest */
  s->kb_held[s->kb_n++] = note;
  kb_set_note(s, note);
  s->mem[REG_KBGATE] = 1; s->gate_hold = 1;
  fez_fire_gate(s);
  for (int p = 0; p < NUM_PULSERS; p++) if ((s->mem[REG_KBPUL] >> p) & 1) s->vm.pending |= 1u << p;
}

void fez_note_off(FezSynth *s, int note) {
  note = fez_clamp(note, 0, 127);
  int i = s->kb_n - 1;
  while (i >= 0 && s->kb_held[i] != note) i--;                  /* the newest press of this note */
  if (i < 0) return;                                            /* it was never down: nothing to do */
  int was_newest = i == s->kb_n - 1;
  memmove(s->kb_held + i, s->kb_held + i + 1, (size_t)(s->kb_n - 1 - i) * sizeof s->kb_held[0]);
  s->kb_n--;
  if (s->kb_n == 0) { s->mem[REG_KBGATE] = 0; s->gate_hold = 0; }      /* the envelope now falls over REL */
  else if (was_newest) kb_set_note(s, s->kb_held[s->kb_n - 1]);        /* back to the key before, with no new trigger */
}

void fez_all_notes_off(FezSynth *s) {
  while (s->kb_n > 0) fez_note_off(s, s->kb_held[s->kb_n - 1]);
}

/* ---------------------------------------------------------------- modulation matrix */

/* The value of one matrix source, named by the register address SRC_TABLE gives it. Each is a signed number, nominally
   -32768..32767. Most are a register as it stands; a few are scaled so that "around the middle" is 0. */
static int source_value(const FezSynth *s, int addr) {
  if (addr == REG_LFO0) return s->lfo_val;
  if (addr == REG_ENV) return fez_round(s->env.v * 32767);
  if (addr == REG_CADENS) return fez_ca_read(s, REG_CADENS);
  if (addr >= REG_LZX && addr <= REG_LZZ) return fez_lz_read(s, addr);
  if (addr == REG_SQOUT) return fez_clamp((s->mem[REG_SQ0 + fez_clamp(s->sq_cur, 0, 7)] - 15360) * 8, -32768, 32767);   /* the step's note around C4 */
  if (addr == REG_TMOUT) return fez_tm_out(s);
  if (addr == REG_KBNOTE) return s->mem[REG_KBGATE] ? fez_clamp((s->mem[REG_KBNOTE] - 15360) * 8, -32768, 32767) : 0;  /* held note around C4, 0 with no key down */
  if (addr == 0) return 0;                                                                     /* source 0: none */
  return s->mem[addr];                                                                        /* CV0..CV3, MP0..MP15, SLO0..SLO3 */
}

/* Every matrix source's value, in SRC_TABLE order. */
static void matrix_sources(const FezSynth *s, int sv[NUM_SOURCES]) {
  for (int i = 0; i < NUM_SOURCES; i++) sv[i] = source_value(s, SRC_TABLE[i].addr);
}

/* One matrix source's value right now (src is a row of SRC_TABLE). For screens that show what a route is doing. */
int fez_matrix_source(const FezSynth *s, int src) {
  int sv[NUM_SOURCES];
  matrix_sources(s, sv);
  return src >= 0 && src < NUM_SOURCES ? sv[src] : 0;
}

/* eff[a] = the register's value plus every route aimed at it (weight/32768 times the source), clamped to a
   register's range. The voice and effects read eff, not the raw registers. */
void fez_compute_eff(FezSynth *s) {
  int sv[NUM_SOURCES];
  matrix_sources(s, sv);
  for (int i = 0; i < PARAM_COUNT; i++) {
    int a = PARAM_TABLE[i].addr;
    if (a == REG_GATE) continue;                                     /* GATE has no continuous value */
    double base = a == REG_LEVEL ? s->level_pot : s->mem[a];
    if (a == REG_CUTOFF) base += s->cutoff_add;
    double sum = 0;
    for (int r = 0; r < s->nroutes; r++)
      if (s->routes[r].dst == a) sum += ((double)s->routes[r].w * sv[s->routes[r].src]) / 32768.0;
    s->eff[a] = fez_clamp(fez_round(base + sum), 0, 32767);
  }
  if (s->mem[REG_QON]) s->eff[REG_PITCH] = fez_quantise(s->eff[REG_PITCH], s->mem[REG_QSCALE], s->mem[REG_QROOT]);   /* the quantiser snaps what the voice hears */
}

/* ---------------------------------------------------------------- the clock */

/* A pulser aimed at GRN starts the next grain voice, round robin, skipping voices with no length or no level. */
static void grain_trigger(FezSynth *s) {
  for (int k = 0; k < 8; k++) {
    int i = (s->grn_next + k) % 8;
    int lvl = i == 0 ? s->eff[REG_GLVL0] : s->mem[REG_GLVL0 + i];           /* voice 0's level can be modulated */
    if (s->mem[REG_GLEN0 + i] > 0 && lvl > 0) { fez_push_buf_event(s, EV_TRIG, 1 << i); s->grn_next = (i + 1) % 8; return; }
  }
}

/* Move everything that changes continuously over time forward by dt_ms. */
static void advance(FezSynth *s, double dt_ms) {
  fez_lz_advance(s, dt_ms);

  /* the write head of the buffer device always moves; "frozen" only stops it writing */
  double ms = s->mem[REG_BUFMS];
  s->bw.pos += dt_ms / ms;
  if (s->bw.pos >= 1) s->bw.pos -= floor(s->bw.pos);
  if (s->bw.state == 2) {
    s->bw.rec += dt_ms / ms;
    if (s->bw.rec >= s->mem[REG_BWLEN] / 32767.0) s->bw.state = 0;
  }

  /* LFO: a sine, rate set by LFORATE */
  s->lfo_phase += (2 * FEZ_PI * lfo_hz(s->mem[REG_LFORATE]) * dt_ms) / 1000.0;
  if (s->lfo_phase > 1e6) s->lfo_phase = 0;                     /* keep the phase small so sin() stays accurate */
  s->lfo_val = fez_round(sin(s->lfo_phase) * 32767);

  /* slew generators: one-pole smoothing of CV0..CV3, written to SLO0..SLO3. A one-pole filter moves a fixed
     fraction of the remaining distance each step: y += (target - y) * (1 - e^(-dt/tau)). */
  for (int i = 0; i < NUM_SLEW; i++) {
    double target = s->mem[REG_CV0 + i], y = s->slew[i];
    int t = s->mem[REG_SLT0 + i];
    if (t == 0) y = target;                                     /* slew time 0 means off */
    else {
      y += (target - y) * (1 - exp(-dt_ms / 1000.0 / slew_sec(t)));
      if (fabs(target - y) < 1) y = target;                     /* snap when close, so the tail ends */
    }
    s->slew[i] = y;
    s->mem[REG_SLO0 + i] = (int16_t)fez_round(y);
  }

  /* control envelope: linear attack, then exponential release with time constant rel/3 */
  double sec = dt_ms / 1000.0;
  if (s->env.phase == ENV_ATT) {
    s->env.v += sec / s->env.atk;
    if (s->env.v >= 1) { s->env.v = 1; s->env.phase = s->gate_hold ? ENV_ATT : ENV_REL; }   /* a held key keeps it at the top */
  } else if (s->env.phase == ENV_REL) {
    s->env.v *= exp(-sec / (s->env.rel / 3));
    if (s->env.v < 0.0005) { s->env.v = 0; s->env.phase = ENV_IDLE; }
  }
}

void fez_tick(FezSynth *s, double dt_ms) {
  s->tick_count++;

  /* 1) which pulsers fire: ones TRIGged last tick (by blocks or devices), plus any whose timer ran out */
  unsigned fire = s->vm.pending;
  s->vm.pending = 0;
  for (int i = 0; i < NUM_PULSERS; i++) {
    if (!s->pulsers[i].on) continue;
    s->pulsers[i].acc += dt_ms;
    double period = 60000.0 / s->pulsers[i].rate;
    if (s->pulsers[i].acc >= period) {
      s->pulsers[i].acc -= period;
      if (s->pulsers[i].acc > period) s->pulsers[i].acc = 0;    /* we fell far behind: do not fire a burst */
      fire |= 1u << i;
    }
  }

  for (int i = 0; i < NUM_PULSERS; i++) if ((fire >> i) & 1) s->pulsers[i].last_fire = s->tick_count;   /* for the screens' firing lights */

  /* 2) which blocks run: the ones aimed at a firing pulser, plus any asked for by writing RUN. A block runs once
     per tick however many pulsers fire. */
  unsigned run = s->sw_run;
  s->sw_run = 0;
  for (int b = 0; b < NUM_BLOCKS; b++) if (s->mem[REG_BP0 + b] & fire) run |= 1u << b;
  if (s->mem[REG_SQPUL] & fire) fez_seq_clock(s);
  if (s->mem[REG_TMPUL] & fire) fez_tm_step(s);
  if (s->mem[REG_GRPUL] & fire) grain_trigger(s);
  for (int b = 0; b < NUM_BLOCKS; b++) if ((run >> b) & 1) vm_run_block(&s->vm, b);

  /* 3) let time pass, then refresh the effective parameters */
  advance(s, dt_ms);
  fez_compute_eff(s);
}
