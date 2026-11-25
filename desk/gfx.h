/* Software drawing for the desktop app: a canvas of 0xAARRGGBB pixels in memory, and a few primitives.
   Nothing here knows about SDL; the platform layer uploads the canvas to the screen once per frame.

   Text uses the same 5x8 bitmap font as the handheld UI (FONT5), drawn at a whole-number scale: at scale 2 a
   character cell is 10 pixels wide and 18 high (8 rows of glyph plus one blank row, times 2). */
#ifndef FEZ_GFX_H
#define FEZ_GFX_H

#include <stdint.h>

#define GFX_CELL_W 5            /* pixels per character across, at scale 1 */
#define GFX_CELL_H 9            /* pixels per text row, at scale 1 */

typedef struct {
  uint32_t *px;                 /* w * h pixels, row by row */
  int w, h;
  int clip_x0, clip_y0, clip_x1, clip_y1;   /* drawing outside this box is ignored (x1, y1 are exclusive) */
} Canvas;

uint32_t gfx_colour(int palette_index);     /* a COL_ constant from palette.h as a pixel value */

uint32_t gfx_mix(uint32_t a, uint32_t b, double t);   /* blend two colours: t = 0 gives a, t = 1 gives b */

void gfx_clip(Canvas *c, int x, int y, int w, int h);   /* limit drawing to a box (also kept inside the canvas) */
void gfx_unclip(Canvas *c);
void gfx_fill(Canvas *c, int x, int y, int w, int h, uint32_t rgb);
void gfx_frame(Canvas *c, int x, int y, int w, int h, uint32_t rgb);        /* 1 pixel outline */
void gfx_line(Canvas *c, int x0, int y0, int x1, int y1, uint32_t rgb);
void gfx_text(Canvas *c, int x, int y, const char *s, int scale, uint32_t rgb);

#endif
