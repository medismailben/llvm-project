// Variables whose DWARF locations take different shapes, so that the pass that
// lowers a location expression into C has something of each kind to chew on.
//
// Unoptimized, every local here is DW_OP_fbreg. Optimized, the loop counter and
// the accumulator move into registers, which is DW_OP_regN, and the compiler
// folds some of them to a constant, which is DW_OP_consts followed by
// DW_OP_stack_value.

#include <stdio.h>

int global_total = 0;
// Kept apart from global_total so that adding the composite case below did not
// change what global_total counts, which a test conditions on.
long composite_total = 0;

__attribute__((noinline)) int step(int value) { return value + 1; }

// Two words, so it arrives in two registers rather than one. That is how
// DW_OP_piece shows up: the low half described by one register and the high half
// by another, with a DW_OP_piece after each saying how much of the variable it
// covers.
struct Pair {
  long lo;
  long hi;
};

__attribute__((noinline)) long consume(struct Pair pair, int tag) {
  long sum = pair.lo + pair.hi;
  // Keep this marker on one line: the test finds this line by source regex.
  printf("tag = %d sum = %ld\n", tag, sum); // split across registers
  return sum + tag;
}

int main(int argc, char **argv) {
  int local_count = 0;

  for (int index = 0; index < 32; index++) {
    local_count = step(local_count);
    global_total += local_count;

    struct Pair pair = {index, index * 100 + 9};
    composite_total += consume(pair, index);

    // Keep this marker on one line: the test finds this line by source regex,
    // and a wrapped comment matches nothing.
    printf("index = %d local_count = %d\n", index, local_count); // break here
  }

  return 0;
}
