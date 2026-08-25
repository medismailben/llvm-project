//===-- Disassembler.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_CORE_DISASSEMBLER_H
#define LLDB_CORE_DISASSEMBLER_H

#include "lldb/Core/Address.h"
#include "lldb/Core/EmulateInstruction.h"
#include "lldb/Core/FormatEntity.h"
#include "lldb/Core/Opcode.h"
#include "lldb/Core/PluginInterface.h"
#include "lldb/Interpreter/OptionValue.h"
#include "lldb/Symbol/LineEntry.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Utility/ArchSpec.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/Utility/FileSpec.h"
#include "lldb/Utility/Iterable.h"
#include "lldb/Utility/StructuredData.h"
#include "lldb/lldb-defines.h"
#include "lldb/lldb-forward.h"
#include "lldb/lldb-private-enumerations.h"
#include "lldb/lldb-types.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"

#include "llvm/ADT/StringRef.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace llvm {
template <typename T> class SmallVectorImpl;
}

namespace lldb_private {
class AddressRange;
class DataExtractor;
class Debugger;
class Disassembler;
class Module;
class StackFrame;
class Stream;
class SymbolContext;
class SymbolContextList;
class Target;
struct RegisterInfo;

class Instruction {
public:
  Instruction(const Address &address,
              AddressClass addr_class = AddressClass::eInvalid);

  virtual ~Instruction();

  const Address &GetAddress() const { return m_address; }

  const char *GetMnemonic(const ExecutionContext *exe_ctx,
                          bool markup = false) {
    CalculateMnemonicOperandsAndCommentIfNeeded(exe_ctx);
    return markup ? m_markup_opcode_name.c_str() : m_opcode_name.c_str();
  }

  const char *GetOperands(const ExecutionContext *exe_ctx,
                          bool markup = false) {
    CalculateMnemonicOperandsAndCommentIfNeeded(exe_ctx);
    return markup ? m_markup_mnemonics.c_str() : m_mnemonics.c_str();
  }

  const char *GetComment(const ExecutionContext *exe_ctx) {
    CalculateMnemonicOperandsAndCommentIfNeeded(exe_ctx);
    return m_comment.c_str();
  }

  /// \return
  ///    The control flow kind of this instruction, or
  ///    eInstructionControlFlowKindUnknown if the instruction
  ///    can't be classified.
  virtual lldb::InstructionControlFlowKind
  GetControlFlowKind(const ExecutionContext *exe_ctx) {
    return lldb::eInstructionControlFlowKindUnknown;
  }

  virtual void
  CalculateMnemonicOperandsAndComment(const ExecutionContext *exe_ctx) = 0;

  AddressClass GetAddressClass();

  void SetAddress(const Address &addr) {
    // Invalidate the address class to lazily discover it if we need to.
    m_address_class = AddressClass::eInvalid;
    m_address = addr;
  }

  /// Dump the text representation of this Instruction to a Stream
  ///
  /// Print the (optional) address, (optional) bytes, opcode,
  /// operands, and instruction comments to a stream.
  ///
  /// \param[in] s
  ///     The Stream to add the text to.
  ///
  /// \param[in] show_address
  ///     Whether the address (using disassembly_addr_format_spec formatting)
  ///     should be printed.
  ///
  /// \param[in] show_bytes
  ///     Whether the bytes of the assembly instruction should be printed.
  ///
  /// \param[in] show_control_flow_kind
  ///     Whether the control flow kind of the instruction should be printed.
  ///
  /// \param[in] max_opcode_byte_size
  ///     The size (in bytes) of the largest instruction in the list that
  ///     we are printing (for text justification/alignment purposes)
  ///     Only needed if show_bytes is true.
  ///
  /// \param[in] exe_ctx
  ///     The current execution context, if available.  May be used in
  ///     the assembling of the operands+comments for this instruction.
  ///     Pass NULL if not applicable.
  ///
  /// \param[in] sym_ctx
  ///     The SymbolContext for this instruction.
  ///     Pass NULL if not available/computed.
  ///     Only needed if show_address is true.
  ///
  /// \param[in] prev_sym_ctx
  ///     The SymbolContext for the previous instruction.  Depending on
  ///     the disassembly address format specification, a change in
  ///     Symbol / Function may mean that a line is printed with the new
  ///     symbol/function name.
  ///     Pass NULL if unavailable, or if this is the first instruction of
  ///     the InstructionList.
  ///     Only needed if show_address is true.
  ///
  /// \param[in] disassembly_addr_format
  ///     The format specification for how addresses are printed.
  ///     Only needed if show_address is true.
  ///
  /// \param[in] max_address_text_size
  ///     The length of the longest address string at the start of the
  ///     disassembly line that will be printed (the
  ///     Debugger::FormatDisassemblerAddress() string)
  ///     so this method can properly align the instruction opcodes.
  ///     May be 0 to indicate no indentation/alignment of the opcodes.
  virtual void Dump(Stream *s, uint32_t max_opcode_byte_size, bool show_address,
                    bool show_bytes, bool show_control_flow_kind,
                    const ExecutionContext *exe_ctx,
                    const SymbolContext *sym_ctx,
                    const SymbolContext *prev_sym_ctx,
                    const FormatEntity::Entry *disassembly_addr_format,
                    size_t max_address_text_size);

