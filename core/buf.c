/* The buffer device. See buf.h. It makes one call per sample (buf_process), which suits the voice that feeds it
   sample by sample. */
#include "buf.h"
#include "filters.h"   /* flush_denormal */
#include <math.h>
#include <string.h>

#define PI_D 3.14159265358979323846

static double wrap(const BufEngine *e, double p) {
  double n = e->len;
  p = fmod(p, n);                                    /* fmod keeps the sign of p, so a negative p comes out negative */
  if (p < 0) p += n;
  /* A tiny negative p (say -1e-14) plus n rounds to exactly n in floating point, which is one past the end. */
  return p >= n ? 0 : p;
}

void buf_init(BufEngine *e) {
  memset(e, 0, sizeof *e);
  e->last_mode = 1; e->state = 1;
  e->c.mode = 1; e->c.bin = 1; e->c.ms = 2000; e->c.bwlen = 32767; e->c.win = 0.5; e->c.pitch = 15360;
  for (int i = 0; i < BUF_VOICES; i++) e->c.v[i].rate = 15360;
  /* k_off: the delay-time slew, about 40 ms, so changing a tap's position glides its pitch like tape.
     k_lvl: level smoothing, about 10 ms, so level changes do not click. Both are one-pole coefficients,
     1 - e^(-1/(tau * sample rate)). */
  e->k_off = 1 - exp(-1.0 / (0.04 * FEZ_SAMPLE_RATE));
  e->k_lvl = 1 - exp(-1.0 / (0.01 * FEZ_SAMPLE_RATE));
  e->declick = fmax(8, floor(0.002 * FEZ_SAMPLE_RATE + 0.5));    /* grain edge fade: 2 ms */
  buf_set_cfg(e, &e->c);
}

void buf_set_cfg(BufEngine *e, const BufCfg *c) {
  e->c = *c;
  double l = floor(c->ms * (double)FEZ_SAMPLE_RATE / 1000.0 + 0.5);
  if (l < 64) l = 64;
  if (l > BUF_SAMPLES - 16) l = BUF_SAMPLES - 16;
  e->len = (int)l;
  if (e->wp >= e->len) e->wp = 0;
  if (c->mode != e->last_mode) { e->last_mode = c->mode; e->state = c->mode == 1 ? 1 : 0; }
  for (int i = 0; i < BUF_VOICES; i++) {
    const int *flags = &c->v[i].flags;
    double g = (c->v[i].rate - 15360) / 3072.0;               /* 8.8 semitones to octaves; C4 is unity speed */
    if (*flags & 4) g += (c->pitch - 15360) / 3072.0;         /* TRK: follow the voice's pitch */
    g = g < -4 ? -4 : g > 4 ? 4 : g;
    double r = pow(2.0, g);
    if (*flags & 2) r = -r;                                   /* REV: play backwards */
    e->ratio[i] = r;
  }
}

void buf_record(BufEngine *e) {
  memset(e->buf, 0, sizeof(float) * (size_t)e->len);
  e->wp = 0; e->state = 2;
  double n = floor(e->c.bwlen / 32767.0 * e->len + 0.5);
  e->rec_left = n < 1 ? 1 : (int)n;
}

/* Where a voice starts: behind the write head (flag REL) or at an absolute position, both given as a fraction. */
static double start_pos(const BufEngine *e, int k) {
  double off = e->c.v[k].pos / 32767.0 * e->len;
  return wrap(e, e->c.v[k].flags & 1 ? e->wp - off : off);
}

void buf_trig(BufEngine *e, int mask) {
  for (int i = 0; i < BUF_VOICES; i++) {
    if (!((mask >> i) & 1)) continue;
    BufVoice *o = &e->vo[i];
    if (e->c.v[i].len > 0) {                                   /* a grain: length in ms */
      double t = floor(e->c.v[i].len * (double)FEZ_SAMPLE_RATE / 1000.0 + 0.5);
      o->tot = o->rem = t < 8 ? 8 : t;
      o->pos = start_pos(e, i);
      o->on = 1;
    } else { o->pos = start_pos(e, i); o->started = 1; }       /* a continuous voice: restart the loop */
  }
}

void buf_clear(BufEngine *e) {                                 /* empty the buffer and silence every voice */
  memset(e->buf, 0, sizeof e->buf);
  e->wp = 0;
  for (int i = 0; i < BUF_VOICES; i++) { BufVoice *o = &e->vo[i]; o->on = 0; o->started = 0; o->env = 0; o->lvl_s = 0; }
}

