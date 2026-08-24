// The tests in this directory continue to process exit, so the loop has to stay
// short enough to keep them quick. It still has to call something, because the
// point is to exercise a patched site in a loop the way real code hits one.

#include <stdio.h>

int main(int argc, char **argv) {
  int counter = 0;

  for (int i = 0; i < 200; i++) {
    printf("counter = %d\n", counter++); // break here
  }

  // Reaching the end is the only thing a detached inferior can report back:
  // it is not the test's child, so its exit status is not collectable. A
  // detached process that still has a patch installed dies on the injected trap
  // partway through the loop and never gets here.
  if (argc > 1) {
    FILE *marker = fopen(argv[1], "w");
    if (marker) {
      fputs("done\n", marker);
      fclose(marker);
    }
  }

  return 0;
}
