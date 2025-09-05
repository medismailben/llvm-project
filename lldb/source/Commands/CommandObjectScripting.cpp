//===-- CommandObjectScripting.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CommandObjectScripting.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/DataFormatters/DataVisualization.h"
#include "lldb/Host/Config.h"
#include "lldb/Host/OptionParser.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandOptionArgumentTable.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Interpreter/Interfaces/ScriptedInterfaceUsages.h"
#include "lldb/Interpreter/OptionArgParser.h"
#include "lldb/Interpreter/ScriptInterpreter.h"
#include "lldb/Utility/Args.h"

#include "llvm/ADT/StringMap.h"

using namespace lldb;
using namespace lldb_private;

#define LLDB_OPTIONS_scripting_run
#include "CommandOptions.inc"

class CommandObjectScriptingRun : public CommandObjectRaw {
public:
  CommandObjectScriptingRun(CommandInterpreter &interpreter)
      : CommandObjectRaw(
            interpreter, "scripting run",
            "Invoke the script interpreter with provided code and display any "
            "results.  Start the interactive interpreter if no code is "
            "supplied.",
            "scripting run [--language <scripting-language> --] "
            "[<script-code>]") {}

  ~CommandObjectScriptingRun() override = default;

  Options *GetOptions() override { return &m_options; }

  class CommandOptions : public Options {
  public:
    CommandOptions() = default;
    ~CommandOptions() override = default;
    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override {
      Status error;
      const int short_option = m_getopt_table[option_idx].val;

      switch (short_option) {
      case 'l':
        language = (lldb::ScriptLanguage)OptionArgParser::ToOptionEnum(
            option_arg, GetDefinitions()[option_idx].enum_values,
            eScriptLanguageNone, error);
        if (!error.Success())
          error = Status::FromErrorStringWithFormat(
              "unrecognized value for language '%s'", option_arg.str().c_str());
        break;
      default:
        llvm_unreachable("Unimplemented option");
      }

      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      language = lldb::eScriptLanguageNone;
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
      return llvm::ArrayRef(g_scripting_run_options);
    }

    lldb::ScriptLanguage language = lldb::eScriptLanguageNone;
  };

protected:
  void DoExecute(llvm::StringRef command,
                 CommandReturnObject &result) override {
    // Try parsing the language option but when the command contains a raw part
    // separated by the -- delimiter.
    OptionsWithRaw raw_args(command);
    if (raw_args.HasArgs()) {
      if (!ParseOptions(raw_args.GetArgs(), result))
        return;
      command = raw_args.GetRawPart();
    }

    lldb::ScriptLanguage language =
        (m_options.language == lldb::eScriptLanguageNone)
            ? m_interpreter.GetDebugger().GetScriptLanguage()
            : m_options.language;

    if (language == lldb::eScriptLanguageNone) {
      result.AppendError(
          "the script-lang setting is set to none - scripting not available");
      return;
    }

    ScriptInterpreter *script_interpreter =
        GetDebugger().GetScriptInterpreter(true, language);

    if (script_interpreter == nullptr) {
      result.AppendError("no script interpreter");
      return;
    }

    // Script might change Python code we use for formatting. Make sure we keep
    // up to date with it.
    DataVisualization::ForceUpdate();

    if (command.empty()) {
      script_interpreter->ExecuteInterpreterLoop();
      result.SetStatus(eReturnStatusSuccessFinishNoResult);
      return;
    }

    // We can do better when reporting the status of one-liner script execution.
    if (script_interpreter->ExecuteOneLine(command, &result))
      result.SetStatus(eReturnStatusSuccessFinishNoResult);
    else
      result.SetStatus(eReturnStatusFailed);
  }

private:
  CommandOptions m_options;
};

#define LLDB_OPTIONS_scripting_extension_list
#include "CommandOptions.inc"

class CommandObjectScriptedExtensionList : public CommandObjectParsed {
public:
  CommandObjectScriptedExtensionList(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "scripting extension list",
            "List all the available scripting extension templates. ",
            "scripting extension list [--language <scripting-language> --]") {}

  ~CommandObjectScriptedExtensionList() override = default;

  Options *GetOptions() override { return &m_options; }

  class CommandOptions : public Options {
  public:
    CommandOptions() = default;
    ~CommandOptions() override = default;
    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override {
      Status error;
      const int short_option = m_getopt_table[option_idx].val;

      switch (short_option) {
      case 'l':
        m_language = (lldb::ScriptLanguage)OptionArgParser::ToOptionEnum(
            option_arg, GetDefinitions()[option_idx].enum_values,
            eScriptLanguageNone, error);
        if (!error.Success())
          error = Status::FromErrorStringWithFormatv(
              "unrecognized value for language '{0}'", option_arg);
        break;
      default:
        llvm_unreachable("Unimplemented option");
      }

      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      m_language = lldb::eScriptLanguageDefault;
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
      return llvm::ArrayRef(g_scripting_extension_list_options);
    }

