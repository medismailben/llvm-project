//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../lldb-python.h"

#include "lldb/Core/PluginManager.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Utility/Args.h"
#include "lldb/lldb-enumerations.h"

#include "../SWIGPythonBridge.h"
#include "../ScriptInterpreterPythonImpl.h"
#include "ScriptedCommandPythonInterface.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::python;
using Locker = ScriptInterpreterPythonImpl::Locker;

ScriptedCommandPythonInterface::ScriptedCommandPythonInterface(
    ScriptInterpreterPythonImpl &interpreter)
    : ScriptedCommandInterface(), ScriptedPythonInterface(interpreter) {}

llvm::Expected<StructuredData::GenericSP>
ScriptedCommandPythonInterface::CreatePluginObject(llvm::StringRef class_name,
                                                   lldb::DebuggerSP debugger_sp) {
  if (class_name.empty())
    return llvm::createStringError("empty class name");

  if (!debugger_sp)
    return llvm::createStringError("invalid Debugger pointer");

  Locker py_lock(&m_interpreter,
                 Locker::AcquireLock | Locker::InitSession | Locker::NoSTDIN,
                 Locker::FreeLock);

  std::string class_name_str = class_name.str();
  PythonObject result = SWIGBridge::LLDBSwigPythonCreateCommandObject(
      class_name_str.c_str(), m_interpreter.GetDictionaryName(), debugger_sp);

  if (!result.IsValid())
    return llvm::createStringError(
        llvm::formatv("Could not create command object: {0}", class_name)
            .str());

  m_debugger_sp = debugger_sp;
  m_object_instance_sp =
      StructuredData::GenericSP(new StructuredPythonObject(std::move(result)));
  return m_object_instance_sp;
}

bool ScriptedCommandPythonInterface::RunRawCommand(
    llvm::StringRef args, ScriptedCommandSynchronicity synchronicity,
    CommandReturnObject &cmd_retobj, Status &error,
    const ExecutionContext &exe_ctx) {
  if (!m_object_instance_sp || !m_object_instance_sp->IsValid()) {
    error = Status::FromErrorString("no function to execute");
    return false;
  }

  lldb::DebuggerSP debugger_sp = m_debugger_sp;
  lldb::ExecutionContextRefSP exe_ctx_ref_sp(new ExecutionContextRef(exe_ctx));

  if (!debugger_sp) {
    error = Status::FromErrorString("invalid Debugger pointer");
    return false;
  }

  bool ret_val = false;
  {
    Locker py_lock(&m_interpreter,
                   Locker::AcquireLock | Locker::InitSession |
                       (cmd_retobj.GetInteractive() ? 0 : Locker::NoSTDIN),
                   Locker::FreeLock | Locker::TearDownSession);

    ScriptInterpreterPythonImpl::SynchronicityHandler synch_handler(
        debugger_sp, synchronicity);

    std::string args_str = args.str();
    ret_val = SWIGBridge::LLDBSwigPythonCallCommandObject(
        static_cast<PyObject *>(m_object_instance_sp->GetValue()), debugger_sp,
        args_str.c_str(), cmd_retobj, exe_ctx_ref_sp);
  }

  if (!ret_val)
    error = Status::FromErrorString("unable to execute script function");
  else if (cmd_retobj.GetStatus() == eReturnStatusFailed)
    return false;

  error.Clear();
  return ret_val;
}

