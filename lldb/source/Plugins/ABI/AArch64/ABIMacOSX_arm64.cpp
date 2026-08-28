//===-- ABIMacOSX_arm64.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ABIMacOSX_arm64.h"

#include <cmath>
#include <optional>
#include <vector>

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/TargetParser/Triple.h"

#include "Utility/ARM64_DWARF_Registers.h"
#include "lldb/Breakpoint/BreakpointInjectedSite.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/PatchSiteAnalysis.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/Value.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/Utility/DataBufferHeap.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/RegisterValue.h"
#include "lldb/Utility/Scalar.h"
#include "lldb/Utility/Status.h"
#include "lldb/ValueObject/ValueObjectConstResult.h"
#include "lldb/lldb-enumerations.h"
#include "lldb/lldb-forward.h"

using namespace lldb;
using namespace lldb_private;

static const char *pluginDesc = "Mac OS X ABI for arm64 targets";

bool ABIMacOSX_arm64::GetFramePointerRegister(const char *&name) {
  name = "fp";
  return true;
}

llvm::Expected<std::string> ABIMacOSX_arm64::GetRegisterName(uint32_t num) {
  if (!m_mc_register_info_up)
    return llvm::createStringError(
        llvm::formatv("Failed to get register name for register #{0}: No "
                      "register information in ABI.",
                      num));

  // A DWARF register number, which for AArch64 gives 0 to 30 to x0 to x30 and
  // 31 to the stack pointer. 32 is ELR_mode and 64 to 95 are the vector
  // registers, none of which the caller can name, so they are refused here
  // rather than turned into a field that does not exist.
  //
  // Not compared against getNumRegs(), which counts MC register numbers. That
  // is a different and much larger numbering, so it let a vector register
  // through as "x64".
  constexpr uint32_t dwarf_sp_regnum = 31;
  if (num > dwarf_sp_regnum)
    return llvm::createStringError(
        llvm::formatv("Failed to get register name for register #{0}: not a "
                      "general purpose register.",
                      num));

  // Built directly rather than through a named Twine. A Twine holds references
  // to what it was built from, and the documentation is explicit that one must
  // never outlive the expression that made it, so storing it and calling str()
  // in the next statement reads temporaries that are already gone.
  return GetMCName("x" + std::to_string(num));
}

/// AArch64 `nop`, little endian. Two of these are emitted at the end of the
/// trampoline to reserve room for the displaced instruction and the branch back
/// to user code.
static const uint8_t g_aarch64_nop_bytes[] = {0x1f, 0x20, 0x03, 0xd5};

/// `b` encodes a signed 26 bit immediate scaled by the instruction size, which
/// gives it a reach of +/-128MiB.
static bool IsBranchInRange(int64_t byte_offset) {
  // Signed arithmetic throughout: aarch64_instr_size is unsigned, so dividing a
  // negative offset by it directly would convert the offset to a huge unsigned
  // value and report every backward branch as unreachable.
  constexpr int64_t instr_size = ABIMacOSX_arm64::aarch64_instr_size;

  if (byte_offset % instr_size)
    return false;
  return llvm::isInt<26>(byte_offset / instr_size);
}

/// Encode an unconditional `b` to \a byte_offset bytes from itself, little
/// endian.
///
/// Assembling a single branch by handing text to clang and JIT-ing the result
/// costs a full parse of the expression prefix, an IR pass and an allocation in
/// the inferior, all to produce four bytes. Doing it directly is what the
/// x86_64 builder already does, and it keeps the cost of arming a breakpoint
/// proportional to the work that actually has to happen.
///
/// \a byte_offset has to be in range, see IsBranchInRange().
static void
EncodeAArch64Branch(int64_t byte_offset,
                    uint8_t out[ABIMacOSX_arm64::aarch64_instr_size]) {
  constexpr uint32_t b_opcode = 0x14000000;
  constexpr uint32_t imm26_mask = 0x03ffffff;

  assert(IsBranchInRange(byte_offset) && "branch offset out of reach");

  // Arithmetic shift, so a negative offset keeps its sign before it is
  // truncated to the 26 bit field.
  const uint32_t instruction =
      b_opcode | (static_cast<uint32_t>(byte_offset >> 2) & imm26_mask);

  for (unsigned byte = 0; byte < ABIMacOSX_arm64::aarch64_instr_size; ++byte)
    out[byte] = (instruction >> (8 * byte)) & 0xff;
}

/// Write one instruction word, little endian.
static void EncodeAArch64Word(uint32_t word, uint8_t *out) {
  for (unsigned byte = 0; byte < ABIMacOSX_arm64::aarch64_instr_size; ++byte)
    out[byte] = (word >> (8 * byte)) & 0xff;
}

/// A register the wider patch form can get to its target through.
struct AArch64ScratchRegister {
  /// As the disassembler and the trampoline's register context spell it.
  const char *name;
  /// As the instruction encodings below take it.
  uint32_t number;
};

/// The registers the wider patch form will consider, in the order it tries
/// them.
///
/// x16 and x17 first: the procedure call standard names them IP0 and IP1 and
/// sets them aside for exactly this, a jump that needs a register to reach
/// where it is going. The remaining temporaries follow, so that a site where
/// the pair the linker uses happens to be live is not refused while another is
/// free.
///
/// Whichever is chosen, the register context the trampoline saves holds the
/// trampoline's own address for it rather than what the program had there, so
/// reading it from a frame below the trampoline reports that instead. That is
/// the register the site proved the program no longer reads, which is why this
/// is acceptable, and why the condition is not allowed to read it either.
static constexpr AArch64ScratchRegister g_aarch64_scratch_registers[] = {
    {"x16", 16}, {"x17", 17}, {"x9", 9},   {"x10", 10}, {"x11", 11},
    {"x12", 12}, {"x13", 13}, {"x14", 14}, {"x15", 15}};

/// Encode `adrp reg, page(to)` / `add reg, reg, offset(to)` / `br reg` at
/// \a from, which is how a patch reaches a trampoline a direct branch cannot.
///
/// Three instructions rather than a literal load and a branch, which would be
/// four: every extra instruction the patch displaces is another one that has to
/// be safe to run out of line, and another place nothing in the function may
/// branch into.
static llvm::Error
EncodeAArch64FarBranch(lldb::addr_t from, lldb::addr_t to, uint32_t reg,
                       uint8_t out[ABIMacOSX_arm64::aarch64_far_patch_size]) {
  constexpr uint32_t adrp_opcode = 0x90000000;
  constexpr uint32_t add_imm64_opcode = 0x91000000;
  constexpr uint32_t br_opcode = 0xd61f0000;
  // `adrp` counts 4KiB pages whatever page size the process runs with: the
  // immediate is defined by the instruction, not by the kernel.
  constexpr unsigned adrp_page_shift = 12;
  constexpr uint32_t adrp_page_mask = (1 << adrp_page_shift) - 1;

  const int64_t pages = static_cast<int64_t>(to >> adrp_page_shift) -
                        static_cast<int64_t>(from >> adrp_page_shift);
  if (!llvm::isInt<21>(pages))
    return llvm::createStringError(llvm::formatv(
        "{0:x} is more than 4GiB from {1:x}, which no pc relative address on "
        "this architecture reaches",
        to, from));

  // The immediate is split, with its two low bits at the top of the word.
  const uint32_t imm21 = static_cast<uint32_t>(pages) & 0x1fffff;
  EncodeAArch64Word(adrp_opcode | ((imm21 & 0x3) << 29) |
                        (((imm21 >> 2) & 0x7ffff) << 5) | reg,
                    out);
  EncodeAArch64Word(add_imm64_opcode |
                        ((static_cast<uint32_t>(to) & adrp_page_mask) << 10) |
                        (reg << 5) | reg,
                    out + ABIMacOSX_arm64::aarch64_instr_size);
  EncodeAArch64Word(br_opcode | (reg << 5),
                    out + 2 * ABIMacOSX_arm64::aarch64_instr_size);
  return llvm::Error::success();
}

