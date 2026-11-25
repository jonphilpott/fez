/* SDL2 front end for the desktop app (macOS, Linux). It owns the window, the audio device and the keyboard.
   Everything musical is in core/, and every screen is in desk/ (plain C that draws into a pixel array).

   Threads: SDL calls audio_cb on its own thread, which calls fez_render. Anything that touches the synth from the
   main thread (keys, drawing) holds SDL_LockAudioDevice, which pauses the callback for the moment it takes.

   The window is drawn pixel by pixel into `pixels` and uploaded as one texture per frame. On a high-density
   display the window is measured in points, so SDL stretches the picture by a whole number and it stays sharp. */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desk.h"
#include "userpatch.h"
#include "wavrec.h"
#include <sys/stat.h>
#include <time.h>

static FezSynth synth;                    /* ~2 MB (the buffer device alone is 1.5 MB), so static rather than on the stack */
static Desk desk;
static SDL_AudioDeviceID dev;
static float scope_ring[SCOPE_RING];      /* the last SCOPE_RING output samples, for the scope tab */
static volatile unsigned scope_head;      /* total samples written; the newest is at (scope_head - 1) % SCOPE_RING */
static volatile float out_peak;           /* recent output level, for the scope's header */
/* The load meter. The audio thread times itself: how long one callback takes, divided by how long the buffer it filled lasts.
   At 1.0 the synth only just meets its deadline; above that the audio crackles. Published to the main thread as plain
   volatile numbers (a torn read of a float only ever shows a slightly stale meter, so no lock is needed). */
static volatile float cpu_avg, cpu_peak;
static volatile int clip_left;            /* samples left to show the CLIP flag; reset to one second at every full-scale sample */
#define CPU_SMOOTH 0.05                   /* weight of each new reading in the average: about a fifth of a second */
#define CPU_PEAK_HOLD 3.0                 /* seconds the worst reading is kept */
/* Recording the output. The audio thread must never touch the disk, so it drops each finished sample into a ring buffer and the
   main thread (which runs every 16 ms or so) empties the ring into the WAV file. The ring holds about 2.7 s, far more than the
   main loop ever lags. head is written only by the audio thread, tail only by the main thread, so no lock is needed. */
#define REC_RING (1 << 17)
static float rec_ring[REC_RING];
static SDL_atomic_t rec_head, rec_tail, rec_drops;
static volatile int rec_on;
static WavRec wav;
static char home_fez[400];                /* "~/FEZ/": where recordings and patches live ("" if there is no home folder) */
static float mute_gain = 1.0f;            /* 1 sounding, 0 muted; glides between so muting never clicks */
#define MUTE_STEP (1.0f / 960.0f)         /* a full fade takes 960 samples: 20 ms at 48 kHz */

