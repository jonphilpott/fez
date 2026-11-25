/* Writing the recording to disk as a WAV file: 24-bit PCM, two channels. Plain stdio; the only recording code that touches the disk.
   The audio thread never calls this (disk writes can stall for many milliseconds). main.c passes it samples from a ring buffer. */
#ifndef FEZ_WAVREC_H
#define FEZ_WAVREC_H
#include <stdio.h>

typedef struct { FILE *f; unsigned long frames; int rate; } WavRec;

int  wavrec_start(WavRec *w, const char *path, int rate);          /* 0 ok, -1 could not create the file */
int  wavrec_write(WavRec *w, const float *mono, int n);            /* n mono samples, written to both channels; 0 ok, -1 write failed (disk full) */
int  wavrec_stop(WavRec *w);                                       /* fix the header sizes and close; 0 ok, -1 failed */

#endif
