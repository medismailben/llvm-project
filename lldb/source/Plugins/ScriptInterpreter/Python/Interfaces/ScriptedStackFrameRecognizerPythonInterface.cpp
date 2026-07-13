//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../lldb-python.h"

#include "lldb/Core/PluginManager.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/lldb-enumerations.h"

#include "../SWIGPythonBridge.h"
#include "../ScriptInterpreterPythonImpl.h"
#include "ScriptedStackFrameRecognizerPythonInterface.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::python;

ScriptedStackFrameRecognizerPythonInterface::
    ScriptedStackFrameRecognizerPythonInterface(
        ScriptInterpreterPythonImpl &interpreter)
    : ScriptedStackFrameRecognizerInterface(), ScriptedPythonInterface(interpreter) {}

llvm::Expected<StructuredData::GenericSP>
ScriptedStackFrameRecognizerPythonInterface::CreatePluginObject(
    const ScriptedMetadata &scripted_metadata) {
  return ScriptedPythonInterface::CreatePluginObject(scripted_metadata,
                                                     nullptr);
}

lldb::ValueObjectListSP
ScriptedStackFrameRecognizerPythonInterface::GetRecognizedArguments(
    lldb::StackFrameSP frame_sp) {
  using Locker = ScriptInterpreterPythonImpl::Locker;
  Locker py_lock(&m_interpreter, Locker::AcquireLock | Locker::NoSTDIN,
                Locker::FreeLock);

  if (!m_object_instance_sp)
    return ValueObjectListSP();

  PythonObject implementor(PyRefType::Borrowed,
                           (PyObject *)m_object_instance_sp->GetValue());

  if (!implementor.IsAllocated())
    return ValueObjectListSP();

  // The documented Python contract for get_recognized_arguments returns a
  // plain list of lldb.SBValue (not an lldb.SBValueList like most other
  // ValueObjectListSP-returning extension methods), so this can't go
  // through the generic Dispatch<ValueObjectListSP>() extractor, which
  // expects an SBValueList.
  PythonObject py_return(PyRefType::Owned,
                         SWIGBridge::LLDBSwigPython_GetRecognizedArguments(
                             implementor.get(), frame_sp));

  // if it fails, print the error but otherwise go on
  if (PyErr_Occurred()) {
    PyErr_Print();
    PyErr_Clear();
  }
  if (py_return.get()) {
    PythonList result_list(PyRefType::Borrowed, py_return.get());
    ValueObjectListSP result = std::make_shared<ValueObjectList>();
    for (size_t i = 0; i < result_list.GetSize(); i++) {
      PyObject *item = result_list.GetItemAtIndex(i).get();
      lldb::SBValue *sb_value_ptr =
          (lldb::SBValue *)LLDBSWIGPython_CastPyObjectToSBValue(item);
      auto valobj_sp =
          SWIGBridge::LLDBSWIGPython_GetValueObjectSPFromSBValue(sb_value_ptr);
      if (valobj_sp)
        result->Append(valobj_sp);
    }
    return result;
  }
  return ValueObjectListSP();
}

bool ScriptedStackFrameRecognizerPythonInterface::ShouldHide(
    lldb::StackFrameSP frame_sp) {
  Status error;
  StructuredData::ObjectSP obj = Dispatch("should_hide", error, frame_sp);

  // should_hide is optional on the Python side; a missing/failed method
  // call is not an error, it just means "don't hide" (matches the default
  // in the interface and in the ABC template).
  if (!ScriptedInterface::CheckStructuredDataObject(LLVM_PRETTY_FUNCTION, obj,
                                                    error))
    return false;

  return obj->GetBooleanValue();
}

void ScriptedStackFrameRecognizerPythonInterface::Initialize() {
  PluginManager::RegisterPlugin(
      GetPluginNameStatic(),
      "Recognize a stack frame and provide extra information about it "
      "(e.g. recognized arguments), used by 'frame recognizer add -l'",
      CreateInstance, eScriptedExtensionScriptedStackFrameRecognizer,
      eScriptLanguagePython, {});
}

void ScriptedStackFrameRecognizerPythonInterface::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}
