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

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatAdapters.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <iterator>
#include <optional>

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

/// Wrap \a lldb_data in the extractor llvm's DWARF expression decoder takes.
///
/// The expression bytes are binary, not a string: they routinely contain zero
/// bytes both as opcodes and inside operands, so the length has to come from
/// the buffer rather than from a terminator.
static llvm::DataExtractor
GetLLVMDataExtractor(const DataExtractor &lldb_data) {
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

  // A condition that reads no variable from the program is a constant, so there
  // is nothing for the inferior to decide that could not have been decided when
  // the breakpoint was set. Refuse rather than build a trampoline, a JIT-ed
  // expression and an argument structure to evaluate it.
  //
  // This also keeps the feature away from a case it does not handle correctly:
  // with no variables the argument structure is zero bytes, so the trampoline
  // reserves nothing for it and hands the condition a pointer to its own saved
  // register context, and the unwind out of the trampoline picks the wrong plan
  // and loses the user's frame.
  if (captured_or_err->variables.empty()) {
    LLDB_LOG(log,
             "FCB: the condition reads no variables, so evaluating it in the "
             "inferior would decide nothing");
    return false;
  }

  // The layout is authoritative, including its padding. Accumulating a size
  // from the variables this pass happens to understand would disagree with what
  // the condition expression was compiled to read.
  m_args_struct_size = captured_or_err->struct_size;

  // Rebuilding a site starts from the captured variables again, so anything
  // gathered by a previous attempt describes slots this layout no longer has.
  m_metadatas.clear();

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

    auto size = var_sp->GetType()->GetByteSize(m_target_sp.get());
    if (!size) {
      LLDB_LOG(log, "FCB: Variable {0} has invalid size",
               var_sp->GetName().GetCString());
      return false;
    }

    // Looked up before the location is selected, not only for the frame base
    // below, because selecting from a location list needs the function's load
    // address to convert this site's pc back into the file address the list is
    // keyed by.
    //
    // A global has no enclosing function and needs neither: its location is a
    // single always-valid DW_OP_addr, which the selection below returns without
    // consulting an address, and which is relative to nothing. So this is
    // recorded rather than refused, and a variable that turns out to need
    // either is refused where it is needed.
    SymbolContextScope *owner_scope = var_sp->GetSymbolContextScope();
    Function *func =
        owner_scope ? owner_scope->CalculateSymbolContextFunction() : nullptr;

    DWARFExpressionList lldb_dwarf_exprs = var_sp->LocationExpressionList();

    // Select the location that covers this site.
    //
    // A variable with one location is always valid and was found whatever was
    // passed here, which is why this worked at all. A variable with a location
    // list was not: the default arguments are an invalid function address and a
    // pc of zero, and the conversion back to a file address then yields zero,
    // which no entry contains. So every location list refused the whole
    // install.
    //
    // The site's own address is the one to ask about, and it is the user's
    // breakpoint address rather than the patched site's: the trampoline runs on
    // its behalf, before the displaced instruction.
    const addr_t func_load_addr =
        func ? func->GetAddress().GetLoadAddress(m_target_sp.get())
             : LLDB_INVALID_ADDRESS;
    const addr_t site_load_addr = m_real_addr.GetLoadAddress(m_target_sp.get());

    std::optional<DWARFExpressionList::DWARFExpressionEntry> location =
        lldb_dwarf_exprs.GetExpressionEntryAtAddress(func_load_addr,
                                                     site_load_addr);
    // An entry with an empty expression is how DWARF says the variable has no
    // location over that range, so it reads the same way as no entry at all.
    DataExtractor lldb_data;
    if (!location || !location->expr ||
        !location->expr->GetExpressionData(lldb_data)) {
      LLDB_LOG(log,
               "FCB: variable {0} has no location at {1:x}, so its condition "
               "cannot be evaluated in the inferior",
               var_sp->GetName(), site_load_addr);
      return false;
    }

    uint8_t addr_size = m_target_sp->GetArchitecture().GetAddressByteSize();

    // Empty for a global, which is what makes DW_OP_fbreg the thing that
    // refuses rather than the lookup above.
    DWARFExpressionList frame_base_expr =
        func ? func->GetFrameBaseExpression() : DWARFExpressionList();

    // Copied rather than referenced. The location expression's bytes belong to
    // the module's DWARF section, reached through a chain of extractors that
    // are all local to this loop, and the metadata outlives every one of them.
    lldb::DataBufferSP location_bytes = std::make_shared<DataBufferHeap>(
        lldb_data.GetDataStart(), lldb_data.GetByteSize());

    m_metadatas.emplace_back(expr_var->GetName().GetCString(), *size,
                             captured.offset, std::move(location_bytes),
                             lldb_data.GetByteOrder() == lldb::eByteOrderLittle,
                             addr_size, lldb_dwarf_exprs, frame_base_expr,
                             func_load_addr);
  }

  m_condition_expression_sp->ResetCapturedVariables();

  return true;
}

namespace {

/// The DWARF stack machine, lowered into C instead of interpreted.
///
/// A variable's location at a given pc is a fixed formula over register values:
/// the DWARF says how to compute the address, and only the register values
/// change from one hit to the next. So the expression is walked once, when the
/// breakpoint is armed, and turned into C statements that the argument
/// structure builder already compiles into the inferior. clang is the
/// evaluator, which is what keeps this from being a second implementation of
/// DWARF expression semantics to hold in step with lldb's own.
///
/// Nothing here decodes an opcode. The decoder is llvm::DWARFExpression, the
/// same one DWARFExpression::Evaluate() walks, so a fix to it is a fix to both.
///
/// The stack is lowered to named locals with statically known indices rather
/// than to an array with a running index, so the generated code cannot run off
/// either end of it and there is no bounds check to pay for on the hot path. An
/// expression whose depth is not statically determined, or that could loop, is
/// refused when the breakpoint is armed instead.
class DWARFLocationEmitter {
public:
  DWARFLocationEmitter(Target &target, ABI &abi,
                       const BreakpointInjectedSite::VariableMetadata &metadata,
                       lldb::addr_t site_load_addr, unsigned index)
      : m_target(target), m_abi(abi), m_metadata(metadata),
        m_site_load_addr(site_load_addr), m_index(index) {}

  /// Decode the location and decide what the emitted code will look like,
  /// without emitting it.
  ///
  /// Separate from Emit() because a location that produces a value rather than
  /// an address needs somewhere to put it, those slots live past the end of the
  /// argument structure, and their offsets are only known once every variable
  /// has been looked at.
  llvm::Error Analyze();

  /// How many bytes past the end of the condition's own layout this location
  /// needs. Zero unless the variable has to be assembled or copied somewhere.
  size_t GetScratchSize() const {
    // A composite is copied piece by piece into a buffer the size of the whole
    // variable. A single implicit value needs one word to sit in.
    if (m_is_composite)
      return llvm::alignTo(m_metadata.size, m_addr_size);
    return m_pieces.front().kind == ResultKind::ImplicitValue ? m_addr_size : 0;
  }

  /// Render the C statements that store this variable's address into the
  /// argument structure.
  ///
  /// Infallible: everything that could be refused was refused by Analyze().
  std::string Emit(lldb::offset_t scratch_offset) const;

