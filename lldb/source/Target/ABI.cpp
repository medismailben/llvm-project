//===-- ABI.cpp -----------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/ABI.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/Value.h"
#include "lldb/Expression/ExpressionVariable.h"
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Symbol/FuncUnwinders.h"
#include "lldb/Symbol/TypeSystem.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "lldb/ValueObject/ValueObjectConstResult.h"
#include "Plugins/ObjectFile/Trampoline/ObjectFileTrampoline.h"
#include "llvm/MC/TargetRegistry.h"
#include <cctype>

using namespace lldb;
using namespace lldb_private;

ABISP
ABI::FindPlugin(lldb::ProcessSP process_sp, const ArchSpec &arch) {
  for (auto create_callback : PluginManager::GetABICreateCallbacks()) {
    if (ABISP abi_sp = create_callback(process_sp, arch))
      return abi_sp;
  }
  return {};
}

ABI::~ABI() = default;

bool RegInfoBasedABI::GetRegisterInfoByName(llvm::StringRef name,
                                            RegisterInfo &info) {
  uint32_t count = 0;
  const RegisterInfo *register_info_array = GetRegisterInfoArray(count);
  if (register_info_array) {
    uint32_t i;
    for (i = 0; i < count; ++i) {
      const char *reg_name = register_info_array[i].name;
      if (reg_name == name) {
        info = register_info_array[i];
        return true;
      }
    }
    for (i = 0; i < count; ++i) {
      const char *reg_alt_name = register_info_array[i].alt_name;
      if (reg_alt_name == name) {
        info = register_info_array[i];
        return true;
      }
    }
  }
  return false;
}

lldb::WritableDataBufferSP ABI::EmitAssembly(llvm::StringRef name,
                                             std::stringstream &expr,
                                             ExecutionContext exe_ctx) {
  Log *log = GetLog(LLDBLog::JITLoader);

  ProcessSP process_sp = GetProcessSP();
  if (!process_sp)
    return nullptr;

  std::string symbol_name = ("$__lldb_" + name).str();

  Target &target = process_sp->GetTarget();

  auto utility_fn_or_error = target.CreateUtilityFunction(
      expr.str(), symbol_name, eLanguageTypeC, exe_ctx);

  if (!utility_fn_or_error) {
    std::string error_str = llvm::toString(utility_fn_or_error.takeError());
    LLDB_LOG(log, "Error creating utility function: {0}.", error_str);
    return nullptr;
  }

  lldb::UtilityFunctionSP emitted_function_sp = std::move(*utility_fn_or_error);

  const AddressRange &jit_addr_range =
      emitted_function_sp->GetJITAddressRange();

  WritableDataBufferSP buffer(
      new DataBufferHeap(jit_addr_range.GetByteSize(), 0));

  lldb::addr_t jit_addr =
      jit_addr_range.GetBaseAddress().GetCallableLoadAddress(&target);

  Status error;
  size_t memory_read = GetProcessSP()->ReadMemory(jit_addr, buffer->GetBytes(),
                                                  buffer->GetByteSize(), error);

  if (memory_read != jit_addr_range.GetByteSize() || error.Fail()) {
    error = Status::FromErrorString("Couldn't read jit memory");
    return nullptr;
  }

  const ArchSpec &arch = target.GetArchitecture();

  auto dis = Disassembler::DisassembleRange(
      arch, /*plugin_name=*/nullptr, /*flavor=*/nullptr, /*cpu=*/nullptr,
      /*features=*/nullptr, target, jit_addr_range);

  if (!dis)
    return nullptr;

  StreamString s;
  Debugger &dbg = target.GetDebugger();
  dis->PrintInstructions(dbg, arch, exe_ctx, false, 0, 0, s);
  if (log)
    log->PutString(s.GetString());

  return buffer;
}

