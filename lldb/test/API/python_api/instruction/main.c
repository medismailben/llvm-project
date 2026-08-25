// The point of this program is to make the compiler emit a call inside main,
// which is the one PC-relative instruction the relocation API currently
// accepts. Nothing here is ever run: the tests relocate the bytes and inspect
// the result, they do not launch.

#include <stdio.h>

int compute(int value) { return value * 2; }

int main(int argc, char **argv) {
  int result = compute(argc);
  printf("result = %d\n", result);
  return 0;
}
