//===-- ScriptedPlatform.cpp ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ScriptedPlatform.h"

#include "lldb/Core/Debugger.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Interpreter/ScriptInterpreter.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Utility/LLDBLog.h"

using namespace lldb;
using namespace lldb_private;

LLDB_PLUGIN_DEFINE(ScriptedPlatform)

static uint32_t g_initialize_count = 0;

static constexpr lldb::ScriptLanguage g_supported_script_languages[] = {
    ScriptLanguage::eScriptLanguagePython,
};

bool ScriptedPlatform::IsScriptLanguageSupported(
    lldb::ScriptLanguage language) {
  llvm::ArrayRef<lldb::ScriptLanguage> supported_languages(
      g_supported_script_languages);

  return llvm::is_contained(supported_languages, language);
}

ScriptedPlatformInterface &ScriptedPlatform::GetInterface() const {
  return *m_interface_up;
}

lldb::PlatformSP ScriptedPlatform::CreateInstance(bool force,
                                                  const ArchSpec *arch) {
  Log *log = GetLog(LLDBLog::Platform);
  if (log) {
    const char *arch_name;
    if (arch && arch->GetArchitectureName())
      arch_name = arch->GetArchitectureName();
    else
      arch_name = "<null>";

    const char *triple_cstr =
        arch ? arch->GetTriple().getTriple().c_str() : "<null>";

    LLDB_LOGF(log, "ScriptedPlatform::%s(force=%s, arch={%s,%s})",
              __PRETTY_FUNCTION__, force ? "true" : "false", arch_name,
              triple_cstr);
  }

  return std::make_shared<ScriptedPlatform>();
}

ScriptedPlatform::ScriptedPlatform() : Platform(false) {}

bool ScriptedPlatform::SetupScriptedObject(Status &error) {

  auto error_with_message = [&error](llvm::StringRef message) {
    return ScriptedInterface::ErrorWithMessage<bool>(
        LLVM_PRETTY_FUNCTION, message, error, LLDBLog::Platform);
  };

  Debugger &debugger = m_metadata->GetDebugger();

  if (!IsScriptLanguageSupported(debugger.GetScriptLanguage()))
    return error_with_message("Debugger language not supported");

  ScriptInterpreter *interpreter = debugger.GetScriptInterpreter();
  if (!interpreter)
    return error_with_message("Debugger has no Script Interpreter");

  // Create platform instance interface
  m_interface_up = interpreter->CreateScriptedPlatformInterface();
  if (!m_interface_up)
    return error_with_message(
        "Script interpreter couldn't create Scripted Process Interface");

  const ScriptedMetadata scripted_metadata = m_metadata->GetScriptedMetadata();

  ExecutionContext e;
  auto obj_or_err = GetInterface().CreatePluginObject(
      scripted_metadata.GetClassName(), e, scripted_metadata.GetArgsSP());

  if (!obj_or_err) {
    llvm::consumeError(obj_or_err.takeError());
    return error_with_message("Failed to create script object.");
  }

  StructuredData::GenericSP object_sp = *obj_or_err;
  if (!object_sp || !object_sp->IsValid())
    return error_with_message("Failed to create valid script object");

  m_hostname = GetHostPlatform()->GetHostname();
  return true;
}

ScriptedPlatform::~ScriptedPlatform() {}

bool ScriptedPlatform::ReloadMetadata() {
  if (!m_metadata)
    return false;

  Status error;
  return SetupScriptedObject(error);
}

void ScriptedPlatform::Initialize() {
  if (g_initialize_count++ == 0) {
    // NOTE: This should probably be using the driving process platform
    PluginManager::RegisterPlugin(ScriptedPlatform::GetPluginNameStatic(),
                                  ScriptedPlatform::GetDescriptionStatic(),
                                  ScriptedPlatform::CreateInstance);
  }
}

void ScriptedPlatform::Terminate() {
  if (g_initialize_count > 0) {
    if (--g_initialize_count == 0) {
      PluginManager::UnregisterPlugin(ScriptedPlatform::CreateInstance);
    }
  }
}

std::vector<ArchSpec>
ScriptedPlatform::GetSupportedArchitectures(const ArchSpec &process_host_arch) {
  std::vector<ArchSpec> result;
  result.push_back(process_host_arch);
  return result;
}

lldb::ProcessSP
ScriptedPlatform::Attach(lldb_private::ProcessAttachInfo &attach_info,
                         lldb_private::Debugger &debugger,
                         lldb_private::Target *target, // Can be nullptr, if
                                                       // nullptr create a new
                                                       // target, else use
                                                       // existing one
                         lldb_private::Status &error) {
  lldb::ProcessAttachInfoSP attach_info_sp =
      std::make_shared<ProcessAttachInfo>(attach_info);

  if (!target) {
    target = &debugger.GetSelectedOrDummyTarget();
  }

  ProcessSP process_sp =
      GetInterface().AttachToProcess(attach_info_sp, target->shared_from_this(),
                                     debugger.shared_from_this(), error);
  if (!process_sp || error.Fail())
    return {};
  return process_sp;
}

