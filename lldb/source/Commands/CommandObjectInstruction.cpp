//===-- CommandObjectInstruction.cpp --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CommandObjectInstruction.h"

#include "CommandObjectDisassemble.h"

#include "lldb/Core/Debugger.h"
#include "lldb/Core/Disassembler.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/PatchSiteAnalysis.h"
#include "lldb/Host/OptionParser.h"
#include "lldb/Host/Terminal.h"
#include "lldb/Interpreter/CommandInterpreter.h"
// Used by the generated option table below, which a header-usage check cannot
// see through.
#include "lldb/Interpreter/CommandOptionArgumentTable.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Interpreter/OptionArgParser.h"
#include "lldb/Interpreter/Options.h"
#include "lldb/Target/ABI.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/AnsiTerminal.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/MathExtras.h"

using namespace lldb;
using namespace lldb_private;

namespace {

/// The colors the `instruction` subcommands print with, and the only place in
/// this file that knows an escape sequence exists.
///
/// Every accessor hands back a finished string rather than a prefix and a
/// suffix a caller has to pair up, so no print site can leak a color into the
/// rest of its line, and with color off every accessor returns exactly the text
/// these commands printed before any of them had a color.
class Colors {
public:
  explicit Colors(bool use_color) : m_use_color(use_color) {}

  /// Cyan, which is the color the default frame format already gives a pc, so
  /// that an address is recognizable as one across commands.
  std::string Address(lldb::addr_t addr) const {
    return Paint(llvm::formatv("{0:x}", addr).str(), "${ansi.fg.cyan}");
  }

  /// The pass and fail glyphs.
  ///
  /// Both fall back to one ASCII column rather than to a word, so that the
  /// marks stay in a single column that can be scanned without reading the text
  /// next to them, which is the only reason to print marks instead of prose.
  /// Only the glyph is colored: the requirement beside it is a sentence, and a
  /// whole sentence in red reads as a complaint about the command rather than
  /// as an answer about the site.
  std::string Mark(bool passed) const {
    const bool unicode = Terminal::SupportsUnicode();
    if (passed)
      return Paint(unicode ? reinterpret_cast<const char *>(u8"✔") : "+",
                   "${ansi.fg.green}");
    return Paint(unicode ? reinterpret_cast<const char *>(u8"✘") : "x",
                 "${ansi.fg.red}");
  }

  /// One instruction spelled the way the disassembler spells it.
  ///
  /// The disassembler already produces a colored form of every instruction, so
  /// asking for that form keeps these commands and `disassemble` coloring the
  /// same operand the same way instead of inventing a second scheme that would
  /// drift from it. Only some plugins fill the markup in, so an empty answer
  /// falls back to the plain spelling rather than printing nothing.
  std::string Code(Instruction &instruction,
                   const ExecutionContext &exe_ctx) const {
    llvm::StringRef mnemonic = instruction.GetMnemonic(&exe_ctx, m_use_color);
    if (mnemonic.empty())
      mnemonic = instruction.GetMnemonic(&exe_ctx);

    llvm::StringRef operands = instruction.GetOperands(&exe_ctx, m_use_color);
    if (operands.empty())
      operands = instruction.GetOperands(&exe_ctx);

    return (llvm::Twine(mnemonic) + " " + operands).str();
  }

private:
  /// Wrap \a text so that the color ends exactly where the text does.
  ///
  /// Written as the same token form the ansi settings use and expanded by the
  /// same code, so that turning color off removes the tokens instead of
  /// emitting escapes a non-terminal would have to be trusted to swallow.
  std::string Paint(llvm::StringRef text, llvm::StringRef color) const {
    if (!m_use_color)
      return text.str();
    return ansi::FormatAnsiTerminalCodes(
        (llvm::Twine(color) + text + "${ansi.normal}").str(), true);
  }

  bool m_use_color;
};

} // namespace

#define LLDB_OPTIONS_instruction_info
#include "CommandOptions.inc"

/// CommandObjectInstructionInfo
///
/// Report what the debugger knows about one instruction. Every answer here
/// comes from the disassembler plugin, so this is also the way to see what that
/// plugin believes, which otherwise takes a rebuild with logging added.
class CommandObjectInstructionInfo : public CommandObjectParsed {
public:
  class CommandOptions : public Options {
  public:
    CommandOptions() = default;
    ~CommandOptions() override = default;

    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override {
      Status error;
      switch (m_getopt_table[option_idx].val) {
      case 'c':
        if (option_arg.getAsInteger(0, count) || !count)
          error = Status::FromErrorStringWithFormat(
              "invalid instruction count '%s'", option_arg.str().c_str());
        break;
      default:
        llvm_unreachable("Unimplemented option");
      }
      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      count = 1;
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
      return llvm::ArrayRef(g_instruction_info_options);
    }

    uint32_t count = 1;
  };

  CommandObjectInstructionInfo(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "instruction info",
            "Describe the instruction at an address: what it refers to, which "
            "registers it touches, and whether it can be moved out of line.",
            nullptr, eCommandRequiresTarget) {
    AddSimpleArgumentList(eArgTypeAddressOrExpression);
  }

  ~CommandObjectInstructionInfo() override = default;

