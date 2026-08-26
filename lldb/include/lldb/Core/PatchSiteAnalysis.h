//===-- PatchSiteAnalysis.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_CORE_PATCHSITEANALYSIS_H
#define LLDB_CORE_PATCHSITEANALYSIS_H

#include "lldb/lldb-forward.h"
#include "lldb/lldb-types.h"

#include "llvm/Support/Error.h"

#include <cstddef>
#include <vector>

namespace lldb_private {

class Address;
class Process;

/// Decides whether a range of instructions can be overwritten with a branch and
/// executed from somewhere else instead.
///
/// Overwriting code in a live process is only safe under a set of conditions
/// that have nothing to do with why the code is being overwritten, so this is
/// deliberately not specific to breakpoints. Fast conditional breakpoints are
/// the first user; tracing and instrumentation need the same answers.
///
/// The analysis is conservative by construction: it answers "no" whenever it
/// cannot prove "yes". A caller that is refused is expected to fall back to
/// whatever it did before code patching was available, so a false negative
/// costs performance while a false positive corrupts the process being
/// debugged.
class PatchSiteAnalysis {
public:
  /// What it takes to run the instructions a patch displaces from somewhere
  /// else.
  struct PatchPlan {
    /// The bytes the patch overwrites: \a patch_size rounded up to an
    /// instruction boundary.
    size_t displaced_size = 0;
    /// Those instructions, in the order they execute.
    std::vector<lldb::InstructionSP> displaced_instructions;
    /// Bytes of code needed to run them from somewhere else. Equal to
    /// \a displaced_size when none of them is position dependent, larger when
    /// one has to be rewritten as a sequence.
    size_t relocated_code_size = 0;
    /// Keeps the instructions above usable.
    ///
    /// An Instruction holds a weak reference back to the disassembler that
    /// decoded it and needs it alive to answer anything that has to consult the
    /// decoded form, which is most of what a caller relocating one will ask.
    /// Nothing else here owns it, so without this the instructions are dangling
    /// by the time CanPatch() returns.
    lldb::DisassemblerSP disassembler;
  };

  /// Whether \a patch_size bytes at \a site can be replaced with a branch.
  ///
  /// \param[in] process
  ///     The process to patch. Must be stopped, since the check inspects the
  ///     program counter of every thread.
  ///
  /// \param[in] site
  ///     The first byte that would be overwritten.
  ///
  /// \param[in] patch_size
  ///     How many bytes the branch occupies.
  ///
  /// \param[in] clobbered_registers
  ///     Registers the patch sequence itself overwrites before control reaches
  ///     the trampoline, named as the disassembler spells them. An indirect
  ///     branch needs a scratch register, and its previous contents are gone by
  ///     the time the trampoline could save them, so each one has to be proven
  ///     dead at the site.
  ///
  /// \return
  ///     What the caller has to reserve to run the displaced instructions out
  ///     of line, otherwise an error describing which condition failed,
  ///     suitable for showing to the user as the reason a faster implementation
  ///     was not available.
  static llvm::Expected<PatchPlan>
  CanPatch(Process &process, const Address &site, size_t patch_size,
           llvm::ArrayRef<llvm::StringRef> clobbered_registers = {});

  /// Whether a callee may clobber \a reg_name without restoring it.
  ///
  /// The calling convention decides two of the conditions below: a path that
  /// reaches a call or a return stops mattering for a caller saved register,
  /// and means the opposite for a callee saved one, whose value belongs to the
  /// caller. Exposed so that anything reporting a verdict can report the reason
  /// from the same authority rather than from a second table that could
  /// disagree with it.
  ///
  /// Answers false when the question cannot be settled, which is the answer
  /// that keeps a register live.
  ///
  /// Note this comes from lldb's ABI plugin rather than from LLVM's MC layer,
  /// which carries no calling convention information: LLVM keeps it in
  /// TargetRegisterInfo, which belongs to CodeGen and is not linked here.
  static bool IsCallerSaved(Process &process, llvm::StringRef reg_name);

  /// The individual conditions CanPatch() requires, in the order it applies
  /// them.
  ///
  /// CanPatch() stops at the first condition that fails, which is what a caller
  /// about to patch wants and what someone asking why a site was refused does
  /// not: an explanation has to name every condition, including the ones that
  /// hold. Anything deciding whether to patch should call CanPatch() rather
  /// than assemble the conjunction itself, so that the order and the set of
  /// conditions stay in one place.
  /// @{

  /// The instructions the branch would displace must be whole instructions, and
  /// they must all be safe to execute from a different address.
  static llvm::Error CheckDisplacedInstructions(Process &process,
                                                const Address &site,
                                                size_t patch_size,
                                                PatchPlan &plan);

  /// Nothing already in the function may jump into the middle of the branch we
  /// are about to write.
  static llvm::Error CheckNoBranchIntoPatch(Process &process,
                                            const Address &site,
                                            size_t patch_size);

  /// No thread may be executing inside the range being overwritten.
  static llvm::Error CheckNoThreadInPatch(Process &process, const Address &site,
                                          size_t patch_size);

  /// \a reg_name must hold no value that the program still needs at \a site.
  ///
  /// Proven by following every path forward from the site until the register is
  /// either read, which means it was live, or provably no longer needed: it is
  /// overwritten, the path reaches a call, since the ABI lets a callee clobber
  /// the scratch registers, or the path returns. A path whose continuation
  /// cannot be determined leaves the register assumed live.
  static llvm::Error CheckRegisterIsDead(Process &process, const Address &site,
                                         llvm::StringRef reg_name);

  /// @}
};

} // namespace lldb_private

#endif // LLDB_CORE_PATCHSITEANALYSIS_H