llvm::Expected<ProcessInstanceInfo>
ScriptedPlatform::ParseProcessInfo(StructuredData::Dictionary &dict,
                                   lldb::pid_t pid) const {
  if (!dict.HasKey("name"))
    return llvm::make_error<llvm::StringError>(
        "No 'arch' key in process info dictionary.",
        llvm::inconvertibleErrorCode());
  if (!dict.HasKey("arch"))
    return llvm::make_error<llvm::StringError>(
        "No 'arch' key in process info dictionary.",
        llvm::inconvertibleErrorCode());

  llvm::StringRef result;
  if (!dict.GetValueForKeyAsString("name", result))
    return llvm::make_error<llvm::StringError>(
        "Couldn't extract 'name' key from process info dictionary.",
        llvm::inconvertibleErrorCode());
  std::string name = result.str();

  if (!dict.GetValueForKeyAsString("arch", result))
    return llvm::make_error<llvm::StringError>(
        "Couldn't extract 'arch' key from process info dictionary.",
        llvm::inconvertibleErrorCode());
  const ArchSpec arch(result.data());
  if (!arch.IsValid())
    return llvm::make_error<llvm::StringError>(
        "Invalid 'arch' key in process info dictionary.",
        llvm::inconvertibleErrorCode());

  ProcessInstanceInfo proc_info = ProcessInstanceInfo(name.c_str(), arch, pid);

  lldb::pid_t parent = LLDB_INVALID_PROCESS_ID;
  if (dict.GetValueForKeyAsInteger("parent", parent))
    proc_info.SetParentProcessID(parent);

  uint32_t uid = UINT32_MAX;
  if (dict.GetValueForKeyAsInteger("uid", uid))
    proc_info.SetEffectiveUserID(uid);

  uint32_t gid = UINT32_MAX;
  if (dict.GetValueForKeyAsInteger("gid", gid))
    proc_info.SetEffectiveGroupID(gid);

  return proc_info;
}

uint32_t
ScriptedPlatform::FindProcesses(const ProcessInstanceInfoMatch &match_info,
                                ProcessInstanceInfoList &proc_infos) {
  CheckInterpreterAndScriptObject();
  StructuredData::DictionarySP dict_sp = GetInterface().ListProcesses();

  Status error;
  if (!dict_sp)
    return ScriptedInterface::ErrorWithMessage<uint32_t>(
        LLVM_PRETTY_FUNCTION, "Failed to get scripted platform processes.",
        error, LLDBLog::Platform);

  auto parse_process_info = [this,
                             &proc_infos](llvm::StringRef key,
                                          StructuredData::Object *val) -> bool {
    if (!val)
      return false;

    StructuredData::Dictionary *dict = val->GetAsDictionary();

    if (!dict || !dict->IsValid())
      return false;

    lldb::pid_t pid = LLDB_INVALID_PROCESS_ID;
    if (!llvm::to_integer(key, pid))
      return false;

    auto proc_info_or_error = ParseProcessInfo(*dict, pid);

    if (llvm::Error e = proc_info_or_error.takeError()) {
      LLDB_LOGF(GetLog(LLDBLog::Platform), "%s ERROR = %s",
                LLVM_PRETTY_FUNCTION, llvm::toString(std::move(e)).c_str());
      return false;
    }

    if (!proc_info_or_error) {
      llvm::consumeError(proc_info_or_error.takeError());
      return false;
    }

    proc_infos.push_back(*proc_info_or_error);
    return true;
  };

  dict_sp->ForEach(parse_process_info);

  // TODO: Use match_info to filter through processes
  return proc_infos.size();
}

bool ScriptedPlatform::GetProcessInfo(lldb::pid_t pid,
                                      ProcessInstanceInfo &proc_info) {
  if (pid == LLDB_INVALID_PROCESS_ID)
    return false;

  StructuredData::DictionarySP dict_sp = GetInterface().GetProcessInfo(pid);

  if (!dict_sp || !dict_sp->IsValid())
    return false;

  auto proc_info_or_error = ParseProcessInfo(*dict_sp.get(), pid);

  if (!proc_info_or_error) {
    llvm::consumeError(proc_info_or_error.takeError());
    return false;
  }

  proc_info = *proc_info_or_error;
  return true;
}

Status ScriptedPlatform::LaunchProcess(ProcessLaunchInfo &launch_info) {
  lldb::ProcessLaunchInfoSP launch_info_sp =
      std::make_shared<ProcessLaunchInfo>(launch_info);
  return GetInterface().LaunchProcess(launch_info_sp);
}

Status ScriptedPlatform::KillProcess(const lldb::pid_t pid) {
  return GetInterface().KillProcess(pid);
}
