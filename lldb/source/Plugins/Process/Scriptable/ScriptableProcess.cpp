//===-- ScriptableProcess.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===--------------------------------------------------------------------===//

#include "ScriptableProcess.h"

#include "lldb/Core/Debugger.h"
#include "lldb/Core/Module.h"

//#include "lldb/Core/ModuleSpec.h"

#include "lldb/Core/PluginManager.h"

//#include "lldb/Core/Section.h"

#include "lldb/Host/OptionParser.h"

#include "lldb/Interpreter/OptionArgParser.h"
#include "lldb/Interpreter/OptionGroupBoolean.h"
#include "lldb/Interpreter/ScriptInterpreter.h"

#include "lldb/Target/JITLoaderList.h"
//#include "lldb/Target/MemoryRegionInfo.h"
//#include "lldb/Target/SectionLoadList.h"
//#include "lldb/Target/Target.h"
//#include "lldb/Target/UnixSignals.h"
//#include "lldb/Utility/LLDBAssert.h"
//#include "lldb/Utility/Log.h"
//#include "lldb/Utility/State.h"
//#include "llvm/BinaryFormat/Magic.h"
//#include "llvm/Support/MemoryBuffer.h"
//#include "llvm/Support/Threading.h"

//#include <memory>

using namespace lldb;
using namespace lldb_private;
using namespace scriptable;

LLDB_PLUGIN_DEFINE(ScriptableProcess)

ConstString ScriptableProcess::GetPluginNameStatic() {
  static ConstString g_name("Scriptable Process");
  return g_name;
}

const char *ScriptableProcess::GetPluginDescriptionStatic() {
  return "Scriptable Process plug-in.";
}

lldb::ProcessSP ScriptableProcess::CreateInstance(lldb::TargetSP target_sp,
                                                  lldb::ListenerSP listener_sp,
                                                  const FileSpec *crash_file) {
  //  if (!crash_file)
  return nullptr;
  //
  //  lldb::ProcessSP process_sp;
  //    // Read enough data for the Minidump header
  //  constexpr size_t header_size = sizeof(Header);
  //  auto DataPtr =
  //  FileSystem::Instance().CreateDataBuffer(crash_file->GetPath(),
  //                                                         header_size, 0);
  //  if (!DataPtr)
  //    return nullptr;
  //
  //  lldbassert(DataPtr->GetByteSize() == header_size);
  //  if (identify_magic(toStringRef(DataPtr->GetData())) !=
  //  llvm::file_magic::minidump)
  //    return nullptr;
  //
  //  auto AllData =
  //  FileSystem::Instance().CreateDataBuffer(crash_file->GetPath(), -1, 0);
  //  if (!AllData)
  //    return nullptr;
  //
  //  return std::make_shared<ProcessMinidump>(target_sp, listener_sp,
  //  *crash_file,
  //                                           std::move(AllData));
}

bool ScriptableProcess::CanDebug(lldb::TargetSP target_sp,
                                 bool plugin_specified_by_name) {
  return true;
}

ScriptableProcess::ScriptableProcess(lldb::TargetSP target_sp,
                                     lldb::ListenerSP listener_sp,
                                     const FileSpec &core_file,
                                     DataBufferSP core_data)
    : Process(target_sp, listener_sp)
//, m_core_file(core_file),
// m_core_data(std::move(core_data)), m_is_wow64(false)
{}

ScriptableProcess::~ScriptableProcess() {
  Clear();
  // We need to call finalize on the process before destroying ourselves to
  // make sure all of the broadcaster cleanup goes as planned. If we destruct
  // this class, then Process::~Process() might have problems trying to fully
  // destroy the broadcaster.
  Finalize();
}

void ScriptableProcess::Initialize() {
  static llvm::once_flag g_once_flag;

  llvm::call_once(g_once_flag, []() {
    PluginManager::RegisterPlugin(GetPluginNameStatic(),
                                  GetPluginDescriptionStatic(),
                                  ScriptableProcess::CreateInstance);
  });
}

void ScriptableProcess::Terminate() {
  PluginManager::UnregisterPlugin(ScriptableProcess::CreateInstance);
}

