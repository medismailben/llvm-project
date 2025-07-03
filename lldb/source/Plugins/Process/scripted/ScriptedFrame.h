//===-- ScriptedFrame.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_SCRIPTED_FRAME_H
#define LLDB_SOURCE_PLUGINS_SCRIPTED_FRAME_H

#include <string>

#include "ScriptedThread.h"

#include "Plugins/Process/Utility/RegisterContextMemory.h"
#include "lldb/Interpreter/ScriptInterpreter.h"
#include "lldb/Target/DynamicRegisterInfo.h"
#include "lldb/Target/StackFrame.h"

namespace lldb_private {
class ScriptedThread;
}

namespace lldb_private {

class ScriptedFrame : public lldb_private::StackFrame {

public:
  ScriptedFrame(ScriptedThread &thread,
                lldb::ScriptedFrameInterfaceSP interface_sp,
                lldb::user_id_t frame_idx,
                StructuredData::GenericSP script_object_sp = nullptr);

  ~ScriptedFrame() = default;

  static llvm::Expected<std::shared_ptr<ScriptedFrame>>
  Create(ScriptedThread &thread, StructuredData::DictionarySP args_sp,
         StructuredData::Generic *script_object = nullptr);

  lldb::RegisterContextSP GetRegisterContext() override;

  bool IsInlined() override;
  bool IsArtificial() const override;
  bool IsHidden() override;
  const char *GetFunctionName() override;
  const char *GetDisplayFunctionName() override;

private:
  void CheckInterpreterAndScriptObject() const;
  lldb::ScriptedFrameInterfaceSP GetInterface() const;

  ScriptedFrame(const ScriptedFrame &) = delete;
  const ScriptedFrame &operator=(const ScriptedFrame &) = delete;

  std::shared_ptr<DynamicRegisterInfo> GetDynamicRegisterInfo();

  const ScriptedThread &m_scripted_thread;
  lldb::ScriptedFrameInterfaceSP m_scripted_frame_interface_sp = nullptr;
  lldb_private::StructuredData::GenericSP m_script_object_sp = nullptr;
  std::shared_ptr<DynamicRegisterInfo> m_register_info_sp = nullptr;
};

} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_SCRIPTED_FRAME_H
