//===-- ABI.h ---------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_TARGET_ABI_H
#define LLDB_TARGET_ABI_H

#include <ios>
#include <sstream>

#include "lldb/Core/PluginInterface.h"
#include "lldb/Symbol/UnwindPlan.h"
#include "lldb/Target/DynamicRegisterInfo.h"
#include "lldb/Utility/Status.h"
#include "lldb/Utility/UnimplementedError.h"
#include "lldb/lldb-forward.h"
#include "lldb/lldb-private.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/Errc.h"

namespace llvm {
class Type;
}

namespace lldb_private {

class ABI : public PluginInterface {
public:
  struct CallArgument {
    enum eType {
      HostPointer = 0, /* pointer to host data */
      TargetValue,     /* value is on the target or literal */
    };
    eType type;  /* value of eType */
    size_t size; /* size in bytes of this argument */

    lldb::addr_t value;                 /* literal value */
    std::unique_ptr<uint8_t[]> data_up; /* host data pointer */
  };

  using OpcodeArray = llvm::ArrayRef<llvm::SmallVector<uint8_t, 8>>;

  ~ABI() override;

  virtual size_t GetRedZoneSize() const = 0;

  virtual bool PrepareTrivialCall(lldb_private::Thread &thread, lldb::addr_t sp,
                                  lldb::addr_t functionAddress,
                                  lldb::addr_t returnAddress,
                                  llvm::ArrayRef<lldb::addr_t> args) const = 0;

  // Prepare trivial call used from ThreadPlanFunctionCallUsingABI
  // AD:
  //  . Because i don't want to change other ABI's this is not declared pure
  //  virtual.
  //    The dummy implementation will simply fail.  Only HexagonABI will
  //    currently
  //    use this method.
  //  . Two PrepareTrivialCall's is not good design so perhaps this should be
  //  combined.
  //
  virtual bool PrepareTrivialCall(lldb_private::Thread &thread, lldb::addr_t sp,
                                  lldb::addr_t functionAddress,
                                  lldb::addr_t returnAddress,
                                  llvm::Type &prototype,
                                  llvm::ArrayRef<CallArgument> args) const;

  virtual bool GetArgumentValues(Thread &thread, ValueList &values) const = 0;

  lldb::ValueObjectSP GetReturnValueObject(Thread &thread, CompilerType &type,
                                           bool persistent = true) const;

  // specialized to work with llvm IR types
  lldb::ValueObjectSP GetReturnValueObject(Thread &thread, llvm::Type &type,
                                           bool persistent = true) const;

  // Set the Return value object in the current frame as though a function with
  virtual Status SetReturnValueObject(lldb::StackFrameSP &frame_sp,
                                      lldb::ValueObjectSP &new_value) = 0;

protected:
  // This is the method the ABI will call to actually calculate the return
  // value. Don't put it in a persistent value object, that will be done by the
  // ABI::GetReturnValueObject.
  virtual lldb::ValueObjectSP
  GetReturnValueObjectImpl(Thread &thread, CompilerType &ast_type) const = 0;

  // specialized to work with llvm IR types
  virtual lldb::ValueObjectSP
  GetReturnValueObjectImpl(Thread &thread, llvm::Type &ir_type) const;

  /// Request to get a Process shared pointer.
  ///
  /// This ABI object may not have been created with a Process object,
  /// or the Process object may no longer be alive.  Be sure to handle
  /// the case where the shared pointer returned does not have an
  /// object inside it.
  lldb::ProcessSP GetProcessSP() const { return m_process_wp.lock(); }

public:
  virtual lldb::UnwindPlanSP CreateFunctionEntryUnwindPlan() = 0;

  virtual lldb::UnwindPlanSP CreateDefaultUnwindPlan() = 0;

  /// Describe how to unwind out of a fast conditional breakpoint trampoline.
  ///
  /// A trampoline is reached by a branch rather than a call, so the stack
  /// pointer of the function it was branched out of is unchanged. That makes
  /// the canonical frame address of the trampoline frame equal to that stack
  /// pointer, which is \a frame_size above where the stack pointer sits once
  /// the trampoline has built its frame.
  ///
  /// \param[in] site_address
  ///     The patched address in the user's code. Reported as the program
  ///     counter of the frame above, because the displaced instructions have
  ///     not run yet when the condition is evaluated.
  ///
  /// \param[in] frame_size
  ///     How much the trampoline subtracts from the stack pointer before
  ///     calling the condition checker: the saved register context plus the
  ///     argument structure.
  virtual lldb::UnwindPlanSP
  CreateTrampolineUnwindPlan(lldb::addr_t site_address, size_t frame_size) {
    return {};
  }

  virtual bool RegisterIsVolatile(const RegisterInfo *reg_info) = 0;