static void audio_cb(void *user, Uint8 *stream, int len) {
  (void)user;
  Uint64 t0 = SDL_GetPerformanceCounter();                   /* for the load meter: see the end of this function */
  float *out = (float *)stream;
  int frames = len / (int)(2 * sizeof(float));               /* interleaved stereo */
  static float mono[8192];
  if (frames > 8192) frames = 8192;
  fez_render(&synth, mono, frames);
  /* Global mute (Shift+Esc). The synth keeps running underneath, so the delay tail and the loop carry on; only the output
     is faded. The gain moves one small step per sample towards its target, which is a 20 ms fade rather than a click. */
  float mute_target = desk.muted ? 0.0f : 1.0f;
  for (int i = 0; i < frames; i++) {
    if (mute_gain < mute_target) mute_gain = fminf(mute_gain + MUTE_STEP, mute_target);
    else if (mute_gain > mute_target) mute_gain = fmaxf(mute_gain - MUTE_STEP, mute_target);
    mono[i] *= mute_gain;
  }
  float pk = out_peak * 0.9f;                                /* the meter falls away between buffers */
  unsigned head = scope_head;
  for (int i = 0; i < frames; i++) {
    out[2 * i] = out[2 * i + 1] = mono[i];                   /* same signal in both ears */
    scope_ring[(head + (unsigned)i) & (SCOPE_RING - 1)] = mono[i];
    if (fabsf(mono[i]) > pk) pk = fabsf(mono[i]);
    if (fabsf(mono[i]) >= 0.99f) clip_left = FEZ_SAMPLE_RATE;
  }
  scope_head = head + (unsigned)frames;
  if (rec_on) {                                              /* what goes to the speakers is what goes to the file (mute fade included) */
    int h = SDL_AtomicGet(&rec_head), t = SDL_AtomicGet(&rec_tail);
    for (int i = 0; i < frames; i++) {
      if (h - t >= REC_RING) { SDL_AtomicAdd(&rec_drops, 1); continue; }   /* the main thread fell behind: lose the sample, count it */
      rec_ring[h & (REC_RING - 1)] = mono[i]; h++;
    }
    SDL_AtomicSet(&rec_head, h);
  }
  out_peak = pk;
  if (clip_left > 0) clip_left -= frames;

  /* Load meter: time spent / time available. The peak is held for a few seconds, then restarts from the current reading. */
  static double peak_left = 0, peak = 0;
  double avail = (double)frames / FEZ_SAMPLE_RATE;
  double load = (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency() / avail;
  cpu_avg = (float)(cpu_avg + (load - cpu_avg) * CPU_SMOOTH);
  peak_left -= avail;
  if (load >= peak || peak_left <= 0) { peak = load; peak_left = CPU_PEAK_HOLD; }
  cpu_peak = (float)peak;
}

/* Move whatever the audio thread has queued into the file. Called often from the main loop; returns 0 unless the disk failed. */
static int rec_drain(void) {
  int ok = 0, t = SDL_AtomicGet(&rec_tail), h = SDL_AtomicGet(&rec_head);
  while (t != h) {
    int n = h - t, start = t & (REC_RING - 1);
    if (n > REC_RING - start) n = REC_RING - start;          /* stop at the end of the ring; the next pass takes the wrapped part */
    if (wavrec_write(&wav, rec_ring + start, n) != 0) ok = -1;
    t += n;
  }
  SDL_AtomicSet(&rec_tail, t);
  return ok;
}

/* Start a take: ~/FEZ/recordings/2026-11-25_041230_<patch name>.wav (the seconds keep two quick takes apart). */
static void rec_start(void) {
  char dir[560], path[700], name[64];
  if (!home_fez[0]) { desk_say(&desk, "NO RECORDINGS FOLDER"); return; }
  snprintf(dir, sizeof dir, "%srecordings/", home_fez);
  time_t now = time(NULL); struct tm *tm = localtime(&now);
  size_t k = 0;
  for (const char *c = desk.loaded_name; *c && k < sizeof name - 1; c++) if (userpatch_name_char_ok((unsigned char)*c)) name[k++] = *c;   /* (blank) becomes blank */
  name[k] = '\0';
  char stamp[32]; strftime(stamp, sizeof stamp, "%Y-%m-%d_%H%M%S", tm);
  snprintf(path, sizeof path, "%s%s_%s.wav", dir, stamp, name);
  if (wavrec_start(&wav, path, FEZ_SAMPLE_RATE) != 0) { desk_say(&desk, "CANNOT CREATE THE RECORDING FILE"); return; }
  SDL_LockAudioDevice(dev);
  SDL_AtomicSet(&rec_head, 0); SDL_AtomicSet(&rec_tail, 0); SDL_AtomicSet(&rec_drops, 0);
  rec_on = 1;
  SDL_UnlockAudioDevice(dev);
  desk.recording = 1; desk.rec_secs = 0;
  snprintf(desk.msg, sizeof desk.msg, "RECORDING to %s_%s.wav (Ctrl+T stops)", stamp, name); desk.msg_until = desk.now_ms + 3000;
}

static void rec_stop(void) {
  SDL_LockAudioDevice(dev); rec_on = 0; SDL_UnlockAudioDevice(dev);     /* after this the audio thread no longer touches the ring */
  int bad = rec_drain();
  unsigned long frames = wav.frames; int drops = SDL_AtomicGet(&rec_drops);
  if (wavrec_stop(&wav) != 0) bad = -1;
  desk.recording = 0;
  int secs = (int)(frames / FEZ_SAMPLE_RATE);
  if (bad) snprintf(desk.msg, sizeof desk.msg, "RECORDING FAILED: DISK FULL OR UNWRITABLE");
  else snprintf(desk.msg, sizeof desk.msg, "SAVED %d:%02d in ~/FEZ/recordings%s", secs / 60, secs % 60, drops ? " (SOME SAMPLES LOST)" : "");
  desk.msg_until = desk.now_ms + 8000;
}

/* The system clipboard, for the editor's copy, cut and paste. */
static void clip_set(const char *text) { SDL_SetClipboardText(text); }
static int clip_get(char *buf, int max) {
  char *t = SDL_GetClipboardText();
  if (!t) return 0;
  snprintf(buf, (size_t)max, "%s", t);
  SDL_free(t);
  return (int)strlen(buf);
}

/* The keyboard device plays by where a key IS, not what it types, so a French or German keyboard has the same piano under
   the fingers. SDL scancodes name physical positions (named after a US keyboard). 0 means "not a note key". */
static int note_key_char(SDL_Scancode sc) {
  if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'a' + (int)(sc - SDL_SCANCODE_A);
  if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (int)(sc - SDL_SCANCODE_1);
  return 0;
}

/* SDL key codes to the desk's: specials get their own codes; letters and symbols stay as lower-case ASCII. */
static int translate_key(SDL_Keycode k) {
  switch (k) {
    case SDLK_UP: return KEY_UP;         case SDLK_DOWN: return KEY_DOWN;
    case SDLK_LEFT: return KEY_LEFT;     case SDLK_RIGHT: return KEY_RIGHT;
    case SDLK_RETURN: case SDLK_KP_ENTER: return KEY_ENTER;
    case SDLK_ESCAPE: return KEY_ESC;    case SDLK_BACKSPACE: return KEY_BACKSPACE;
    case SDLK_TAB: return KEY_TAB;       case SDLK_DELETE: return KEY_DELETE;
    case SDLK_F1: return KEY_F1;
    default: return k > 32 && k < 127 ? (int)k : KEY_NONE;
  }
}

static int translate_mods(Uint16 m) {
  return ((m & KMOD_SHIFT) ? MOD_SHIFT : 0) | ((m & KMOD_CTRL) ? MOD_CTRL : 0) | ((m & KMOD_ALT) ? MOD_ALT : 0);
}

int main(void) {
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
  /* 1024x600 is the Raspberry Pi LCD; the window can be resized, or taken full screen with Ctrl+F */
  SDL_Window *win = SDL_CreateWindow("FEZ", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 600, SDL_WINDOW_RESIZABLE);
  SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, 0) : NULL;
  if (!win || !ren) { fprintf(stderr, "window: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

  fez_init(&synth, (uint32_t)SDL_GetTicks() | 1);            /* seed the random generator from the clock */
  desk_init(&desk, &synth);
  desk.scope = scope_ring; desk.scope_head = &scope_head;
  desk.clip_set = clip_set; desk.clip_get = clip_get;
  {                                                           /* one visible folder: ~/FEZ/patches/ and ~/FEZ/recordings/ (easy to find, back up and share) */
    const char *home = getenv("HOME");
    if (home && home[0] && snprintf(home_fez, sizeof home_fez, "%s/FEZ/", home) < (int)sizeof home_fez) {
      char sub[512];
      mkdir(home_fez, 0755);
      snprintf(sub, sizeof sub, "%srecordings/", home_fez); mkdir(sub, 0755);
      snprintf(sub, sizeof sub, "%spatches/", home_fez);
      int fresh = mkdir(sub, 0755) == 0;                      /* 0 only if the folder did not exist yet */
      snprintf(desk.pat.dir, sizeof desk.pat.dir, "%s", sub);
      if (fresh) {                                            /* first run with this folder: copy patches saved in the old hidden one (kept, not deleted) */
        char *old = SDL_GetPrefPath("fez", "patches");        /* ~/Library/Application Support/fez/patches/ (macOS), ~/.local/share/fez/patches/ (Linux) */
        if (old) { userpatch_import(old, sub); SDL_free(old); }
      }
      desk_patches_refresh(&desk);
    }
  }

  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = FEZ_SAMPLE_RATE; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 512; want.callback = audio_cb;
  dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);       /* 0: SDL converts if the hardware differs from our request */
  if (!dev) { fprintf(stderr, "audio: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

  uint32_t *pixels = NULL; SDL_Texture *tex = NULL; int pw = 0, ph = 0;
  SDL_StartTextInput();                                      /* printable characters arrive as SDL_TEXTINPUT */
  SDL_PauseAudioDevice(dev, 0);                              /* start playing */

  Uint32 last_frame = SDL_GetTicks();
  while (!desk.quit) {
    SDL_Event ev;
    int got = SDL_WaitEventTimeout(&ev, 16);
    while (got) {
      if (ev.type == SDL_QUIT) desk.quit = 1;
      else if (ev.type == SDL_KEYDOWN) {                     /* (key repeats are wanted: held arrows keep stepping) */
        int mods = translate_mods(ev.key.keysym.mod), key = translate_key(ev.key.keysym.sym);
        int nk = note_key_char(ev.key.keysym.scancode), played = 0;
        if (nk && !(mods & (MOD_CTRL | MOD_ALT))) {            /* on the KEYS page a letter or digit plays a note (key repeats are swallowed there) */
          SDL_LockAudioDevice(dev);
          played = desk_note_key(&desk, nk, 1);
          SDL_UnlockAudioDevice(dev);
        }
        if (played) {
          /* a note key: nothing more to do (its text event will be ignored by the KEYS page) */
        } else if ((mods & MOD_CTRL) && key == 'f') {
          SDL_SetWindowFullscreen(win, (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        } else if (ev.key.repeat && key == KEY_ESC && (mods & MOD_SHIFT)) {
          /* a held Shift+Esc must not flicker the mute on and off */
        } else if (key != KEY_NONE && (key >= 256 || (mods & (MOD_CTRL | MOD_ALT)))) {
          /* Plain letters arrive as text instead, so shifted and non-US keyboards just work. */
          SDL_LockAudioDevice(dev);
          desk_key(&desk, key, mods);
          SDL_UnlockAudioDevice(dev);
        }
      } else if (ev.type == SDL_KEYUP) {
        int nk = note_key_char(ev.key.keysym.scancode);
        if (nk) { SDL_LockAudioDevice(dev); desk_note_key(&desk, nk, 0); SDL_UnlockAudioDevice(dev); }
      } else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        SDL_LockAudioDevice(dev); desk_all_notes_off(&desk); SDL_UnlockAudioDevice(dev);   /* the key-ups will never arrive: do not leave a note hanging */
      } else if (ev.type == SDL_TEXTINPUT) {
        const char *t = ev.text.text;
        SDL_LockAudioDevice(dev);
        for (; *t; t++) if ((unsigned char)*t >= 32 && (unsigned char)*t < 127) desk_text(&desk, *t);
        SDL_UnlockAudioDevice(dev);
      }
      got = SDL_PollEvent(&ev);
    }

    if (desk.rec_toggle) { desk.rec_toggle = 0; if (desk.recording) rec_stop(); else rec_start(); }
    if (desk.recording) {
      if (rec_drain() != 0) rec_stop();                       /* the disk failed: stop rather than keep losing audio */
      else desk.rec_secs = (double)wav.frames / FEZ_SAMPLE_RATE;
    }

    Uint32 now = SDL_GetTicks();
    if (now - last_frame < 16) continue;

    int w, h;
    SDL_GetWindowSize(win, &w, &h);
    if (w != pw || h != ph) {                                /* new window size: new picture, new texture */
      free(pixels); if (tex) SDL_DestroyTexture(tex);
      pw = w; ph = h;
      pixels = malloc((size_t)pw * (size_t)ph * sizeof *pixels);
      tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pw, ph);
      if (!pixels || !tex) { fprintf(stderr, "out of memory for a %dx%d picture\n", pw, ph); break; }
      desk_set_canvas(&desk, pixels, pw, ph);
    }
    SDL_LockAudioDevice(dev);
    desk.peak = out_peak; desk.cpu = cpu_avg; desk.cpu_peak = cpu_peak; desk.clip = clip_left > 0;
    desk_tick(&desk, (double)(now - last_frame));
    desk_draw(&desk);
    SDL_UnlockAudioDevice(dev);
    last_frame = now;
    SDL_UpdateTexture(tex, NULL, pixels, pw * (int)sizeof *pixels);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
  }

  if (desk.recording) rec_stop();                             /* close the file properly so its header is right */
  SDL_CloseAudioDevice(dev);
  free(pixels); if (tex) SDL_DestroyTexture(tex);
  SDL_DestroyRenderer(ren); SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
