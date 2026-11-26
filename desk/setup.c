/* The F2 setup page: where the sound goes, where MIDI comes from, and which MIDI controller moves each pot.

   It is a full-window page like F1 help, on the desktop only. Three parts:
     AUDIO OUTPUT   the output devices SDL found; Enter on one switches to it ("default" follows the system's choice)
     MIDI INPUT     the MIDI inputs found (see midi.c); Enter on one listens to it, "none" stops listening
     CHANNEL        which MIDI channel is heard: every channel, or 1 to 16
     POTS           MP0..MP15: the CC number that moves each one, and how its value is read (the MODE):
                      ABS    an ordinary knob or fader: 0..127 is the position, bottom to top
                      REL64  an endless encoder, "binary offset": 64 means no change, 65 one step up, 63 one step down
                      REL2C  an endless encoder, "two's complement": 1 is one step up, 127 one step down
                    (controllers name these differently: check which one yours sends; the LAST MESSAGE line shows it)
   Notes on any heard channel play the keyboard device (fez_note_on), as the computer keys and the handheld's stick do.

   This file has no system calls: the platform layer (main.c, midi.c) lists the devices into the Desk, opens what is
   chosen, saves the Setup as text (setup_write / setup_read here) and hands over the MIDI bytes as they arrive. */
#include "desk.h"
#include "regview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REL_STEP 256        /* one encoder click moves a pot 256 of 32767: 128 clicks from bottom to top. ponytail: fixed; a per-pot speed if anyone asks */
#define COLW 40             /* characters in one column of the page */

static const char *const MODE_NAMES[CC_MODES] = { "abs", "rel64", "rel2c" };

/* ---------------------------------------------------------------- the settings as text */

void setup_defaults(Setup *s) {
  memset(s, 0, sizeof *s);
  for (int i = 0; i < NUM_POTS; i++) s->pot[i].cc = -1;      /* nothing mapped until you choose */
}

int setup_write(const Setup *s, char *out, int cap) {
  int n = snprintf(out, (size_t)cap, "FEZ SETUP 1\naudio %s\nmidi %s\nchannel %d\n", s->audio, s->midi, s->chan);
  for (int i = 0; i < NUM_POTS && n < cap; i++) {
    if (s->pot[i].cc < 0) n += snprintf(out + n, (size_t)(cap - n), "pot %d off %s\n", i, MODE_NAMES[s->pot[i].mode]);
    else n += snprintf(out + n, (size_t)(cap - n), "pot %d %d %s\n", i, s->pot[i].cc, MODE_NAMES[s->pot[i].mode]);
  }
  return n < cap ? n : cap - 1;
}

/* Copy the rest of a line (after the keyword and one space) into a name field. */
static void take_name(char *dst, const char *line, size_t skip) {
  size_t k = 0;
  for (const char *c = line + skip; *c && *c != '\n' && *c != '\r' && k < SETUP_NAME - 1; c++) dst[k++] = *c;
  dst[k] = '\0';
}

void setup_read(Setup *s, const char *text) {
  setup_defaults(s);
  for (const char *line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
    int i, cc, ch; char a[16], m[16];
    if (!strncmp(line, "audio ", 6)) take_name(s->audio, line, 6);
    else if (!strncmp(line, "midi ", 5)) take_name(s->midi, line, 5);
    else if (sscanf(line, "channel %d", &ch) == 1) s->chan = fez_clamp(ch, 0, 16);
    else if (sscanf(line, "pot %d %15s %15s", &i, a, m) == 3 && i >= 0 && i < NUM_POTS) {
      s->pot[i].cc = sscanf(a, "%d", &cc) == 1 ? fez_clamp(cc, 0, 127) : -1;     /* "off" (or anything not a number) is unmapped */
      for (int k = 0; k < CC_MODES; k++) if (!strcmp(m, MODE_NAMES[k])) s->pot[i].mode = k;
    }
  }
}

/* ---------------------------------------------------------------- MIDI in

   MIDI is a stream of bytes. A byte with its top bit set (0x80 and up) is a STATUS: what the message is (high four bits)
   and its channel (low four bits). The one or two bytes after it are its DATA (each 0..127). A device may leave the status
   out when it repeats ("running status"), so the last status is kept and new data bytes reuse it. Bytes 0xF0..0xF7 start
   system messages (sysex and the like) that FEZ ignores up to the next status; 0xF8 and up are clock ticks, which can
   arrive in the middle of another message and are skipped on the spot. */

