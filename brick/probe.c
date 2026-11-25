/* FEZ probe: finds out what the Brick's SDL2 gives us, before we trust the real app to it. Run it from the launcher; it writes
   a report to stdout (the launch script sends that to probe.log next to the app) and then, for 40 seconds, prints every
   button, key and stick event it sees, shows the last one on screen, and plays a quiet 440 Hz tone for the first second.
   The report answers the questions we cannot answer from a desk: which video and audio drivers exist, what size the screen is,
   which controllers appear, and which button number is which physical button. Plain SDL2, no other files. */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned tone_pos;

/* Audio callback: a sine at 440 Hz for one second, then silence. (The device may not give us float; handle the two common formats.) */
static void audio_cb(void *user, Uint8 *stream, int len) {
  const SDL_AudioSpec *spec = user;
  int channels = spec->channels, bytes = SDL_AUDIO_BITSIZE(spec->format) / 8, frames = len / (channels * bytes);
  for (int i = 0; i < frames; i++, tone_pos++) {
    float v = tone_pos < (unsigned)spec->freq ? 0.1f * sinf(6.2831853f * 440.0f * (float)tone_pos / (float)spec->freq) : 0.0f;
    for (int c = 0; c < channels; c++) {
      Uint8 *p = stream + (i * channels + c) * bytes;
      if (spec->format == AUDIO_F32SYS) memcpy(p, &v, sizeof v);
      else if (spec->format == AUDIO_S16SYS) { Sint16 s = (Sint16)(v * 32767.0f); memcpy(p, &s, sizeof s); }
      else memset(p, 0, (size_t)bytes);
    }
  }
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char last_line[128];
#include <stdarg.h>
static void say(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  vsnprintf(last_line, sizeof last_line, fmt, ap);
  va_end(ap);
  printf("%s\n", last_line); fflush(stdout);
}

/* Analog axes (sticks and triggers). A held stick sends hundreds of events, so the log gets one line when an axis passes into
   a deflection and one when it returns to the middle, with hysteresis (in at 12000, out at 6000) so a wobble at the edge does not
   chatter. The lowest and highest value seen on each axis is kept and printed at the end. `kind` separates the raw joystick axes
   from SDL's named controller axes: they are two views of the same hardware. */
#define MAX_AXES 16
enum { joy, ctl };
static int axis_state[2][MAX_AXES], seen[2][MAX_AXES], lo[2][MAX_AXES], hi[2][MAX_AXES];

static void watch_axis(int kind, int axis, int value, const char *name) {
  if (axis < 0 || axis >= MAX_AXES) return;
  if (!seen[kind][axis]) { seen[kind][axis] = 1; lo[kind][axis] = hi[kind][axis] = value; }
  if (value < lo[kind][axis]) lo[kind][axis] = value;
  if (value > hi[kind][axis]) hi[kind][axis] = value;
  int now = axis_state[kind][axis];
  if (value > 12000) now = 1; else if (value < -12000) now = -1; else if (value > -6000 && value < 6000) now = 0;
  if (now != axis_state[kind][axis]) {
    axis_state[kind][axis] = now;
    say("%s %d (%s): %s (value %d)", kind == joy ? "joy axis" : "controller axis", axis, name, now > 0 ? "pushed positive" : now < 0 ? "pushed negative" : "back to centre", value);
  }
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  say("FEZ probe, SDL %d.%d.%d", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
  SDL_version linked; SDL_GetVersion(&linked);
  say("linked SDL %d.%d.%d", linked.major, linked.minor, linked.patch);
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) { say("SDL_Init failed: %s", SDL_GetError()); return 1; }

  for (int i = 0; i < SDL_GetNumVideoDrivers(); i++) say("video driver %d: %s%s", i, SDL_GetVideoDriver(i), !strcmp(SDL_GetVideoDriver(i), SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "") ? " (in use)" : "");
  for (int i = 0; i < SDL_GetNumAudioDrivers(); i++) say("audio driver %d: %s", i, SDL_GetAudioDriver(i));
  say("current audio driver: %s", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(none)");
  for (int i = 0; i < SDL_GetNumVideoDisplays(); i++) {
    SDL_DisplayMode m;
    if (SDL_GetCurrentDisplayMode(i, &m) == 0) say("display %d: %dx%d @ %d Hz, format %s", i, m.w, m.h, m.refresh_rate, SDL_GetPixelFormatName(m.format));
  }
  for (int i = 0; i < SDL_GetNumAudioDevices(0); i++) say("audio output %d: %s", i, SDL_GetAudioDeviceName(i, 0));
  say("joysticks: %d", SDL_NumJoysticks());
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    SDL_Joystick *j = SDL_JoystickOpen(i);
    char guid[40]; SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof guid);
    say("joystick %d: %s  guid %s  buttons %d hats %d axes %d  gamecontroller %s", i, SDL_JoystickNameForIndex(i), guid,
        j ? SDL_JoystickNumButtons(j) : -1, j ? SDL_JoystickNumHats(j) : -1, j ? SDL_JoystickNumAxes(j) : -1, SDL_IsGameController(i) ? "yes" : "no");
    if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
    /* Resting values of every axis, before anyone touches anything: a worn or off-centre stick shows up here as a number that is
       not near 0 (a trigger rests near 0 or near -32768, which tells us its range). */
    if (j) for (int a = 0; a < SDL_JoystickNumAxes(j) && a < MAX_AXES; a++) say("  axis %d at rest: %d", a, SDL_JoystickGetAxis(j, (int)a));
  }

  SDL_Window *win = SDL_CreateWindow("probe", 0, 0, 1024, 768, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
  SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, 0) : NULL;
  if (!win || !ren) say("window or renderer failed: %s", SDL_GetError());
  else {
    SDL_RendererInfo ri; SDL_GetRendererInfo(ren, &ri);
    int w, h; SDL_GetRendererOutputSize(ren, &w, &h);
    say("renderer: %s, output %dx%d", ri.name, w, h);
    SDL_RenderSetLogicalSize(ren, 256, 192);
  }

  SDL_AudioSpec want, have; SDL_zero(want);
  want.freq = 48000; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 512; want.callback = audio_cb;
  static SDL_AudioSpec have_for_cb;
  want.userdata = &have_for_cb;
  SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_ANY_CHANGE);
  if (!dev) say("audio open failed: %s", SDL_GetError());
  else {
    have_for_cb = have;
    say("audio opened: %d Hz, format 0x%04x, %d channels, buffer %d frames (asked for 48000 Hz float stereo 512)", have.freq, have.format, have.channels, have.samples);
    SDL_PauseAudioDevice(dev, 0);
  }

  Uint32 end = SDL_GetTicks() + 60000;
  while (SDL_GetTicks() < end) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
        case SDL_QUIT: end = 0; break;
        case SDL_KEYDOWN: case SDL_KEYUP: say("key %s: %s (sym %d, scancode %d)", e.type == SDL_KEYDOWN ? "down" : "up", SDL_GetKeyName(e.key.keysym.sym), e.key.keysym.sym, e.key.keysym.scancode); break;
        case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP: say("joy button %d %s", e.jbutton.button, e.type == SDL_JOYBUTTONDOWN ? "down" : "up"); break;
        case SDL_JOYHATMOTION: say("joy hat %d value %d", e.jhat.hat, e.jhat.value); break;
        case SDL_JOYAXISMOTION: watch_axis(joy, e.jaxis.axis, e.jaxis.value, "joy axis"); break;
        case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: say("controller button %s %s", SDL_GameControllerGetStringForButton((SDL_GameControllerButton)e.cbutton.button), e.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up"); break;
        case SDL_CONTROLLERAXISMOTION: watch_axis(ctl, e.caxis.axis, e.caxis.value, SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)e.caxis.axis)); break;
        default: break;
      }
    }
    if (ren) {                                            /* the screen: a dark field with the last event's text, so we know the picture works */
      SDL_SetRenderDrawColor(ren, 10, 20, 30, 255); SDL_RenderClear(ren);
      SDL_SetRenderDrawColor(ren, 0, 255, 120, 255);
      SDL_Rect bar = { 4, 4, (int)((SDL_GetTicks() % 2000) * 248 / 2000), 8 };   /* a moving bar: the picture is alive */
      SDL_RenderFillRect(ren, &bar);
      SDL_RenderPresent(ren);
    }
    SDL_Delay(16);
  }
  for (int k = 0; k < 2; k++)                               /* the range each axis covered: a stick should reach about -32768 to 32767, a trigger half of that */
    for (int a = 0; a < MAX_AXES; a++)
      if (seen[k][a]) say("%s axis %d: lowest %d, highest %d", k == joy ? "joystick" : "controller", a, lo[k][a], hi[k][a]);
  say("probe finished");
  if (dev) SDL_CloseAudioDevice(dev);
  SDL_Quit();
  return 0;
}
