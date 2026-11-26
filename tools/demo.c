/* Makes the 30 second portrait demo video (1080x1920, for Instagram): ./demo OUT_DIR [audio]
   It plays a scripted performance on the real synth and the real desktop UI, offline and in lockstep: for every video frame it
   renders the matching 1/30 s of audio, types the next few characters into the editor, and draws the screen. Nothing is faked:
   what you see typed is what makes the sound, and the tiles show the live state.
   Output in OUT_DIR: frames go straight to ffmpeg (video.mp4), audio.wav is written at the end, and ffmpeg joins them (demo.mp4).
   `./demo OUT_DIR audio` skips the pictures (fast) and prints the level of each second, for tuning the music.

   The timeline is a list of steps (see add_*): each has a time in seconds. Typing is spread over time at a given speed. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desk.h"

#define W 1080
#define H 1920
#define FPS 30
#define SECONDS 30
#define FRAMES (FPS * SECONDS)
#define SCOPE_GAIN 5.0f                              /* display gain for the scope tile (the sound is untouched) */
#define SPF (FEZ_SAMPLE_RATE / FPS)                 /* audio samples per video frame: 1600 at 48 kHz */

static FezSynth synth;
static Desk desk;
static float ring[2 * SCOPE_RING];
static unsigned head;
static uint32_t px[W * H];
static float audio[2 * SPF * FRAMES];

/* ---- the piece: every register that shapes the sound is here, set before the performance starts. Nothing is audible until the
   typed code runs (no pulser fires, the drone and the buffer return are at zero), so the screen and the sound arrive together. ---- */
static const char *PIECE =
  "FEZ PATCH 1\n"
  "name Demo\n"
  "level 16384\n"
  "reg PITCH 15360\n"
  /* a soft sine: no FM, little folding, a long attack and a very long release (about 3.5 s) */
  "reg FOLD 1500\n"
  "reg CUTOFF 12000\n"
  "reg ATK 32000\n"
  "reg REL 32000\n"
  "reg LFORATE 4000\n"
  "reg FMAMT 0\n"
  "reg CXBLEND 0\n"
  "reg MIXCX 14000\n"
  /* a roomy, dark delay loop of about 1.8 s, and a long spring */
  "reg DLYTIME 5000\n"
  "reg DLYFB 11000\n"
  "reg DLYMIX 6000\n"
  "reg DLYLP 18000\n"
  "reg SPRMIX 14000\n"
  "reg SPRDEC 24000\n"
  /* the mod voice is a dark bed of noise, silent until the drone line is typed */
  "reg MDNZ 32767\n"
  "reg MDCUT 9000\n"
  "reg MDLVL 0\n"
  /* the buffer: 8 s long, four read voices at unrelated distances behind the write head, silent until its return is raised */
  "reg MIXBF 0\n"
  "reg BWFB 12000\n"
  "reg BWSRC 1\n"
  "reg BUFMS 8000\n"
  "reg GPOS0 11877\n" "reg GPOS1 17611\n" "reg GPOS2 24984\n" "reg GPOS3 31535\n"
  "reg GLVL0 11000\n" "reg GLVL1 11000\n" "reg GLVL2 11000\n" "reg GLVL3 11000\n"
  "reg GFLG0 1\n" "reg GFLG1 1\n" "reg GFLG2 1\n" "reg GFLG3 1\n"
  "lz 4000 15300 4\n";

/* ---- the timeline ---- */
enum { S_BLOCK, S_TYPE, S_APPLY, S_RUN, S_TAB, S_DEVPAGE, S_PULSER, S_REG, S_CAPTION };
typedef struct { double t; int kind; int a, b; const char *s; double cps; } Step;
static Step steps[200]; static int nsteps; static int done[200];

