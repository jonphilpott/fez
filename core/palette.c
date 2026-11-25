/* The colours every FEZ screen uses: see palette.h. High-contrast: a pure black ground and bright, saturated inks. */
#include "palette.h"

const unsigned char FEZ_PALETTE[COL_COUNT][3] = {
  { 0x00, 0x00, 0x00 },   /* COL_BG    background */
  { 0xff, 0xff, 0xff },   /* COL_FG    normal text */
  { 0xbc, 0xc4, 0xd0 },   /* COL_DIM   quiet text */
  { 0xff, 0xd8, 0x00 },   /* COL_AMB   highlight (gold) */
  { 0x00, 0xff, 0xff },   /* COL_CYN   register names */
  { 0xff, 0x40, 0x40 },   /* COL_RED   errors */
  { 0x00, 0xff, 0x80 },   /* COL_GRN   notes, the scope beam */
  { 0x0c, 0x0e, 0x14 },   /* COL_PNL   panels */
  { 0x3c, 0x4a, 0x66 },   /* COL_SEL   selected row, empty bars */
  { 0x90, 0x68, 0x00 },   /* COL_AMBD  dark gold behind a selection */
  { 0x90, 0xa0, 0xb8 },   /* COL_LINE  thin lines */
  { 0xff, 0x40, 0xf0 }    /* COL_MAG   movement: modulation, playheads, activity, held keys, group headings */
};