    lldb::ScriptLanguage m_language = lldb::eScriptLanguageDefault;
  };

protected:
  void DoExecute(Args &command, CommandReturnObject &result) override {
    Stream &s = result.GetOutputStream();
    s.Printf("Available scripted extension templates:");

    auto print_field = [&s](llvm::StringRef key, llvm::StringRef value) {
      if (!value.empty()) {
        s.IndentMore();
        s.Indent();
        s << key << ": " << value << '\n';
        s.IndentLess();
      }
    };

    llvm::StringMap<std::vector<size_t>> grouped_by_extension;
    for (size_t i = 0; i < PluginManager::GetNumScriptedInterfaces(); i++) {
      lldb::ScriptedExtension extension =
          PluginManager::GetScriptedInterfaceExtensionAtIndex(i);
      if (extension == eScriptedExtensionInvalid)
        continue;

      llvm::StringLiteral extension_name =
          ScriptInterpreter::ExtensionToString(extension);
      if (grouped_by_extension.contains(extension_name))
        grouped_by_extension[extension_name].push_back(i);
      else
        grouped_by_extension[extension_name] = {i};
    }

    size_t num_listed_interface = 0;
    size_t num_extensions = grouped_by_extension.size();
    for (const auto &extension_pair : grouped_by_extension) {
      if (!num_listed_interface)
        s.EOL();
      num_listed_interface++;

      llvm::StringRef desc =
          PluginManager::GetScriptedInterfaceDescriptionAtIndex(
              extension_pair.second[0]);
      ScriptedInterfaceUsages usages =
          PluginManager::GetScriptedInterfaceUsagesAtIndex(
              extension_pair.second[0]);

      std::vector<llvm::StringRef> languages;
      for (const size_t idx : extension_pair.second) {
        lldb::ScriptLanguage lang =
            PluginManager::GetScriptedInterfaceLanguageAtIndex(idx);
        if (lang != m_options.m_language)
          continue;
        languages.push_back(ScriptInterpreter::LanguageToString(lang));
      }

      print_field("Name", extension_pair.first());
      print_field("Description", desc);
      print_field("Language", llvm::join(languages, ""));
      usages.Dump(s, ScriptedInterfaceUsages::UsageKind::API);
      usages.Dump(s, ScriptedInterfaceUsages::UsageKind::CommandInterpreter);

      if (num_listed_interface != num_extensions - 1)
        s.EOL();
    }

    if (!num_listed_interface)
      s << " None\n";
  }

private:
  CommandOptions m_options;
};

#define LLDB_OPTIONS_scripting_extension_generate
#include "CommandOptions.inc"

class CommandObjectScriptedExtensionGenerate : public CommandObjectParsed {
public:
  CommandObjectScriptedExtensionGenerate(CommandInterpreter &interpreter)
      : CommandObjectParsed(interpreter, "scripting extension generate",
                            "Generate a scripting extension template. ",
                            "scripting extension generate") {
    AddSimpleArgumentList(eArgTypeScriptedExtension, eArgRepeatPlus);
  }

  ~CommandObjectScriptedExtensionGenerate() override = default;

  Options *GetOptions() override { return &m_options; }

  class CommandOptions : public Options {
  public:
    CommandOptions() = default;
    ~CommandOptions() override = default;
    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override {
      Status error;
      const char short_option =
          g_scripting_extension_generate_options[option_idx].short_option;
      const char *long_option =
          g_scripting_extension_generate_options[option_idx].long_option;

      switch (short_option) {
      case 'a':
        bool success;
        if (OptionArgParser::ToBoolean(option_arg, true, &success))
          m_generate_non_abstract_methods = eLazyBoolYes;
        else
          m_generate_non_abstract_methods = eLazyBoolNo;

        if (!success)
          error = Status::FromError(
              CreateOptionParsingError(option_arg, short_option, long_option,
                                       g_bool_parsing_error_message));
        break;
      case 'l':
        m_language = (lldb::ScriptLanguage)OptionArgParser::ToOptionEnum(
            option_arg, GetDefinitions()[option_idx].enum_values,
            eScriptLanguageNone, error);
        if (!error.Success())
          error = Status::FromErrorStringWithFormatv(
              "unrecognized value for language '{0}'", option_arg);
        break;
      case 'n':
        m_generated_class_prefix = option_arg.str();
        break;
      case 'o':
        m_output_filepath = option_arg.str();
        break;
      default:
        llvm_unreachable("Unimplemented option");
      }

      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      m_generate_non_abstract_methods = eLazyBoolCalculate;
      m_language = lldb::eScriptLanguageDefault;
      m_generated_class_prefix.clear();
      m_output_filepath.clear();
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
      return llvm::ArrayRef(g_scripting_extension_generate_options);
    }