  /// The typedefs the emitted code is written against, emitted once ahead of
  /// every variable.
  ///
  /// Spelled with clang's predefined macros rather than with `intptr_t` so that
  /// nothing here needs a header: this is compiled while the breakpoint is
  /// being installed, when the inferior may not have loaded the library a
  /// header's symbols would come from. That is also why a composite is
  /// assembled with these rather than with memcpy.
  static llvm::StringRef GetPrologue() {
    return "typedef __INTPTR_TYPE__ $__lldb_word;\n"
           "typedef __UINTPTR_TYPE__ $__lldb_uword;\n"
           "/* Packed, because a piece of a variable can start at any byte, "
           "and "
           "a plain\n"
           "   cast to a wider type at a misaligned address is undefined. */\n"
           "typedef struct { unsigned char v; } "
           "__attribute__((packed)) $__lldb_b1;\n"
           "typedef struct { unsigned short v; } "
           "__attribute__((packed)) $__lldb_b2;\n"
           "typedef struct { unsigned int v; } "
           "__attribute__((packed)) $__lldb_b4;\n"
           "typedef struct { unsigned long long v; } "
           "__attribute__((packed)) $__lldb_b8;\n";
  }

private:
  /// Marks an operation the walk never reached, which is the only way a slot
  /// index can be unknown.
  static constexpr int kUnreached = -1;
  /// How deep a stack the generated code will declare. Real location
  /// expressions use two or three slots.
  static constexpr int kMaxDepth = 32;
  /// A bound on the generated text. Real location expressions are a handful of
  /// operations long.
  static constexpr size_t kMaxOperations = 256;

  /// The C type of a stack slot, and its unsigned counterpart for the
  /// operations DWARF defines as unsigned.
  static constexpr const char *kWord = "$__lldb_word";
  static constexpr const char *kUWord = "$__lldb_uword";

  /// What the emitted code hands to the condition expression.
  enum class ResultKind {
    /// The top of the stack is the address of the variable. This is DWARF's
    /// plain memory location description, and every unoptimized local is one.
    MemoryAddress,
    /// The variable lives in a register, so its address is the address of the
    /// slot the trampoline saved that register into.
    RegisterSlot,
    /// DW_OP_stack_value: the computed value *is* the variable, so there is no
    /// address to hand over until the builder writes it somewhere.
    ImplicitValue,
  };

  /// What an operation does to the stack.
  struct Effect {
    /// How many values have to be on the stack before it runs.
    int needs = 0;
    /// The change in depth once it has run.
    int delta = 0;
  };

  struct Op {
    llvm::DWARFExpression::Operation op;
    /// Its byte offset in the expression, which is what a branch names.
    uint64_t offset = 0;
    Effect effect;
    /// The stack depth on entry, or kUnreached until the walk gets here.
    int depth = kUnreached;
    /// Set when some DW_OP_skip or DW_OP_bra targets this offset.
    bool is_label = false;
    /// For the operations whose operand has to be resolved against the target,
    /// the C expression producing the value they push. Empty for the rest.
    std::string value;
  };

  /// One simple location description, and how much of the variable it covers.
  ///
  /// A location that is not a composite is a single piece covering all of it,
  /// which is what lets the composite and the ordinary case share every pass.
  struct Piece {
    /// Its operations, as a range into m_ops. A DW_OP_piece separator is in
    /// neither the piece before it nor the piece after it.
    size_t first = 0;
    size_t count = 0;
    /// Where evaluating it stops: the offset of its DW_OP_piece separator, or
    /// the end of the expression for the last piece and for a non-composite.
    uint64_t end_offset = 0;
    /// The DW_OP_piece operand. Zero for a non-composite, where the piece is
    /// the location itself rather than a part to be copied.
    uint64_t bytes = 0;
    /// Where it goes in the assembled variable.
    uint64_t at = 0;
    ResultKind kind = ResultKind::MemoryAddress;
    /// For ResultKind::RegisterSlot, the field of `register_context` holding
    /// the register, already qualified as `regs->x19`.
    std::string reg;
    /// The depth on reaching the piece's end, which is where its result is.
    int end_depth = kUnreached;
    /// Set when a branch jumps to the end rather than control falling off it.
    bool end_is_label = false;
    /// How many slots this piece needs.
    int max_depth = 0;
  };

  /// What \a op does to the stack, or nothing if it cannot be emitted.
  ///
  /// This doubles as the list of supported operations: an opcode with no entry
  /// is one the generated code has no way to carry out, and the injection is
  /// refused rather than the variable silently dropped.
  static std::optional<Effect>
  GetEffect(const llvm::DWARFExpression::Operation &op);

  /// Analyze() runs these in order. Each one refuses rather than guessing, so
  /// that by the time Emit() runs there is nothing left that can fail.

  /// Decode the operations into m_ops, refusing an opcode that cannot be
  /// emitted and one whose operands are truncated.
  llvm::Error Decode();

  /// Cut m_ops into pieces at each DW_OP_piece, or make one piece of the lot if
  /// the location is not a composite.
  llvm::Error SplitPieces();

  /// Decide whether \a piece describes a register, an address, or a value,
  /// which is what says where its bytes have to be copied from.
  llvm::Error ClassifyResult(Piece &piece);

  /// Work out the stack depth at every operation of \a piece, and refuse the
  /// piece if that cannot be done.
  ///
  /// This is what lets the DWARF stack become plain C locals. A DWARF operation
  /// says "pop two, push one" without naming anything, so to write `s2 = s0 +
  /// s1` the emitter has to know that this particular `DW_OP_plus` runs with
  /// three values on the stack. That is a property of the path taken to reach
  /// it, not of the operation, so it has to be propagated.
  ///
  /// A worklist walk over the operations does it: start the piece's first
  /// operation at depth 0, and push each operation's successors with the depth
  /// its Effect leaves behind. Fall-through for most, both edges for
  /// `DW_OP_bra`, only the target for `DW_OP_skip`. Op::depth is the answer,
  /// and an operation running at depth d reads slot d-1 and writes slot d.
  ///
  /// Four things make a piece unusable, and each is refused here rather than
  /// mis-emitted:
  ///
  ///   - An operation reached at two different depths. Where paths join, one
  ///     of them would have to call the top of the stack by a different name,
  ///     and there is only one name to emit.
  ///   - An operation that needs more values than are on the stack, which is
  ///     DWARF popping past the bottom.
  ///   - A backward branch. Static depths do not rule out a loop, and a loop in
  ///     the generated code hangs the inferior with the user's breakpoint
  ///     nowhere in sight.
  ///   - A stack deeper than the generated code is willing to declare.
  ///
  /// It also leaves behind Piece::max_depth, which is how many locals to
  /// declare, and Piece::end_depth, which says which slot holds the result.
  ///
  /// The payoff is that the emitted code indexes nothing at runtime. There is
  /// no stack pointer to keep and no bounds check to pay for, because every
  /// slot was named here or the whole injection was refused.
  llvm::Error WalkDepths(Piece &piece);

  /// Mark the operations some branch jumps to, so that Emit() writes a label
  /// only where one is needed.
  llvm::Error MarkBranchTargets(Piece &piece);

  /// Resolve the operands that mean nothing without the target: register
  /// numbers through the ABI, DW_OP_addr through the section load list, and the
  /// frame base through the enclosing function.
  llvm::Error ResolveOperands();

  llvm::Expected<std::string> ResolveFrameBase();
  llvm::Expected<std::string> RegisterField(uint64_t dwarf_regnum) const;

  void EmitOperation(llvm::raw_ostream &os, const Op &op) const;
  void EmitPiece(llvm::raw_ostream &os, const Piece &piece,
                 lldb::offset_t scratch_offset) const;
  void EmitCopy(llvm::raw_ostream &os, uint64_t bytes) const;
  std::string Describe() const;

