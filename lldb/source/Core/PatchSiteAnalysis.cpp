//===-- PatchSiteAnalysis.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Core/PatchSiteAnalysis.h"

#include "lldb/Core/Address.h"
#include "lldb/Core/Disassembler.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "lldb/Target/ThreadList.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FormatVariadic.h"

using namespace lldb;
using namespace lldb_private;

/// Disassemble from \a start up to the end of the range of \a function that
/// contains it.
static DisassemblerSP DisassembleToEndOfRange(Target &target, Function &function,
                                              addr_t start,
                                              addr_t &range_end_out) {
  range_end_out = LLDB_INVALID_ADDRESS;

  for (const AddressRange &range : function.GetAddressRanges()) {
    const addr_t range_start =
        range.GetBaseAddress().GetCallableLoadAddress(&target);
    if (start >= range_start && start < range_start + range.GetByteSize()) {
      range_end_out = range_start + range.GetByteSize();
      break;
    }
  }

  if (range_end_out == LLDB_INVALID_ADDRESS)
    return nullptr;

  const AddressRange disasm_range(start, range_end_out - start);
  return Disassembler::DisassembleRange(
      target.GetArchitecture(), /*plugin_name=*/nullptr, /*flavor=*/nullptr,
      /*cpu=*/nullptr, /*features=*/nullptr, target, disasm_range,
      /*force_live_memory=*/true);
}

llvm::Error
PatchSiteAnalysis::CanPatch(Process &process, const Address &site,
                            size_t patch_size,
                            llvm::ArrayRef<llvm::StringRef> clobbered_registers) {
  if (!patch_size)
    return llvm::createStringError("a patch cannot be zero bytes wide");

  if (llvm::Error error = CheckDisplacedInstructions(process, site, patch_size))
    return error;

  if (llvm::Error error = CheckNoBranchIntoPatch(process, site, patch_size))
    return error;

  for (llvm::StringRef reg_name : clobbered_registers)
    if (llvm::Error error = CheckRegisterIsDead(process, site, reg_name))
      return error;

  return CheckNoThreadInPatch(process, site, patch_size);
}

llvm::Error PatchSiteAnalysis::CheckDisplacedInstructions(Process &process,
                                                          const Address &site,
                                                          size_t patch_size) {
  Target &target = process.GetTarget();
  const addr_t site_addr = site.GetCallableLoadAddress(&target);

  Address resolved_site(site);
  Function *function = resolved_site.CalculateSymbolContextFunction();
  if (!function)
    return llvm::createStringError(
        llvm::formatv("no function covers {0:x}, so the instructions a branch "
                      "would displace cannot be identified",
                      site_addr));

  addr_t range_end = LLDB_INVALID_ADDRESS;
  DisassemblerSP disassembler_sp =
      DisassembleToEndOfRange(target, *function, site_addr, range_end);

  if (!disassembler_sp)
    return llvm::createStringError(llvm::formatv(
        "couldn't disassemble '{0}' at {1:x}", function->GetName(), site_addr));

  InstructionList &instructions = disassembler_sp->GetInstructionList();

  // Walk whole instructions until the branch is covered. A branch that ends
  // part way through an instruction would leave the tail of that instruction
  // behind to be executed as though it were a new one.
  size_t displaced_size = 0;
  for (size_t i = 0; i < instructions.GetSize() && displaced_size < patch_size;
       ++i) {
    InstructionSP instruction = instructions.GetInstructionAtIndex(i);
    const uint32_t size = instruction->GetOpcode().GetByteSize();

    if (!size)
      return llvm::createStringError(
          llvm::formatv("couldn't determine the size of the instruction at "
                        "{0:x}",
                        site_addr + displaced_size));

    // Relocating an instruction that refers to its own address would change
    // what it refers to. Adjusting the operand is possible for some forms, but
    // until that exists these have to be refused.
    if (instruction->IsPCRelative())
      return llvm::createStringError(llvm::formatv(
          "the instruction at {0:x} is position dependent, so it cannot be "
          "moved out of line",
          site_addr + displaced_size));

    displaced_size += size;
  }

  if (displaced_size < patch_size)
    return llvm::createStringError(llvm::formatv(
        "only {0} bytes of instructions are available at {1:x} before the end "
        "of '{2}', and the branch needs {3}",
        displaced_size, site_addr, function->GetName(), patch_size));

  return llvm::Error::success();
}

