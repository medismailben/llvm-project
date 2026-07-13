//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../lldb-python.h"

#include "lldb/Core/PluginManager.h"
#include "lldb/ValueObject/ValueObject.h"
#include "lldb/lldb-enumerations.h"

#include "../SWIGPythonBridge.h"
#include "../ScriptInterpreterPythonImpl.h"
#include "ScriptedSyntheticChildrenPythonInterface.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::python;
using Locker = ScriptInterpreterPythonImpl::Locker;

ScriptedSyntheticChildrenPythonInterface::
    ScriptedSyntheticChildrenPythonInterface(
        ScriptInterpreterPythonImpl &interpreter)
    : ScriptedSyntheticChildrenInterface(), ScriptedPythonInterface(interpreter) {}

llvm::Expected<StructuredData::GenericSP>
ScriptedSyntheticChildrenPythonInterface::CreatePluginObject(
    llvm::StringRef class_name, ValueObject &backend) {
  if (class_name.empty())
    return llvm::createStringError("empty class name");

  ValueObjectSP valobj_sp = backend.GetSP();
  if (!valobj_sp)
    return llvm::createStringError("invalid backing value");

  Locker py_lock(&m_interpreter,
                 Locker::AcquireLock | Locker::InitSession | Locker::NoSTDIN,
                 Locker::FreeLock);

  // LLDBSwigPythonCreateSyntheticProvider (rather than the generic
  // CreatePluginObject/Transform machinery) is used deliberately: it wraps
  // the backing value with SetPreferSyntheticValue(false) before handing it
  // to __init__, which the generic Transform(ValueObject&) path doesn't do,
  // and which is required to avoid the provider recursively re-entering its
  // own synthetic children when introspecting the backing value.
  std::string class_name_str = class_name.str();
  PythonObject result = SWIGBridge::LLDBSwigPythonCreateSyntheticProvider(
      class_name_str.c_str(), m_interpreter.GetDictionaryName(), valobj_sp);

  if (!result.IsAllocated())
    return llvm::createStringError(
        llvm::formatv("Could not create synthetic provider: {0}", class_name)
            .str());

  m_object_instance_sp =
      StructuredData::GenericSP(new StructuredPythonObject(std::move(result)));
  return m_object_instance_sp;
}

llvm::Expected<uint32_t>
ScriptedSyntheticChildrenPythonInterface::CalculateNumChildren(uint32_t max) {
  if (!m_object_instance_sp)
    return 0;

  Locker py_lock(&m_interpreter,
                 Locker::AcquireLock | Locker::InitSession | Locker::NoSTDIN,
                 Locker::FreeLock);

  PythonObject implementor(PyRefType::Borrowed,
                           (PyObject *)m_object_instance_sp->GetValue());
  if (!implementor.IsAllocated())
    return 0;

  // num_children's arity is introspected at runtime: some providers define
  // num_children(self), others num_children(self, max_count). This can't go
  // through the generic Dispatch<T>() machinery, which always passes a
  // fixed argument list.
  return SWIGBridge::LLDBSwigPython_CalculateNumChildren(implementor.get(),
                                                         max);
}

lldb::ValueObjectSP
ScriptedSyntheticChildrenPythonInterface::GetChildAtIndex(uint32_t idx) {
  if (!m_object_instance_sp)
    return lldb::ValueObjectSP();

  Locker py_lock(&m_interpreter,
                 Locker::AcquireLock | Locker::InitSession | Locker::NoSTDIN,
                 Locker::FreeLock);

  PythonObject implementor(PyRefType::Borrowed,
                           (PyObject *)m_object_instance_sp->GetValue());
  if (!implementor.IsAllocated())
    return lldb::ValueObjectSP();

  PyObject *child_ptr =
      SWIGBridge::LLDBSwigPython_GetChildAtIndex(implementor.get(), idx);
  if (child_ptr == nullptr || child_ptr == Py_None) {
    Py_XDECREF(child_ptr);
    return lldb::ValueObjectSP();
  }

  lldb::SBValue *sb_value_ptr =
      (lldb::SBValue *)LLDBSWIGPython_CastPyObjectToSBValue(child_ptr);
  if (sb_value_ptr == nullptr) {
    Py_XDECREF(child_ptr);
    return lldb::ValueObjectSP();
  }

  return SWIGBridge::LLDBSWIGPython_GetValueObjectSPFromSBValue(sb_value_ptr);
}