/// Encode four `movz`/`movk` building \a value in \a reg, then `br reg`, which
/// is how the trampoline gets back to a site a direct branch cannot reach.
///
/// Absolute rather than pc relative, unlike the patch: the trampoline has room
/// for the extra two instructions, and an absolute address cannot be out of
/// range, so where the allocator happened to put the trampoline stops mattering
/// once it is reachable from the site at all.
static void EncodeAArch64FarBranchAbsolute(
    lldb::addr_t value, uint32_t reg,
    uint8_t out[ABIMacOSX_arm64::aarch64_far_branch_slots *
                ABIMacOSX_arm64::aarch64_instr_size]) {
  constexpr uint32_t movz64_opcode = 0xd2800000;
  constexpr uint32_t movk64_opcode = 0xf2800000;
  constexpr uint32_t br_opcode = 0xd61f0000;
  constexpr unsigned field_shift = 21;
  constexpr unsigned fields = 4;

  for (unsigned field = 0; field < fields; ++field) {
    const uint32_t opcode = field ? movk64_opcode : movz64_opcode;
    const uint32_t imm16 = (value >> (16 * field)) & 0xffff;
    EncodeAArch64Word(opcode | (field << field_shift) | (imm16 << 5) | reg,
                      out + field * ABIMacOSX_arm64::aarch64_instr_size);
  }
  EncodeAArch64Word(br_opcode | (reg << 5),
                    out + fields * ABIMacOSX_arm64::aarch64_instr_size);
}

/// Locate the run of \a count reserved nops at the end of the emitted
/// trampoline.
///
/// Scanning for them rather than hardcoding an instruction index keeps this
/// working when the assembler changes how many instructions the preceding code
/// needs, which it does depending on the immediates and literal pools involved.
static lldb::offset_t FindTrampolineNopSlots(llvm::ArrayRef<uint8_t> buffer,
                                             size_t count) {
  const size_t instr_size = ABIMacOSX_arm64::aarch64_instr_size;
  const size_t slots_size = count * instr_size;

  if (!count || buffer.size() < slots_size)
    return LLDB_INVALID_OFFSET;

  for (lldb::offset_t offset = 0; offset + slots_size <= buffer.size();
       offset += instr_size) {
    size_t found = 0;
    while (found < count &&
           !std::memcmp(buffer.data() + offset + found * instr_size,
                        g_aarch64_nop_bytes, sizeof(g_aarch64_nop_bytes)))
      ++found;
    if (found == count)
      return offset;
  }

  return LLDB_INVALID_OFFSET;
}

