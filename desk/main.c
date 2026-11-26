/* SDL2 front end for the desktop app (macOS, Linux). It owns the window, the audio device and the keyboard.
   Everything musical is in core/, and every screen is in desk/ (plain C that draws into a pixel array).

   Threads: SDL calls audio_cb on its own thread, which calls fez_render. Anything that CHANGES the synth from the main
   thread (keys, buttons, sticks, the per-frame tick) holds SDL_LockAudioDevice, which pauses the callback for the moment it
   takes. Drawing only READS the synth and runs without the lock, so the sound never waits for the screen (see the main loop).

   The window is drawn pixel by pixel into `pixels` and uploaded as one texture per frame. On a high-density
   display the window is measured in points, so SDL stretches the picture by a whole number and it stays sharp.

   `fez --brick` runs the handheld version: a fixed 256x192 picture scaled up, the same screens, and the controls of the
   TrimUI Brick (see pad.c). A game controller works; so does the keyboard, standing in for the buttons (see key_to_button). */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desk.h"
#include "pad.h"
#include "userpatch.h"
#include "wavrec.h"
#include "midi.h"
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)      /* Windows' mkdir takes no permission bits */
#endif
#include <time.h>

static FezSynth synth;                    /* about 8 MB (the buffer device is 5.8 MB, the delay 2.3 MB), so static rather than on the stack */
static Desk desk;
static Pad pad;
static int brick_mode;                    /* --brick: the handheld UI at 256x192, driven by buttons */
static SDL_AudioDeviceID dev;
static float scope_ring[2 * SCOPE_RING];  /* the last SCOPE_RING output frames, interleaved left and right, for the scope tab */
static volatile unsigned scope_head;      /* total frames written; the newest is at (scope_head - 1) % SCOPE_RING */
static volatile float out_peak;           /* recent output level, for the scope's header */
/* The load meter. The audio thread times itself: how long one callback takes, divided by how long the buffer it filled lasts.
   At 1.0 the synth only just meets its deadline; above that the audio crackles. Published to the main thread as plain
   volatile numbers (a torn read of a float only ever shows a slightly stale meter, so no lock is needed). */
static volatile float cpu_avg, cpu_peak;
static volatile int clip_left;            /* samples left to show the CLIP flag; reset to one second at every full-scale sample */
#define CPU_SMOOTH 0.05                   /* weight of each new reading in the average: about a fifth of a second */
#define CPU_PEAK_HOLD 3.0                 /* seconds the worst reading is kept */
/* Recording the output (stereo). The audio thread must never touch the disk, so it drops each finished sample into a ring buffer and the
   main thread (which runs every 16 ms or so) empties the ring into the WAV file. The ring holds about 1.4 s of stereo, far more than the
   main loop ever lags. head is written only by the audio thread, tail only by the main thread, so no lock is needed. */
