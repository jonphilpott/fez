/* The buffer device: one circular buffer of audio, one write head that records into it, and eight read voices.
   A voice is either a continuous tap (a delay line or a free-running loop) or a windowed "grain" (a short snippet
   played once or looped). It began as a port of a JavaScript prototype. */
#ifndef FEZ_BUF_H
#define FEZ_BUF_H

#define FEZ_SAMPLE_RATE 48000                     /* fixed for now (see fez.h); every time constant derives from it */
#define BUF_MAX_SECONDS 30                        /* the longest BUFMS */
#define BUF_SAMPLES (FEZ_SAMPLE_RATE * BUF_MAX_SECONDS + 16)
#define BUF_VOICES 8

/* Settings the control side hands over (a copy of registers, in friendly units). */
typedef struct {
  int mode;            /* 0 frozen, 1 running, 2 one-shot */
  double fb;           /* feedback from the voices back into the write head, 0..1 */
  double bin;          /* record level, 0..1 */
  int ms;              /* buffer length in milliseconds */
  int bwlen;           /* one-shot length, 1..32767 (fraction of the buffer) */
  double win;          /* grain window: 0 rectangular .. 1 Hann */
  int pitch;           /* the voice's current pitch, 8.8 semitones (for voices that track it) */
  struct { int pos, rate, len, lvl, flags; } v[BUF_VOICES];   /* flags: 1 REL, 2 REV, 4 TRK, 8 LOOP */
} BufCfg;

typedef struct {
  int on, started;
  double pos, rem, tot, lvl_s, off_s, env;
} BufVoice;

typedef struct {
  float buf[BUF_SAMPLES];
  int len;                       /* samples in use (BUFMS worth) */
  double wp;                     /* write position; an integer value kept as double like the JS */
  int state;                     /* 0 frozen, 1 running, 2 recording a one-shot */
  int rec_left, last_mode;
  BufCfg c;
  BufVoice vo[BUF_VOICES];
  float ratio[BUF_VOICES];       /* playback speed of each voice, from RATE (and PITCH if tracking). A float, not a
                                    double: the prototype kept these in a Float32Array, and the output stays the same */
  double k_off, k_lvl, declick;  /* smoothing coefficients; grain edge length in samples */
} BufEngine;

void buf_init(BufEngine *e);
void buf_set_cfg(BufEngine *e, const BufCfg *c);
void buf_record(BufEngine *e);
void buf_trig(BufEngine *e, int mask);
void buf_stop(BufEngine *e, int mask);
void buf_clear(BufEngine *e);
/* One sample in, one sample out: x goes to the write head (scaled by BIN); the return value is the sum of the
   read voices. Call buf_set_cfg only between samples. */
double buf_process(BufEngine *e, double x);

#endif