llvm::Error ABIMacOSX_arm64::SetupFastConditionalBreakpointTrampoline(
    BreakpointInjectedSite *bp_injected_site) {
  TargetSP target_sp = bp_injected_site->GetTargetSP();
  if (!target_sp)
    return llvm::createStringError("the breakpoint site has no target");

  ProcessSP process_sp = target_sp->GetProcessSP();
  if (!process_sp)
    return llvm::createStringError("there is no live process to patch");

  Address &bp_addr = bp_injected_site->GetRealAddress();
  const addr_t bp_load_addr = bp_addr.GetLoadAddress(target_sp.get());

  // Allocate the trampoline before building it. How far it lands from the site
  // decides which form the patch takes, and that decides how many instructions
  // the patch displaces, which decides how much room the trampoline needs at
  // its end. Asking the other way round means guessing at the answer and
  // refusing when the guess turns out wrong.
  Status error;
  const uint32_t permission = ePermissionsReadable | ePermissionsExecutable;
  const addr_t trampoline_addr = process_sp->AllocateFCBTrampoline(
      bp_load_addr, aarch64_trampoline_reservation, permission,
      aarch64_far_patch_reach, error);
  if (!trampoline_addr || trampoline_addr == LLDB_INVALID_ADDRESS)
    return llvm::createStringError(llvm::formatv(
        "Couldn't allocate trampoline buffer: {0}", error.AsCString()));

  if (!process_sp->NewFCBTrampolineAllocation(trampoline_addr,
                                              aarch64_trampoline_reservation))
    return llvm::createStringError(
        "Allocated trampoline address already in use");

  // The site owns the reservation from here on, so any failure below releases
  // it by destroying the site.
  bp_injected_site->SetTrampolineAllocation(trampoline_addr);

  // The narrowest patch that reaches what was allocated. Measured to the far
  // end of the reservation, because the branch back to the site is emitted near
  // there and has to reach just as far.
  const addr_t distance =
      trampoline_addr > bp_load_addr
          ? (trampoline_addr + aarch64_trampoline_reservation) - bp_load_addr
          : bp_load_addr - trampoline_addr;
  const bool direct = distance <= aarch64_branch_reach;

  const size_t patch_size =
      direct ? aarch64_instr_size : aarch64_far_patch_size;

  // Everything after this assumes the displaced instructions can run from a
  // different address and that nothing else in the function reaches into the
  // bytes being overwritten. Refuse rather than corrupt the inferior when that
  // does not hold; the caller falls back to evaluating the condition out of
  // process.
  //
  // The wider form also needs a register to get where it is going, and whatever
  // that register held is gone before the trampoline could save it, so it has
  // to be one this site proves the program no longer needs. Candidates are
  // tried in turn because a site that refuses one may well accept another, and
  // being refused here costs the speedup this feature exists for.
  std::optional<AArch64ScratchRegister> scratch;
  llvm::Expected<PatchSiteAnalysis::PatchPlan> plan = [&] {
    if (direct)
      return PatchSiteAnalysis::CanPatch(*process_sp, bp_addr, patch_size);

    llvm::Error refused = llvm::Error::success();
    for (const AArch64ScratchRegister &candidate :
         g_aarch64_scratch_registers) {
      // Not just any dead register: one the condition reads out of the register
      // context would be handed the trampoline's own address instead of the
      // value the variable had, which is a wrong answer rather than a refusal.
      if (bp_injected_site->ConditionReadsRegister(candidate.name))
        continue;

      llvm::Expected<PatchSiteAnalysis::PatchPlan> attempt =
          PatchSiteAnalysis::CanPatch(*process_sp, bp_addr, patch_size,
                                      {candidate.name});
      if (attempt) {
        consumeError(std::move(refused));
        scratch = candidate;
        return attempt;
      }

      // The first refusal is the one worth reporting: the conditions are the
      // same for every candidate but the register, so a later one fails the
      // same way or fails on its own register, and a list of both is longer
      // without saying more.
      if (!refused)
        refused = attempt.takeError();
      else
        consumeError(attempt.takeError());
    }

    llvm::Error unreachable = llvm::createStringError(llvm::formatv(
        "the nearest trampoline the site at {0:x} could get is {1} bytes away, "
        "further than a direct branch reaches, so the patch has to get there "
        "through a register and this site has none to spare",
        bp_load_addr, distance));

    if (refused)
      return llvm::Expected<PatchSiteAnalysis::PatchPlan>(
          llvm::joinErrors(std::move(unreachable), std::move(refused)));
    return llvm::Expected<PatchSiteAnalysis::PatchPlan>(std::move(unreachable));
  }();
  if (!plan)
    return plan.takeError();

  // The trampoline reserves room for the relocated form plus the branch back,
  // so a relocated form that is a sequence is fine as long as it is whole
  // instructions. Nothing here needs a constant pool yet, so a form asking for
  // data has nowhere to put it.
  if (!plan->relocated_code_size ||
      plan->relocated_code_size % aarch64_instr_size)
    return llvm::createStringError(llvm::formatv(
        "the instruction displaced at {0:x} needs {1} bytes to run out of "
        "line, "
        "which is not a whole number of instructions",
        bp_injected_site->GetLoadAddress(), plan->relocated_code_size));

  // One slot per relocated instruction, plus the branch back to the user's
  // code, which takes the wider form too when the site is out of reach of a
  // direct branch from the trampoline.
  const size_t branch_back_slots = direct ? 1 : aarch64_far_branch_slots;
  const size_t reserved_slots =
      plan->relocated_code_size / aarch64_instr_size + branch_back_slots;

  std::stringstream ss;

  ss << "__attribute__((naked,noreturn)) void $__lldb_emit_trampoline() {\n"
     << "    __asm__ (\n"
     << "      R\"(\n";

  /// Saving General Purpose Registers.
  ss << "           // Allocate space for the register_context struct on the "
        "stack\n"
     << "           sub     sp, sp, #0x100\n"
     << "\n"
     << "           // Save registers into the allocated space\n"
     << "           stp     x0, x1, [sp, #0x00]\n"
     << "           stp     x2, x3, [sp, #0x10]\n"
     << "           stp     x4, x5, [sp, #0x20]\n"
     << "           stp     x6, x7, [sp, #0x30]\n"
     << "           stp     x8, x9, [sp, #0x40]\n"
     << "           stp     x10, x11, [sp, #0x50]\n"
     << "           stp     x12, x13, [sp, #0x60]\n"
     << "           stp     x14, x15, [sp, #0x70]\n"
     << "           stp     x16, x17, [sp, #0x80]\n"
     << "           stp     x18, x19, [sp, #0x90]\n"
     << "           stp     x20, x21, [sp, #0xa0]\n"
     << "           stp     x22, x23, [sp, #0xb0]\n"
     << "           stp     x24, x25, [sp, #0xc0]\n"
     << "           stp     x26, x27, [sp, #0xd0]\n"
     << "           stp     x28, x29, [sp, #0xe0]\n"
     << "           str     x30, [sp, #0xf0]\n"
     << "\n"
     << "           // Store the stack pointer value (before any allocation) "
        "at the end of the context structure\n"
     << "           mov     x1, sp\n"
     << "           add     x1, x1, #0x100\n"
     << "           str     x1, [sp, #0xf8]\n";

  /// Pass register context address to argument structure builder.
  /// Allocating argument structure on the stack.
  /// Pass argument structure address to argument structure builder.
  /// Call argument structure builder.
  /// No need to move return value to the right register.
  /// Call condition expression evaluator.
  /// Restore General Purpose Registers.

  const lldb::addr_t util_func_addr =
      bp_injected_site->GetUtilityFunctionAddress();
  const lldb::addr_t cond_expr_addr =
      bp_injected_site->GetConditionExpressionAddress();

  // AArch64 requires the stack pointer to stay 16 byte aligned for any sp
  // relative access, so reserve a rounded up amount rather than the exact size
  // of the argument structure.
  const uint64_t args_struct_size =
      llvm::alignTo<16>(bp_injected_site->GetArgsStructSize());

  ss << "           mov x0, sp\n"
     << "           sub sp, sp, #" << args_struct_size << "\n"
     << "           mov x1, sp\n"
     << "           ldr x17, =0x" << std::hex << util_func_addr << "\n"
     << "           blr x17\n"
     << "           ldr x17, =0x" << std::hex << cond_expr_addr << "\n"
     << "           blr x17\n"
     << "\n"
     << "           // Release the argument structure so that the offsets "
        "below "
        "are relative to the register_context frame again\n"
     << "           add sp, sp, #" << std::dec << args_struct_size << "\n"
     << "\n"
     << "           // Restore registers from the stack in reverse order\n"
     << "           ldr     x30, [sp, #0xf0]\n"
     << "           ldp     x28, x29, [sp, #0xe0]\n"
     << "           ldp     x26, x27, [sp, #0xd0]\n"
     << "           ldp     x24, x25, [sp, #0xc0]\n"
     << "           ldp     x22, x23, [sp, #0xb0]\n"
     << "           ldp     x20, x21, [sp, #0xa0]\n"
     << "           ldp     x18, x19, [sp, #0x90]\n"
     << "           ldp     x16, x17, [sp, #0x80]\n"
     << "           ldp     x14, x15, [sp, #0x70]\n"
     << "           ldp     x12, x13, [sp, #0x60]\n"
     << "           ldp     x10, x11, [sp, #0x50]\n"
     << "           ldp     x8, x9, [sp, #0x40]\n"
     << "           ldp     x6, x7, [sp, #0x30]\n"
     << "           ldp     x4, x5, [sp, #0x20]\n"
     << "           ldp     x2, x3, [sp, #0x10]\n"
     << "           ldp     x0, x1, [sp, #0x00]\n"
     << "\n"
     << "           // Free allocated stack memory for register_context "
        "structure. The saved sp at #0xf8 is only there for the condition "
        "checker to read, restoring it here would clobber x1.\n"
     << "           add     sp, sp, #0x100\n";

  /// Allocate space to copy inferior instructions and jump back to user's code
  ss << "";
  for (size_t slot = 0; slot < reserved_slots; ++slot)
    ss << "           nop\n";
  ss << "        )\");\n"
     << "}";

  ExecutionContext exe_ctx = bp_injected_site->GetOwnerExecutionContext();
  auto trampoline_instr = EmitAssembly("emit_trampoline", ss, exe_ctx);
  if (!trampoline_instr)
    return llvm::createStringError("couldn't assemble the trampoline");

  auto saved_instrs = process_sp->SaveInstructions(bp_addr, patch_size);
  if (!saved_instrs) {
    return llvm::createStringError(
        "Couldn't save the instructions displaced by the branch");
  }

  // The slots reserved at the end of the trampoline hold exactly the relocated
  // form of what was displaced and the branch back, so saving a different
  // amount than the patch displaces would leave one of them wrong.
  if (saved_instrs->GetByteSize() != plan->displaced_size) {
    return llvm::createStringError(
        llvm::formatv("Expected {0} bytes of displaced instructions, saved {1}",
                      plan->displaced_size, saved_instrs->GetByteSize()));
  }

  // Everything from the reservation on is written into it, so a trampoline that
  // outgrew it has to be refused rather than truncated. The reservation is a
  // page and a trampoline is a few hundred bytes, so this is a guard on the
  // code above changing out from under the constant, not a case to expect.
  const size_t trampoline_size = trampoline_instr->GetByteSize();
  if (trampoline_size > aarch64_trampoline_reservation) {
    return llvm::createStringError(llvm::formatv(
        "the assembled trampoline needs {0} bytes, more than the {1} reserved "
        "for it",
        trampoline_size, aarch64_trampoline_reservation));
  }

  const auto &trampoline_buffer = trampoline_instr->GetData();

  /// Run copied instructions and jump back to user's code. The slots are the
  /// two trailing nops emitted above; locating them by scanning keeps this
  /// independent of how many instructions the assembler produced for the code
  /// before them.
  const lldb::offset_t copied_instr_offset =
      FindTrampolineNopSlots(trampoline_buffer, reserved_slots);

  if (copied_instr_offset == LLDB_INVALID_OFFSET) {
    return llvm::createStringError(
        "Couldn't find the reserved nop slots in the trampoline");
  }

  // `b <imm>` assembles the immediate as a PC-relative byte offset, so the
  // offset has to be measured from where the instruction ends up in the
  // trampoline, not from wherever the assembler happened to lay it out. The
  // branch back occupies the slots after the relocated code and resumes at the
  // instruction following the last one the patch overwrote.
  //
  // Signed throughout: every term is an unsigned type, so computing this in
  // their arithmetic and only then storing it signed relies on wraparound.
  const int64_t source_resume_addr =
      static_cast<int64_t>(bp_load_addr) + static_cast<int64_t>(patch_size);
  // The branch back sits after the whole relocated form, which is more than one
  // instruction when a displaced instruction had to be rewritten as a sequence.
  // Measuring from the wrong slot would land the inferior an instruction away
  // from where it left off.
  const int64_t source_branch_addr =
      static_cast<int64_t>(trampoline_addr) +
      static_cast<int64_t>(copied_instr_offset) +
      static_cast<int64_t>(plan->relocated_code_size);
  const int64_t source_branch_target = source_resume_addr - source_branch_addr;

  // `b` reaches +/-128MiB either way, so a direct patch has to be checked in
  // both directions. The wider form was chosen precisely because nothing was
  // free that close, so it is checked against its own reach where it is
  // encoded, not here.
  if (direct && (!IsBranchInRange(source_branch_target) ||
                 !IsBranchInRange(static_cast<int64_t>(trampoline_addr) -
                                  static_cast<int64_t>(bp_load_addr)))) {
    return llvm::createStringError(llvm::formatv(
        "the trampoline at {0:x} is out of branch range of the site at {1:x}",
        trampoline_addr, bp_load_addr));
  }

  /// Put the displaced instructions in the first reserved slots, then the
  /// branch back to the instruction after the last one they came from.
  ///
  /// Relocate() rather than a copy: an instruction that refers to its own
  /// address, a branch for instance, means something different from the
  /// trampoline and has to be rewritten to keep referring to what it did. For
  /// anything position independent this hands back the original bytes.
  ///
  /// A reference that lands inside the displaced range would have to be
  /// redirected to wherever the copy of that instruction went, which is not
  /// done here because nothing gets this far: CanPatch() refuses a site
  /// anything branches into, including the displaced instructions themselves.
  llvm::SmallVector<uint8_t, 32> relocated;
  {
    lldb::addr_t from = bp_load_addr;
    lldb::addr_t slot_addr = trampoline_addr + copied_instr_offset;

    for (const InstructionSP &displaced : plan->displaced_instructions) {
      llvm::SmallVector<uint8_t, 8> bytes;
      const lldb::addr_t referenced =
          displaced->GetReferencedAddress(from).value_or(LLDB_INVALID_ADDRESS);

      if (llvm::Error error =
              displaced->Relocate(from, slot_addr, referenced, bytes))
        return error;

      from += displaced->GetOpcode().GetByteSize();
      slot_addr += bytes.size();
      relocated.append(bytes.begin(), bytes.end());
    }

    // Reserving and emitting have to agree, or the branch back would land
    // inside the relocated sequence or leave a nop running as code.
    if (relocated.size() != plan->relocated_code_size)
      return llvm::createStringError(llvm::formatv(
          "relocating the instructions displaced at {0:x} produced {1} bytes "
          "but {2} were reserved",
          bp_load_addr, relocated.size(), plan->relocated_code_size));
  }

  std::memcpy(&trampoline_buffer[copied_instr_offset], relocated.data(),
              relocated.size());
  uint8_t *branch_back =
      &trampoline_buffer[copied_instr_offset + relocated.size()];
  if (direct)
    EncodeAArch64Branch(source_branch_target, branch_back);
  else
    EncodeAArch64FarBranchAbsolute(static_cast<addr_t>(source_resume_addr),
                                   scratch->number, branch_back);

  // The trampoline is only registered as a module once everything below
  // succeeds, so this is the one chance to see what was built.
  LogTrampolineDisassembly(trampoline_buffer, trampoline_addr);

  size_t written_bytes = process_sp->WriteMemory(
      trampoline_addr, trampoline_buffer.data(), trampoline_size, error);
  if (written_bytes != trampoline_size || error.Fail()) {
    return llvm::createStringError(
        "Couldn't write trampoline buffer to inferior");
  }

  /// Patch inferior to branch to trampoline
  uint8_t trampoline_branch[aarch64_far_patch_size];
  if (direct) {
    const int64_t trampoline_branch_target =
        static_cast<int64_t>(trampoline_addr) -
        static_cast<int64_t>(bp_load_addr);
    EncodeAArch64Branch(trampoline_branch_target, trampoline_branch);
  } else if (llvm::Error error =
                 EncodeAArch64FarBranch(bp_load_addr, trampoline_addr,
                                        scratch->number, trampoline_branch)) {
    return error;
  }

  written_bytes = process_sp->WriteMemory(bp_load_addr, trampoline_branch,
                                          patch_size, error);
  if (written_bytes != patch_size || error.Fail()) {
    return llvm::createStringError(
        "Couldn't patch inferior with branch to trampoline");
  }

  // From here on the inferior is modified, so the site owns the job of putting
  // the original instruction back. Any failure below destroys the site, which
  // undoes the patch.
  bp_injected_site->SetPatchedInstructions(
      bp_load_addr,
      std::make_shared<DataBufferHeap>(trampoline_branch, patch_size),
      saved_instrs);

  // What the trampoline subtracts from sp before calling the condition checker:
  // the register_context frame plus the argument structure. The unwinder needs
  // this to recover the stack pointer of the patched function, which the branch
  // into the trampoline left untouched.
  const size_t trampoline_frame_size =
      aarch64_register_context_size + args_struct_size;

  lldb::ModuleSP trampoline_module_sp =
      CreateModuleForFastConditionalBreakpointTrampoline(
          trampoline_addr, trampoline_size, bp_load_addr,
          trampoline_frame_size);

  if (!trampoline_module_sp) {
    return llvm::createStringError("Couldn't get trampoline module");
  }

  ModuleList &images = target_sp->GetImages();
  size_t image_count = images.GetSize();

  images.Append(trampoline_module_sp);

  if (images.GetSize() == image_count) {
    return llvm::createStringError(
        "Couldn't add trampoline module to image list");
  }

  bp_injected_site->SetTrampolineModule(trampoline_module_sp);

  return llvm::Error::success();
}

