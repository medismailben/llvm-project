//===-- CommandObjectTestPicker.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CommandObjectTestPicker.h"

#include "lldb/Core/Debugger.h"
#include "lldb/Core/IOHandlerPicker.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandReturnObject.h"

using namespace lldb;
using namespace lldb_private;

static const char *const g_names[] = {
    "alpha",  "bravo",    "charlie", "delta",  "echo",    "foxtrot", "golf",
    "hotel",  "india",    "juliet",  "kilo",   "lima",    "mike",    "november",
    "oscar",  "papa",     "quebec",  "romeo",  "sierra",  "tango",   "uniform",
    "victor", "whiskey",  "xray",    "yankee", "zulu",
};

/// Generates items in descending id order so that sorting has a visible
/// effect. Avoids <random> because its output differs across standard
/// libraries, which would make the tests non-portable.
static std::vector<PickerItem> GenerateItems(size_t count, PickerMode mode) {
  std::vector<PickerItem> items;
  for (size_t id = count; id > 0; --id) {
    PickerItem item;
    item.id = std::to_string(id);
    item.columns = {
        {"ID", item.id, /*is_numeric=*/true},
        {"NAME", g_names[(id - 1) % std::size(g_names)], /*is_numeric=*/false},
        {"VALUE", std::to_string(id * 7919 % 10000), /*is_numeric=*/true},
    };
    item.initially_selected = mode == PickerMode::MultiSelect && id % 3 == 1;
    items.push_back(std::move(item));
  }
  return items;
}

class CommandObjectTestPickerMode : public CommandObjectParsed {
public:
  CommandObjectTestPickerMode(CommandInterpreter &interpreter, PickerMode mode)
      : CommandObjectParsed(
            interpreter,
            mode == PickerMode::SingleSelect ? "test-picker single"
                                             : "test-picker multi",
            mode == PickerMode::SingleSelect
                ? "Test IOHandlerPicker in single-select mode."
                : "Test IOHandlerPicker in multi-select mode.",
            mode == PickerMode::SingleSelect ? "test-picker single [<count>]"
                                             : "test-picker multi [<count>]"),
        m_mode(mode) {}

protected:
  void DoExecute(Args &args, CommandReturnObject &result) override {
    size_t count = 10;
    if (args.GetArgumentCount() >= 1 &&
        args[0].ref().getAsInteger(10, count)) {
      result.AppendErrorWithFormatv("invalid count: '{0}'", args[0].ref());
      return;
    }

    auto picker_sp = std::make_shared<IOHandlerPicker>(
        GetDebugger(),
        m_mode == PickerMode::SingleSelect ? "Select an item:"
                                           : "Toggle items:",
        GenerateItems(count, m_mode), m_mode);
    GetDebugger().RunIOHandlerSync(picker_sp);

    const PickerResult &picked = picker_sp->GetResult();
    if (picked.was_canceled)
      result.AppendMessage("canceled");
    else if (picked.selected_ids.empty())
      result.AppendMessage("none selected");
    for (const std::string &id : picked.selected_ids)
      result.AppendMessageWithFormatv("selected: {0}", id);
    result.SetStatus(eReturnStatusSuccessFinishResult);
  }

private:
  PickerMode m_mode;
};

CommandObjectTestPicker::CommandObjectTestPicker(
    CommandInterpreter &interpreter)
    : CommandObjectMultiword(interpreter, "test-picker",
                             "Test the IOHandlerPicker interactive widget.",
                             "test-picker <subcommand> [<count>]") {
  LoadSubCommand("single", std::make_shared<CommandObjectTestPickerMode>(
                               interpreter, PickerMode::SingleSelect));
  LoadSubCommand("multi", std::make_shared<CommandObjectTestPickerMode>(
                              interpreter, PickerMode::MultiSelect));
}

CommandObjectTestPicker::~CommandObjectTestPicker() = default;
