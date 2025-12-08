//===--- ArgumentLabelCommentsCheck.cpp - clang-tidy ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ArgumentLabelCommentsCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

void ArgumentLabelCommentsCheck::storeOptions(
    ClangTidyOptions::OptionMap &Opts) {
  Options.store(Opts, "MinimumArguments", MinimumArguments);
}

void ArgumentLabelCommentsCheck::registerMatchers(MatchFinder *Finder) {
  // Match call expressions with literal arguments
  Finder->addMatcher(
      callExpr(
          unless(isExpansionInSystemHeader()),
          hasAnyArgument(
              expr(anyOf(integerLiteral(), floatingLiteral(), stringLiteral(),
                         cxxBoolLiteral(), cxxNullPtrLiteralExpr(),
                         gnuNullExpr()))
                  .bind("literal")))
          .bind("call"),
      this);
}

bool ArgumentLabelCommentsCheck::isLabelableLiteral(const Expr *E) {
  if (!E)
    return false;

  // Check if it's a literal type we want to label
  return isa<IntegerLiteral>(E) || isa<FloatingLiteral>(E) ||
         isa<StringLiteral>(E) || isa<CXXBoolLiteralExpr>(E) ||
         isa<CXXNullPtrLiteralExpr>(E) || isa<GNUNullExpr>(E);
}

bool ArgumentLabelCommentsCheck::hasExistingLabel(
    const Expr *Arg, const SourceManager &SM, const LangOptions &LangOpts) {
  // Get the source range before the argument
  SourceLocation ArgLoc = Arg->getBeginLoc();
  if (ArgLoc.isMacroID())
    ArgLoc = SM.getExpansionLoc(ArgLoc);

  // Look backwards from the argument location for a comment
  // This is a simple heuristic - check if there's a /*...*/ comment
  // immediately before the argument

  // Get the character data before the argument
  bool Invalid = false;
  const char *ArgStart = SM.getCharacterData(ArgLoc, &Invalid);
  if (Invalid)
    return false;

  // Look backwards up to 100 characters for a label comment pattern /*...=*/
  const char *SearchStart = ArgStart - std::min(100, (int)(ArgStart - SM.getCharacterData(SM.getLocForStartOfFile(SM.getFileID(ArgLoc)))));

  StringRef BeforeArg(SearchStart, ArgStart - SearchStart);

  // Check if there's a /*...*/ pattern ending with =
  size_t LastCloseComment = BeforeArg.rfind("*/");
  if (LastCloseComment == StringRef::npos)
    return false;

  size_t LastOpenComment = BeforeArg.rfind("/*", LastCloseComment);
  if (LastOpenComment == StringRef::npos)
    return false;

  // Extract the comment content
  StringRef CommentContent = BeforeArg.substr(
      LastOpenComment + 2, LastCloseComment - LastOpenComment - 2);
  CommentContent = CommentContent.trim();

  // Check if it ends with '=' (argument label pattern)
  return !CommentContent.empty() && CommentContent.back() == '=';
}

void ArgumentLabelCommentsCheck::check(
    const MatchFinder::MatchResult &Result) {
  const auto *Call = Result.Nodes.getNodeAs<CallExpr>("call");
  const auto *Literal = Result.Nodes.getNodeAs<Expr>("literal");

  if (!Call || !Literal)
    return;

  // Skip if the call has fewer arguments than the minimum
  if (Call->getNumArgs() < MinimumArguments)
    return;

  // Get the function declaration
  const Decl *CalleeDecl = Call->getCalleeDecl();
  if (!CalleeDecl)
    return;

  const FunctionDecl *FuncDecl = CalleeDecl->getAsFunction();
  if (!FuncDecl)
    return;

  // Find which argument position the literal is at
  unsigned ArgIndex = 0;
  bool Found = false;
  for (unsigned I = 0; I < Call->getNumArgs(); ++I) {
    if (Call->getArg(I) == Literal) {
      ArgIndex = I;
      Found = true;
      break;
    }
  }

  if (!Found)
    return;

  // Check if this argument corresponds to a parameter
  if (ArgIndex >= FuncDecl->getNumParams())
    return;

  const ParmVarDecl *Param = FuncDecl->getParamDecl(ArgIndex);
  if (!Param)
    return;

  // Get the parameter name
  StringRef ParamName = Param->getName();
  if (ParamName.empty())
    return;

  // Check if there's already a label comment
  const SourceManager &SM = *Result.SourceManager;
  if (hasExistingLabel(Literal, SM, Result.Context->getLangOpts()))
    return;

  // Create diagnostic and fix-it
  SourceLocation InsertLoc = Literal->getBeginLoc();

  auto Diag = diag(InsertLoc,
                   "consider adding argument label comment for literal argument");

  std::string LabelComment = "/*" + ParamName.str() + "=*/";
  Diag << FixItHint::CreateInsertion(InsertLoc, LabelComment);
}

} // namespace clang::tidy::readability
