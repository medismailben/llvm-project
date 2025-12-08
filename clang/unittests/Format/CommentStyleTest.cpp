//===- CommentStyleTest.cpp - Tests for Comment Style Fixers -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "FormatTestBase.h"

namespace clang {
namespace format {
namespace test {
namespace {

class CommentStyleTest : public FormatTestBase {};

TEST_F(CommentStyleTest, CapitalizesLineComments) {
  FormatStyle Style = getLLVMStyle();
  Style.CapitalizeComments = true;

  verifyFormat("// This is a comment.", "// this is a comment.", Style);
  verifyFormat("// Another comment.", "// another comment.", Style);
}

TEST_F(CommentStyleTest, CapitalizesBlockComments) {
  FormatStyle Style = getLLVMStyle();
  Style.CapitalizeComments = true;

  verifyFormat("/* This is a comment. */", "/* this is a comment. */", Style);
  verifyFormat("/* Another comment. */", "/* another comment. */", Style);
}

TEST_F(CommentStyleTest, DoesNotCapitalizeAlreadyCapitalized) {
  FormatStyle Style = getLLVMStyle();
  Style.CapitalizeComments = true;

  verifyFormat("// This is already capitalized.", Style);
  verifyFormat("/* This is already capitalized. */", Style);
}

TEST_F(CommentStyleTest, DoesNotCapitalizeNonLetters) {
  FormatStyle Style = getLLVMStyle();
  Style.CapitalizeComments = true;

  verifyFormat("// 123 starts with number.", Style);
  verifyFormat("// @param starts with symbol.", Style);
  verifyFormat("/* 456 another number. */", Style);
}

TEST_F(CommentStyleTest, SkipsSpecialComments) {
  FormatStyle Style = getLLVMStyle();
  Style.CapitalizeComments = true;

  // Should not capitalize clang-format directives
  verifyFormat("// clang-format off", Style);
  verifyFormat("// clang-format on", Style);

  // Should not capitalize NOLINT
  verifyFormat("// NOLINT", Style);
  verifyFormat("// NOLINTNEXTLINE", Style);

  // Should not capitalize Doxygen comments
  verifyFormat("/// this is doxygen", Style);
  verifyFormat("//! this is doxygen", Style);
  verifyFormat("/** this is doxygen */", Style);
  verifyFormat("/*! this is doxygen */", Style);

  // Should not capitalize copyright
  verifyFormat("// copyright 2024", Style);
  verifyFormat("// LICENSE information", Style);
}

TEST_F(CommentStyleTest, PunctuatesLineComments) {
  FormatStyle Style = getLLVMStyle();
  Style.PunctuateComments = true;

  verifyFormat("// This is a comment.", "// This is a comment", Style);
  verifyFormat("// Another sentence.", "// Another sentence", Style);
}

TEST_F(CommentStyleTest, PunctuatesBlockComments) {
  FormatStyle Style = getLLVMStyle();
  Style.PunctuateComments = true;

  verifyFormat("/* This is a comment. */", "/* This is a comment */", Style);
  verifyFormat("/* Another sentence. */", "/* Another sentence */", Style);
}

TEST_F(CommentStyleTest, DoesNotPunctuateAlreadyPunctuated) {
  FormatStyle Style = getLLVMStyle();
  Style.PunctuateComments = true;

  verifyFormat("// Already has a period.", Style);
  verifyFormat("// Already has an exclamation!", Style);
  verifyFormat("// Already has a question?", Style);
  verifyFormat("/* Already punctuated. */", Style);
}

TEST_F(CommentStyleTest, DoesNotPunctuateSingleWords) {
  FormatStyle Style = getLLVMStyle();
  Style.PunctuateComments = true;

  // Single words should not get punctuation
  verifyFormat("// TODO", Style);
  verifyFormat("// FIXME", Style);
  verifyFormat("// NOTE", Style);
  verifyFormat("// HACK", Style);
  verifyFormat("// WARNING", Style);
}

TEST_F(CommentStyleTest, DoesNotPunctuateCodeSnippets) {
  FormatStyle Style = getLLVMStyle();
  Style.PunctuateComments = true;

  // Code-like comments should not get punctuation
  verifyFormat("// foo(bar)", Style);
  verifyFormat("// x = 42", Style);
  verifyFormat("// array[index]", Style);
  verifyFormat("// { block }", Style);
}

TEST_F(CommentStyleTest, DoesNotPunctuateVeryShortComments) {
  FormatStyle Style = getLLVMStyle();
  Style.PunctuateComments = true;

  verifyFormat("// OK", Style);
  verifyFormat("// No", Style);
}

TEST_F(CommentStyleTest, BothFixersCanWorkTogether) {
  FormatStyle Style = getLLVMStyle();
  Style.CapitalizeComments = true;
  Style.PunctuateComments = true;

  verifyFormat("// This is a comment.", "// this is a comment", Style);
  verifyFormat("/* Another comment. */", "/* another comment */", Style);
}

TEST_F(CommentStyleTest, PreservesCommentSpacing) {
  FormatStyle Style = getLLVMStyle();
  Style.CapitalizeComments = true;

  verifyFormat("//  This has two spaces.", "//  this has two spaces.", Style);
  verifyFormat("//   This has three.", "//   this has three.", Style);
}

TEST_F(CommentStyleTest, HandlesEmptyComments) {
  FormatStyle Style = getLLVMStyle();
  Style.CapitalizeComments = true;
  Style.PunctuateComments = true;

  verifyFormat("//", Style);
  verifyFormat("/* */", Style);
  verifyFormat("//   ", Style);
}

TEST_F(CommentStyleTest, DisabledByDefault) {
  FormatStyle Style = getLLVMStyle();
  // Default should not change comments
  verifyFormat("// this is lowercase", Style);
  verifyFormat("// This has no period", Style);
}

TEST_F(CommentStyleTest, SkipsArgumentLabelComments) {
  FormatStyle Style = getLLVMStyle();
  Style.CapitalizeComments = true;
  Style.PunctuateComments = true;

  // Should not modify argument label comments (block comments ending with =)
  verifyFormat("/*width=*/", Style);
  verifyFormat("/*height=*/", Style);
  verifyFormat("/*test=*/", Style);
  verifyFormat("/*enable=*/", Style);
  verifyFormat("/*name=*/", Style);
  verifyFormat("/* width = */", Style);
  verifyFormat("/* param_name = */", Style);

  // Test in context
  verifyFormat("foo(/*width=*/42, /*height=*/100);", Style);
  verifyFormat("bar(/*test=*/true);", Style);
}

TEST_F(CommentStyleTest, CapitalizesNonLabelCommentsWithEquals) {
  FormatStyle Style = getLLVMStyle();
  Style.CapitalizeComments = true;
  Style.PunctuateComments = true;

  // These are real comments, not argument labels, so they should be modified
  // (they don't end with = before the */)
  verifyFormat("// This sets x = 42 for the test.",
               "// this sets x = 42 for the test",
               Style);
  verifyFormat("/* This uses width = 100. */",
               "/* this uses width = 100 */",
               Style);

  // Line comments with = are never argument labels
  verifyFormat("// Width = 42.", "// width = 42", Style);
}

TEST_F(CommentStyleTest, WorksWithCode) {
  FormatStyle Style = getLLVMStyle();
  Style.CapitalizeComments = true;
  Style.PunctuateComments = true;

  verifyFormat("// This is a comment.\n"
               "int x = 42;",
               "// this is a comment\n"
               "int x = 42;",
               Style);

  verifyFormat("int x = 42; // This is a trailing comment.",
               "int x = 42; // this is a trailing comment", Style);
}

} // namespace
} // namespace test
} // namespace format
} // namespace clang
