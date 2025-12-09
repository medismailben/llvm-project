// RUN: grep -Ev "// *[A-Z-]+:" %s \
// RUN:   | clang-format -style="{BasedOnStyle: LLVM, CapitalizeComments: true, PunctuateComments: true}" \
// RUN:   | FileCheck -strict-whitespace %s

// Example 1: Regular comments (WILL be modified with CapitalizeComments=true and PunctuateComments=true)
// CHECK: // This is a regular comment.
// this is a regular comment
// CHECK: /* Another regular comment. */
/* another regular comment */

// Example 2: Argument label comments (will NOT be modified)
// These are block comments ending with '=' before the */
void foo(int width, int height, bool test) {
  // Function calls with argument labels
  // CHECK: someFunction(/*width=*/42, /*height=*/100);
  someFunction(/*width=*/42, /*height=*/100);
  // CHECK: anotherFunction(/*test=*/true);
  anotherFunction(/*test=*/true);
  // CHECK: thirdFunction(/*enable=*/false, /*count=*/5);
  thirdFunction(/*enable=*/false, /*count=*/5);
  // CHECK: fourthFunction(/* param_name = */value);
  fourthFunction(/* param_name = */value);
}

// Example 3: Comments with '=' that WILL be modified
// (these don't end with = so they're not argument labels)
// CHECK: // This sets x = 42 for the test.
// this sets x = 42 for the test
// CHECK: /* This uses width = 100. */
/* this uses width = 100 */
// CHECK: // width = 42
// width = 42

// Example 4: Special comments (will NOT be modified)
// CHECK: // clang-format off
// clang-format off
// CHECK: // NOLINT
// NOLINT
// CHECK: // TODO
// TODO
// CHECK: // FIXME
// FIXME
// CHECK: // http://example.com
// http://example.com
// CHECK: /// Doxygen comment
/// Doxygen comment
// CHECK: /** Another doxygen */
/** Another doxygen */

// Example 5: Code-like comments (will NOT be modified)
// CHECK: // x = 42
// x = 42
// CHECK: // foo(bar)
// foo(bar)
// CHECK: // array[index]
// array[index]

// Example 6: Sentence-like comments will be modified:
// CHECK: // This is a sentence.
// this is a sentence
// CHECK: /* Another sentence. */
/* another sentence */

// Test with CapitalizeComments=false and PunctuateComments=false
// RUN: grep -Ev "// *[A-Z-]+:" %s \
// RUN:   | clang-format -style="{BasedOnStyle: LLVM, CapitalizeComments: false, PunctuateComments: false}" \
// RUN:   | FileCheck -check-prefix=NOFIX %s

// NOFIX: // this is a regular comment
// NOFIX: /* another regular comment */
// NOFIX: // this sets x = 42 for the test
// NOFIX: /* this uses width = 100 */
// NOFIX: // this is a sentence
// NOFIX: /* another sentence */
