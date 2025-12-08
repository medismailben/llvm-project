// RUN: %check_clang_tidy %s readability-argument-label-comments %t

void function(int count, const char* name, bool debug) {}

void intLiterals(int x, int y) {}
void floatLiterals(double a, double b) {}
void stringLiterals(const char* s1, const char* s2) {}
void boolLiterals(bool flag1, bool flag2) {}
void ptrLiterals(int* ptr1, int* ptr2) {}

void testBasicCase() {
  // Function with multiple literal arguments
  function(42, "hello", true);
  // CHECK-MESSAGES: :[[@LINE-1]]:12: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-MESSAGES: :[[@LINE-2]]:16: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-MESSAGES: :[[@LINE-3]]:25: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-FIXES: function(/*count=*/42, /*name=*/"hello", /*debug=*/true);
}

void testIntegerLiterals() {
  intLiterals(100, 200);
  // CHECK-MESSAGES: :[[@LINE-1]]:15: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-MESSAGES: :[[@LINE-2]]:20: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-FIXES: intLiterals(/*x=*/100, /*y=*/200);
}

void testFloatingLiterals() {
  floatLiterals(3.14, 2.71);
  // CHECK-MESSAGES: :[[@LINE-1]]:17: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-MESSAGES: :[[@LINE-2]]:23: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-FIXES: floatLiterals(/*a=*/3.14, /*b=*/2.71);
}

void testStringLiterals() {
  stringLiterals("foo", "bar");
  // CHECK-MESSAGES: :[[@LINE-1]]:18: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-MESSAGES: :[[@LINE-2]]:25: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-FIXES: stringLiterals(/*s1=*/"foo", /*s2=*/"bar");
}

void testBoolLiterals() {
  boolLiterals(true, false);
  // CHECK-MESSAGES: :[[@LINE-1]]:16: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-MESSAGES: :[[@LINE-2]]:22: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-FIXES: boolLiterals(/*flag1=*/true, /*flag2=*/false);
}

void testNullptrLiterals() {
  ptrLiterals(nullptr, nullptr);
  // CHECK-MESSAGES: :[[@LINE-1]]:15: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-MESSAGES: :[[@LINE-2]]:24: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-FIXES: ptrLiterals(/*ptr1=*/nullptr, /*ptr2=*/nullptr);
}

void testAlreadyHasLabel() {
  // Should not warn if label already exists
  function(/*count=*/42, "hello", true);
  // CHECK-MESSAGES: :[[@LINE-1]]:26: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-MESSAGES: :[[@LINE-2]]:35: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-FIXES: function(/*count=*/42, /*name=*/"hello", /*debug=*/true);
}

void testNonLiteralArguments() {
  // Should not warn for non-literal arguments
  int count = 42;
  const char* name = "hello";
  bool debug = true;

  function(count, name, debug);
  // No CHECK-MESSAGES expected - these are not literals
}

void testSingleArgument(int x) {}

void testMinimumArguments() {
  // By default, only functions with 2+ arguments trigger the check
  testSingleArgument(42);
  // No CHECK-MESSAGES expected - single argument

  intLiterals(100, 200);
  // CHECK-MESSAGES: :[[@LINE-1]]:15: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-MESSAGES: :[[@LINE-2]]:20: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-FIXES: intLiterals(/*x=*/100, /*y=*/200);
}

void testMixedArguments() {
  int x = 5;
  // Mixed literal and non-literal arguments
  intLiterals(x, 200);
  // CHECK-MESSAGES: :[[@LINE-1]]:18: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-FIXES: intLiterals(x, /*y=*/200);
}

void testZeroLiteral() {
  intLiterals(0, 1);
  // CHECK-MESSAGES: :[[@LINE-1]]:15: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-MESSAGES: :[[@LINE-2]]:18: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-FIXES: intLiterals(/*x=*/0, /*y=*/1);
}

void testNegativeLiteral() {
  intLiterals(-5, -10);
  // CHECK-MESSAGES: :[[@LINE-1]]:15: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-MESSAGES: :[[@LINE-2]]:19: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-FIXES: intLiterals(/*x=*/-5, /*y=*/-10);
}

void unnamedParam(int, int y) {}

void testUnnamedParameter() {
  // Should not suggest label for unnamed parameters
  unnamedParam(42, 100);
  // CHECK-MESSAGES: :[[@LINE-1]]:24: warning: consider adding argument label comment for literal argument [readability-argument-label-comments]
  // CHECK-FIXES: unnamedParam(42, /*y=*/100);
}
