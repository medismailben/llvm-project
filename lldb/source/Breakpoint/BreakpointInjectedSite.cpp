//===-- BreakpointInjectedSite.cpp ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Breakpoint/BreakpointInjectedSite.h"

#include "lldb/Expression/ExpressionVariable.h"
#include "lldb/Target/ABI.h"
#include "lldb/Target/Language.h"
#include "lldb/Target/Platform.h"

#include "lldb/Utility/DataBufferHeap.h"

#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/FormatAdapters.h"

using namespace lldb;
using namespace lldb_private;

BreakpointInjectedSite::BreakpointInjectedSite(
    const BreakpointLocationSP &owner, lldb::addr_t addr)
    : BreakpointSite(owner, addr, false, eKindBreakpointInjectedSite),
      m_target_sp(owner->GetTarget().shared_from_this()),
      m_real_addr(owner->GetAddress()), m_trap_addr(LLDB_INVALID_ADDRESS),
      m_args_struct_size(0) {}

void BreakpointInjectedSite::SetPatchedInstructions(
    lldb::addr_t address, WritableDataBufferSP patch,
    WritableDataBufferSP displaced) {
  assert(patch && displaced && "a patch needs both halves to be reversible");
  assert(patch->GetByteSize() == displaced->GetByteSize() &&
         "the patch and what it displaced have to cover the same bytes");

  m_patch_addr = address;
  m_patch_instructions_sp = std::move(patch);
  m_displaced_instructions_sp = std::move(displaced);
  m_patched = true;
}

llvm::ArrayRef<uint8_t> BreakpointInjectedSite::GetPatchBytes() const {
  if (!m_patch_instructions_sp)
    return {};
  return {m_patch_instructions_sp->GetBytes(),
          m_patch_instructions_sp->GetByteSize()};
}

llvm::ArrayRef<uint8_t> BreakpointInjectedSite::GetDisplacedBytes() const {
  if (!m_displaced_instructions_sp)
    return {};
  return {m_displaced_instructions_sp->GetBytes(),
          m_displaced_instructions_sp->GetByteSize()};
}

BreakpointInjectedSite::~BreakpointInjectedSite() {
  Log *log = GetLog(LLDBLog::JITLoader);

  if (!m_target_sp)
    return;

  ProcessSP process_sp = m_target_sp->GetProcessSP();

  // Take the patch out first. While the branch is installed a thread can enter
  // the trampoline at any moment, so the trampoline has to outlive it.
  //
  // This doubles as the unwind path for a trampoline builder that fails after
  // patching: dropping the site restores the inferior. Disabling the site
  // already did this in the common case, and the write is idempotent, so a
  // disable followed by destruction does not write twice.
  if (process_sp)
    LLDB_LOG_ERROR(log, process_sp->DisableInjectedBreakpoint(*this),
                   "FCB: couldn't take the patch out: {0}");

  // Now that nothing can reach the trampoline, stop describing it. Keeping the
  // module would leave a symbol and an unwind plan pointing at memory that is
  // about to be handed back.
  if (m_trampoline_module_sp) {
    m_target_sp->GetImages().Remove(m_trampoline_module_sp);
    m_trampoline_module_sp.reset();
  }

  if (process_sp && m_trampoline_addr != LLDB_INVALID_ADDRESS)
    process_sp->FreeFCBTrampolineAllocation(m_trampoline_addr);
}

