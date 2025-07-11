//===- AArch64InstrInfo.h - AArch64 Instruction Information -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the AArch64 implementation of the TargetInstrInfo class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AARCH64_AARCH64INSTRINFO_H
#define LLVM_LIB_TARGET_AARCH64_AARCH64INSTRINFO_H

#include "AArch64.h"
#include "AArch64RegisterInfo.h"
#include "AArch64StackOffset.h"
#include "llvm/ADT/Optional.h"
#include "llvm/CodeGen/MachineCombinerPattern.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

#define GET_INSTRINFO_HEADER
#include "AArch64GenInstrInfo.inc"

namespace llvm {

class AArch64Subtarget;
class AArch64TargetMachine;

static const MachineMemOperand::Flags MOSuppressPair =
    MachineMemOperand::MOTargetFlag1;
static const MachineMemOperand::Flags MOStridedAccess =
    MachineMemOperand::MOTargetFlag2;

#define FALKOR_STRIDED_ACCESS_MD "falkor.strided.access"

class AArch64InstrInfo final : public AArch64GenInstrInfo {
  const AArch64RegisterInfo RI;
  const AArch64Subtarget &Subtarget;

public:
  explicit AArch64InstrInfo(const AArch64Subtarget &STI);

  /// getRegisterInfo - TargetInstrInfo is a superset of MRegister info.  As
  /// such, whenever a client has an instance of instruction info, it should
  /// always be able to get register info as well (through this method).
  const AArch64RegisterInfo &getRegisterInfo() const { return RI; }

  unsigned getInstSizeInBytes(const MachineInstr &MI) const override;

  bool isAsCheapAsAMove(const MachineInstr &MI) const override;

  bool isCoalescableExtInstr(const MachineInstr &MI, unsigned &SrcReg,
                             unsigned &DstReg, unsigned &SubIdx) const override;

  bool
  areMemAccessesTriviallyDisjoint(const MachineInstr &MIa,
                                  const MachineInstr &MIb) const override;

  unsigned isLoadFromStackSlot(const MachineInstr &MI,
                               int &FrameIndex) const override;
  unsigned isStoreToStackSlot(const MachineInstr &MI,
                              int &FrameIndex) const override;

  /// Does this instruction set its full destination register to zero?
  static bool isGPRZero(const MachineInstr &MI);

  /// Does this instruction rename a GPR without modifying bits?
  static bool isGPRCopy(const MachineInstr &MI);

  /// Does this instruction rename an FPR without modifying bits?
  static bool isFPRCopy(const MachineInstr &MI);

  /// Return true if pairing the given load or store is hinted to be
  /// unprofitable.
  static bool isLdStPairSuppressed(const MachineInstr &MI);

  /// Return true if the given load or store is a strided memory access.
  static bool isStridedAccess(const MachineInstr &MI);

  /// Return true if this is an unscaled load/store.
  static bool isUnscaledLdSt(unsigned Opc);
  static bool isUnscaledLdSt(MachineInstr &MI) {
    return isUnscaledLdSt(MI.getOpcode());
  }

  /// Returns the unscaled load/store for the scaled load/store opcode,
  /// if there is a corresponding unscaled variant available.
  static Optional<unsigned> getUnscaledLdSt(unsigned Opc);

  /// Scaling factor for (scaled or unscaled) load or store.
  static int getMemScale(unsigned Opc);
  static int getMemScale(const MachineInstr &MI) {
    return getMemScale(MI.getOpcode());
  }


  /// Returns the index for the immediate for a given instruction.
  static unsigned getLoadStoreImmIdx(unsigned Opc);

  /// Return true if pairing the given load or store may be paired with another.
  static bool isPairableLdStInst(const MachineInstr &MI);

  /// Return the opcode that set flags when possible.  The caller is
  /// responsible for ensuring the opc has a flag setting equivalent.
  static unsigned convertToFlagSettingOpc(unsigned Opc, bool &Is64Bit);

  /// Return true if this is a load/store that can be potentially paired/merged.
  bool isCandidateToMergeOrPair(const MachineInstr &MI) const;

