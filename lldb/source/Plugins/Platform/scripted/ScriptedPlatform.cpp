#include "ScriptedPlatform.h"

#include "lldb/Core/Debugger.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Interpreter/ScriptInterpreter.h"
#include "lldb/Target/ExecutionContext.h"
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
  llvm::ArrayRef<lldb::ScriptLanguage> supported_languages =
      llvm::makeArrayRef(g_supported_script_languages);

  return llvm::is_contained(supported_languages, language);
}

ScriptedPlatformInterface &ScriptedPlatform::GetInterface() const {
  return m_interpreter->GetScriptedPlatformInterface();
}

void ScriptedPlatform::CheckInterpreterAndScriptObject() const {
  lldbassert(m_interpreter && "Invalid Script Interpreter.");
  lldbassert(m_script_object_sp && "Invalid Script Object.");
}

lldb::PlatformSP
ScriptedPlatform::CreateInstance(bool force, const ArchSpec *arch,
                                 const Debugger *debugger,
                                 const ScriptedMetadata *metadata) {
  Log *log = GetLog(LLDBLog::Platform);
  if (log) {
    const char *arch_name;
    if (arch && arch->GetArchitectureName())
      arch_name = arch->GetArchitectureName();
    else
      arch_name = "<null>";

    const char *triple_cstr =
        arch ? arch->GetTriple().getTriple().c_str() : "<null>";

    LLDB_LOGF(log,
              "ScriptedPlatform::%s(force=%s, arch={%s,%s}, debugger=%" PRIxPTR
              ")",
              __PRETTY_FUNCTION__, force ? "true" : "false", arch_name,
              triple_cstr, (uintptr_t)debugger);
  }

  if (!metadata)
    return {};

  if (!debugger || !IsScriptLanguageSupported(debugger->GetScriptLanguage()))
    return {};

  if (!metadata)
    return {};

  Status error;
  ScriptedPlatform *scripted_platform =
      new ScriptedPlatform(const_cast<Debugger *>(debugger), metadata, error);

  if (error.Success())
    return PlatformSP(scripted_platform);

  LLDB_LOGF(log, "ScriptedPlatform::%s() aborting creation of platform",
            __PRETTY_FUNCTION__);

  return {};
}

ScriptedPlatform::ScriptedPlatform(Debugger *debugger,
                                   const ScriptedMetadata *scripted_metadata,
                                   Status &error)
    : Platform(false), m_scripted_metadata(scripted_metadata) {
  if (!debugger) {
    error.SetErrorStringWithFormat("ScriptedPlatform::%s () - ERROR: %s",
                                   __FUNCTION__, "Invalid debugger");
    return;
  }

  m_interpreter = debugger->GetScriptInterpreter();

  if (!m_interpreter) {
    error.SetErrorStringWithFormat("ScriptedPlatform::%s () - ERROR: %s",
                                   __FUNCTION__,
                                   "Debugger has no Script Interpreter");
    return;
  }

  ExecutionContext e;

  StructuredData::GenericSP object_sp = GetInterface().CreatePluginObject(
      m_scripted_metadata->GetClassName(), e,
      m_scripted_metadata->GetArgsSP());

  if (!object_sp || !object_sp->IsValid()) {
    error.SetErrorStringWithFormat("ScriptedPlatform::%s () - ERROR: %s",
                                   __FUNCTION__,
                                   "Failed to create valid script object");
    return;
  }

  m_hostname = GetHostPlatform()->GetHostname();
  m_script_object_sp = object_sp;
}

