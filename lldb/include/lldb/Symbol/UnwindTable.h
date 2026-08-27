//===-- UnwindTable.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SYMBOL_UNWINDTABLE_H
#define LLDB_SYMBOL_UNWINDTABLE_H

#include <atomic>
#include <map>
#include <mutex>
#include <optional>

#include "lldb/lldb-private.h"

namespace lldb_private {

// A class which holds all the FuncUnwinders objects for a given ObjectFile.
// The UnwindTable is populated with FuncUnwinders objects lazily during the
// debug session.

class UnwindTable {
public:
  /// Create an Unwind table using the data in the given module.
  explicit UnwindTable(Module &module);

  ~UnwindTable();

  lldb_private::CallFrameInfo *GetObjectFileUnwindInfo();

  lldb_private::DWARFCallFrameInfo *GetEHFrameInfo();
  lldb_private::DWARFCallFrameInfo *GetDebugFrameInfo();

  lldb_private::CompactUnwindInfo *GetCompactUnwindInfo();

  ArmUnwindInfo *GetArmUnwindInfo();
  SymbolFile *GetSymbolFile();

  lldb::FuncUnwindersSP
  GetFuncUnwindersContainingAddress(const Address &addr,
                                    const SymbolContext &sc);

  bool GetAllowAssemblyEmulationUnwindPlans();

  // Normally when we create a new FuncUnwinders object we track it in this
  // UnwindTable so it can be reused later.  But for the target modules show-
  // unwind we want to create brand new UnwindPlans for the function of
  // interest - so ignore any existing FuncUnwinders for that function and
  // don't add this new one to our UnwindTable. This FuncUnwinders object does
  // have a reference to the UnwindTable but the lifetime of this uncached
  // FuncUnwinders is expected to be short so in practice this will not be a
  // problem.
  lldb::FuncUnwindersSP
  GetUncachedFuncUnwindersContainingAddress(const Address &addr,
                                            const SymbolContext &sc);

  ArchSpec GetArchitecture();

  /// Record the unwind plan for a trampoline lldb wrote itself, so that every
  /// FuncUnwinders for the function starting at \a start_file_addr reports it.
  ///
  /// Kept here rather than on the FuncUnwinders because ModuleWasUpdated()
  /// throws those away, and there is nowhere else to recover a plan for
  /// synthesized code from: nothing on disk describes it. Setting it directly
  /// on a FuncUnwinders meant it survived only until the first cache flush,
  /// after which the frame was unwound with an assembly plan that assumes a
  /// normal call.
  void SetTrampolineUnwindPlan(lldb::addr_t start_file_addr,
                               lldb::UnwindPlanSP plan_sp);

  /// Called after an ObjectFile/SymbolFile has been added to a Module to add
  /// any new unwind sections that may now be available.
  void ModuleWasUpdated();

private:
  void Dump(Stream &s);

  void Initialize();
  AddressRanges GetAddressRanges(const Address &addr, const SymbolContext &sc);

  /// Hand \a func_unwinders_sp the trampoline plan recorded for its function,
  /// if there is one. Called for every FuncUnwinders this table produces,
  /// cached or not, so that a flush re-installs rather than loses it.
  void InstallTrampolineUnwindPlan(lldb::FuncUnwindersSP &func_unwinders_sp);

  typedef std::map<lldb::addr_t, lldb::FuncUnwindersSP> collection;
  typedef collection::iterator iterator;
  typedef collection::const_iterator const_iterator;

  Module &m_module;
  collection m_unwinds;
  /// Unwind plans for the trampolines lldb wrote into this module, by the file
  /// address their function starts at. Deliberately not cleared by
  /// ModuleWasUpdated().
  std::map<lldb::addr_t, lldb::UnwindPlanSP> m_trampoline_plans;

  /// This is true when we have looked at the ObjectFile and SymbolFile for all
  /// sources of unwind information; false if we haven't done that yet, or one
  /// of the files has been updated in the Module.
  std::atomic<bool> m_scanned_all_unwind_sources;
  std::mutex m_mutex;

  std::unique_ptr<CallFrameInfo> m_object_file_unwind_up;
  std::unique_ptr<DWARFCallFrameInfo> m_eh_frame_up;
  std::unique_ptr<DWARFCallFrameInfo> m_debug_frame_up;
  std::unique_ptr<CompactUnwindInfo> m_compact_unwind_up;
  std::unique_ptr<ArmUnwindInfo> m_arm_unwind_up;

  UnwindTable(const UnwindTable &) = delete;
  const UnwindTable &operator=(const UnwindTable &) = delete;
};

} // namespace lldb_private

#endif // LLDB_SYMBOL_UNWINDTABLE_H