Status ScriptableProcess::DoLoadCore() {
  //  auto expected_parser = ScriptableProcess::Create(m_core_data);
  //  if (!expected_parser)
  //    return Status(expected_parser.takeError());
  //  m_minidump_parser = std::move(*expected_parser);
  //
  Status error;

  //    // Do we support the minidump's architecture?
  //  ArchSpec arch = GetArchitecture();
  //  switch (arch.GetMachine()) {
  //    case llvm::Triple::x86:
  //    case llvm::Triple::x86_64:
  //    case llvm::Triple::arm:
  //    case llvm::Triple::aarch64:
  //        // Any supported architectures must be listed here and also
  //        supported in
  //        // ThreadMinidump::CreateRegisterContextForFrame().
  //      break;
  //    default:
  //      error.SetErrorStringWithFormat("unsupported minidump architecture:
  //      %s",
  //                                     arch.GetArchitectureName());
  //      return error;
  //  }
  //  GetTarget().SetArchitecture(arch, true /*set_platform*/);
  //
  //  m_thread_list = m_minidump_parser->GetThreads();
  //  m_active_exception = m_minidump_parser->GetExceptionStream();
  //
  //  SetUnixSignals(UnixSignals::Create(GetArchitecture()));
  //
  //  ReadModuleList();
  //
  //  llvm::Optional<lldb::pid_t> pid = m_minidump_parser->GetPid();
  //  if (!pid) {
  //    GetTarget().GetDebugger().GetAsyncErrorStream()->PutCString(
  //                                                                "Unable to
  //                                                                retrieve
  //                                                                process ID
  //                                                                from
  //                                                                minidump
  //                                                                file,
  //                                                                setting
  //                                                                process ID "
  //                                                                "to 1.\n");
  //    pid = 1;
  //  }
  //  SetID(pid.getValue());

  return error;
}

ConstString ScriptableProcess::GetPluginName() { return GetPluginNameStatic(); }

uint32_t ScriptableProcess::GetPluginVersion() { return 1; }

Status ScriptableProcess::DoDestroy() { return Status(); }

void ScriptableProcess::RefreshStateAfterStop() {
  //
  //  if (!m_active_exception)
  //    return;
  //
  //  constexpr uint32_t BreakpadDumpRequested = 0xFFFFFFFF;
  //  if (m_active_exception->ExceptionRecord.ExceptionCode ==
  //      BreakpadDumpRequested) {
  //      // This "ExceptionCode" value is a sentinel that is sometimes used
  //      // when generating a dump for a process that hasn't crashed.
  //
  //      // TODO: The definition and use of this "dump requested" constant
  //      // in Breakpad are actually Linux-specific, and for similar use
  //      // cases on Mac/Windows it defines different constants, referring
  //      // to them as "simulated" exceptions; consider moving this check
  //      // down to the OS-specific paths and checking each OS for its own
  //      // constant.
  //    return;
  //  }
  //
  //  lldb::StopInfoSP stop_info;
  //  lldb::ThreadSP stop_thread;
  //
  //  Process::m_thread_list.SetSelectedThreadByID(m_active_exception->ThreadId);
  //  stop_thread = Process::m_thread_list.GetSelectedThread();
  //  ArchSpec arch = GetArchitecture();
  //
  //  if (arch.GetTriple().getOS() == llvm::Triple::Linux) {
  //    uint32_t signo = m_active_exception->ExceptionRecord.ExceptionCode;
  //
  //    if (signo == 0) {
  //        // No stop.
  //      return;
  //    }
  //
  //    stop_info = StopInfo::CreateStopReasonWithSignal(
  //                                                     *stop_thread, signo);
  //  } else if (arch.GetTriple().getVendor() == llvm::Triple::Apple) {
  //    stop_info = StopInfoMachException::CreateStopReasonWithMachException(
  //                                                                         *stop_thread,
  //                                                                         m_active_exception->ExceptionRecord.ExceptionCode,
  //                                                                         2,
  //                                                                         m_active_exception->ExceptionRecord.ExceptionFlags,
  //                                                                         m_active_exception->ExceptionRecord.ExceptionAddress,
  //                                                                         0);
  //  } else {
  //    std::string desc;
  //    llvm::raw_string_ostream desc_stream(desc);
  //    desc_stream << "Exception "
  //    << llvm::format_hex(
  //                        m_active_exception->ExceptionRecord.ExceptionCode,
  //                        8)
  //    << " encountered at address "
  //    << llvm::format_hex(
  //                        m_active_exception->ExceptionRecord.ExceptionAddress,
  //                        8);
  //    stop_info = StopInfo::CreateStopReasonWithException(
  //                                                        *stop_thread,
  //                                                        desc_stream.str().c_str());
  //  }
  //
  //  stop_thread->SetStopInfo(stop_info);
}

bool ScriptableProcess::IsAlive() { return true; }

bool ScriptableProcess::WarnBeforeDetach() const { return false; }

size_t ScriptableProcess::ReadMemory(lldb::addr_t addr, void *buf, size_t size,
                                     Status &error) {
  // Don't allow the caching that lldb_private::Process::ReadMemory does since
  // we have it all cached in our dump file anyway.
  return DoReadMemory(addr, buf, size, error);
}