#define REC_RING (1 << 17)
static float rec_ring[REC_RING];                            /* interleaved left, right: a frame is two entries, so head and tail always move by 2 */
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
  int frames = len / (int)(2 * sizeof(float));               /* interleaved stereo: left, right, left, right ... */
  if (frames > 8192) frames = 8192;
  fez_render_stereo(&synth, out, frames);
  /* Global mute (Shift+Esc). The synth keeps running underneath, so the delay tail and the loop carry on; only the output
     is faded. The gain moves one small step per sample towards its target, which is a 20 ms fade rather than a click.
     The same gain applies to both ears. */
  float mute_target = desk.muted ? 0.0f : 1.0f;
  float pk = out_peak * 0.9f;                                /* the meter falls away between buffers */
  unsigned head = scope_head;
  for (int i = 0; i < frames; i++) {
    if (mute_gain < mute_target) mute_gain = fminf(mute_gain + MUTE_STEP, mute_target);
    else if (mute_gain > mute_target) mute_gain = fmaxf(mute_gain - MUTE_STEP, mute_target);
    float l = out[2 * i] * mute_gain, r = out[2 * i + 1] * mute_gain;
    out[2 * i] = l; out[2 * i + 1] = r;
    unsigned slot = (head + (unsigned)i) & (SCOPE_RING - 1);
    scope_ring[2 * slot] = l; scope_ring[2 * slot + 1] = r;
    float loud = fmaxf(fabsf(l), fabsf(r));                  /* the meters follow the louder ear */
    if (loud > pk) pk = loud;
    if (loud >= 0.99f) clip_left = FEZ_SAMPLE_RATE;
  }
  scope_head = head + (unsigned)frames;
  if (rec_on) {                                              /* what goes to the speakers is what goes to the file (mute fade included) */
    int h = SDL_AtomicGet(&rec_head), t = SDL_AtomicGet(&rec_tail);
    for (int i = 0; i < frames; i++) {
      if (h - t >= REC_RING) { SDL_AtomicAdd(&rec_drops, 1); continue; }   /* the main thread fell behind: lose the frame, count it */
      rec_ring[h & (REC_RING - 1)] = out[2 * i]; rec_ring[(h + 1) & (REC_RING - 1)] = out[2 * i + 1];
      h += 2;
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
    if (wavrec_write(&wav, rec_ring + start, n / 2) != 0) ok = -1;                 /* n counts floats; a frame is two */
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
  else snprintf(desk.msg, sizeof desk.msg, "SAVED %d:%02d in the FEZ/recordings folder%s", secs / 60, secs % 60, drops ? " (SOME SAMPLES LOST)" : "");
  desk.msg_until = desk.now_ms + 8000;
}

/* ---------------------------------------------------------------- the audio device and the F2 setup page

   The output device is opened by name (the one chosen on the F2 page), or SDL's default when the name is "" or that device
   is not there. Reopening closes the old one first; the synth carries on as if nothing happened. */
static double buffer_secs;                    /* how long one audio buffer lasts (512 frames: 10.7 ms), for the load meter */

static int open_audio(const char *name) {
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = FEZ_SAMPLE_RATE; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 512; want.callback = audio_cb;
  if (dev) SDL_CloseAudioDevice(dev);
  dev = name && name[0] ? SDL_OpenAudioDevice(name, 0, &want, &have, 0) : 0;   /* 0 for "allowed changes": SDL converts if the hardware differs from our request */
  int ok = dev != 0 || !(name && name[0]);
  if (!dev) dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);                /* not there (unplugged?): the system's default */
  if (!dev) return -1;
  buffer_secs = (double)have.samples / have.freq;
  SDL_PauseAudioDevice(dev, 0);                                                /* start playing */
  return ok ? 0 : 1;                                                           /* 1: playing, but on the default instead */
}

static char setup_path[440];                  /* ~/FEZ/setup.txt ("" if there is no home folder) */

static void setup_save(void) {
  if (!setup_path[0]) return;
  char text[2048];
  int n = setup_write(&desk.setup, text, sizeof text);
  FILE *f = fopen(setup_path, "wb");
  if (!f || fwrite(text, 1, (size_t)n, f) != (size_t)n) desk_say(&desk, "CANNOT SAVE SETUP.TXT");
  if (f) fclose(f);
}

static void setup_load(void) {
  char text[2048] = "";
  FILE *f = setup_path[0] ? fopen(setup_path, "rb") : NULL;
  if (!f) return;                                                              /* first run: the defaults stand */
  size_t n = fread(text, 1, sizeof text - 1, f);
  text[n] = '\0';
  fclose(f);
  setup_read(&desk.setup, text);
}

/* The F2 page asks for the device lists when it opens ... */
static void setup_scan(Desk *d) {
  int n = SDL_GetNumAudioDevices(0);                                           /* 0: outputs (1 would be inputs) */
  d->n_audio = 0;
  for (int i = 0; i < n && d->n_audio < SETUP_DEVS; i++) {
    const char *name = SDL_GetAudioDeviceName(i, 0);
    if (name) snprintf(d->audio_names[d->n_audio++], SETUP_NAME, "%s", name);
  }
  d->n_midi = midi_list(d->midi_names, SETUP_DEVS);
}