static void add(double t, int kind, int a, int b, const char *s, double cps) {
  steps[nsteps] = (Step){ t, kind, a, b, s, cps }; nsteps++;
}
/* Type `text` into block `blk` starting at t, then apply it. Returns the time when applied (so the next step can follow). */
static double add_typed(double t, int blk, const char *text, double cps) {
  add(t, S_BLOCK, blk, 0, NULL, 0);
  add(t, S_TYPE, 0, 0, text, cps);
  double end = t + (double)strlen(text) / cps + 0.35;
  add(end, S_APPLY, 0, 0, NULL, 0);
  return end;
}

/* the typist: one string being spread over time */
static const char *typing; static double type_t0, type_cps; static int typed_n;

static const char *caption1;

static void goto_block(int blk) {
  while (desk.ed.blk < blk) desk_key(&desk, '.', MOD_CTRL);
  while (desk.ed.blk > blk) desk_key(&desk, ',', MOD_CTRL);
}

static void do_step(const Step *s, double t) {
  switch (s->kind) {
    case S_BLOCK:  goto_block(s->a); break;
    case S_TYPE:   typing = s->s; type_t0 = t; type_cps = s->cps; typed_n = 0; break;
    case S_APPLY:  desk_key(&desk, KEY_ENTER, MOD_CTRL); break;           /* Ctrl+Enter: compile the block into the synth */
    case S_RUN:    desk_key(&desk, 'r', MOD_CTRL); break;                 /* Ctrl+R: run it once */
    case S_TAB:    desk.layouts[0].tab[1] = s->a; break;
    case S_DEVPAGE: desk.dev.sub = s->a; break;
    case S_PULSER: synth.pulsers[s->a].on = 1; synth.pulsers[s->a].rate = s->b; break;
    case S_REG:    fez_reg_write(&synth, s->a, s->b); break;
    case S_CAPTION: caption1 = s->s; break;
  }
}

/* ---- captions, drawn over the bottom band ---- */
#define AMBER 0xffd21a
#define DIM   0xb0b0b0
static void centre(int y, const char *s, int scale, uint32_t rgb) {
  int w = (int)strlen(s) * GFX_CELL_W * scale;
  gfx_text(&desk.cv, (W - w) / 2, y, s, scale, rgb);
}
static void draw_caption(double t) {
  gfx_fill(&desk.cv, 0, 1770, W, H - 1770, 0x000000);
  gfx_fill(&desk.cv, 0, 1770, W, 2, 0x303030);
  if (t < 3.0) {                                     /* the title */
    centre(1790, "FEZ", 10, AMBER);
    centre(1880, "Personal Sound Computer", 4, DIM);
  } else if (t >= 27.0) {                            /* the end: name and tag line */
    centre(1782, "FEZ", 7, AMBER);
    centre(1850, "a forth programmable", 4, DIM);
    centre(1884, "virtual analog synth engine", 4, DIM);
  } else if (caption1) {
    centre(1820, caption1, 5, AMBER);
  }
}

