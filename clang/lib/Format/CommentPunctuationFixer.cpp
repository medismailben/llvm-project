//===--- CommentPunctuationFixer.cpp - Comment Punctuation ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements CommentPunctuationFixer, a TokenAnalyzer that ensures
/// comments end with proper punctuation.
///
//===----------------------------------------------------------------------===//

#include "CommentPunctuationFixer.h"
#include "clang/Basic/TokenKinds.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "comment-punctuation-fixer"

namespace clang {
namespace format {

CommentPunctuationFixer::CommentPunctuationFixer(const Environment &Env,
                                                 const FormatStyle &Style)
    : TokenAnalyzer(Env, Style) {}

std::pair<tooling::Replacements, unsigned>
CommentPunctuationFixer::analyze(
    TokenAnnotator &Annotator, SmallVectorImpl<AnnotatedLine *> &AnnotatedLines,
    FormatTokenLexer &Tokens) {
  tooling::Replacements Fixes;

  for (AnnotatedLine *Line : AnnotatedLines) {
    for (const FormatToken *Tok = Line->First; Tok; Tok = Tok->Next) {
      if (Tok->is(tok::comment)) {
        fixCommentPunctuation(Tok, Fixes);
      }
    }
  }

  return {Fixes, 0};
}

bool CommentPunctuationFixer::shouldSkipComment(StringRef Comment) {
  // Skip special comments:
  // - clang-format directives
  // - NOLINT and similar tool directives
  // - Copyright/license headers
  // - URLs
  if (Comment.contains("clang-format") || Comment.contains("NOLINT") ||
      Comment.contains("NOLINTNEXTLINE") || Comment.contains("IWYU") ||
      Comment.contains("Copyright") || Comment.contains("LICENSE") ||
      Comment.contains("SPDX-License") || Comment.contains("http://") ||
      Comment.contains("https://")) {
    return true;
  }

  // Skip Doxygen-style comments (they have their own formatting rules)
  if (Comment.starts_with("///") || Comment.starts_with("//!") ||
      Comment.starts_with("/**") || Comment.starts_with("/*!")) {
    return true;
  }

  // Skip argument label comments like /*width=*/ or /*test=*/
  // These are commonly used in function calls to name arguments at the callsite
  // They are always block comments ending with '=' before the closing */
  if (Comment.starts_with("/*") && Comment.ends_with("*/")) {
    // Extract content between /* and */
    StringRef Content = Comment.substr(2, Comment.size() - 4);
    // Trim trailing whitespace
    Content = Content.rtrim();
    // Check if it ends with =
    if (!Content.empty() && Content.back() == '=') {
      return true;
    }
  }

  return false;
}

void CommentPunctuationFixer::fixCommentPunctuation(
    const FormatToken *Tok, tooling::Replacements &Fixes) {
  StringRef Comment = Tok->TokenText;

  if (shouldSkipComment(Comment)) {
    return;
  }

  bool isLineComment = Comment.starts_with("//");
  std::string FixedComment = punctuateComment(Comment, isLineComment);

  // Only create a replacement if the comment actually changed
  if (FixedComment != Comment) {
    const SourceManager &SourceMgr = Env.getSourceManager();
    SourceLocation Start = Tok->Tok.getLocation();
    unsigned Length = Tok->TokenText.size();

    auto Err = Fixes.add(
        tooling::Replacement(SourceMgr, Start, Length, FixedComment));
    if (Err) {
      llvm::errs() << "Error creating replacement: "
                   << llvm::toString(std::move(Err)) << "\n";
    }
  }
}

std::pair<StringRef, StringRef>
CommentPunctuationFixer::getCommentPrefixAndContent(StringRef Comment,
                                                    bool isLineComment) {
  if (isLineComment) {
    // Handle // comments
    size_t ContentStart = Comment.find_first_not_of("/ \t", 2);
    if (ContentStart == StringRef::npos) {
      return {Comment, ""};
    }
    StringRef Prefix = Comment.substr(0, ContentStart);
    StringRef Content = Comment.substr(ContentStart);
    return {Prefix, Content};
  } else {
    // Handle /* */ comments
    if (Comment.size() < 4) { // Minimum: /* */
      return {Comment, ""};
    }
    size_t ContentStart = Comment.find_first_not_of("/* \t", 2);
    size_t ContentEnd = Comment.rfind("*/");
    if (ContentStart == StringRef::npos || ContentEnd == StringRef::npos ||
        ContentStart >= ContentEnd) {
      return {Comment, ""};
    }

    // Find the actual content, excluding trailing whitespace before */
    StringRef PotentialContent =
        Comment.substr(ContentStart, ContentEnd - ContentStart);
    size_t ContentRealEnd = PotentialContent.find_last_not_of(" \t");
    if (ContentRealEnd == StringRef::npos) {
      return {Comment, ""};
    }

    StringRef Prefix = Comment.substr(0, ContentStart);
    StringRef Content = PotentialContent.substr(0, ContentRealEnd + 1);
    return {Prefix, Content};
  }
}

bool CommentPunctuationFixer::needsPunctuation(StringRef Content) {
  if (Content.empty())
    return false;

  // Trim trailing whitespace
  size_t End = Content.find_last_not_of(" \t\r\n");
  if (End == StringRef::npos)
    return false;

  char LastChar = Content[End];

  // Already has punctuation
  if (LastChar == '.' || LastChar == '!' || LastChar == '?' ||
      LastChar == ':' || LastChar == ';' || LastChar == ',') {
    return false;
  }

  // Skip adding punctuation to:
  // - Very short comments (< 3 chars)
  // - Single words (like "TODO", "FIXME", "NOTE", "HACK")
  // - Code snippets (contains special characters)
  if (Content.size() < 3)
    return false;

  // Check if it's a single word (no spaces)
  if (Content.find(' ') == StringRef::npos) {
    // Common single-word comments that don't need punctuation
    if (Content.equals_insensitive("TODO") ||
        Content.equals_insensitive("FIXME") ||
        Content.equals_insensitive("NOTE") ||
        Content.equals_insensitive("HACK") ||
        Content.equals_insensitive("XXX") ||
        Content.equals_insensitive("BUG") ||
        Content.equals_insensitive("WARNING")) {
      return false;
    }
    // Other single words also don't need punctuation
    return false;
  }

  // Check if it looks like code (has special chars like (), {}, [], =, ;)
  if (Content.contains('(') || Content.contains('{') ||
      Content.contains('[') || Content.contains('=') || Content.contains(';')) {
    return false;
  }

  // Looks like a sentence without punctuation
  return true;
}

std::string
CommentPunctuationFixer::punctuateComment(StringRef Original,
                                          bool isLineComment) {
  auto [Prefix, Content] = getCommentPrefixAndContent(Original, isLineComment);

  if (Content.empty() || !needsPunctuation(Content)) {
    return Original.str();
  }

  std::string FixedContent = Content.str();
  FixedContent += '.';

  // Reconstruct the comment
  std::string Result;
  if (isLineComment) {
    Result = Prefix.str() + FixedContent;
  } else {
    // For block comments, we need to add back the closing */
    StringRef Suffix = Original.substr(Original.rfind("*/"));
    size_t ContentEnd = Original.rfind("*/");
    size_t PrefixEnd = Prefix.size() + Content.size();
    StringRef MiddleSpace = Original.substr(PrefixEnd, ContentEnd - PrefixEnd);

    Result = Prefix.str() + FixedContent + MiddleSpace.str() + Suffix.str();
  }

  return Result;
}

} // end namespace format
} // end namespace clang
