//===-- CommandOptionsProcessAttach.h ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_COMMANDS_COMMANDOPTIONSPROCESSATTACH_H
#define LLDB_SOURCE_COMMANDS_COMMANDOPTIONSPROCESSATTACH_H

#include "lldb/Core/IOHandlerPicker.h"
#include "lldb/Interpreter/Options.h"
#include "lldb/Target/Platform.h"
#include "lldb/Target/Process.h"

namespace lldb_private {

class CommandReturnObject;

// CommandOptionsProcessAttach

class CommandOptionsProcessAttach : public lldb_private::OptionGroup {
public:
  CommandOptionsProcessAttach() {
    // Keep default values of all options in one place: OptionParsingStarting
    // ()
    OptionParsingStarting(nullptr);
  }

  ~CommandOptionsProcessAttach() override = default;

  lldb_private::Status
  SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                 lldb_private::ExecutionContext *execution_context) override;

  void OptionParsingStarting(
      lldb_private::ExecutionContext *execution_context) override {
    attach_info.Clear();
  }

  llvm::ArrayRef<lldb_private::OptionDefinition> GetDefinitions() override;

  // Instance variables to hold the values for command options.

  lldb_private::ProcessAttachInfo attach_info;
}; // CommandOptionsProcessAttach

/// Show an interactive process picker and return the selected PID.
/// Returns LLDB_INVALID_PROCESS_ID if canceled or no processes found.
/// The \a result object is updated with an error message on failure.
lldb::pid_t PickProcessToAttach(Debugger &debugger, Platform &platform,
                                CommandReturnObject &result);

} // namespace lldb_private

#endif // LLDB_SOURCE_COMMANDS_COMMANDOPTIONSPROCESSATTACH_H
