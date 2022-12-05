//===-- PlatformPOSIX.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_SCRIPTED_PLATFORM_H
#define LLDB_SOURCE_PLUGINS_SCRIPTED_PLATFORM_H

#include "lldb/Interpreter/ScriptedMetadata.h"
#include "lldb/Target/Platform.h"

namespace lldb_private {

class ScriptedPlatform : public Platform {
public:
  ScriptedPlatform(Debugger *debugger,
                   const ScriptedMetadata *scripted_metadata, Status &error);

  ~ScriptedPlatform() override;

  static lldb::PlatformSP CreateInstance(bool force, const ArchSpec *arch,
                                         const Debugger *debugger,
                                         const ScriptedMetadata *metadata);

  static void Initialize();

  static void Terminate();

  static llvm::StringRef GetPluginNameStatic() { return "scripted-platform"; }

  static llvm::StringRef GetDescriptionStatic() {
    return "Scripted Platform plug-in.";
  }

  llvm::StringRef GetDescription() override { return GetDescriptionStatic(); }

  llvm::StringRef GetPluginName() override { return GetPluginNameStatic(); }

  std::vector<ArchSpec>
  GetSupportedArchitectures(const ArchSpec &process_host_arch) override;

  bool IsConnected() const override { return true; }

  lldb::ProcessSP Attach(lldb_private::ProcessAttachInfo &attach_info,
                         lldb_private::Debugger &debugger,
                         lldb_private::Target *target, // Can be nullptr, if
                         // nullptr create a new
                         // target, else use
                         // existing one
                         lldb_private::Status &error) override;

  uint32_t FindProcesses(const ProcessInstanceInfoMatch &match_info,
                         ProcessInstanceInfoList &proc_infos) override;

  bool GetProcessInfo(lldb::pid_t pid, ProcessInstanceInfo &proc_info) override;

  Status LaunchProcess(ProcessLaunchInfo &launch_info) override;

  Status KillProcess(const lldb::pid_t pid) override;

  void CalculateTrapHandlerSymbolNames() override {}

private:
  ScriptedPlatform(const ScriptedPlatform &) = delete;
  const ScriptedPlatform &operator=(const ScriptedPlatform &) = delete;

  void CheckInterpreterAndScriptObject() const;
  ScriptedPlatformInterface &GetInterface() const;
  static bool IsScriptLanguageSupported(lldb::ScriptLanguage language);

  // Member variables.
  const ScriptedMetadata *m_scripted_metadata = nullptr;
  lldb_private::ScriptInterpreter *m_interpreter = nullptr;
  lldb_private::StructuredData::ObjectSP m_script_object_sp = nullptr;
  //@}
};

} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_SCRIPTED_PLATFORM_H
