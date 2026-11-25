/* Builds the manual: docs/manual.html, one page, from
     docs/template.html     the page skeleton, with {{STYLE}} {{TITLE}} {{NAV}} {{CONTENT}} marking where things go
     docs/style.css         the look (its colours are NOT in it: they are written in front of it from core/palette.c)
     docs/chapters/ (html)    the prose, one file per chapter, written by hand
     the tables in core/    the word reference (core/words.c) and the register reference (core/tables.c, core/regview.c)
   so a colour, a register or a word has one definition and the manual cannot drift from the program.

   Run from the repo root:   make manual        (or:  ./manual docs/manual.html)

   Two helpers make the prose easier to write and keep honest:
   - Inside a chapter, <pre class="fez"> ... </pre> holds FEZ source as plain text. This tool colours it with the same
     rules the editor uses (regview_token_kind), so a word is gold, a register cyan, a note green. Write it exactly as you
     would type it, including < and >: the tool escapes it for the page.
   - Headings are drawn in the app's own 5x8 pixel font as little SVG pictures made from FONT5. The real text stays beside
     the picture, hidden, so searching, screen readers and copying still work. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fez.h"
#include "palette.h"
#include "regview.h"
#include "reg_names.h"
#include "tables.h"
#include "words.h"

/* ---------------------------------------------------------------- the chapters, in order */

/* file: the hand-written prose. A chapter with no file is generated from the tables, and its id says which. */
typedef struct { const char *id, *title, *file; } Chapter;
static const Chapter CHAPTERS[] = {
  { "first-sound", "First sound",                  "docs/chapters/01_first_sound.html" },
  { "overview",    "How it fits together",         "docs/chapters/02_overview.html" },
  { "language",    "The language",                 "docs/chapters/03_language.html" },
  { "voice",       "The voice",                    "docs/chapters/04_voice.html" },
  { "control",     "Control",                      "docs/chapters/05_control.html" },
  { "devices",     "Devices",                      "docs/chapters/06_devices.html" },
  { "space",       "Space and time",               "docs/chapters/07_space.html" },
  { "playing",     "Playing it",                   "docs/chapters/08_playing.html" },
  { "handheld",    "The handheld",                 "docs/chapters/09_handheld.html" },
  { "patches",     "Patches",                      "docs/chapters/10_patches.html" },
  { "examples",    "The example patches",          "docs/chapters/11_examples.html" },
  { "words",       "Word reference",               NULL },
  { "registers",   "Register reference",           NULL },
};
#define NCHAPTERS ((int)(sizeof CHAPTERS / sizeof CHAPTERS[0]))

/* ---------------------------------------------------------------- small helpers */

/* Read a whole file into a new string. This is a build tool, not the synth, so malloc is fine here. */
static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "manual: cannot open %s (run it from the repo root)\n", path); exit(1); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  rewind(f);
  char *buf = malloc((size_t)n + 1);
  if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "manual: cannot read %s\n", path); exit(1); }
  buf[n] = '\0';
  fclose(f);
  return buf;
}

/* Write n characters as HTML text: the three characters that mean something to a browser are replaced. */
static void esc(FILE *out, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '<') fputs("&lt;", out);
    else if (s[i] == '>') fputs("&gt;", out);
    else if (s[i] == '&') fputs("&amp;", out);
    else fputc(s[i], out);
  }
}
static void esc_str(FILE *out, const char *s) { esc(out, s, strlen(s)); }

/* ---------------------------------------------------------------- pixel headings */

/* Draw text in the 5x8 bitmap font as an SVG. Each glyph is 8 rows of 5 bits (bit 4 is the leftmost pixel), and a character
   takes 6 columns (5 plus a gap). Each row is written as runs of lit pixels: "M x y h w v 1 h -w z" is a w by 1 rectangle.
   The text is shown in capitals, as in the app's tab bar. CSS sets the size of one font pixel (--px) and the picture's width
   in pixels (--w), so the same picture serves every heading size. */
static void pixel_text(FILE *out, const char *text) {
  int n = (int)strlen(text), width = n * 6 - 1;
  fprintf(out, "<svg class=\"px\" style=\"--w:%d\" viewBox=\"0 0 %d 8\" aria-hidden=\"true\" focusable=\"false\"><path d=\"", width, width);
  for (int i = 0; i < n; i++) {
    int ch = toupper((unsigned char)text[i]);
    if (ch < 32 || ch > 126) ch = '?';
    const unsigned char *glyph = FONT5[ch - 32];
    for (int y = 0; y < 8; y++) {
      int x = 0;
      while (x < 5) {
        if (!((glyph[y] >> (4 - x)) & 1)) { x++; continue; }
        int run = 0;
        while (x + run < 5 && ((glyph[y] >> (4 - (x + run))) & 1)) run++;
        fprintf(out, "M%d %dh%dv1h-%dz", i * 6 + x, y, run, run);
        x += run;
      }
    }
  }
  fputs("\"/></svg>", out);
}

