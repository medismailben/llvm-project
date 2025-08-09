//===-- ABISysV_x86_64.h ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_ABI_X86_ABISYSV_X86_64_H
#define LLDB_SOURCE_PLUGINS_ABI_X86_ABISYSV_X86_64_H

#include "Plugins/ABI/X86/ABIX86_64.h"
#include "lldb/lldb-forward.h"

class ABISysV_x86_64 : public ABIX86_64 {
public:
  static constexpr const uint8_t x86_64_jmp_opcode                 = 0xE9;
  static constexpr const std::size_t x86_64_jmp_size               = 5;

  static constexpr const uint8_t x86_64_call_opcode                = 0xE8;
  static constexpr const std::size_t x86_64_call_size              = 5;

  static constexpr const uint8_t x86_64_mov_opcode                 = 0x89;
  static constexpr const std::size_t x86_64_mov_size               = 3;

  static constexpr const uint8_t x86_64_sub_opcode                 = 0x83;
  static constexpr const std::size_t x86_64_sub_size               = 4;

  static constexpr const uint8_t x86_64_add_opcode                 = 0x83;
  static constexpr const std::size_t x86_64_add_size               = 4;

  static constexpr const uint8_t x86_64_push_opcode                = 0x50;
  static constexpr const uint8_t x86_64_pop_opcode                 = 0x58;

  static constexpr const uint8_t x86_64_rexb_opcode                = 0x41;
  static constexpr const uint8_t x86_64_rexw_opcode                = 0x48;

  static constexpr const uint8_t x86_64_add_byte                   = 0xC4;
  static constexpr const uint8_t x86_64_sub_byte                   = 0xEC;

  static constexpr const uint8_t x86_64_rax_rdi_sib_byte           = 0xC7;
  static constexpr const uint8_t x86_64_rsp_rsi_sib_byte           = 0xE6;
  static constexpr const uint8_t x86_64_rsp_rdi_sib_byte           = 0xE7;

  static constexpr const std::size_t x86_64_saved_register_size    = 16;
  static constexpr const std::size_t x86_64_volatile_register_size = 8;

  static constexpr const char *register_context = R"(typedef struct {
                                                         intptr_t r15;
                                                         intptr_t r14;
                                                         intptr_t r13;
                                                         intptr_t r12;
                                                         intptr_t r11;
                                                         intptr_t r10;
                                                         intptr_t r9;
                                                         intptr_t r8;
                                                         intptr_t rdi;
                                                         intptr_t rsi;
                                                         intptr_t rbp;
                                                         intptr_t rsp;
                                                         intptr_t rbx;
                                                         intptr_t rdx;
                                                         intptr_t rcx;
                                                         intptr_t rax;
                                                      } register_context;)";

  ~ABISysV_x86_64() override = default;

  llvm::Expected<OpcodeArray> GetDebugTrapOpcode() override;

  size_t GetRedZoneSize() const override;

  bool PrepareTrivialCall(lldb_private::Thread &thread, lldb::addr_t sp,
                          lldb::addr_t functionAddress,
                          lldb::addr_t returnAddress,
                          llvm::ArrayRef<lldb::addr_t> args) const override;

  bool GetArgumentValues(lldb_private::Thread &thread,
                         lldb_private::ValueList &values) const override;

  lldb_private::Status
  SetReturnValueObject(lldb::StackFrameSP &frame_sp,
                       lldb::ValueObjectSP &new_value) override;

  lldb::ValueObjectSP
  GetReturnValueObjectImpl(lldb_private::Thread &thread,
                           lldb_private::CompilerType &type) const override;

  lldb::UnwindPlanSP CreateFunctionEntryUnwindPlan() override;

  lldb::UnwindPlanSP CreateDefaultUnwindPlan() override;

  lldb::UnwindPlanSP CreateTrampolineUnwindPlan(lldb::addr_t return_address) override;

  bool RegisterIsVolatile(const lldb_private::RegisterInfo *reg_info) override;

  // The SysV x86_64 ABI requires that stack frames be 16 byte aligned.
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
    // We have a 64 bit address space, so anything is valid as opcodes
    // aren't fixed width...
    return true;
  }

  bool GetPointerReturnRegister(const char *&name) override;
  bool GetFramePointerRegister(const char *&name) override;

  /// Allocate a memory stub for the fast condition breakpoint trampoline, and
  /// build it by saving the register context, calling the argument structure
  /// builder, passing the resulting structure to the condition checker,
  /// restoring the register context, running the copied instructions and]
  /// jumping back to the user source code.
  ///
  /// \param[in] instrs_size
  ///    The size in bytes of the copied instructions.
  ///
  /// \param[in] data
  ///    The copied instructions buffer.
  ///
  /// \param[in] jmp_addr
  ///    The address of the source .
  ///
  /// \param[in] util_func_addr
  ///    The address of the JIT-ed argument structure builder.
  ///
  /// \param[in] cond_expr_addr
  ///    The address of the JIT-ed condition checker.
  ///
  /// \return
  ///    \b true If building the Trampoline succeeded, \b false otherwise.
  ///
  bool SetupFastConditionalBreakpointTrampoline(
      lldb_private::BreakpointInjectedSite *bp_inject_site) override;

  size_t GetJumpSize() override { return x86_64_jmp_size; }

  llvm::StringRef GetRegisterContextAsString() override {
    return register_context;
  }

  bool SupportsFCB() override { return true; }

  // Static Functions

  static void Initialize();

  static void Terminate();

  static lldb::ABISP CreateInstance(lldb::ProcessSP process_sp, const lldb_private::ArchSpec &arch);

  static llvm::StringRef GetPluginNameStatic() { return "sysv-x86_64"; }

  // PluginInterface protocol
  llvm::StringRef GetPluginName() override { return GetPluginNameStatic(); }

protected:
  void CreateRegisterMapIfNeeded();

  lldb::ValueObjectSP
  GetReturnValueObjectSimple(lldb_private::Thread &thread,
                             lldb_private::CompilerType &ast_type) const;

  bool RegisterIsCalleeSaved(const lldb_private::RegisterInfo *reg_info);
  uint32_t GetGenericNum(llvm::StringRef reg) override;

private:
  using ABIX86_64::ABIX86_64; // Call CreateInstance instead.
};

#endif // LLDB_SOURCE_PLUGINS_ABI_X86_ABISYSV_X86_64_H