  /// Where a DW_OP_skip or DW_OP_bra goes, as a byte offset in the expression.
  ///
  /// Signed, because the operand is, and because a target before the start of
  /// the expression has to come out recognisably invalid rather than wrap.
  int64_t BranchTarget(const Op &op) const {
    return static_cast<int64_t>(op.op.getEndOffset()) +
           static_cast<int64_t>(op.op.getRawOperand(0));
  }

  std::string Slot(int index) const { return "s" + std::to_string(index); }

  std::string Label(int64_t offset) const {
    return "l" + std::to_string(m_index) + "_" + std::to_string(offset);
  }

  llvm::Error Refuse(llvm::StringRef reason) const {
    return llvm::createStringError(llvm::formatv(
        "the location of variable '{0}' {1}", m_metadata.name, reason));
  }

  llvm::Error RefuseOpcode(uint8_t code, llvm::StringRef reason) const {
    return Refuse(llvm::formatv("uses {0}, which {1}",
                                llvm::dwarf::OperationEncodingString(code),
                                reason)
                      .str());
  }

  Target &m_target;
  ABI &m_abi;
  const BreakpointInjectedSite::VariableMetadata &m_metadata;
  /// The pc the location was selected for, which is also what the frame base
  /// list has to be selected for.
  lldb::addr_t m_site_load_addr;
  /// Which variable this is, used only to keep emitted labels apart. Labels are
  /// function scoped in C, so a block per variable is not enough.
  unsigned m_index;

  /// The size of the expression in bytes, which is also the offset that means
  /// "the end".
  uint64_t m_size = 0;
  uint8_t m_addr_size = 0;
  /// The frame base as a C expression, resolved on first use because most
  /// locations never mention it.
  std::string m_frame_base;

