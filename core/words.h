/* The description of every word of the FEZ language (the operations of vm.c), kept in one table so the manual, the editor's
   palette and the tests all read the same text. The table runs in the same order as OP_NAMES in vm.c: a test checks that. */
#ifndef FEZ_WORDS_H
#define FEZ_WORDS_H

typedef struct {
  const char *name;    /* as typed (the same string as OP_NAMES) */
  const char *stack;   /* the stack picture: what it takes and leaves, top of the stack on the right */
  const char *text;    /* what it does, in a sentence or two */
} WordDef;

extern const WordDef WORD_TABLE[];
extern const int WORD_COUNT;

#endif