lldb::ModuleSP ABI::CreateModuleForFastConditionalBreakpointTrampoline(
    lldb::addr_t address, std::size_t size, lldb::addr_t return_address) {
  Log *log = GetLog(LLDBLog::JITLoader);

  if (!SupportsFCB()) {
    LLDB_LOG(log, "JIT: ABI {0} does not implement JIT-ed breakpoint condition",
             GetPluginName().data());
    return nullptr;
  }

  ProcessSP process_sp = m_process_wp.lock();

  if (!process_sp) {
    LLDB_LOG(log, "JIT: No live process to create the trampoline module for");
    return nullptr;
  }

  lldb::ModuleSP trampoline_module_sp =
      Module::CreateModuleFromObjectFile<ObjectFileTrampoline>(process_sp,
                                                               address, size);

  if (!trampoline_module_sp) {
    LLDB_LOG(log, "JIT: Couldn't create module from trampoline ObjectFile");
    return nullptr;
  }

  bool changed = false;
  trampoline_module_sp->SetLoadAddress(process_sp->GetTarget(), 0, true,
                                       changed);

  Symtab *symtab = trampoline_module_sp->GetObjectFile()->GetSymtab();

  if (!symtab || !symtab->GetNumSymbols()) {
    LLDB_LOG(log, "JIT: Couldn't find any symbol or symbol table");
    return nullptr;
  }

  Symbol *symbol = symtab->SymbolAtIndex(0);

  SymbolContext sc(trampoline_module_sp, nullptr, nullptr, nullptr, nullptr,
                   symbol);
  UnwindTable &unwind_table = trampoline_module_sp->GetUnwindTable();
  FuncUnwindersSP func_unwinders_sp =
      unwind_table.GetFuncUnwindersContainingAddress(address, sc);

  if (!func_unwinders_sp) {
    LLDB_LOG(log, "JIT: Couldn't find any function unwinder for {0} ({1})",
             symbol->GetName().AsCString(), address);
    return nullptr;
  }

  if (UnwindPlanSP trampoline_unwind_plan_sp =
          CreateTrampolineUnwindPlan(return_address)) {
    func_unwinders_sp->SetTrampolineUnwindPlan(trampoline_unwind_plan_sp);
    return trampoline_module_sp;
  }

  LLDB_LOG(log, "JIT: Couldn't create trampoline unwind plan");
  return nullptr;
}

ValueObjectSP ABI::GetReturnValueObject(Thread &thread, CompilerType &ast_type,
                                        bool persistent) const {
  if (!ast_type.IsValid())
    return ValueObjectSP();

  ValueObjectSP return_valobj_sp;

  return_valobj_sp = GetReturnValueObjectImpl(thread, ast_type);
  if (!return_valobj_sp)
    return return_valobj_sp;

  // Now turn this into a persistent variable.
  // FIXME: This code is duplicated from Target::EvaluateExpression, and it is
  // used in similar form in a couple
  // of other places.  Figure out the correct Create function to do all this
  // work.

  if (persistent) {
    Target &target = *thread.CalculateTarget();
    PersistentExpressionState *persistent_expression_state =
        target.GetPersistentExpressionStateForLanguage(
            ast_type.GetMinimumLanguage());

    if (!persistent_expression_state)
      return {};

    ConstString persistent_variable_name =
        persistent_expression_state->GetNextPersistentVariableName();

    lldb::ValueObjectSP const_valobj_sp;

    // Check in case our value is already a constant value
    if (return_valobj_sp->GetIsConstant()) {
      const_valobj_sp = return_valobj_sp;
      const_valobj_sp->SetName(persistent_variable_name);
    } else
      const_valobj_sp =
          return_valobj_sp->CreateConstantValue(persistent_variable_name);

    lldb::ValueObjectSP live_valobj_sp = return_valobj_sp;

    return_valobj_sp = const_valobj_sp;

    ExpressionVariableSP expr_variable_sp(
        persistent_expression_state->CreatePersistentVariable(
            return_valobj_sp));

    assert(expr_variable_sp);

    // Set flags and live data as appropriate

    const Value &result_value = live_valobj_sp->GetValue();

    switch (result_value.GetValueType()) {
    case Value::ValueType::Invalid:
      return {};
    case Value::ValueType::HostAddress:
    case Value::ValueType::FileAddress:
      // we odon't do anything with these for now
      break;
    case Value::ValueType::Scalar:
      expr_variable_sp->m_flags |=
          ExpressionVariable::EVIsFreezeDried;
      expr_variable_sp->m_flags |=
          ExpressionVariable::EVIsLLDBAllocated;
      expr_variable_sp->m_flags |=
          ExpressionVariable::EVNeedsAllocation;
      break;
    case Value::ValueType::LoadAddress:
      expr_variable_sp->GetLiveObject() = live_valobj_sp;
      expr_variable_sp->m_flags |=
          ExpressionVariable::EVIsProgramReference;
      break;
    }

    return_valobj_sp = expr_variable_sp->GetValueObject();
  }
  return return_valobj_sp;
}

