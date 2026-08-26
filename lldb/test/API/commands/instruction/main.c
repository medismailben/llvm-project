// The tests here look for particular instruction forms in main rather than at
// particular addresses, so this has to keep emitting them at -O0:
//
//   - a call, for the relocation round trip, which is the only form whose
//     encoding has to be rewritten when it moves;
//   - a reference to a string literal, which on AArch64 is an adrp;
//   - a loop, so the call site sits inside a function with branches around it,
//     which is what the patch site analysis inspects.

#include <stdio.h>

int helper(int value) { return value + 1; }

// The pc-relative forms a compiler will not produce, written out so that
// relocating them is covered by something other than inspection.
//
// clang on AArch64 builds a constant with movz and movk rather than loading it
// from a pool, even a floating point one, which it materializes in a general
// purpose register and then moves across. So no literal load appears in compiled
// code at all, and the only honest way to test the relocated form is to write the
// instruction.
//
// Naked, so nothing is emitted around the asm and these forms are the whole
// function. Never called: the tests read it out of the module on disk.
#if defined(__aarch64__) || defined(__arm64__)
__attribute__((naked, used)) void pcrel_forms(void) {
  __asm__("adr   x0, Lpcrel_near\n"
          // A page computation, and the three literal loads whose destination can
          // hold the address a copy has to compute for itself.
          "adrp  x1, Lpcrel_pool@PAGE\n"
          "ldr   x2, Lpcrel_pool\n"
          "ldr   w3, Lpcrel_pool\n"
          "ldrsw x4, Lpcrel_pool\n"
          // And one whose destination cannot, which is the form that has to be
          // refused rather than moved.
          "ldr   s0, Lpcrel_pool\n"
          "ret\n"
          "Lpcrel_near:\n"
          "  .quad 0\n"
          "  .p2align 3\n"
          "Lpcrel_pool:\n"
          "  .quad 0\n");
}
#endif

int main(int argc, char **argv) {
  int counter = 0;

  for (int i = 0; i < 10; i++)
    counter = helper(counter); // break here

  printf("counter = %d\n", counter);
  return 0;
}