  std::vector<Op> m_ops;
  llvm::DenseMap<uint64_t, size_t> m_offset_to_index;
  /// One entry unless the location is a composite, in which case one per
  /// DW_OP_piece, in the order they make up the variable.
  std::vector<Piece> m_pieces;
  /// Whether the variable is assembled from pieces rather than being in one
  /// place. Not the same as m_pieces.size() > 1, which never happens otherwise.
  bool m_is_composite = false;
};

/// The fewest bytes an operand of this encoding can occupy.
///
/// Used to tell a truncated operation from a complete one, so it has to be a
/// lower bound: a LEB128 is at least one byte and can be more.
static uint64_t MinimumOperandBytes(uint8_t encoding, uint8_t addr_size) {
  using Operation = llvm::DWARFExpression::Operation;
  switch (encoding & ~Operation::SignBit) {
  case Operation::Size1:
    return 1;
  case Operation::Size2:
    return 2;
  case Operation::Size4:
    return 4;
  case Operation::Size8:
    return 8;
  case Operation::SizeAddr:
    return addr_size;
  case Operation::SizeRefAddr:
    return 4;
  case Operation::SizeBlock:
    // A block's bytes are counted by the length operand ahead of it.
    return 0;
  default:
    return 1;
  }
}

std::optional<DWARFLocationEmitter::Effect>
DWARFLocationEmitter::GetEffect(const llvm::DWARFExpression::Operation &op) {
  const uint8_t code = op.getCode();

  // The register families, which are ranges of opcodes rather than single ones.
  if (code >= llvm::dwarf::DW_OP_lit0 && code <= llvm::dwarf::DW_OP_lit31)
    return Effect{0, 1};
  if (code >= llvm::dwarf::DW_OP_breg0 && code <= llvm::dwarf::DW_OP_breg31)
    return Effect{0, 1};
  // A register location description names where the variable is rather than
  // computing anything, so it leaves the stack alone.
  if (code >= llvm::dwarf::DW_OP_reg0 && code <= llvm::dwarf::DW_OP_reg31)
    return Effect{0, 0};

  switch (code) {
  // Values pushed from an operand.
  case llvm::dwarf::DW_OP_addr:
  case llvm::dwarf::DW_OP_const1u:
  case llvm::dwarf::DW_OP_const1s:
  case llvm::dwarf::DW_OP_const2u:
  case llvm::dwarf::DW_OP_const2s:
  case llvm::dwarf::DW_OP_const4u:
  case llvm::dwarf::DW_OP_const4s:
  case llvm::dwarf::DW_OP_const8u:
  case llvm::dwarf::DW_OP_const8s:
  case llvm::dwarf::DW_OP_constu:
  case llvm::dwarf::DW_OP_consts:
  case llvm::dwarf::DW_OP_fbreg:
  case llvm::dwarf::DW_OP_bregx:
    return Effect{0, 1};

  case llvm::dwarf::DW_OP_regx:
    return Effect{0, 0};

  // Stack shuffles.
  case llvm::dwarf::DW_OP_dup:
    return Effect{1, 1};
  case llvm::dwarf::DW_OP_over:
    return Effect{2, 1};
  case llvm::dwarf::DW_OP_pick:
    // Needs one more slot than the index it reaches back to.
    return Effect{static_cast<int>(op.getRawOperand(0)) + 1, 1};
  case llvm::dwarf::DW_OP_drop:
    return Effect{1, -1};
  case llvm::dwarf::DW_OP_swap:
    return Effect{2, 0};
  case llvm::dwarf::DW_OP_rot:
    return Effect{3, 0};

  // Operations on the top of the stack alone.
  case llvm::dwarf::DW_OP_deref:
  case llvm::dwarf::DW_OP_deref_size:
  case llvm::dwarf::DW_OP_abs:
  case llvm::dwarf::DW_OP_neg:
  case llvm::dwarf::DW_OP_not:
  case llvm::dwarf::DW_OP_plus_uconst:
    return Effect{1, 0};

  // Operations on the top two, leaving one.
  case llvm::dwarf::DW_OP_and:
  case llvm::dwarf::DW_OP_div:
  case llvm::dwarf::DW_OP_minus:
  case llvm::dwarf::DW_OP_mod:
  case llvm::dwarf::DW_OP_mul:
  case llvm::dwarf::DW_OP_or:
  case llvm::dwarf::DW_OP_plus:
  case llvm::dwarf::DW_OP_shl:
  case llvm::dwarf::DW_OP_shr:
  case llvm::dwarf::DW_OP_shra:
  case llvm::dwarf::DW_OP_xor:
  case llvm::dwarf::DW_OP_eq:
  case llvm::dwarf::DW_OP_ge:
  case llvm::dwarf::DW_OP_gt:
  case llvm::dwarf::DW_OP_le:
  case llvm::dwarf::DW_OP_lt:
  case llvm::dwarf::DW_OP_ne:
    return Effect{2, -1};

  // Control flow.
  case llvm::dwarf::DW_OP_skip:
  case llvm::dwarf::DW_OP_nop:
    return Effect{0, 0};
  case llvm::dwarf::DW_OP_bra:
    return Effect{1, -1};

  // Terminal, and it needs the value it is describing.
  case llvm::dwarf::DW_OP_stack_value:
    return Effect{1, 0};

  // A separator rather than an operation: SplitPieces() takes it out of every
  // piece's range, so it is neither walked nor emitted. Listed here only so
  // that Decode() accepts the opcode.
  case llvm::dwarf::DW_OP_piece:
    return Effect{0, 0};

  default:
    return std::nullopt;
  }
}

llvm::Error DWARFLocationEmitter::Analyze() {
  m_size = m_metadata.dwarf.getData().size();
  m_addr_size = m_target.GetArchitecture().GetAddressByteSize();

  if (llvm::Error error = Decode())
    return error;
  if (llvm::Error error = SplitPieces())
    return error;

  for (Piece &piece : m_pieces) {
    if (llvm::Error error = ClassifyResult(piece))
      return error;

    // A register location has no stack program at all, so there is nothing to
    // walk.
    if (piece.kind == ResultKind::RegisterSlot)
      continue;

    if (llvm::Error error = WalkDepths(piece))
      return error;
    if (llvm::Error error = MarkBranchTargets(piece))
      return error;
  }

  return ResolveOperands();
}

llvm::Error DWARFLocationEmitter::SplitPieces() {
  // A DW_OP_piece separates one simple location description from the next. Not
  // an operation: it neither reads nor writes the stack, so it belongs to no
  // piece and is never emitted.
  auto is_separator = [](const Op &op) {
    return op.op.getCode() == llvm::dwarf::DW_OP_piece;
  };

  m_is_composite = llvm::any_of(m_ops, is_separator);

  if (!m_is_composite) {
    Piece whole;
    whole.count = m_ops.size();
    whole.end_offset = m_size;
    m_pieces.push_back(std::move(whole));
    return llvm::Error::success();
  }

  uint64_t assembled = 0;
  size_t first = 0;
  for (size_t index = 0; index < m_ops.size(); ++index) {
    if (!is_separator(m_ops[index]))
      continue;

    Piece piece;
    piece.first = first;
    piece.count = index - first;
    piece.end_offset = m_ops[index].offset;
    piece.bytes = m_ops[index].op.getRawOperand(0);
    piece.at = assembled;

    // A DW_OP_piece with nothing in front of it is how DWARF says that part of
    // the variable is not available anywhere. Zero filling it would have the
    // condition answer on bytes the compiler is telling us it does not know, so
    // it is refused instead.
    if (!piece.count)
      return Refuse(
          llvm::formatv(
              "has {0} bytes at offset {1} that the compiler says are "
              "in no register and at no address",
              piece.bytes, piece.at)
              .str());
    if (!piece.bytes)
      return Refuse("has a piece that covers no bytes of it");

    assembled += piece.bytes;
    if (assembled > m_metadata.size)
      return Refuse(
          llvm::formatv("is assembled from pieces adding up to more than the "
                        "{0} bytes it occupies",
                        m_metadata.size)
              .str());

    m_pieces.push_back(std::move(piece));
    first = index + 1;
  }

  // Trailing operations after the last separator would be a piece with no size,
  // so there is nowhere to put what they compute.
  if (first != m_ops.size())
    return Refuse("ends with operations that no DW_OP_piece gives a size to");

  // Refused rather than zero filled, for the same reason as an empty piece. A
  // partially described variable is one the condition cannot be answered on.
  if (assembled != m_metadata.size)
    return Refuse(
        llvm::formatv("is assembled from pieces adding up to {0} of the {1} "
                      "bytes it occupies",
                      assembled, m_metadata.size)
            .str());

  if (m_target.GetArchitecture().GetByteOrder() != lldb::eByteOrderLittle)
    return Refuse("is assembled from pieces, which are laid out in increasing "
                  "byte order and so only assembled this way on a little "
                  "endian target");

  return llvm::Error::success();
}

llvm::Error DWARFLocationEmitter::Decode() {
  const llvm::DWARFExpression &expr = m_metadata.dwarf;
  uint64_t consumed = 0;

  for (llvm::DWARFExpression::iterator it = expr.begin(), end = expr.end();
       it != end; ++it) {
    const llvm::DWARFExpression::Operation &op = *it;
    const uint64_t offset = it.getOffset();

    if (op.isError())
      return Refuse("is not a DWARF expression this version of DWARF defines");

    // llvm's decoder reads an operand that runs off the end of the buffer as
    // zero and reports success, so a truncated location comes out as a
    // plausible wrong value rather than as an error. Counting the bytes back is
    // what catches it: an operand that could not be read did not move the
    // cursor.
    uint64_t minimum = 1;
    for (uint8_t encoding : op.getDescription().Op)
      minimum += MinimumOperandBytes(encoding, m_addr_size);
    if (op.getEndOffset() - offset < minimum)
      return Refuse("is truncated");

    // Named separately from the generic refusal below because these are the
    // opcodes a user is likely to actually hit, and "not supported" is a much
    // less useful answer than which unsupported thing it was.
    switch (op.getCode()) {
    case llvm::dwarf::DW_OP_call_frame_cfa:
      return RefuseOpcode(op.getCode(),
                          "needs the function's CFA rule from its unwind plan "
                          "and is not supported yet");
    case llvm::dwarf::DW_OP_entry_value:
    case llvm::dwarf::DW_OP_GNU_entry_value:
      return RefuseOpcode(op.getCode(),
                          "asks for the value a register held on entry to the "
                          "function, which the trampoline did not save");
    case llvm::dwarf::DW_OP_bit_piece:
      // Never emitted by any compiler measured, and the only one of the two
      // that would need bitfield extraction rather than a byte copy.
      return RefuseOpcode(op.getCode(),
                          "describes a variable split at bit boundaries, which "
                          "this pass cannot assemble");
    case llvm::dwarf::DW_OP_form_tls_address:
    case llvm::dwarf::DW_OP_GNU_push_tls_address:
      return RefuseOpcode(op.getCode(),
                          "needs the thread local base of a module, which is "
                          "not reachable from the trampoline");
    case llvm::dwarf::DW_OP_addrx:
    case llvm::dwarf::DW_OP_constx:
    case llvm::dwarf::DW_OP_GNU_addr_index:
    case llvm::dwarf::DW_OP_GNU_const_index:
      return RefuseOpcode(op.getCode(),
                          "indexes a table this layer has no compile unit to "
                          "look in");
    default:
      break;
    }

    std::optional<Effect> effect = GetEffect(op);
    if (!effect)
      return RefuseOpcode(op.getCode(),
                          "has no equivalent that can run in the inferior");

    if (m_ops.size() >= kMaxOperations)
      return Refuse("is too long to evaluate in the inferior");

    consumed = op.getEndOffset();
    m_offset_to_index[offset] = m_ops.size();

    Op entry;
    entry.op = op;
    entry.offset = offset;
    entry.effect = *effect;
    m_ops.push_back(std::move(entry));
  }

  if (m_ops.empty())
    return Refuse("is empty");

  // The expression is exactly its operations, so a walk that stops anywhere but
  // the end read something that was not there.
  if (consumed != m_size)
    return Refuse("is truncated");

  return llvm::Error::success();
}

llvm::Error DWARFLocationEmitter::ClassifyResult(Piece &piece) {
  auto is_register_location = [](uint8_t code) {
    return (code >= llvm::dwarf::DW_OP_reg0 &&
            code <= llvm::dwarf::DW_OP_reg31) ||
           code == llvm::dwarf::DW_OP_regx;
  };

  const llvm::ArrayRef<Op> ops =
      llvm::ArrayRef<Op>(m_ops).slice(piece.first, piece.count);
  // How much a register or a value has to hold: the whole variable when it is
  // in one place, otherwise just this piece of it.
  const uint64_t covers = m_is_composite ? piece.bytes : m_metadata.size;

  const uint8_t first = ops.front().op.getCode();
  if (is_register_location(first)) {
    // A register location description is a complete description on its own, so
    // it is the whole of whatever describes it. Anything after it inside one
    // piece is a location DWARF does not define.
    if (ops.size() != 1)
      return Refuse("names a register and then keeps going, which is not a "
                    "location DWARF defines");

    // The variable is read out of the slot the trampoline saved the register
    // into, and a variable narrower than the slot is read from the slot's low
    // bytes, which is only where they are on a little endian target.
    if (m_target.GetArchitecture().GetByteOrder() != lldb::eByteOrderLittle)
      return Refuse("is a register, which can only be read out of the saved "
                    "register context on a little endian target");
    if (covers > m_addr_size)
      return Refuse("is wider than the register it is said to live in");

    const uint64_t regnum = first == llvm::dwarf::DW_OP_regx
                                ? ops.front().op.getRawOperand(0)
                                : first - llvm::dwarf::DW_OP_reg0;
    llvm::Expected<std::string> field = RegisterField(regnum);
    if (!field)
      return field.takeError();

    piece.reg = std::move(*field);
    piece.kind = ResultKind::RegisterSlot;
    return llvm::Error::success();
  }

  for (const Op &op : ops)
    if (is_register_location(op.op.getCode()))
      return Refuse("names a register part way through computing an address, "
                    "which is not a location DWARF defines");

  size_t stack_values = 0;
  for (const Op &op : ops)
    if (op.op.getCode() == llvm::dwarf::DW_OP_stack_value)
      ++stack_values;

  if (stack_values == 0) {
    piece.kind = ResultKind::MemoryAddress;
    return llvm::Error::success();
  }

  // DW_OP_stack_value ends evaluation, so requiring it to be the last operation
  // costs nothing real and makes the result a property of the whole piece
  // rather than of whichever path ran. Without that, one path could yield an
  // address and another a value, and there is only one store to emit.
  if (stack_values > 1 ||
      ops.back().op.getCode() != llvm::dwarf::DW_OP_stack_value)
    return Refuse("computes a value somewhere other than at its end, so "
                  "whether its result is an address or a value depends on "
                  "which way its branches go");

  if (m_target.GetArchitecture().GetByteOrder() != lldb::eByteOrderLittle)
    return Refuse("is a computed value, which can only be handed over through "
                  "a scratch slot on a little endian target");
  if (covers > m_addr_size)
    return Refuse("is a computed value wider than a pointer, which does not "
                  "fit in a scratch slot");

  piece.kind = ResultKind::ImplicitValue;
  return llvm::Error::success();
}

llvm::Error DWARFLocationEmitter::WalkDepths(Piece &piece) {
  llvm::SmallVector<size_t, 16> worklist;

  // Record that control can arrive at \a offset with \a depth values on the
  // stack, and queue it if that is news. Every edge in the piece goes through
  // here, which is why the disagreement checks live here rather than at each
  // call.
  auto reach = [&](int64_t offset, int depth) -> llvm::Error {
    if (depth > kMaxDepth)
      return Refuse("needs a deeper stack than the generated code declares");

    // Reaching the end, by falling off it or by branching to it, is how a
    // location expression finishes.
    if (offset == static_cast<int64_t>(piece.end_offset)) {
      if (piece.end_depth != kUnreached && piece.end_depth != depth)
        return Refuse("finishes with a different number of values on the stack "
                      "depending on which way its branches go");
      piece.end_depth = depth;
      piece.max_depth = std::max(piece.max_depth, depth);
      return llvm::Error::success();
    }

    if (offset < 0)
      return Refuse("branches to before its own start");

    llvm::DenseMap<uint64_t, size_t>::const_iterator entry =
        m_offset_to_index.find(static_cast<uint64_t>(offset));
    if (entry == m_offset_to_index.end())
      return Refuse("branches somewhere that is not the start of one of its "
                    "own operations");

    Op &target = m_ops[entry->second];
    if (target.depth != kUnreached) {
      if (target.depth != depth)
        return Refuse("reaches one of its operations with a different number "
                      "of values on the stack depending on which way its "
                      "branches go");
      return llvm::Error::success();
    }

    target.depth = depth;
    worklist.push_back(entry->second);
    return llvm::Error::success();
  };

  // Each piece starts with an empty stack: DWARF evaluates every simple
  // location description on its own, and a compiler never carries a value
  // across a DW_OP_piece.
  if (llvm::Error error = reach(m_ops[piece.first].offset, 0))
    return error;

  while (!worklist.empty()) {
    const size_t index = worklist.pop_back_val();
    const int depth = m_ops[index].depth;
    const Effect effect = m_ops[index].effect;
    const uint8_t code = m_ops[index].op.getCode();
    const uint64_t after = m_ops[index].op.getEndOffset();
    const int next_depth = depth + effect.delta;

    if (depth < effect.needs)
      return Refuse("takes more values off its stack than it puts on");

    // A slot is written before the value below it is popped, so both the depth
    // on entry and the depth on exit have to have somewhere to live.
    piece.max_depth = std::max({piece.max_depth, depth, next_depth});

    // Evaluation stops here, so what follows is the end.
    if (code == llvm::dwarf::DW_OP_stack_value) {
      if (llvm::Error error = reach(piece.end_offset, next_depth))
        return error;
      continue;
    }

    if (code == llvm::dwarf::DW_OP_skip || code == llvm::dwarf::DW_OP_bra) {
      // Splitting a composite into pieces assumes control flows straight
      // through each one, which is what makes a piece's operations a contiguous
      // range.
      if (m_is_composite)
        return Refuse("branches inside one of its pieces, and a piece is taken "
                      "to run straight through");
      const int64_t target = BranchTarget(m_ops[index]);
      // A backward branch is a loop, and a loop in the generated code is a hung
      // inferior with the user's breakpoint nowhere in sight. No compiler emits
      // one in a location expression, so refusing costs nothing real and makes
      // the emitted code terminating by construction.
      if (target <= static_cast<int64_t>(m_ops[index].offset))
        return Refuse("branches backwards, which could loop in the inferior");
      if (llvm::Error error = reach(target, next_depth))
        return error;
      // An unconditional branch has no other successor.
      if (code == llvm::dwarf::DW_OP_skip)
        continue;
    }

    if (llvm::Error error = reach(after, next_depth))
      return error;
  }

  if (piece.end_depth == kUnreached)
    return Refuse("never reaches its own end");
  if (piece.end_depth < 1)
    return Refuse("leaves nothing on its stack, so it describes no location");

  return llvm::Error::success();
}

llvm::Error DWARFLocationEmitter::MarkBranchTargets(Piece &piece) {
  for (const Op &op :
       llvm::ArrayRef<Op>(m_ops).slice(piece.first, piece.count)) {
    if (op.depth == kUnreached)
      continue;
    const uint8_t code = op.op.getCode();
    if (code != llvm::dwarf::DW_OP_skip && code != llvm::dwarf::DW_OP_bra)
      continue;

    // Already validated by the walk, which refused any target that was neither
    // the start of an operation nor the end of the expression.
    const int64_t target = BranchTarget(op);
    if (target == static_cast<int64_t>(piece.end_offset)) {
      // With DW_OP_stack_value required to be last, a branch to the end steps
      // over it and would produce an address where the store expects a value.
      if (piece.kind == ResultKind::ImplicitValue)
        return Refuse("branches past the operation that says its result is a "
                      "value rather than an address");
      piece.end_is_label = true;
      continue;
    }
    m_ops[m_offset_to_index.find(static_cast<uint64_t>(target))->second]
        .is_label = true;
  }
  return llvm::Error::success();
}

llvm::Error DWARFLocationEmitter::ResolveOperands() {
  for (Op &op : m_ops) {
    // An operation no path reaches is never emitted, so there is nothing to
    // resolve and no reason to refuse over it.
    if (op.depth == kUnreached)
      continue;

    const uint8_t code = op.op.getCode();

    if (code >= llvm::dwarf::DW_OP_lit0 && code <= llvm::dwarf::DW_OP_lit31) {
      op.value = std::to_string(code - llvm::dwarf::DW_OP_lit0);
      continue;
    }

    if (code >= llvm::dwarf::DW_OP_breg0 && code <= llvm::dwarf::DW_OP_breg31) {
      llvm::Expected<std::string> field =
          RegisterField(code - llvm::dwarf::DW_OP_breg0);
      if (!field)
        return field.takeError();
      // Signed: the operand is a SLEB128, so reading it unsigned turns a stack
      // offset of -24 into 18446744073709551592.
      op.value = *field + " + (" +
                 std::to_string(static_cast<int64_t>(op.op.getRawOperand(0))) +
                 ")";
      continue;
    }

    switch (code) {
    case llvm::dwarf::DW_OP_const1u:
    case llvm::dwarf::DW_OP_const2u:
    case llvm::dwarf::DW_OP_const4u:
    case llvm::dwarf::DW_OP_const8u:
    case llvm::dwarf::DW_OP_constu:
      op.value = std::to_string(op.op.getRawOperand(0)) + "ULL";
      break;

    case llvm::dwarf::DW_OP_const1s:
    case llvm::dwarf::DW_OP_const2s:
    case llvm::dwarf::DW_OP_const4s:
    case llvm::dwarf::DW_OP_const8s:
    case llvm::dwarf::DW_OP_consts:
      // llvm's decoder has already sign extended these into the raw operand.
      op.value =
          std::to_string(static_cast<int64_t>(op.op.getRawOperand(0))) + "LL";
      break;

    case llvm::dwarf::DW_OP_addr: {
      // The operand is a link-time file address. Emitting it as written would
      // have the inferior dereference an address that is only correct for a
      // module loaded with no slide.
      Address file_addr;
      const lldb::addr_t raw = op.op.getRawOperand(0);
      if (!m_target.ResolveFileAddress(raw, file_addr))
        return Refuse(llvm::formatv("names file address {0:x}, which is in no "
                                    "section of any loaded module",
                                    raw)
                          .str());
      const lldb::addr_t load_addr = file_addr.GetLoadAddress(&m_target);
      if (load_addr == LLDB_INVALID_ADDRESS)
        return Refuse("is in a section that is not loaded");
      op.value = std::to_string(load_addr) + "ULL";
      break;
    }

    case llvm::dwarf::DW_OP_fbreg: {
      if (m_frame_base.empty()) {
        llvm::Expected<std::string> frame_base = ResolveFrameBase();
        if (!frame_base)
          return frame_base.takeError();
        m_frame_base = std::move(*frame_base);
      }
      op.value = m_frame_base + " + (" +
                 std::to_string(static_cast<int64_t>(op.op.getRawOperand(0))) +
                 ")";
      break;
    }

    case llvm::dwarf::DW_OP_bregx: {
      llvm::Expected<std::string> field = RegisterField(op.op.getRawOperand(0));
      if (!field)
        return field.takeError();
      op.value = *field + " + (" +
                 std::to_string(static_cast<int64_t>(op.op.getRawOperand(1))) +
                 ")";
      break;
    }

    case llvm::dwarf::DW_OP_deref_size: {
      // Emitted as a load through a pointer, so the size has to be one C has a
      // type for. DWARF permits 3, 5, 6 and 7; no compiler emits them.
      const uint64_t size = op.op.getRawOperand(0);
      if (size != 1 && size != 2 && size != 4 && size != 8)
        return Refuse(llvm::formatv("reads {0} bytes at a time, which has no C "
                                    "type to load through",
                                    size)
                          .str());
      break;
    }

    default:
      break;
    }
  }

  return llvm::Error::success();
}

llvm::Expected<std::string> DWARFLocationEmitter::ResolveFrameBase() {
  DataExtractor data;
  // Selected for this site rather than taken with the defaults, for the reason
  // spelled out in GatherArgumentsMetadata. A frame base is usually a single
  // always-valid expression, so this only matters where the compiler emitted a
  // list, which clang does under -fomit-frame-pointer.
  if (!m_metadata.frame_base_expr_list.GetExpressionData(
          data, m_metadata.func_load_addr, m_site_load_addr))
    return Refuse("is relative to a frame base that its function does not "
                  "describe at this address");

  llvm::DWARFExpression expr(GetLLVMDataExtractor(data),
                             data.GetAddressByteSize());

  llvm::DWARFExpression::iterator op = expr.begin();
  if (op == expr.end() || op->isError())
    return Refuse("is relative to a frame base that could not be decoded");

  // One operation, of the register family. A frame base is a register value,
  // possibly with an offset, and anything else is either debugger state this
  // pass cannot reach or a stack program of its own. The previous version of
  // this code emitted the raw operand of anything else as if it were a field
  // name, which does not compile.
  if (std::next(op) != expr.end())
    return Refuse("is relative to a frame base that takes more than one "
                  "operation to compute");

  const uint8_t code = op->getCode();
  const bool has_offset =
      code >= llvm::dwarf::DW_OP_breg0 && code <= llvm::dwarf::DW_OP_breg31;
  const bool is_register =
      code >= llvm::dwarf::DW_OP_reg0 && code <= llvm::dwarf::DW_OP_reg31;

  if (!has_offset && !is_register) {
    if (code == llvm::dwarf::DW_OP_call_frame_cfa)
      return Refuse("is relative to DW_OP_call_frame_cfa, which GCC emits and "
                    "which needs the function's CFA rule from its unwind plan, "
                    "and that is not supported yet");
    return Refuse(
        llvm::formatv("is relative to a frame base computed with {0}, which "
                      "has no equivalent that can run in the inferior",
                      llvm::dwarf::OperationEncodingString(code))
            .str());
  }

  const uint64_t regnum =
      code - (has_offset ? llvm::dwarf::DW_OP_breg0 : llvm::dwarf::DW_OP_reg0);
  llvm::Expected<std::string> field = RegisterField(regnum);
  if (!field)
    return field.takeError();

  const int64_t offset =
      has_offset ? static_cast<int64_t>(op->getRawOperand(0)) : 0;
  return *field + " + (" + std::to_string(offset) + ")";
}

llvm::Expected<std::string>
DWARFLocationEmitter::RegisterField(uint64_t dwarf_regnum) const {
  // The operand is a DWARF register number, so it has to be resolved through
  // the ABI, which is what knows both that numbering and the field names of the
  // register context the trampoline fills in. Going through a RegisterContext
  // instead would index its own register array and name an unrelated register.
  llvm::Expected<std::string> name = m_abi.GetRegisterName(dwarf_regnum);
  if (!name)
    return llvm::createStringError(llvm::formatv(
        "the location of variable '{0}' reads DWARF register "
        "{1}, which this target's ABI cannot name: {2}",
        m_metadata.name, dwarf_regnum, llvm::fmt_consume(name.takeError())));
  return "regs->" + *name;
}

std::string DWARFLocationEmitter::Describe() const {
  std::string described;
  llvm::raw_string_ostream os(described);
  llvm::ListSeparator separator(", ");
  for (const Op &op : m_ops) {
    os << separator << llvm::dwarf::OperationEncodingString(op.op.getCode());
    for (uint64_t operand : op.op.getRawOperands())
      os << " " << static_cast<int64_t>(operand);
  }
  return described;
}

void DWARFLocationEmitter::EmitOperation(llvm::raw_ostream &os,
                                         const Op &op) const {
  const uint8_t code = op.op.getCode();
  const int depth = op.depth;
  llvm::StringRef indent = "      ";

  // The operations whose operand had to be resolved against the target already
  // carry the C expression that produces their value.
  if (!op.value.empty()) {
    os << indent << Slot(depth) << " = (" << kWord << ")(" << op.value
       << ");\n";
    return;
  }

  // The former top of the stack, and the former second entry, which is where a
  // binary operation leaves its result. Only the operations that need two
  // values read `second`, and they are the ones whose Effect says so.
  const std::string top = Slot(depth - 1);
  const std::string second = depth >= 2 ? Slot(depth - 2) : std::string();

  // How wide a shift may be before C stops defining it.
  const std::string bits = std::string("(int)(8 * sizeof(") + kWord + "))";

  auto binary = [&](llvm::StringRef c_operator) {
    os << indent << second << " = " << second << " " << c_operator << " " << top
       << ";\n";
  };
  // DWARF defines the relational operators over signed values and has them push
  // 1 or 0. The direction is the same as DW_OP_minus: the former second entry
  // is the left hand side.
  auto compare = [&](llvm::StringRef comparison) {
    os << indent << second << " = (" << second << " " << comparison << " "
       << top << ") ? 1 : 0;\n";
  };

  switch (code) {
  case llvm::dwarf::DW_OP_nop:
  // Nothing is written: the slot simply stops being read.
  case llvm::dwarf::DW_OP_drop:
  // Terminal. The store that follows reads the top slot.
  case llvm::dwarf::DW_OP_stack_value:
    break;

  case llvm::dwarf::DW_OP_dup:
    os << indent << Slot(depth) << " = " << top << ";\n";
    break;
  case llvm::dwarf::DW_OP_over:
    os << indent << Slot(depth) << " = " << second << ";\n";
    break;
  case llvm::dwarf::DW_OP_pick:
    os << indent << Slot(depth) << " = "
       << Slot(depth - 1 - static_cast<int>(op.op.getRawOperand(0))) << ";\n";
    break;
  case llvm::dwarf::DW_OP_swap:
    os << indent << "{ " << kWord << " t = " << top << "; " << top << " = "
       << second << "; " << second << " = t; }\n";
    break;
  case llvm::dwarf::DW_OP_rot: {
    // The top becomes the third, the second becomes the top, the third becomes
    // the second.
    const std::string third = Slot(depth - 3);
    os << indent << "{ " << kWord << " t = " << top << "; " << top << " = "
       << second << "; " << second << " = " << third << "; " << third
       << " = t; }\n";
    break;
  }

  case llvm::dwarf::DW_OP_deref:
    os << indent << top << " = *(" << kWord << " *)" << top << ";\n";
    break;
  case llvm::dwarf::DW_OP_deref_size: {
    // Zero extended, per DWARF. Spelled with the built-in types rather than the
    // fixed width ones for the same reason as GetPrologue(): no headers. Every
    // target that can carry an injected condition has an 8 bit char, a 16 bit
    // short, a 32 bit int and a 64 bit long long. The size was checked to be a
    // power of two no wider than a long long.
    static constexpr const char *loads[] = {"unsigned char", "unsigned short",
                                            "unsigned int",
                                            "unsigned long long"};
    const char *load = loads[llvm::Log2_64(op.op.getRawOperand(0))];
    os << indent << top << " = (" << kWord << ")*(" << load << " *)" << top
       << ";\n";
    break;
  }

  case llvm::dwarf::DW_OP_abs:
    os << indent << top << " = " << top << " < 0 ? -" << top << " : " << top
       << ";\n";
    break;
  case llvm::dwarf::DW_OP_neg:
    os << indent << top << " = -" << top << ";\n";
    break;
  case llvm::dwarf::DW_OP_not:
    os << indent << top << " = ~" << top << ";\n";
    break;
  case llvm::dwarf::DW_OP_plus_uconst:
    os << indent << top << " = " << top << " + (" << kWord << ")"
       << op.op.getRawOperand(0) << "ULL;\n";
    break;

  case llvm::dwarf::DW_OP_and:
    binary("&");
    break;
  case llvm::dwarf::DW_OP_or:
    binary("|");
    break;
  case llvm::dwarf::DW_OP_xor:
    binary("^");
    break;
  case llvm::dwarf::DW_OP_plus:
    binary("+");
    break;
  case llvm::dwarf::DW_OP_minus:
    // The former top is subtracted from the former second entry, not the other
    // way round.
    binary("-");
    break;
  case llvm::dwarf::DW_OP_mul:
    binary("*");
    break;

  case llvm::dwarf::DW_OP_div:
    // Signed, per DWARF. A zero divisor cannot be reported from the inferior,
    // and dividing by it there would raise a fault in the user's process, so it
    // yields zero instead. Debug info that divides by zero to find a variable
    // is already broken.
    os << indent << second << " = " << top << " == 0 ? 0 : " << second << " / "
       << top << ";\n";
    break;
  case llvm::dwarf::DW_OP_mod:
    // Unsigned, per DWARF, with the same divisor guard.
    os << indent << second << " = " << top << " == 0 ? 0 : (" << kWord << ")(("
       << kUWord << ")" << second << " % (" << kUWord << ")" << top << ");\n";
    break;

  // A shift by the width of the type or more is undefined in C but not in
  // DWARF, so the out of range cases are spelled out. Shifting left is done
  // unsigned because signed overflow is undefined too.
  case llvm::dwarf::DW_OP_shl:
    os << indent << second << " = (" << top << " < 0 || " << top
       << " >= " << bits << ") ? 0 : (" << kWord << ")((" << kUWord << ")"
       << second << " << " << top << ");\n";
    break;
  case llvm::dwarf::DW_OP_shr:
    os << indent << second << " = (" << top << " < 0 || " << top
       << " >= " << bits << ") ? 0 : (" << kWord << ")((" << kUWord << ")"
       << second << " >> " << top << ");\n";
    break;
  case llvm::dwarf::DW_OP_shra:
    // Arithmetic, so out of range fills with the sign rather than with zero.
    os << indent << second << " = (" << top << " < 0 || " << top
       << " >= " << bits << ") ? (" << second << " < 0 ? -1 : 0) : (" << second
       << " >> " << top << ");\n";
    break;

  case llvm::dwarf::DW_OP_eq:
    compare("==");
    break;
  case llvm::dwarf::DW_OP_ne:
    compare("!=");
    break;
  case llvm::dwarf::DW_OP_lt:
    compare("<");
    break;
  case llvm::dwarf::DW_OP_le:
    compare("<=");
    break;
  case llvm::dwarf::DW_OP_gt:
    compare(">");
    break;
  case llvm::dwarf::DW_OP_ge:
    compare(">=");
    break;

  case llvm::dwarf::DW_OP_skip:
    os << indent << "goto " << Label(BranchTarget(op)) << ";\n";
    break;
  case llvm::dwarf::DW_OP_bra:
    os << indent << "if (" << top << " != 0) goto " << Label(BranchTarget(op))
       << ";\n";
    break;

  default:
    llvm_unreachable("every operation GetEffect() accepts has to be emitted, "
                     "or the slot the condition reads is left unwritten");
  }
}

void DWARFLocationEmitter::EmitCopy(llvm::raw_ostream &os,
                                    uint64_t bytes) const {
  // Widest unit first, so a piece the size of a register is a single load and
  // store. `d` and `s` are the locals EmitPiece() set up.
  static const struct {
    uint64_t width;
    const char *type;
  } units[] = {{8, "$__lldb_b8"},
               {4, "$__lldb_b4"},
               {2, "$__lldb_b2"},
               {1, "$__lldb_b1"}};

  uint64_t at = 0;
  for (const auto &unit : units)
    while (bytes - at >= unit.width) {
      // The member rather than the struct: assigning the struct is a copy of an
      // aggregate, which clang lowers to a call to memcpy, and the JIT has no
      // memcpy to resolve while the breakpoint is being installed. Assigning
      // the member is one load and one store, unaligned but well defined
      // because the type is packed.
      os << "         ((" << unit.type << " *)(d + " << at << "))->v = ((const "
         << unit.type << " *)(s + " << at << "))->v;\n";
      at += unit.width;
    }
}

void DWARFLocationEmitter::EmitPiece(llvm::raw_ostream &os, const Piece &piece,
                                     lldb::offset_t scratch_offset) const {
  for (const Op &op :
       llvm::ArrayRef<Op>(m_ops).slice(piece.first, piece.count)) {
    if (op.depth == kUnreached)
      continue;
    if (op.is_label)
      os << "   " << Label(op.offset) << ": ;\n";
    EmitOperation(os, op);
  }

  if (piece.end_is_label)
    os << "   " << Label(piece.end_offset) << ": ;\n";

  if (!m_is_composite)
    return;

  // Where this piece's bytes are, as an address. A register piece reads out of
  // the context the trampoline saved, a memory piece out of the address it
  // computed, and a value piece out of the slot holding it, which is why that
  // one needs its address taken.
  os << "      {\n"
     << "         char *d = (char *)(arg_struct + " << scratch_offset << ") + "
     << piece.at << ";\n"
     << "         const char *s = (const char *)";
  switch (piece.kind) {
  case ResultKind::RegisterSlot:
    os << "&" << piece.reg;
    break;
  case ResultKind::MemoryAddress:
    os << Slot(piece.end_depth - 1);
    break;
  case ResultKind::ImplicitValue:
    os << "&" << Slot(piece.end_depth - 1);
    break;
  }
  os << ";\n";
  EmitCopy(os, piece.bytes);
  os << "      }\n";
}

std::string DWARFLocationEmitter::Emit(lldb::offset_t scratch_offset) const {
  std::string source;
  llvm::raw_string_ostream os(source);

  os << "\n   /* " << m_metadata.name << " -> +" << m_metadata.offset << ": "
     << Describe() << " */\n";

  const Piece &only = m_pieces.front();

  if (!m_is_composite && only.kind == ResultKind::RegisterSlot) {
    // The trampoline saved every general purpose register into the context it
    // passes here, so the variable is already in the inferior's memory and its
    // address is the address of that slot. Nothing has to be copied.
    //
    // A condition that wrote to the variable would write to the saved slot and
    // have it discarded when the trampoline restores the registers. Conditions
    // are read only in practice, and the alternative is a dematerialization
    // path in the trampoline for a case that should not arise.
    os << "   *(void **)(arg_struct + " << m_metadata.offset << ") = (void *)&"
       << only.reg << ";\n";
    return source;
  }

  // A block per variable, so the slots of one do not have to be told apart from
  // the slots of the next. Labels are function scoped in C and so carry the
  // variable index instead.
  //
  // Every slot is initialized because a goto can carry control past the point a
  // slot would have been written on the other path, and clang warns about that
  // even where the depth walk has proved the value is never read.
  //
  // The slots are shared across pieces, because each piece is copied out before
  // the next one runs.
  int slots = 0;
  for (const Piece &piece : m_pieces)
    slots = std::max(slots, piece.max_depth);

  os << "   {\n";
  if (slots) {
    os << "      " << kWord;
    for (int slot = 0; slot < slots; ++slot)
      os << (slot ? ", " : " ") << Slot(slot) << " = 0";
    os << ";\n";
  }

  for (const Piece &piece : m_pieces)
    EmitPiece(os, piece, scratch_offset);

  if (m_is_composite) {
    // Assembled in the scratch, so what the condition gets is its address.
    os << "      *(void **)(arg_struct + " << m_metadata.offset
       << ") = (void *)(arg_struct + " << scratch_offset << ");\n";
  } else if (only.kind == ResultKind::ImplicitValue) {
    // The value is the variable, so it has to be somewhere before its address
    // can be handed over. The slot is past the end of the layout the condition
    // was compiled against, so nothing else reads it.
    os << "      *(" << kWord << " *)(arg_struct + " << scratch_offset
       << ") = " << Slot(only.end_depth - 1) << ";\n"
       << "      *(void **)(arg_struct + " << m_metadata.offset
       << ") = (void *)(arg_struct + " << scratch_offset << ");\n";
  } else {
    os << "      *(void **)(arg_struct + " << m_metadata.offset
       << ") = (void *)" << Slot(only.end_depth - 1) << ";\n";
  }

  os << "   }\n";
  return source;
}

} // namespace