size_t ABIMacOSX_arm64::GetRedZoneSize() const { return 128; }

// Static Functions

ABISP
ABIMacOSX_arm64::CreateInstance(ProcessSP process_sp, const ArchSpec &arch) {
  const llvm::Triple::ArchType arch_type = arch.GetTriple().getArch();
  const llvm::Triple::VendorType vendor_type = arch.GetTriple().getVendor();

  if (vendor_type == llvm::Triple::Apple) {
    if (arch_type == llvm::Triple::aarch64 ||
        arch_type == llvm::Triple::aarch64_32) {
      return ABISP(
          new ABIMacOSX_arm64(std::move(process_sp), MakeMCRegisterInfo(arch)));
    }
  }

  return ABISP();
}

bool ABIMacOSX_arm64::PrepareTrivialCall(
    Thread &thread, lldb::addr_t sp, lldb::addr_t func_addr,
    lldb::addr_t return_addr, llvm::ArrayRef<lldb::addr_t> args) const {
  RegisterContext *reg_ctx = thread.GetRegisterContext().get();
  if (!reg_ctx)
    return false;

  Log *log = GetLog(LLDBLog::Expressions);

  if (log) {
    StreamString s;
    s.Printf("ABIMacOSX_arm64::PrepareTrivialCall (tid = 0x%" PRIx64
             ", sp = 0x%" PRIx64 ", func_addr = 0x%" PRIx64
             ", return_addr = 0x%" PRIx64,
             thread.GetID(), (uint64_t)sp, (uint64_t)func_addr,
             (uint64_t)return_addr);

    for (size_t i = 0; i < args.size(); ++i)
      s.Printf(", arg%d = 0x%" PRIx64, static_cast<int>(i + 1), args[i]);
    s.PutCString(")");
    log->PutString(s.GetString());
  }

  const uint32_t pc_reg_num = reg_ctx->ConvertRegisterKindToRegisterNumber(
      eRegisterKindGeneric, LLDB_REGNUM_GENERIC_PC);
  const uint32_t sp_reg_num = reg_ctx->ConvertRegisterKindToRegisterNumber(
      eRegisterKindGeneric, LLDB_REGNUM_GENERIC_SP);
  const uint32_t ra_reg_num = reg_ctx->ConvertRegisterKindToRegisterNumber(
      eRegisterKindGeneric, LLDB_REGNUM_GENERIC_RA);

  // x0 - x7 contain first 8 simple args
  if (args.size() > 8) // TODO handle more than 8 arguments
    return false;

  for (size_t i = 0; i < args.size(); ++i) {
    const RegisterInfo *reg_info = reg_ctx->GetRegisterInfo(
        eRegisterKindGeneric, LLDB_REGNUM_GENERIC_ARG1 + i);
    LLDB_LOGF(log, "About to write arg%d (0x%" PRIx64 ") into %s",
              static_cast<int>(i + 1), args[i], reg_info->name);
    if (!reg_ctx->WriteRegisterFromUnsigned(reg_info, args[i]))
      return false;
  }

  // Set "lr" to the return address
  if (!reg_ctx->WriteRegisterFromUnsigned(
          reg_ctx->GetRegisterInfoAtIndex(ra_reg_num), return_addr))
    return false;

  // Set "sp" to the requested value
  if (!reg_ctx->WriteRegisterFromUnsigned(
          reg_ctx->GetRegisterInfoAtIndex(sp_reg_num), sp))
    return false;

  // Set "pc" to the address requested
  if (!reg_ctx->WriteRegisterFromUnsigned(
          reg_ctx->GetRegisterInfoAtIndex(pc_reg_num), func_addr))
    return false;

  return true;
}