  /// Whether \a reg_name is a pure temporary under this calling convention:
  /// one this function may use freely, that no callee may read as an input and
  /// no caller may expect to get back.
  ///
  /// Only for such a register does a path reaching a call or a return prove
  /// that the old value stopped mattering. Being caller saved is not enough: an
  /// argument register is caller saved and is read by the callee, and a result
  /// register is caller saved and is read by the caller. Neither may be pruned.
  ///
  /// The default is false, which is the answer that keeps a register live, so
  /// an ABI that has not described itself refuses rather than guesses.
  virtual bool RegisterIsPureTemporary(llvm::StringRef reg_name) {
    return false;
  }

  virtual bool GetFallbackRegisterLocation(
      const RegisterInfo *reg_info,
      UnwindPlan::Row::AbstractRegisterLocation &unwind_regloc);

  // Should take a look at a call frame address (CFA) which is just the stack
  // pointer value upon entry to a function. ABIs usually impose alignment
  // restrictions (4, 8 or 16 byte aligned), and zero is usually not allowed.
  // This function should return true if "cfa" is valid call frame address for
  // the ABI, and false otherwise. This is used by the generic stack frame
  // unwinding code to help determine when a stack ends.
  virtual bool CallFrameAddressIsValid(lldb::addr_t cfa) = 0;

  // Validates a possible PC value and returns true if an opcode can be at
  // "pc".
  virtual bool CodeAddressIsValid(lldb::addr_t pc) = 0;

  /// Some targets might use bits in a code address to indicate a mode switch.
  /// ARM uses bit zero to signify a code address is thumb, so any ARM ABI
  /// plug-ins would strip those bits.
  /// @{
  virtual lldb::addr_t FixCodeAddress(lldb::addr_t pc);
  virtual lldb::addr_t FixDataAddress(lldb::addr_t pc);
  /// @}

  /// Use this method when you do not know, or do not care what kind of address
  /// you are fixing. On platforms where there would be a difference between the
  /// two types, it will pick the safest option.
  ///
  /// Its purpose is to signal that no specific choice was made and provide an
  /// alternative to randomly picking FixCode/FixData address. Which could break
  /// platforms where there is a difference (only Arm Thumb at this time).
  virtual lldb::addr_t FixAnyAddress(lldb::addr_t pc) {
    // On Arm Thumb fixing a code address zeroes the bottom bit, so FixData is
    // the safe choice. On any other platform (so far) code and data addresses
    // are fixed in the same way.
    return FixDataAddress(pc);
  }

  llvm::MCRegisterInfo &GetMCRegisterInfo() { return *m_mc_register_info_up; }

  virtual void
  AugmentRegisterInfo(std::vector<DynamicRegisterInfo::Register> &regs) = 0;

  virtual bool GetPointerReturnRegister(const char *&name) { return false; }
  virtual bool GetFramePointerRegister(const char *&name) { return false; }
  virtual llvm::Expected<std::string> GetRegisterName(uint32_t num) {
    return llvm::make_error<UnimplementedError>();
  }

  /// Allocate a memory stub for the fast conditional breakpoint trampoline and
  /// build it, by saving the register context, calling the argument structure
  /// builder, passing the resulting structure to the condition checker,
  /// restoring the register context, running the instructions displaced by the
  /// branch and jumping back to the user's code.
  ///
  /// The site is patched with a branch to the trampoline as part of this call,
  /// and a module describing the trampoline is added to the target so that the
  /// unwinder can walk out of it.
  ///
  /// \param[in] bp_inject_site
  ///    The site to patch. It carries the addresses of the JIT-ed argument
  ///    structure builder and condition checker, and the variable metadata
  ///    needed to size the argument structure.
  ///
  /// \return
  ///    Success if the trampoline was built and installed, otherwise the reason
  ///    it could not be. That reason is shown to the user as the explanation
  ///    for why the condition is evaluated out of process instead, so it should
  ///    read as a sentence rather than as a log line.
  ///
  virtual llvm::Error SetupFastConditionalBreakpointTrampoline(
      BreakpointInjectedSite *bp_inject_site) {
    return llvm::createStringError(llvm::formatv(
        "the {0} ABI does not implement injected conditions", GetPluginName()));
  }

  /// Set aside the memory a trampoline for \a bp_inject_site will need, before
  /// anything else is allocated in the inferior on its behalf.
  ///
  /// The trampoline and the JIT-ed condition both land in inferior memory, and
  /// the JIT asks for pages without caring where they are, so whichever is
  /// allocated first takes the holes nearest the site. The trampoline is the
  /// one that has to be near: the patch branches to it, and an instruction
  /// displaced into it may be pc relative with a much shorter reach than the
  /// patch's branch, so a nearer trampoline is one more of those can be
  /// re-encoded rather than refused.
  ///
  /// Reserving is separate from building because the two happen either side of
  /// compiling the condition. An ABI that does not care may leave this alone;
  /// SetupFastConditionalBreakpointTrampoline() is then responsible for its own
  /// allocation.
  virtual llvm::Error
  ReserveFastConditionalBreakpointTrampoline(BreakpointInjectedSite *) {
    return llvm::Error::success();
  }

  /// Log the disassembly of a freshly built trampoline.
  ///
  /// The trampoline is only registered as a module once it is installed, so
  /// this is the only way to inspect it when something goes wrong before that.
  void LogTrampolineDisassembly(llvm::ArrayRef<uint8_t> trampoline,
                                lldb::addr_t address);