ScriptedPlatform::~ScriptedPlatform() {}

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
#if defined(__arm__) || defined(__arm64__) || defined(__aarch64__)
  llvm::Triple::OSType host_os = llvm::Triple::MacOSX;

  if (host_os == llvm::Triple::MacOSX) {
    // We can't use x86GetSupportedArchitectures() because it uses
    // the system architecture for some of its return values and also
    // has a 32bits variant.
    result.push_back(ArchSpec("x86_64-apple-macosx"));
    result.push_back(ArchSpec("x86_64-apple-ios-macabi"));
    result.push_back(ArchSpec("arm64-apple-ios-macabi"));
    result.push_back(ArchSpec("arm64e-apple-ios-macabi"));

    // On Apple Silicon, the host platform is compatible with iOS triples to
    // support unmodified "iPhone and iPad Apps on Apple Silicon Macs". Because
    // the binaries are identical, we must rely on the host architecture to
    // tell them apart and mark the host platform as compatible or not.
    if (!process_host_arch ||
        process_host_arch.GetTriple().getOS() == llvm::Triple::MacOSX) {
      result.push_back(ArchSpec("arm64-apple-ios"));
      result.push_back(ArchSpec("arm64e-apple-ios"));
    }
  }
#else
  x86GetSupportedArchitectures(result);
  result.push_back(ArchSpec("x86_64-apple-ios-macabi"));
#endif
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
  return nullptr;
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

  auto parse_process_info = [&proc_infos](ConstString key,
                                          StructuredData::Object *val) {
    if (!val)
      return false;

    StructuredData::Dictionary *dict = val->GetAsDictionary();

    if (!dict || !dict->IsValid())
      return false;

    bool has_name = dict->HasKey("name");
    bool has_arch = dict->HasKey("arch");
    if (!has_name && !has_arch)
      return false;

    llvm::StringRef result;
    if (!dict->GetValueForKeyAsString("name", result))
      return false;
    std::string name = result.str();

    if (!dict->GetValueForKeyAsString("arch", result))
      return false;
    const ArchSpec arch(result.data());
    if (!arch.IsValid())
      return false;

    lldb::pid_t pid = LLDB_INVALID_PROCESS_ID;
    if (!llvm::to_integer(key.GetStringRef(), pid))
      return false;

    ProcessInstanceInfo proc(name.c_str(), arch, pid);

    lldb::pid_t parent = LLDB_INVALID_PROCESS_ID;
    if (dict->GetValueForKeyAsInteger("parent", parent))
      proc.SetParentProcessID(parent);

    uint32_t uid = UINT32_MAX;
    if (dict->GetValueForKeyAsInteger("uid", uid))
      proc.SetEffectiveUserID(uid);

    uint32_t gid = UINT32_MAX;
    if (dict->GetValueForKeyAsInteger("gid", gid))
      proc.SetEffectiveGroupID(gid);

    proc_infos.push_back(proc);

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

  bool has_name = dict_sp->HasKey("name");
  bool has_arch = dict_sp->HasKey("arch");
  if (!has_name && !has_arch)
    return false;

  llvm::StringRef result;
  if (!dict_sp->GetValueForKeyAsString("name", result))
    return false;
  std::string name = result.str();

  if (!dict_sp->GetValueForKeyAsString("arch", result))
    return false;
  const ArchSpec arch(result.data());
  if (!arch.IsValid())
    return false;

  proc_info = ProcessInstanceInfo(name.c_str(), arch, pid);

  lldb::pid_t parent = LLDB_INVALID_PROCESS_ID;
  if (dict_sp->GetValueForKeyAsInteger("parent", parent))
    proc_info.SetParentProcessID(parent);

  uint32_t uid = UINT32_MAX;
  if (dict_sp->GetValueForKeyAsInteger("uid", uid))
    proc_info.SetEffectiveUserID(uid);

  uint32_t gid = UINT32_MAX;
  if (dict_sp->GetValueForKeyAsInteger("gid", gid))
    proc_info.SetEffectiveGroupID(gid);

  return true;
}

Status ScriptedPlatform::LaunchProcess(ProcessLaunchInfo &launch_info) {
  return GetInterface().LaunchProcess();
}

Status ScriptedPlatform::KillProcess(const lldb::pid_t pid) {
  return GetInterface().KillProcess(pid);
}