/* ... and tells us when something changed. The page is drawn and driven with the audio locked (desk_key), but the device
   must be closed and opened without the lock held, because closing waits for the audio thread to finish. */
static int setup_pending = -1;                /* what to apply after the key has been handled, or -1 */
static void setup_apply(Desk *d, int what) { (void)d; if (setup_pending < what) setup_pending = what; }

static void setup_do(void) {
  int what = setup_pending;
  setup_pending = -1;
  if (what == SETUP_AUDIO) {
    int r = open_audio(desk.setup.audio);
    if (r < 0) { fprintf(stderr, "audio: %s\n", SDL_GetError()); desk.quit = 1; return; }
    desk_say(&desk, r ? "THAT OUTPUT IS NOT THERE: USING THE DEFAULT" : "OUTPUT CHANGED");
  } else if (what == SETUP_MIDI) {
    if (!desk.setup.midi[0]) { midi_close(); desk_say(&desk, "MIDI OFF"); }
    else desk_say(&desk, midi_open(desk.setup.midi) == 0 ? "LISTENING TO MIDI" : "CANNOT OPEN THAT MIDI INPUT");
  }
  setup_save();
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
    case SDLK_F1: return KEY_F1;         case SDLK_F2: return KEY_F2;
    default: return k > 32 && k < 127 ? (int)k : KEY_NONE;
  }
}

static int translate_mods(Uint16 m) {
  return ((m & KMOD_SHIFT) ? MOD_SHIFT : 0) | ((m & KMOD_CTRL) ? MOD_CTRL : 0) | ((m & KMOD_ALT) ? MOD_ALT : 0);
}

/* The Brick's buttons on a computer keyboard, for working on the handheld UI at a desk. BTN_COUNT means "not a button".
   Layout: arrows = D-pad; X Z = A B (X is the right-hand one, as on the device); S A = X Y; Q W = L1 R1; E R = L2 R2;
   Return = Start; Shift = Select; Esc = Menu; - and = volume. */
static Button key_to_button(SDL_Keycode k) {
  switch (k) {
    case SDLK_UP: return BTN_UP;       case SDLK_DOWN: return BTN_DOWN;
    case SDLK_LEFT: return BTN_LEFT;   case SDLK_RIGHT: return BTN_RIGHT;
    case SDLK_x: return BTN_A;         case SDLK_z: return BTN_B;
    case SDLK_s: return BTN_X;         case SDLK_a: return BTN_Y;
    case SDLK_q: return BTN_L1;        case SDLK_w: return BTN_R1;
    case SDLK_e: return BTN_L2;        case SDLK_r: return BTN_R2;
    case SDLK_RETURN: return BTN_START;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return BTN_SELECT;
    case SDLK_ESCAPE: return BTN_MENU;
    case SDLK_EQUALS: case SDLK_VOLUMEUP: return BTN_VOLU;
    case SDLK_MINUS: case SDLK_VOLUMEDOWN: return BTN_VOLD;
    default: return BTN_COUNT;
  }
}

/* A game controller's buttons (the Brick shows up as one). SDL names buttons by position on an Xbox pad: A is the bottom
   one. The Brick is laid out like a Nintendo pad (A right, B bottom, X top, Y left), so its A and B arrive swapped. Measured on
   a real Brick, 25 Nov 2026 (A and B; X and Y assumed to follow). Swapping A with B and X with Y is therefore the default;
   FEZ_PAD_SWAP=0 turns it off for a pad with the Xbox layout. */