bool ABIMacOSX_arm64::GetArgumentValues(Thread &thread,
                                        ValueList &values) const {
  uint32_t num_values = values.GetSize();

  ExecutionContext exe_ctx(thread.shared_from_this());

  // Extract the register context so we can read arguments from registers

  RegisterContext *reg_ctx = thread.GetRegisterContext().get();

  if (!reg_ctx)
    return false;

  addr_t sp = 0;

  for (uint32_t value_idx = 0; value_idx < num_values; ++value_idx) {
    // We currently only support extracting values with Clang QualTypes. Do we
    // care about others?
    Value *value = values.GetValueAtIndex(value_idx);

    if (!value)
      return false;

    CompilerType value_type = value->GetCompilerType();
    std::optional<uint64_t> bit_size =
        llvm::expectedToOptional(value_type.GetBitSize(&thread));
    if (!bit_size)
      return false;

    bool is_signed = false;
    size_t bit_width = 0;
    if (value_type.IsIntegerOrEnumerationType(is_signed)) {
      bit_width = *bit_size;
    } else if (value_type.IsPointerOrReferenceType()) {
      bit_width = *bit_size;
    } else {
      // We only handle integer, pointer and reference types currently...
      return false;
    }

    if (bit_width <= (exe_ctx.GetProcessRef().GetAddressByteSize() * 8)) {
      if (value_idx < 8) {
        // Arguments 1-6 are in x0-x5...
        const RegisterInfo *reg_info = nullptr;
        // Search by generic ID first, then fall back to by name
        uint32_t arg_reg_num = reg_ctx->ConvertRegisterKindToRegisterNumber(
            eRegisterKindGeneric, LLDB_REGNUM_GENERIC_ARG1 + value_idx);
        if (arg_reg_num != LLDB_INVALID_REGNUM) {
          reg_info = reg_ctx->GetRegisterInfoAtIndex(arg_reg_num);
        } else {
          switch (value_idx) {
          case 0:
            reg_info = reg_ctx->GetRegisterInfoByName("x0");
            break;
          case 1:
            reg_info = reg_ctx->GetRegisterInfoByName("x1");
            break;
          case 2:
            reg_info = reg_ctx->GetRegisterInfoByName("x2");
            break;
          case 3:
            reg_info = reg_ctx->GetRegisterInfoByName("x3");
            break;
          case 4:
            reg_info = reg_ctx->GetRegisterInfoByName("x4");
            break;
          case 5:
            reg_info = reg_ctx->GetRegisterInfoByName("x5");
            break;
          case 6:
            reg_info = reg_ctx->GetRegisterInfoByName("x6");
            break;
          case 7:
            reg_info = reg_ctx->GetRegisterInfoByName("x7");
            break;
          }
        }

        if (reg_info) {
          RegisterValue reg_value;

          if (reg_ctx->ReadRegister(reg_info, reg_value)) {
            if (is_signed)
              reg_value.SignExtend(bit_width);
            if (!reg_value.GetScalarValue(value->GetScalar()))
              return false;
            continue;
          }
        }
        return false;
      } else {
        if (sp == 0) {
          // Read the stack pointer if we already haven't read it
          sp = reg_ctx->GetSP(0);
          if (sp == 0)
            return false;
        }

        // Arguments 5 on up are on the stack
        const uint32_t arg_byte_size = (bit_width + (8 - 1)) / 8;
        Status error;
        if (!exe_ctx.GetProcessRef().ReadScalarIntegerFromMemory(
                sp, arg_byte_size, is_signed, value->GetScalar(), error))
          return false;

        sp += arg_byte_size;
        // Align up to the next 8 byte boundary if needed
        if (sp % 8) {
          sp >>= 3;
          sp += 1;
          sp <<= 3;
        }
      }
    }
  }
  return true;
}