bool ScriptedCommandPythonInterface::RunParsedCommand(
    Args &args, ScriptedCommandSynchronicity synchronicity,
    CommandReturnObject &cmd_retobj, Status &error,
    const ExecutionContext &exe_ctx) {
  if (!m_object_instance_sp || !m_object_instance_sp->IsValid()) {
    error = Status::FromErrorString("no function to execute");
    return false;
  }

  lldb::DebuggerSP debugger_sp = m_debugger_sp;
  lldb::ExecutionContextRefSP exe_ctx_ref_sp(new ExecutionContextRef(exe_ctx));

  if (!debugger_sp) {
    error = Status::FromErrorString("invalid Debugger pointer");
    return false;
  }

  bool ret_val = false;
  {
    Locker py_lock(&m_interpreter,
                   Locker::AcquireLock | Locker::InitSession |
                       (cmd_retobj.GetInteractive() ? 0 : Locker::NoSTDIN),
                   Locker::FreeLock | Locker::TearDownSession);

    ScriptInterpreterPythonImpl::SynchronicityHandler synch_handler(
        debugger_sp, synchronicity);

    StructuredData::ArraySP args_arr_sp(new StructuredData::Array());
    for (const Args::ArgEntry &entry : args)
      args_arr_sp->AddStringItem(entry.ref());
    StructuredDataImpl args_impl(args_arr_sp);

    ret_val = SWIGBridge::LLDBSwigPythonCallParsedCommandObject(
        static_cast<PyObject *>(m_object_instance_sp->GetValue()), debugger_sp,
        args_impl, cmd_retobj, exe_ctx_ref_sp);
  }

  if (!ret_val)
    error = Status::FromErrorString("unable to execute script function");
  else if (cmd_retobj.GetStatus() == eReturnStatusFailed)
    return false;

  error.Clear();
  return ret_val;
}

std::optional<std::string>
ScriptedCommandPythonInterface::GetRepeatCommand(Args &args) {
  if (!m_object_instance_sp || !m_object_instance_sp->IsValid())
    return std::nullopt;

  Locker py_lock(&m_interpreter, Locker::AcquireLock | Locker::NoSTDIN,
                Locker::FreeLock);

  std::string command;
  args.GetQuotedCommandString(command);
  return SWIGBridge::LLDBSwigPythonGetRepeatCommandForScriptedCommand(
      static_cast<PyObject *>(m_object_instance_sp->GetValue()), command);
}

StructuredData::DictionarySP
ScriptedCommandPythonInterface::HandleArgumentCompletion(
    std::vector<llvm::StringRef> &args, size_t args_pos, size_t char_in_arg) {
  if (!m_object_instance_sp || !m_object_instance_sp->IsValid())
    return {};

  Locker py_lock(&m_interpreter, Locker::AcquireLock | Locker::NoSTDIN,
                Locker::FreeLock);

  return SWIGBridge::LLDBSwigPythonHandleArgumentCompletionForScriptedCommand(
      static_cast<PyObject *>(m_object_instance_sp->GetValue()), args,
      args_pos, char_in_arg);
}

StructuredData::DictionarySP
ScriptedCommandPythonInterface::HandleOptionArgumentCompletion(
    llvm::StringRef &long_option, size_t char_in_arg) {
  if (!m_object_instance_sp || !m_object_instance_sp->IsValid())
    return {};

  Locker py_lock(&m_interpreter, Locker::AcquireLock | Locker::NoSTDIN,
                Locker::FreeLock);

  return SWIGBridge::LLDBSwigPythonHandleOptionArgumentCompletionForScriptedCommand(
      static_cast<PyObject *>(m_object_instance_sp->GetValue()), long_option,
      char_in_arg);
}

bool ScriptedCommandPythonInterface::GetShortHelp(std::string &dest) {
  dest.clear();

  if (!m_object_instance_sp)
    return false;

  Locker py_lock(&m_interpreter, Locker::AcquireLock | Locker::NoSTDIN,
                Locker::FreeLock);

  PythonObject implementor(PyRefType::Borrowed,
                           (PyObject *)m_object_instance_sp->GetValue());
  if (!implementor.IsAllocated())
    return false;

  llvm::Expected<PythonObject> expected_py_return =
      implementor.CallMethod("get_short_help");

  if (!expected_py_return) {
    llvm::consumeError(expected_py_return.takeError());
    return false;
  }

  PythonObject py_return = std::move(expected_py_return.get());
  if (py_return.IsAllocated() && PythonString::Check(py_return.get())) {
    PythonString py_string(PyRefType::Borrowed, py_return.get());
    llvm::StringRef return_data(py_string.GetString());
    dest.assign(return_data.data(), return_data.size());
    return true;
  }

  return false;
}