/* A heading: the picture for the eye, the real text (hidden) for everything else. */
static void pixel_heading(FILE *out, const char *tag, const char *id, const char *number, const char *text) {
  fprintf(out, "<%s id=\"%s-h\">", tag, id);
  if (number) fprintf(out, "<span class=\"num\">%s</span>", number);
  pixel_text(out, text);
  fputs("<span class=\"sr\">", out); esc_str(out, text); fprintf(out, "</span></%s>\n", tag);
}

/* ---------------------------------------------------------------- FEZ source, coloured like the editor */

/* The class for each kind of token (see regview_token_kind): the editor's own choices, gold words, cyan registers, green notes. */
static const char *token_class(int kind) {
  switch (kind) { case TOK_OP: return "op"; case TOK_REG: return "reg"; case TOK_NOTE: return "note"; case TOK_NUM: return "num"; default: return NULL; }
}

static void fez_code(FILE *out, const char *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    if (s[i] == '\\') {                                           /* a comment runs to the end of the line */
      size_t e = i;
      while (e < n && s[e] != '\n') e++;
      fputs("<span class=\"cmt\">", out); esc(out, s + i, e - i); fputs("</span>", out);
      i = e;
    } else if (isspace((unsigned char)s[i])) {
      fputc(s[i++], out);
    } else {
      size_t e = i;
      while (e < n && !isspace((unsigned char)s[e]) && s[e] != '\\') e++;
      char token[64];
      size_t len = e - i < sizeof token - 1 ? e - i : sizeof token - 1;
      memcpy(token, s + i, len); token[len] = '\0';
      const char *cls = token_class(regview_token_kind(token));
      if (cls) fprintf(out, "<span class=\"%s\">", cls);
      esc(out, s + i, e - i);
      if (cls) fputs("</span>", out);
      i = e;
    }
  }
}

/* Every example in the manual must compile. If the language changes and an example stops being valid, the build of the manual
   stops here and names it, instead of printing a wrong example. (Compiling is not running: an example may still read a
   register that holds nothing yet. But it catches the typos and the removed features.) */
static void check_example(const char *code, size_t n) {
  char src[BLOCK_SRC_MAX];
  static Compiled compiled;
  if (n >= sizeof src) { fprintf(stderr, "manual: an example is too long to be a block\n"); exit(1); }
  memcpy(src, code, n); src[n] = '\0';
  vm_compile(&compiled, src, BLOCK_SLOTS);
  if (!compiled.ok) { fprintf(stderr, "manual: an example does not compile (%s):\n%s\n", compiled.err, src); exit(1); }
}

/* Copy a chapter's prose to the page, colouring the inside of every <pre class="fez"> block on the way. */
static void chapter_prose(FILE *out, const char *text) {
  static const char OPEN[] = "<pre class=\"fez\">", CLOSE[] = "</pre>";
  const char *p = text;
  for (;;) {
    const char *start = strstr(p, OPEN);
    if (!start) { fputs(p, out); return; }
    fwrite(p, 1, (size_t)(start - p), out);
    const char *code = start + sizeof OPEN - 1, *end = strstr(code, CLOSE);
    if (!end) { fprintf(stderr, "manual: a <pre class=\"fez\"> has no closing </pre>\n"); exit(1); }
    check_example(code, (size_t)(end - code));
    fputs("<pre class=\"fez\"><code>", out);
    fez_code(out, code, (size_t)(end - code));
    fputs("</code></pre>", out);
    p = end + sizeof CLOSE - 1;
  }
}

/* ---------------------------------------------------------------- the generated references */

/* True if every character is a letter: such a word can be linked to as #w-NAME. The others (the symbols, like the star-slash
   word and @) have no anchor, because a symbol makes an awkward link. */
static int all_letters(const char *s) { for (; *s; s++) if (!isalpha((unsigned char)*s)) return 0; return 1; }