  virtual bool DoesBranch() = 0;

  virtual bool HasDelaySlot();

  virtual bool IsLoad() = 0;

  virtual bool IsBarrier() = 0;

  virtual bool IsAuthenticated() = 0;

  /// Whether any operand of this instruction is relative to the program
  /// counter.
  ///
  /// Copying such an instruction to a different address changes what it refers
  /// to, so anything that relocates instructions must either adjust the operand
  /// or decline to move it.
  ///
  /// This is a best effort answer derived from the instruction descriptor, so
  /// it is deliberately biased towards \b true: a caller that relocates
  /// instructions is expected to treat it as "may be position dependent" rather
  /// than as proof.
  ///
  /// \return
  ///     \b false only when the instruction is known to be position
  ///     independent, \b true when it is PC-relative or when the disassembler
  ///     cannot tell.
  virtual bool IsPCRelative() { return true; }

  /// The address this instruction refers to through a PC-relative operand.
  ///
  /// \param[in] pc
  ///     The address to interpret this instruction at. The result is in the
  ///     same address domain as this argument.
  ///
  /// \return
  ///     The referenced address, or \b std::nullopt when the instruction is not
  ///     PC-relative, or when the target cannot be known without running the
  ///     program, as is the case for an indirect branch.
  virtual std::optional<lldb::addr_t> GetReferencedAddress(lldb::addr_t pc) {
    return std::nullopt;
  }

  /// How much room a copy of this instruction needs to behave the same way from
  /// a different address.
  struct RelocationSize {
    /// Bytes of code. At least this instruction's size, more when behaving the
    /// same somewhere else takes a sequence rather than one instruction.
    size_t code = 0;
    /// Bytes of constant data that code addresses, placed after it and never
    /// executed. Zero when the relocated form needs none.
    size_t data = 0;
  };

  /// Whether this instruction can execute from a different address, and how
  /// much room a copy that does needs.
  ///
  /// The answer does not depend on where the copy ends up, so a caller can
  /// reserve room before it has an address to relocate to, and Relocate() then
  /// produces exactly this much. The relocated form is chosen by the
  /// instruction alone and never by how far it moves, so reserving and emitting
  /// cannot disagree.
  ///
  /// Unlike IsPCRelative(), this is not biased towards saying yes: it reports
  /// an error whenever it cannot prove a relocated form exists. That is what
  /// lets a caller decline to move an instruction rather than move it wrongly.
  ///
  /// \return
  ///     The room a relocated copy needs, or the reason no relocated form
  ///     exists. That reason reaches the user as the explanation for why a
  ///     faster implementation was unavailable, so it should read as a
  ///     sentence.
  virtual llvm::Expected<RelocationSize> GetRelocationSize();

  /// Bytes that behave at \a to as this instruction does at \a from.
  ///
  /// \param[in] from
  ///     The address this instruction executes at now.
  ///
  /// \param[in] to
  ///     The address the copy will execute at.
  ///
  /// \param[in] referenced_address
  ///     The address the copy has to keep referring to. Normally
  ///     GetReferencedAddress(from), but a caller relocating a whole range has
  ///     to redirect a reference that lands inside that range to wherever the
  ///     copy of that instruction went. Ignored when this instruction refers to
  ///     nothing.
  ///
  /// \param[out] code
  ///     Appended with the relocated instructions, exactly the code size
  ///     GetRelocationSize() reported.
  ///
  /// \return
  ///     Success, or the reason the copy cannot reach \a referenced_address
  ///     from
  ///     \a to. A displacement that no longer fits is the usual reason, so this
  ///     can fail for an instruction GetRelocationSize() accepted: how far the
  ///     copy moves is not known until the trampoline has been allocated.
  virtual llvm::Error Relocate(lldb::addr_t from, lldb::addr_t to,
                               lldb::addr_t referenced_address,
                               llvm::SmallVectorImpl<uint8_t> &code);

