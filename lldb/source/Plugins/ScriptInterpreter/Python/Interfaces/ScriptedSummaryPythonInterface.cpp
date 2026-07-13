//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../lldb-python.h"

#include "lldb/Core/PluginManager.h"
#include "lldb/DataFormatters/TypeSummary.h"
#include "lldb/ValueObject/ValueObject.h"
#include "lldb/lldb-enumerations.h"

#include "../SWIGPythonBridge.h"
#include "../ScriptInterpreterPythonImpl.h"
#include "ScriptedSummaryPythonInterface.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::python;
using Locker = ScriptInterpreterPythonImpl::Locker;

ScriptedSummaryPythonInterface::ScriptedSummaryPythonInterface(
    ScriptInterpreterPythonImpl &interpreter)
    : ScriptedSummaryInterface(), ScriptedPythonInterface(interpreter) {}

llvm::Expected<StructuredData::GenericSP>
ScriptedSummaryPythonInterface::CreatePluginObject(
    llvm::StringRef class_name) {
  if (class_name.empty())
    return llvm::createStringError("empty class name");

  return ScriptedPythonInterface::CreatePluginObject(
      ScriptedMetadata(class_name, nullptr), nullptr);
}

std::optional<std::string> ScriptedSummaryPythonInterface::GetSummary(
    ValueObject &valobj, const TypeSummaryOptions &options) {
  if (!m_object_instance_sp)
    return std::nullopt;

  Locker py_lock(&m_interpreter,
                Locker::AcquireLock | Locker::InitSession | Locker::NoSTDIN,
                Locker::FreeLock);

  PythonObject implementor(PyRefType::Borrowed,
                           (PyObject *)m_object_instance_sp->GetValue());
  if (!implementor.IsAllocated())
    return std::nullopt;

  llvm::Expected<PythonObject> expected_py_return = implementor.CallMethod(
      "get_summary", SWIGBridge::ToSWIGWrapper(valobj.GetSP()),
      SWIGBridge::ToSWIGWrapper(options));

  if (!expected_py_return) {
    llvm::consumeError(expected_py_return.takeError());
    return std::nullopt;
  }

  PythonObject py_return = std::move(expected_py_return.get());
  if (!py_return.IsAllocated())
    return std::nullopt;

  // Match the legacy function-based summary contract
  // (LLDBSwigPythonCallTypeScript): the return value is converted via
  // Python's str() regardless of its type, so `get_summary` may return any
  // str()-convertible object, not just a literal string.
  return py_return.Str().GetString().str();
}

void ScriptedSummaryPythonInterface::Initialize() {
  PluginManager::RegisterPlugin(
      GetPluginNameStatic(),
      "Provide a summary string for a type, used by 'type summary add -l'",
      CreateInstance, eScriptedExtensionScriptedSummary, eScriptLanguagePython,
      {});
}

void ScriptedSummaryPythonInterface::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}
