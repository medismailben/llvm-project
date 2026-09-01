#include <stdio.h>

// Called twice on purpose. Replacing a function takes effect on the next call,
// since the current activation's frame was laid out by the optimiser and cannot
// be exchanged for the clone's, so a single call would exercise nothing.
__attribute__((noinline)) int hot(int n) {
  int acc = 0;
  for (int i = 0; i < n; i++) {
    int tmp = i * 3;
    acc += tmp; // break here
  }
  return acc;
}

int main(int argc, char **argv) {
  printf("%d\n", hot(10));
  printf("%d\n", hot(10));
  return 0;
}
