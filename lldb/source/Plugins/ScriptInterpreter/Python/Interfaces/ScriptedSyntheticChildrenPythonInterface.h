//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_SCRIPTINTERPRETER_PYTHON_INTERFACES_SCRIPTEDSYNTHETICCHILDRENPYTHONINTERFACE_H
#define LLDB_SOURCE_PLUGINS_SCRIPTINTERPRETER_PYTHON_INTERFACES_SCRIPTEDSYNTHETICCHILDRENPYTHONINTERFACE_H

#include "lldb/Interpreter/Interfaces/ScriptedSyntheticChildrenInterface.h"

#include "ScriptedPythonInterface.h"
namespace lldb_private {

class ScriptedSyntheticChildrenPythonInterface
    : public ScriptedSyntheticChildrenInterface,
      public ScriptedPythonInterface,
      public PluginInterface {
public:
  ScriptedSyntheticChildrenPythonInterface(
      ScriptInterpreterPythonImpl &interpreter);

  llvm::Expected<StructuredData::GenericSP>
  CreatePluginObject(llvm::StringRef class_name,
                     ValueObject &backend) override;

  llvm::SmallVector<AbstractMethodRequirement>
  GetAbstractMethodRequirements() const override {
    return llvm::SmallVector<AbstractMethodRequirement>(
        {{"get_child_at_index", 2}});
  }

  llvm::Expected<uint32_t> CalculateNumChildren(uint32_t max) override;

  lldb::ValueObjectSP GetChildAtIndex(uint32_t idx) override;

  llvm::Expected<uint32_t> GetIndexOfChildWithName(ConstString name) override;

  lldb::ChildCacheState Update() override;

  bool MightHaveChildren() override;

  lldb::ValueObjectSP GetSyntheticValue() override;

  ConstString GetSyntheticTypeName() override;

  static void Initialize();

  static void Terminate();

  static llvm::StringRef GetPluginNameStatic() {
    return "ScriptedSyntheticChildrenPythonInterface";
  }

  llvm::StringRef GetPluginName() override { return GetPluginNameStatic(); }
};
} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_SCRIPTINTERPRETER_PYTHON_INTERFACES_SCRIPTEDSYNTHETICCHILDRENPYTHONINTERFACE_H