addr_t ABI::FixCodeAddress(lldb::addr_t pc) {
  ProcessSP process_sp(GetProcessSP());

  addr_t mask = process_sp->GetCodeAddressMask();
  if (mask == LLDB_INVALID_ADDRESS_MASK)
    return pc;

  // Assume the high bit is used for addressing, which
  // may not be correct on all architectures e.g. AArch64
  // where Top Byte Ignore mode is often used to store
  // metadata in the top byte, and b55 is the bit used for
  // differentiating between low- and high-memory addresses.
  // That target's ABIs need to override this method.
  bool is_highmem = pc & (1ULL << 63);
  return is_highmem ? pc | mask : pc & (~mask);
}

addr_t ABI::FixDataAddress(lldb::addr_t pc) {
  ProcessSP process_sp(GetProcessSP());
  addr_t mask = process_sp->GetDataAddressMask();
  if (mask == LLDB_INVALID_ADDRESS_MASK)
    return pc;

  // Assume the high bit is used for addressing, which
  // may not be correct on all architectures e.g. AArch64
  // where Top Byte Ignore mode is often used to store
  // metadata in the top byte, and b55 is the bit used for
  // differentiating between low- and high-memory addresses.
  // That target's ABIs need to override this method.
  bool is_highmem = pc & (1ULL << 63);
  return is_highmem ? pc | mask : pc & (~mask);
}

ValueObjectSP ABI::GetReturnValueObject(Thread &thread, llvm::Type &ast_type,
                                        bool persistent) const {
  ValueObjectSP return_valobj_sp;
  return_valobj_sp = GetReturnValueObjectImpl(thread, ast_type);
  return return_valobj_sp;
}

// specialized to work with llvm IR types
//
// for now we will specify a default implementation so that we don't need to
// modify other ABIs
lldb::ValueObjectSP ABI::GetReturnValueObjectImpl(Thread &thread,
                                                  llvm::Type &ir_type) const {
  ValueObjectSP return_valobj_sp;

  /* this is a dummy and will only be called if an ABI does not override this */

  return return_valobj_sp;
}

bool ABI::PrepareTrivialCall(Thread &thread, lldb::addr_t sp,
                             lldb::addr_t functionAddress,
                             lldb::addr_t returnAddress, llvm::Type &returntype,
                             llvm::ArrayRef<ABI::CallArgument> args) const {
  // dummy prepare trivial call
  llvm_unreachable("Should never get here!");
}

bool ABI::GetFallbackRegisterLocation(
    const RegisterInfo *reg_info,
    UnwindPlan::Row::AbstractRegisterLocation &unwind_regloc) {
  // Did the UnwindPlan fail to give us the caller's stack pointer? The stack
  // pointer is defined to be the same as THIS frame's CFA, so return the CFA
  // value as the caller's stack pointer.  This is true on x86-32/x86-64 at
  // least.
  if (reg_info->kinds[eRegisterKindGeneric] == LLDB_REGNUM_GENERIC_SP) {
    unwind_regloc.SetIsCFAPlusOffset(0);
    return true;
  }

  // If a volatile register is being requested, we don't want to forward the
  // next frame's register contents up the stack -- the register is not
  // retrievable at this frame.
  if (RegisterIsVolatile(reg_info)) {
    unwind_regloc.SetUndefined();
    return true;
  }

  return false;
}