  Options *GetOptions() override { return &m_options; }

protected:
  /// Print one instruction's properties.
  ///
  /// Ordered so that the cheap syntactic facts come first and the question a
  /// caller patching code actually cares about, whether it can be moved, comes
  /// last.
  void Describe(Stream &s, Target &target, const InstructionSP &instruction,
                const Colors &colors) {
    ExecutionContext exe_ctx(&target, false);

    // Without a process there is no load address, so fall back to the file
    // address. Whichever it is has to be the one handed to
    // GetReferencedAddress(), which answers in the same address domain as its
    // argument.
    const Address &address = instruction->GetAddress();
    addr_t pc = address.GetLoadAddress(&target);
    if (pc == LLDB_INVALID_ADDRESS)
      pc = address.GetFileAddress();

    s.Format("{0}  {1}\n", colors.Address(pc),
             colors.Code(*instruction, exe_ctx));

    s.Format("  size            {0} bytes\n",
             instruction->GetOpcode().GetByteSize());
    s.Format("  branches        {0}\n",
             instruction->DoesBranch() ? "yes" : "no");
    s.Format("  call            {0}\n", instruction->IsCall() ? "yes" : "no");
    s.Format("  return          {0}\n", instruction->IsReturn() ? "yes" : "no");
    s.Format("  barrier         {0}\n",
             instruction->IsBarrier() ? "yes" : "no");
    s.Format("  load            {0}\n", instruction->IsLoad() ? "yes" : "no");

    // Biased towards saying yes, so report it as the question it answers rather
    // than as a fact.
    s.Format("  position        {0}\n", instruction->IsPCRelative()
                                            ? "may depend on the pc"
                                            : "independent");

    if (std::optional<addr_t> referenced =
            instruction->GetReferencedAddress(pc)) {
      s.Format("  refers to       {0}", colors.Address(*referenced));
      Address resolved;
      if (target.ResolveLoadAddress(*referenced, resolved)) {
        StreamString name;
        resolved.Dump(&name, &target, Address::DumpStyleResolvedDescription);
        if (name.GetSize())
          s.Format("  {0}", name.GetString());
      }
      s.PutCString("\n");
    } else if (instruction->DoesBranch()) {
      s.PutCString("  refers to       an address only known at run time\n");
    }

    llvm::Expected<Instruction::RelocationSize> room =
        instruction->GetRelocationSize();
    if (room)
      s.Format("  relocatable     yes, {0} bytes of code{1}\n", room->code,
               room->data ? llvm::formatv(" and {0} of data", room->data).str()
                          : std::string());
    else
      s.Format("  relocatable     no: {0}\n", llvm::toString(room.takeError()));
  }

  void DoExecute(Args &command, CommandReturnObject &result) override {
    // eCommandRequiresTarget, so this is never null.
    Target &target = *GetTarget();

    if (command.GetArgumentCount() != 1) {
      result.AppendError("an address or an expression that yields one is "
                         "required");
      return;
    }

    Status error;
    addr_t addr = OptionArgParser::ToAddress(&m_exe_ctx, command[0].ref(),
                                             LLDB_INVALID_ADDRESS, &error);
    if (addr == LLDB_INVALID_ADDRESS) {
      result.AppendErrorWithFormat("'%s' is not an address: %s",
                                   command[0].c_str(), error.AsCString());
      return;
    }

    Address start;
    if (!target.ResolveLoadAddress(addr, start))
      start = Address(addr);

    // Live memory, because the point is to describe the code that will run. A
    // site that has already been patched reads as the patch, which is the
    // honest answer.
    DisassemblerSP disassembler_sp = Disassembler::DisassembleRange(
        target.GetArchitecture(), /*plugin_name=*/nullptr, /*flavor=*/nullptr,
        /*cpu=*/nullptr, /*features=*/nullptr, target,
        AddressRange(start,
                     m_options.count *
                         target.GetArchitecture().GetMaximumOpcodeByteSize()),
        /*force_live_memory=*/true);

    if (!disassembler_sp) {
      result.AppendErrorWithFormat("couldn't disassemble at 0x%" PRIx64, addr);
      return;
    }

    InstructionList &instructions = disassembler_sp->GetInstructionList();
    const size_t count =
        std::min<size_t>(m_options.count, instructions.GetSize());
    if (!count) {
      result.AppendErrorWithFormat("no instruction at 0x%" PRIx64, addr);
      return;
    }

    Stream &s = result.GetOutputStream();
    // Read per execution rather than cached in the command object, so that
    // `settings set use-color` takes effect on the next run.
    const Colors colors(GetDebugger().GetUseColor());
    for (size_t i = 0; i < count; ++i) {
      if (i)
        s.PutCString("\n");
      Describe(s, target, instructions.GetInstructionAtIndex(i), colors);
    }
    result.SetStatus(eReturnStatusSuccessFinishResult);
  }

  CommandOptions m_options;
};

#define LLDB_OPTIONS_instruction_reach
#include "CommandOptions.inc"

