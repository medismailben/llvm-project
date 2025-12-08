//===--- ArgumentLabelCommentsCheck.h - clang-tidy -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_READABILITY_ARGUMENTLABELCOMMENTSCHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_READABILITY_ARGUMENTLABELCOMMENTSCHECK_H

#include "../ClangTidyCheck.h"

namespace clang::tidy::readability {

/// Inserts argument label comments before literal arguments in function calls.
///
/// This check identifies function calls where literal values (numbers, strings,
/// booleans, nullptr) are passed as arguments and suggests adding comments
/// that label these literals with their parameter names.
///
/// For the user-facing documentation see:
/// http://clang.llvm.org/extra/clang-tidy/checks/readability/argument-label-comments.html
class ArgumentLabelCommentsCheck : public ClangTidyCheck {
public:
  ArgumentLabelCommentsCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context),
        MinimumArguments(
            Options.get("MinimumArguments", 2U)) {}

  void storeOptions(ClangTidyOptions::OptionMap &Opts) override;
  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;

  std::optional<TraversalKind> getCheckTraversalKind() const override {
    return TK_IgnoreUnlessSpelledInSource;
  }

private:
  // Check if an expression is a labelable literal
  bool isLabelableLiteral(const Expr *E);

  // Check if an argument already has a label comment
  bool hasExistingLabel(const Expr *Arg, const SourceManager &SM,
                        const LangOptions &LangOpts);

  // Minimum number of arguments before suggesting labels
  const unsigned MinimumArguments;
};

} // namespace clang::tidy::readability

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_READABILITY_ARGUMENTLABELCOMMENTSCHECK_H
