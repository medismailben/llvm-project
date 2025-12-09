//===--- CommentCapitalizationFixer.h - Comment Capitalization -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file declares CommentCapitalizationFixer, a TokenAnalyzer that ensures
/// comments start with a capital letter.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_FORMAT_COMMENTCAPITALIZATIONFIXER_H
#define LLVM_CLANG_LIB_FORMAT_COMMENTCAPITALIZATIONFIXER_H

#include "TokenAnalyzer.h"

namespace clang {
namespace format {

class CommentCapitalizationFixer : public TokenAnalyzer {
public:
  CommentCapitalizationFixer(const Environment &Env, const FormatStyle &Style);

  std::pair<tooling::Replacements, unsigned>
  analyze(TokenAnnotator &Annotator,
          SmallVectorImpl<AnnotatedLine *> &AnnotatedLines,
          FormatTokenLexer &Tokens) override;

private:
  // Fix capitalization in a single comment token
  void fixCommentCapitalization(const FormatToken *Tok,
                                tooling::Replacements &Fixes);

  // Extract the comment content (without // or /* */)
  std::pair<StringRef, StringRef> getCommentPrefixAndContent(StringRef Comment,
                                                              bool isLineComment);

  // Check if a comment should be skipped
  bool shouldSkipComment(StringRef Comment);

  // Check if a comment needs capitalization
  bool needsCapitalization(StringRef Content);

  // Create fixed comment text with proper capitalization
  std::string capitalizeComment(StringRef Original, bool isLineComment);
};

} // end namespace format
} // end namespace clang

#endif // LLVM_CLANG_LIB_FORMAT_COMMENTCAPITALIZATIONFIXER_H