bool BreakpointInjectedSite::BuildConditionExpression(void) {
  Log *log = GetLog(LLDBLog::JITLoader);

  Status error;

  std::string trap;
  std::string condition_text;
  bool single_condition = true;

  LanguageType language = eLanguageTypeUnknown;

  for (BreakpointLocationSP loc_sp : m_constituents.BreakpointLocations()) {
    std::string condition = loc_sp->GetCondition().GetText().str();

    // Stop building the expression if a location condition is not JIT-ed
    if (!loc_sp->GetInjectCondition()) {
      LLDB_LOG(log, "FCB: BreakpointLocation ({0}) condition is not JIT-ed",
               condition);
      return false;
    }

    // See if we can figure out the language from the frame, otherwise use the
    // default language:
    CompileUnit *comp_unit =
        loc_sp->GetAddress().CalculateSymbolContextCompileUnit();
    if (comp_unit)
      language = comp_unit->GetLanguage();

    if (language == eLanguageTypeSwift) {
      trap += "Builtin.int_trap()";
    } else if (Language::LanguageIsCFamily(language)) {
      trap = "__builtin_trap()";
    } else {
      LLDB_LOG(log, "FCB: Language {0} not supported",
               Language::GetNameForLanguageType(language));
      m_condition_expression_sp.reset();
      return false;
    }

    condition_text += (single_condition) ? "if (" : " || ";
    condition_text += condition;

    single_condition = false;
  }

  condition_text += ") {\n\t";

  condition_text += trap + ";\n    }";

  LLDB_LOG_VERBOSE(log, "Injected Condition:\n{0}\n", condition_text.c_str());

  DiagnosticManager diagnostics;

  EvaluateExpressionOptions options;
  options.SetInjectCondition(true);
  options.SetKeepInMemory(true);
  options.SetGenerateDebugInfo(true);

  m_condition_expression_sp.reset(m_target_sp->GetUserExpressionForLanguage(
      condition_text, llvm::StringRef(), SourceLanguage(language),
      Expression::eResultTypeAny, options, nullptr, error));

  if (error.Fail()) {
    if (log)
      log->Printf("Error getting condition expression: %s.", error.AsCString());
    m_condition_expression_sp.reset();
    return false;
  }

  diagnostics.Clear();

  ThreadSP thread_sp = m_target_sp->GetProcessSP()
                           ->GetThreadList()
                           .GetExpressionExecutionThread();

  user_id_t frame_idx = -1;
  user_id_t concrete_frame_idx = -1;
  addr_t cfa = LLDB_INVALID_ADDRESS;
  bool cfa_is_valid = false;
  addr_t pc = LLDB_INVALID_ADDRESS;
  StackFrame::Kind frame_kind = StackFrame::Kind::Regular;
  bool artificial = false;
  bool zeroth_frame = false;
  SymbolContext sc;
  m_real_addr.CalculateSymbolContext(&sc);

  StackFrameSP frame_sp = std::make_shared<StackFrame>(
      thread_sp, frame_idx, concrete_frame_idx, cfa, cfa_is_valid, pc,
      frame_kind, artificial, zeroth_frame, &sc);

  m_owner_exe_ctx = ExecutionContext(frame_sp);
  ExecutionPolicy execution_policy = eExecutionPolicyAlways;
  bool keep_result_in_memory = true;
  bool generate_debug_info = true;

  if (!m_condition_expression_sp->Parse(diagnostics, m_owner_exe_ctx,
                                        execution_policy, keep_result_in_memory,
                                        generate_debug_info)) {
    LLDB_LOG(log, "Couldn't parse conditional expression:\n{0}",
             diagnostics.GetString().c_str());
    m_condition_expression_sp.reset();
    return false;
  }

  const AddressRange &jit_addr_range =
      m_condition_expression_sp->GetJITAddressRange();

  error.Clear();

  WritableDataBufferSP buffer(
      new DataBufferHeap(jit_addr_range.GetByteSize(), 0));

  lldb::addr_t jit_addr =
      jit_addr_range.GetBaseAddress().GetCallableLoadAddress(m_target_sp.get());

  size_t memory_read = m_target_sp->GetProcessSP()->ReadMemory(
      jit_addr, buffer->GetBytes(), jit_addr_range.GetByteSize(), error);

  if (memory_read != jit_addr_range.GetByteSize() || error.Fail()) {
    m_condition_expression_sp.reset();
    LLDB_LOG(log, "FCB: Couldn't read jit memory");
    return false;
  }

  PlatformSP platform_sp = m_target_sp->GetPlatform();

  if (!platform_sp) {
    LLDB_LOG(log, "FCB: Couldn't get running platform");
    return false;
  }

  if (!platform_sp->GetSoftwareBreakpointTrapOpcode(*m_target_sp.get(), this)) {
    LLDB_LOG(log, "FCB: Couldn't get current architecture trap opcode");
    return false;
  }

  if (!ResolveTrapAddress(buffer->GetBytes(), memory_read)) {
    LLDB_LOG(log, "FCB: Couldn't find trap in jitted expression");
    return false;
  }

  if (!GatherArgumentsMetadata()) {
    LLDB_LOG(log, "FCB: Couldn't gather argument metadata");
    return false;
  }

  if (!CreateArgumentsStructure()) {
    LLDB_LOG(log, "FCB: Couldn't create argument structure");
    return false;
  }

  return true;
}