  /// Hint that pairing the given load or store is unprofitable.
  static void suppressLdStPair(MachineInstr &MI);

  bool getMemOperandsWithOffset(
      const MachineInstr &MI, SmallVectorImpl<const MachineOperand *> &BaseOps,
      int64_t &Offset, const TargetRegisterInfo *TRI) const override;

  bool getMemOperandWithOffsetWidth(const MachineInstr &MI,
                                    const MachineOperand *&BaseOp,
                                    int64_t &Offset, unsigned &Width,
                                    const TargetRegisterInfo *TRI) const;

  /// Return the immediate offset of the base register in a load/store \p LdSt.
  MachineOperand &getMemOpBaseRegImmOfsOffsetOperand(MachineInstr &LdSt) const;

  /// Returns true if opcode \p Opc is a memory operation. If it is, set
  /// \p Scale, \p Width, \p MinOffset, and \p MaxOffset accordingly.
  ///
  /// For unscaled instructions, \p Scale is set to 1.
  static bool getMemOpInfo(unsigned Opcode, unsigned &Scale, unsigned &Width,
                           int64_t &MinOffset, int64_t &MaxOffset);

  bool shouldClusterMemOps(ArrayRef<const MachineOperand *> BaseOps1,
                           ArrayRef<const MachineOperand *> BaseOps2,
                           unsigned NumLoads) const override;

  void copyPhysRegTuple(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
                        const DebugLoc &DL, MCRegister DestReg,
                        MCRegister SrcReg, bool KillSrc, unsigned Opcode,
                        llvm::ArrayRef<unsigned> Indices) const;
  void copyGPRRegTuple(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
                       DebugLoc DL, unsigned DestReg, unsigned SrcReg,
                       bool KillSrc, unsigned Opcode, unsigned ZeroReg,
                       llvm::ArrayRef<unsigned> Indices) const;
  void copyPhysReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
                   const DebugLoc &DL, MCRegister DestReg, MCRegister SrcReg,
                   bool KillSrc) const override;

  void storeRegToStackSlot(MachineBasicBlock &MBB,
                           MachineBasicBlock::iterator MBBI, Register SrcReg,
                           bool isKill, int FrameIndex,
                           const TargetRegisterClass *RC,
                           const TargetRegisterInfo *TRI) const override;

  void loadRegFromStackSlot(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator MBBI, Register DestReg,
                            int FrameIndex, const TargetRegisterClass *RC,
                            const TargetRegisterInfo *TRI) const override;

  // This tells target independent code that it is okay to pass instructions
  // with subreg operands to foldMemoryOperandImpl.
  bool isSubregFoldable() const override { return true; }

  using TargetInstrInfo::foldMemoryOperandImpl;
  MachineInstr *
  foldMemoryOperandImpl(MachineFunction &MF, MachineInstr &MI,
                        ArrayRef<unsigned> Ops,
                        MachineBasicBlock::iterator InsertPt, int FrameIndex,
                        LiveIntervals *LIS = nullptr,
                        VirtRegMap *VRM = nullptr) const override;

  /// \returns true if a branch from an instruction with opcode \p BranchOpc
  ///  bytes is capable of jumping to a position \p BrOffset bytes away.
  bool isBranchOffsetInRange(unsigned BranchOpc,
                             int64_t BrOffset) const override;

  MachineBasicBlock *getBranchDestBlock(const MachineInstr &MI) const override;

  bool analyzeBranch(MachineBasicBlock &MBB, MachineBasicBlock *&TBB,
                     MachineBasicBlock *&FBB,
                     SmallVectorImpl<MachineOperand> &Cond,
                     bool AllowModify = false) const override;
  unsigned removeBranch(MachineBasicBlock &MBB,
                        int *BytesRemoved = nullptr) const override;
  unsigned insertBranch(MachineBasicBlock &MBB, MachineBasicBlock *TBB,
                        MachineBasicBlock *FBB, ArrayRef<MachineOperand> Cond,
                        const DebugLoc &DL,
                        int *BytesAdded = nullptr) const override;
  bool
  reverseBranchCondition(SmallVectorImpl<MachineOperand> &Cond) const override;
  bool canInsertSelect(const MachineBasicBlock &, ArrayRef<MachineOperand> Cond,
                       unsigned, unsigned, unsigned, int &, int &,
                       int &) const override;
  void insertSelect(MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
                    const DebugLoc &DL, unsigned DstReg,
                    ArrayRef<MachineOperand> Cond, unsigned TrueReg,
                    unsigned FalseReg) const override;
  void getNoop(MCInst &NopInst) const override;

