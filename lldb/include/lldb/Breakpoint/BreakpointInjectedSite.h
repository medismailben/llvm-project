//===-- BreakpointInjectedSite.h --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_BreakpointInjectedSite_h_
#define liblldb_BreakpointInjectedSite_h_

#include "lldb/lldb-forward.h"

#include "lldb/Breakpoint/BreakpointLocation.h"
#include "lldb/Breakpoint/BreakpointLocationCollection.h"
#include "lldb/Breakpoint/BreakpointSite.h"
#include "lldb/Breakpoint/StopPointSiteList.h"
#include "lldb/Expression/DiagnosticManager.h"
#include "lldb/Expression/UserExpression.h"
#include "lldb/Expression/UtilityFunction.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/Process.h"
#include "lldb/Utility/DataEncoder.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"

#include "llvm/DebugInfo/DWARF/LowLevel/DWARFExpression.h"

#include <numeric>

namespace lldb_private {

/// \class BreakpointInjectedSite BreakpointInjectedSite.h
/// Class that setup fast conditional breakpoints.
///
/// Fast conditional breakpoints have a different way of evaluating the
/// condition expression by doing the check in-process, which saves the cost
/// of doing several context switches between the inferior and LLDB.
///
///
class BreakpointInjectedSite : public BreakpointSite {
public:
  /// LLVM-style RTTI support.
  static bool classof(const BreakpointSite *bp_site) {
    return bp_site->getKind() == eKindBreakpointInjectedSite;
  }

  // Destructor
  ~BreakpointInjectedSite() override;

  /// Fetch each breakpoint location's condition and build the JIT-ed condition
  /// checker with the injected trap.
  ///
  /// \return
  ///     \b true if building the condition checker succeeded,
  ///     \b false otherwise.
  bool BuildConditionExpression();

  lldb_private::ExecutionContext GetOwnerExecutionContext() {
    return m_owner_exe_ctx;
  }

  lldb::addr_t GetConditionExpressionAddress() {
    return m_condition_expression_sp->StartAddress();
  }

  lldb::addr_t GetUtilityFunctionAddress() {
    return m_create_args_struct_function_sp->StartAddress();
  }

  Address &GetRealAddress() { return m_real_addr; }

  lldb::addr_t GetTrapAddress() {
    return m_trap_addr.GetLoadAddress(m_target_sp.get());
  }

  lldb::TargetSP GetTargetSP() { return m_target_sp; }

  std::size_t GetVariableCount() { return m_metadatas.size(); }

  /// \struct ArgumentMetadata BreakpointInjectedSite.h
  /// "lldb/Breakpoint/BreakpointInjectedSite.h" Struct that contains debugging
  /// information for the variable used in the condition expression.
  struct VariableMetadata {

    // Constructor

    /// This constructor stores the variable name and type size and create a
    /// DWARF Expression from the DataExtractor containing the DWARF Operation
    /// and its operands.
    ///
    /// \param[in] name
    ///    The name of the variable.
    ///
    /// \param[in] size
    ///    The type size of the variable.
    ///
    /// \param[in] data
    ///    The buffer containing the variable DWARF Expression data.
    ///
    /// \param[in] address_size
    ///    The size in bytes for the address of the current architecture.
    ///
    /// \param[in] expr
    ///    The DWARF expression list for the variable.
    ///
    /// \param[in] frame_base_expr
    ///    The DWARF expression list for the frame base register.
    ///
    VariableMetadata(std::string name, size_t size, llvm::DataExtractor data,
                     uint8_t address_size, DWARFExpressionList expr,
                     DWARFExpressionList &frame_base_expr)
        : name(std::move(name)), size(size), dwarf(data, address_size),
          expr_list(expr), frame_base_expr_list(frame_base_expr) {}

    /// The variable name.
    std::string name;
    /// The variable size.
    size_t size;
    /// The variable DWARF Expression.
    llvm::DWARFExpression dwarf;
    /// The LLDB DWARF variable expression list.
    DWARFExpressionList expr_list;
    /// The LLDB DWARF frame base register expression list.
    DWARFExpressionList frame_base_expr_list;
  };

  size_t GetArgsStructSize() const { return m_args_struct_size; }

private:
  friend class Process;

  // Constructor

  /// This constructor stores the variable name and type size and create a
  /// DWARF Expression from the DataExtractor containing the DWARF Operation
  /// and its operands.
  ///
  /// \param[in] owner
  ///    The breakpoint location holding this breakpoint site.
  ///
  /// \param[in] addr
  ///    The breakpoint site load address.
  ///
  BreakpointInjectedSite(const lldb::BreakpointLocationSP &owner,
                         lldb::addr_t addr);

  /// Scan the JIT-ed condition expression instructions and look for the
  /// injected trap instruction.
  ///
  /// \param[in] jit
  ///     The buffer containing the JIT-ed condition expression.
  ///
  /// \param[in] size
  ///     The size of the JIT-ed condition expression in memory.
  ///
  /// \return
  ///     \b true if the injected trap instruction is found, \b false otherwise.
  bool ResolveTrapAddress(void *jit, size_t size);

  /// Iterate over the JIT-ed condition expression variable and build a metadata
  /// vector used to resolve variables when checking the condition.
  ///
  /// \return
  ///     \b true if the metadata gathering succeeded, \b false otherwise.
  bool GatherArgumentsMetadata();

  /// Build the argument structure used by the JIT-ed condition expression.
  /// Allocate dynamically the structure and using the variable metadata vector,
  /// write the variable address in the argument structure.
  ///
  /// \return
  ///     \b true if building the argument structure succeeded,
  ///     \b false otherwise.
  bool CreateArgumentsStructure();

  /// Parse the variable's DWARF Expression and return the proper source code,
  /// according to the DWARF Operation.
  ///
  /// \param[in] index
  ///     The index of the variable in the metadata vector.
  ///
  /// \param[in] error
  ///     The thread against which to test.
  ///
  /// \return
  ///     The source code needed to copy the variable in the argument structure.
  std::string ParseDWARFExpression(size_t index, Status &error);

  llvm::DataExtractor GetLLVMDataExtractor(const DataExtractor &lldb_data);

private:
  /// The target that hold the breakpoint.
  lldb::TargetSP m_target_sp;
  /// The breakpoint location load address.
  Address m_real_addr;
  /// The injected trap instruction address.
  Address m_trap_addr;
  /// The breakpoint location execution context.
  lldb_private::ExecutionContext m_owner_exe_ctx;
  /// The disassembler used to resolve the injected trap address.
  lldb::DisassemblerSP m_disassembler_sp;
  /// The JIT-ed condition checker.
  lldb::UserExpressionSP m_condition_expression_sp;
  /// The JIT-ed argument structure builder.
  lldb::UtilityFunctionSP m_create_args_struct_function_sp;
  /// The variable metadata vector.
  std::vector<VariableMetadata> m_metadatas;
  /// The size of the JIT-ed argument structure.
  size_t m_args_struct_size;
};

} // namespace lldb_private

#endif // liblldb_BreakpointInjectedSite_h_
