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

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_locals_read_in_the_clone(self):
        """The locals the optimiser discarded are readable in the clone.

        Stopped inside the loop rather than at the clone's entry. At the entry
        the prologue has not run, so the parameter is still in its argument
        register while the debug info already describes it at the stack slot the
        prologue will spill it to, and reading it gives whatever that slot
        happened to hold.
        """
        process, target, entry = self.arm()
        self.expect("frame deoptimize", substrs=["now runs a clone"])

        contexts = target.FindFunctions("$__lldb_deopt_hot")
        self.assertEqual(contexts.GetSize(), 1,
                         "expected exactly one clone, found %d" %
                         contexts.GetSize())
        clone_function = contexts.GetContextAtIndex(0).GetFunction()
        self.assertTrue(clone_function,
                        "the clone has no debug info, so it is a bare address "
                        "and none of what follows can work")

        # The clone's text is the original's from its signature down, so the
        # line holding 'acc += tmp;' is that far below the signature in both.
        signature = line_number("main.c", "int hot(int n) {")
        wanted = line_number("main.c", "// break here") - signature + 1
        source = clone_function.GetStartAddress().GetLineEntry().GetFileSpec()
        in_the_loop = target.BreakpointCreateByLocation(source, wanted)
        self.assertEqual(in_the_loop.GetNumLocations(), 1,
                         "no code at line %d of the clone" % wanted)

        # Reaches the clone on the second call. The first is already running and
        # keeps the optimised body; the breakpoint that stopped us is in that
        # body, which nothing enters again now that the entry branches away.
        process.Continue()
        self.assertState(process.GetState(), lldb.eStateStopped)
        frame = process.GetSelectedThread().GetFrameAtIndex(0)
        self.assertIn("$__lldb_deopt_hot", frame.GetFunctionName(),
                      "the clone's frame does not symbolicate")

        n = frame.FindVariable("n")
        self.assertTrue(n.IsValid(), "'n' is not readable in the clone")
        self.assertEqual(n.GetValueAsSigned(), 10)

        # The payoff: 'tmp' is the variable the optimiser discarded, which
        # test_variables_are_unavailable_without_it asserts cannot be read in
        # the original. Two iterations rather than one, because 'tmp' is 0 on
        # the first and a slot of zeroed stack would read the same.
        tmp = frame.FindVariable("tmp")
        self.assertTrue(tmp.IsValid(),
                        "'tmp' is not readable in the clone either, so the "
                        "clone bought nothing")
        self.assertEqual(tmp.GetValueAsSigned(), 0)

        process.Continue()
        self.assertState(process.GetState(), lldb.eStateStopped)
        frame = process.GetSelectedThread().GetFrameAtIndex(0)
        self.assertEqual(frame.FindVariable("tmp").GetValueAsSigned(), 3,
                         "'tmp' does not track the loop, so what was read is "
                         "not really it")