bool ScriptedCommandPythonInterface::GetLongHelp(std::string &dest) {
  dest.clear();

  if (!m_object_instance_sp)
    return false;

  Locker py_lock(&m_interpreter, Locker::AcquireLock | Locker::NoSTDIN,
                Locker::FreeLock);

  PythonObject implementor(PyRefType::Borrowed,
                           (PyObject *)m_object_instance_sp->GetValue());
  if (!implementor.IsAllocated())
    return false;

  llvm::Expected<PythonObject> expected_py_return =
      implementor.CallMethod("get_long_help");

  if (!expected_py_return) {
    llvm::consumeError(expected_py_return.takeError());
    return false;
  }

  PythonObject py_return = std::move(expected_py_return.get());
  if (py_return.IsAllocated() && PythonString::Check(py_return.get())) {
    PythonString py_string(PyRefType::Borrowed, py_return.get());
    llvm::StringRef return_data(py_string.GetString());
    dest.assign(return_data.data(), return_data.size());
    return true;
  }

  return false;
}

uint32_t ScriptedCommandPythonInterface::GetFlags() {
  uint32_t result = 0;

  if (!m_object_instance_sp)
    return result;

  Locker py_lock(&m_interpreter, Locker::AcquireLock | Locker::NoSTDIN,
                Locker::FreeLock);

  static char callee_name[] = "get_flags";

  PythonObject implementor(PyRefType::Borrowed,
                           (PyObject *)m_object_instance_sp->GetValue());
  if (!implementor.IsAllocated())
    return result;

  PythonObject pmeth(PyRefType::Owned,
                     PyObject_GetAttrString(implementor.get(), callee_name));

  if (PyErr_Occurred())
    PyErr_Clear();

  if (!pmeth.IsAllocated())
    return result;

  if (PyCallable_Check(pmeth.get()) == 0) {
    if (PyErr_Occurred())
      PyErr_Clear();
    return result;
  }

  if (PyErr_Occurred())
    PyErr_Clear();

  long long py_return = unwrapOrSetPythonException(
      As<long long>(implementor.CallMethod(callee_name)));

  if (PyErr_Occurred()) {
    PyErr_Print();
    PyErr_Clear();
  } else {
    result = py_return;
  }

  return result;
}

StructuredData::ObjectSP ScriptedCommandPythonInterface::GetOptionsDefinition() {
  StructuredData::ObjectSP result = {};

  if (!m_object_instance_sp)
    return result;

  Locker py_lock(&m_interpreter, Locker::AcquireLock | Locker::NoSTDIN,
                Locker::FreeLock);

  static char callee_name[] = "get_options_definition";

  PythonObject implementor(PyRefType::Borrowed,
                           (PyObject *)m_object_instance_sp->GetValue());
  if (!implementor.IsAllocated())
    return result;

  PythonObject pmeth(PyRefType::Owned,
                     PyObject_GetAttrString(implementor.get(), callee_name));

  if (PyErr_Occurred())
    PyErr_Clear();

  if (!pmeth.IsAllocated())
    return result;

  if (PyCallable_Check(pmeth.get()) == 0) {
    if (PyErr_Occurred())
      PyErr_Clear();
    return result;
  }

  if (PyErr_Occurred())
    PyErr_Clear();

  PythonDictionary py_return = unwrapOrSetPythonException(
      As<PythonDictionary>(implementor.CallMethod(callee_name)));

  if (PyErr_Occurred()) {
    PyErr_Print();
    PyErr_Clear();
    return {};
  }
  return py_return.CreateStructuredObject();
}