bool BreakpointInjectedSite::ResolveTrapAddress(void *jit, size_t size) {
  Log *log = GetLog(LLDBLog::JITLoader);

  const ABISP abi_sp = m_target_sp->GetProcessSP()->GetABI();
  const ArchSpec &arch = m_target_sp->GetArchitecture();
  const char *plugin_name = nullptr;
  const char *flavor = nullptr;
  const char *cpu = nullptr;
  const char *features = nullptr;
  const bool prefer_file_cache = true;

  m_disassembler_sp = Disassembler::DisassembleRange(
      arch, plugin_name, flavor, cpu, features, *m_target_sp.get(),
      m_condition_expression_sp->GetJITAddressRange(), prefer_file_cache);

  if (!m_disassembler_sp) {
    LLDB_LOG(log, "FCB: Couldn't disassemble JIT-ed expression");
    return false;
  }

  InstructionList &instructions = m_disassembler_sp->GetInstructionList();

  if (!instructions.GetSize()) {
    LLDB_LOG(log, "FCB: No instructions found for JIT-ed expression");
    return false;
  }

  auto abi_debug_trap_opcode = abi_sp->GetDebugTrapOpcode();

  for (size_t i = 0; i < instructions.GetSize(); i++) {
    InstructionSP instr = instructions.GetInstructionAtIndex(i);

    DataExtractor data;
    instr->GetData(data);

    const size_t trap_size = instr->Decode(*m_disassembler_sp.get(), data, 0);

    const void *instr_opcode = instr->GetOpcode().GetOpcodeDataBytes();

    if (!instr_opcode) {
      return false;
    }

    if (!abi_debug_trap_opcode) {
      LLDB_LOG(log, "FCB: No ABI debug_trap opcode found.");
      return false;
    }

    // Within a same platform, the compiler can generate different opcodes for
    // the same debug trap builtin. https://reviews.llvm.org/D84014
    for (auto &abi_trap_code : *abi_debug_trap_opcode) {
      // Compare the whole candidate opcode and nothing beyond it: trap_size is
      // the size of the instruction being examined, which can be larger.
      if (trap_size == abi_trap_code.size() &&
          !memcmp(instr_opcode, abi_trap_code.data(), abi_trap_code.size())) {
        addr_t addr =
            instr->GetAddress().GetOpcodeLoadAddress(m_target_sp.get());
        m_trap_addr = Address(addr);
        LLDB_LOG_VERBOSE(log, "Injected trap address: {0:X+}", addr);
        return true;
      }
    }
  }
  return false;
}

llvm::DataExtractor
BreakpointInjectedSite::GetLLVMDataExtractor(const DataExtractor &lldb_data) {
  // The expression bytes are binary, not a string: they routinely contain zero
  // bytes both as opcodes and inside operands, so the length has to come from
  // the buffer rather than from a terminator.
  llvm::StringRef data(reinterpret_cast<const char *>(lldb_data.GetDataStart()),
                       lldb_data.GetByteSize());
  bool is_le = (lldb_data.GetByteOrder() == lldb::eByteOrderLittle);
  return llvm::DataExtractor(data, is_le);
}