static void handle(Desk *d, int status, int d1, int d2) {
  FezSynth *s = d->synth;
  int type = status & 0xF0, ch = (status & 0x0F) + 1;
  int heard = d->setup.chan == 0 || d->setup.chan == ch;
  char note[8];
  /* 1) the message, for the setup page (even from a channel not heard, so you can see why nothing happens) */
  if (type == 0xB0) snprintf(d->midi_last, sizeof d->midi_last, "CC %d = %d  ch %d%s", d1, d2, ch, heard ? "" : " (not heard)");
  else if (type == 0x90 || type == 0x80) {
    regview_fmt_note(note, sizeof note, d1 * 256);
    snprintf(d->midi_last, sizeof d->midi_last, "NOTE %s %s vel %d  ch %d%s", note, type == 0x90 && d2 ? "on" : "off", d2, ch, heard ? "" : " (not heard)");
  } else snprintf(d->midi_last, sizeof d->midi_last, "status %02X  ch %d", status, ch);
  if (!heard) return;
  /* 2) notes: a note-on with velocity 0 is a note-off (many devices send it that way) */
  if (type == 0x90 && d2 > 0) fez_note_on(s, d1);
  else if (type == 0x80 || type == 0x90) fez_note_off(s, d1);
  else if (type == 0xB0) {
    if (d1 == 120 || d1 == 123) fez_all_notes_off(s);                       /* "all sound off" and "all notes off" */
    /* 3) every pot mapped to this CC (two pots may share one) */
    for (int i = 0; i < NUM_POTS; i++) {
      if (d->setup.pot[i].cc != d1) continue;
      int mode = d->setup.pot[i].mode, now = fez_reg_read(s, REG_MP0 + i);
      if (mode == CC_ABS) fez_set_pot(s, i, d2 * 32767 / 127);              /* 0 is the bottom, 127 the top */
      else {
        int step = mode == CC_REL64 ? d2 - 64 : d2 < 64 ? d2 : d2 - 128;   /* clicks: up is positive, down negative */
        fez_set_pot(s, i, now + step * REL_STEP);                           /* fez_set_pot keeps it in 0..32767 */
      }
    }
  }
}

void desk_midi_bytes(Desk *d, const unsigned char *b, int n) {
  for (int i = 0; i < n; i++) {
    int x = b[i];
    if (x >= 0xF8) continue;                                                 /* real-time: clock, start, stop. Not used */
    if (x >= 0xF0) { d->midi_status = 0; continue; }                         /* system messages: ignore until the next status */
    if (x & 0x80) { d->midi_status = x; d->midi_n = 0; continue; }
    if (!d->midi_status) continue;                                           /* data with no status yet (we joined mid-stream) */
    d->midi_data[d->midi_n++] = (unsigned char)x;
    int type = d->midi_status & 0xF0, need = type == 0xC0 || type == 0xD0 ? 1 : 2;   /* program change and pressure have one data byte */
    if (d->midi_n == need) { handle(d, d->midi_status, d->midi_data[0], need == 2 ? d->midi_data[1] : 0); d->midi_n = 0; }
  }
}

/* ---------------------------------------------------------------- the page

   The rows, top to bottom (Up/Down walks them in this order): "default" and each audio device; "none" and each MIDI
   input; the channel; the sixteen pots. Their number changes with the devices found, so a row is worked out from its index. */
enum { R_AUDIO, R_MIDI, R_CHAN, R_POT };
static int row_count(const Desk *d) { return 1 + d->n_audio + 1 + d->n_midi + 1 + NUM_POTS; }
static int row_kind(const Desk *d, int r, int *index) {                       /* index: -1 for default / none, else the device or pot */
  if (r < 1 + d->n_audio) { *index = r - 1; return R_AUDIO; }
  r -= 1 + d->n_audio;
  if (r < 1 + d->n_midi) { *index = r - 1; return R_MIDI; }
  r -= 1 + d->n_midi;
  if (r == 0) { *index = 0; return R_CHAN; }
  *index = r - 1; return R_POT;
}

void desk_setup_open(Desk *d) {
  d->setup_open = 1; entry_clear(&d->setup_in);
  if (d->setup_scan) d->setup_scan(d);                                         /* devices may have come and gone since last time */
  if (d->setup_row >= row_count(d)) d->setup_row = 0;
}

