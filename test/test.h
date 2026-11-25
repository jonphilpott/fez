/* Tiny test helpers shared by the test files. No framework: a counter and a macro. Tests run from the repo root (make test). */
#ifndef FEZ_TEST_H
#define FEZ_TEST_H

#include <stddef.h>

extern int test_fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); test_fails++; } } while (0)

void test_tables(void);
void test_patchfile(void);
void test_audio(void);
void test_pad(void);

#endif