static void wav_write(const char *path, const float *a, int frames, float gain) {
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); exit(1); }
  unsigned data = (unsigned)frames * 4, rate = FEZ_SAMPLE_RATE;
  unsigned char h[44] = { 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,0,0,0,0,0,0,0,0,4,0,16,0,'d','a','t','a',0,0,0,0 };
  unsigned riff = 36 + data, br = rate * 4;
  memcpy(h + 4, &riff, 4); memcpy(h + 24, &rate, 4); memcpy(h + 28, &br, 4); memcpy(h + 40, &data, 4);
  fwrite(h, 1, 44, f);
  for (int i = 0; i < frames * 2; i++) {
    float v = a[i] * gain; if (v > 1) v = 1; if (v < -1) v = -1;
    short s = (short)lrintf(v * 32767.0f); fwrite(&s, 2, 1, f);
  }
  fclose(f);
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: demo OUT_DIR [audio]\n"); return 1; }
  const char *dir = argv[1]; int audio_only = argc > 2 && !strcmp(argv[2], "audio");
  char path[600];

  /* ---- the timeline ---- */
  const char *BELLS = "\\ a slow note\n5 RND 5 + 13 COLOUR C2 8 ST + + PITCH !\n1 GATE !";
  const char *LOOP  = "\\ let the loop remember\n25000 DLYFB !\n20000 DLYMIX !";
  const char *CHAOS = "\\ a dark bed, moved by chaos\n13000 MDLVL !\nLZX MDCUT 9000 ROUTE\nLZZ CUTOFF 5000 ROUTE";
  const char *PAST  = "\\ the past comes back\n20000 MIXBF !";
  add(0.0, S_TAB, TAB_CTRL, 0, NULL, 0);
  add(3.0, S_CAPTION, 0, 0, "slow notes, written as code", 0);
  double t = add_typed(0.3, 0, BELLS, 34);                      /* bells */
  add(t + 0.05, S_REG, REG_BP0, 5, NULL, 0);                      /* block 0 listens to pulsers 0 and 2 */
  add(t + 0.1, S_RUN, 0, 0, NULL, 0);                              /* the first note at once; the pulsers carry on from there */
  add(t + 0.2, S_PULSER, 0, 40, NULL, 0);                          /* a note about every 1.5 s ... */
  add(t + 0.9, S_PULSER, 2, 28, NULL, 0);                          /* ... and another about every 2.1 s */
  add(9.0, S_CAPTION, 0, 0, "a tape loop that remembers", 0);
  add(9.0, S_TAB, TAB_DEVICES, 0, NULL, 0); add(9.0, S_DEVPAGE, DEV_DELAY, 0, NULL, 0);
  t = add_typed(9.3, 1, LOOP, 30);
  add(t + 0.05, S_RUN, 0, 0, NULL, 0);
  add(15.0, S_CAPTION, 0, 0, "chaos moves the filter", 0);
  add(15.0, S_DEVPAGE, DEV_LORENZ, 0, NULL, 0);
  t = add_typed(15.3, 2, CHAOS, 30);
  add(t + 0.05, S_RUN, 0, 0, NULL, 0);
  add(21.0, S_CAPTION, 0, 0, "the past comes back", 0);
  add(21.0, S_TAB, TAB_BUFFER, 0, NULL, 0);
  t = add_typed(21.3, 3, PAST, 30);
  add(t + 0.05, S_RUN, 0, 0, NULL, 0);
  add(25.0, S_TAB, TAB_MATRIX, 0, NULL, 0);

  /* ---- set up the synth and the screen ---- */
  fez_init(&synth, 7);
  desk_init(&desk, &synth);                    /* desk_init starts from a blank patch, so the piece is loaded after it */
  char err[200]; PatchInfo info;
  if (patchfile_read(&synth, PIECE, &info, err, sizeof err) != 0) { fprintf(stderr, "piece: %s\n", err); return 1; }
  desk.scope = ring; desk.scope_head = &head;
  desk_reload_editor(&desk);
  desk_set_canvas(&desk, px, W, H);
  desk_set_scale(&desk, 5);                 /* big text: it is read on a phone */
  /* one portrait layout: the editor on top, a changing tile in the middle, the scope underneath */
  Layout *L = &desk.layouts[0];
  L->ntiles = 3;
  L->rect[0] = (Frac){ 0, 0, 1000, 300 };   L->tab[0] = TAB_EDITOR;
  L->rect[1] = (Frac){ 0, 300, 1000, 740 }; L->tab[1] = TAB_CTRL;
  L->rect[2] = (Frac){ 0, 740, 1000, 920 }; L->tab[2] = TAB_SCOPE;
  desk.layout = 0; desk.focus = 0;
  desk_set_canvas(&desk, px, W, H);          /* again: lays the tiles out */
  for (int i = 0; i < NUM_PULSERS; i++) synth.pulsers[i].on = 0;
  desk.now_ms = 1000;

  FILE *vid = NULL;
  if (!audio_only) {
    snprintf(path, sizeof path, "ffmpeg -y -loglevel error -f rawvideo -pixel_format bgra -video_size %dx%d -framerate %d -i - "
             "-c:v libx264 -preset slow -crf 16 -pix_fmt yuv420p -movflags +faststart %s/video.mp4", W, H, FPS, dir);
    vid = popen(path, "w");
    if (!vid) { perror("ffmpeg"); return 1; }
  }

  /* ---- play it ---- */
  for (int f = 0; f < FRAMES; f++) {
    double T = (double)f / FPS;
    for (int i = 0; i < nsteps; i++) if (!done[i] && steps[i].t <= T) { done[i] = 1; do_step(&steps[i], T); }
    if (typing) {                                       /* feed the characters that are due */
      int want = (int)((T - type_t0) * type_cps), n = (int)strlen(typing);
      if (want > n) want = n;
      while (typed_n < want) { desk_text(&desk, typing[typed_n] == '\n' ? '\n' : typing[typed_n]); typed_n++; }
      if (typed_n >= n) typing = NULL;
    }
    float *out = audio + (size_t)f * 2 * SPF;
    fez_render_stereo(&synth, out, SPF);
    float pk = 0;
    for (int k = 0; k < SPF; k++) {
      unsigned slot = (head + (unsigned)k) & (SCOPE_RING - 1);
      ring[2 * slot] = out[2 * k] * SCOPE_GAIN; ring[2 * slot + 1] = out[2 * k + 1] * SCOPE_GAIN;   /* the scope only: quiet music would draw a flat line */
      float a = fabsf(out[2 * k]); if (a > pk) pk = a;
    }
    head += SPF;
    desk.peak = pk;
    desk.now_ms += 1000.0 / FPS;
    desk.ed.last_key_ms = desk.now_ms;                  /* keep the editor's cursor solid while typing */
    desk_tick(&desk, 1000.0 / FPS);
    if (audio_only) {
      if (f % FPS == FPS - 1) { double s = 0; float mx = 0; for (int k = 0; k < SPF * FPS * 2; k++) { float a = audio[(size_t)(f - FPS + 1) * 2 * SPF + k]; s += (double)a * a; if (fabsf(a) > mx) mx = fabsf(a); }
        printf("%2ds  rms %.3f  peak %.3f\n", (f + 1) / FPS, sqrt(s / (SPF * FPS * 2)), mx); }
      continue;
    }
    desk_draw(&desk);
    draw_caption(T);
    fwrite(px, sizeof px[0], (size_t)W * H, vid);
  }
  if (vid) pclose(vid);

  /* ---- the sound: fade the last 1.5 s, scale the peak to about -1 dB, write the file, and join ---- */
  int total = SPF * FRAMES, fade = FEZ_SAMPLE_RATE * 3 / 2;
  for (int i = 0; i < fade; i++) { float g = (float)i / fade; g = 1 - g; for (int c = 0; c < 2; c++) audio[2 * (total - fade + i) + c] *= g; }
  /* loudness: phones are small and Instagram is loud, so aim for an overall RMS of 0.14 (about -17 dB) and let a soft clip
     (tanh) round off the bell transients that would otherwise stick out. */
  double sum = 0; int cnt = 0;
  for (int i = FEZ_SAMPLE_RATE * 2 * 3; i < total * 2; i++) { sum += (double)audio[i] * audio[i]; cnt++; }
  float gain = (float)(0.14 / sqrt(sum / cnt));
  for (int i = 0; i < total * 2; i++) audio[i] = tanhf(audio[i] * gain);
  gain = 1;
  printf("rms gain %.2f\n", 0.14 / sqrt(sum / cnt));
  snprintf(path, sizeof path, "%s/audio.wav", dir);
  wav_write(path, audio, total, gain);
  if (!audio_only) {
    snprintf(path, sizeof path, "ffmpeg -y -loglevel error -i %s/video.mp4 -i %s/audio.wav -c:v copy -c:a aac -b:a 192k -shortest %s/demo.mp4", dir, dir, dir);
    if (system(path) != 0) return 1;
    printf("wrote %s/demo.mp4\n", dir);
  }
  return 0;
}