static void saved(Desk *d, int what) { if (d->setup_apply) d->setup_apply(d, what); }

void desk_setup_key(Desk *d, int key, int mods) {
  int n = row_count(d), idx, kind = row_kind(d, d->setup_row, &idx);
  int big = (mods & MOD_SHIFT) ? 10 : 1;
  switch (key) {
    case KEY_F2: d->setup_open = 0; break;
    case KEY_ESC: if (d->setup_in.editing) entry_clear(&d->setup_in); else d->setup_open = 0; break;
    case KEY_UP:   d->setup_row = (d->setup_row + n - 1) % n; entry_clear(&d->setup_in); break;
    case KEY_DOWN: d->setup_row = (d->setup_row + 1) % n; entry_clear(&d->setup_in); break;
    case KEY_BACKSPACE: entry_backspace(&d->setup_in); break;
    case KEY_LEFT: case KEY_RIGHT: {
      int dir = key == KEY_RIGHT ? 1 : -1;
      if (kind == R_CHAN) { d->setup.chan = fez_clamp(d->setup.chan + dir, 0, 16); saved(d, SETUP_SAVE); }
      else if (kind == R_POT) {                                                /* -1 is "off", below CC 0 */
        d->setup.pot[idx].cc = fez_clamp(d->setup.pot[idx].cc + dir * big, -1, 127);
        entry_clear(&d->setup_in); saved(d, SETUP_SAVE);
      }
      break;
    }
    case KEY_ENTER:
      if (kind == R_AUDIO) { snprintf(d->setup.audio, SETUP_NAME, "%s", idx < 0 ? "" : d->audio_names[idx]); saved(d, SETUP_AUDIO); }
      else if (kind == R_MIDI) { snprintf(d->setup.midi, SETUP_NAME, "%s", idx < 0 ? "" : d->midi_names[idx]); saved(d, SETUP_MIDI); }
      else if (kind == R_POT && d->setup_in.editing) {
        d->setup.pot[idx].cc = fez_clamp(atoi(d->setup_in.buf), 0, 127);
        entry_clear(&d->setup_in); saved(d, SETUP_SAVE);
      }
      break;
    default: break;
  }
}

void desk_setup_text(Desk *d, int ch) {
  int idx, kind = row_kind(d, d->setup_row, &idx);
  if (kind == R_POT && ch >= '0' && ch <= '9') entry_add(&d->setup_in, ch, 3);
  else if (kind == R_POT && ch == ' ') { d->setup.pot[idx].mode = (d->setup.pot[idx].mode + 1) % CC_MODES; saved(d, SETUP_SAVE); }
  else if (ch == 'r' || ch == 'R') desk_setup_open(d);                        /* look for devices again (one was just plugged in) */
}

/* One row of a device list: a mark on the one in use, the cursor's highlight. */
static void device_row(Desk *d, View *v, int col, int row, int r, const char *name, int in_use) {
  int sel = r == d->setup_row;
  if (sel) view_fill(v, col, row, COLW - 2, 1, COL_AMBD);
  view_text(v, col, row, in_use ? "*" : " ", COL_GRN);
  char buf[COLW]; snprintf(buf, sizeof buf, "%.*s", COLW - 5, name);
  view_text(v, col + 2, row, buf, sel ? COL_AMB : in_use ? COL_GRN : COL_FG);
}