static void word_reference(FILE *out) {
  fputs("<p>Every word of the language, with its stack picture. The top of the stack is on the right: <code>( a b -- c )</code> "
        "means <code>b</code> was on top, and <code>c</code> is left in place of both. Numbers are 16 bit and signed, and a "
        "result that goes past the ends sticks there instead of wrapping (see <a href=\"#saturation\">Numbers stop at the ends</a>; "
        "the bit words are the exception). Pitches are numbers "
        "too, 256 to a semitone: see <a href=\"#pitch\">Pitch</a>.</p>\n", out);
  for (int i = 0; i < WORD_COUNT; i++) {
    const WordDef *w = &WORD_TABLE[i];
    fputs("<div class=\"ref ref-row\"", out);
    if (all_letters(w->name)) fprintf(out, " id=\"w-%s\"", w->name);
    fputs("><span class=\"name\">", out); esc_str(out, w->name);
    fputs("</span><span class=\"stack\">", out); esc_str(out, w->stack);
    fputs("</span>", out);
    /* A sentence that starts "WATCH OUT" is a trap, and goes in a red box of its own. */
    const char *trap = strstr(w->text, "WATCH OUT: ");
    if (trap) {
      fputs("<p>", out); esc(out, w->text, (size_t)(trap - w->text)); fputs("</p><p class=\"warn\">", out);
      esc_str(out, trap + strlen("WATCH OUT: "));
      fputs("</p>", out);
    } else {
      fputs("<p>", out); esc_str(out, w->text); fputs("</p>", out);
    }
    fputs("</div>\n", out);
  }
}

static void register_reference(FILE *out) {
  static RegEntry regs[512];
  int n = regview_list(regs, 512);
  fputs("<p>Every register, in address order under the same headings as the CTRL tab. A register's name is its address, so "
        "<code>CUTOFF @</code> reads it and <code>12000 CUTOFF !</code> writes it. Most hold 0 to 32767. "
        "(The per-block registers P0 to P15 are private to each block and are not listed.)</p>\n", out);
  fputs("<table class=\"regs\"><tbody>\n", out);
  int g = 0;
  for (int i = 0; i < n; i++) {
    while (g < REG_GROUP_COUNT && regs[i].addr >= REG_GROUPS[g].first) {     /* a heading before the first register of each group */
      fprintf(out, "<tr class=\"grp\"><th colspan=\"5\">%s</th></tr>\n", REG_GROUPS[g].name);
      g++;
    }
    const RegEntry *e = &regs[i];
    char def[32] = "", units[48] = "";
    for (int k = 0; k < PARAM_COUNT; k++)
      if (PARAM_TABLE[k].addr == e->addr) {
        snprintf(def, sizeof def, "%d", PARAM_TABLE[k].def);
        regview_fmt_param(units, sizeof units, e->addr, PARAM_TABLE[k].def);
        break;
      }
    const char *how = e->ro ? "read only" : e->wo ? "write only" : e->is_signed ? "signed" : "";
    fprintf(out, "<tr class=\"ref-row\"><td class=\"name\" id=\"r-%s\">%s</td><td class=\"addr\">%d</td><td class=\"def\">", e->name, e->name, e->addr);
    esc_str(out, def);
    if (units[0] && strcmp(units, def) != 0) { fputs(" = ", out); esc_str(out, units); }
    fprintf(out, "</td><td class=\"def\">%s</td><td>", how);
    esc_str(out, regview_help(e->name));
    fputs("</td></tr>\n", out);
  }
  fputs("</tbody></table>\n", out);
}

/* ---------------------------------------------------------------- the page */

/* The palette as CSS variables, from the one table the screens use. */
static void palette_css(FILE *out) {
  static const char *const NAMES[COL_COUNT] = { "bg", "fg", "dim", "amb", "cyn", "red", "grn", "pnl", "sel", "ambd", "line", "mag" };
  fputs(":root {\n", out);
  for (int i = 0; i < COL_COUNT; i++)
    fprintf(out, "  --%s: #%02x%02x%02x;\n", NAMES[i], FEZ_PALETTE[i][0], FEZ_PALETTE[i][1], FEZ_PALETTE[i][2]);
  fputs("}\n", out);
}

static void contents(FILE *out) {
  for (int i = 0; i < NCHAPTERS; i++) {
    fprintf(out, "    <a href=\"#%s\"><span class=\"n\">%02d</span>", CHAPTERS[i].id, i + 1);
    esc_str(out, CHAPTERS[i].title);
    fputs("</a>\n", out);
  }
}

/* ---------------------------------------------------------------- pictures
   A chapter may hold <img src="shots/NAME.png">. The page must stay one file, so each such picture is replaced by the picture's
   own bytes written as text (base64, a way of spelling bytes with 64 safe letters; a "data URI" in a web page). The
   pictures are made by tools/shots.sh and kept in docs/shots/. A missing one stops the build rather than leaving a hole. */
