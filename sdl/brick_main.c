/* SDL2 front end for FEZ (macOS, Linux). It owns the audio device, the window and the keyboard; everything musical
   is in the core, and every screen is in ui/ (plain C that draws a 256x192 picture and knows nothing about SDL).

   Threads: SDL calls audio_cb on its own audio thread, which calls fez_render. Anything that touches the synth from
   the main thread (keys, drawing the screens) must hold SDL_LockAudioDevice, which pauses the callback briefly.

   Keys follow the TrimUI Brick page, so the screens behave the same here and on the device:
     arrows = D-pad     X = A (act)     Z = B (back/delete)     S = X     A = Y (hold for coarse steps / run)
     Q W = L1 R1 (change screen)    E R = L2 R2 (change tab / block)    Enter = START (fire the envelope)
     Shift = SELECT     Esc = MENU     = and - : volume       [ and ] : change screen (works on every screen)
   On the PLAY screen the home row plays notes instead:
     A S D F G H J K L ;   white notes     W E   T Y U   O P   black notes
     Left/Right octave     Enter fires the envelope     Tab pulsers on/off
   Close the window (or Cmd+Q) to quit.                                                                          */
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ui.h"
#include "ui_priv.h"

static FezSynth synth;                    /* ~2 MB (the buffer device alone is 1.5 MB), so static rather than on the stack */
static Ui ui;
static SDL_AudioDeviceID dev;
static volatile float out_peak;            /* written by the audio thread, read by the meter */

static void audio_cb(void *user, Uint8 *stream, int len) {
  (void)user;
  float *out = (float *)stream;
  int frames = len / (int)(2 * sizeof(float));               /* interleaved stereo */
  static float mono[8192];
  if (frames > 8192) frames = 8192;
  fez_render(&synth, mono, frames);
  float pk = out_peak * 0.9f;                                /* the meter falls away between buffers */
  for (int i = 0; i < frames; i++) {
    out[2 * i] = out[2 * i + 1] = mono[i];                   /* same signal in both ears */
    if (fabsf(mono[i]) > pk) pk = fabsf(mono[i]);
  }
  out_peak = pk;
}


/* Piano-style layout on the home row. Returns semitones above C, or -1 for keys that are not notes. */
static int key_to_semitone(SDL_Scancode sc) {
  switch (sc) {
    case SDL_SCANCODE_A: return 0;   case SDL_SCANCODE_W: return 1;   case SDL_SCANCODE_S: return 2;
    case SDL_SCANCODE_E: return 3;   case SDL_SCANCODE_D: return 4;   case SDL_SCANCODE_F: return 5;
    case SDL_SCANCODE_T: return 6;   case SDL_SCANCODE_G: return 7;   case SDL_SCANCODE_Y: return 8;
    case SDL_SCANCODE_H: return 9;   case SDL_SCANCODE_U: return 10;  case SDL_SCANCODE_J: return 11;
    case SDL_SCANCODE_K: return 12;  case SDL_SCANCODE_O: return 13;  case SDL_SCANCODE_L: return 14;
    case SDL_SCANCODE_P: return 15;  case SDL_SCANCODE_SEMICOLON: return 16;
    default: return -1;
  }
}

/* The Brick's buttons on a computer keyboard. Returns BTN_COUNT for keys that are not buttons. */
static Button key_to_button(SDL_Scancode sc) {
  switch (sc) {
    case SDL_SCANCODE_UP: return BTN_UP;       case SDL_SCANCODE_DOWN: return BTN_DOWN;
    case SDL_SCANCODE_LEFT: return BTN_LEFT;   case SDL_SCANCODE_RIGHT: return BTN_RIGHT;
    case SDL_SCANCODE_X: return BTN_A;         case SDL_SCANCODE_Z: return BTN_B;
    case SDL_SCANCODE_S: return BTN_X;         case SDL_SCANCODE_A: return BTN_Y;
    case SDL_SCANCODE_Q: case SDL_SCANCODE_LEFTBRACKET: return BTN_L1;
    case SDL_SCANCODE_W: case SDL_SCANCODE_RIGHTBRACKET: return BTN_R1;
    case SDL_SCANCODE_E: return BTN_L2;        case SDL_SCANCODE_R: return BTN_R2;
    case SDL_SCANCODE_RETURN: return BTN_START;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return BTN_SELECT;
    case SDL_SCANCODE_ESCAPE: return BTN_MENU;
    case SDL_SCANCODE_EQUALS: return BTN_VOLU; case SDL_SCANCODE_MINUS: return BTN_VOLD;
    default: return BTN_COUNT;
  }
}

