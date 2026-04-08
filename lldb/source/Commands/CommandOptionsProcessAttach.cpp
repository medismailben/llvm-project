//===-- CommandOptionsProcessAttach.cpp -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CommandOptionsProcessAttach.h"

#include "lldb/Core/Debugger.h"
#include "lldb/Core/IOHandlerPicker.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Host/OptionParser.h"
#include "lldb/Interpreter/CommandCompletions.h"
#include "lldb/Interpreter/CommandObject.h"
#include "lldb/Interpreter/CommandOptionArgumentTable.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Interpreter/OptionArgParser.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Platform.h"
#include "lldb/Target/Target.h"

#include "llvm/ADT/ArrayRef.h"

using namespace llvm;
using namespace lldb;
using namespace lldb_private;

#define LLDB_OPTIONS_process_attach
#include "CommandOptions.inc"

Status CommandOptionsProcessAttach::SetOptionValue(
    uint32_t option_idx, llvm::StringRef option_arg,
    ExecutionContext *execution_context) {
  Status error;
  const int short_option = g_process_attach_options[option_idx].short_option;
  switch (short_option) {
  case 'c':
    attach_info.SetContinueOnceAttached(true);
    break;

  case 'p': {
    lldb::pid_t pid;
    if (option_arg.getAsInteger(0, pid)) {
      return Status::FromErrorStringWithFormatv("invalid process ID '{0}'",
                                                option_arg);
    } else {
      attach_info.SetProcessID(pid);
    }
  } break;

  case 'P':
    attach_info.SetProcessPluginName(option_arg);
    break;

  case 'n':
    attach_info.GetExecutableFile().SetFile(option_arg,
                                            FileSpec::Style::native);
    break;

  case 'w':
    attach_info.SetWaitForLaunch(true);
    break;

  case 'i':
    attach_info.SetIgnoreExisting(false);
    break;

  default:
    llvm_unreachable("Unimplemented option");
  }
  return error;
}

llvm::ArrayRef<OptionDefinition> CommandOptionsProcessAttach::GetDefinitions() {
  return llvm::ArrayRef(g_process_attach_options);
}

lldb::pid_t lldb_private::PickProcessToAttach(Debugger &debugger,
                                              Platform &platform,
                                              CommandReturnObject &result) {
  ProcessInstanceInfoMatch match_info;
  ProcessInstanceInfoList proc_infos;
  platform.FindProcesses(match_info, proc_infos);

  if (proc_infos.empty()) {
    result.AppendError("no processes found on the current platform");
    return LLDB_INVALID_PROCESS_ID;
  }

  std::vector<PickerItem> items;
  for (const auto &proc : proc_infos) {
    PickerItem item;
    item.id = std::to_string(proc.GetProcessID());
    item.columns.push_back(
        {"PID", std::to_string(proc.GetProcessID()), /*is_numeric=*/true});
    item.columns.push_back(
        {"NAME", proc.GetName() ? proc.GetName() : "", /*is_numeric=*/false});
    item.columns.push_back(
        {"ARCH",
         proc.GetArchitecture().IsValid()
             ? proc.GetArchitecture().GetTriple().getArchName().str()
             : "",
         /*is_numeric=*/false});
    items.push_back(std::move(item));
  }

  IOHandlerSP picker_sp = std::make_shared<IOHandlerPicker>(
      debugger, "Select a process to attach to:", std::move(items),
      PickerMode::SingleSelect);
  debugger.RunIOHandlerSync(picker_sp);

  auto &picker = static_cast<IOHandlerPicker &>(*picker_sp);
  const PickerResult &r = picker.GetResult();

  if (r.was_canceled || r.selected_ids.empty())
    return LLDB_INVALID_PROCESS_ID;

  lldb::pid_t selected_pid = LLDB_INVALID_PROCESS_ID;
  llvm::StringRef(r.selected_ids[0]).getAsInteger(10, selected_pid);

  // Find the name of the selected process for the status message.
  for (const auto &proc : proc_infos) {
    if (proc.GetProcessID() == selected_pid) {
      llvm::StringRef name = proc.GetNameAsStringRef();
      if (!name.empty())
        result.AppendMessageWithFormatv("Attaching to \"{0}\" ({1})...", name,
                                        selected_pid);
      else
        result.AppendMessageWithFormatv("Attaching to process {0}...",
                                        selected_pid);
      break;
    }
  }

  return selected_pid;
}
