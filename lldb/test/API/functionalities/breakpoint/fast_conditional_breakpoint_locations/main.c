// Variables whose DWARF locations take different shapes, so that the pass that
// lowers a location expression into C has something of each kind to chew on.
//
// Unoptimized, every local here is DW_OP_fbreg. Optimized, the loop counter and
// the accumulator move into registers, which is DW_OP_regN, and the compiler
// folds some of them to a constant, which is DW_OP_consts followed by
// DW_OP_stack_value.

#include <stdio.h>

int global_total = 0;

__attribute__((noinline)) int step(int value) { return value + 1; }

int main(int argc, char **argv) {
  int local_count = 0;

  for (int index = 0; index < 32; index++) {
    local_count = step(local_count);
    global_total += local_count;
    // Keep this marker on one line: the test finds this line by source regex,
    // and a wrapped comment matches nothing.
    printf("index = %d local_count = %d\n", index, local_count); // break here
  }

  return 0;
}