  bool isSchedulingBoundary(const MachineInstr &MI,
                            const MachineBasicBlock *MBB,
                            const MachineFunction &MF) const override;

  /// analyzeCompare - For a comparison instruction, return the source registers
  /// in SrcReg and SrcReg2, and the value it compares against in CmpValue.
  /// Return true if the comparison instruction can be analyzed.
  bool analyzeCompare(const MachineInstr &MI, unsigned &SrcReg,
                      unsigned &SrcReg2, int &CmpMask,
                      int &CmpValue) const override;
  /// optimizeCompareInstr - Convert the instruction supplying the argument to
  /// the comparison into one that sets the zero bit in the flags register.
  bool optimizeCompareInstr(MachineInstr &CmpInstr, unsigned SrcReg,
                            unsigned SrcReg2, int CmpMask, int CmpValue,
                            const MachineRegisterInfo *MRI) const override;
  bool optimizeCondBranch(MachineInstr &MI) const override;

  /// Return true when a code sequence can improve throughput. It
  /// should be called only for instructions in loops.
  /// \param Pattern - combiner pattern
  bool isThroughputPattern(MachineCombinerPattern Pattern) const override;
  /// Return true when there is potentially a faster code sequence
  /// for an instruction chain ending in ``Root``. All potential patterns are
  /// listed in the ``Patterns`` array.
  bool getMachineCombinerPatterns(
      MachineInstr &Root,
      SmallVectorImpl<MachineCombinerPattern> &Patterns) const override;
  /// Return true when Inst is associative and commutative so that it can be
  /// reassociated.
  bool isAssociativeAndCommutative(const MachineInstr &Inst) const override;
  /// When getMachineCombinerPatterns() finds patterns, this function generates
  /// the instructions that could replace the original code sequence
  void genAlternativeCodeSequence(
      MachineInstr &Root, MachineCombinerPattern Pattern,
      SmallVectorImpl<MachineInstr *> &InsInstrs,
      SmallVectorImpl<MachineInstr *> &DelInstrs,
      DenseMap<unsigned, unsigned> &InstrIdxForVirtReg) const override;
  /// AArch64 supports MachineCombiner.
  bool useMachineCombiner() const override;

  bool expandPostRAPseudo(MachineInstr &MI) const override;

  std::pair<unsigned, unsigned>
  decomposeMachineOperandsTargetFlags(unsigned TF) const override;
  ArrayRef<std::pair<unsigned, const char *>>
  getSerializableDirectMachineOperandTargetFlags() const override;
  ArrayRef<std::pair<unsigned, const char *>>
  getSerializableBitmaskMachineOperandTargetFlags() const override;
  ArrayRef<std::pair<MachineMemOperand::Flags, const char *>>
  getSerializableMachineMemOperandTargetFlags() const override;

  bool isFunctionSafeToOutlineFrom(MachineFunction &MF,
                                   bool OutlineFromLinkOnceODRs) const override;
  outliner::OutlinedFunction getOutliningCandidateInfo(
      std::vector<outliner::Candidate> &RepeatedSequenceLocs) const override;
  outliner::InstrType
  getOutliningType(MachineBasicBlock::iterator &MIT, unsigned Flags) const override;
  bool isMBBSafeToOutlineFrom(MachineBasicBlock &MBB,
                              unsigned &Flags) const override;
  void buildOutlinedFrame(MachineBasicBlock &MBB, MachineFunction &MF,
                          const outliner::OutlinedFunction &OF) const override;
  MachineBasicBlock::iterator
  insertOutlinedCall(Module &M, MachineBasicBlock &MBB,
                     MachineBasicBlock::iterator &It, MachineFunction &MF,
                     const outliner::Candidate &C) const override;
  bool shouldOutlineFromFunctionByDefault(MachineFunction &MF) const override;
  /// Returns true if the instruction has a shift by immediate that can be
  /// executed in one cycle less.
  static bool isFalkorShiftExtFast(const MachineInstr &MI);
  /// Return true if the instructions is a SEH instruciton used for unwinding
  /// on Windows.
  static bool isSEHInstruction(const MachineInstr &MI);

