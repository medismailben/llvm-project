#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/CodeGen/ObjectFilePCHContainerWriter.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Serialization/ObjectFilePCHContainerReader.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ToolOutputFile.h"

#include <iostream>
#include <set>
#include <sstream>
#include <string>

using namespace clang;
using namespace clang::driver;
using namespace clang::tooling;

static llvm::cl::OptionCategory SBGenCategory("LLDB Scripting Bridge Generator");
static llvm::cl::opt<std::string>
    OutputDir("output-dir",
              llvm::cl::desc("Directory to output generated files to"),
              llvm::cl::init(""), llvm::cl::cat(SBGenCategory));
static llvm::cl::opt<bool> GenerateOptionalClass(
    "generate-optional-class",
    llvm::cl::desc("Generate a header & source files for SBOptional class"),
    llvm::cl::init(false),
    llvm::cl::cat(SBGenCategory));
static llvm::cl::opt<bool> Verbose(
    "verbose",
    llvm::cl::desc("Dumps all the visited SB classes."),
    llvm::cl::init(false),
    llvm::cl::cat(SBGenCategory));

static std::unique_ptr<llvm::ToolOutputFile>
CreateOutputFile(llvm::StringRef OutputDir, llvm::StringRef Filename) {
  llvm::SmallString<256> Path(OutputDir);
  llvm::sys::path::append(Path, Filename);

  std::error_code EC;
  auto OutputFile =
      std::make_unique<llvm::ToolOutputFile>(Path, EC, llvm::sys::fs::OF_None);
  if (EC) {
    llvm::errs() << "Failed to create output file: " << Path << "!\n";
    return nullptr;
  }
  return OutputFile;
}

class SBVisitor : public RecursiveASTVisitor<SBVisitor> {
public:
  SBVisitor(std::set<std::string>& ClassNames) : ClassNames(ClassNames) {}

  bool VisitCXXRecordDecl(CXXRecordDecl *Decl) {
    // Skip implicit declarations and non-definitions
    if (!Decl->isThisDeclarationADefinition() || Decl->isImplicit())
      return true;

    // Skip redeclarations (we only want the canonical definition)
    if (Decl != Decl->getDefinition())
      return true;

    ClassNames.insert(Decl->getNameAsString());
    return true;
  }

private:
  std::set<std::string>& ClassNames;
};

class SBConsumer : public ASTConsumer {
public:
  SBConsumer(Rewriter &R, ASTContext &Context, std::set<std::string>& ClassNames) : Visitor(ClassNames) {}

  void HandleTranslationUnit(ASTContext &Context) override {
    Visitor.TraverseDecl(Context.getTranslationUnitDecl());
  }

private:
  SBVisitor Visitor;
};

class SBAction : public ASTFrontendAction {
public:
  explicit SBAction(std::set<std::string>& ClassNames) : ClassNames(ClassNames) {}

  bool BeginSourceFileAction(CompilerInstance &CI) override { return true; }

  void EndSourceFileAction() override { MyRewriter.overwriteChangedFiles(); }

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef File) override {
    MyRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<SBConsumer>(MyRewriter, CI.getASTContext(), ClassNames);
  }

private:
  std::set<std::string>& ClassNames;
  Rewriter MyRewriter;
};

class SBActionFactory : public clang::tooling::FrontendActionFactory {
public:
  explicit SBActionFactory(std::set<std::string> &ClassNames)
      : ClassNames(ClassNames) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<SBAction>(ClassNames);
  }

private:
  std::set<std::string> &ClassNames;
};

int main(int argc, const char **argv) {
  auto ExpectedParser = CommonOptionsParser::create(
      argc, argv, SBGenCategory, llvm::cl::OneOrMore,
      "Utility for generating the scripting bridge dynamic APIs for LLDB's "
      "framework.");
  if (!ExpectedParser) {
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  
  if (OutputDir.empty()) {
    llvm::errs() << "Please specify an output directory for the generated "
                    "files with --output-dir!\n";
    return 1;
  }
  
  // Create the output directory if the user specified one does not exist.
  if (!llvm::sys::fs::exists(OutputDir.getValue())) {
    llvm::sys::fs::create_directory(OutputDir.getValue());
  }
  
  CommonOptionsParser &OP = ExpectedParser.get();

  auto PCHOpts = std::make_shared<PCHContainerOperations>();
  PCHOpts->registerWriter(std::make_unique<ObjectFilePCHContainerWriter>());
  PCHOpts->registerReader(std::make_unique<ObjectFilePCHContainerReader>());

  ClangTool T(OP.getCompilations(), OP.getSourcePathList(), PCHOpts);

  std::set<std::string> ClassNames;
  SBActionFactory Factory(ClassNames);
  T.run(&Factory);

  if (Verbose)
    for (auto& class_name: ClassNames)
      std::cout << class_name << std::endl;

  return 0;
}