  /// How this instruction touches a register.
  ///
  /// Aliasing is accounted for, so asking about "x16" also reports an access
  /// through "w16", and asking about "rax" reports an access through "eax".
  struct RegisterAccess {
    /// The instruction reads the register, including reading it as part of a
    /// read-modify-write.
    bool reads = true;
    /// The instruction overwrites the register without depending on its
    /// previous contents.
    bool writes = false;
  };

  /// Whether this instruction reads or writes \a reg_name.
  ///
  /// Biased like IsPCRelative(): a disassembler that cannot answer reports a
  /// read and no write, which is what makes a caller doing liveness analysis
  /// conclude that the register is live and give up.
  ///
  /// \param[in] reg_name
  ///     The register name as the disassembler spells it, matched
  ///     case-insensitively.
  virtual RegisterAccess GetRegisterAccess(llvm::StringRef reg_name) {
    return RegisterAccess();
  }

  bool CanSetBreakpoint();

  virtual size_t Decode(const Disassembler &disassembler,
                        const DataExtractor &data,
                        lldb::offset_t data_offset) = 0;

  virtual void SetDescription(llvm::StringRef) {
  } // May be overridden in sub-classes that have descriptions.

  lldb::OptionValueSP ReadArray(FILE *in_file, Stream &out_stream,
                                OptionValue::Type data_type);

  lldb::OptionValueSP ReadDictionary(FILE *in_file, Stream &out_stream);

  bool DumpEmulation(const ArchSpec &arch);

  virtual bool TestEmulation(Stream &stream, const char *test_file_name);

  bool Emulate(const ArchSpec &arch, uint32_t evaluate_options, void *baton,
               EmulateInstruction::ReadMemoryCallback read_mem_callback,
               EmulateInstruction::WriteMemoryCallback write_mem_calback,
               EmulateInstruction::ReadRegisterCallback read_reg_callback,
               EmulateInstruction::WriteRegisterCallback write_reg_callback);

  const Opcode &GetOpcode() const { return m_opcode; }

  uint32_t GetData(DataExtractor &data);

  struct Operand {
    enum class Type {
      Invalid = 0,
      Register,
      Immediate,
      Dereference,
      Sum,
      Product
    } m_type = Type::Invalid;
    std::vector<Operand> m_children;
    lldb::addr_t m_immediate = 0;
    ConstString m_register;
    bool m_negative = false;
    bool m_clobbered = false;

    bool IsValid() { return m_type != Type::Invalid; }

    static Operand BuildRegister(ConstString &r);
    static Operand BuildImmediate(lldb::addr_t imm, bool neg);
    static Operand BuildImmediate(int64_t imm);
    static Operand BuildDereference(const Operand &ref);
    static Operand BuildSum(const Operand &lhs, const Operand &rhs);
    static Operand BuildProduct(const Operand &lhs, const Operand &rhs);
  };

  virtual bool ParseOperands(llvm::SmallVectorImpl<Operand> &operands) {
    return false;
  }

  virtual bool IsCall() { return false; }

  /// Whether this instruction returns from the function.
  ///
  /// Unlike GetControlFlowKind(), which only x86 implements, this comes from the
  /// instruction descriptor and so works on every target.
  virtual bool IsReturn() { return false; }

  static const char *GetNameForInstructionControlFlowKind(
      lldb::InstructionControlFlowKind instruction_control_flow_kind);