namespace {

/// AArch64 page size for `adrp`, which is fixed by the encoding rather than by
/// the operating system's page size.
constexpr uint64_t g_adrp_page_size = 4096;

/// One pc-relative displacement field, described by how it is encoded rather
/// than by the mnemonics that use it.
///
/// Reach is a property of the field, not of the spelling, so a table keyed on
/// the encoding stays right when a form gains another mnemonic or an assembly
/// alias, and it lets both architectures answer the same question with the same
/// code.
struct BranchReachDescription {
  /// The instructions that share this field, reported so the answer can be
  /// checked against the architecture manual without reading this table.
  const char *forms;
  /// Width of the signed immediate field.
  unsigned bits;
  /// How many bytes one unit of the immediate covers.
  uint64_t scale;
  /// Distance from the start of the instruction to the point the displacement
  /// is measured from. AArch64 measures from the instruction itself, x86-64
  /// from the first byte after it.
  uint64_t displacement_origin;
  /// Whether the displacement counts pages from the page holding the
  /// instruction rather than bytes from the instruction, as `adrp` does.
  bool page_relative;
};

struct BranchReachEntry {
  BranchKind kind;
  BranchReachDescription description;
};

constexpr BranchReachEntry g_aarch64_reach[] = {
    {eBranchKindBranch, {"b, bl", 26, 4, 0, false}},
    {eBranchKindConditional, {"b.cond, cbz, cbnz", 19, 4, 0, false}},
    {eBranchKindTestBranch, {"tbz, tbnz", 14, 4, 0, false}},
    {eBranchKindLiteral,
     {"ldr (literal), ldrsw (literal), prfm (literal)", 19, 4, 0, false}},
    {eBranchKindAddress, {"adr", 21, 1, 0, false}},
    {eBranchKindPage, {"adrp", 21, g_adrp_page_size, 0, true}},
};

/// x86-64 measures every relative displacement from the end of the instruction,
/// so the origin here is the encoded length of the shortest instruction that
/// uses the field: five bytes for `jmp rel32` and `call rel32`, six for `jcc
/// rel32` with its two byte opcode, two for the rel8 forms.
///
/// Nothing is listed for the rip-relative data forms. Their disp32 is measured
/// from the end of an instruction whose length depends on its operands, so
/// there is no reach that can be stated from an address alone, and guessing one
/// would be worse than refusing.
constexpr BranchReachEntry g_x86_64_reach[] = {
    {eBranchKindBranch, {"jmp rel32, call rel32", 32, 1, 5, false}},
    {eBranchKindConditional, {"jcc rel32", 32, 1, 6, false}},
    {eBranchKindShort, {"jmp rel8, jcc rel8", 8, 1, 2, false}},
};

llvm::ArrayRef<BranchReachEntry> GetReachTable(llvm::Triple::ArchType arch) {
  switch (arch) {
  case llvm::Triple::aarch64:
  case llvm::Triple::aarch64_be:
  case llvm::Triple::aarch64_32:
    return llvm::ArrayRef(g_aarch64_reach);
  case llvm::Triple::x86_64:
    return llvm::ArrayRef(g_x86_64_reach);
  default:
    return {};
  }
}

/// Spell a kind the way the user typed it, by going back to the option's own
/// enumeration instead of keeping a second list that can drift from it.
llvm::StringRef GetBranchKindName(BranchKind kind) {
  for (const OptionEnumValueElement &value :
       g_argument_table[lldb::eArgTypeBranchKind].enum_values)
    if (value.value == kind)
      return value.string_value;
  return "unknown";
}

/// Move \a base by \a displacement, clamping at the ends of the address space.
///
/// The reachable range of a branch near address zero or near the top of the
/// address space is genuinely truncated, and wrapping around would report an
/// address the branch cannot reach as though it could.
lldb::addr_t SaturatingAdd(lldb::addr_t base, int64_t displacement) {
  if (displacement < 0) {
    // Unsigned negation, which is defined even for INT64_MIN, unlike negating
    // the signed value first.
    const uint64_t magnitude = 0 - static_cast<uint64_t>(displacement);
    return base < magnitude ? 0 : base - magnitude;
  }
  const uint64_t magnitude = static_cast<uint64_t>(displacement);
  return UINT64_MAX - base < magnitude ? UINT64_MAX : base + magnitude;
}

/// Render a power of two byte count the way an architecture manual states a
/// reach, so the report can be compared against it directly.
std::string HumanReadableSize(uint64_t bytes) {
  static constexpr struct {
    uint64_t size;
    const char *suffix;
  } units[] = {{1ULL << 30, "GiB"}, {1ULL << 20, "MiB"}, {1ULL << 10, "KiB"}};

  for (const auto &unit : units)
    if (bytes >= unit.size && bytes % unit.size == 0)
      return llvm::formatv("{0}{1}", bytes / unit.size, unit.suffix).str();
  return llvm::formatv("{0} bytes", bytes).str();
}

} // namespace