static Button controller_to_button(Uint8 b) {
  static int swap = -1;
  if (swap < 0) { const char *e = getenv("FEZ_PAD_SWAP"); swap = !(e && e[0] == '0'); }
  switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return BTN_UP;       case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return BTN_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return BTN_LEFT;   case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return BTN_RIGHT;
    case SDL_CONTROLLER_BUTTON_A: return swap ? BTN_B : BTN_A;  case SDL_CONTROLLER_BUTTON_B: return swap ? BTN_A : BTN_B;
    case SDL_CONTROLLER_BUTTON_X: return swap ? BTN_Y : BTN_X;  case SDL_CONTROLLER_BUTTON_Y: return swap ? BTN_X : BTN_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return BTN_L1;  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return BTN_R1;
    case SDL_CONTROLLER_BUTTON_START: return BTN_START;      case SDL_CONTROLLER_BUTTON_BACK: return BTN_SELECT;
    case SDL_CONTROLLER_BUTTON_GUIDE: return BTN_MENU;
    default: return BTN_COUNT;
  }
}

int main(int argc, char **argv) {
  brick_mode = argc > 1 && !strcmp(argv[1], "--brick");
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
  /* 1024x600 is the Raspberry Pi LCD; the window can be resized, or taken full screen with Ctrl+F */
  SDL_Window *win = brick_mode ? SDL_CreateWindow("FEZ", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 768, SDL_WINDOW_RESIZABLE)    /* 4 x 256x192 */
                               : SDL_CreateWindow("FEZ", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 600, SDL_WINDOW_RESIZABLE);
  SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, 0) : NULL;
  if (!win || !ren) { fprintf(stderr, "window: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
  if (brick_mode) SDL_RenderSetLogicalSize(ren, 256, 192);   /* whatever the window size, the picture is 256x192 scaled by a whole number where it fits */

  fez_init(&synth, (uint32_t)SDL_GetTicks() | 1);            /* seed the random generator from the clock */
  desk_init(&desk, &synth);
  if (brick_mode) { desk_set_handheld(&desk); pad_init(&pad, &desk); }
  desk.scope = scope_ring; desk.scope_head = &scope_head;
  desk.clip_set = clip_set; desk.clip_get = clip_get;
  {                                                           /* one visible folder: ~/FEZ/patches/ and ~/FEZ/recordings/ (easy to find, back up and share) */
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");              /* C:\Users\<name>: Windows has no HOME. Forward slashes work in Windows paths. */
#else
    const char *home = getenv("HOME");
#endif
#ifdef __IPHONEOS__
    /* On an iPad HOME is the app's private container; only its Documents folder is shown in the Files app (once the app's Info.plist
       sets UIFileSharingEnabled), so the FEZ folder goes there. */
    const char *under = "%s/Documents/FEZ/";
#else
    const char *under = "%s/FEZ/";
#endif
    if (home && home[0] && snprintf(home_fez, sizeof home_fez, under, home) < (int)sizeof home_fez) {
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
      snprintf(setup_path, sizeof setup_path, "%ssetup.txt", home_fez);
    }
  }

  /* The F2 setup page (desktop only): the saved choices, then the devices they name. */
  if (!brick_mode) { desk.setup_scan = setup_scan; desk.setup_apply = setup_apply; setup_load(); }
  if (open_audio(desk.setup.audio) < 0) { fprintf(stderr, "audio: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
  if (desk.setup.midi[0] && midi_open(desk.setup.midi) != 0) desk_say(&desk, "THE SAVED MIDI INPUT IS NOT THERE (F2 TO CHOOSE)");

  double lock_peak = 0, lock_left = 0;                       /* the longest the per-frame tick held the audio lock lately, and how long to keep it */
  uint32_t *pixels = NULL; SDL_Texture *tex = NULL; int pw = 0, ph = 0;
  if (!brick_mode) SDL_StartTextInput();                     /* printable characters arrive as SDL_TEXTINPUT (the handheld has no typing) */
  if (brick_mode) for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
  /* (the desktop opens and counts gamepads as their SDL_CONTROLLERDEVICEADDED events arrive: SDL sends one for each pad
     already plugged in at start too) */

  Uint32 last_frame = SDL_GetTicks();
  while (!desk.quit) {
    SDL_Event ev;
    int got = SDL_WaitEventTimeout(&ev, 16);
    while (got) {
      if (ev.type == SDL_QUIT) desk.quit = 1;
      else if (brick_mode && (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP)) {          /* the keyboard standing in for the buttons */
        Button b = key_to_button(ev.key.keysym.sym);
        if (b != BTN_COUNT) { SDL_LockAudioDevice(dev); pad_button(&pad, b, ev.type == SDL_KEYDOWN); SDL_UnlockAudioDevice(dev); }
      } else if (brick_mode && (ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERBUTTONUP)) {
        Button b = controller_to_button(ev.cbutton.button);
        if (b != BTN_COUNT) { SDL_LockAudioDevice(dev); pad_button(&pad, b, ev.type == SDL_CONTROLLERBUTTONDOWN); SDL_UnlockAudioDevice(dev); }
      } else if (brick_mode && ev.type == SDL_CONTROLLERAXISMOTION &&
                 (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {   /* the lower shoulder buttons are analogue triggers: past halfway is a press */
        SDL_LockAudioDevice(dev);
        pad_button(&pad, ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? BTN_L2 : BTN_R2, ev.caxis.value > 16384);
        SDL_UnlockAudioDevice(dev);
      } else if (brick_mode && ev.type == SDL_CONTROLLERAXISMOTION && ev.caxis.axis <= SDL_CONTROLLER_AXIS_RIGHTY) {   /* the sticks are modulation pots MP0..MP3 (SDL numbers them 0 to 3, as pad_stick does) */
        SDL_LockAudioDevice(dev);
        pad_stick(&pad, ev.caxis.axis, ev.caxis.value);
        SDL_UnlockAudioDevice(dev);
      } else if (brick_mode && ev.type == SDL_CONTROLLERDEVICEADDED) {
        SDL_GameControllerOpen(ev.cdevice.which);
      } else if (brick_mode) {
        /* the other events (mouse, text) mean nothing on the handheld */
      } else if (ev.type == SDL_CONTROLLERDEVICEADDED) {       /* the desktop: a gamepad plugged in shows the STICKS tab */
        if (SDL_GameControllerOpen(ev.cdevice.which)) desk.gamepads++;
      } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
        SDL_GameController *gc = SDL_GameControllerFromInstanceID(ev.cdevice.which);
        if (gc) { SDL_GameControllerClose(gc); desk.gamepads--; }
      } else if (ev.type == SDL_CONTROLLERAXISMOTION && ev.caxis.axis <= SDL_CONTROLLER_AXIS_RIGHTY) {   /* sticks: pots, or the STICKS tab's melody */
        SDL_LockAudioDevice(dev); desk_gamepad_axis(&desk, ev.caxis.axis, ev.caxis.value); SDL_UnlockAudioDevice(dev);
      } else if (ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button <= SDL_CONTROLLER_BUTTON_Y) {   /* A B X Y, by SDL's names (an Xbox pad's labels) */
        SDL_LockAudioDevice(dev); desk_gamepad_button(&desk, ev.cbutton.button); SDL_UnlockAudioDevice(dev);
      } else if (ev.type == SDL_KEYDOWN) {                     /* (key repeats are wanted: held arrows keep stepping) */
        int mods = translate_mods(ev.key.keysym.mod), key = translate_key(ev.key.keysym.sym);
        int nk = note_key_char(ev.key.keysym.scancode), played = 0;
        if (nk && !(mods & (MOD_CTRL | MOD_ALT))) {            /* on the KEYS tab a letter or digit plays a note (key repeats are swallowed there) */
          SDL_LockAudioDevice(dev);
          played = desk_note_key(&desk, nk, 1);
          SDL_UnlockAudioDevice(dev);
        }
        if (played) {
          /* a note key: nothing more to do (its text event will be ignored by the KEYS tab) */
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
    if (setup_pending >= 0) setup_do();                        /* a change on the F2 page: reopen a device, save (without the lock) */
    {                                                          /* MIDI that arrived since the last pass. ponytail: up to 16 ms late (the loop's pace); a MIDI thread if notes feel late */
      unsigned char mb[512]; int n;
      while ((n = midi_read(mb, sizeof mb)) > 0) { SDL_LockAudioDevice(dev); desk_midi_bytes(&desk, mb, n); SDL_UnlockAudioDevice(dev); }
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
    if (brick_mode) { w = 256; h = 192; }                    /* the picture never changes size; SDL scales it */
    if (w != pw || h != ph) {                                /* new window size: new picture, new texture */
      free(pixels); if (tex) SDL_DestroyTexture(tex);
      pw = w; ph = h;
      pixels = malloc((size_t)pw * (size_t)ph * sizeof *pixels);
      tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pw, ph);
      if (!pixels || !tex) { fprintf(stderr, "out of memory for a %dx%d picture\n", pw, ph); break; }
      desk_set_canvas(&desk, pixels, pw, ph);
    }
    /* 1) Under the lock: only what changes the synth (the UI's per-frame tick: key repeats, the handheld's sticks). While the
       lock is held the audio callback has to wait, so the meter's worst-case marker adds the longest recent lock to the
       callback's own worst time, both as a share of how long one buffer lasts. */
    SDL_LockAudioDevice(dev);
    Uint64 lock_t0 = SDL_GetPerformanceCounter();            /* timed from when we HOLD the lock: waiting to get it delays us, not the audio */
    desk.peak = out_peak; desk.cpu = cpu_avg; desk.cpu_peak = cpu_peak + lock_peak / buffer_secs; desk.clip = clip_left > 0;
    desk_tick(&desk, (double)(now - last_frame));
    if (brick_mode) pad_tick(&pad);
    SDL_UnlockAudioDevice(dev);
    /* 2) Without the lock: the drawing. It takes a millisecond or two here and much longer on a small ARM board, and with the
       lock held the sound had to wait for it. The screens only READ the synth (nothing in desk_draw or pad_draw writes to
       it), so the worst a race can do is show a value the audio thread is halfway through changing: one frame of the
       picture a moment out of date. It cannot read out of bounds: every array is a fixed size, and the indices the screens
       use are clamped where they are used. (Strictly, C calls an unlocked read during a write undefined; in practice, for
       ints, floats and doubles on the machines FEZ runs on, it is a stale or fresh value. The scope has always worked so.) */
    Uint64 draw_t0 = SDL_GetPerformanceCounter();
    desk_draw(&desk);
    if (brick_mode) pad_draw(&pad);
    double draw_secs = (double)(SDL_GetPerformanceCounter() - draw_t0) / (double)SDL_GetPerformanceFrequency();
    {                                                        /* the lock just held, kept as the worst for CPU_PEAK_HOLD seconds */
      double held = (double)(draw_t0 - lock_t0) / (double)SDL_GetPerformanceFrequency();
      lock_left -= (now - last_frame) / 1000.0;
      if (held >= lock_peak || lock_left <= 0) { lock_peak = held; lock_left = CPU_PEAK_HOLD; }
      static Uint32 last_log;                                 /* FEZ_LOAD_LOG=1: once a second, the two parts of the white marker */
      if (getenv("FEZ_LOAD_LOG") && now - last_log >= 1000) {
        last_log = now;
        fprintf(stderr, "load: audio avg %.1f%% worst %.1f%% | lock %.2f ms (worst %.2f ms) = %.1f%% of a %.1f ms buffer | drawing %.2f ms (unlocked)\n",
                cpu_avg * 100, cpu_peak * 100, held * 1000, lock_peak * 1000, lock_peak / buffer_secs * 100, buffer_secs * 1000, draw_secs * 1000);
      }
    }
    last_frame = now;
    SDL_UpdateTexture(tex, NULL, pixels, pw * (int)sizeof *pixels);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
  }

  if (desk.recording) rec_stop();                             /* close the file properly so its header is right */
  midi_close();
  SDL_CloseAudioDevice(dev);
  free(pixels); if (tex) SDL_DestroyTexture(tex);
  SDL_DestroyRenderer(ren); SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