  /// Get variable annotations for this instruction as structured data.
  /// Returns an array of dictionaries to be used in SBInstruction class.
  StructuredData::ArraySP GetVariableAnnotations();

protected:
  Address m_address; // The section offset address of this instruction
                     // We include an address class in the Instruction class to
                     // allow the instruction specify the
                     // AddressClass::eCodeAlternateISA (currently used for
                     // thumb), and also to specify data (AddressClass::eData).
                     // The usual value will be AddressClass::eCode, but often
                     // when disassembling memory, you might run into data.
                     // This can help us to disassemble appropriately.
private:
  AddressClass m_address_class; // Use GetAddressClass () accessor function!

protected:
  Opcode m_opcode; // The opcode for this instruction
  std::string m_opcode_name;
  std::string m_markup_opcode_name;
  std::string m_mnemonics;
  std::string m_markup_mnemonics;
  std::string m_comment;
  bool m_calculated_strings;

  void
  CalculateMnemonicOperandsAndCommentIfNeeded(const ExecutionContext *exe_ctx) {
    if (!m_calculated_strings) {
      m_calculated_strings = true;
      CalculateMnemonicOperandsAndComment(exe_ctx);
    }
  }
};

namespace OperandMatchers {
std::function<bool(const Instruction::Operand &)>
MatchBinaryOp(std::function<bool(const Instruction::Operand &)> base,
              std::function<bool(const Instruction::Operand &)> left,
              std::function<bool(const Instruction::Operand &)> right);

std::function<bool(const Instruction::Operand &)>
MatchUnaryOp(std::function<bool(const Instruction::Operand &)> base,
             std::function<bool(const Instruction::Operand &)> child);

std::function<bool(const Instruction::Operand &)>
MatchRegOp(const RegisterInfo &info);

std::function<bool(const Instruction::Operand &)> FetchRegOp(ConstString &reg);

std::function<bool(const Instruction::Operand &)> MatchImmOp(int64_t imm);

std::function<bool(const Instruction::Operand &)> FetchImmOp(int64_t &imm);

std::function<bool(const Instruction::Operand &)>
MatchOpType(Instruction::Operand::Type type);
} // namespace OperandMatchers

class InstructionList {
public:
  InstructionList();
  ~InstructionList();

  size_t GetSize() const;

  size_t GetTotalByteSize() const;

  uint32_t GetMaxOpcocdeByteSize() const;

  lldb::InstructionSP GetInstructionAtIndex(size_t idx) const;

  llvm::ArrayRef<lldb::InstructionSP> Instructions() const {
    return m_instructions;
  }

  /// Get the instruction at the given address.
  ///
  /// \return
  ///    A valid \a InstructionSP if the address could be found, or null
  ///    otherwise.
  lldb::InstructionSP GetInstructionAtAddress(const Address &addr);

  //------------------------------------------------------------------
  /// Get the index of the next branch instruction.
  ///
  /// Given a list of instructions, find the next branch instruction
  /// in the list by returning an index.
  ///
  /// @param[in] start
  ///     The instruction index of the first instruction to check.
  ///
  /// @param[in] ignore_calls
  ///     It true, then fine the first branch instruction that isn't
  ///     a function call (a branch that calls and returns to the next
  ///     instruction). If false, find the instruction index of any
  ///     branch in the list.
  ///
  /// @param[out] found_calls
  ///     If non-null, this will be set to true if any calls were found in
  ///     extending the range.
  ///
  /// @return
  ///     The instruction index of the first branch that is at or past
  ///     \a start. Returns UINT32_MAX if no matching branches are
  ///     found.
  //------------------------------------------------------------------
  uint32_t GetIndexOfNextBranchInstruction(uint32_t start, bool ignore_calls,
                                           bool *found_calls) const;

  uint32_t GetIndexOfInstructionAtLoadAddress(lldb::addr_t load_addr,
                                              Target &target);

  uint32_t GetIndexOfInstructionAtAddress(const Address &addr);

  void Clear();

  void Append(lldb::InstructionSP &inst_sp);

  void Dump(Stream *s, bool show_address, bool show_bytes,
            bool show_control_flow_kind, const ExecutionContext *exe_ctx);

private:
    std::vector<lldb::InstructionSP> m_instructions;
};

class PseudoInstruction : public Instruction {
public:
  PseudoInstruction();

  ~PseudoInstruction() override;

  bool DoesBranch() override;

  bool HasDelaySlot() override;

  bool IsLoad() override;

  bool IsBarrier() override;

  bool IsAuthenticated() override;

  void CalculateMnemonicOperandsAndComment(
      const ExecutionContext *exe_ctx) override {
    // TODO: fill this in and put opcode name into Instruction::m_opcode_name,
    // mnemonic into Instruction::m_mnemonics, and any comment into
    // Instruction::m_comment
  }