void buf_stop(BufEngine *e, int mask) {
  for (int i = 0; i < BUF_VOICES; i++) if ((mask >> i) & 1) e->vo[i].on = 0;
}

/* Read with 4-point Hermite interpolation on the circular buffer: a smooth curve through four neighbouring
   samples, much cleaner than linear interpolation when a voice plays at a fractional speed. */
static double read_at(const BufEngine *e, double p) {
  int n = e->len;
  const float *b = e->buf;
  p = wrap(e, p);
  int i1 = (int)floor(p);
  double f = p - i1;
  int i0 = i1 == 0 ? n - 1 : i1 - 1, i2 = i1 + 1 >= n ? i1 + 1 - n : i1 + 1, i3 = i2 + 1 >= n ? i2 + 1 - n : i2 + 1;
  double y0 = b[i0], y1 = b[i1], y2 = b[i2], y3 = b[i3];
  double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
  return ((c3 * f + c2) * f + c1) * f + y1;
}

double buf_process(BufEngine *e, double x) {
  const BufCfg *c = &e->c;
  double sum = 0;
  for (int k = 0; k < BUF_VOICES; k++) {
    BufVoice *o = &e->vo[k];
    double lt = c->v[k].lvl / 32767.0;
    /* flush: decaying toward 0 it would otherwise end as a "denormal", a tiny number that is very slow on Intel chips */
    o->lvl_s = flush_denormal(o->lvl_s + (lt - o->lvl_s) * e->k_lvl);
    if (c->v[k].len == 0) {                                    /* continuous voice: a delay tap, or a free-running loop */
      if (lt == 0 && o->lvl_s < 1e-4) { o->started = 0; o->env = 0; continue; }
      double p;
      if (c->v[k].flags & 1) {                                 /* REL: a delay tap that sits POS behind the head */
        double tgt = c->v[k].pos / 32767.0 * e->len;
        tgt = tgt < 2 ? 2 : tgt > e->len - 2 ? e->len - 2 : tgt;
        if (!o->started) { o->off_s = tgt; o->started = 1; }
        o->off_s += (tgt - o->off_s) * e->k_off;               /* glide to the new delay time */
        p = e->wp - o->off_s; o->pos = wrap(e, p);
      } else {                                                 /* free-running loop of the buffer */
        if (!o->started) { o->pos = start_pos(e, k); o->started = 1; }
        p = o->pos; o->pos = wrap(e, o->pos + e->ratio[k]);
      }
      o->env = 1;
      sum += read_at(e, p) * o->lvl_s;
    } else if (o->on) {                                        /* grain: windowed one-shot, optionally looping */
      double ph = 1 - o->rem / o->tot;
      double hann = 0.5 - 0.5 * cos(2 * PI_D * ph);
      double edge = fmin(1, fmin(ph * o->tot / e->declick, (1 - ph) * o->tot / e->declick));
      double w = c->win * hann + (1 - c->win) * fmax(0, edge);  /* blend rectangular (with short edges) and Hann */
      o->env = w;
      sum += read_at(e, o->pos) * w * o->lvl_s;
      o->pos = wrap(e, o->pos + e->ratio[k]);
      if (--o->rem <= 0) {
        if (c->v[k].flags & 8) { o->rem = o->tot; o->pos = start_pos(e, k); }   /* LOOP: start over */
        else { o->on = 0; o->env = 0; }
      }
    } else o->env = 0;
  }
  if (e->state != 0) {                                         /* the head writes unless frozen */
    double w = x * c->bin + sum * c->fb;
    /* Soft clip every write: above 0.8 the signal bends toward 1.0. Feedback is not only BWFB: the buffer can also loop
       back to itself through the gate (MIXBP into the gate, and BWSRC taking the gate's output), so the clip must not
       depend on BWFB. Without it that loop grows to infinity, and then NaN, which sticks in the filters. */
    double a = fabs(w);
    if (a > 0.8) w = (w < 0 ? -1 : 1) * (0.8 + 0.2 * tanh((a - 0.8) / 0.2));
    e->buf[e->wp] = (float)w;
    if (e->state == 2 && --e->rec_left <= 0) e->state = 0;
  }
  if (++e->wp >= e->len) e->wp = 0;
  return sum;
}
