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
    /// DWARF Expression from the buffer containing the DWARF Operations and
    /// their operands.
    ///
    /// \param[in] name
    ///    The name of the variable.
    ///
    /// \param[in] size
    ///    The type size of the variable.
    ///
    /// \param[in] location_bytes
    ///    The variable's DWARF location expression, owned rather than
    ///    referenced: llvm::DWARFExpression holds a StringRef over these bytes,
    ///    so they have to outlive it and survive this being copied into the
    ///    site's vector.
    ///
    /// \param[in] is_little_endian
    ///    The byte order the location expression's operands are encoded in.
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
    /// \param[in] func_load_addr
    ///    Where the enclosing function starts, which is what selecting from a
    ///    location list needs in order to turn a pc back into the file address
    ///    the list is keyed by.
    ///
    VariableMetadata(std::string name, size_t size, lldb::offset_t offset,
                     lldb::DataBufferSP location_bytes, bool is_little_endian,
                     uint8_t address_size, DWARFExpressionList expr,
                     DWARFExpressionList &frame_base_expr,
                     lldb::addr_t func_load_addr)
        : name(std::move(name)), size(size), offset(offset),
          location_bytes(std::move(location_bytes)),
          dwarf(llvm::DataExtractor(
                    llvm::StringRef(reinterpret_cast<const char *>(
                                        this->location_bytes->GetBytes()),
                                    this->location_bytes->GetByteSize()),
                    is_little_endian),
                address_size),
          expr_list(expr), frame_base_expr_list(frame_base_expr),
          func_load_addr(func_load_addr) {}

    /// The variable name.
    std::string name;
    /// The variable size.
    size_t size;
    /// Its byte offset in the argument structure, as the condition expression's
    /// own layout assigned it. Not derivable from the position in this vector.
    lldb::offset_t offset;
    /// The bytes \a dwarf below reads. Declared before it so that it is
    /// constructed first, and shared rather than copied so that copying the
    /// metadata does not leave the expression pointing at a dead buffer.
    lldb::DataBufferSP location_bytes;
    /// The variable DWARF Expression.
    llvm::DWARFExpression dwarf;
    /// The LLDB DWARF variable expression list.
    DWARFExpressionList expr_list;
    /// The LLDB DWARF frame base register expression list.
    DWARFExpressionList frame_base_expr_list;
    /// Where the enclosing function starts.
    lldb::addr_t func_load_addr = LLDB_INVALID_ADDRESS;
  };

  size_t GetArgsStructSize() const { return m_args_struct_size; }

  /// Hand over the patch: where it was written, the bytes the branch to the
  /// trampoline is made of, and the bytes it overwrote.
  ///
  /// The ABI calls this once the site is patched. Until it does, destroying the
  /// site leaves the inferior unmodified, which is what makes it safe for the
  /// trampoline builder to bail out half way through.
  ///
  /// Both halves arrive together because a site that knows what it displaced but
  /// not what it wrote can be taken out of the inferior and never put back.
  ///
  /// The address is recorded rather than recomputed from the site's Address,
  /// because by the time a site is disabled the module it belonged to may be
  /// gone, and with it the section load list an Address needs to resolve.
  void SetPatchedInstructions(lldb::addr_t address,
                              lldb::WritableDataBufferSP patch,
                              lldb::WritableDataBufferSP displaced);

  /// Whether the ABI got far enough to record a patch that can be put back.
  bool HasPatch() const {
    return m_patch_instructions_sp && m_displaced_instructions_sp;
  }

  /// Whether the branch to the trampoline is in the inferior right now.
  bool IsPatched() const { return m_patched; }

  lldb::addr_t GetPatchAddress() const { return m_patch_addr; }
  llvm::ArrayRef<uint8_t> GetPatchBytes() const;
  llvm::ArrayRef<uint8_t> GetDisplacedBytes() const;

  /// Take ownership of the trampoline reservation this site branches to, so
  /// that the pages are given back when the site goes away.
  ///
  /// Call this as soon as the allocation succeeds: a trampoline builder that
  /// fails afterwards drops the site, which is what releases it.
  void SetTrampolineAllocation(lldb::addr_t address) {
    m_trampoline_addr = address;
  }

  /// Take ownership of the module describing the trampoline, which is what
  /// makes its addresses symbolicate and its unwind plan reachable.
  void SetTrampolineModule(lldb::ModuleSP module_sp) {
    m_trampoline_module_sp = std::move(module_sp);
  }

private:
  friend class Process;

  /// Record whether the branch to the trampoline is installed. Only Process
  /// writes this, because only Process writes to the inferior.
  void SetPatched(bool patched) { m_patched = patched; }

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
  /// The instructions the branch to the trampoline overwrote, kept so the patch
  /// can be undone. Empty until the ABI has actually patched the site.
  lldb::WritableDataBufferSP m_displaced_instructions_sp;
  /// The bytes the branch to the trampoline is made of, kept so that re-enabling
  /// the site is one write rather than a rebuild.
  lldb::WritableDataBufferSP m_patch_instructions_sp;
  /// Where the patch was written.
  lldb::addr_t m_patch_addr = LLDB_INVALID_ADDRESS;
  /// Whether the patch is in the inferior right now.
  bool m_patched = false;
  /// The trampoline this site branches to, owned by the site.
  lldb::addr_t m_trampoline_addr = LLDB_INVALID_ADDRESS;
  /// The module describing the trampoline, owned by the site.
  lldb::ModuleSP m_trampoline_module_sp;
};

} // namespace lldb_private

#endif // liblldb_BreakpointInjectedSite_h_