  size_t Decode(const Disassembler &disassembler, const DataExtractor &data,
                lldb::offset_t data_offset) override;

  void SetOpcode(size_t opcode_size, void *opcode_data);

  void SetDescription(llvm::StringRef description) override;

protected:
  std::string m_description;

  PseudoInstruction(const PseudoInstruction &) = delete;
  const PseudoInstruction &operator=(const PseudoInstruction &) = delete;
};

class Disassembler : public std::enable_shared_from_this<Disassembler>,
                     public PluginInterface {
public:
  enum {
    eOptionNone = 0u,
    eOptionShowBytes = (1u << 0),
    eOptionRawOuput = (1u << 1),
    eOptionMarkPCSourceLine = (1u << 2), // Mark the source line that contains
                                         // the current PC (mixed mode only)
    eOptionMarkPCAddress =
        (1u << 3), // Mark the disassembly line the contains the PC
    eOptionShowControlFlowKind = (1u << 4),
    eOptionVariableAnnotations = (1u << 5),
  };

  enum HexImmediateStyle {
    eHexStyleC,
    eHexStyleAsm,
  };

  // FindPlugin should be lax about the flavor string (it is too annoying to
  // have various internal uses of the disassembler fail because the global
  // flavor string gets set wrong. Instead, if you get a flavor string you
  // don't understand, use the default.  Folks who care to check can use the
  // FlavorValidForArchSpec method on the disassembler they got back.
  static lldb::DisassemblerSP FindPlugin(const ArchSpec &arch,
                                         const char *flavor, const char *cpu,
                                         const char *features,
                                         const char *plugin_name);

  // This version will use the value in the Target settings if flavor is NULL;
  static lldb::DisassemblerSP
  FindPluginForTarget(const Target &target, const ArchSpec &arch,
                      const char *flavor, const char *cpu, const char *features,
                      const char *plugin_name);

  struct Limit {
    enum { Bytes, Instructions } kind;
    lldb::addr_t value;
  };

  static lldb::DisassemblerSP
  DisassembleRange(const ArchSpec &arch, const char *plugin_name,
                   const char *flavor, const char *cpu, const char *features,
                   Target &target, llvm::ArrayRef<AddressRange> disasm_ranges,
                   bool force_live_memory = false);

  static lldb::DisassemblerSP
  DisassembleBytes(const ArchSpec &arch, const char *plugin_name,
                   const char *flavor, const char *cpu, const char *features,
                   const Address &start, const void *bytes, size_t length,
                   uint32_t max_num_instructions, bool data_from_file);

  static bool Disassemble(Debugger &debugger, const ArchSpec &arch,
                          const char *plugin_name, const char *flavor,
                          const char *cpu, const char *features,
                          const ExecutionContext &exe_ctx, const Address &start,
                          Limit limit, bool mixed_source_and_assembly,
                          uint32_t num_mixed_context_lines, uint32_t options,
                          Stream &strm);

  static bool Disassemble(Debugger &debugger, const ArchSpec &arch,
                          StackFrame &frame, Stream &strm);

  // Constructors and Destructors
  Disassembler(const ArchSpec &arch, const char *flavor);
  ~Disassembler() override;

  void PrintInstructions(Debugger &debugger, const ArchSpec &arch,
                         const ExecutionContext &exe_ctx,
                         bool mixed_source_and_assembly,
                         uint32_t num_mixed_context_lines, uint32_t options,
                         Stream &strm);

  size_t ParseInstructions(Target &target, Address address, Limit limit,
                           Stream *error_strm_ptr,
                           bool force_live_memory = false) {
    m_instruction_list.Clear();
    return AppendInstructions(target, address, limit, error_strm_ptr,
                              force_live_memory);
  }

  virtual size_t DecodeInstructions(const Address &base_addr,
                                    const DataExtractor &data,
                                    lldb::offset_t data_offset,
                                    size_t num_instructions, bool append,
                                    bool data_from_file) = 0;

  InstructionList &GetInstructionList();

  const InstructionList &GetInstructionList() const;

  const ArchSpec &GetArchitecture() const { return m_arch; }

  const char *GetFlavor() const { return m_flavor.c_str(); }

  virtual bool FlavorValidForArchSpec(const lldb_private::ArchSpec &arch,
                                      const char *flavor) = 0;

protected:
  size_t AppendInstructions(Target &target, Address address, Limit limit,
                            Stream *error_strm_ptr, bool force_live_memory);

  // SourceLine and SourceLinesToDisplay structures are only used in the mixed
  // source and assembly display methods internal to this class.

  struct SourceLine {
    FileSpec file;
    uint32_t line = LLDB_INVALID_LINE_NUMBER;
    uint32_t column = 0;

    SourceLine() = default;

    bool operator==(const SourceLine &rhs) const {
      return file == rhs.file && line == rhs.line && rhs.column == column;
    }

    bool operator!=(const SourceLine &rhs) const {
      return file != rhs.file || line != rhs.line || column != rhs.column;
    }

    bool IsValid() const { return line != LLDB_INVALID_LINE_NUMBER; }
  };

  struct SourceLinesToDisplay {
    std::vector<SourceLine> lines;

    // index of the "current" source line, if we want to highlight that when
    // displaying the source lines.  (as opposed to the surrounding source
    // lines provided to give context)
    size_t current_source_line = -1;

    // Whether to print a blank line at the end of the source lines.
    bool print_source_context_end_eol = true;

    SourceLinesToDisplay() = default;
  };

  // Get the function's declaration line number, hopefully a line number
  // earlier than the opening curly brace at the start of the function body.
  static SourceLine GetFunctionDeclLineEntry(const SymbolContext &sc);

  // Add the provided SourceLine to the map of filenames-to-source-lines-seen.
  static void AddLineToSourceLineTables(
      SourceLine &line,
      std::map<FileSpec, std::set<uint32_t>> &source_lines_seen);

  // Given a source line, determine if we should print it when we're doing
  // mixed source & assembly output. We're currently using the
  // target.process.thread.step-avoid-regexp setting (which is used for
  // stepping over inlined STL functions by default) to determine what source
  // lines to avoid showing.
  //
  // Returns true if this source line should be elided (if the source line
  // should not be displayed).
  static bool
  ElideMixedSourceAndDisassemblyLine(const ExecutionContext &exe_ctx,
                                     const SymbolContext &sc, SourceLine &line);

  static bool
  ElideMixedSourceAndDisassemblyLine(const ExecutionContext &exe_ctx,
                                     const SymbolContext &sc, LineEntry &line) {
    SourceLine sl;
    sl.file = line.GetFile();
    sl.line = line.line;
    sl.column = line.column;
    return ElideMixedSourceAndDisassemblyLine(exe_ctx, sc, sl);
  };

  // Classes that inherit from Disassembler can see and modify these
  ArchSpec m_arch;
  InstructionList m_instruction_list;
  std::string m_flavor;

private:
  // For Disassembler only
  Disassembler(const Disassembler &) = delete;
  const Disassembler &operator=(const Disassembler &) = delete;
};

/// Structured data for a single variable annotation.
struct VariableAnnotation {
  std::string variable_name;
  /// Location description (e.g., "r15", "undef", "const_0").
  std::string location_description;
  /// Whether variable is live at this instruction.
  bool is_live;
  /// Register numbering scheme for location interpretation.
  lldb::RegisterKind register_kind;
  /// Where this annotation is valid.
  std::optional<lldb_private::AddressRange> address_range;
  /// Source file where variable was declared.
  std::optional<std::string> decl_file;
  /// Line number where variable was declared.
  std::optional<uint32_t> decl_line;
  /// Variable's type name.
  std::optional<std::string> type_name;
};

/// Tracks live variable annotations across instructions and produces
/// per-instruction "events" like `name = RDI` or `name = <undef>`.
class VariableAnnotator {

  // Live state from the previous instruction, keyed by Variable::GetID().
  llvm::DenseMap<lldb::user_id_t, VariableAnnotation> m_live_vars;

public:
  /// Compute annotation strings for a single instruction and update
  /// `m_live_vars`. Returns only the events that should be printed *at this
  /// instruction*.
  std::vector<std::string> Annotate(Instruction &inst);

  /// Returns structured data for all variables relevant at this instruction.
  std::vector<VariableAnnotation> AnnotateStructured(Instruction &inst);
};

} // namespace lldb_private

#endif // LLDB_CORE_DISASSEMBLER_H
