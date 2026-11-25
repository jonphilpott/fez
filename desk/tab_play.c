/* The PLAY tab: four big pads, one for each pulser, so a button press (or the keys 1 to 4) fires a block by hand.
   It exists for the handheld, where there are no note keys: A, B, X and Y are the four pads. A pad fires its pulser exactly
   as the pulser's own timer would, so whatever blocks, the sequencer or the grains are wired to it happen on the next 5 ms tick.
   Underneath, the sequencer's eight steps show where it is, and S gives it a clock tick by hand. */
#include "desk.h"
#include <stdio.h>
#include <string.h>

#define FIRE_LIGHT_TICKS 40          /* a pad stays lit for 40 ticks of 5 ms: 200 ms, like the Pulsers tab's light */

static const char *PAD_LABEL[NUM_PULSERS] = { "A", "B", "X", "Y" };   /* the handheld buttons; the desktop keys are 1 to 4 */

void tab_play_draw(Desk *d, View *v) {
  FezSynth *s = d->synth;
  view_text(v, 0, 0, "PLAY", COL_AMB);
  view_text_right(v, 6, v->cols - 6, 0, d->pulsers_on ? "pulsers on" : "pulsers OFF", d->pulsers_on ? COL_DIM : COL_RED);

  /* 1. the pads: four boxes side by side across the view */
  int gap = 4, pw = (v->w - 3 * gap) / NUM_PULSERS, ph = v->rh * 4;
  int y0 = v->y + v->rh * 2;
  for (int p = 0; p < NUM_PULSERS; p++) {
    int x = v->x + p * (pw + gap);
    unsigned long since = s->tick_count - s->pulsers[p].last_fire;
    double light = s->pulsers[p].last_fire && since < FIRE_LIGHT_TICKS ? 1.0 - (double)since / FIRE_LIGHT_TICKS : 0.0;
    gfx_fill(&d->cv, x, y0, pw, ph, gfx_mix(gfx_colour(COL_PNL), gfx_colour(COL_MAG), light));
    gfx_frame(&d->cv, x, y0, pw, ph, gfx_colour(light > 0.05 ? COL_MAG : COL_LINE));
    char buf[16];
    snprintf(buf, sizeof buf, "P%d", p);
    gfx_text(&d->cv, x + (pw - 2 * GFX_CELL_W * d->scale) / 2, y0 + 3, buf, d->scale, gfx_colour(light > 0.5 ? COL_BG : COL_FG));
    gfx_text(&d->cv, x + (pw - GFX_CELL_W * d->scale) / 2, y0 + v->rh + 3, PAD_LABEL[p], d->scale, gfx_colour(light > 0.5 ? COL_BG : COL_AMB));
    snprintf(buf, sizeof buf, "%s %d", s->pulsers[p].on ? "on" : "off", (int)(s->pulsers[p].rate + 0.5));
    gfx_text(&d->cv, x + (pw - (int)strlen(buf) * GFX_CELL_W * d->scale) / 2, y0 + ph - v->rh, buf, d->scale, gfx_colour(light > 0.5 ? COL_BG : COL_DIM));
  }

  /* 2. the sequencer: eight steps, the one it last played lit, and the steps that are switched off dim */
  int y1 = y0 + ph + v->rh;
  view_text(v, 0, (y1 - v->y) / v->rh, "SEQUENCER", COL_FG);
  int sw = (v->w - 7 * gap) / 8, y2 = y1 + v->rh + 2;
  for (int i = 0; i < 8; i++) {
    int x = v->x + i * (sw + gap), on = (s->mem[REG_SQEN] >> i) & 1, here = i == s->sq_cur && s->sq_count > 0;
    gfx_fill(&d->cv, x, y2, sw, v->rh, gfx_colour(here ? COL_MAG : on ? COL_SEL : COL_PNL));
    gfx_frame(&d->cv, x, y2, sw, v->rh, gfx_colour(on ? COL_DIM : COL_LINE));
  }
}

void tab_play_key(Desk *d, int key, int mods) { (void)d; (void)key; (void)mods; }

void tab_play_text(Desk *d, int ch) {
  if (ch >= '1' && ch <= '0' + NUM_PULSERS) d->synth->vm.pending |= 1u << (ch - '1');   /* fire on the next tick, as TRIG does */
  else if (ch == 's' || ch == 'S') fez_seq_clock(d->synth);
}
