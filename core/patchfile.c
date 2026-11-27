/* Patch files: see patchfile.h for the format. */
#include "patchfile.h"
#include "regview.h"
#include "scales.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- helpers */

/* The register's name, or NULL if it has none. */
static const char *name_of(int addr) {
  for (int i = 0; i < NAME_COUNT; i++) if (NAME_TABLE[i].addr == addr) return NAME_TABLE[i].name;
  return NULL;
}

/* A register by name (any case) or by decimal address. Returns the address, or -1. */
static int addr_of(const char *word) {
  if (isdigit((unsigned char)word[0])) { int a = atoi(word); return a >= 0 && a < NUM_REGS ? a : -1; }
  for (int i = 0; i < NAME_COUNT; i++) {
    const char *n = NAME_TABLE[i].name, *w = word;
    while (*n && *w && toupper((unsigned char)*n) == toupper((unsigned char)*w)) { n++; w++; }
    if (!*n && !*w) return NAME_TABLE[i].addr;
  }
  return -1;
}


/* ---------------------------------------------------------------- writing */

/* A growing output buffer that never overruns: once it is full it only records that it overflowed. */
typedef struct { char *p; size_t cap, n; int over; } Out;

static void put(Out *o, const char *fmt, ...) {
  if (o->over) return;
  va_list ap;
  va_start(ap, fmt);
  int k = vsnprintf(o->p + o->n, o->cap - o->n, fmt, ap);
  va_end(ap);
  if (k < 0 || (size_t)k >= o->cap - o->n) o->over = 1; else o->n += (size_t)k;
}

/* Source text as "> " lines. Source may hold newlines; an empty line becomes a lone ">". */
static void put_source(Out *o, const char *keyword, int index, const char *src) {
  if (!src[0]) return;                                         /* empty blocks and words are not written */
  put(o, "%s %d\n", keyword, index);
  const char *line = src;
  for (;;) {
    const char *end = strchr(line, '\n');
    int len = end ? (int)(end - line) : (int)strlen(line);
    put(o, len ? "> %.*s\n" : ">\n", len, line);
    if (!end) break;
    line = end + 1;
  }
}

int patchfile_write(const FezSynth *s, const PatchInfo *info, char *out, size_t cap) {
  Out o = { out, cap, 0, 0 };
  if (cap) out[0] = '\0';
  int16_t def[NUM_REGS] = { 0 };
  fez_default_registers(def);

  put(&o, "FEZ PATCH %d\n", PATCH_FILE_VERSION);
  put(&o, "name %s\ndesc %s\nlisten %s\n", info->name, info->desc, info->listen);
  put(&o, "level %d\n", s->master_level);
  for (int i = 0; i < NUM_PULSERS; i++)
    put(&o, "pulser %d on %d rate %d\n", i, s->pulsers[i].on ? 1 : 0, s->mem[REG_PRATE0 + i]);

  /* registers: only the named, writable ones, and only those that differ from the defaults */
  RegEntry list[NUM_REGS];
  int n = regview_list(list, NUM_REGS);
  for (int i = 0; i < n; i++) {
    int a = list[i].addr;
    /* TMBITS is the one register whose value lives outside the register file (in s->tm), with its own default */
    int value = a == REG_TMBITS ? (int16_t)s->tm.bits : s->mem[a], dflt = a == REG_TMBITS ? 0x5ac3 : def[a];
    if (list[i].ro || list[i].wo || value == dflt) continue;
    if (a >= REG_PRATE0 && a <= REG_PRATE3) continue;           /* the pulser lines above already hold the rates */
    put(&o, "reg %s %d\n", list[i].name, value);
  }
  for (int i = 0; i < s->nroutes; i++) {
    const char *dst = name_of(s->routes[i].dst);
    put(&o, "route %s %s %d\n", SRC_TABLE[s->routes[i].src].name, dst ? dst : "?", s->routes[i].w);
  }
  put(&o, "ca %d %d %d %d\n", s->ca.rule, s->ca.row, s->ca.tap, s->ca.trig);
  put(&o, "lz %d %d %d\n", s->lz.rate, s->lz.rho, s->lz.trig);
  for (int i = 0; i < NUM_BLOCKS; i++) put_source(&o, "block", i, s->vm.blocks[i].src);
  for (int i = 0; i < NUM_WORDS; i++) put_source(&o, "word", i, s->vm.word_src[i]);
  return o.over ? -1 : (int)o.n;
}

/* ---------------------------------------------------------------- reading */

static void copy_text(char *dst, size_t cap, const char *src) {
  snprintf(dst, cap, "%s", src);
}

/* Remember the first problem only: the user fixes that one, then sees the next. */
static void complain(char *err, size_t errcap, int *bad, int lineno, const char *msg, const char *detail) {
  if (*bad == 0 && err && errcap) snprintf(err, errcap, "line %d: %s %s", lineno, msg, detail);
  (*bad)++;
}

