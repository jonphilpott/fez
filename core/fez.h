/* FEZ core: the one public header. The caller owns a FezSynth (no malloc, no globals, no OS calls inside the core),
   so the same code runs in a desktop app, on a Raspberry Pi, or on a microcontroller.

   The core has two halves that talk through small queues:
     control side  registers, the VM, pulsers, the modulation matrix, the devices (cellular automaton, Lorenz
                   attractor, sequencer). It runs in "ticks" of a few milliseconds.
     audio side    the voice, effects and buffer device. It runs sample by sample (audio.c, buf.c).
   Everything is deterministic: given the same patch, random seed and sequence of calls, a run repeats exactly.
   (A change that alters a run for the same inputs is a change in behaviour: look at it twice.) */
#ifndef FEZ_H
#define FEZ_H

#include <stdint.h>
#include "tables.h"
#include "reg_names.h"           /* REG_DLYTIME etc: one constant per register name */
#include "vm.h"
#include "audio.h"

#define FEZ_PI       3.14159265358979323846   /* M_PI is not in strict C99 */
#define NUM_REGS     640       /* the register file: addresses 0..639 */
#define NUM_PULSERS  4
#define FEZ_RING 16                  /* recent rows each device keeps for its picture (see ca.hist, tm.hist) */
#define NUM_SLEW     4          /* slew generators: CV0..CV3 smoothed into SLO0..SLO3 */
#define PRATE_MIN    10         /* pulser rates, pulses per minute: the slowest (slower events: a block that acts by chance) */
#define PRATE_MAX    600        /* ... and the fastest, ten a second */
#define MAX_ROUTES   16         /* modulation matrix size */
#define BUF_Q_SIZE   64         /* events waiting for the audio side; extra events are dropped */
#define GATE_Q_SIZE  16
#define NUM_SOURCES  35         /* matrix sources, 0 (none) to 34 (SWELL); test_core checks it against SRC_TABLE */
#define KB_MAX_HELD  16         /* keys the keyboard device tracks at once */

/* Events the control side hands to the audio side. */
enum { EV_CLEAR, EV_TRIG, EV_STOP, EV_REC };
typedef struct { int type; int mask; } BufEvent;                 /* mask: bit n is grain voice n */
typedef struct { double atk, rel, v0; } GateEvent;               /* envelope fired: times in seconds, level it started from */

enum { ENV_IDLE, ENV_ATT, ENV_REL };

typedef struct FezSynth {
  /* ---- registers ---- */
  int16_t mem[NUM_REGS];          /* every public register lives here, one cell per address */
  int16_t priv[NUM_BLOCKS][16];   /* P0..P15: private to each block */
  int8_t param_index[NUM_REGS];   /* address -> row of PARAM_TABLE, or -1 if the register is not a parameter */
  int eff[NUM_REGS];              /* effective value of each parameter: register + modulation matrix. Voice reads these. */
  int master_level;               /* the master volume: LEVEL reads this, not the register, so blocks cannot set it (CTRL and Vol +/- do) */

  /* ---- the VM and its random numbers ---- */
  Vm vm;
  uint32_t rng;                   /* xorshift32 state, never 0 */

  /* ---- clock ---- */
  struct { int on; double acc; unsigned long last_fire; } pulsers[NUM_PULSERS];   /* (the rate is the register PRATE0+i) acc: ms since last pulse; last_fire: tick_count when it last fired, 0 if never */
  unsigned sw_run;                /* blocks asked to run on the next tick by writing RUN */
  unsigned long tick_count;

  /* ---- modulation matrix ---- */
  RouteDef routes[MAX_ROUTES];
  int nroutes;

  /* ---- slow modulators ---- */
  double slew[NUM_SLEW];          /* CV0..CV3 smoothed by their slew time (the SLO registers hold the rounded value) */
  double lfo_phase;               /* radians */
  int lfo_val;                    /* -32767..32767 */
  struct { double v; int phase; double atk, rel; } env;   /* control-rate envelope: a modulation source only */

  /* ---- devices ---- */
  struct { int rule, row, tap, trig; unsigned gen; int hist[FEZ_RING]; } ca;   /* cellular automaton: 16 cells in a ring. gen counts generations run; hist keeps the last FEZ_RING rows (generation g is hist[g % FEZ_RING]), so a screen can draw every step, even several within one frame, or a step that changed nothing */
  struct { double x, y, z; int rho, trig; } lz;    /* (its speed is the register LZRATE, a matrix destination) */     /* Lorenz attractor */
  int sq_cur, sq_count;           /* step sequencer position, number of clocks so far */
  struct { int bits; unsigned steps; int hist[FEZ_RING]; } tm;   /* Turing machine: the 16-bit ring (only the low TMLEN bits are used). steps and hist: the same record as ca.gen and ca.hist */
  int kb_held[KB_MAX_HELD], kb_n; /* keyboard device: MIDI notes held, oldest first; the newest one sounds */
  int gate_hold;                  /* 1 while a keyboard key is down: the envelope (control and audio) stays open */
  int fx_clear;                   /* set by a patch load: the audio side empties the delay, spring and compressor at its next tick */
  struct { double pos, rec; int state; } bw;              /* write head of the buffer device, as the registers see it */
  int grn_next;                   /* next grain voice for a pulser aimed at GRN (round robin) */

  /* ---- queues to the audio side (ring buffers) ---- */
  BufEvent buf_q[BUF_Q_SIZE];   int buf_q_head, buf_q_count;
  GateEvent gate_q[GATE_Q_SIZE]; int gate_q_head, gate_q_count;

  /* ---- audio side ---- */
  Audio audio;
} FezSynth;