/* On the PLAY screen the keys mean notes, so only these Brick buttons stay: the arrows, Enter, Shift, Esc, volume,
   and [ ] for changing screen. */
static Button key_to_button_play(SDL_Scancode sc) {
  Button b = key_to_button(sc);
  switch (b) {
    case BTN_UP: case BTN_DOWN: case BTN_LEFT: case BTN_RIGHT: case BTN_START: case BTN_SELECT: case BTN_MENU: case BTN_VOLU: case BTN_VOLD: return b;
    default: break;
  }
  if (sc == SDL_SCANCODE_LEFTBRACKET) return BTN_L1;
  if (sc == SDL_SCANCODE_RIGHTBRACKET) return BTN_R1;
  return BTN_COUNT;
}

static uint32_t pixels[UI_W * UI_H];       /* the picture converted to colours */

int main(void) {
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
  /* 4x the Brick's 256x192 screen; the window can be resized and the picture stays sharp and in proportion */
  SDL_Window *win = SDL_CreateWindow("FEZ", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, UI_W * 4, UI_H * 4, SDL_WINDOW_RESIZABLE);
  SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, 0) : NULL;
  if (!win || !ren) { fprintf(stderr, "window: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
  SDL_RenderSetLogicalSize(ren, UI_W, UI_H);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, UI_W, UI_H);
  if (!tex) { fprintf(stderr, "texture: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

  fez_init(&synth, (uint32_t)SDL_GetTicks() | 1);            /* seed the random generator from the clock */
  ui_init(&ui, &synth);

  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = FEZ_SAMPLE_RATE; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 512; want.callback = audio_cb;
  dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);       /* 0: SDL converts if the hardware differs from our request */
  if (!dev) { fprintf(stderr, "audio: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
  ui.audio_ok = 1;
  SDL_PauseAudioDevice(dev, 0);                              /* start playing */

  Uint32 last_frame = SDL_GetTicks();
  for (int running = 1; running; ) {
    SDL_Event ev;
    int got = SDL_WaitEventTimeout(&ev, 16);
    while (got) {
      if (ev.type == SDL_QUIT) { running = 0; break; }
      if ((ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) && !ev.key.repeat) {
        int down = ev.type == SDL_KEYDOWN;
        SDL_Scancode sc = ev.key.keysym.scancode;
        SDL_LockAudioDevice(dev);                            /* pause the audio thread while we touch the synth */
        if (ui.screen == SCR_PLAY) {
          int note = key_to_semitone(sc);
          if (note >= 0) { if (down) ui_play_note(&ui, note); }
          else if (down && sc == SDL_SCANCODE_TAB) ui_toggle_pulsers(&ui);
          else { Button b = key_to_button_play(sc); if (b != BTN_COUNT) ui_button(&ui, b, down); }
        } else {
          Button b = key_to_button(sc);
          if (b != BTN_COUNT) ui_button(&ui, b, down);
        }
        SDL_UnlockAudioDevice(dev);
      }
      got = SDL_PollEvent(&ev);
    }

    Uint32 now = SDL_GetTicks();
    if (now - last_frame >= 16) {
      SDL_LockAudioDevice(dev);
      ui.peak = out_peak;
      ui_tick(&ui, (double)(now - last_frame));
      ui_draw(&ui);
      SDL_UnlockAudioDevice(dev);
      last_frame = now;
      for (int i = 0; i < UI_W * UI_H; i++) {                /* palette index -> 0xAARRGGBB */
        const unsigned char *c = UI_PAL[ui.fb[i]];
        pixels[i] = 0xff000000u | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
      }
      SDL_UpdateTexture(tex, NULL, pixels, UI_W * (int)sizeof(uint32_t));
      SDL_RenderClear(ren);
      SDL_RenderCopy(ren, tex, NULL, NULL);
      SDL_RenderPresent(ren);
    }
  }

  SDL_CloseAudioDevice(dev);
  SDL_DestroyTexture(tex); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
