/* User patch files: a folder of "<name>.fez" text files. The only code in the desktop app that touches the disk
   for patches; patchfile.c (in the core) does the text. Names are limited to letters, digits, space, - _ and .
   so a patch name is always a safe file name. The folder path ends in a slash. */
#ifndef FEZ_USERPATCH_H
#define FEZ_USERPATCH_H

#include <stddef.h>
#include "patchfile.h"

#define PATCH_TEXT_BUF 65536     /* room for a whole patch file: 16 blocks of 1280 characters, 16 words, and the registers */

/* Fill names with the patch names in dir, sorted, without the ".fez". Returns the count (at most max). */
int  userpatch_list(const char *dir, char names[][PATCH_NAME_MAX], int max);
int  userpatch_exists(const char *dir, const char *name);
int  userpatch_read(const char *dir, const char *name, char *buf, size_t cap);     /* length, or -1 */
int  userpatch_read_path(const char *path, char *buf, size_t cap);                 /* the same, for a file anywhere */
int  userpatch_write(const char *dir, const char *name, const char *text);         /* 0 ok, -1 failed */
int  userpatch_delete(const char *dir, const char *name);                          /* 0 ok, -1 failed */
int  userpatch_name_char_ok(int ch);
int  userpatch_import(const char *from, const char *to);                           /* copy every .fez from one folder to another; returns how many */

#endif
