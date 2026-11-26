/* MIDI input, one version per operating system (see midi.h). Nothing here knows what a note or a CC is: it only finds the
   inputs and passes on their bytes.

   macOS and Windows deliver MIDI on a thread of their own, through a function we give them (a "callback"). That thread must
   not touch the synth, so it only drops the bytes into a ring buffer, and the main loop takes them out (midi_read), with
   the audio locked, about every 16 ms. One writer (the callback) and one reader (the main loop) can share a ring safely
   with two counters, as the recording ring in main.c does. Linux needs no thread: the main loop reads the device file. */
#include "midi.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- the ring (macOS, Windows) */
#if (defined(__APPLE__) && !defined(__IPHONEOS__)) || defined(_WIN32)

#define RING 4096                               /* bytes: far more than arrive in 16 ms (a busy controller sends a few hundred a second) */
static unsigned char ring[RING];
static SDL_atomic_t head, tail;                 /* head: bytes written so far (the callback); tail: bytes read (the main loop) */

static void ring_put(const unsigned char *b, int n) {
  int h = SDL_AtomicGet(&head), t = SDL_AtomicGet(&tail);
  for (int i = 0; i < n && h - t < RING; i++, h++) ring[h & (RING - 1)] = b[i];   /* full: the rest is dropped (never in practice) */
  SDL_AtomicSet(&head, h);
}

static int ring_take(unsigned char *buf, int max) {
  int h = SDL_AtomicGet(&head), t = SDL_AtomicGet(&tail), n = 0;
  while (t != h && n < max) buf[n++] = ring[t++ & (RING - 1)];
  SDL_AtomicSet(&tail, t);
  return n;
}
#endif

#if defined(__APPLE__) && !defined(__IPHONEOS__)
/* ---------------------------------------------------------------- macOS: CoreMIDI

   CoreMIDI calls the inputs "sources". We make one client (our app, as CoreMIDI sees it) and one input port, and connect the
   port to the chosen source. MIDIInputPortCreate is marked "to be deprecated" in favour of a newer call that delivers
   MIDI 2.0 packets; the old one still works on every macOS FEZ runs on and gives plain MIDI 1.0 bytes. */
#include <CoreMIDI/CoreMIDI.h>

static MIDIClientRef client;
static MIDIPortRef port;
static MIDIEndpointRef source;                  /* the one we listen to, or 0 */

static void source_name(MIDIEndpointRef e, char *out, int cap) {
  CFStringRef s = NULL;
  out[0] = '\0';
  if (MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &s) == noErr && s) {
    CFStringGetCString(s, out, cap, kCFStringEncodingUTF8);
    CFRelease(s);
  }
}

/* Called by CoreMIDI on its own thread with a list of packets; each packet holds whole messages. */
static void read_proc(const MIDIPacketList *list, void *port_ref, void *source_ref) {
  (void)port_ref; (void)source_ref;
  const MIDIPacket *p = &list->packet[0];
  for (unsigned i = 0; i < list->numPackets; i++) {
    ring_put(p->data, p->length);
    p = MIDIPacketNext(p);                      /* packets have different lengths: this finds the next one */
  }
}

int midi_list(char names[][SETUP_NAME], int max) {
  int n = (int)MIDIGetNumberOfSources(), k = 0;
  for (int i = 0; i < n && k < max; i++) { source_name(MIDIGetSource((ItemCount)i), names[k], SETUP_NAME); if (names[k][0]) k++; }
  return k;
}

int midi_open(const char *name) {
  midi_close();
  if (!client && MIDIClientCreate(CFSTR("FEZ"), NULL, NULL, &client) != noErr) return -1;
  if (!port && MIDIInputPortCreate(client, CFSTR("FEZ in"), read_proc, NULL, &port) != noErr) return -1;
  int n = (int)MIDIGetNumberOfSources();
  for (int i = 0; i < n; i++) {
    char got[SETUP_NAME]; MIDIEndpointRef e = MIDIGetSource((ItemCount)i);
    source_name(e, got, sizeof got);
    if (!strcmp(got, name) && MIDIPortConnectSource(port, e, NULL) == noErr) { source = e; return 0; }
  }
  return -1;
}

void midi_close(void) {
  if (source) { MIDIPortDisconnectSource(port, source); source = 0; }
}

int midi_read(unsigned char *buf, int max) { return ring_take(buf, max); }

#elif defined(_WIN32)
/* ---------------------------------------------------------------- Windows: winmm

   The oldest Windows MIDI interface, and still the one every USB MIDI device appears in. Each short message arrives packed
   into one number: status in the low byte, then the two data bytes. */
#include <windows.h>
#include <mmsystem.h>

