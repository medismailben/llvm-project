//===-- CommandObjectTestPicker.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CommandObjectTestPicker.h"

#include "lldb/Core/IOHandlerPicker.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandReturnObject.h"

#include <random>

using namespace lldb;
using namespace lldb_private;

static const char *const kNames[] = {
    "alpha",   "bravo",   "charlie", "delta",   "echo",    "foxtrot",
    "golf",    "hotel",   "india",   "juliet",  "kilo",    "lima",
    "mike",    "november","oscar",   "papa",    "quebec",  "romeo",
    "sierra",  "tango",   "uniform", "victor",  "whiskey", "xray",
    "yankee",  "zulu",
};

static std::vector<PickerItem> GenerateItems(size_t count, PickerMode mode) {
  std::mt19937 rng(42); // Fixed seed for reproducibility.
  std::uniform_int_distribution<int> value_dist(1, 9999);
  constexpr size_t kNameCount = sizeof(kNames) / sizeof(kNames[0]);

  std::vector<PickerItem> items;
  for (size_t i = 0; i < count; ++i) {
    PickerItem item;
    item.id = std::to_string(i + 1);
    item.columns.push_back(
        {"ID", std::to_string(i + 1), /*is_numeric=*/true});
    item.columns.push_back(
        {"NAME", kNames[i % kNameCount], /*is_numeric=*/false});
    item.columns.push_back(
        {"VALUE", std::to_string(value_dist(rng)), /*is_numeric=*/true});
    if (mode == PickerMode::MultiSelect && i % 3 == 0)
      item.initially_selected = true;
    items.push_back(std::move(item));
  }

  // Shuffle so the list starts unsorted.
  std::shuffle(items.begin(), items.end(), rng);
  return items;
}

// test-picker single [count]
class CommandObjectTestPickerSingle : public CommandObjectParsed {
public:
  CommandObjectTestPickerSingle(CommandInterpreter &interpreter)
      : CommandObjectParsed(interpreter, "test-picker single",
                            "Test IOHandlerPicker in single-select mode.",
                            "test-picker single [<count>]") {}

  ~CommandObjectTestPickerSingle() override = default;

protected:
  void DoExecute(Args &args, CommandReturnObject &result) override {
    size_t count = 10;
    if (args.GetArgumentCount() >= 1) {
      uint64_t n;
      if (!args[0].ref().getAsInteger(10, n))
        count = static_cast<size_t>(n);
    }

    auto items = GenerateItems(count, PickerMode::SingleSelect);

    Debugger &debugger = GetDebugger();
    IOHandlerSP picker_sp = std::make_shared<IOHandlerPicker>(
        debugger, "Select an item:", std::move(items),
        PickerMode::SingleSelect);
    debugger.RunIOHandlerSync(picker_sp);

    auto &picker = static_cast<IOHandlerPicker &>(*picker_sp);
    const PickerResult &r = picker.GetResult();

    if (r.was_canceled) {
      result.AppendMessage("canceled");
    } else {
      for (const auto &id : r.selected_ids)
        result.AppendMessage(("selected: " + id).c_str());
    }
    result.SetStatus(eReturnStatusSuccessFinishResult);
  }
};

// test-picker multi [count]
class CommandObjectTestPickerMulti : public CommandObjectParsed {
public:
  CommandObjectTestPickerMulti(CommandInterpreter &interpreter)
      : CommandObjectParsed(interpreter, "test-picker multi",
                            "Test IOHandlerPicker in multi-select mode.",
                            "test-picker multi [<count>]") {}

  ~CommandObjectTestPickerMulti() override = default;

protected:
  void DoExecute(Args &args, CommandReturnObject &result) override {
    size_t count = 10;
    if (args.GetArgumentCount() >= 1) {
      uint64_t n;
      if (!args[0].ref().getAsInteger(10, n))
        count = static_cast<size_t>(n);
    }

    auto items = GenerateItems(count, PickerMode::MultiSelect);

    Debugger &debugger = GetDebugger();
    IOHandlerSP picker_sp = std::make_shared<IOHandlerPicker>(
        debugger, "Toggle items:", std::move(items), PickerMode::MultiSelect);
    debugger.RunIOHandlerSync(picker_sp);

    auto &picker = static_cast<IOHandlerPicker &>(*picker_sp);
    const PickerResult &r = picker.GetResult();

    if (r.was_canceled) {
      result.AppendMessage("canceled");
    } else if (r.selected_ids.empty()) {
      result.AppendMessage("none selected");
    } else {
      for (const auto &id : r.selected_ids)
        result.AppendMessage(("selected: " + id).c_str());
    }
    result.SetStatus(eReturnStatusSuccessFinishResult);
  }
};

CommandObjectTestPicker::CommandObjectTestPicker(
    CommandInterpreter &interpreter)
    : CommandObjectMultiword(
          interpreter, "test-picker",
          "Test the IOHandlerPicker interactive widget.",
          "test-picker <subcommand> [<count>]") {
  LoadSubCommand(
      "single",
      CommandObjectSP(new CommandObjectTestPickerSingle(interpreter)));
  LoadSubCommand(
      "multi",
      CommandObjectSP(new CommandObjectTestPickerMulti(interpreter)));
}

CommandObjectTestPicker::~CommandObjectTestPicker() = default;
