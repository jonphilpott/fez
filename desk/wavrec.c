/* WAV file writer: see wavrec.h.

   A WAV file is a 44-byte header followed by the raw samples. The header holds the total length, which we do not know until the
   recording ends, so we write the header with zero lengths first and come back to fill the real sizes in at stop. If the app
   crashes mid-take the sizes stay zero; most editors still open such a file, and the audio is all there.

   ponytail: the size fields are 32-bit, so a take stops being valid at 4 GB (about 3.9 hours of 48 kHz stereo). */
#include "wavrec.h"
#include <math.h>

/* Little-endian writers: a WAV file is little-endian whatever the machine is, so build the bytes by hand. */
static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned long v) { put16(p, (unsigned)(v & 0xFFFF)); put16(p + 2, (unsigned)((v >> 16) & 0xFFFF)); }

#define CHANNELS 2
#define BYTES_PER_SAMPLE 3                                        /* 24 bit */
#define FRAME_BYTES (CHANNELS * BYTES_PER_SAMPLE)

/* The 44-byte header for a file holding `frames` stereo frames. */
static void make_header(unsigned char h[44], int rate, unsigned long frames) {
  unsigned long data = frames * FRAME_BYTES;
  const char *riff = "RIFF", *wave = "WAVEfmt ", *dat = "data";
  for (int i = 0; i < 4; i++) h[i] = (unsigned char)riff[i];
  put32(h + 4, 36 + data);                                        /* file size minus the first 8 bytes */
  for (int i = 0; i < 8; i++) h[8 + i] = (unsigned char)wave[i];
  put32(h + 16, 16);                                              /* size of the fmt block */
  put16(h + 20, 1);                                               /* 1 = plain integer PCM */
  put16(h + 22, CHANNELS);
  put32(h + 24, (unsigned long)rate);
  put32(h + 28, (unsigned long)rate * FRAME_BYTES);               /* bytes per second */
  put16(h + 32, FRAME_BYTES);                                     /* bytes per frame */
  put16(h + 34, 8 * BYTES_PER_SAMPLE);                            /* bits per sample */
  for (int i = 0; i < 4; i++) h[36 + i] = (unsigned char)dat[i];
  put32(h + 40, data);
}

int wavrec_start(WavRec *w, const char *path, int rate) {
  unsigned char h[44];
  w->frames = 0; w->rate = rate;
  w->f = fopen(path, "wb");
  if (!w->f) return -1;
  make_header(h, rate, 0);
  if (fwrite(h, 1, sizeof h, w->f) != sizeof h) { fclose(w->f); w->f = NULL; return -1; }
  return 0;
}

int wavrec_write(WavRec *w, const float *lr, int frames) {
  unsigned char buf[512 * FRAME_BYTES];
  while (frames > 0) {
    int chunk = frames < 512 ? frames : 512;
    for (int i = 0; i < chunk; i++) {
      for (int c = 0; c < CHANNELS; c++) {                        /* left, then right */
        float x = lr[2 * i + c];
        if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;    /* a clipped peak must not wrap round to the opposite sign */
        long v = lrintf(x * 8388607.0f);                          /* 2^23 - 1: the largest 24-bit signed value */
        unsigned long u = (unsigned long)v & 0xFFFFFF;            /* two's complement, 24 bits */
        unsigned char *p = buf + i * FRAME_BYTES + c * BYTES_PER_SAMPLE;
        p[0] = (unsigned char)u; p[1] = (unsigned char)(u >> 8); p[2] = (unsigned char)(u >> 16);
      }
    }
    if (fwrite(buf, FRAME_BYTES, (size_t)chunk, w->f) != (size_t)chunk) return -1;
    w->frames += (unsigned long)chunk;
    lr += 2 * chunk; frames -= chunk;
  }
  return 0;
}

int wavrec_stop(WavRec *w) {
  unsigned char h[44];
  int ok = 1;
  if (!w->f) return -1;
  make_header(h, w->rate, w->frames);
  ok = fseek(w->f, 0, SEEK_SET) == 0 && fwrite(h, 1, sizeof h, w->f) == sizeof h;
  ok = fclose(w->f) == 0 && ok;
  w->f = NULL;
  return ok ? 0 : -1;
}