  Optional<RegImmPair> isAddImmediate(const MachineInstr &MI,
                                      Register Reg) const override;

  Optional<ParamLoadedValue> describeLoadedValue(const MachineInstr &MI,
                                                 Register Reg) const override;

#define GET_INSTRINFO_HELPER_DECLS
#include "AArch64GenInstrInfo.inc"

protected:
  /// If the specific machine instruction is an instruction that moves/copies
  /// value from one register to another register return destination and source
  /// registers as machine operands.
  Optional<DestSourcePair>
  isCopyInstrImpl(const MachineInstr &MI) const override;

private:
  /// Sets the offsets on outlined instructions in \p MBB which use SP
  /// so that they will be valid post-outlining.
  ///
  /// \param MBB A \p MachineBasicBlock in an outlined function.
  void fixupPostOutline(MachineBasicBlock &MBB) const;

  void instantiateCondBranch(MachineBasicBlock &MBB, const DebugLoc &DL,
                             MachineBasicBlock *TBB,
                             ArrayRef<MachineOperand> Cond) const;
  bool substituteCmpToZero(MachineInstr &CmpInstr, unsigned SrcReg,
                           const MachineRegisterInfo *MRI) const;

  /// Returns an unused general-purpose register which can be used for
  /// constructing an outlined call if one exists. Returns 0 otherwise.
  unsigned findRegisterToSaveLRTo(const outliner::Candidate &C) const;
};

/// emitFrameOffset - Emit instructions as needed to set DestReg to SrcReg
/// plus Offset.  This is intended to be used from within the prolog/epilog
/// insertion (PEI) pass, where a virtual scratch register may be allocated
/// if necessary, to be replaced by the scavenger at the end of PEI.
void emitFrameOffset(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
                     const DebugLoc &DL, unsigned DestReg, unsigned SrcReg,
                     StackOffset Offset, const TargetInstrInfo *TII,
                     MachineInstr::MIFlag = MachineInstr::NoFlags,
                     bool SetNZCV = false, bool NeedsWinCFI = false,
                     bool *HasWinCFI = nullptr);

/// rewriteAArch64FrameIndex - Rewrite MI to access 'Offset' bytes from the
/// FP. Return false if the offset could not be handled directly in MI, and
/// return the left-over portion by reference.
bool rewriteAArch64FrameIndex(MachineInstr &MI, unsigned FrameRegIdx,
                              unsigned FrameReg, StackOffset &Offset,
                              const AArch64InstrInfo *TII);

/// Use to report the frame offset status in isAArch64FrameOffsetLegal.
enum AArch64FrameOffsetStatus {
  AArch64FrameOffsetCannotUpdate = 0x0, ///< Offset cannot apply.
  AArch64FrameOffsetIsLegal = 0x1,      ///< Offset is legal.
  AArch64FrameOffsetCanUpdate = 0x2     ///< Offset can apply, at least partly.
};

