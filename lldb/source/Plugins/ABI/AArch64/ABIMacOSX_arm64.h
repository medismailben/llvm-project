//===-- ABIMacOSX_arm64.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_ABI_AARCH64_ABIMACOSX_ARM64_H
#define LLDB_SOURCE_PLUGINS_ABI_AARCH64_ABIMACOSX_ARM64_H

#include "Plugins/ABI/AArch64/ABIAArch64.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/lldb-private.h"

class ABIMacOSX_arm64 : public ABIAArch64 {
public:
  static constexpr const std::size_t aarch64_instr_size = 4;

  /// `b` encodes a signed 26 bit immediate scaled by the instruction size, so
  /// every address within 128MiB of the site is in reach of a direct patch.
  static constexpr const uint64_t aarch64_branch_reach = 128ULL * 1024 * 1024;

  /// Bytes the wider patch form occupies: `adrp`, `add`, `br`.
  ///
  /// Needed because a large image can have no free page within 128MiB of any of
  /// its code: a debug build of clang leaves nothing unmapped for a quarter of
  /// a gigabyte past its own text, and there is nothing but the rest of the
  /// image below.
  static constexpr const std::size_t aarch64_far_patch_size =
      3 * aarch64_instr_size;

  /// `adrp` displaces by a signed 21 bit count of 4KiB pages, so the wider form
  /// reaches 4GiB either way. One page short of that, since what the immediate
  /// counts is pages between the page the patch is in and the page it names.
  static constexpr const uint64_t aarch64_far_patch_reach =
      4ULL * 1024 * 1024 * 1024 - 4096;

  /// Instructions the branch back to the patched code takes when the site is
  /// out of a direct branch's reach: four to build its address, then `br`.
  static constexpr const std::size_t aarch64_far_branch_slots = 5;

  /// What is set aside for a trampoline before it has been assembled.
  ///
  /// It has to be allocated before it can be built: how far it lands from the
  /// site decides which form the patch takes, and that decides how many
  /// instructions the patch displaces, which decides what the trampoline has to
  /// hold. A page is the smallest thing the allocator hands out anyway, and a
  /// trampoline that somehow does not fit in one is refused rather than
  /// truncated.
  static constexpr const std::size_t aarch64_trampoline_reservation = 4096;

  /// Stack the trampoline reserves for the `register_context` struct below. The
  /// generated prologue subtracts this from sp, so the unwind plan has to agree
  /// with it.
  static constexpr const std::size_t aarch64_register_context_size = 0x100;

  /// Width of one slot in that context. The prologue stores x0 through x30 as
  /// consecutive doublewords from its base, which is what lets the trampoline's
  /// unwind plan describe where each of the patched function's registers went.
  static constexpr const std::size_t aarch64_gpr_size = 8;