size_t ScriptableProcess::DoReadMemory(lldb::addr_t addr, void *buf,
                                       size_t size, Status &error) {

  //  llvm::ArrayRef<uint8_t> mem = m_minidump_parser->GetMemory(addr, size);
  //  if (mem.empty()) {
  //    error.SetErrorString("could not parse memory info");
  return 0;
  //  }
  //
  //  std::memcpy(buf, mem.data(), mem.size());
  //  return mem.size();
}

ArchSpec ScriptableProcess::GetArchitecture() {
  //  if (!m_is_wow64) {
  //    return m_minidump_parser->GetArchitecture();
  //  }

  llvm::Triple triple;
  //  triple.setVendor(llvm::Triple::VendorType::UnknownVendor);
  //  triple.setArch(llvm::Triple::ArchType::x86);
  //  triple.setOS(llvm::Triple::OSType::Win32);
  return ArchSpec(triple);
}

Status ScriptableProcess::GetMemoryRegionInfo(lldb::addr_t load_addr,
                                              MemoryRegionInfo &region) {
  //  BuildMemoryRegions();
  //  region = MinidumpParser::GetMemoryRegionInfo(*m_memory_regions,
  //  load_addr);
  return Status();
}

Status ScriptableProcess::GetMemoryRegions(MemoryRegionInfos &region_list) {
  //  BuildMemoryRegions();
  //  region_list = *m_memory_regions;
  return Status();
}

void ScriptableProcess::Clear() { Process::m_thread_list.Clear(); }

bool ScriptableProcess::UpdateThreadList(ThreadList &old_thread_list,
                                         ThreadList &new_thread_list) {
  //  for (const minidump::Thread &thread : m_thread_list) {
  //    LocationDescriptor context_location = thread.Context;
  //
  //      // If the minidump contains an exception context, use it
  //    if (m_active_exception != nullptr &&
  //        m_active_exception->ThreadId == thread.ThreadId) {
  //      context_location = m_active_exception->ThreadContext;
  //    }
  //
  //    llvm::ArrayRef<uint8_t> context;
  //    if (!m_is_wow64)
  //      context = m_minidump_parser->GetThreadContext(context_location);
  //    else
  //      context = m_minidump_parser->GetThreadContextWow64(thread);
  //
  //    lldb::ThreadSP thread_sp(new ThreadMinidump(*this, thread, context));
  //    new_thread_list.AddThread(thread_sp);
  //  }
  return new_thread_list.GetSize(false) > 0;
}

bool ScriptableProcess::GetProcessInfo(ProcessInstanceInfo &info) {
  info.Clear();
  info.SetProcessID(GetID());
  info.SetArchitecture(GetArchitecture());
  lldb::ModuleSP module_sp = GetTarget().GetExecutableModule();
  if (module_sp) {
    const bool add_exe_file_as_first_arg = false;
    info.SetExecutableFile(GetTarget().GetExecutableModule()->GetFileSpec(),
                           add_exe_file_as_first_arg);
  }
  return true;
}

// For minidumps there's no runtime generated code so we don't need JITLoader(s)
// Avoiding them will also speed up minidump loading since JITLoaders normally
// try to set up symbolic breakpoints, which in turn may force loading more
// debug information than needed.
JITLoaderList &ScriptableProcess::GetJITLoaders() {
  if (!m_jit_loaders_up) {
    m_jit_loaders_up = std::make_unique<JITLoaderList>();
  }
  return *m_jit_loaders_up;
}

#pragma mark CommandObjectProcessPluginScriptableLoad

#define LLDB_OPTIONS_process_plugin_scriptable_load
#include "ScriptableProcessOptions.inc"

class CommandObjectProcessPluginScriptableLoad : public CommandObjectParsed {
private:
  class CommandOptions : public Options {
  public:
    CommandOptions() : Options() {}
    ~CommandOptions() override = default;

    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override {
      Status error;
      const int short_option = m_getopt_table[option_idx].val;

      switch (short_option) {
      case 'l':
        m_class_name = std::string(option_arg);
        break;
      case 's':
        m_module = std::string(option_arg);
        break;
      default:
        llvm_unreachable("Unimplemented option");
      }

      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      m_module = "";
      m_class_name = "";
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
      return llvm::makeArrayRef(g_process_plugin_scriptable_load_options);
    }

    std::string m_class_name;
    std::string m_module;
  };

  CommandOptions m_options;