static HMIDIIN in;

static void CALLBACK in_proc(HMIDIIN h, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2) {
  (void)h; (void)user; (void)p2;
  if (msg != MIM_DATA) return;                  /* sysex and the open/close notices are not used */
  unsigned char b[3] = { (unsigned char)(p1 & 0xFF), (unsigned char)((p1 >> 8) & 0xFF), (unsigned char)((p1 >> 16) & 0xFF) };
  int type = b[0] & 0xF0, n = b[0] >= 0xF8 ? 1 : type == 0xC0 || type == 0xD0 ? 2 : 3;   /* how many bytes the message has */
  ring_put(b, n);
}

int midi_list(char names[][SETUP_NAME], int max) {
  int n = (int)midiInGetNumDevs(), k = 0;
  for (int i = 0; i < n && k < max; i++) {
    MIDIINCAPSA caps;
    if (midiInGetDevCapsA((UINT)i, &caps, sizeof caps) == MMSYSERR_NOERROR) snprintf(names[k++], SETUP_NAME, "%s", caps.szPname);
  }
  return k;
}

int midi_open(const char *name) {
  midi_close();
  int n = (int)midiInGetNumDevs();
  for (int i = 0; i < n; i++) {
    MIDIINCAPSA caps;
    if (midiInGetDevCapsA((UINT)i, &caps, sizeof caps) != MMSYSERR_NOERROR || strcmp(caps.szPname, name)) continue;
    if (midiInOpen(&in, (UINT)i, (DWORD_PTR)in_proc, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) { in = NULL; return -1; }
    midiInStart(in);
    return 0;
  }
  return -1;
}

void midi_close(void) {
  if (in) { midiInStop(in); midiInClose(in); in = NULL; }
}

int midi_read(unsigned char *buf, int max) { return ring_take(buf, max); }

#elif defined(__linux__)
/* ---------------------------------------------------------------- Linux: ALSA's raw MIDI files

   ALSA shows each hardware MIDI port as a file, /dev/snd/midiC<card>D<device>, which gives the bytes as they arrive. Reading
   it needs no library. The card's short name is in /proc/asound/card<N>/id. (Software ports, such as a virtual keyboard,
   live in ALSA's "sequencer" instead and do not appear here; connect them to a hardware or virtual raw port with aconnect, or
   use the snd-virmidi module, which makes raw ports for them.) */
#include <fcntl.h>
#include <unistd.h>

static int fd = -1;

/* The n-th raw MIDI file that exists, in card and device order: its path and a name to show. Returns 0 when there is no n-th. */
static int nth_port(int n, char *path, int pcap, char *name, int ncap) {
  for (int card = 0; card < 32; card++)
    for (int dev = 0; dev < 8; dev++) {
      snprintf(path, (size_t)pcap, "/dev/snd/midiC%dD%d", card, dev);
      if (access(path, F_OK) != 0 || n-- > 0) continue;
      char idp[64], id[40] = "card"; FILE *f;
      snprintf(idp, sizeof idp, "/proc/asound/card%d/id", card);
      if ((f = fopen(idp, "r"))) { if (fgets(id, sizeof id, f)) id[strcspn(id, "\n")] = '\0'; fclose(f); }
      snprintf(name, (size_t)ncap, "%s (hw:%d,%d)", id, card, dev);
      return 1;
    }
  return 0;
}

int midi_list(char names[][SETUP_NAME], int max) {
  char path[48]; int k = 0;
  while (k < max && nth_port(k, path, sizeof path, names[k], SETUP_NAME)) k++;
  return k;
}

int midi_open(const char *name) {
  midi_close();
  char path[48], got[SETUP_NAME];
  for (int i = 0; nth_port(i, path, sizeof path, got, sizeof got); i++)
    if (!strcmp(got, name)) { fd = open(path, O_RDONLY | O_NONBLOCK); return fd >= 0 ? 0 : -1; }   /* non-blocking: a read with nothing waiting returns at once */
  return -1;
}

void midi_close(void) {
  if (fd >= 0) { close(fd); fd = -1; }
}

int midi_read(unsigned char *buf, int max) {
  if (fd < 0) return 0;
  ssize_t n = read(fd, buf, (size_t)max);
  return n > 0 ? (int)n : 0;
}

#else
/* ---------------------------------------------------------------- anywhere else (iOS): no MIDI yet */
int midi_list(char names[][SETUP_NAME], int max) { (void)names; (void)max; return 0; }
int midi_open(const char *name) { (void)name; return -1; }
void midi_close(void) {}
int midi_read(unsigned char *buf, int max) { (void)buf; (void)max; return 0; }
#endif
