//===-- ScriptedFrame.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ScriptedFrame.h"

using namespace lldb;
using namespace lldb_private;

void ScriptedFrame::CheckInterpreterAndScriptObject() const {
  lldbassert(m_script_object_sp && "Invalid Script Object.");
  lldbassert(GetInterface() && "Invalid Scripted Frame Interface.");
}

llvm::Expected<std::shared_ptr<ScriptedFrame>>
ScriptedFrame::Create(ScriptedThread &thread,
                      StructuredData::DictionarySP args_sp,
                      StructuredData::Generic *script_object) {
  if (!thread.IsValid())
    return llvm::createStringError("Invalid scripted thread.");

  thread.CheckInterpreterAndScriptObject();

  auto scripted_frame_interface =
      thread.GetInterface()->CreateScriptedFrameInterface();
  if (!scripted_frame_interface)
    return llvm::createStringError(
        "Failed to create scripted frame interface.");

  llvm::StringRef frame_class_name;
  if (!script_object) {
    std::optional<std::string> class_name =
        thread.GetInterface()->GetScriptedFramePluginName();
    if (!class_name || class_name->empty())
      return llvm::createStringError(
          "Failed to get scripted thread class name.");
    frame_class_name = *class_name;
  }

  ExecutionContext exe_ctx(thread);
  auto obj_or_err = scripted_frame_interface->CreatePluginObject(
      frame_class_name, exe_ctx, args_sp, script_object);

  if (!obj_or_err) {
    llvm::consumeError(obj_or_err.takeError());
    return llvm::createStringError("Failed to create script object.");
  }

  StructuredData::GenericSP owned_script_object_sp = *obj_or_err;

  if (!owned_script_object_sp->IsValid())
    return llvm::createStringError("Created script object is invalid.");

  lldb::user_id_t frame_id = scripted_frame_interface->GetID();

  return std::make_shared<ScriptedFrame>(thread, scripted_frame_interface,
                                         frame_id, owned_script_object_sp);
}

ScriptedFrame::ScriptedFrame(ScriptedThread &thread,
                             ScriptedFrameInterfaceSP interface_sp,
                             lldb::user_id_t id,
                             StructuredData::GenericSP script_object_sp)
    : StackFrame(thread.shared_from_this(), id, id, /*cfa=*/0,
                 /*cfa_is_valid=*/false, /*pc=*/0,
                 /*frame_kind=*/Kind::Artificial,
                 /*behaves_like_zeroth_frame=*/!id, /*symbol_ctx=*/nullptr),
      m_scripted_thread(thread), m_scripted_frame_interface_sp(interface_sp),
      m_script_object_sp(script_object_sp) {}

const char *ScriptedFrame::GetFunctionName() {
  CheckInterpreterAndScriptObject();
  std::optional<std::string> function_name = GetInterface()->GetFunctionName();
  if (!function_name)
    return nullptr;
  return ConstString(function_name->c_str()).AsCString();
}

const char *ScriptedFrame::GetDisplayFunctionName() {
  CheckInterpreterAndScriptObject();
  std::optional<std::string> function_name =
      GetInterface()->GetDisplayFunctionName();
  if (!function_name)
    return nullptr;
  return ConstString(function_name->c_str()).AsCString();
}

bool ScriptedFrame::IsInlined() { return GetInterface()->IsInlined(); }

bool ScriptedFrame::IsArtificial() const {
  return GetInterface()->IsArtificial();
}

bool ScriptedFrame::IsHidden() { return GetInterface()->IsHidden(); }

lldb::ScriptedFrameInterfaceSP ScriptedFrame::GetInterface() const {
  return m_scripted_frame_interface_sp;
}

std::shared_ptr<DynamicRegisterInfo> ScriptedFrame::GetDynamicRegisterInfo() {
  CheckInterpreterAndScriptObject();

  if (!m_register_info_sp) {
    StructuredData::DictionarySP reg_info = GetInterface()->GetRegisterInfo();

    Status error;
    if (!reg_info)
      return ScriptedInterface::ErrorWithMessage<
          std::shared_ptr<DynamicRegisterInfo>>(
          LLVM_PRETTY_FUNCTION, "Failed to get scripted frame registers info.",
          error, LLDBLog::Thread);

    ThreadSP thread_sp = m_thread_wp.lock();
    if (!thread_sp || !thread_sp->IsValid())
      return ScriptedInterface::ErrorWithMessage<
          std::shared_ptr<DynamicRegisterInfo>>(
          LLVM_PRETTY_FUNCTION,
          "Failed to get scripted frame registers info: invalid thread.", error,
          LLDBLog::Thread);

    ProcessSP process_sp = thread_sp->GetProcess();
    if (!process_sp || !process_sp->IsValid())
      return ScriptedInterface::ErrorWithMessage<
          std::shared_ptr<DynamicRegisterInfo>>(
          LLVM_PRETTY_FUNCTION,
          "Failed to get scripted frame registers info: invalid process.",
          error, LLDBLog::Thread);

    m_register_info_sp = DynamicRegisterInfo::Create(
        *reg_info, process_sp->GetTarget().GetArchitecture());
  }

  return m_register_info_sp;
}
