//===-- ABIAArch64Test.cpp ------------------------------------------------===//

//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Plugins/ABI/AArch64/ABIMacOSX_arm64.h"
#include "Plugins/ABI/AArch64/ABISysV_arm64.h"
#include "Utility/ARM64_DWARF_Registers.h"
#include "Utility/ARM64_ehframe_Registers.h"
#include "lldb/Target/DynamicRegisterInfo.h"
#include "lldb/Utility/ArchSpec.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/TargetSelect.h"
#include "gtest/gtest.h"
#include <vector>

using namespace lldb_private;
using namespace lldb;

class ABIAArch64TestFixture : public testing::TestWithParam<llvm::StringRef> {
public:
  static void SetUpTestCase();
  static void TearDownTestCase();

  //  virtual void SetUp() override { }
  //  virtual void TearDown() override { }

protected:
};

void ABIAArch64TestFixture::SetUpTestCase() {
  LLVMInitializeAArch64TargetInfo();
  LLVMInitializeAArch64TargetMC();
  ABISysV_arm64::Initialize();
  ABIMacOSX_arm64::Initialize();
}

void ABIAArch64TestFixture::TearDownTestCase() {
  ABISysV_arm64::Terminate();
  ABIMacOSX_arm64::Terminate();
  llvm::llvm_shutdown();
}

TEST_P(ABIAArch64TestFixture, AugmentRegisterInfo) {
  ABISP abi_sp = ABI::FindPlugin(ProcessSP(), ArchSpec(GetParam()));
  ASSERT_TRUE(abi_sp);
  using Register = DynamicRegisterInfo::Register;

  Register pc;
  pc.name = ConstString("pc");
  pc.alt_name = ConstString();
  pc.set_name = ConstString("GPR");
  std::vector<Register> regs{pc};

  abi_sp->AugmentRegisterInfo(regs);

  ASSERT_EQ(regs.size(), 1U);
  Register new_pc = regs[0];
  EXPECT_EQ(new_pc.name, pc.name);
  EXPECT_EQ(new_pc.set_name, pc.set_name);
  EXPECT_EQ(new_pc.regnum_ehframe, arm64_ehframe::pc);
  EXPECT_EQ(new_pc.regnum_dwarf, arm64_dwarf::pc);
}

TEST_P(ABIAArch64TestFixture, AugmentRegisterInfoThreadPointer) {
  ABISP abi_sp = ABI::FindPlugin(ProcessSP(), ArchSpec(GetParam()));
  ASSERT_TRUE(abi_sp);
  using Register = DynamicRegisterInfo::Register;

  Register tpidr;
  tpidr.name = ConstString("tpidr");
  tpidr.set_name = ConstString("GPR");
  std::vector<Register> regs{tpidr};

  abi_sp->AugmentRegisterInfo(regs);

  ASSERT_EQ(regs.size(), 1U);
  EXPECT_EQ(regs[0].regnum_generic,
            static_cast<uint32_t>(LLDB_REGNUM_GENERIC_TP));
}

INSTANTIATE_TEST_SUITE_P(ABIAArch64Tests, ABIAArch64TestFixture,
                         testing::Values("aarch64-pc-linux",
                                         "arm64-apple-macosx"));

// The patch a fast conditional breakpoint installs, and the branch back out of
// its trampoline, are encoded by hand rather than assembled, so the encodings
// are the one part of that path with no other check on it: a small test binary
// always has a hole within a direct branch's reach, so the wider forms below
// are never reached by the API tests.
//
// Every expectation here was produced by llvm-mc for the instruction named in
// the comment beside it, so a disagreement means this code and the assembler
// disagree rather than that a hand computation slipped.

/// The little endian word at \a index in an encoded sequence.
static uint32_t WordAt(llvm::ArrayRef<uint8_t> code, size_t index) {
  const size_t offset = index * ABIMacOSX_arm64::aarch64_instr_size;
  return code[offset] | (code[offset + 1] << 8) | (code[offset + 2] << 16) |
         (static_cast<uint32_t>(code[offset + 3]) << 24);
}