void desk_setup_draw(Desk *d) {
  Canvas *cv = &d->cv;
  int s = d->scale, status = GFX_CELL_H * s + 4;
  View v;
  v.d = d; v.focused = 1;
  v.cw = GFX_CELL_W * s; v.rh = GFX_CELL_H * s;
  v.x = 8 * s; v.y = 4 * s; v.w = cv->w - 16 * s; v.h = cv->h - status - 8 * s;
  v.cols = v.w / v.cw; v.rows = v.h / v.rh;
  gfx_unclip(cv);
  gfx_fill(cv, 0, 0, cv->w, cv->h - status, gfx_colour(COL_BG));               /* cover the tiles, leave the status bar */
  gfx_frame(cv, 0, 0, cv->w, cv->h - status, gfx_colour(COL_AMB));
  gfx_clip(cv, 0, 0, cv->w, cv->h - status);
  view_text(&v, 0, 0, "FEZ SETUP", COL_AMB);
  view_text_right(&v, 10, v.cols - 10, 0, "F2 or Esc: close", COL_DIM);
  gfx_fill(cv, v.x, v.y + v.rh - 2 * s, v.w, 1, gfx_colour(COL_LINE));

  /* 1. the left column: devices, channel, the last message. (A narrow window stacks the pots underneath, and the page may
        run off the bottom. ponytail: no scrolling; widen the window or make the text smaller.) */
  int wide = v.cols >= 2 * COLW + 2, row = 2, r = 0;
  view_text(&v, 0, row++, "AUDIO OUTPUT", COL_MAG);
  device_row(d, &v, 0, row++, r++, "default (the system's choice)", d->setup.audio[0] == '\0');
  int found = !d->setup.audio[0];
  for (int i = 0; i < d->n_audio; i++) { int on = !strcmp(d->audio_names[i], d->setup.audio); found |= on; device_row(d, &v, 0, row++, r++, d->audio_names[i], on); }
  if (!found) { char b[COLW + 24]; snprintf(b, sizeof b, "  saved %.*s: not found", COLW - 20, d->setup.audio); view_text(&v, 0, row++, b, COL_RED); }
  row++;
  view_text(&v, 0, row++, "MIDI INPUT", COL_MAG);
  device_row(d, &v, 0, row++, r++, "none", d->setup.midi[0] == '\0');
  found = !d->setup.midi[0];
  for (int i = 0; i < d->n_midi; i++) { int on = !strcmp(d->midi_names[i], d->setup.midi); found |= on; device_row(d, &v, 0, row++, r++, d->midi_names[i], on); }
  if (!found) { char b[COLW + 24]; snprintf(b, sizeof b, "  saved %.*s: not found", COLW - 20, d->setup.midi); view_text(&v, 0, row++, b, COL_RED); }
  row++;
  char buf[64];
  int sel = r == d->setup_row;
  if (sel) view_fill(&v, 0, row, COLW - 2, 1, COL_AMBD);
  if (d->setup.chan) snprintf(buf, sizeof buf, "CHANNEL  %d", d->setup.chan); else snprintf(buf, sizeof buf, "CHANNEL  every one");
  view_text(&v, 0, row++, buf, sel ? COL_AMB : COL_FG);
  r++;
  row++;
  view_text(&v, 0, row++, "LAST MESSAGE", COL_MAG);
  view_text(&v, 0, row++, d->midi_last[0] ? d->midi_last : "(nothing yet)", d->midi_last[0] ? COL_MAG : COL_DIM);
  row++;
  view_text(&v, 0, row++, "Enter use a device   R look again", COL_DIM);

  /* 2. the pots: CC number, mode, and the pot's value now (so you can watch a knob move it) */
  int col = wide ? COLW + 2 : 0, prow = wide ? 2 : row + 1;
  view_text(&v, col, prow++, "POTS", COL_MAG);
  view_text(&v, col, prow++, "pot    CC    mode   now", COL_DIM);
  for (int i = 0; i < NUM_POTS; i++, r++, prow++) {
    sel = r == d->setup_row;
    if (sel) view_fill(&v, col, prow, COLW - 2, 1, COL_AMBD);
    snprintf(buf, sizeof buf, "MP%d", i);
    view_text(&v, col, prow, buf, sel ? COL_AMB : COL_CYN);
    if (sel && d->setup_in.editing) snprintf(buf, sizeof buf, "%s_", d->setup_in.buf);
    else if (d->setup.pot[i].cc < 0) snprintf(buf, sizeof buf, "off");
    else snprintf(buf, sizeof buf, "%d", d->setup.pot[i].cc);
    view_text_right(&v, col + 5, 4, prow, buf, d->setup.pot[i].cc < 0 && !(sel && d->setup_in.editing) ? COL_DIM : COL_FG);
    view_text(&v, col + 13, prow, MODE_NAMES[d->setup.pot[i].mode], COL_FG);
    view_bar(&v, col + 20, prow, COLW - 23, fez_reg_read(d->synth, REG_MP0 + i) / 32767.0, COL_MAG, COL_SEL);
  }
  prow++;
  view_text(&v, col, prow++, "Left/Right CC (Shift 10)  type a CC", COL_DIM);
  view_text(&v, col, prow, "Space mode: abs, rel64, rel2c", COL_DIM);
  gfx_unclip(cv);
}
