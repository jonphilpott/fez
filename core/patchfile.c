/* Patch files: see patchfile.h for the format. */
#include "patchfile.h"
#include "regview.h"
#include <ctype.h>
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
  put(&o, "level %d\ncutoff %d\n", s->level_pot, s->cutoff_add);
  for (int i = 0; i < NUM_PULSERS; i++)
    put(&o, "pulser %d on %d rate %d\n", i, s->pulsers[i].on ? 1 : 0, (int)(s->pulsers[i].rate + 0.5));

  /* registers: only the named, writable ones, and only those that differ from the defaults */
  RegEntry list[NUM_REGS];
  int n = regview_list(list, NUM_REGS);
  for (int i = 0; i < n; i++) {
    int a = list[i].addr;
    /* TMBITS is the one register whose value lives outside the register file (in s->tm), with its own default */
    int value = a == REG_TMBITS ? (int16_t)s->tm.bits : s->mem[a], dflt = a == REG_TMBITS ? 0x5ac3 : def[a];
    if (list[i].ro || list[i].wo || value == dflt) continue;
    put(&o, "reg %s %d\n", list[i].name, value);
  }
  for (int i = 0; i < s->nroutes; i++) {
    const char *dst = name_of(s->routes[i].dst);
    put(&o, "route %d %s %d\n", s->routes[i].src, dst ? dst : "?", s->routes[i].w);
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
  info->name[0] = info->desc[0] = info->listen[0] = '\0';

  /* 2) one line at a time */
  int bad = 0, lineno = 0;
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
      if (srclen + tl + 2 >= sizeof src) { complain(err, errcap, &bad, lineno, "source too long in", kind == 1 ? "block" : "word"); continue; }
      if (srclen) src[srclen++] = '\n';
      memcpy(src + srclen, t, tl); srclen += tl; src[srclen] = '\0';
      continue;
    }
    if (kind) {                                                /* any other line ends the source: store it */
      if (kind == 1) vm_set_block(&s->vm, index, src); else vm_set_word(&s->vm, index, src);
      kind = 0;
    }
    if (!line[0] || line[0] == '#') continue;

    char word[32] = "", rest[BLOCK_SRC_MAX] = "";
    sscanf(line, "%31s %[^\n]", word, rest);
    int a, b, c, d, e;
    char nm[32];

    if (!strcmp(word, "name")) copy_text(info->name, sizeof info->name, rest);
    else if (!strcmp(word, "desc")) copy_text(info->desc, sizeof info->desc, rest);
    else if (!strcmp(word, "listen")) copy_text(info->listen, sizeof info->listen, rest);
    else if (!strcmp(word, "level") && sscanf(rest, "%d", &a) == 1) s->level_pot = fez_clamp(a, 0, 32767);
    else if (!strcmp(word, "cutoff") && sscanf(rest, "%d", &a) == 1) s->cutoff_add = a;
    else if (!strcmp(word, "pulser") && sscanf(rest, "%d on %d rate %d", &a, &b, &c) == 3 && a >= 0 && a < NUM_PULSERS) {
      s->pulsers[a].on = b != 0; s->pulsers[a].rate = fez_clamp(c, 10, 600);
    }
    else if (!strcmp(word, "reg") && sscanf(rest, "%31s %d", nm, &a) == 2) {
      int addr = addr_of(nm);
      if (addr < 0) complain(err, errcap, &bad, lineno, "unknown register", nm);
      else if (addr == REG_TMBITS) s->tm.bits = a & 0xffff;
      else s->mem[addr] = (int16_t)fez_clamp(a, -32768, 32767);
    }
    else if (!strcmp(word, "route") && sscanf(rest, "%d %31s %d", &a, nm, &b) == 3) {
      int dst = addr_of(nm);
      if (dst < 0 || a < 1 || a >= SRC_COUNT || s->nroutes >= MAX_ROUTES || s->param_index[dst] < 0) complain(err, errcap, &bad, lineno, "bad route", rest);
      else { s->routes[s->nroutes].src = a; s->routes[s->nroutes].dst = dst; s->routes[s->nroutes].w = b; s->nroutes++; }
    }
    else if (!strcmp(word, "ca") && sscanf(rest, "%d %d %d %d", &a, &b, &c, &d) == 4) { s->ca.rule = a & 255; s->ca.row = b & 0xFFFF; s->ca.tap = fez_clamp(c, 0, 15); s->ca.trig = d; }
    else if (!strcmp(word, "lz") && sscanf(rest, "%d %d %d", &a, &b, &c) == 3) { s->lz.rate = a; s->lz.rho = b; s->lz.trig = c; }
    else if ((!strcmp(word, "block") || !strcmp(word, "word")) && sscanf(rest, "%d", &e) == 1 && e >= 0 && e < NUM_BLOCKS) {
      kind = word[0] == 'b' ? 1 : 2; index = e; src[0] = '\0'; srclen = 0;
    }
    else complain(err, errcap, &bad, lineno, "cannot read", line);
  }
  if (kind) { if (kind == 1) vm_set_block(&s->vm, index, src); else vm_set_word(&s->vm, index, src); }

  /* 3) state that follows from the registers */
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
