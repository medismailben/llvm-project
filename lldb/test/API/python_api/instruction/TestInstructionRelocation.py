"""
Test the SBInstruction API for relocating an instruction.

Relocation answers two separate questions: whether a copy of an instruction can
behave the same way from a different address at all, which does not depend on
where it goes, and whether it can still reach what it referred to from one
particular destination, which does. These cover both, and prove the second one
by decoding the bytes that come back rather than by asserting about them.
"""

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class InstructionRelocationTestCase(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

    # Far enough to be a real move, close enough to stay inside the +/-128MiB an
    # AArch64 immediate branch reaches, and instruction aligned.
    NEARBY_OFFSET = 0x1000
    # Past that reach, so the branch cannot be rewritten to keep its target.
    OUT_OF_RANGE_OFFSET = 0x10000000

    def setUp(self):
        TestBase.setUp(self)
        self.build()
        self.target = self.dbg.CreateTarget(self.getBuildArtifact("a.out"))
        self.assertTrue(self.target, VALID_TARGET)

    def find_call(self):
        """The first call in main, with the address it lives at.

        Found rather than assumed: which line holds it and what the compiler
        emits around it are both free to change. No process is launched, so the
        addresses here are file addresses and everything below stays in that
        domain.
        """
        main = self.target.FindFunctions("main").GetContextAtIndex(0).GetFunction()
        self.assertTrue(main, "no main to disassemble")

        for instruction in main.GetInstructions(self.target):
            if (
                instruction.DoesBranch()
                and instruction.GetMnemonic(self.target) == "bl"
            ):
                return instruction, instruction.GetAddress().GetFileAddress()

        self.fail("no call instruction in main to relocate")

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_call_reports_the_room_a_copy_needs(self):
        """A call can be moved out of line, and a copy is one instruction."""
        call, _ = self.find_call()

        error = lldb.SBError()
        code_size = call.GetRelocatedCodeSize(error)
        self.assertSuccess(error, "a call should be relocatable")
        self.assertEqual(code_size, call.GetByteSize())

        # Nothing accepted so far needs a constant placed next to the code, so a
        # caller reserving a trampoline only has to reserve code.
        data_size = call.GetRelocatedDataSize(error)
        self.assertSuccess(error, "a call should be relocatable")
        self.assertEqual(data_size, 0)

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_relocated_call_still_reaches_its_target(self):
        """A relocated call refers to the same address from its new home.

        This is the assertion that matters: the bytes are decoded back at the
        address they were produced for, and asked where they point. Copying the
        original bytes verbatim would pass every other check in this file and
        fail this one.
        """
        call, origin = self.find_call()

        referenced = call.GetReferencedAddress(origin)
        self.assertNotEqual(
            referenced,
            lldb.LLDB_INVALID_ADDRESS,
            "a direct call should have a known target",
        )

        destination = origin + self.NEARBY_OFFSET

        error = lldb.SBError()
        data = call.Relocate(self.target, origin, destination, referenced, error)
        self.assertSuccess(error, "relocating a call within branch range")
        self.assertTrue(data.IsValid())

        code = bytearray(data.uint8s)
        # A caller sizes a trampoline from GetRelocatedCodeSize() before it has
        # a destination, so the two disagreeing would silently corrupt it.
        self.assertEqual(len(code), call.GetRelocatedCodeSize(error))

        # Decoded at the destination, because an operand rewritten for a new
        # address is only provably right when it is read from that address.
        relocated = self.target.GetInstructions(
            lldb.SBAddress(destination, self.target), code
        )
        self.assertEqual(relocated.GetSize(), 1)

        copy = relocated.GetInstructionAtIndex(0)
        self.assertEqual(copy.GetMnemonic(self.target), "bl")
        self.assertEqual(
            copy.GetReferencedAddress(destination),
            referenced,
            "the relocated call points somewhere other than the original did",
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_destination_out_of_reach_is_refused(self):
        """A relocatable instruction is still refused when it cannot reach.

        The two questions are answered separately for a reason: this call has a
        relocated form, and it is only this destination that has no working one.
        A caller that treated the first answer as final would emit a branch to
        the wrong place.
        """
        call, origin = self.find_call()

        error = lldb.SBError()
        self.assertNotEqual(call.GetRelocatedCodeSize(error), 0)
        self.assertSuccess(error, "a call should be relocatable in principle")

        referenced = call.GetReferencedAddress(origin)
        destination = origin + self.OUT_OF_RANGE_OFFSET

        data = call.Relocate(self.target, origin, destination, referenced, error)
        self.assertTrue(error.Fail(), "a branch cannot reach 256MiB away")
        # A default constructed SBData holds an empty DataExtractor rather than
        # nothing, so it reports valid. Emptiness is what says no bytes came
        # back.
        self.assertEqual(data.GetByteSize(), 0)

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_invalid_instruction_reports_an_error(self):
        """Every entry point explains itself rather than looking relocatable.

        Zero is a legitimate data size, so a default constructed instruction
        that quietly answered zero would be indistinguishable from a real one.
        """
        instruction = lldb.SBInstruction()
        self.assertFalse(instruction.IsValid())

        error = lldb.SBError()
        self.assertEqual(instruction.GetRelocatedCodeSize(error), 0)
        self.assertTrue(error.Fail())

        self.assertEqual(instruction.GetRelocatedDataSize(error), 0)
        self.assertTrue(error.Fail())

        self.assertEqual(instruction.GetReferencedAddress(0), lldb.LLDB_INVALID_ADDRESS)

        data = instruction.Relocate(self.target, 0, 0, lldb.LLDB_INVALID_ADDRESS, error)
        self.assertTrue(error.Fail())
        self.assertEqual(data.GetByteSize(), 0)