  virtual llvm::ArrayRef<uint8_t> GetJumpOpcode() { return {}; }

  virtual size_t GetJumpSize() { return 0; }

  virtual llvm::StringRef GetRegisterContextAsString() { return ""; }

  virtual llvm::StringRef GetMachTypesAsString() { return ""; }

  virtual bool SupportsFCB() { return false; }

  lldb::WritableDataBufferSP
  EmitAssembly(llvm::StringRef name, std::stringstream &expr,
               lldb_private::ExecutionContext exe_ctx);

  virtual llvm::Expected<OpcodeArray> GetDebugTrapOpcode() {
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "Unknown Debug Trap Opcode");
  }

  virtual uint64_t GetStackFrameSize() { return 512 * 1024; }

  static lldb::ABISP FindPlugin(lldb::ProcessSP process_sp,
                                const ArchSpec &arch);

  struct MemoryPermissions {
    // Both of these are sets of lldb::Permissions values.
    // Overlay are the permissions being applied to the original permissions.
    uint32_t overlay;
    // Effective is the result of applying the overlay to the original
    // permissions. Calculating this is done by the plugin because some
    // permission overlays are done as positive (add permissions) and some as
    // negative (remove permissions).
    uint32_t effective;
  };

  /// Get the effective memory permissions that result when the permissions
  /// referred to by a protection key are applied to the original permissions.
  ///
  /// This is intended for architectures that have some sort of permission
  /// overlay system. Where the protection key is used to look up a set of
  /// permissions that modifies the original permissions.
  ///
  /// \returns the overlay permissions (that the protection key refers to) and
  ///   the effective permissions. If the target does not have an overlay
  ///   system, or it does and the protection key is invalid, returns nullopt.
  virtual std::optional<MemoryPermissions>
  GetMemoryPermissions(lldb_private::RegisterContext &reg_ctx,
                       unsigned protection_key, uint32_t original_permissions) {
    return std::nullopt;
  }

protected:
  ABI(lldb::ProcessSP process_sp, std::unique_ptr<llvm::MCRegisterInfo> info_up)
      : m_process_wp(process_sp), m_mc_register_info_up(std::move(info_up)) {
    assert(m_mc_register_info_up && "ABI must have MCRegisterInfo");
  }

  using ByteArray = llvm::ArrayRef<llvm::SmallVector<uint8_t, 4>>;

  /// Utility function to construct a MCRegisterInfo using the ArchSpec triple.
  /// Plugins wishing to customize the construction can construct the
  /// MCRegisterInfo themselves.
  static std::unique_ptr<llvm::MCRegisterInfo>
  MakeMCRegisterInfo(const ArchSpec &arch);

  lldb::ProcessWP m_process_wp;
  std::unique_ptr<llvm::MCRegisterInfo> m_mc_register_info_up;

  /// Wrap an installed trampoline in a Module so that the unwinder can walk out
  /// of it.
  ///
  /// \param[in] address
  ///     Where the trampoline was installed.
  ///
  /// \param[in] size
  ///     Its size in bytes.
  ///
  /// \param[in] site_address
  ///     The patched address in the user's code.
  ///
  /// \param[in] frame_size
  ///     How much the trampoline subtracts from the stack pointer before
  ///     calling the condition checker.
  lldb::ModuleSP CreateModuleForFastConditionalBreakpointTrampoline(
      lldb::addr_t address, std::size_t size, lldb::addr_t site_address,
      size_t frame_size);

private:
  ABI(const ABI &) = delete;
  const ABI &operator=(const ABI &) = delete;
};

class RegInfoBasedABI : public ABI {
public:
  void AugmentRegisterInfo(
      std::vector<DynamicRegisterInfo::Register> &regs) override;

protected:
  using ABI::ABI;

  bool GetRegisterInfoByName(llvm::StringRef name, RegisterInfo &info);

  virtual const RegisterInfo *GetRegisterInfoArray(uint32_t &count) = 0;
};

class MCBasedABI : public ABI {
public:
  void AugmentRegisterInfo(
      std::vector<DynamicRegisterInfo::Register> &regs) override;

  /// If the register name is of the form "<from_prefix>[<number>]" then change
  /// the name to "<to_prefix>[<number>]". Otherwise, leave the name unchanged.
  static void MapRegisterName(std::string &reg, llvm::StringRef from_prefix,
                              llvm::StringRef to_prefix);

protected:
  using ABI::ABI;

  /// Return eh_frame and dwarf numbers for the given register.
  virtual std::pair<uint32_t, uint32_t> GetEHAndDWARFNums(llvm::StringRef reg);

  /// Return the generic number of the given register.
  virtual uint32_t GetGenericNum(llvm::StringRef reg) = 0;

  /// For the given (capitalized) lldb register name, return the name of this
  /// register in the MCRegisterInfo struct.
  virtual std::string GetMCName(std::string reg) { return reg; }
};

} // namespace lldb_private

#endif // LLDB_TARGET_ABI_H