TEST(ABIMacOSXArm64Patch, EncodeBranch) {
  uint8_t code[ABIMacOSX_arm64::aarch64_instr_size];

  ABIMacOSX_arm64::EncodeBranch(8, code);
  EXPECT_EQ(WordAt(code, 0), 0x14000002U); // b #8

  // A negative offset has to keep its sign through the shift and the truncation
  // to the 26 bit field, which is what makes a backward branch to a trampoline
  // below the site work at all.
  ABIMacOSX_arm64::EncodeBranch(-16, code);
  EXPECT_EQ(WordAt(code, 0), 0x17fffffcU); // b #-16
}

TEST(ABIMacOSXArm64Patch, BranchRange) {
  // +/-128MiB, in whole instructions.
  EXPECT_TRUE(ABIMacOSX_arm64::IsBranchInRange(0));
  EXPECT_TRUE(ABIMacOSX_arm64::IsBranchInRange(128 * 1024 * 1024 - 4));
  EXPECT_TRUE(ABIMacOSX_arm64::IsBranchInRange(-128 * 1024 * 1024));
  EXPECT_FALSE(ABIMacOSX_arm64::IsBranchInRange(128 * 1024 * 1024));
  EXPECT_FALSE(ABIMacOSX_arm64::IsBranchInRange(-128 * 1024 * 1024 - 4));

  // Not a whole number of instructions, so no `b` encodes it however near it
  // is.
  EXPECT_FALSE(ABIMacOSX_arm64::IsBranchInRange(2));
}

TEST(ABIMacOSXArm64Patch, EncodeFarBranch) {
  uint8_t code[ABIMacOSX_arm64::aarch64_far_patch_size];

  // 256MiB up, which is past a direct branch's reach and is why this form
  // exists. The low 12 bits of the target become the `add`.
  ASSERT_FALSE(llvm::errorToBool(
      ABIMacOSX_arm64::EncodeFarBranch(0x100000000, 0x110000123, 16, code)));
  EXPECT_EQ(WordAt(code, 0), 0x90080010U); // adrp x16, #0x10000000
  EXPECT_EQ(WordAt(code, 1), 0x91048e10U); // add  x16, x16, #0x123
  EXPECT_EQ(WordAt(code, 2), 0xd61f0200U); // br   x16

  // Down, where the page count is negative and has to survive truncation to the
  // split 21 bit field.
  ASSERT_FALSE(llvm::errorToBool(
      ABIMacOSX_arm64::EncodeFarBranch(0x110000000, 0x100000000, 16, code)));
  EXPECT_EQ(WordAt(code, 0), 0x90f80010U); // adrp x16, #-0x10000000
  EXPECT_EQ(WordAt(code, 1), 0x91000210U); // add  x16, x16, #0
  EXPECT_EQ(WordAt(code, 2), 0xd61f0200U); // br   x16
}

TEST(ABIMacOSXArm64Patch, EncodeFarBranchOutOfReach) {
  uint8_t code[ABIMacOSX_arm64::aarch64_far_patch_size];

  // `adrp` counts 4KiB pages in a signed 21 bit field, so it reaches 4GiB. A
  // target beyond that has to be refused rather than encoded wrongly, since the
  // caller can still fall back to evaluating the condition out of process.
  EXPECT_TRUE(llvm::errorToBool(
      ABIMacOSX_arm64::EncodeFarBranch(0, 0x200000000000, 16, code)));
}

TEST(ABIMacOSXArm64Patch, EncodeFarBranchAbsolute) {
  uint8_t code[ABIMacOSX_arm64::aarch64_far_branch_slots *
               ABIMacOSX_arm64::aarch64_instr_size];

  // A whole 64 bit address in four halves, which is what the branch back out of
  // a far trampoline uses so that its own placement cannot put the site out of
  // reach.
  ABIMacOSX_arm64::EncodeFarBranchAbsolute(0x1000027a8, 16, code);
  EXPECT_EQ(WordAt(code, 0), 0xd284f510U); // movz x16, #0x27a8
  EXPECT_EQ(WordAt(code, 1), 0xf2a00010U); // movk x16, #0, lsl #16
  EXPECT_EQ(WordAt(code, 2), 0xf2c00030U); // movk x16, #1, lsl #32
  EXPECT_EQ(WordAt(code, 3), 0xf2e00010U); // movk x16, #0, lsl #48
  EXPECT_EQ(WordAt(code, 4), 0xd61f0200U); // br   x16
}
