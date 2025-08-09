// This simple program is to demonstrate the capability of the lldb command
// "breakpoint set -c "condition() == 999999" -f main.c -l 29 -I" or
// "breakpoint set -c "local_count == 999999" -f main.c -l 29 -I" to break
// the condition for an inject breakpoint evaluate to true.

#include <stdio.h>

static int global_count = 0;

int condition(void) {
  printf("global_count = %d\n", global_count);
  return global_count++;
}

int main(int argc, char *argv[]) {
  int local_count = 0;
  for (int i = 0; i < 10000000; i++) {
    printf("local_count = %d\n",
           local_count++); // Find the line number of condition breakpoint for
                           // local_count
  }

  return 0;
}