static void base64(const unsigned char *in, size_t n, char *out) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = (unsigned)in[i] << 16 | (i + 1 < n ? (unsigned)in[i + 1] << 8 : 0u) | (i + 2 < n ? in[i + 2] : 0u);
    *out++ = T[v >> 18]; *out++ = T[(v >> 12) & 63];
    *out++ = i + 1 < n ? T[(v >> 6) & 63] : '=';
    *out++ = i + 2 < n ? T[v & 63] : '=';
  }
  *out = '\0';
}

/* Returns a new copy of html with every src="shots/NAME.png" replaced; the caller frees it. */
static char *embed_pictures(const char *html) {
  static const char KEY[] = "src=\"shots/";
  size_t cap = strlen(html) + 1, len = 0;
  char *out = malloc(cap);
  if (!out) { fprintf(stderr, "manual: out of memory\n"); exit(1); }
  while (*html) {
    const char *hit = strstr(html, KEY);
    size_t upto = hit ? (size_t)(hit - html) : strlen(html);
    if (len + upto + 1 > cap) { cap = (len + upto + 1) * 2; out = realloc(out, cap); }
    memcpy(out + len, html, upto); len += upto; out[len] = '\0';
    if (!hit) break;
    const char *name = hit + sizeof KEY - 1, *end = strchr(name, '"');
    if (!end || end - name > 80) { fprintf(stderr, "manual: a picture name is not closed\n"); exit(1); }
    char path[160], file[96];
    snprintf(file, sizeof file, "%.*s", (int)(end - name), name);
    snprintf(path, sizeof path, "docs/shots/%s", file);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "manual: missing picture %s (run tools/shots.sh)\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    unsigned char *bytes = malloc((size_t)n);
    if (!bytes || fread(bytes, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "manual: cannot read %s\n", path); exit(1); }
    fclose(f);
    size_t need = len + (size_t)n * 4 / 3 + 64 + strlen(end) + 1;
    if (need > cap) { cap = need * 2; out = realloc(out, cap); }
    len += (size_t)sprintf(out + len, "src=\"data:image/png;base64,");
    base64(bytes, (size_t)n, out + len); len += (size_t)((n + 2) / 3 * 4);
    free(bytes);
    html = end;                                                  /* the closing quote is copied on the next pass */
  }
  return out;
}

static void all_chapters(FILE *out) {
  for (int i = 0; i < NCHAPTERS; i++) {
    const Chapter *c = &CHAPTERS[i];
    char number[8];
    snprintf(number, sizeof number, "%02d", i + 1);
    fprintf(out, "<section class=\"chapter\" id=\"%s\">\n", c->id);
    pixel_heading(out, "h2", c->id, number, c->title);
    if (c->file) { char *raw = read_file(c->file), *prose = embed_pictures(raw); chapter_prose(out, prose); free(prose); free(raw); }
    else if (strcmp(c->id, "words") == 0) word_reference(out);
    else if (strcmp(c->id, "registers") == 0) register_reference(out);
    fputs("</section>\n", out);
  }
}

/* Copy the template to the output, filling each {{NAME}} marker. */
static void fill_template(FILE *out, const char *tpl) {
  const char *p = tpl;
  for (;;) {
    const char *open = strstr(p, "{{");
    if (!open) { fputs(p, out); return; }
    fwrite(p, 1, (size_t)(open - p), out);
    const char *close = strstr(open, "}}");
    if (!close) { fprintf(stderr, "manual: unclosed {{ in the template\n"); exit(1); }
    char name[32];
    size_t len = (size_t)(close - open - 2) < sizeof name - 1 ? (size_t)(close - open - 2) : sizeof name - 1;
    memcpy(name, open + 2, len); name[len] = '\0';
    if (strcmp(name, "STYLE") == 0)        { char *css = read_file("docs/style.css"); palette_css(out); fputs(css, out); free(css); }
    else if (strcmp(name, "TITLE") == 0)   { pixel_text(out, "FEZ"); fputs("<span class=\"sr\">FEZ</span>", out); }
    else if (strcmp(name, "NAV") == 0)     contents(out);
    else if (strcmp(name, "CONTENT") == 0) all_chapters(out);
    else { fprintf(stderr, "manual: unknown marker {{%s}}\n", name); exit(1); }
    p = close + 2;
  }
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "docs/manual.html";
  char *tpl = read_file("docs/template.html");
  FILE *out = fopen(path, "wb");
  if (!out) { fprintf(stderr, "manual: cannot write %s\n", path); return 1; }
  fill_template(out, tpl);
  fclose(out);
  free(tpl);
  printf("wrote %s\n", path);
  return 0;
}