std::unique_ptr<llvm::MCRegisterInfo> ABI::MakeMCRegisterInfo(const ArchSpec &arch) {
  const llvm::Triple &triple = arch.GetTriple();
  std::string lookup_error;
  const llvm::Target *target =
      llvm::TargetRegistry::lookupTarget(triple, lookup_error);
  if (!target) {
    LLDB_LOG(GetLog(LLDBLog::Process),
             "Failed to create an llvm target for {0}: {1}", triple.str(),
             lookup_error);
    return nullptr;
  }
  std::unique_ptr<llvm::MCRegisterInfo> info_up(
      target->createMCRegInfo(triple));
  assert(info_up);
  return info_up;
}

void RegInfoBasedABI::AugmentRegisterInfo(
    std::vector<DynamicRegisterInfo::Register> &regs) {
  for (DynamicRegisterInfo::Register &info : regs) {
    if (info.regnum_ehframe != LLDB_INVALID_REGNUM &&
        info.regnum_dwarf != LLDB_INVALID_REGNUM)
      continue;

    RegisterInfo abi_info;
    if (!GetRegisterInfoByName(info.name.GetStringRef(), abi_info))
      continue;

    if (info.regnum_ehframe == LLDB_INVALID_REGNUM)
      info.regnum_ehframe = abi_info.kinds[eRegisterKindEHFrame];
    if (info.regnum_dwarf == LLDB_INVALID_REGNUM)
      info.regnum_dwarf = abi_info.kinds[eRegisterKindDWARF];
    if (info.regnum_generic == LLDB_INVALID_REGNUM)
      info.regnum_generic = abi_info.kinds[eRegisterKindGeneric];
  }
}

void MCBasedABI::AugmentRegisterInfo(
    std::vector<DynamicRegisterInfo::Register> &regs) {
  for (DynamicRegisterInfo::Register &info : regs) {
    uint32_t eh, dwarf;
    std::tie(eh, dwarf) = GetEHAndDWARFNums(info.name.GetStringRef());

    if (info.regnum_ehframe == LLDB_INVALID_REGNUM)
      info.regnum_ehframe = eh;
    if (info.regnum_dwarf == LLDB_INVALID_REGNUM)
      info.regnum_dwarf = dwarf;
    if (info.regnum_generic == LLDB_INVALID_REGNUM)
      info.regnum_generic = GetGenericNum(info.name.GetStringRef());
  }
}

std::pair<uint32_t, uint32_t>
MCBasedABI::GetEHAndDWARFNums(llvm::StringRef name) {
  std::string mc_name = GetMCName(name.str());
  for (char &c : mc_name)
    c = std::toupper(c);
  int eh = -1;
  int dwarf = -1;
  for (unsigned reg = 0; reg < m_mc_register_info_up->getNumRegs(); ++reg) {
    if (m_mc_register_info_up->getName(reg) == mc_name) {
      eh = m_mc_register_info_up->getDwarfRegNum(reg, /*isEH=*/true);
      dwarf = m_mc_register_info_up->getDwarfRegNum(reg, /*isEH=*/false);
      break;
    }
  }
  return std::pair<uint32_t, uint32_t>(eh == -1 ? LLDB_INVALID_REGNUM : eh,
                                       dwarf == -1 ? LLDB_INVALID_REGNUM
                                                   : dwarf);
}

void MCBasedABI::MapRegisterName(std::string &name, llvm::StringRef from_prefix,
                                 llvm::StringRef to_prefix) {
  llvm::StringRef name_ref = name;
  if (!name_ref.consume_front(from_prefix))
    return;
  uint64_t _;
  if (name_ref.empty() || to_integer(name_ref, _, 10))
    name = (to_prefix + name_ref).str();
}