Status
ABIMacOSX_arm64::SetReturnValueObject(lldb::StackFrameSP &frame_sp,
                                      lldb::ValueObjectSP &new_value_sp) {
  Status error;
  if (!new_value_sp) {
    error = Status::FromErrorString("Empty value object for return value.");
    return error;
  }

  CompilerType return_value_type = new_value_sp->GetCompilerType();
  if (!return_value_type) {
    error = Status::FromErrorString("Null clang type for return value.");
    return error;
  }

  Thread *thread = frame_sp->GetThread().get();

  RegisterContext *reg_ctx = thread->GetRegisterContext().get();

  if (reg_ctx) {
    DataExtractor data;
    Status data_error;
    const uint64_t byte_size = new_value_sp->GetData(data, data_error);
    if (data_error.Fail()) {
      error = Status::FromErrorStringWithFormat(
          "Couldn't convert return value to raw data: %s",
          data_error.AsCString());
      return error;
    }

    const uint32_t type_flags = return_value_type.GetTypeInfo(nullptr);
    if (type_flags & eTypeIsScalar || type_flags & eTypeIsPointer) {
      if (type_flags & eTypeIsInteger || type_flags & eTypeIsPointer) {
        // Extract the register context so we can read arguments from registers
        lldb::offset_t offset = 0;
        if (byte_size <= 16) {
          const RegisterInfo *x0_info = reg_ctx->GetRegisterInfoByName("x0", 0);
          if (byte_size <= 8) {
            uint64_t raw_value = data.GetMaxU64(&offset, byte_size);

            if (!reg_ctx->WriteRegisterFromUnsigned(x0_info, raw_value))
              error = Status::FromErrorString("failed to write register x0");
          } else {
            uint64_t raw_value = data.GetMaxU64(&offset, 8);

            if (reg_ctx->WriteRegisterFromUnsigned(x0_info, raw_value)) {
              const RegisterInfo *x1_info =
                  reg_ctx->GetRegisterInfoByName("x1", 0);
              raw_value = data.GetMaxU64(&offset, byte_size - offset);

              if (!reg_ctx->WriteRegisterFromUnsigned(x1_info, raw_value))
                error = Status::FromErrorString("failed to write register x1");
            }
          }
        } else {
          error = Status::FromErrorString(
              "We don't support returning longer than 128 bit "
              "integer values at present.");
        }
      } else if (type_flags & eTypeIsFloat) {
        if (type_flags & eTypeIsComplex) {
          // Don't handle complex yet.
          error = Status::FromErrorString(
              "returning complex float values are not supported");
        } else {
          const RegisterInfo *v0_info = reg_ctx->GetRegisterInfoByName("v0", 0);

          if (v0_info) {
            if (byte_size <= 16) {
              RegisterValue reg_value;
              error = reg_value.SetValueFromData(*v0_info, data, 0, true);
              if (error.Success())
                if (!reg_ctx->WriteRegister(v0_info, reg_value))
                  error =
                      Status::FromErrorString("failed to write register v0");
            } else {
              error = Status::FromErrorString(
                  "returning float values longer than 128 "
                  "bits are not supported");
            }
          } else
            error = Status::FromErrorString(
                "v0 register is not available on this target");
        }
      }
    } else if (type_flags & eTypeIsVector) {
      if (byte_size > 0) {
        const RegisterInfo *v0_info = reg_ctx->GetRegisterInfoByName("v0", 0);

        if (v0_info) {
          if (byte_size <= v0_info->byte_size) {
            RegisterValue reg_value;
            error = reg_value.SetValueFromData(*v0_info, data, 0, true);
            if (error.Success()) {
              if (!reg_ctx->WriteRegister(v0_info, reg_value))
                error = Status::FromErrorString("failed to write register v0");
            }
          }
        }
      }
    }
  } else {
    error = Status::FromErrorString("no registers are available");
  }

  return error;
}

lldb::UnwindPlanSP
ABIMacOSX_arm64::CreateTrampolineUnwindPlan(addr_t site_address,
                                            size_t frame_size) {
  uint32_t pc_reg_num = arm64_dwarf::pc;
  uint32_t sp_reg_num = arm64_dwarf::sp;

  UnwindPlan::Row row;

  // The trampoline is branched to rather than called, so the stack pointer of
  // the function it left is exactly frame_size above the current one: nothing
  // pushed a return address. That value is this frame's CFA.
  row.GetCFAValue().SetIsRegisterPlusOffset(sp_reg_num, frame_size);
  row.SetOffset(0);

  // The single row is calibrated for the only place execution can stop, inside
  // the condition checker, where the whole frame has been built. Earlier in the
  // trampoline the stack pointer is higher and this row does not describe it.

  // A frame's CFA is defined to be its caller's stack pointer.
  row.SetRegisterLocationToIsCFAPlusOffset(sp_reg_num, 0, true);

  // Where the trampoline's prologue put each of the patched function's
  // registers. It stores x0 through x30 as consecutive doublewords at the base
  // of the register context, which sits aarch64_register_context_size below the
  // CFA, so xN is at CFA - context_size + 8N.
  //
  // Describing them is not optional. Without this the caller's callee-saved
  // registers are undefined, and a function whose own CFA rule is fp relative,
  // which is every unoptimized one, then cannot be unwound out of: the
  // backtrace stops at the patched function instead of continuing past it.
  for (uint32_t reg = arm64_dwarf::x0; reg <= arm64_dwarf::lr; ++reg) {
    const int32_t offset = static_cast<int32_t>(reg * aarch64_gpr_size) -
                           static_cast<int32_t>(aarch64_register_context_size);
    row.SetRegisterLocationToAtCFAPlusOffset(reg, offset, true);
  }

  // Report the patched address itself rather than where the trampoline will
  // resume: the displaced instruction has not run yet. The frame is marked as a
  // trap below so that the unwinder does not back the address up by one the way
  // it would for a return address.
  //
  // Set after the loop, because lr is one of the registers above and a caller's
  // pc is not its lr here: the trampoline was branched to, not called, so the
  // saved lr belongs to the frame above the patched function rather than to it.
  row.SetRegisterLocationToConstantValue(pc_reg_num, site_address, true);

  auto plan_sp = std::make_shared<UnwindPlan>(eRegisterKindDWARF);
  plan_sp->AppendRow(row);
  plan_sp->SetSourceName("arm64-apple-darwin trampoline unwind plan");
  plan_sp->SetSourcedFromCompiler(eLazyBoolNo);
  plan_sp->SetUnwindPlanValidAtAllInstructions(eLazyBoolNo);
  plan_sp->SetUnwindPlanForSignalTrap(eLazyBoolYes);
  return plan_sp;
}

// AAPCS64 (Procedure Call Standard for the ARM 64-bit Architecture) says
// registers x19 through x28 and sp are callee preserved. v8-v15 are non-
// volatile (and specifically only the lower 8 bytes of these regs), the rest
// of the fp/SIMD registers are volatile.
//
// v. https://github.com/ARM-software/abi-aa/blob/main/aapcs64/

// We treat x29 as callee preserved also, else the unwinder won't try to
// retrieve fp saves.

/// AAPCS64 reserves x9 through x15 as caller-saved temporaries and x16, x17 as
/// the intra-procedure-call registers, and none of the nine conveys anything
/// across a call boundary: a linker veneer is allowed to destroy IP0 and IP1
/// between a caller and the callee it reaches, so no callee may read them and
/// no caller may expect them back.
///
/// Everything else is excluded for a reason. x0 through x7 carry arguments and
/// x0 through x1 carry results, so a callee or a caller reads them even though
/// they are caller saved. x8 is the indirect result location register. x18 is
/// reserved by the platform on Darwin, and is reported volatile by
/// RegisterIsVolatile even though nothing may rely on it, which is exactly the
/// trap this predicate exists to avoid.
bool ABIMacOSX_arm64::RegisterIsPureTemporary(llvm::StringRef reg_name) {
  // Accept both widths: the disassembler names the 32 bit view of a register
  // when the instruction writes it, and the two views are one register.
  if (reg_name.size() < 2 || (reg_name[0] != 'x' && reg_name[0] != 'w'))
    return false;

  uint32_t num = 0;
  if (reg_name.drop_front().getAsInteger(10, num))
    return false;

  return num >= 9 && num <= 17;
}