int patchfile_read(FezSynth *s, const char *text, PatchInfo *info, char *err, size_t errcap) {
  if (err && errcap) err[0] = '\0';
  if (strncmp(text, "FEZ PATCH ", 10) != 0) return -1;
  if (atoi(text + 10) > PATCH_FILE_VERSION) return -1;         /* written by a newer program: do not guess */

  /* 1) start from a blank patch: a file lists only what differs from it */
  fez_load_blank(s);
  int version = atoi(text + 10);
  /* A file lists only what differs from the defaults, so one written before version 3 left out a delay mix of 9000 and a
     spring mix of 8000 (the defaults then; both are 0 now). Put them back first; a line in the file still wins. */
  if (version < 3) { fez_reg_write(s, REG_DLYMIX, 9000); fez_reg_write(s, REG_SPRMIX, 8000); }
  if (version < 3) fez_reg_write(s, REG_SPRTONE, 0);              /* SPRTONE came in version 3: 0 is the spring they were made with */
  info->name[0] = info->desc[0] = info->listen[0] = '\0';

  /* 2) one line at a time */
  int bad = 0, lineno = 0;
  int fmrate = 8000;                                           /* FMRATE, which files before version 3 may hold (8000 was its default) */
  int kind = 0, index = 0;                                     /* source being collected: 1 block, 2 word */
  char src[BLOCK_SRC_MAX];
  size_t srclen = 0;
  const char *p = strchr(text, '\n');                          /* skip the header line */
  p = p ? p + 1 : text + strlen(text);
  lineno = 1;
  while (*p) {
    char line[BLOCK_SRC_MAX + 16];
    size_t len = strcspn(p, "\n");
    if (len >= sizeof line) len = sizeof line - 1;
    memcpy(line, p, len); line[len] = '\0';
    if (len && line[len - 1] == '\r') line[len - 1] = '\0';    /* tolerate files saved with Windows line endings */
    p += strcspn(p, "\n"); if (*p) p++;
    lineno++;

    if (line[0] == '>') {                                      /* a line of source for the current block or word */
      if (!kind) { complain(err, errcap, &bad, lineno, "source line outside a block:", line); continue; }
      const char *t = line[1] == ' ' ? line + 2 : line + 1;
      size_t tl = strlen(t);
      size_t cap = kind == 1 ? BLOCK_SRC_MAX : WORD_SRC_MAX;    /* what vm_set_block / vm_set_word can store */
      if (srclen + tl + 2 >= cap) { complain(err, errcap, &bad, lineno, "source too long in", kind == 1 ? "block" : "word"); continue; }
      if (srclen) src[srclen++] = '\n';
      memcpy(src + srclen, t, tl); srclen += tl; src[srclen] = '\0';
      continue;
    }
    if (kind) {                                                /* any other line ends the source: store it */
      if (kind == 1) vm_set_block(&s->vm, index, src); else vm_set_word(&s->vm, index, src);
      kind = 0;
    }
    if (!line[0] || line[0] == '#') continue;

    /* split the line into its keyword and the rest. rest is as big as line, so the copy always fits. */
    char word[32] = "", rest[sizeof line] = "";
    sscanf(line, "%31s %[^\n]", word, rest);
    int a, b, c, d, e;
    char nm[32], sn[32];

    if (!strcmp(word, "name")) copy_text(info->name, sizeof info->name, rest);
    else if (!strcmp(word, "desc")) copy_text(info->desc, sizeof info->desc, rest);
    else if (!strcmp(word, "listen")) copy_text(info->listen, sizeof info->listen, rest);
    else if (!strcmp(word, "level") && sscanf(rest, "%d", &a) == 1) s->master_level = fez_clamp(a, 0, 32767);
    else if (!strcmp(word, "cutoff")) { }                       /* files before 0.4.9 carry an unused "cutoff 0" line: read and ignored */
    else if (!strcmp(word, "pulser") && sscanf(rest, "%d on %d rate %d", &a, &b, &c) == 3 && a >= 0 && a < NUM_PULSERS) {
      s->pulsers[a].on = b != 0; s->mem[REG_PRATE0 + a] = (int16_t)fez_clamp(c, 10, 600);
    }
    else if (!strcmp(word, "reg") && sscanf(rest, "%31s %d", nm, &a) == 2 && !strcmp(nm, "FMRATE")) fmrate = a;   /* gone: see step 3 */
    else if (!strcmp(word, "reg") && sscanf(rest, "%31s %d", nm, &a) == 2) {
      int addr = addr_of(nm);
      if (addr < 0) complain(err, errcap, &bad, lineno, "unknown register", nm);
      else {
        /* Through fez_write, like a block's "!": the same range checks, so a file cannot hold a value code could not
           write (BUFMS 0 would divide by zero). Read-only registers are refused. fez_write also handles TMBITS. */
        unsigned f = 0;
        fez_write(s, addr, a, 0, &f);
        if (f) complain(err, errcap, &bad, lineno, "register cannot be written:", nm);
      }
    }
    else if (!strcmp(word, "route") && sscanf(rest, "%31s %31s %d", sn, nm, &b) == 3) {
      int dst = addr_of(nm);
      a = fez_source_index(sn);
      if (dst < 0 || a < 1 || s->nroutes >= MAX_ROUTES || s->param_index[dst] < 0) complain(err, errcap, &bad, lineno, "bad route", rest);
      else { s->routes[s->nroutes].src = a; s->routes[s->nroutes].dst = dst; s->routes[s->nroutes].w = fez_clamp(b, -32768, 32767); s->nroutes++; }
    }
    /* the devices: through their own register writers, so the same limits apply as for code (a trig of -1 would shift by -1) */
    else if (!strcmp(word, "ca") && sscanf(rest, "%d %d %d %d", &a, &b, &c, &d) == 4) {
      unsigned f = 0;
      fez_ca_write(s, REG_CARULE, a & 255, &f); fez_ca_write(s, REG_CAROW, b, &f);
      fez_ca_write(s, REG_CATAP, c, &f);        fez_ca_write(s, REG_CATRIG, d, &f);
    }
    else if (!strcmp(word, "lz") && sscanf(rest, "%d %d %d", &a, &b, &c) == 3) {
      unsigned f = 0;
      fez_lz_write(s, REG_LZRATE, a, &f); fez_lz_write(s, REG_LZRHO, b, &f); fez_lz_write(s, REG_LZTRIG, c, &f);
    }
    else if (!strcmp(word, "block") && sscanf(rest, "%d", &e) == 1 && e >= 0 && e < NUM_BLOCKS) { kind = 1; index = e; src[0] = '\0'; srclen = 0; }
    else if (!strcmp(word, "word") && sscanf(rest, "%d", &e) == 1 && e >= 0 && e < NUM_WORDS) { kind = 2; index = e; src[0] = '\0'; srclen = 0; }
    else complain(err, errcap, &bad, lineno, "cannot read", line);
  }
  if (kind) { if (kind == 1) vm_set_block(&s->vm, index, src); else vm_set_word(&s->vm, index, src); }

  /* 3) FMRATE, the mod oscillator's speed on its own log scale, is gone (version 3); MDPITCH tunes that oscillator now. An
        older file keeps the frequency it had, as a pitch (8.8 is MIDI note x 256; 69 is A at 440 Hz; below about 8 Hz the
        pitch bottoms out at 0):
        - version 1: one oscillator, set by FMRATE, so always;
        - version 2 (FEZ 0.4.1, when FM and the mod voice had separate oscillators): only if the FM is heard (FMAMT above 0)
          and the mod voice is not (MDLVL 0). If both are heard the mod voice keeps its pitch and the FM moves with it. */
  if (version < 2 || (version == 2 && s->mem[REG_FMAMT] > 0 && s->mem[REG_MDLVL] == 0)) {
    double hz = fm_hz(fmrate);
    fez_reg_write(s, REG_MDPITCH, (int)lround((69.0 + 12.0 * log2(hz / 440.0)) * 256.0));
  }

  /* 4) state that follows from the registers */
  s->bw.state = s->mem[REG_BWMODE] == 1 ? 1 : 0;
  fez_compute_eff(s);
  return bad;
}

void patchfile_info(const char *text, PatchInfo *info) {
  info->name[0] = info->desc[0] = info->listen[0] = '\0';
  static const struct { const char *key; size_t off, cap; } F[] = {
    { "name ", offsetof(PatchInfo, name), PATCH_NAME_MAX }, { "desc ", offsetof(PatchInfo, desc), PATCH_TEXT_MAX },
    { "listen ", offsetof(PatchInfo, listen), PATCH_TEXT_MAX } };
  for (const char *p = text; *p; ) {
    size_t len = strcspn(p, "\n");
    for (int i = 0; i < 3; i++) {
      size_t kl = strlen(F[i].key);
      if (len >= kl && !strncmp(p, F[i].key, kl)) {
        size_t n = len - kl < F[i].cap - 1 ? len - kl : F[i].cap - 1;
        memcpy((char *)info + F[i].off, p + kl, n); ((char *)info)[F[i].off + n] = '\0';
      }
    }
    if (!strncmp(p, "block ", 6) || !strncmp(p, "word ", 5)) break;       /* the header ends where the code starts */
    p += len; if (*p) p++;
  }
}