  static constexpr const char *register_context = R"(typedef struct {
                                                      intptr_t x0;
                                                      intptr_t x1;
                                                      intptr_t x2;
                                                      intptr_t x3;
                                                      intptr_t x4;
                                                      intptr_t x5;
                                                      intptr_t x6;
                                                      intptr_t x7;
                                                      intptr_t x8;
                                                      intptr_t x9;
                                                      intptr_t x10;
                                                      intptr_t x11;
                                                      intptr_t x12;
                                                      intptr_t x13;
                                                      intptr_t x14;
                                                      intptr_t x15;
                                                      intptr_t x16;
                                                      intptr_t x17;
                                                      intptr_t x18;
                                                      intptr_t x19;
                                                      intptr_t x20;
                                                      intptr_t x21;
                                                      intptr_t x22;
                                                      intptr_t x23;
                                                      intptr_t x24;
                                                      intptr_t x25;
                                                      intptr_t x26;
                                                      intptr_t x27;
                                                      intptr_t x28;
                                                      intptr_t fp;
                                                      intptr_t lr;
                                                      intptr_t sp;
                                                      } register_context;)";

  ~ABIMacOSX_arm64() override = default;

  size_t GetRedZoneSize() const override;

  bool PrepareTrivialCall(lldb_private::Thread &thread, lldb::addr_t sp,
                          lldb::addr_t functionAddress,
                          lldb::addr_t returnAddress,
                          llvm::ArrayRef<lldb::addr_t> args) const override;

  bool GetArgumentValues(lldb_private::Thread &thread,
                         lldb_private::ValueList &values) const override;

  lldb::UnwindPlanSP CreateTrampolineUnwindPlan(lldb::addr_t site_address,
                                                size_t frame_size) override;

  bool RegisterIsVolatile(const lldb_private::RegisterInfo *reg_info) override;

  // The arm64 ABI requires that stack frames be 16 byte aligned.
  // When there is a trap handler on the stack, e.g. _sigtramp in userland
  // code, we've seen that the stack pointer is often not aligned properly
  // before the handler is invoked.  This means that lldb will stop the unwind
  // early -- before the function which caused the trap.
  //
  // To work around this, we relax that alignment to be just word-size
  // (8-bytes).
  // Allowing the trap handlers for user space would be easy (_sigtramp) but
  // in other environments there can be a large number of different functions
  // involved in async traps.
  bool CallFrameAddressIsValid(lldb::addr_t cfa) override {
    // Make sure the stack call frame addresses are 8 byte aligned
    if (cfa & (8ull - 1ull))
      return false; // Not 8 byte aligned
    if (cfa == 0)
      return false; // Zero is not a valid stack address
    return true;
  }

  bool CodeAddressIsValid(lldb::addr_t pc) override {
    if (pc & (4ull - 1ull))
      return false; // Not 4 byte aligned

    // Anything else if fair game..
    return true;
  }

  llvm::Expected<std::string> GetRegisterName(uint32_t num) override;

  bool GetFramePointerRegister(const char *&name) override;

  lldb::addr_t FixCodeAddress(lldb::addr_t pc) override;
  lldb::addr_t FixDataAddress(lldb::addr_t pc) override;

  /// Allocate a memory stub for the fast condition breakpoint trampoline, and
  /// build it by saving the register context, calling the argument structure
  /// builder, passing the resulting structure to the condition checker,
  /// restoring the register context, running the copied instructions and]
  /// jumping back to the user source code.
  ///
  /// \param[in] instrs_size
  ///    The size in bytes of the copied instructions.
  ///
  /// \return
  ///    \b true If building the Trampoline succeeded, \b false otherwise.
  ///
  llvm::Error SetupFastConditionalBreakpointTrampoline(
      lldb_private::BreakpointInjectedSite *bp_inject_site) override;

  bool RegisterIsPureTemporary(llvm::StringRef reg_name) override;

  size_t GetJumpSize() override { return aarch64_instr_size; };

  llvm::StringRef GetRegisterContextAsString() override {
    return register_context;
  }

  bool SupportsFCB() override { return true; }

  // Static Functions

  static void Initialize();

  static void Terminate();

  static lldb::ABISP CreateInstance(lldb::ProcessSP process_sp,
                                    const lldb_private::ArchSpec &arch);

  // PluginInterface protocol

  static llvm::StringRef GetPluginNameStatic() { return "ABIMacOSX_arm64"; }

  llvm::StringRef GetPluginName() override { return GetPluginNameStatic(); }

  lldb_private::Status
  SetReturnValueObject(lldb::StackFrameSP &frame_sp,
                       lldb::ValueObjectSP &new_value) override;

protected:
  lldb::ValueObjectSP
  GetReturnValueObjectImpl(lldb_private::Thread &thread,
                           lldb_private::CompilerType &ast_type) const override;

  lldb::WritableDataBufferSP
  EmitBranchToAddressAssembly(lldb_private::ExecutionContext &exe_ctx,
                              ssize_t return_addr = LLDB_INVALID_ADDRESS);

private:
  using ABIAArch64::ABIAArch64; // Call CreateInstance instead.
};

#endif // LLDB_SOURCE_PLUGINS_ABI_AARCH64_ABIMACOSX_ARM64_H