bool BreakpointInjectedSite::CreateArgumentsStructure() {
  Log *log = GetLog(LLDBLog::JITLoader);

  std::string expr;
  expr.reserve(2048);
  std::string name = "$__lldb_create_args_struct";

  ProcessSP process_sp = m_owner_exe_ctx.GetProcessSP();
  ABISP abi_sp = process_sp ? process_sp->GetABI() : nullptr;

  if (!abi_sp) {
    LLDB_LOG(log, "FCB: Couldn't get the target's ABI");
    return false;
  }

  const lldb::addr_t site_load_addr =
      m_real_addr.GetLoadAddress(m_target_sp.get());

  // Look at every location before emitting any of them. A location that
  // produces a value rather than an address needs a slot to hold it, those
  // slots live past the end of the layout the condition expression was compiled
  // against, and their offsets are only known once every variable has been
  // accounted for.
  std::vector<DWARFLocationEmitter> emitters;
  emitters.reserve(m_metadatas.size());
  size_t scratch_size = 0;

  for (size_t index = 0; index < m_metadatas.size(); ++index) {
    emitters.emplace_back(*m_target_sp, *abi_sp, m_metadatas[index],
                          site_load_addr, index);
    if (llvm::Error error = emitters.back().Analyze()) {
      // Refuse rather than leave the slot unwritten. The condition expression
      // reads every slot at the offset its own layout assigned, so an unwritten
      // one is read as whatever the trampoline's stack happened to hold, and
      // the condition answers on garbage instead of falling back to the
      // debugger.
      LLDB_LOG_ERROR(log, std::move(error), "FCB: {0}");
      return false;
    }
    scratch_size += emitters.back().GetScratchSize();
  }

  // The trampoline rounds this up to the stack alignment, so the scratch does
  // not have to.
  const lldb::offset_t layout_size = m_args_struct_size;
  m_args_struct_size = layout_size + scratch_size;

  // Deliberately no external declarations: this runs while the breakpoint is
  // being installed, when the inferior may not have loaded the libraries its
  // symbols would come from yet, and every store below is pointer sized anyway,
  // so nothing here needs a call.
  expr += abi_sp->GetRegisterContextAsString();
  expr += "\n\n";
  expr += DWARFLocationEmitter::GetPrologue();

  expr += "\nintptr_t $__lldb_create_args_struct(register_context* regs, "
          "intptr_t arg_struct) {\n";

  lldb::offset_t scratch_offset = layout_size;
  for (DWARFLocationEmitter &emitter : emitters) {
    expr += emitter.Emit(scratch_offset);
    scratch_offset += emitter.GetScratchSize();
  }

  expr += "\n"
          "   return arg_struct;\n"
          "}\n";

  LLDB_LOG_VERBOSE(log, "FCB: Argument structure builder source:\n{0}", expr);

  auto utility_fn_or_error = m_target_sp->CreateUtilityFunction(
      expr, name, eLanguageTypeC, m_owner_exe_ctx);

  if (!utility_fn_or_error) {
    LLDB_LOG_ERROR(log, utility_fn_or_error.takeError(),
                   "FCB: Couldn't compile the argument structure builder: {0}");
    LLDB_LOG(log, "FCB: Argument structure builder source:\n{0}", expr);
    m_create_args_struct_function_sp.reset();
    return false;
  }

  m_create_args_struct_function_sp = std::move(*utility_fn_or_error);

  return true;
}