/// CommandObjectInstructionReach
///
/// Report how far a given branch form reaches from an address. Patching a site
/// with a branch to a trampoline fails when the trampoline is out of range, and
/// without this the only way to find that out is to attempt the patch and read
/// the refusal, which says nothing about where a trampoline would have worked.
class CommandObjectInstructionReach : public CommandObjectParsed {
public:
  class CommandOptions : public Options {
  public:
    CommandOptions() = default;
    ~CommandOptions() override = default;

    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override {
      Status error;
      switch (m_getopt_table[option_idx].val) {
      case 'k':
        kind = static_cast<BranchKind>(OptionArgParser::ToOptionEnum(
            option_arg, GetDefinitions()[option_idx].enum_values,
            eBranchKindBranch, error));
        break;
      case 't':
        target = OptionArgParser::ToAddress(execution_context, option_arg,
                                            LLDB_INVALID_ADDRESS, &error);
        break;
      default:
        llvm_unreachable("Unimplemented option");
      }
      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      kind = eBranchKindBranch;
      target = LLDB_INVALID_ADDRESS;
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
      return llvm::ArrayRef(g_instruction_reach_options);
    }

    /// Defaults to the unconditional branch, which is the form a patched site
    /// uses and so the one this question is nearly always about.
    BranchKind kind = eBranchKindBranch;
    lldb::addr_t target = LLDB_INVALID_ADDRESS;
  };

  CommandObjectInstructionReach(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "instruction reach",
            "Report the range of addresses a branch of a given form can reach "
            "from an address, and whether a particular target is within it.",
            nullptr, eCommandRequiresTarget) {
    AddSimpleArgumentList(eArgTypeAddressOrExpression);
  }

  ~CommandObjectInstructionReach() override = default;

  Options *GetOptions() override { return &m_options; }

protected:
  /// Print the reach of one form from one address.
  ///
  /// The concrete endpoints come before everything else: the question behind
  /// this command is where a trampoline may be placed, and a size alone does
  /// not answer it.
  void Describe(Stream &s, lldb::addr_t addr,
                const BranchReachDescription &desc, const Colors &colors) {
    const int64_t min_displacement =
        llvm::minIntN(desc.bits) * static_cast<int64_t>(desc.scale);
    const int64_t max_displacement =
        llvm::maxIntN(desc.bits) * static_cast<int64_t>(desc.scale);

    const lldb::addr_t origin = desc.page_relative
                                    ? (addr & ~(g_adrp_page_size - 1))
                                    : addr + desc.displacement_origin;

    lldb::addr_t lowest = SaturatingAdd(origin, min_displacement);
    lldb::addr_t highest = SaturatingAdd(origin, max_displacement);
    // adrp names a page, so every byte of the last reachable page is reachable.
    if (desc.page_relative && highest != UINT64_MAX)
      highest = SaturatingAdd(highest, g_adrp_page_size - 1);

    s.Format("{0}  {1} ({2})\n", colors.Address(addr),
             GetBranchKindName(m_options.kind), desc.forms);

    if (desc.page_relative)
      s.Format("  encoding        signed {0} bit immediate counting {1} "
               "pages\n",
               desc.bits, HumanReadableSize(desc.scale));
    else if (desc.scale > 1)
      s.Format("  encoding        signed {0} bit immediate scaled by {1}\n",
               desc.bits, desc.scale);
    else
      s.Format("  encoding        signed {0} bit immediate in bytes\n",
               desc.bits);

    s.Format("  measured from   {0} ({1})\n", colors.Address(origin),
             desc.page_relative
                 ? "the page holding the instruction"
                 : (desc.displacement_origin ? "the byte after the instruction"
                                             : "the instruction itself"));

    // The magnitude of the backward limit is the exact power of two the manual
    // quotes; the forward limit is one unit short of it, so state both.
    s.Format("  reach           +/-{0} (displacement {1} to +{2} bytes)\n",
             HumanReadableSize(0 - static_cast<uint64_t>(min_displacement)),
             min_displacement, max_displacement);
    s.Format("  reaches         {0} through {1}\n", colors.Address(lowest),
             colors.Address(highest));

    if (desc.scale > 1 && !desc.page_relative)
      s.Format("  alignment       targets must be {0} byte aligned\n",
               desc.scale);

    if (m_options.target == LLDB_INVALID_ADDRESS)
      return;

    const lldb::addr_t reference =
        desc.page_relative ? (m_options.target & ~(g_adrp_page_size - 1))
                           : m_options.target;
    const int64_t displacement =
        static_cast<int64_t>(reference) - static_cast<int64_t>(origin);

    if (desc.scale > 1 && !desc.page_relative &&
        displacement % static_cast<int64_t>(desc.scale)) {
      s.Format("  target          {0}  not {1} byte aligned, so this form "
               "cannot encode it\n",
               colors.Address(m_options.target), desc.scale);
      return;
    }

    if (displacement > max_displacement) {
      s.Format("  target          {0}  out of reach, {1} bytes too far "
               "forward\n",
               colors.Address(m_options.target),
               displacement - max_displacement);
      return;
    }

    if (displacement < min_displacement) {
      s.Format("  target          {0}  out of reach, {1} bytes too far "
               "back\n",
               colors.Address(m_options.target),
               min_displacement - displacement);
      return;
    }

    s.Format("  target          {0}  in reach, displacement {1}{2} bytes\n",
             colors.Address(m_options.target), displacement < 0 ? "" : "+",
             displacement);
  }

