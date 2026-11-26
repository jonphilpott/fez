/* MIDI input from the operating system (midi.c): list the inputs, listen to one, and collect its bytes. Each system has
   its own way in (CoreMIDI on macOS, the raw MIDI device files on Linux, winmm on Windows); this header hides which.
   Only main.c uses it. What the bytes mean is the desk's business (desk_midi_bytes in setup.c). */
#ifndef FEZ_MIDI_H
#define FEZ_MIDI_H

#include "desk.h"           /* SETUP_NAME */

int  midi_list(char names[][SETUP_NAME], int max);   /* the inputs there are now; returns how many */
int  midi_open(const char *name);                    /* listen to the input of that name (closing any other); 0 ok, -1 not found or failed */
void midi_close(void);
int  midi_read(unsigned char *buf, int max);         /* the bytes that arrived since the last call; returns how many (0 if none) */

#endif