/* ---- lifecycle ---- */
void fez_init(FezSynth *s, uint32_t seed);              /* zero everything, set the random seed, load patch 0 */
void fez_default_registers(int16_t mem[NUM_REGS]);      /* the register values every patch starts from (mem must be zeroed first) */
void fez_clear_buffer(FezSynth *s);                     /* empty the buffer device's recording (queues the event for the audio side) */
void fez_load_blank(FezSynth *s);                       /* an empty patch: defaults, no code, pulsers off */
void fez_load_patch(FezSynth *s, int index);            /* replace the whole machine state with an example patch */

/* ---- registers: what a UI, MIDI input or test uses. Bad addresses read as 0 and writes are ignored. ---- */
int  fez_reg_read(FezSynth *s, int addr);
void fez_reg_write(FezSynth *s, int addr, int value);
/* Set modulation pot n (0..15) to value (0..32767). The pots are read only to code, so this is how a knob or a MIDI controller
   moves one: the host calls it, the matrix reads it (sources MP0..MP15). Out-of-range arguments are clamped. */
void fez_set_pot(FezSynth *s, int pot, int value);

/* ---- time ---- */
void fez_tick(FezSynth *s, double dt_ms);               /* advance the control side by dt_ms milliseconds */

/* ---- sound ----
   fez_render_stereo writes `frames` left/right pairs (-1..1) at FEZ_SAMPLE_RATE (48 kHz); fez_render writes the mono mix of the same. Runs the control side itself, one tick every
   5 ms of audio, so the app only has to call this from its audio callback. Registers may be written between calls
   (from the same thread, or with the audio device locked). */
void fez_render(FezSynth *s, float *out, int frames);         /* mono: (left + right) / 2, `frames` floats */
void fez_render_stereo(FezSynth *s, float *out, int frames);  /* interleaved left, right: 2 * `frames` floats */

/* ---- queues the audio side drains ---- */
int  fez_pop_buf_event(FezSynth *s, BufEvent *out);     /* 1 if an event was returned */
int  fez_pop_gate_event(FezSynth *s, GateEvent *out);

/* ---- internals shared between core files (not for apps) ---- */
int  fez_read(FezSynth *s, int addr, int block, unsigned *fault);
void fez_write(FezSynth *s, int addr, int value, int block, unsigned *fault);
void fez_set_route(FezSynth *s, int src, int dst, int w, unsigned *fault);
int  fez_source_index(const char *name);                /* a matrix source's number from its name (any case), or -1 */
int fez_modulated_now(const FezSynth *s, int addr);   /* one register plus its routes, worked out now (eff is a tick old) */
void fez_compute_eff(FezSynth *s);
int  fez_matrix_source(const FezSynth *s, int src);     /* a matrix source's current value, -32768..32767 */
void fez_fire_gate(FezSynth *s);

/* ---- the keyboard device (also where MIDI will land) ---- Notes are MIDI numbers 0..127. */
void fez_note_on(FezSynth *s, int note);                /* PITCH and KBNOTE take the note, the envelope fires and is held open, KBPUL pulsers fire */
void fez_note_off(FezSynth *s, int note);               /* the newest key up returns to the key before it; the last key up lets the envelope fall */
void fez_all_notes_off(FezSynth *s);                    /* every key up (window lost focus, patch loaded) */
int  fez_ca_read(const FezSynth *s, int addr);
void fez_ca_write(FezSynth *s, int addr, int v, unsigned *fault);
int  fez_lz_read(const FezSynth *s, int addr);
void fez_lz_write(FezSynth *s, int addr, int v, unsigned *fault);
void fez_lz_advance(FezSynth *s, double dt_ms);
void fez_seq_clock(FezSynth *s);
void fez_tm_step(FezSynth *s);
int  fez_tm_out(const FezSynth *s);                      /* the Turing machine's output, 0..32767 */
int  fez_scale_mask(int scale);                          /* 12 bits: bit k set = k semitones above the root is in the scale */
const char *fez_scale_name(int scale);
int  fez_scale_degree(int scale, int k);                 /* degree k (any integer) of the scale (0..7), 8.8 pitch above the root */
int  fez_colour_degree(int col, int k);                  /* degree k (any integer) of the colour (0..15), 8.8 pitch above the root */
int  fez_quantise(int pitch, int scale, int root);       /* snap a pitch (8.8 semitones) to a scale over a root */
void fez_push_buf_event(FezSynth *s, int type, int mask);
int  fez_clamp(int x, int lo, int hi);
int  fez_round(double x);                               /* round half up (C round() differs for -0.5) */
int  fez_floor_div(int a, int b);                       /* a / b rounded toward minus infinity (C's / rounds toward zero) */
int  fez_i16(int x);                                    /* wrap into -32768..32767, as a 16-bit register would */

#endif
