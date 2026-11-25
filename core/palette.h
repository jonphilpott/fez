/* The colours every FEZ screen uses (handheld and desktop), so the two look like one instrument.
   Screens store a colour as an index into this table; the platform turns an index into pixels. */
#ifndef FEZ_PALETTE_H
#define FEZ_PALETTE_H

enum { COL_BG, COL_FG, COL_DIM, COL_AMB, COL_CYN, COL_RED, COL_GRN, COL_PNL, COL_SEL, COL_AMBD, COL_LINE, COL_MAG, COL_COUNT };

extern const unsigned char FEZ_PALETTE[COL_COUNT][3];   /* red, green, blue */

#endif