/// Check if the @p Offset is a valid frame offset for @p MI.
/// The returned value reports the validity of the frame offset for @p MI.
/// It uses the values defined by AArch64FrameOffsetStatus for that.
/// If result == AArch64FrameOffsetCannotUpdate, @p MI cannot be updated to
/// use an offset.eq
/// If result & AArch64FrameOffsetIsLegal, @p Offset can completely be
/// rewritten in @p MI.
/// If result & AArch64FrameOffsetCanUpdate, @p Offset contains the
/// amount that is off the limit of the legal offset.
/// If set, @p OutUseUnscaledOp will contain the whether @p MI should be
/// turned into an unscaled operator, which opcode is in @p OutUnscaledOp.
/// If set, @p EmittableOffset contains the amount that can be set in @p MI
/// (possibly with @p OutUnscaledOp if OutUseUnscaledOp is true) and that
/// is a legal offset.
int isAArch64FrameOffsetLegal(const MachineInstr &MI, StackOffset &Offset,
                              bool *OutUseUnscaledOp = nullptr,
                              unsigned *OutUnscaledOp = nullptr,
                              int64_t *EmittableOffset = nullptr);

static inline bool isUncondBranchOpcode(int Opc) { return Opc == AArch64::B; }

static inline bool isCondBranchOpcode(int Opc) {
  switch (Opc) {
  case AArch64::Bcc:
  case AArch64::CBZW:
  case AArch64::CBZX:
  case AArch64::CBNZW:
  case AArch64::CBNZX:
  case AArch64::TBZW:
  case AArch64::TBZX:
  case AArch64::TBNZW:
  case AArch64::TBNZX:
    return true;
  default:
    return false;
  }
}

static inline bool isIndirectBranchOpcode(int Opc) {
  return Opc == AArch64::BR;
}

static inline unsigned getXPACOpcodeForKey(AArch64PACKey::ID K) {
  using namespace AArch64PACKey;
  switch (K) {
  case IA: case IB: return AArch64::XPACI;
  case DA: case DB: return AArch64::XPACD;
  }
}

static inline unsigned getAUTOpcodeForKey(AArch64PACKey::ID K, bool Zero) {
  using namespace AArch64PACKey;
  switch (K) {
  case IA: return Zero ? AArch64::AUTIZA : AArch64::AUTIA;
  case IB: return Zero ? AArch64::AUTIZB : AArch64::AUTIB;
  case DA: return Zero ? AArch64::AUTDZA : AArch64::AUTDA;
  case DB: return Zero ? AArch64::AUTDZB : AArch64::AUTDB;
  }
}

static inline unsigned getPACOpcodeForKey(AArch64PACKey::ID K, bool Zero) {
  using namespace AArch64PACKey;
  switch (K) {
  case IA: return Zero ? AArch64::PACIZA : AArch64::PACIA;
  case IB: return Zero ? AArch64::PACIZB : AArch64::PACIB;
  case DA: return Zero ? AArch64::PACDZA : AArch64::PACDA;
  case DB: return Zero ? AArch64::PACDZB : AArch64::PACDB;
  }
}

// struct TSFlags {
#define TSFLAG_ELEMENT_SIZE_TYPE(X)      (X)       // 3-bits
#define TSFLAG_DESTRUCTIVE_INST_TYPE(X) ((X) << 3) // 1-bit
// }

namespace AArch64 {

enum ElementSizeType {
  ElementSizeMask = TSFLAG_ELEMENT_SIZE_TYPE(0x7),
  ElementSizeNone = TSFLAG_ELEMENT_SIZE_TYPE(0x0),
  ElementSizeB    = TSFLAG_ELEMENT_SIZE_TYPE(0x1),
  ElementSizeH    = TSFLAG_ELEMENT_SIZE_TYPE(0x2),
  ElementSizeS    = TSFLAG_ELEMENT_SIZE_TYPE(0x3),
  ElementSizeD    = TSFLAG_ELEMENT_SIZE_TYPE(0x4),
};

enum DestructiveInstType {
  DestructiveInstTypeMask = TSFLAG_DESTRUCTIVE_INST_TYPE(0x1),
  NotDestructive          = TSFLAG_DESTRUCTIVE_INST_TYPE(0x0),
  DestructiveOther        = TSFLAG_DESTRUCTIVE_INST_TYPE(0x1),
};

#undef TSFLAG_ELEMENT_SIZE_TYPE
#undef TSFLAG_DESTRUCTIVE_INST_TYPE
}

} // end namespace llvm

#endif
