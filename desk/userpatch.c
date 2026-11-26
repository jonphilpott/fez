/* User patch files on disk: see userpatch.h. Plain stdio plus opendir (POSIX: macOS and Linux; MinGW supplies it on Windows). */
#include "userpatch.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>          /* strcasecmp: POSIX, and glibc hides it from <string.h> under -std=c99 */
#include <sys/stat.h>

int userpatch_name_char_ok(int ch) { return isalnum(ch) || ch == ' ' || ch == '-' || ch == '_' || ch == '.'; }

static int path_for(char *out, size_t cap, const char *dir, const char *name) {
  if (!dir[0] || !name[0] || name[0] == '.') return -1;                    /* no folder, no name, or a hidden file */
  for (const char *c = name; *c; c++) if (!userpatch_name_char_ok((unsigned char)*c)) return -1;
  return snprintf(out, cap, "%s%s.fez", dir, name) < (int)cap ? 0 : -1;
}

static int by_name(const void *a, const void *b) { return strcasecmp(a, b); }

int userpatch_list(const char *dir, char names[][PATCH_NAME_MAX], int max) {
  int n = 0;
  DIR *d = dir[0] ? opendir(dir) : NULL;
  if (!d) return 0;
  struct dirent *e;
  while (n < max && (e = readdir(d))) {
    size_t len = strlen(e->d_name);
    if (len < 5 || strcmp(e->d_name + len - 4, ".fez") != 0 || len - 4 >= PATCH_NAME_MAX || e->d_name[0] == '.') continue;
    memcpy(names[n], e->d_name, len - 4); names[n][len - 4] = '\0';
    n++;
  }
  closedir(d);
  qsort(names, (size_t)n, PATCH_NAME_MAX, by_name);
  return n;
}

int userpatch_exists(const char *dir, const char *name) {
  char path[600]; struct stat st;
  return path_for(path, sizeof path, dir, name) == 0 && stat(path, &st) == 0;
}

int userpatch_read(const char *dir, const char *name, char *buf, size_t cap) {
  char path[600];
  if (path_for(path, sizeof path, dir, name) != 0) return -1;
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  size_t n = fread(buf, 1, cap - 1, f);
  int too_big = !feof(f);                                                  /* more left than fits: refuse rather than load half */
  fclose(f);
  if (too_big) return -1;
  buf[n] = '\0';
  return (int)n;
}

int userpatch_write(const char *dir, const char *name, const char *text) {
  char path[600], tmp[610];
  if (path_for(path, sizeof path, dir, name) != 0) return -1;
  /* Write a temporary file and rename it over the real one, so a crash mid-write never leaves half a patch. */
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (!f) return -1;
  size_t len = strlen(text);
  int ok = fwrite(text, 1, len, f) == len;
  ok = fclose(f) == 0 && ok;
#ifdef _WIN32
  remove(path);                    /* Windows' rename will not replace an existing file. ponytail: a crash between the two calls loses the old copy; MoveFileEx would be atomic */
#endif
  if (!ok || rename(tmp, path) != 0) { remove(tmp); return -1; }
  return 0;
}

int userpatch_import(const char *from, const char *to) {
  static char names[256][PATCH_NAME_MAX];
  static char text[PATCH_TEXT_BUF];
  int n = userpatch_list(from, names, 256), copied = 0;
  for (int i = 0; i < n; i++)
    if (userpatch_read(from, names[i], text, sizeof text) >= 0 && userpatch_write(to, names[i], text) == 0) copied++;
  return copied;
}

int userpatch_delete(const char *dir, const char *name) {
  char path[600];
  return path_for(path, sizeof path, dir, name) == 0 && remove(path) == 0 ? 0 : -1;
}