  void DoExecute(Args &command, CommandReturnObject &result) override {
    // eCommandRequiresTarget, so this is never null.
    Target &target = *GetTarget();

    if (command.GetArgumentCount() != 1) {
      result.AppendError("an address or an expression that yields one is "
                         "required");
      return;
    }

    Status error;
    addr_t addr = OptionArgParser::ToAddress(&m_exe_ctx, command[0].ref(),
                                             LLDB_INVALID_ADDRESS, &error);
    if (addr == LLDB_INVALID_ADDRESS) {
      result.AppendErrorWithFormat("'%s' is not an address: %s",
                                   command[0].c_str(), error.AsCString());
      return;
    }

    const ArchSpec &arch = target.GetArchitecture();
    llvm::ArrayRef<BranchReachEntry> table = GetReachTable(arch.GetMachine());

    // An architecture with no table has to say so. Reach limits are encoding
    // specific, and an answer invented from another architecture's fields would
    // be believed and would be wrong.
    if (table.empty()) {
      result.AppendErrorWithFormatv(
          "no branch reach table for {0}; the reach of a form is a property of "
          "its encoding and has to be described per architecture",
          arch.GetArchitectureName());
      return;
    }

    const BranchReachEntry *entry =
        llvm::find_if(table, [this](const BranchReachEntry &candidate) {
          return candidate.kind == m_options.kind;
        });

    if (entry == table.end()) {
      StreamString available;
      llvm::interleaveComma(table, available.AsRawOstream(),
                            [&](const BranchReachEntry &candidate) {
                              available << GetBranchKindName(candidate.kind);
                            });
      result.AppendErrorWithFormatv(
          "{0} has no '{1}' form with a reach that can be stated from an "
          "address; it has: {2}",
          arch.GetArchitectureName(), GetBranchKindName(m_options.kind),
          available.GetString());
      return;
    }

    Describe(result.GetOutputStream(), addr, entry->description,
             Colors(GetDebugger().GetUseColor()));
    result.SetStatus(eReturnStatusSuccessFinishResult);
  }

  CommandOptions m_options;
};

#define LLDB_OPTIONS_instruction_relocate
#include "CommandOptions.inc"

/// CommandObjectInstructionRelocate
///
/// Answer what an instruction would become at a different address, and whether
/// it would still work there. This is the observable form of the two questions
/// the code that moves instructions out of line to patch a breakpoint site asks
/// of the disassembler, and the relocated bytes are disassembled back at the
/// destination so the answer can be read rather than trusted.
class CommandObjectInstructionRelocate : public CommandObjectParsed {
public:
  class CommandOptions : public Options {
  public:
    CommandOptions() = default;
    ~CommandOptions() override = default;

    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override {
      Status error;
      switch (m_getopt_table[option_idx].val) {
      case 't': {
        const addr_t addr = OptionArgParser::ToAddress(
            execution_context, option_arg, LLDB_INVALID_ADDRESS, &error);
        if (addr != LLDB_INVALID_ADDRESS)
          to = addr;
      } break;
      case 'o': {
        int64_t bytes;
        if (option_arg.getAsInteger(0, bytes))
          error = Status::FromErrorStringWithFormat("invalid offset '%s'",
                                                    option_arg.str().c_str());
        else
          offset = bytes;
      } break;
      default:
        llvm_unreachable("Unimplemented option");
      }
      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      to.reset();
      offset.reset();
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
      return llvm::ArrayRef(g_instruction_relocate_options);
    }

    std::optional<addr_t> to;
    std::optional<int64_t> offset;
  };

  CommandObjectInstructionRelocate(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "instruction relocate",
            "Show what the instruction at an address would become if it were "
            "moved to another address, and why it could not be.",
            nullptr, eCommandRequiresTarget) {
    AddSimpleArgumentList(eArgTypeAddressOrExpression);
  }

  ~CommandObjectInstructionRelocate() override = default;

  Options *GetOptions() override { return &m_options; }

protected:
  /// Print one instruction the way the disassembler spells it, so that the
  /// original and the relocated copy can be compared line against line.
  static void PrintInstruction(Stream &s, ExecutionContext &exe_ctx, addr_t pc,
                               Instruction &instruction, const Colors &colors) {
    s.Format("{0}  {1}", colors.Address(pc), colors.Code(instruction, exe_ctx));

    // The comment is where the disassembler names a branch target, which is the
    // part a reader checks when the operand was rewritten.
    llvm::StringRef comment = instruction.GetComment(&exe_ctx);
    if (!comment.empty())
      s.Format("  ; {0}", comment);

    s.PutCString("\n");
  }

  /// Disassemble the bytes Relocate() produced as if they lived at \a to.
  ///
  /// This is the only part of the report that is evidence rather than an
  /// assertion: an operand that was rewritten for the new address is only
  /// provably right when it is decoded from that address.
  void PrintRelocatedCode(Stream &s, Target &target, addr_t to,
                          llvm::ArrayRef<uint8_t> code, const Colors &colors) {
    // The bytes are in a local buffer rather than in the target, and the
    // destination need not be mapped at all, so this cannot go through
    // DisassembleRange().
    DisassemblerSP disassembler_sp = Disassembler::DisassembleBytes(
        target.GetArchitecture(), /*plugin_name=*/nullptr,
        target.GetDisassemblyFlavor(), target.GetDisassemblyCPU(),
        target.GetDisassemblyFeatures(), Address(to), code.data(), code.size(),
        /*max_num_instructions=*/UINT32_MAX, /*data_from_file=*/true);

    InstructionList empty;
    InstructionList &instructions =
        disassembler_sp ? disassembler_sp->GetInstructionList() : empty;

    if (!instructions.GetSize()) {
      s.PutCString("  reads as        <the relocated bytes do not "
                   "disassemble>\n");
      return;
    }

    ExecutionContext exe_ctx(&target, false);
    for (size_t i = 0; i < instructions.GetSize(); ++i) {
      // Only the first line is labelled; a relocated form that took several
      // instructions continues under the same heading.
      s.PutCString(i ? "                  " : "  reads as        ");
      InstructionSP instruction = instructions.GetInstructionAtIndex(i);
      // These were decoded from a bare address, so the file address is the
      // address they were decoded at.
      PrintInstruction(s, exe_ctx, instruction->GetAddress().GetFileAddress(),
                       *instruction, colors);
    }
  }