llvm::Expected<uint32_t>
ScriptedSyntheticChildrenPythonInterface::GetIndexOfChildWithName(
    ConstString name) {
  if (!m_object_instance_sp)
    return llvm::createStringErrorV("type has no child named '{0}'", name);

  Locker py_lock(&m_interpreter,
                 Locker::AcquireLock | Locker::InitSession | Locker::NoSTDIN,
                 Locker::FreeLock);

  PythonObject implementor(PyRefType::Borrowed,
                           (PyObject *)m_object_instance_sp->GetValue());
  if (!implementor.IsAllocated())
    return llvm::createStringErrorV("type has no child named '{0}'", name);

  uint32_t ret_val = SWIGBridge::LLDBSwigPython_GetIndexOfChildWithName(
      implementor.get(), name.GetCString());

  if (ret_val == UINT32_MAX)
    return llvm::createStringErrorV("type has no child named '{0}'", name);
  return ret_val;
}

lldb::ChildCacheState ScriptedSyntheticChildrenPythonInterface::Update() {
  if (!m_object_instance_sp)
    return lldb::eRefetch;

  Locker py_lock(&m_interpreter,
                 Locker::AcquireLock | Locker::InitSession | Locker::NoSTDIN,
                 Locker::FreeLock);

  PythonObject implementor(PyRefType::Borrowed,
                           (PyObject *)m_object_instance_sp->GetValue());
  if (!implementor.IsAllocated())
    return lldb::eRefetch;

  // update() is optional; a missing method means "always refetch", matching
  // LLDBSwigPython_UpdateSynthProviderInstance's behavior.
  return SWIGBridge::LLDBSwigPython_UpdateSynthProviderInstance(
             implementor.get())
             ? lldb::eReuse
             : lldb::eRefetch;
}

bool ScriptedSyntheticChildrenPythonInterface::MightHaveChildren() {
  if (!m_object_instance_sp)
    return true;

  Locker py_lock(&m_interpreter,
                 Locker::AcquireLock | Locker::InitSession | Locker::NoSTDIN,
                 Locker::FreeLock);

  PythonObject implementor(PyRefType::Borrowed,
                           (PyObject *)m_object_instance_sp->GetValue());
  if (!implementor.IsAllocated())
    return true;

  // has_children() is optional and defaults to True when missing, matching
  // LLDBSwigPython_MightHaveChildrenSynthProviderInstance's behavior.
  return SWIGBridge::LLDBSwigPython_MightHaveChildrenSynthProviderInstance(
      implementor.get());
}

lldb::ValueObjectSP
ScriptedSyntheticChildrenPythonInterface::GetSyntheticValue() {
  if (!m_object_instance_sp)
    return nullptr;

  Locker py_lock(&m_interpreter,
                 Locker::AcquireLock | Locker::InitSession | Locker::NoSTDIN,
                 Locker::FreeLock);

  PythonObject implementor(PyRefType::Borrowed,
                           (PyObject *)m_object_instance_sp->GetValue());
  if (!implementor.IsAllocated())
    return nullptr;

  PyObject *child_ptr = SWIGBridge::LLDBSwigPython_GetValueSynthProviderInstance(
      implementor.get());
  if (child_ptr == nullptr || child_ptr == Py_None) {
    Py_XDECREF(child_ptr);
    return nullptr;
  }

  lldb::SBValue *sb_value_ptr =
      (lldb::SBValue *)LLDBSWIGPython_CastPyObjectToSBValue(child_ptr);
  if (sb_value_ptr == nullptr) {
    Py_XDECREF(child_ptr);
    return nullptr;
  }

  return SWIGBridge::LLDBSWIGPython_GetValueObjectSPFromSBValue(sb_value_ptr);
}

ConstString ScriptedSyntheticChildrenPythonInterface::GetSyntheticTypeName() {
  if (!m_object_instance_sp)
    return {};

  Locker py_lock(&m_interpreter,
                 Locker::AcquireLock | Locker::InitSession | Locker::NoSTDIN,
                 Locker::FreeLock);

  PythonObject implementor(PyRefType::Borrowed,
                           (PyObject *)m_object_instance_sp->GetValue());
  if (!implementor.IsAllocated())
    return {};

  llvm::Expected<PythonObject> expected_py_return =
      implementor.CallMethod("get_type_name");

  if (!expected_py_return) {
    llvm::consumeError(expected_py_return.takeError());
    return {};
  }

  PythonObject py_return = std::move(expected_py_return.get());
  if (!py_return.IsAllocated() || !PythonString::Check(py_return.get()))
    return {};

  PythonString type_name(PyRefType::Borrowed, py_return.get());
  return ConstString(type_name.GetString());
}

void ScriptedSyntheticChildrenPythonInterface::Initialize() {
  PluginManager::RegisterPlugin(
      GetPluginNameStatic(),
      "Provide synthetic children for a type, used by 'type synthetic add -l'",
      CreateInstance, eScriptedExtensionScriptedSyntheticChildren,
      eScriptLanguagePython, {});
}

void ScriptedSyntheticChildrenPythonInterface::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}