bool BreakpointInjectedSite::GatherArgumentsMetadata() {
  Log *log = GetLog(LLDBLog::JITLoader);

  LanguageType native_language =
      m_condition_expression_sp->Language().AsLanguageType();

  if (!Language::LanguageIsCFamily(native_language)) {
    LLDB_LOG(log, "FCB: {0} language does not support Injected Conditional \
             Breapoint",
             Language::GetNameForLanguageType(native_language));
    return false;
  }

  auto captured_or_err = m_condition_expression_sp->GetCapturedVariables();

  if (!captured_or_err) {
    LLDB_LOG_ERROR(log, captured_or_err.takeError(),
                   "FCB: Couldn't gather the captured variables: {0}");
    return false;
  }

  // The layout is authoritative, including its padding. Accumulating a size
  // from the variables this pass happens to understand would disagree with what
  // the condition expression was compiled to read.
  m_args_struct_size = captured_or_err->struct_size;

  for (const UserExpression::CapturedVariable &captured :
       captured_or_err->variables) {
    const ExpressionVariableSP &expr_var = captured.variable;
    ValueObjectSP val_obj_sp = expr_var->GetValueObject();

    // Every captured variable occupies its slot whether or not this pass can
    // describe it, so one that cannot be described has to fail the whole
    // install. Skipping it would leave its slot holding whatever was on the
    // trampoline's stack and the condition would read that.
    if (!val_obj_sp || !val_obj_sp->GetVariable()) {
      LLDB_LOG(log,
               "FCB: captured variable {0} has no variable to locate, so the "
               "condition cannot be evaluated in the inferior",
               expr_var->GetName());
      return false;
    }

    VariableSP var_sp = val_obj_sp->GetVariable();

    DWARFExpressionList lldb_dwarf_exprs = var_sp->LocationExpressionList();

    DataExtractor lldb_data;
    if (!lldb_dwarf_exprs.GetExpressionData(lldb_data)) {
      return false;
    }

    llvm::DataExtractor llvm_data = GetLLVMDataExtractor(lldb_data);

    uint8_t addr_size = m_target_sp->GetArchitecture().GetAddressByteSize();

    auto size = var_sp->GetType()->GetByteSize(m_target_sp.get());
    if (!size) {
      LLDB_LOG(log, "FCB: Variable {0} has invalid size",
               var_sp->GetName().GetCString());
      return false;
    }

    SymbolContextScope *owner_scope = var_sp->GetSymbolContextScope();
    Function *func = nullptr;
    if (!owner_scope ||
        !(func = owner_scope->CalculateSymbolContextFunction())) {
      LLDB_LOG(log,
               "FCB: variable {0} has no enclosing function, so its frame base "
               "cannot be resolved",
               var_sp->GetName());
      return false;
    }

    // FIXME: const ref ?
    DWARFExpressionList frame_base_expr = func->GetFrameBaseExpression();

    VariableMetadata metadata(expr_var->GetName().GetCString(), *size,
                              captured.offset, llvm_data, addr_size,
                              lldb_dwarf_exprs, frame_base_expr);

    m_metadatas.push_back(metadata);
  }

  m_condition_expression_sp->ResetCapturedVariables();

  return true;
}

bool BreakpointInjectedSite::CreateArgumentsStructure() {
  Log *log = GetLog(LLDBLog::JITLoader);

  Status error;
  std::string expr;
  expr.reserve(2048);
  std::string name = "$__lldb_create_args_struct";

  ProcessSP process_sp = m_owner_exe_ctx.GetProcessSP();
  ABISP abi_sp = process_sp ? process_sp->GetABI() : nullptr;

  if (!abi_sp) {
    LLDB_LOG(log, "FCB: Couldn't get the target's ABI");
    return false;
  }

  // Deliberately no external declarations: this runs while the breakpoint is
  // being installed, when the inferior may not have loaded the libraries its
  // symbols would come from yet, and every store below is pointer sized anyway,
  // so nothing here needs a call.
  expr += abi_sp->GetRegisterContextAsString();

  expr += "\n\n"
          "intptr_t $__lldb_create_args_struct(register_context* regs, "
          "intptr_t arg_struct) {\n";

  for (size_t index = 0; index < m_metadatas.size(); index++) {
    expr += ParseDWARFExpression(index, error);
    if (error.Fail()) {
      LLDB_LOG(log, "FCB: Couldn't parse DWARFExpression ({0}/{1})", index,
               m_metadatas.size());
      return false;
    }
  }

  expr += "\n";

  expr += "   return arg_struct;\n"
          "}\n";

  auto utility_fn_or_error = m_target_sp->CreateUtilityFunction(
      expr, name, eLanguageTypeC, m_owner_exe_ctx);

  if (!utility_fn_or_error) {
    LLDB_LOG_ERROR(log, utility_fn_or_error.takeError(),
                   "FCB: Couldn't compile the argument structure builder: {0}");
    LLDB_LOG_VERBOSE(log, "FCB: Argument structure builder source:\n{0}", expr);
    m_create_args_struct_function_sp.reset();
    return false;
  }

  m_create_args_struct_function_sp = std::move(*utility_fn_or_error);

  return true;
}