  void DoExecute(Args &command, CommandReturnObject &result) override {
    // eCommandRequiresTarget, so this is never null.
    Target &target = *GetTarget();

    if (command.GetArgumentCount() != 1) {
      result.AppendError("an address or an expression that yields one is "
                         "required");
      return;
    }

    if (!m_options.to && !m_options.offset) {
      result.AppendError("a destination is required: --to <address> or "
                         "--offset <bytes>");
      return;
    }

    Status error;
    addr_t addr = OptionArgParser::ToAddress(&m_exe_ctx, command[0].ref(),
                                             LLDB_INVALID_ADDRESS, &error);
    if (addr == LLDB_INVALID_ADDRESS) {
      result.AppendErrorWithFormat("'%s' is not an address: %s",
                                   command[0].c_str(), error.AsCString());
      return;
    }

    Address start;
    if (!target.ResolveLoadAddress(addr, start))
      start = Address(addr);

    // Live memory, because the question is about the code that will run. A site
    // that has already been patched reads as the patch, which is the honest
    // answer.
    DisassemblerSP disassembler_sp = Disassembler::DisassembleRange(
        target.GetArchitecture(), /*plugin_name=*/nullptr, /*flavor=*/nullptr,
        /*cpu=*/nullptr, /*features=*/nullptr, target,
        AddressRange(start,
                     target.GetArchitecture().GetMaximumOpcodeByteSize()),
        /*force_live_memory=*/true);

    if (!disassembler_sp) {
      result.AppendErrorWithFormat("couldn't disassemble at 0x%" PRIx64, addr);
      return;
    }

    InstructionSP instruction =
        disassembler_sp->GetInstructionList().GetInstructionAtIndex(0);
    if (!instruction) {
      result.AppendErrorWithFormat("no instruction at 0x%" PRIx64, addr);
      return;
    }

    // Without a process there is no load address, so fall back to the file
    // address. Every address below has to stay in whichever domain this picks:
    // GetReferencedAddress() answers in the domain of its argument, and
    // Relocate() computes a displacement between the two it is handed.
    const Address &address = instruction->GetAddress();
    addr_t from = address.GetLoadAddress(&target);
    if (from == LLDB_INVALID_ADDRESS)
      from = address.GetFileAddress();

    // Unsigned arithmetic so that a negative offset wraps rather than
    // overflowing a signed value.
    const addr_t to = m_options.to
                          ? *m_options.to
                          : from + static_cast<addr_t>(*m_options.offset);

    Stream &s = result.GetOutputStream();
    ExecutionContext exe_ctx(&target, false);
    const Colors colors(GetDebugger().GetUseColor());
    PrintInstruction(s, exe_ctx, from, *instruction, colors);

    // Two tiers, reported separately: whether a relocated form exists at all
    // does not depend on the destination, so a refusal here means the
    // instruction can never be moved, no matter how the trampoline is placed.
    llvm::Expected<Instruction::RelocationSize> room =
        instruction->GetRelocationSize();
    if (!room) {
      s.Format("  relocatable     no, at any address: {0}\n",
               llvm::toString(room.takeError()));
      result.SetStatus(eReturnStatusSuccessFinishResult);
      return;
    }

    s.Format("  relocatable     yes, {0} bytes of code{1}\n", room->code,
             room->data ? llvm::formatv(" and {0} of data", room->data).str()
                        : std::string());

    const int64_t distance =
        static_cast<int64_t>(to) - static_cast<int64_t>(from);
    const uint64_t magnitude = distance < 0 ? -static_cast<uint64_t>(distance)
                                            : static_cast<uint64_t>(distance);
    s.Format("  moves to        {0} ({1}{2:x} bytes)\n", colors.Address(to),
             distance < 0 ? "-" : "+", magnitude);

    // GetReferencedAddress() is asked at the address the instruction executes
    // at now, which is what the copy has to keep referring to. An instruction
    // that refers to nothing hands over an invalid address, which Relocate()
    // ignores.
    const addr_t referenced =
        instruction->GetReferencedAddress(from).value_or(LLDB_INVALID_ADDRESS);

    llvm::SmallVector<uint8_t, 16> code;
    if (llvm::Error relocation_error =
            instruction->Relocate(from, to, referenced, code)) {
      // The second tier. A relocated form exists, but not one that works from
      // this destination, which is almost always a displacement that no longer
      // fits.
      s.Format("  refused         {0}\n",
               llvm::toString(std::move(relocation_error)));
      result.SetStatus(eReturnStatusSuccessFinishResult);
      return;
    }

    // A caller sizes the trampoline from GetRelocationSize() before it has an
    // address, so the two disagreeing is a plugin bug that would otherwise only
    // show up as a corrupt trampoline.
    if (code.size() != room->code)
      s.Format("  size mismatch   {0} bytes reserved but {1} produced\n",
               room->code, code.size());

    s.PutCString("  relocated code  ");
    for (size_t i = 0; i < code.size(); ++i)
      s.Format("{0}{1:x-2}", i ? " " : "", code[i]);
    s.PutCString("\n");

    PrintRelocatedCode(s, target, to, code, colors);
    result.SetStatus(eReturnStatusSuccessFinishResult);
  }

