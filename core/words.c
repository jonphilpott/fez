/* The words of the FEZ language, described. See words.h. Written from vm.c (the interpreter is the authority: if a line here
   disagrees with it, the line is wrong).

   Things true of every word, said once here and in the manual rather than on every line:
   - Numbers are 16 bit and signed, -32768 to 32767. Results that go past that stick at the end (they saturate; they do not wrap),
     except where a line says otherwise (AND, OR, XOR, NOT and SHR work on the 16 bits as they are).
   - The stack holds 16 numbers. Taking from an empty stack or pushing onto a full one stops the block (a fault).
   - A stack picture shows the top of the stack on the right: ( a b -- c ) means b was on top, and c is left in place of both. */
#include "words.h"
#include "vm.h"

const WordDef WORD_TABLE[] = {
  /* ---- the stack ---- */
  { "DUP",   "( a -- a a )",        "Copy the top number." },
  { "DROP",  "( a -- )",            "Throw the top number away." },
  { "SWAP",  "( a b -- b a )",      "Swap the top two numbers." },
  { "OVER",  "( a b -- a b a )",    "Copy the second number to the top." },
  { "ROT",   "( a b c -- b c a )",  "Bring the third number to the top." },

  /* ---- arithmetic ---- */
  { "+",     "( a b -- a+b )",      "Add. A sum past 32767 stays at 32767, and one below -32768 stays there: numbers stick at the ends, they do not wrap." },
  { "-",     "( a b -- a-b )",      "Subtract the top from the one below. Sticks at the ends of the range like +." },
  { "*",     "( a b -- a*b )",      "Multiply whole numbers: 6 7 * leaves 42. Sticks at the ends of the range like + and -. To scale by a fraction use F*." },
  { "F*",    "( a b -- a*b )",      "Multiply as fractions: 32768 counts as 1.0, so .5 .5 F* leaves .25 (8192). The way to scale a register by a fraction, or to turn one level down by another. 20000 .5 F* leaves 10000." },
  { "/",     "( a b -- a/b )",      "Divide whole numbers, rounding towards zero. Dividing by zero leaves 0 and flags a fault." },
  { "*/",    "( a b c -- a*b/c )",  "Multiply a by b and divide by c, keeping the full product in between, so nothing sticks until the final answer (unlike * followed by /). Use it for ratios: 100 3 4 */ leaves 75." },
  { "NEG",   "( a -- -a )",         "Change the sign. -32768 has no positive twin, so it becomes 32767." },
  { "ABS",   "( a -- |a| )",        "Drop the sign: the distance from zero. -32768 becomes 32767." },
  { "MIN",   "( a b -- smaller )",  "The smaller of the two." },
  { "MAX",   "( a b -- larger )",   "The larger of the two." },
  { "MOD",   "( a b -- a mod b )",  "The remainder after dividing by b. For a positive b the answer is 0 up to b-1, even if a is negative. Good for counting round a loop." },

  /* ---- bits ---- */
  { "AND",   "( a b -- a&b )",      "Keep the bits set in both numbers. Use it to mask or test a bit." },
  { "OR",    "( a b -- a|b )",      "Set every bit that is set in either." },
  { "XOR",   "( a b -- a^b )",      "Set the bits that differ. XOR with a bit twice puts it back: a toggle." },
  { "NOT",   "( a -- ~a )",         "Flip every one of the 16 bits." },
  { "SHL",   "( a n -- a<<n )",     "Shift the bits left n places, which doubles the number n times (the same as multiplying by a power of two). The result sticks at the end of the range. Only the low four bits of n count." },
  { "SHR",   "( a n -- a>>n )",     "Shift the 16 bits right n places and fill with zeros, so a negative number becomes a large positive one. Only the low four bits of n count." },
  { "ROL",   "( a n -- rotated )",  "Rotate the 16 bits left n places: the bits that fall off the top come back in at the bottom, so none are lost. Slides a pattern round a ring, such as a rhythm mask (SQEN) or the automaton's row (CAROW). Only the low four bits of n count." },
  { "ROR",   "( a n -- rotated )",  "Rotate the 16 bits right n places: the bits that fall off the bottom come back in at the top. The mirror of ROL." },

  /* ---- comparing: each leaves 1 for true and 0 for false ---- */
  { "=",     "( a b -- flag )",     "1 if equal." },
  { "<",     "( a b -- flag )",     "1 if a is less than b." },
  { ">",     "( a b -- flag )",     "1 if a is greater than b." },
  { "U<",    "( a b -- flag )",     "1 if a is less than b when both are read as unsigned 16-bit numbers (0 to 65535). For bit patterns." },

  /* ---- registers ---- */
  { "@",     "( addr -- value )",   "Read a register. A register's name is just its address, so CUTOFF @ reads the cutoff." },
  { "!",     "( value addr -- )",   "Write a register: 12000 CUTOFF ! sets the cutoff. Registers keep their own limits (most hold 0 to 32767). Read-only registers and bad addresses flag a fault." },

  /* ---- flow ---- */
  { "L",     "( n -- )",            "Mark a place in the block as label n (0 to 15). Write the number and then L: 3 L. A jump to 3 lands just after it." },
  { "JMP",   "( n -- )",            "Jump to label n." },
  { "JZ",    "( flag n -- )",       "Jump to label n if the flag is 0." },
  { "JNZ",   "( flag n -- )",       "Jump to label n if the flag is not 0." },
  { "SKIP",  "( flag -- )",         "If the flag is 0, skip the next token. Every token is one slot (a number, a note name, a register name or a word), so what you skip is exactly what you see. 7 0 SKIP 5 NEG leaves -7: the 5 was skipped and the NEG still ran. To skip more than one token, jump to a label with JZ instead." },
  { "EXIT",  "( -- )",              "Stop now. The block (or the word being run) ends here, with no fault." },
  { "YIELD", "( -- )",              "Stop here, and next time this block runs, carry on from this point instead of the top. The way to make a block step through a sequence. Not allowed inside a word." },
  { "TRIG",  "( n -- )",            "Fire pulser n (0 to 3): the blocks that pulser is wired to run at the next tick." },
  { "ROUTE", "( src dst w -- )",    "Wire a modulation source to a register, with a weight: the same as a line in the MATRIX tab. A pair already wired just changes its weight." },
  { "W",     "( n -- )",            "Run word n (0 to 15), a short program kept apart from the blocks (24 slots). It shares the stack. Not allowed inside a word." },

  /* ---- numbers and pitch ---- */
  { "RND",   "( n -- r )",          "A random whole number from 0 up to n-1. Gives 0 if n is not above 0." },
  { "ST",    "( n -- pitch )",      "Semitones to a pitch distance (n times 256). 7 ST is a fifth up; add it to a pitch to transpose." },
  { "HZ",    "( t -- pitch )",      "A frequency in tenths of a hertz to a pitch: 4400 HZ is A4." },
  { "COLOUR", "( index col -- pitch )", "A note of a five-note scale in just intonation (pure ratios, not equal temperament). index walks up the scale (5 notes an octave; negative goes below the root) and col, 0 to 15, picks the flavour: bits 2 and 3 choose the second note from four, bits 0 and 1 the top note from four. The answer is the distance above the root, so add the root yourself." },
  { "?",     "( a b flag -- a or b )", "Pick one: leaves b if the flag is not 0, and a if it is 0." },
  { "SCALE", "( index scale -- pitch )", "A note of one of the quantiser's scales (0 chromatic, 1 major, 2 minor, 3 dorian, 4 major pentatonic, 5 minor pentatonic, 6 blues, 7 whole tone; other numbers wrap round, so 8 is 0 again). index is the step up the scale, counted from 0 at the root; it wraps into the next octave after the last note (negative goes below the root). The answer is the distance above the root, so add the root yourself." },
};

const int WORD_COUNT = (int)(sizeof WORD_TABLE / sizeof WORD_TABLE[0]);
