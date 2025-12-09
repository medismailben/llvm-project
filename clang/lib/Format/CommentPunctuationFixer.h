//===--- CommentPunctuationFixer.h - Comment Punctuation ----*- C++ -*-----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file declares CommentPunctuationFixer, a TokenAnalyzer that ensures
/// comments end with proper punctuation.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_FORMAT_COMMENTPUNCTUATIONFIXER_H
#define LLVM_CLANG_LIB_FORMAT_COMMENTPUNCTUATIONFIXER_H

#include "TokenAnalyzer.h"

namespace clang {
namespace format {

class CommentPunctuationFixer : public TokenAnalyzer {
public:
  CommentPunctuationFixer(const Environment &Env, const FormatStyle &Style);

  std::pair<tooling::Replacements, unsigned>
  analyze(TokenAnnotator &Annotator,
          SmallVectorImpl<AnnotatedLine *> &AnnotatedLines,
          FormatTokenLexer &Tokens) override;

private:
  // Fix punctuation in a single comment token
  void fixCommentPunctuation(const FormatToken *Tok,
                             tooling::Replacements &Fixes);

  // Extract the comment content (without // or /* */)
  std::pair<StringRef, StringRef> getCommentPrefixAndContent(StringRef Comment,
                                                              bool isLineComment);

  // Check if a comment should be skipped
  bool shouldSkipComment(StringRef Comment);

  // Check if a comment needs punctuation
  bool needsPunctuation(StringRef Content);

  // Create fixed comment text with proper punctuation
  std::string punctuateComment(StringRef Original, bool isLineComment);
};

} // end namespace format
} // end namespace clang

#endif // LLVM_CLANG_LIB_FORMAT_COMMENTPUNCTUATIONFIXER_H