  Options *GetOptions() override { return &m_options; }

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override;

public:
  CommandObjectProcessPluginScriptableLoad(CommandInterpreter &interpreter)
      : CommandObjectParsed(interpreter, "process plugin scriptable load",
                            "Load a scriptable process.", nullptr),
        m_options() {
    //    FIXME: Update Long Help
    //    SetHelpLong(R"(
    //                Frame recognizers allow for retrieving information about
    //                special frames based on ABI, arguments or other special
    //                properties of that frame, even without source code or
    //                debug info. Currently, one use case is to extract function
    //                arguments that would otherwise be unaccesible, or augment
    //                existing arguments.
    //
    //                Adding a custom frame recognizer is possible by
    //                implementing a Python class and using the 'frame
    //                recognizer add' command. The Python class should have a
    //                'get_recognized_arguments' method and it will receive an
    //                argument of type lldb.SBFrame representing the current
    //                frame that we are trying to recognize. The method should
    //                return a (possibly empty) list of lldb.SBValue objects
    //                that represent the recognized arguments.
    //
    //                An example of a recognizer that retrieves the file
    //                descriptor values from libc functions 'read', 'write' and
    //                'close' follows:
    //
    //                class LibcFdRecognizer(object):
    //                def get_recognized_arguments(self, frame):
    //                if frame.name in ["read", "write", "close"]:
    //                fd = frame.EvaluateExpression("$arg1").unsigned
    //                value = lldb.target.CreateValueFromExpression("fd",
    //                "(int)%d" % fd) return [value] return []
    //
    //                The file containing this implementation can be imported
    //                via 'command script import' and then we can register this
    //                recognizer with 'frame recognizer add'. It's important to
    //                restrict the recognizer to the libc library (which is
    //                libsystem_kernel.dylib on macOS) to avoid matching
    //                functions with the same name in other modules:
    //
    //                (lldb) command script import .../fd_recognizer.py
    //                (lldb) frame recognizer add -l
    //                fd_recognizer.LibcFdRecognizer -n read -s
    //                libsystem_kernel.dylib
    //
    //                When the program is stopped at the beginning of the 'read'
    //                function in libc, we can view the recognizer arguments in
    //                'frame variable':
    //
    //                (lldb) b read
    //                (lldb) r
    //                Process 1234 stopped
    //                * thread #1, queue = 'com.apple.main-thread', stop reason
    //                = breakpoint 1.3 frame #0: 0x00007fff06013ca0
    //                libsystem_kernel.dylib`read (lldb) frame variable (int) fd
    //                = 3
    //
    //                )");
  }
  ~CommandObjectProcessPluginScriptableLoad() override = default;
};

bool CommandObjectProcessPluginScriptableLoad::DoExecute(
    Args &command, CommandReturnObject &result) {
#if LLDB_ENABLE_PYTHON
  if (m_options.m_class_name.empty()) {
    result.AppendErrorWithFormat(
        "%s needs a Python class name (-l argument).\n", m_cmd_name.c_str());
    result.SetStatus(eReturnStatusFailed);
    return false;
  }

  if (m_options.m_module.empty()) {
    result.AppendErrorWithFormat("%s needs a module name (-s argument).\n",
                                 m_cmd_name.c_str());
    result.SetStatus(eReturnStatusFailed);
    return false;
  }

  ScriptInterpreter *interpreter = GetDebugger().GetScriptInterpreter();

  if (interpreter &&
      !interpreter->CheckObjectExists(m_options.m_class_name.c_str())) {
    result.AppendWarning("The provided class does not exist - please define it "
                         "before attempting to use this frame recognizer");
  }

//  FIXME: Update
//  StackFrameRecognizerSP recognizer_sp =
//  StackFrameRecognizerSP(new ScriptedStackFrameRecognizer(
//                                                          interpreter,
//                                                          m_options.m_class_name.c_str()));
//  auto module = ConstString(m_options.m_module);
//  std::vector<ConstString> symbols(m_options.m_symbols.begin(),
//                                   m_options.m_symbols.end());
//  GetSelectedOrDummyTarget().GetFrameRecognizerManager().AddRecognizer(
//                                                                       recognizer_sp,
//                                                                       module,
//                                                                       symbols);
#endif

  //  MinidumpParser &minidump = *process->m_minidump_parser;

  result.SetStatus(eReturnStatusSuccessFinishNoResult);
  return result.Succeeded();
}

CommandObjectMultiwordScriptableProcess::
    CommandObjectMultiwordScriptableProcess(CommandInterpreter &interpreter)
    : CommandObjectMultiword(
          interpreter, "process plugin scriptable",
          "Commands for operating on a Scriptable processes.",
          "process plugin <subcommand> [<subcommand-options>]") {
  LoadSubCommand(
      "load", CommandObjectSP(
                  new CommandObjectProcessPluginScriptableLoad(interpreter)));
}
