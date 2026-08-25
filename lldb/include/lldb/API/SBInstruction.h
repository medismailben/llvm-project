//===-- SBInstruction.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_API_SBINSTRUCTION_H
#define LLDB_API_SBINSTRUCTION_H

#include "lldb/API/SBData.h"
#include "lldb/API/SBDefines.h"

#include <cstdio>

// There's a lot to be fixed here, but need to wait for underlying insn
// implementation to be revised & settle down first.

class InstructionImpl;

namespace lldb {

class LLDB_API SBInstruction {
public:
  SBInstruction();

  SBInstruction(const SBInstruction &rhs);

  const SBInstruction &operator=(const SBInstruction &rhs);

  ~SBInstruction();

  explicit operator bool() const;

  bool IsValid();

  SBAddress GetAddress();

  const char *GetMnemonic(lldb::SBTarget target);

  const char *GetOperands(lldb::SBTarget target);

  const char *GetComment(lldb::SBTarget target);

  lldb::InstructionControlFlowKind GetControlFlowKind(lldb::SBTarget target);

  lldb::SBData GetData(lldb::SBTarget target);

  size_t GetByteSize();

  bool DoesBranch();

  bool HasDelaySlot();

  bool CanSetBreakpoint();

  /// The bytes of code a copy of this instruction needs to behave the same way
  /// from a different address.
  ///
  /// The answer does not depend on where the copy ends up, so a caller can
  /// reserve room before it has chosen a destination. It is at least
  /// GetByteSize(), and larger when behaving the same somewhere else takes a
  /// sequence rather than a single instruction.
  ///
  /// \param[out] error
  ///     Why no relocated form exists, when the result is zero. The reason is
  ///     phrased for a user, so it can be shown as-is.
  ///
  /// \return
  ///     The bytes of code to reserve, or zero when this instruction cannot be
  ///     moved out of line at any address.
  size_t GetRelocatedCodeSize(lldb::SBError &error);

  /// The bytes of constant data a copy of this instruction needs.
  ///
  /// Placed after the relocated code and never executed. Zero when the
  /// relocated form needs none, which is also what a refusal reports, so check
  /// \a error rather than the result to tell the two apart.
  size_t GetRelocatedDataSize(lldb::SBError &error);

  /// The address this instruction refers to through a PC-relative operand.
  ///
  /// This is what Relocate() has to be told to preserve. It is separate from
  /// Relocate() because a caller moving a whole range has to redirect a
  /// reference that lands inside that range to wherever that instruction's copy
  /// went, which only the caller knows.
  ///
  /// \param[in] pc
  ///     The address to interpret this instruction at. The result is in the
  ///     same address domain as this argument.
  ///
  /// \return
  ///     The referenced address, or LLDB_INVALID_ADDRESS when this instruction
  ///     is not PC-relative or its target cannot be known without running the
  ///     program, as is the case for an indirect branch.
  lldb::addr_t GetReferencedAddress(lldb::addr_t pc);

  /// Bytes that behave at \a to as this instruction does at \a from.
  ///
  /// \param[in] target
  ///     The target these bytes are destined for. Supplies the byte order and
  ///     address size the result is described with.
  ///
  /// \param[in] from
  ///     The address this instruction executes at now.
  ///
  /// \param[in] to
  ///     The address the copy will execute at.
  ///
  /// \param[in] referenced_address
  ///     The address the copy has to keep referring to, normally
  ///     GetReferencedAddress(from). Ignored when this instruction refers to
  ///     nothing, so LLDB_INVALID_ADDRESS is fine in that case.
  ///
  /// \param[out] error
  ///     Why the copy cannot reach \a referenced_address from \a to. This can
  ///     fail for an instruction GetRelocatedCodeSize() accepted, because how
  ///     far the copy moves is not known until a destination is chosen.
  ///
  /// \return
  ///     The relocated instructions, exactly GetRelocatedCodeSize() bytes of
  ///     them, or invalid data on failure.
  lldb::SBData Relocate(lldb::SBTarget target, lldb::addr_t from,
                        lldb::addr_t to, lldb::addr_t referenced_address,
                        lldb::SBError &error);

#ifndef SWIG
  void Print(FILE *out);
#endif

  void Print(SBFile out);

  void Print(FileSP BORROWED);

  bool GetDescription(lldb::SBStream &description);

  bool EmulateWithFrame(lldb::SBFrame &frame, uint32_t evaluate_options);

  bool DumpEmulation(const char *triple); // triple is to specify the
                                          // architecture, e.g. 'armv6' or
                                          // 'armv7-apple-ios'

  bool TestEmulation(lldb::SBStream &output_stream, const char *test_file);

  /// Get variable annotations for this instruction as structured data.
  /// Returns an array of dictionaries, each containing:
  /// - "variable_name": string name of the variable
  /// - "location_description": string description of where variable is stored
  ///   ("RDI", "R15", "undef", etc.)
  /// - "start_address": unsigned integer address where this annotation becomes
  ///   valid
  /// - "end_address": unsigned integer address where this annotation becomes
  ///   invalid
  /// - "register_kind": unsigned integer indicating the register numbering
  /// scheme
  /// - "decl_file": string path to the file where variable is declared
  /// - "decl_line": unsigned integer line number where variable is declared
  /// - "type_name": string type name of the variable
  lldb::SBStructuredData GetVariableAnnotations();

protected:
  friend class SBInstructionList;

  SBInstruction(const lldb::DisassemblerSP &disasm_sp,
                const lldb::InstructionSP &inst_sp);

  void SetOpaque(const lldb::DisassemblerSP &disasm_sp,
                 const lldb::InstructionSP &inst_sp);

  lldb::InstructionSP GetOpaque();

private:
  std::shared_ptr<InstructionImpl> m_opaque_sp;
};

} // namespace lldb

#endif // LLDB_API_SBINSTRUCTION_H