bool ABIMacOSX_arm64::RegisterIsVolatile(const RegisterInfo *reg_info) {
  if (reg_info) {
    const char *name = reg_info->name;

    // Sometimes we'll be called with the "alternate" name for these registers;
    // recognize them as non-volatile.

    if (name[0] == 'p' && name[1] == 'c') // pc
      return false;
    if (name[0] == 'f' && name[1] == 'p') // fp
      return false;
    if (name[0] == 's' && name[1] == 'p') // sp
      return false;
    if (name[0] == 'l' && name[1] == 'r') // lr
      return false;

    if (name[0] == 'x') {
      // Volatile registers: x0-x18, x30 (lr)
      // Return false for the non-volatile gpr regs, true for everything else
      switch (name[1]) {
      case '1':
        switch (name[2]) {
        case '9':
          return false; // x19 is non-volatile
        default:
          return true;
        }
        break;
      case '2':
        switch (name[2]) {
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
          return false; // x20 - 28 are non-volatile
        case '9':
          return false; // x29 aka fp treat as non-volatile on Darwin
        default:
          return true;
        }
      case '3': // x30 aka lr treat as non-volatile
        if (name[2] == '0')
          return false;
        break;
      default:
        return true;
      }
    } else if (name[0] == 'v' || name[0] == 's' || name[0] == 'd') {
      // Volatile registers: v0-7, v16-v31
      // Return false for non-volatile fp/SIMD regs, true for everything else
      switch (name[1]) {
      case '8':
      case '9':
        return false; // v8-v9 are non-volatile
      case '1':
        switch (name[2]) {
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
          return false; // v10-v15 are non-volatile
        default:
          return true;
        }
      default:
        return true;
      }
    }
  }
  return true;
}

static bool LoadValueFromConsecutiveGPRRegisters(
    ExecutionContext &exe_ctx, RegisterContext *reg_ctx,
    const CompilerType &value_type,
    bool is_return_value, // false => parameter, true => return value
    uint32_t &NGRN,       // NGRN (see ABI documentation)
    uint32_t &NSRN,       // NSRN (see ABI documentation)
    DataExtractor &data) {
  std::optional<uint64_t> byte_size = llvm::expectedToOptional(
      value_type.GetByteSize(exe_ctx.GetBestExecutionContextScope()));
  if (!byte_size || *byte_size == 0)
    return false;

  std::unique_ptr<DataBufferHeap> heap_data_up(
      new DataBufferHeap(*byte_size, 0));
  const ByteOrder byte_order = exe_ctx.GetProcessRef().GetByteOrder();
  Status error;

  CompilerType base_type;
  const uint32_t homogeneous_count =
      value_type.IsHomogeneousAggregate(&base_type);
  if (homogeneous_count > 0 && homogeneous_count <= 8) {
    // Make sure we have enough registers
    if (NSRN < 8 && (8 - NSRN) >= homogeneous_count) {
      if (!base_type)
        return false;
      std::optional<uint64_t> base_byte_size = llvm::expectedToOptional(
          base_type.GetByteSize(exe_ctx.GetBestExecutionContextScope()));
      if (!base_byte_size)
        return false;
      uint32_t data_offset = 0;

      for (uint32_t i = 0; i < homogeneous_count; ++i) {
        char v_name[8];
        ::snprintf(v_name, sizeof(v_name), "v%u", NSRN);
        const RegisterInfo *reg_info =
            reg_ctx->GetRegisterInfoByName(v_name, 0);
        if (reg_info == nullptr)
          return false;

        if (*base_byte_size > reg_info->byte_size)
          return false;

        RegisterValue reg_value;

        if (!reg_ctx->ReadRegister(reg_info, reg_value))
          return false;

        // Make sure we have enough room in "heap_data_up"
        if ((data_offset + *base_byte_size) <= heap_data_up->GetByteSize()) {
          const size_t bytes_copied = reg_value.GetAsMemoryData(
              *reg_info, heap_data_up->GetBytes() + data_offset,
              *base_byte_size, byte_order, error);
          if (bytes_copied != *base_byte_size)
            return false;
          data_offset += bytes_copied;
          ++NSRN;
        } else
          return false;
      }
      data.SetByteOrder(byte_order);
      data.SetAddressByteSize(exe_ctx.GetProcessRef().GetAddressByteSize());
      data.SetData(DataBufferSP(heap_data_up.release()));
      return true;
    }
  }

  const size_t max_reg_byte_size = 16;
  if (*byte_size <= max_reg_byte_size) {
    size_t bytes_left = *byte_size;
    uint32_t data_offset = 0;
    while (data_offset < *byte_size) {
      if (NGRN >= 8)
        return false;

      uint32_t reg_num = reg_ctx->ConvertRegisterKindToRegisterNumber(
          eRegisterKindGeneric, LLDB_REGNUM_GENERIC_ARG1 + NGRN);
      if (reg_num == LLDB_INVALID_REGNUM)
        return false;

      const RegisterInfo *reg_info = reg_ctx->GetRegisterInfoAtIndex(reg_num);
      if (reg_info == nullptr)
        return false;

      RegisterValue reg_value;

      if (!reg_ctx->ReadRegister(reg_info, reg_value))
        return false;

      const size_t curr_byte_size = std::min<size_t>(8, bytes_left);
      const size_t bytes_copied = reg_value.GetAsMemoryData(
          *reg_info, heap_data_up->GetBytes() + data_offset, curr_byte_size,
          byte_order, error);
      if (bytes_copied == 0)
        return false;
      if (bytes_copied >= bytes_left)
        break;
      data_offset += bytes_copied;
      bytes_left -= bytes_copied;
      ++NGRN;
    }
  } else {
    const RegisterInfo *reg_info = nullptr;
    if (is_return_value) {
      // The Darwin arm64 ABI doesn't write the return location back to x8
      // before returning from the function the way the x86_64 ABI does.  So
      // we can't reconstruct stack based returns on exit from the function:
      return false;
    } else {
      // We are assuming we are stopped at the first instruction in a function
      // and that the ABI is being respected so all parameters appear where
      // they should be (functions with no external linkage can legally violate
      // the ABI).
      if (NGRN >= 8)
        return false;

      uint32_t reg_num = reg_ctx->ConvertRegisterKindToRegisterNumber(
          eRegisterKindGeneric, LLDB_REGNUM_GENERIC_ARG1 + NGRN);
      if (reg_num == LLDB_INVALID_REGNUM)
        return false;
      reg_info = reg_ctx->GetRegisterInfoAtIndex(reg_num);
      if (reg_info == nullptr)
        return false;
      ++NGRN;
    }

    const lldb::addr_t value_addr =
        reg_ctx->ReadRegisterAsUnsigned(reg_info, LLDB_INVALID_ADDRESS);

    if (value_addr == LLDB_INVALID_ADDRESS)
      return false;

    if (exe_ctx.GetProcessRef().ReadMemory(
            value_addr, heap_data_up->GetBytes(), heap_data_up->GetByteSize(),
            error) != heap_data_up->GetByteSize()) {
      return false;
    }
  }

  data.SetByteOrder(byte_order);
  data.SetAddressByteSize(exe_ctx.GetProcessRef().GetAddressByteSize());
  data.SetData(DataBufferSP(heap_data_up.release()));
  return true;
}