    LazyBool m_generate_non_abstract_methods;
    lldb::ScriptLanguage m_language = lldb::eScriptLanguageDefault;
    std::string m_generated_class_prefix;
    std::string m_output_filepath;
  };

  void
  HandleArgumentCompletion(CompletionRequest &request,
                           OptionElementVector &opt_element_vector) override {
    uint32_t completion_mask =
        lldb::eScriptedExtensionCompletion | lldb::eDiskFileCompletion;
    lldb_private::CommandCompletions::InvokeCommonCompletionCallbacks(
        GetCommandInterpreter(), completion_mask, request, nullptr);
  }

protected:
  void DoExecute(Args &command, CommandReturnObject &result) override {
    if (command.GetArgumentCount() == 0) {
      result.SetError(
          Status::FromErrorString("specify extension name to generate"));
      return;
    }

    std::vector<std::pair<llvm::StringRef, llvm::SmallVector<llvm::StringRef>>>
        name_import_pair;

    for (size_t i = 0; i < command.GetArgumentCount(); i++) {
      llvm::StringRef extension_name = command.GetArgumentAtIndex(i);
      llvm::SmallVector<llvm::StringRef> extension_components;
      extension_name.split(extension_components, ".");
      lldb::ScriptedExtension extension =
          ScriptInterpreter::StringToExtension(extension_components.back());
      if (extension == eScriptedExtensionInvalid) {
        result.SetError(Status::FromErrorString("invalid extension name"));
        return;
      }
      name_import_pair.push_back({extension_name, extension_components});
    }

    lldb::ScriptLanguage language =
        (m_options.m_language == lldb::eScriptLanguageNone)
            ? m_interpreter.GetDebugger().GetScriptLanguage()
            : m_options.m_language;

    if (language == lldb::eScriptLanguageNone) {
      result.AppendError(
          "the script-lang setting is set to none - scripting not available");
      return;
    }

    ScriptInterpreter *script_interpreter =
        GetDebugger().GetScriptInterpreter(true, language);

    if (script_interpreter == nullptr) {
      result.AppendError("no script interpreter");
      return;
    }

    auto generated_file_or_err = script_interpreter->GenerateExtensionTemplate(
        m_options.m_generated_class_prefix, name_import_pair,
        m_options.m_generate_non_abstract_methods, m_options.m_output_filepath);
    if (!generated_file_or_err) {
      result.SetError(generated_file_or_err.takeError());
      return;
    }

    if (llvm::Error err = Host::OpenFileInExternalEditor(
            "", *generated_file_or_err, 1, true)) {
      result.SetError(std::move(err));
      return;
    }
    result.SetStatus(eReturnStatusSuccessFinishNoResult);
  }

private:
  CommandOptions m_options;
};

class CommandObjectMultiwordScriptedExtension : public CommandObjectMultiword {
public:
  CommandObjectMultiwordScriptedExtension(CommandInterpreter &interpreter)
      : CommandObjectMultiword(
            interpreter, "scripting extension",
            "Commands for operating on the scripting extensions.",
            "scripting extension [<subcommand-options>]") {
    LoadSubCommand(
        "list",
        CommandObjectSP(new CommandObjectScriptedExtensionList(interpreter)));
    LoadSubCommand("generate",
                   CommandObjectSP(new CommandObjectScriptedExtensionGenerate(
                       interpreter)));
  }

  ~CommandObjectMultiwordScriptedExtension() override = default;
};

CommandObjectMultiwordScripting::CommandObjectMultiwordScripting(
    CommandInterpreter &interpreter)
    : CommandObjectMultiword(
          interpreter, "scripting",
          "Commands for operating on the scripting functionalities.",
          "scripting <subcommand> [<subcommand-options>]") {
  LoadSubCommand("run",
                 CommandObjectSP(new CommandObjectScriptingRun(interpreter)));
  LoadSubCommand("extension",
                 CommandObjectSP(
                     new CommandObjectMultiwordScriptedExtension(interpreter)));
}

CommandObjectMultiwordScripting::~CommandObjectMultiwordScripting() = default;