std::string BreakpointInjectedSite::ParseDWARFExpression(size_t expr_idx,
                                                         Status &error) {
  std::string expr;
  ProcessSP process_sp = m_owner_exe_ctx.GetProcessSP();
  ABISP abi_sp = process_sp ? process_sp->GetABI() : nullptr;

  if (!abi_sp) {
    error = Status::FromErrorString("Couldn't get the target's ABI");
    return "";
  }

  auto resolve_frame_base =
      [&](llvm::DWARFExpression::Operation &op) -> llvm::Expected<std::string> {
    uint8_t opcode = op.getCode();
    switch (opcode) {
    case llvm::dwarf::DW_OP_const1u:
    case llvm::dwarf::DW_OP_const1s:
    case llvm::dwarf::DW_OP_addr:
      return std::to_string(op.getRawOperand(0));
    case llvm::dwarf::DW_OP_reg0:
    case llvm::dwarf::DW_OP_reg1:
    case llvm::dwarf::DW_OP_reg2:
    case llvm::dwarf::DW_OP_reg3:
    case llvm::dwarf::DW_OP_reg4:
    case llvm::dwarf::DW_OP_reg5:
    case llvm::dwarf::DW_OP_reg6:
    case llvm::dwarf::DW_OP_reg7:
    case llvm::dwarf::DW_OP_reg8:
    case llvm::dwarf::DW_OP_reg9:
    case llvm::dwarf::DW_OP_reg10:
    case llvm::dwarf::DW_OP_reg11:
    case llvm::dwarf::DW_OP_reg12:
    case llvm::dwarf::DW_OP_reg13:
    case llvm::dwarf::DW_OP_reg14:
    case llvm::dwarf::DW_OP_reg15:
    case llvm::dwarf::DW_OP_reg16:
    case llvm::dwarf::DW_OP_reg17:
    case llvm::dwarf::DW_OP_reg18:
    case llvm::dwarf::DW_OP_reg19:
    case llvm::dwarf::DW_OP_reg20:
    case llvm::dwarf::DW_OP_reg21:
    case llvm::dwarf::DW_OP_reg22:
    case llvm::dwarf::DW_OP_reg23:
    case llvm::dwarf::DW_OP_reg24:
    case llvm::dwarf::DW_OP_reg25:
    case llvm::dwarf::DW_OP_reg26:
    case llvm::dwarf::DW_OP_reg27:
    case llvm::dwarf::DW_OP_reg28:
    case llvm::dwarf::DW_OP_reg29:
    case llvm::dwarf::DW_OP_reg30:
    case llvm::dwarf::DW_OP_reg31:
    case llvm::dwarf::DW_OP_breg0:
    case llvm::dwarf::DW_OP_breg1:
    case llvm::dwarf::DW_OP_breg2:
    case llvm::dwarf::DW_OP_breg3:
    case llvm::dwarf::DW_OP_breg4:
    case llvm::dwarf::DW_OP_breg5:
    case llvm::dwarf::DW_OP_breg6:
    case llvm::dwarf::DW_OP_breg7:
    case llvm::dwarf::DW_OP_breg8:
    case llvm::dwarf::DW_OP_breg9:
    case llvm::dwarf::DW_OP_breg10:
    case llvm::dwarf::DW_OP_breg11:
    case llvm::dwarf::DW_OP_breg12:
    case llvm::dwarf::DW_OP_breg13:
    case llvm::dwarf::DW_OP_breg14:
    case llvm::dwarf::DW_OP_breg15:
    case llvm::dwarf::DW_OP_breg16:
    case llvm::dwarf::DW_OP_breg17:
    case llvm::dwarf::DW_OP_breg18:
    case llvm::dwarf::DW_OP_breg19:
    case llvm::dwarf::DW_OP_breg20:
    case llvm::dwarf::DW_OP_breg21:
    case llvm::dwarf::DW_OP_breg22:
    case llvm::dwarf::DW_OP_breg23:
    case llvm::dwarf::DW_OP_breg24:
    case llvm::dwarf::DW_OP_breg25:
    case llvm::dwarf::DW_OP_breg26:
    case llvm::dwarf::DW_OP_breg27:
    case llvm::dwarf::DW_OP_breg28:
    case llvm::dwarf::DW_OP_breg29:
    case llvm::dwarf::DW_OP_breg30:
    case llvm::dwarf::DW_OP_breg31: {
      bool has_offset = (opcode > llvm::dwarf::DW_OP_reg31);
      uint8_t reg_num = opcode - (has_offset ? llvm::dwarf::DW_OP_breg0
                                             : llvm::dwarf::DW_OP_reg0);
      uint8_t reg_offset = has_offset ? op.getRawOperand(0) : 0;

      auto reg_name_or_err = abi_sp->GetRegisterName(reg_num);
      if (!reg_name_or_err)
        return llvm::createStringError(
            llvm::formatv("Failed to resolve frame base attribute: {0}.",
                          llvm::fmt_consume(reg_name_or_err.takeError())));
      std::string reg_name = *reg_name_or_err;
      reg_name += " + " + std::to_string(reg_offset);
      return reg_name;
    } break;
    default: {
      return llvm::createStringError(
          llvm::formatv("Failed to resolve frame base attribute: Unsupported "
                        "DWARF opcode ({0}). DW_OP_call_frame_cfa, which GCC "
                        "emits, needs the function's CFA rule and is not "
                        "supported yet.",
                        opcode));
    }
    }
  };

  std::vector<std::string> frame_bases;
  VariableMetadata &var_metadata = m_metadatas[expr_idx];
  DataExtractor fb_expr_data;
  if (var_metadata.frame_base_expr_list.GetExpressionData(fb_expr_data)) {
    llvm::DataExtractor data = GetLLVMDataExtractor(fb_expr_data);
    llvm::DWARFExpression fb_expr(data, fb_expr_data.GetAddressByteSize());
    for (auto op : fb_expr) {
      auto fb_or_err = resolve_frame_base(op);
      if (!fb_or_err) {
        LLDB_LOG_ERROR(GetLog(LLDBLog::JITLoader), fb_or_err.takeError(),
                       "FCB: {0}");
        continue;
      }
      frame_bases.push_back(std::move(*fb_or_err));
    }
  }

  // A variable has one location, so exactly one operation may produce it. A
  // multi-operation expression is a stack program this pass cannot evaluate, and
  // running the loop over each operation in turn would emit a store per
  // operation and leave the slot holding whichever came last.
  const llvm::DWARFExpression &ops = var_metadata.dwarf;
  if (ops.begin() == ops.end()) {
    error = Status::FromErrorStringWithFormat(
        "variable '%s' has an empty location expression",
        var_metadata.name.c_str());
    return "";
  }
  if (std::next(ops.begin()) != ops.end()) {
    error = Status::FromErrorStringWithFormat(
        "the location of variable '%s' is computed by more than one DWARF "
        "operation, which cannot yet be evaluated in the inferior",
        var_metadata.name.c_str());
    return "";
  }

  const lldb::offset_t dest_offset = var_metadata.offset;
  for (auto op : ops) {
    switch (op.getCode()) {
    case llvm::dwarf::DW_OP_const1u:
    case llvm::dwarf::DW_OP_const1s: {
      int64_t operand = op.getRawOperand(0);
      expr += "   *(void **)(arg_struct + " + std::to_string(dest_offset) +
              ") = (void *)" + std::to_string(operand) + ";\n";
      break;
    }
    case llvm::dwarf::DW_OP_addr: {
      // The operand is a link-time file address. Emitting it as written would
      // have the inferior dereference an address that is only correct for a
      // module loaded with no slide.
      Address file_addr;
      const lldb::addr_t raw = op.getRawOperand(0);
      if (!m_target_sp->ResolveFileAddress(raw, file_addr)) {
        error = Status::FromErrorStringWithFormat(
            "couldn't find the section holding the address of variable '%s'",
            var_metadata.name.c_str());
        return "";
      }
      const lldb::addr_t load_addr = file_addr.GetLoadAddress(m_target_sp.get());
      if (load_addr == LLDB_INVALID_ADDRESS) {
        error = Status::FromErrorStringWithFormat(
            "variable '%s' lives in a section that is not loaded",
            var_metadata.name.c_str());
        return "";
      }
      expr += "   *(void **)(arg_struct + " + std::to_string(dest_offset) +
              ") = (void *)" + std::to_string(load_addr) + "ULL;\n";
      break;
    }
    case llvm::dwarf::DW_OP_fbreg: {
      // Without a resolved frame base there is nothing to offset from, and
      // reading frame_bases.front() here would be undefined behavior.
      if (frame_bases.empty()) {
        error = Status::FromErrorString(
            "Couldn't resolve the frame base of the enclosing function");
        return "";
      }

      int64_t operand = op.getRawOperand(0);
      expr += "   *(void **)(arg_struct + " + std::to_string(dest_offset) +
              ") = (void *)(regs->" + frame_bases.front() + " + " +
              std::to_string(operand) + ");\n";
      break;
    }
    case llvm::dwarf::DW_OP_breg0:
    case llvm::dwarf::DW_OP_breg1:
    case llvm::dwarf::DW_OP_breg2:
    case llvm::dwarf::DW_OP_breg3:
    case llvm::dwarf::DW_OP_breg4:
    case llvm::dwarf::DW_OP_breg5:
    case llvm::dwarf::DW_OP_breg6:
    case llvm::dwarf::DW_OP_breg7:
    case llvm::dwarf::DW_OP_breg8:
    case llvm::dwarf::DW_OP_breg9:
    case llvm::dwarf::DW_OP_breg10:
    case llvm::dwarf::DW_OP_breg11:
    case llvm::dwarf::DW_OP_breg12:
    case llvm::dwarf::DW_OP_breg13:
    case llvm::dwarf::DW_OP_breg14:
    case llvm::dwarf::DW_OP_breg15:
    case llvm::dwarf::DW_OP_breg16:
    case llvm::dwarf::DW_OP_breg17:
    case llvm::dwarf::DW_OP_breg18:
    case llvm::dwarf::DW_OP_breg19:
    case llvm::dwarf::DW_OP_breg20:
    case llvm::dwarf::DW_OP_breg21:
    case llvm::dwarf::DW_OP_breg22:
    case llvm::dwarf::DW_OP_breg23:
    case llvm::dwarf::DW_OP_breg24:
    case llvm::dwarf::DW_OP_breg25:
    case llvm::dwarf::DW_OP_breg26:
    case llvm::dwarf::DW_OP_breg27:
    case llvm::dwarf::DW_OP_breg28:
    case llvm::dwarf::DW_OP_breg29:
    case llvm::dwarf::DW_OP_breg30:
    case llvm::dwarf::DW_OP_breg31: {
      uint8_t reg_num = op.getCode() - llvm::dwarf::DW_OP_breg0;
      // Signed: the operand is a SLEB128, so reading it unsigned turns a stack
      // offset of -24 into 18446744073709551592.
      int64_t reg_offset = static_cast<int64_t>(op.getRawOperand(0));

      // The operand is a DWARF register number, so it has to be resolved
      // through the ABI like the frame base above. Going through the
      // RegisterContext instead would index its own register array and produce
      // the name of an unrelated register.
      auto reg_name_or_err = abi_sp->GetRegisterName(reg_num);
      if (!reg_name_or_err) {
        error = Status::FromError(reg_name_or_err.takeError());
        return "";
      }

      expr += "   *(void **)(arg_struct + " + std::to_string(dest_offset) +
              ") = (void *)(regs->" + *reg_name_or_err + " + " +
              std::to_string(reg_offset) + ");\n";
    } break;
    default:
      // Refuse rather than leave the slot unwritten. The condition expression
      // reads every slot at the offset its own layout assigned, so an unwritten
      // one is read as whatever the trampoline's stack happened to hold, and the
      // condition answers on garbage instead of falling back to the debugger.
      error = Status::FromErrorStringWithFormat(
          "the location of variable '%s' uses %s, which cannot yet be evaluated "
          "in the inferior",
          var_metadata.name.c_str(),
          llvm::dwarf::OperationEncodingString(op.getCode()).str().c_str());
      return "";
    }
  }

  return expr;
}
