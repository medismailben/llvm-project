"""
Test 'frame deoptimize', which replaces an optimised function with a clone of it
compiled without optimisation.

What makes this worth asserting rather than eyeballing is that the obvious check
is worthless: an unmodified clone computes what the original did, so a program
that still produces the right answer says nothing about whether the clone ran.
These tests discriminate instead, on the branch installed at the entry and on
execution arriving at its target.
"""

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class FrameDeoptimizeTestCase(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

    def setUp(self):
        TestBase.setUp(self)
        self.source = lldb.SBFileSpec("main.c")

    def arm(self):
        """Stop in the optimised 'hot' and return (process, target, entry)."""
        self.build()
        target = self.dbg.CreateTarget(self.getBuildArtifact("a.out"))
        self.assertTrue(target, VALID_TARGET)

        breakpoint = target.BreakpointCreateByName("hot")
        self.assertTrue(breakpoint, VALID_BREAKPOINT)

        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertTrue(process, PROCESS_IS_VALID)
        self.assertState(process.GetState(), lldb.eStateStopped)

        function = target.FindFunctions("hot").GetContextAtIndex(0).GetFunction()
        self.assertTrue(function, "no 'hot' to deoptimize")
        return process, target, function.GetStartAddress().GetLoadAddress(target)

    def branch_target(self, process, entry):
        """Where the branch installed at \a entry goes, decoded from the bytes.

        Read rather than taken from the command's output, so that what is
        asserted is what the inferior will actually execute.
        """
        error = lldb.SBError()
        word = int.from_bytes(process.ReadMemory(entry, 4, error), "little")
        self.assertSuccess(error, "reading the patched entry")
        imm26 = word & 0x03FFFFFF
        if imm26 & (1 << 25):
            imm26 -= 1 << 26
        self.assertEqual(word & 0xFC000000, 0x14000000,
                         "the entry should hold an unconditional branch, got "
                         "0x%08x" % word)
        return entry + imm26 * 4

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_variables_are_unavailable_without_it(self):
        """The problem the command exists for is real in this inferior.

        If the optimiser ever stops discarding these, the rest of these tests
        are measuring nothing, and this is the one that says so.
        """
        _, target, _ = self.arm()
        frame = target.GetProcess().GetSelectedThread().GetFrameAtIndex(0)

        # 'tmp' is the one to assert on. At the entry the optimiser still
        # describes 'n', 'acc' and 'i', at their initial values; 'tmp' is the
        # loop body temporary it removed along with the loop, so it has no DIE
        # here at all and lldb calls it an undeclared identifier.
        self.assertFalse(
            frame.FindVariable("tmp").IsValid(),
            "'tmp' is readable in the optimised build, so this inferior no "
            "longer demonstrates the problem",
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_entry_is_redirected(self):
        """The entry holds a branch, in memory the inferior will really run.

        Read through the raw packet as well as through lldb, because lldb masks
        reads over its own breakpoint traps with the saved opcode, so a read that
        agrees with what we wrote could be reading our own write back out of a
        buffer rather than out of the process.
        """
        process, target, entry = self.arm()
        self.expect("frame deoptimize", substrs=["now runs a clone"])

        clone = self.branch_target(process, entry)
        self.assertNotEqual(clone, entry, "the entry branches to itself")

        raw = self.dbg.GetCommandInterpreter()
        result = lldb.SBCommandReturnObject()
        raw.HandleCommand("process plugin packet send m%x,4" % entry, result)
        if result.Succeeded():
            # 087f0014 style: little endian bytes of the branch word.
            error = lldb.SBError()
            through_lldb = process.ReadMemory(entry, 4, error).hex()
            self.assertIn(through_lldb, result.GetOutput(),
                          "the branch lldb reports is not the branch in the "
                          "inferior, so the write was shadowed")

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_the_clone_runs(self):
        """Execution reaches the branch's target on the next call.

        This is the assertion the command's own output cannot make: it reports
        where it put the clone, not that anything ever went there.
        """
        process, target, entry = self.arm()
        self.expect("frame deoptimize", substrs=["now runs a clone"])

        clone = self.branch_target(process, entry)
        arrived = target.BreakpointCreateByAddress(clone)
        self.assertEqual(arrived.GetNumLocations(), 1,
                         "could not place a breakpoint in the clone")

        process.Continue()
        self.assertState(process.GetState(), lldb.eStateStopped,
                         "the second call did not reach the clone")
        self.assertEqual(
            process.GetSelectedThread().GetFrameAtIndex(0).GetPC(), clone,
            "stopped somewhere other than the clone",
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_the_inferior_still_works(self):
        """Replacing the function does not change what the program computes."""
        process, _, _ = self.arm()
        self.expect("frame deoptimize", substrs=["now runs a clone"])
        process.Continue()
        self.assertState(process.GetState(), lldb.eStateExited)
        self.assertEqual(process.GetExitStatus(), 0)

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_it_refuses_without_a_frame(self):
        """A command that needs a frame says so rather than crashing."""
        self.build()
        target = self.dbg.CreateTarget(self.getBuildArtifact("a.out"))
        self.assertTrue(target, VALID_TARGET)
        self.expect("frame deoptimize", error=True)

    @expectedFailureAll(
        bugnumber="the clone's frame does not symbolicate, so its locals do not "
                  "read; this is the point of the command and the test that will "
                  "start passing when it works"
    )
    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_locals_read_in_the_clone(self):
        """The locals the optimiser discarded are readable in the clone."""
        process, target, entry = self.arm()
        self.expect("frame deoptimize", substrs=["now runs a clone"])

        clone = self.branch_target(process, entry)
        target.BreakpointCreateByAddress(clone)
        process.Continue()
        self.assertState(process.GetState(), lldb.eStateStopped)

        frame = process.GetSelectedThread().GetFrameAtIndex(0)
        self.assertEqual(frame.GetFunctionName(), "$__lldb_deopt_hot",
                         "the clone's frame does not symbolicate")

        # The parameter, not a local: this stops at the clone's entry, where its
        # prologue has not run and `int acc = 0;` has not executed, so the locals
        # have slots but no meaningful contents yet. What this asserts is that the
        # clone carries usable debug info at all, which is what does not work.
        n = frame.FindVariable("n")
        self.assertTrue(n.IsValid(), "'n' is not readable in the clone")
        self.assertEqual(n.GetValueAsSigned(), 10)
