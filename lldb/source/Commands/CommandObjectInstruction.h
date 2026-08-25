//===-- CommandObjectInstruction.h ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_COMMANDS_COMMANDOBJECTINSTRUCTION_H
#define LLDB_SOURCE_COMMANDS_COMMANDOBJECTINSTRUCTION_H

#include "lldb/Interpreter/CommandObjectMultiword.h"

namespace lldb_private {

/// Commands that answer questions about the instructions in a program.
///
/// Everything the debugger works out about code before running it belongs here:
/// what an instruction is, what it refers to, which registers it touches,
/// whether it can be moved somewhere else and what it would become there. Those
/// answers are used to decide whether code can be patched, and they were
/// previously only observable by adding logging to lldb and rebuilding it.
class CommandObjectMultiwordInstruction : public CommandObjectMultiword {
public:
  CommandObjectMultiwordInstruction(CommandInterpreter &interpreter);

  ~CommandObjectMultiwordInstruction() override;
};

} // namespace lldb_private

#endif // LLDB_SOURCE_COMMANDS_COMMANDOBJECTINSTRUCTION_H