StructuredData::ObjectSP
ScriptedCommandPythonInterface::GetArgumentsDefinition() {
  StructuredData::ObjectSP result = {};

  if (!m_object_instance_sp)
    return result;

  Locker py_lock(&m_interpreter, Locker::AcquireLock | Locker::NoSTDIN,
                Locker::FreeLock);

  static char callee_name[] = "get_args_definition";

  PythonObject implementor(PyRefType::Borrowed,
                           (PyObject *)m_object_instance_sp->GetValue());
  if (!implementor.IsAllocated())
    return result;

  PythonObject pmeth(PyRefType::Owned,
                     PyObject_GetAttrString(implementor.get(), callee_name));

  if (PyErr_Occurred())
    PyErr_Clear();

  if (!pmeth.IsAllocated())
    return result;

  if (PyCallable_Check(pmeth.get()) == 0) {
    if (PyErr_Occurred())
      PyErr_Clear();
    return result;
  }

  if (PyErr_Occurred())
    PyErr_Clear();

  PythonList py_return = unwrapOrSetPythonException(
      As<PythonList>(implementor.CallMethod(callee_name)));

  if (PyErr_Occurred()) {
    PyErr_Print();
    PyErr_Clear();
    return {};
  }
  return py_return.CreateStructuredObject();
}

void ScriptedCommandPythonInterface::OptionParsingStarted() {
  if (!m_object_instance_sp)
    return;

  Locker py_lock(&m_interpreter, Locker::AcquireLock | Locker::NoSTDIN,
                Locker::FreeLock);

  static char callee_name[] = "option_parsing_started";

  PythonObject implementor(PyRefType::Borrowed,
                           (PyObject *)m_object_instance_sp->GetValue());
  if (!implementor.IsAllocated())
    return;

  PythonObject pmeth(PyRefType::Owned,
                     PyObject_GetAttrString(implementor.get(), callee_name));

  if (PyErr_Occurred())
    PyErr_Clear();

  if (!pmeth.IsAllocated())
    return;

  if (PyCallable_Check(pmeth.get()) == 0) {
    if (PyErr_Occurred())
      PyErr_Clear();
    return;
  }

  if (PyErr_Occurred())
    PyErr_Clear();

  // option_parsing_started doesn't return anything, ignore anything but
  // python errors.
  unwrapOrSetPythonException(As<bool>(implementor.CallMethod(callee_name)));

  if (PyErr_Occurred()) {
    PyErr_Print();
    PyErr_Clear();
  }
}

bool ScriptedCommandPythonInterface::SetOptionValue(ExecutionContext *exe_ctx,
                                                    llvm::StringRef long_option,
                                                    llvm::StringRef value) {
  if (!m_object_instance_sp)
    return false;

  Locker py_lock(&m_interpreter, Locker::AcquireLock | Locker::NoSTDIN,
                Locker::FreeLock);

  static char callee_name[] = "set_option_value";

  PythonObject implementor(PyRefType::Borrowed,
                           (PyObject *)m_object_instance_sp->GetValue());
  if (!implementor.IsAllocated())
    return false;

  PythonObject pmeth(PyRefType::Owned,
                     PyObject_GetAttrString(implementor.get(), callee_name));

  if (PyErr_Occurred())
    PyErr_Clear();

  if (!pmeth.IsAllocated())
    return false;

  if (PyCallable_Check(pmeth.get()) == 0) {
    if (PyErr_Occurred())
      PyErr_Clear();
    return false;
  }

  if (PyErr_Occurred())
    PyErr_Clear();

  lldb::ExecutionContextRefSP exe_ctx_ref_sp;
  if (exe_ctx)
    exe_ctx_ref_sp = std::make_shared<ExecutionContextRef>(exe_ctx);
  PythonObject ctx_ref_obj = SWIGBridge::ToSWIGWrapper(exe_ctx_ref_sp);

  bool py_return = unwrapOrSetPythonException(As<bool>(
      implementor.CallMethod(callee_name, ctx_ref_obj,
                             long_option.str().c_str(), value.str().c_str())));

  if (PyErr_Occurred()) {
    PyErr_Print();
    PyErr_Clear();
    return false;
  }
  return py_return;
}

void ScriptedCommandPythonInterface::Initialize() {
  PluginManager::RegisterPlugin(
      GetPluginNameStatic(),
      "Implement a custom command, used by 'command script add -c'/'-P'",
      CreateInstance, eScriptedExtensionScriptedCommand, eScriptLanguagePython,
      {});
}

void ScriptedCommandPythonInterface::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}