llvm::Error PatchSiteAnalysis::CheckNoBranchIntoPatch(Process &process,
                                                      const Address &site,
                                                      size_t patch_size) {
  Target &target = process.GetTarget();
  const addr_t site_addr = site.GetCallableLoadAddress(&target);
  const addr_t patch_end = site_addr + patch_size;

  Address resolved_site(site);
  Function *function = resolved_site.CalculateSymbolContextFunction();
  if (!function)
    return llvm::createStringError(
        llvm::formatv("no function covers {0:x}", site_addr));

  // The whole function has to be examined, not just the part after the site,
  // because a backward branch from later in the function can land in the patch
  // just as easily as a forward one.
  const AddressRanges ranges = function->GetAddressRanges();
  DisassemblerSP disassembler_sp = Disassembler::DisassembleRange(
      target.GetArchitecture(), /*plugin_name=*/nullptr, /*flavor=*/nullptr,
      /*cpu=*/nullptr, /*features=*/nullptr, target, ranges,
      /*force_live_memory=*/true);

  if (!disassembler_sp)
    return llvm::createStringError(
        llvm::formatv("couldn't disassemble '{0}'", function->GetName()));

  InstructionList &instructions = disassembler_sp->GetInstructionList();

  for (size_t i = 0; i < instructions.GetSize(); ++i) {
    InstructionSP instruction = instructions.GetInstructionAtIndex(i);
    const addr_t pc = instruction->GetAddress().GetCallableLoadAddress(&target);

    if (!instruction->DoesBranch())
      continue;

    // An indirect branch computes its destination at run time, so there is no
    // way to prove it does not land in the patch. Jump table recovery would
    // narrow this down; until then any of them disqualifies the function.
    std::optional<addr_t> destination = instruction->GetReferencedAddress(pc);
    if (!destination) {
      // A call is exempt: it returns to the instruction after itself, and if
      // that instruction is inside the patch then the call itself is being
      // displaced, which the displaced instruction check already covers. A
      // return leaves the function altogether, so it cannot land in the patch
      // either. Both report no destination because neither has a PC-relative
      // operand.
      if (instruction->IsCall() || instruction->IsReturn())
        continue;

      return llvm::createStringError(llvm::formatv(
          "the branch at {0:x} in '{1}' has a target that is only known at run "
          "time, so it cannot be shown to avoid the patch",
          pc, function->GetName()));
    }

    // Landing exactly on the site is fine, that is where the branch will be.
    // Landing after it is what breaks.
    if (*destination > site_addr && *destination < patch_end)
      return llvm::createStringError(llvm::formatv(
          "the branch at {0:x} targets {1:x}, which is inside the {2} bytes a "
          "branch at {3:x} would overwrite",
          pc, *destination, patch_size, site_addr));
  }

  return llvm::Error::success();
}

