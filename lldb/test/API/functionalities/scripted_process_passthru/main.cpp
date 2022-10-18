#include <iostream>

int multiply(int number, int factor) {
  int result = number * factor;
  return result; // check result and step out
}

int divide(int dividend, int divisor) { return dividend / divisor; }

int main() {
  int n = 42;

  n++; // break here and step inst
  n++;
  n++;
  n++;
  n++;
  n++;
  n++;
  n++;

  int a = multiply(n, 2); // check n and step into
  int b = divide(n, 2);   // check a and step over

  int result = a + b; // break here and check result

  return 0;
}
