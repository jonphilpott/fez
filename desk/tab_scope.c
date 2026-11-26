/* The SCOPE tab: the output waveform. The audio thread copies every sample it renders into a ring buffer
   (d->scope); here we find a rising zero crossing so a steady tone holds still, and draw the next stretch. */
#include "desk.h"
#include <stdio.h>

#define SHOWN   1024            /* samples across the screen: 21 ms at 48 kHz */
#define SEARCH  2048            /* how far back we look for a trigger point */

/* One ear's trace in its own lane (a box px,py,pw,ph): a zero line, lines at plus and minus one half, and the beam. The trace looks like
   a phosphor beam: a bright core a few pixels thick (3 rows at the default scale), with a dimmer glow either side. A line is thickened
   by drawing it several times, shifted up and down a pixel at a time. Two passes (all the glow, then all the core) so the glow of one
   segment never paints over the core of the last. `sc` is the text scale, so the beam keeps the same visual weight in a big or small
   window. `ch` is 0 for left, 1 for right; `start` is the first sample to draw. */
static void draw_lane(Desk *d, int px, int py, int pw, int ph, int ch, unsigned start, int colour) {
  Canvas *cv = &d->cv;
  unsigned mask = SCOPE_RING - 1;
  gfx_fill(cv, px, py, pw, ph, gfx_colour(COL_PNL));
  gfx_fill(cv, px, py + ph / 2, pw, 1, gfx_colour(COL_LINE));                   /* zero line */
  gfx_fill(cv, px, py + ph / 4, pw, 1, gfx_colour(COL_SEL));                    /* plus and minus one half */
  gfx_fill(cv, px, py + 3 * ph / 4, pw, 1, gfx_colour(COL_SEL));
  int sc = d->scale;
  uint32_t core = gfx_mix(gfx_colour(colour), gfx_colour(COL_FG), 0.12);       /* the beam: bright, a touch toward white */
  uint32_t glow = gfx_mix(gfx_colour(COL_BG), gfx_colour(colour), 0.45);       /* the halo around it */
  Canvas tile = *cv;                                                           /* remember the tile's clip, to put it back after */
  gfx_clip(cv, px, py, pw, ph);                                                /* a tall wave must not spill into the other lane */
  for (int pass = 0; pass < 2; pass++) {
    int reach = pass == 0 ? sc : sc / 2;                                       /* glow: wide, core: narrow */
    int prev_x = 0, prev_y = 0;
    for (int x = 0; x < pw; x++) {
      float v = d->scope[2 * ((start + (unsigned)(x * SHOWN / pw)) & mask) + ch];
      v = v > 1 ? 1 : v < -1 ? -1 : v;
      int y = py + ph / 2 - (int)(v * (ph / 2 - sc - 2));                       /* leave room so the glow stays inside the lane */
      if (x > 0)
        for (int dy = -reach; dy <= reach; dy++) gfx_line(cv, px + prev_x, prev_y + dy, px + x, y + dy, pass == 0 ? glow : core);
      prev_x = x; prev_y = y;
    }
  }
  cv->clip_x0 = tile.clip_x0; cv->clip_y0 = tile.clip_y0; cv->clip_x1 = tile.clip_x1; cv->clip_y1 = tile.clip_y1;   /* back to the tile's clip, not the whole window */
  gfx_text(cv, px + 2, py + 1, ch ? "R" : "L", sc, gfx_colour(colour));
}

void tab_scope_draw(Desk *d, View *v) {
  char info[64];
  snprintf(info, sizeof info, "%.0f ms  peak %.2f", SHOWN * 1000.0 / FEZ_SAMPLE_RATE, d->peak);
  view_text(v, 0, 0, "SCOPE", COL_AMB);
  view_text_right(v, 8, v->cols - 8, 0, info, COL_DIM);
  if (!d->scope || !d->scope_head) return;

  int px = v->x, py = v->y + v->rh, pw = v->w, ph = v->h - v->rh;
  if (pw < 8 || ph < 16) return;

  /* 1. the newest SHOWN samples are always drawable; look earlier in the LEFT channel for a rising crossing to start from.
     Both lanes start at the same sample, so any time difference between the ears shows as a shift between the two traces. */
  unsigned head = *d->scope_head, mask = SCOPE_RING - 1;
  unsigned newest_start = head - SHOWN, start = newest_start;
  for (unsigned back = SHOWN; back < SEARCH; back++) {
    unsigned i = head - back;
    if (d->scope[2 * ((i - 1) & mask)] < 0.0f && d->scope[2 * (i & mask)] >= 0.0f) { start = i; break; }
  }
  /* 2. two lanes, left above right, with a small gap */
  int gap = 3, lane = (ph - gap) / 2;
  draw_lane(d, px, py, pw, lane, 0, start, COL_GRN);
  draw_lane(d, px, py + lane + gap, pw, lane, 1, start, COL_CYN);
}

void tab_scope_key(Desk *d, int key, int mods) { (void)d; (void)key; (void)mods; }
void tab_scope_text(Desk *d, int ch) { (void)d; (void)ch; }