ValueObjectSP ABIMacOSX_arm64::GetReturnValueObjectImpl(
    Thread &thread, CompilerType &return_compiler_type) const {
  ValueObjectSP return_valobj_sp;
  Value value;

  ExecutionContext exe_ctx(thread.shared_from_this());
  if (exe_ctx.GetTargetPtr() == nullptr || exe_ctx.GetProcessPtr() == nullptr)
    return return_valobj_sp;

  // value.SetContext (Value::eContextTypeClangType, return_compiler_type);
  value.SetCompilerType(return_compiler_type);

  RegisterContext *reg_ctx = thread.GetRegisterContext().get();
  if (!reg_ctx)
    return return_valobj_sp;

  std::optional<uint64_t> byte_size =
      llvm::expectedToOptional(return_compiler_type.GetByteSize(&thread));
  if (!byte_size)
    return return_valobj_sp;

  const uint32_t type_flags = return_compiler_type.GetTypeInfo(nullptr);
  if (type_flags & eTypeIsScalar || type_flags & eTypeIsPointer) {
    value.SetValueType(Value::ValueType::Scalar);

    bool success = false;
    if (type_flags & eTypeIsInteger || type_flags & eTypeIsPointer) {
      // Extract the register context so we can read arguments from registers
      if (*byte_size <= 8) {
        const RegisterInfo *x0_reg_info =
            reg_ctx->GetRegisterInfoByName("x0", 0);
        if (x0_reg_info) {
          uint64_t raw_value =
              thread.GetRegisterContext()->ReadRegisterAsUnsigned(x0_reg_info,
                                                                  0);
          const bool is_signed = (type_flags & eTypeIsSigned) != 0;
          switch (*byte_size) {
          default:
            break;
          case 16: // uint128_t
            // In register x0 and x1
            {
              const RegisterInfo *x1_reg_info =
                  reg_ctx->GetRegisterInfoByName("x1", 0);

              if (x1_reg_info) {
                if (*byte_size <=
                    x0_reg_info->byte_size + x1_reg_info->byte_size) {
                  std::unique_ptr<DataBufferHeap> heap_data_up(
                      new DataBufferHeap(*byte_size, 0));
                  const ByteOrder byte_order =
                      exe_ctx.GetProcessRef().GetByteOrder();
                  RegisterValue x0_reg_value;
                  RegisterValue x1_reg_value;
                  if (reg_ctx->ReadRegister(x0_reg_info, x0_reg_value) &&
                      reg_ctx->ReadRegister(x1_reg_info, x1_reg_value)) {
                    Status error;
                    if (x0_reg_value.GetAsMemoryData(
                            *x0_reg_info, heap_data_up->GetBytes() + 0, 8,
                            byte_order, error) &&
                        x1_reg_value.GetAsMemoryData(
                            *x1_reg_info, heap_data_up->GetBytes() + 8, 8,
                            byte_order, error)) {
                      DataExtractor data(
                          DataBufferSP(heap_data_up.release()), byte_order,
                          exe_ctx.GetProcessRef().GetAddressByteSize());

                      return_valobj_sp = ValueObjectConstResult::Create(
                          &thread, return_compiler_type, ConstString(""), data);
                      return return_valobj_sp;
                    }
                  }
                }
              }
            }
            break;
          case sizeof(uint64_t):
            if (is_signed)
              value.GetScalar() = (int64_t)(raw_value);
            else
              value.GetScalar() = (uint64_t)(raw_value);
            success = true;
            break;

          case sizeof(uint32_t):
            if (is_signed)
              value.GetScalar() = (int32_t)(raw_value & UINT32_MAX);
            else
              value.GetScalar() = (uint32_t)(raw_value & UINT32_MAX);
            success = true;
            break;

          case sizeof(uint16_t):
            if (is_signed)
              value.GetScalar() = (int16_t)(raw_value & UINT16_MAX);
            else
              value.GetScalar() = (uint16_t)(raw_value & UINT16_MAX);
            success = true;
            break;

          case sizeof(uint8_t):
            if (is_signed)
              value.GetScalar() = (int8_t)(raw_value & UINT8_MAX);
            else
              value.GetScalar() = (uint8_t)(raw_value & UINT8_MAX);
            success = true;
            break;
          }
        }
      }
    } else if (type_flags & eTypeIsFloat) {
      if (type_flags & eTypeIsComplex) {
        // Don't handle complex yet.
      } else {
        if (*byte_size <= sizeof(long double)) {
          const RegisterInfo *v0_reg_info =
              reg_ctx->GetRegisterInfoByName("v0", 0);
          RegisterValue v0_value;
          if (reg_ctx->ReadRegister(v0_reg_info, v0_value)) {
            DataExtractor data;
            if (v0_value.GetData(data)) {
              lldb::offset_t offset = 0;
              if (*byte_size == sizeof(float)) {
                value.GetScalar() = data.GetFloat(&offset);
                success = true;
              } else if (*byte_size == sizeof(double)) {
                value.GetScalar() = data.GetDouble(&offset);
                success = true;
              } else if (*byte_size == sizeof(long double)) {
                value.GetScalar() = data.GetLongDouble(&offset);
                success = true;
              }
            }
          }
        }
      }
    }

    if (success)
      return_valobj_sp = ValueObjectConstResult::Create(
          thread.GetStackFrameAtIndex(0).get(), value, ConstString(""));
  } else if (type_flags & eTypeIsVector) {
    if (*byte_size > 0) {

      const RegisterInfo *v0_info = reg_ctx->GetRegisterInfoByName("v0", 0);

      if (v0_info) {
        if (*byte_size <= v0_info->byte_size) {
          std::unique_ptr<DataBufferHeap> heap_data_up(
              new DataBufferHeap(*byte_size, 0));
          const ByteOrder byte_order = exe_ctx.GetProcessRef().GetByteOrder();
          RegisterValue reg_value;
          if (reg_ctx->ReadRegister(v0_info, reg_value)) {
            Status error;
            if (reg_value.GetAsMemoryData(*v0_info, heap_data_up->GetBytes(),
                                          heap_data_up->GetByteSize(),
                                          byte_order, error)) {
              DataExtractor data(DataBufferSP(heap_data_up.release()),
                                 byte_order,
                                 exe_ctx.GetProcessRef().GetAddressByteSize());
              return_valobj_sp = ValueObjectConstResult::Create(
                  &thread, return_compiler_type, ConstString(""), data);
            }
          }
        }
      }
    }
  } else if (type_flags & eTypeIsStructUnion || type_flags & eTypeIsClass) {
    DataExtractor data;

    uint32_t NGRN = 0; // Search ABI docs for NGRN
    uint32_t NSRN = 0; // Search ABI docs for NSRN
    const bool is_return_value = true;
    if (LoadValueFromConsecutiveGPRRegisters(
            exe_ctx, reg_ctx, return_compiler_type, is_return_value, NGRN, NSRN,
            data)) {
      return_valobj_sp = ValueObjectConstResult::Create(
          &thread, return_compiler_type, ConstString(""), data);
    }
  }
  return return_valobj_sp;
}

constexpr addr_t tbi_mask = 0xff80000000000000ULL;
constexpr addr_t pac_sign_extension = 0x0080000000000000ULL;

/// Consults the process for its {code, data} address masks and applies it to
/// `addr`.
static addr_t DoFixAddr(addr_t addr, bool is_code, ProcessSP process_sp) {
  if (!process_sp)
    return addr;

  addr_t mask = is_code ? process_sp->GetCodeAddressMask()
                        : process_sp->GetDataAddressMask();
  if (mask == LLDB_INVALID_ADDRESS_MASK)
    mask = tbi_mask;

  if (addr & pac_sign_extension) {
    addr_t highmem_mask = is_code ? process_sp->GetHighmemCodeAddressMask()
                                  : process_sp->GetHighmemDataAddressMask();
    if (highmem_mask != LLDB_INVALID_ADDRESS_MASK)
      return addr | highmem_mask;
    return addr | mask;
  }

  return addr & (~mask);
}

addr_t ABIMacOSX_arm64::FixCodeAddress(addr_t pc) {
  ProcessSP process_sp = GetProcessSP();
  return DoFixAddr(pc, true /*is_code*/, GetProcessSP());
}

addr_t ABIMacOSX_arm64::FixDataAddress(addr_t addr) {
  ProcessSP process_sp = GetProcessSP();
  return DoFixAddr(addr, false /*is_code*/, GetProcessSP());
}

void ABIMacOSX_arm64::Initialize() {
  PluginManager::RegisterPlugin(GetPluginNameStatic(), pluginDesc,
                                CreateInstance);
}

void ABIMacOSX_arm64::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}
