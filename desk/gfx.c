#include "gfx.h"
#include "palette.h"
#include "tables.h"      /* FONT5 */
#include <stdlib.h>

uint32_t gfx_colour(int i) {
  const unsigned char *c = FEZ_PALETTE[i];
  return 0xff000000u | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
}

uint32_t gfx_mix(uint32_t a, uint32_t b, double t) {
  uint32_t out = 0xff000000u;
  for (int shift = 0; shift <= 16; shift += 8) {
    double ca = (a >> shift) & 255, cb = (b >> shift) & 255;
    out |= (uint32_t)(ca + (cb - ca) * t + 0.5) << shift;
  }
  return out;
}

void gfx_unclip(Canvas *c) { c->clip_x0 = 0; c->clip_y0 = 0; c->clip_x1 = c->w; c->clip_y1 = c->h; }

void gfx_clip(Canvas *c, int x, int y, int w, int h) {
  c->clip_x0 = x < 0 ? 0 : x;            c->clip_y0 = y < 0 ? 0 : y;
  c->clip_x1 = x + w > c->w ? c->w : x + w;  c->clip_y1 = y + h > c->h ? c->h : y + h;
}

static void put(Canvas *c, int x, int y, uint32_t rgb) {
  if (x >= c->clip_x0 && x < c->clip_x1 && y >= c->clip_y0 && y < c->clip_y1) c->px[y * c->w + x] = rgb;
}

void gfx_fill(Canvas *c, int x, int y, int w, int h, uint32_t rgb) {
  /* Clip the box once, then write rows straight into memory (much faster than a put() per pixel). */
  int x0 = x < c->clip_x0 ? c->clip_x0 : x, x1 = x + w > c->clip_x1 ? c->clip_x1 : x + w;
  int y0 = y < c->clip_y0 ? c->clip_y0 : y, y1 = y + h > c->clip_y1 ? c->clip_y1 : y + h;
  for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) c->px[j * c->w + i] = rgb;
}

void gfx_frame(Canvas *c, int x, int y, int w, int h, uint32_t rgb) {
  gfx_fill(c, x, y, w, 1, rgb); gfx_fill(c, x, y + h - 1, w, 1, rgb);
  gfx_fill(c, x, y, 1, h, rgb); gfx_fill(c, x + w - 1, y, 1, h, rgb);
}

/* Bresenham's line: walk from one end to the other one pixel at a time, always stepping along the longer axis and
   stepping along the shorter one when the accumulated error says it is time. Integer maths only. */
void gfx_line(Canvas *c, int x0, int y0, int x1, int y1, uint32_t rgb) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    put(c, x0, y0, rgb);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

/* Each character is 8 rows of 5 bits; bit 4 of a row is the leftmost pixel. Anything outside ASCII 32..126 shows as '?'. */
void gfx_text(Canvas *c, int x, int y, const char *s, int scale, uint32_t rgb) {
  for (int k = 0; s[k]; k++) {
    int ch = (unsigned char)s[k];
    const unsigned char *g = FONT5[ch >= 32 && ch < 127 ? ch - 32 : 31];
    int gx = x + k * GFX_CELL_W * scale;
    if (gx + GFX_CELL_W * scale < c->clip_x0 || gx >= c->clip_x1) continue;      /* wholly outside: skip the glyph */
    for (int r = 0; r < 8; r++) for (int b = 0; b < 5; b++)
      if ((g[r] >> (4 - b)) & 1) gfx_fill(c, gx + b * scale, y + r * scale, scale, scale, rgb);
  }
}