  CommandOptions m_options;
};

#define LLDB_OPTIONS_instruction_patch_site
#include "CommandOptions.inc"

/// CommandObjectInstructionPatchSite
///
/// Answer, before anything is written, the question every code patcher has to
/// ask: can this many bytes here be replaced with a branch, and can what that
/// displaces run somewhere else. The answers come from PatchSiteAnalysis, so
/// this is also the way to see why a fast conditional breakpoint refused a
/// site, which otherwise takes a rebuild with logging added.
class CommandObjectInstructionPatchSite : public CommandObjectParsed {
public:
  class CommandOptions : public Options {
  public:
    CommandOptions() = default;
    ~CommandOptions() override = default;

    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override {
      Status error;
      switch (m_getopt_table[option_idx].val) {
      case 's':
        if (option_arg.getAsInteger(0, size) || !size)
          error = Status::FromErrorStringWithFormat("invalid patch size '%s'",
                                                    option_arg.str().c_str());
        break;
      case 'r':
        registers.push_back(option_arg.str());
        break;
      default:
        llvm_unreachable("Unimplemented option");
      }
      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      size = 0;
      registers.clear();
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
      return llvm::ArrayRef(g_instruction_patch_site_options);
    }

    /// Zero until DoExecute() has had a chance to ask the ABI.
    size_t size = 0;
    std::vector<std::string> registers;
  };

  /// Requires a live, stopped process: one of the conditions is that no thread
  /// is executing inside the range, which is a question about threads, and the
  /// analysis reads the code that is actually mapped rather than the code on
  /// disk.
  CommandObjectInstructionPatchSite(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "instruction patch-site",
            "Report whether the instructions at an address can be replaced "
            "with a branch, and what it would take to run them out of line.",
            nullptr,
            eCommandRequiresTarget | eCommandRequiresProcess |
                eCommandProcessMustBeLaunched | eCommandProcessMustBePaused) {
    AddSimpleArgumentList(eArgTypeAddressOrExpression);
  }

  ~CommandObjectInstructionPatchSite() override = default;

  Options *GetOptions() override { return &m_options; }

protected:
  /// Print one requirement and whether the site meets it.
  ///
  /// \return
  ///     Whether the requirement is met, so that the verdict is built out of
  ///     the same answers the user is reading and cannot disagree with them.
  static bool Report(Stream &s, llvm::StringRef requirement, llvm::Error error,
                     const Colors &colors) {
    if (!error) {
      s.Format("  {0}  {1}\n", colors.Mark(true), requirement);
      return true;
    }

    s.Format("  {0}  {1}\n", colors.Mark(false), requirement);
    // Indented under the requirement it explains. These are the same sentences
    // a fast conditional breakpoint prints when it declines a site, so they are
    // shown as written rather than folded into a summary.
    s.Format("     {0}\n", llvm::toString(std::move(error)));
    return false;
  }

  /// List the instructions the branch would displace.
  ///
  /// Whether each one is position independent is what decides between copying
  /// it into the trampoline and rewriting it, so it is reported per instruction
  /// rather than only as the total the caller has to reserve.
  void DescribeDisplaced(Stream &s, Target &target,
                         const PatchSiteAnalysis::PatchPlan &plan,
                         const Colors &colors) {
    s.Format("\n  displaces       {0} bytes, {1} bytes of relocated code\n",
             plan.displaced_size, plan.relocated_code_size);

    for (const InstructionSP &instruction : plan.displaced_instructions) {
      const addr_t pc = instruction->GetAddress().GetLoadAddress(&target);
      s.Format("    {0}  {1}\n", colors.Address(pc),
               colors.Code(*instruction, m_exe_ctx));

      llvm::Expected<Instruction::RelocationSize> room =
          instruction->GetRelocationSize();
      if (!room) {
        s.Format("      cannot run out of line: {0}\n",
                 llvm::toString(room.takeError()));
        continue;
      }

      // IsPCRelative() is biased towards saying yes, so it describes the
      // instruction rather than condemning it: the relocated form exists either
      // way, it is just larger when the reference has to be rebuilt.
      s.Format("      {0}, {1} bytes relocated{2}\n",
               instruction->IsPCRelative() ? "may depend on the pc"
                                           : "position independent",
               room->code,
               room->data ? llvm::formatv(" and {0} of data", room->data).str()
                          : std::string());
    }
  }