llvm::Error PatchSiteAnalysis::CheckRegisterIsDead(Process &process,
                                                   const Address &site,
                                                   llvm::StringRef reg_name) {
  Target &target = process.GetTarget();
  const addr_t site_addr = site.GetCallableLoadAddress(&target);

  Address resolved_site(site);
  Function *function = resolved_site.CalculateSymbolContextFunction();
  if (!function)
    return llvm::createStringError(
        llvm::formatv("no function covers {0:x}", site_addr));

  const AddressRanges ranges = function->GetAddressRanges();
  DisassemblerSP disassembler_sp = Disassembler::DisassembleRange(
      target.GetArchitecture(), /*plugin_name=*/nullptr, /*flavor=*/nullptr,
      /*cpu=*/nullptr, /*features=*/nullptr, target, ranges,
      /*force_live_memory=*/true);

  if (!disassembler_sp)
    return llvm::createStringError(
        llvm::formatv("couldn't disassemble '{0}'", function->GetName()));

  InstructionList &instructions = disassembler_sp->GetInstructionList();

  // Index the function by address so successors can be looked up. Whether an
  // address is inside the function at all falls out of this too.
  llvm::DenseMap<addr_t, size_t> index_of_address;
  for (size_t i = 0; i < instructions.GetSize(); ++i) {
    InstructionSP instruction = instructions.GetInstructionAtIndex(i);
    index_of_address[instruction->GetAddress().GetCallableLoadAddress(&target)] =
        i;
  }

  auto live = [&](addr_t pc, llvm::StringRef why) {
    return llvm::createStringError(llvm::formatv(
        "{0} may still be needed at {1:x} in '{2}': {3}", reg_name, site_addr,
        function->GetName(), llvm::formatv("{0} at {1:x}", why, pc)));
  };

  // Follow every path from the site. The register is dead when no path reads it
  // before the value stops mattering, which happens at a write, at a call, or at
  // a return.
  //
  // Note this starts at the site rather than after the patch, because the
  // displaced instructions run later out of the trampoline, by which time the
  // patch sequence has already overwritten the register.
  llvm::SmallVector<addr_t, 16> worklist;
  llvm::DenseSet<addr_t> visited;
  worklist.push_back(site_addr);

  // A budget keeps a pathological function from turning breakpoint setting into
  // a long analysis. Running out means the answer is unknown, so the register is
  // assumed live.
  size_t budget = 4096;

  while (!worklist.empty()) {
    const addr_t pc = worklist.pop_back_val();

    if (!visited.insert(pc).second)
      continue;

    if (!budget--)
      return llvm::createStringError(llvm::formatv(
          "gave up proving {0} is dead at {1:x} in '{2}' after {3} "
          "instructions",
          reg_name, site_addr, function->GetName(), visited.size()));

    auto it = index_of_address.find(pc);
    if (it == index_of_address.end())
      return live(pc, "control leaves the function at");

    InstructionSP instruction = instructions.GetInstructionAtIndex(it->second);
    const Instruction::RegisterAccess access =
        instruction->GetRegisterAccess(reg_name);

    // A read means the old value mattered, so the patch cannot overwrite it.
    // This is checked before the write so that a read-modify-write counts.
    if (access.reads)
      return live(pc, "read by the instruction");

    // Overwritten without being read, so nothing downstream can observe what
    // the patch did to it.
    if (access.writes)
      continue;

    // The ABI lets a callee clobber the scratch registers, so a path that
    // reaches a call without reading first cannot care about the old value.
    if (instruction->IsCall())
      continue;

    const uint32_t size = instruction->GetOpcode().GetByteSize();
    if (!size)
      return live(pc, "unknown instruction size at");

    if (!instruction->DoesBranch()) {
      worklist.push_back(pc + size);
      continue;
    }

    std::optional<addr_t> destination = instruction->GetReferencedAddress(pc);
    const lldb::InstructionControlFlowKind kind =
        instruction->GetControlFlowKind(nullptr);

    // A return ends the path: the scratch registers are caller saved, so
    // nothing expects this one to survive.
    if (kind == lldb::eInstructionControlFlowKindReturn ||
        kind == lldb::eInstructionControlFlowKindFarReturn)
      continue;

    // Anything else whose destination cannot be named leaves paths unexplored.
    if (!destination)
      return live(pc, "an unresolved branch at");

    worklist.push_back(*destination);

    // Only an unconditional jump has no fallthrough.
    if (kind != lldb::eInstructionControlFlowKindJump &&
        kind != lldb::eInstructionControlFlowKindFarJump)
      worklist.push_back(pc + size);
  }

  return llvm::Error::success();
}

llvm::Error PatchSiteAnalysis::CheckNoThreadInPatch(Process &process,
                                                    const Address &site,
                                                    size_t patch_size) {
  Target &target = process.GetTarget();
  const addr_t site_addr = site.GetCallableLoadAddress(&target);
  const addr_t patch_end = site_addr + patch_size;

  ThreadList &threads = process.GetThreadList();
  const uint32_t num_threads = threads.GetSize();

  for (uint32_t i = 0; i < num_threads; ++i) {
    ThreadSP thread_sp = threads.GetThreadAtIndex(i);
    if (!thread_sp)
      continue;

    RegisterContextSP reg_ctx_sp = thread_sp->GetRegisterContext();
    if (!reg_ctx_sp)
      continue;

    const addr_t pc = reg_ctx_sp->GetPC();
    if (pc >= site_addr && pc < patch_end)
      return llvm::createStringError(
          llvm::formatv("thread {0:x} is stopped at {1:x}, inside the range a "
                        "branch at {2:x} would overwrite",
                        thread_sp->GetID(), pc, site_addr));
  }

  return llvm::Error::success();
}
