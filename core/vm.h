/* The FEZ token VM: a tiny Forth-like language. A "block" is a program of at most 128 slots (a slot is one number
   or one operation), run to completion when a pulser fires it. A "word" is a shorter program (24 slots) that a
   block can call with W.

   This file knows nothing about the synth. Everything the VM needs from the outside world (reading and writing
   registers, routing, random numbers) comes through the VmHost callbacks, so the VM can be tested on its own. */
#ifndef FEZ_VM_H
#define FEZ_VM_H

#include <stdint.h>

#define BLOCK_SLOTS 128
#define WORD_SLOTS  24
#define NUM_BLOCKS  16
#define NUM_WORDS   16
#define STACK_DEPTH 16
#define WORD_SRC_MAX  256    /* source text kept per word (24 slots at up to 9 characters, plus slack) */
#define BLOCK_SRC_MAX 1280   /* source text kept per block: 128 slots at up to 9 characters each, plus slack */
#define STEP_CAP    256      /* a block may execute this many slots per run; stops endless loops */

/* Fault bits, OR-ed into a block's `fault` field. Bit 8 is unused on purpose: saturation is not a fault. */
enum {
  FAULT_UNDER = 1,    /* stack underflow */
  FAULT_OVER  = 2,    /* stack overflow */
  FAULT_DIV0  = 4,    /* divide by zero */
  FAULT_ADDR  = 16,   /* bad address */
  FAULT_LABEL = 32,   /* jump to a label that does not exist */
  FAULT_CAP   = 64,   /* step cap reached */
  FAULT_WORD  = 128,  /* W called an empty or broken word */
  FAULT_CTX   = 256   /* W or YIELD used where it is not allowed (inside a word) */
};

/* Operations. The order here MUST match OP_NAMES in vm.c (the tests check the count and a few names). */
enum {
  OP_DUP, OP_DROP, OP_SWAP, OP_OVER, OP_ROT, OP_ADD, OP_SUB, OP_MUL, OP_FMUL, OP_DIV, OP_MULDIV, OP_NEG, OP_ABS, OP_MIN,
  OP_MAX, OP_MOD, OP_AND, OP_OR, OP_XOR, OP_NOT, OP_SHL, OP_SHR, OP_ROL, OP_ROR, OP_EQ, OP_LT, OP_GT, OP_ULT, OP_FETCH, OP_STORE,
  OP_LABEL, OP_JMP, OP_JZ, OP_JNZ, OP_SKIP, OP_EXIT, OP_YIELD, OP_TRIG, OP_ROUTE, OP_WORD, OP_RND, OP_ST, OP_HZ,
  OP_COLOUR, OP_SELECT, OP_SCALE, OP_COUNT
};
extern const char *const OP_NAMES[OP_COUNT];

typedef struct { uint8_t is_op; int16_t value; } Slot;   /* is_op: value is an OP_ code; else a number */

typedef struct {
  int ok;                          /* 1 if the source compiled */
  char err[96];                    /* message if not (long tokens are cut short) */
  int count;                       /* slots used */
  int err_pos;                     /* byte offset in the source of the token that failed to compile, or -1 (an editor can highlight it) */
  uint16_t pos[BLOCK_SLOTS];       /* byte offset in the source of the token each slot came from (a note name makes two slots, one token) */
  Slot slots[BLOCK_SLOTS];
  int16_t labels[16];              /* slot index just after "n L", or -1 if label n is not defined */
} Compiled;

/* Callbacks into the host (the synth). `fault` points at the running block's fault bits: set FAULT_ADDR there
   for a bad address. `block` is the number of the running block (private P0..P15 registers are per block). */
typedef struct {
  void *ctx;
  int (*read)(void *ctx, int addr, int block, unsigned *fault);
  void (*write)(void *ctx, int addr, int value, int block, unsigned *fault);
  void (*route)(void *ctx, int src, int dst, int weight, unsigned *fault);
  double (*rnd)(void *ctx);        /* uniform in [0, 1) */
} VmHost;

typedef struct {
  char src[BLOCK_SRC_MAX];         /* the source text, kept so an editor can show it and a patch can be saved */
  Compiled c;
  int halted;                      /* a fault stopped it; it stays stopped until its source is set again */
  int resume;                      /* slot index to continue from after YIELD, or -1 */
  unsigned fault;                  /* all faults since the source was set */
  int fault_slot;                  /* slot of this block's own code that caused the fault (the W call, if a word did), or -1. A halt overwrites it. */
  int steps, peak, runs;           /* slots in the last run, the most in any run, number of runs */
} Block;

typedef struct {
  VmHost host;
  Compiled words[NUM_WORDS];
  char word_src[NUM_WORDS][WORD_SRC_MAX];   /* the words' source text, kept so a patch can be saved */
  Block blocks[NUM_BLOCKS];
  unsigned pending;                /* pulsers fired by TRIG, bit n for pulser n; the synth reads and clears it */
  /* state of the run in progress */
  int stack[STACK_DEPTH], sp;
  int steps, yield_ip;
  unsigned *fault;
  Block *cur;                      /* the block being run, so exec() can say where a fault happened */
} Vm;

void vm_compile(Compiled *c, const char *src, int max_slots);
void vm_init(Vm *vm, VmHost host);
void vm_set_block(Vm *vm, int b, const char *src);
void vm_set_word(Vm *vm, int w, const char *src);
void vm_run_block(Vm *vm, int b);

const char *vm_fault_name(unsigned fault);   /* plain-English name of the first fault bit set, or "" */
int hz_to_pitch(int tenths_of_hz);   /* the HZ word: 4400 gives A4 (15360 + 9*256 = 17664) */

#endif