  void DoExecute(Args &command, CommandReturnObject &result) override {
    // Guaranteed by the flags this command was constructed with.
    Target &target = *GetTarget();
    Process &process = m_exe_ctx.GetProcessRef();

    if (command.GetArgumentCount() != 1) {
      result.AppendError("an address or an expression that yields one is "
                         "required");
      return;
    }

    Status error;
    addr_t addr = OptionArgParser::ToAddress(&m_exe_ctx, command[0].ref(),
                                             LLDB_INVALID_ADDRESS, &error);
    if (addr == LLDB_INVALID_ADDRESS) {
      result.AppendErrorWithFormat("'%s' is not an address: %s",
                                   command[0].c_str(), error.AsCString());
      return;
    }

    // The analysis reaches the function around the site through the section
    // this address belongs to, so an address that resolves to no section is
    // refused here rather than reported four times over as a site no function
    // covers.
    Address site;
    if (!target.ResolveLoadAddress(addr, site)) {
      result.AppendErrorWithFormat("0x%" PRIx64 " is not inside any section "
                                   "loaded in this process",
                                   addr);
      return;
    }

    size_t patch_size = m_options.size;
    llvm::StringRef size_source = "requested";
    if (!patch_size) {
      // The ABI knows how the branch it patches with is encoded, and that width
      // is not the pointer size: it is 5 bytes on x86_64 and 4 on AArch64.
      // Defaulting to it means asking whether a fast conditional breakpoint
      // would work here does not require knowing how one is built.
      if (ABI *abi = process.GetABI().get())
        patch_size = abi->GetJumpSize();
      size_source = "the width of this ABI's branch";
    }

    if (!patch_size) {
      result.AppendError("the ABI of this process does not describe how wide a "
                         "branch is, so --size is required");
      return;
    }

    // The checks report the site by its callable address, so print the same one
    // rather than what was typed, which on a Thumb target differs by a bit.
    const addr_t site_addr = site.GetCallableLoadAddress(&target);

    Stream &s = result.GetOutputStream();
    const Colors colors(GetDebugger().GetUseColor());

    StreamString description;
    site.Dump(&description, &target, Address::DumpStyleResolvedDescription,
              Address::DumpStyleModuleWithFileAddress);
    s.Format("{0}  {1}\n", colors.Address(site_addr), description.GetString());
    s.Format("  patch size      {0} bytes ({1})\n\n", patch_size, size_source);

    PatchSiteAnalysis::PatchPlan plan;

    // Bitwise, so that every check runs and gets a line even once one has
    // failed. The verdict is their conjunction, which is what
    // PatchSiteAnalysis::CanPatch() computes.
    bool can_patch = true;

    can_patch &= Report(s,
                        "the branch displaces whole instructions, and each of "
                        "them can run out of line",
                        PatchSiteAnalysis::CheckDisplacedInstructions(
                            process, site, patch_size, plan),
                        colors);

    can_patch &= Report(
        s,
        "nothing in the function branches into the bytes the "
        "branch would overwrite",
        PatchSiteAnalysis::CheckNoBranchIntoPatch(process, site, patch_size),
        colors);

    for (llvm::StringRef reg_name : m_options.registers)
      can_patch &= Report(
          s,
          llvm::formatv("{0} holds no value the program still needs here",
                        reg_name)
              .str(),
          PatchSiteAnalysis::CheckRegisterIsDead(process, site, reg_name),
          colors);

    can_patch &= Report(
        s,
        "no thread is stopped inside the bytes the branch "
        "would overwrite",
        PatchSiteAnalysis::CheckNoThreadInPatch(process, site, patch_size),
        colors);

    // The plan is filled in as the displaced instructions are examined, so
    // there is something worth showing even when that is the check that failed.
    if (!plan.displaced_instructions.empty())
      DescribeDisplaced(s, target, plan, colors);

    s.Format("\n{0} bytes at {1} {2} be replaced with a branch.\n", patch_size,
             colors.Address(site_addr), can_patch ? "can" : "cannot");

    // A site that cannot be patched is an answer, not a failure: the command
    // did what it was asked. Only a question that could not be asked at all is
    // an error.
    result.SetStatus(eReturnStatusSuccessFinishResult);
  }

  CommandOptions m_options;
};

CommandObjectMultiwordInstruction::CommandObjectMultiwordInstruction(
    CommandInterpreter &interpreter)
    : CommandObjectMultiword(
          interpreter, "instruction",
          "Commands for examining the instructions in a program.",
          "instruction <subcommand> [<subcommand-options>]") {
  LoadSubCommand("disassemble",
                 CommandObjectSP(new CommandObjectDisassemble(interpreter)));
  LoadSubCommand(
      "info", CommandObjectSP(new CommandObjectInstructionInfo(interpreter)));
  LoadSubCommand(
      "patch-site",
      CommandObjectSP(new CommandObjectInstructionPatchSite(interpreter)));
  LoadSubCommand(
      "reach", CommandObjectSP(new CommandObjectInstructionReach(interpreter)));
  LoadSubCommand(
      "relocate",
      CommandObjectSP(new CommandObjectInstructionRelocate(interpreter)));
}

CommandObjectMultiwordInstruction::~CommandObjectMultiwordInstruction() =
    default;
