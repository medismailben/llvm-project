// The tests here look for particular instruction forms in main rather than at
// particular addresses, so this has to keep emitting them at -O0:
//
//   - a call, for the relocation round trip, which is the only form whose
//     encoding has to be rewritten when it moves;
//   - a reference to a string literal, which on AArch64 is an adrp and is the
//     instruction the disassembler currently declines to relocate at all;
//   - a loop, so the call site sits inside a function with branches around it,
//     which is what the patch site analysis inspects.

#include <stdio.h>

int helper(int value) { return value + 1; }

int main(int argc, char **argv) {
  int counter = 0;

  for (int i = 0; i < 10; i++)
    counter = helper(counter); // break here

  printf("counter = %d\n", counter);
  return 0;
}
