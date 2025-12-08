// Example demonstrating comment style fixers

// Example 1: Regular comments (WILL be modified with CapitalizeComments=true and PunctuateComments=true)
// this is a regular comment
/* another regular comment */

// Example 2: Argument label comments (will NOT be modified)
// These are block comments ending with '=' before the */
void foo(int width, int height, bool test) {
  // Function calls with argument labels
  someFunction(/*width=*/42, /*height=*/100);
  anotherFunction(/*test=*/true);
  thirdFunction(/*enable=*/false, /*count=*/5);
  fourthFunction(/* param_name = */value);
}

// Example 3: Comments with '=' that WILL be modified
// (these don't end with = so they're not argument labels)
// this sets x = 42 for the test
/* this uses width = 100 */
// width = 42

// Example 4: Special comments (will NOT be modified)
// clang-format off
// NOLINT
// TODO
// FIXME
// http://example.com
/// Doxygen comment
/** Another doxygen */

// Example 5: Code-like comments (will NOT be modified)
// x = 42
// foo(bar)
// array[index]

// Example 6: What WILL be modified:
// before: // this is a sentence
// after:  // This is a sentence.

// before: /* another sentence */
// after:  /* Another sentence. */

