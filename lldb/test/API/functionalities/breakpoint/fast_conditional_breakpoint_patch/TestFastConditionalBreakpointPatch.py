"""
Test the patch a fast conditional breakpoint installs in the inferior.

The tests in TestFastConditionalBreakpoints.py cover arming an injected
condition and stopping on it. These cover what the patch itself does to the
inferior: that it is visible to a memory read, that it comes out when the
breakpoint is disabled and goes back in when it is enabled, that the inferior
resumes correctly through it, and that it does not outlive the debugger.
"""

import os
import time

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *


class FastConditionalBreakpointPatchTestCase(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

    def setUp(self):
        TestBase.setUp(self)
        self.source = lldb.SBFileSpec("main.c")
        self.comment = "break here"

    def arm(self, condition):
        """Create an injected conditional breakpoint and launch.

        Returns the (process, breakpoint, location) triple, having asserted that
        the condition really was injected rather than quietly handed back to the
        debugger.
        """
        self.build()
        target = self.dbg.CreateTarget(self.getBuildArtifact("a.out"))
        self.assertTrue(target, VALID_TARGET)

        breakpoint = target.BreakpointCreateBySourceRegex(
            self.comment, self.source)
        self.assertEqual(breakpoint.GetNumLocations(), 1, VALID_BREAKPOINT)

        location = breakpoint.GetLocationAtIndex(0)
        self.assertTrue(location.IsEnabled(), VALID_BREAKPOINT_LOCATION)
        location.SetCondition(condition)
        location.SetInjectCondition(True)
        self.assertTrue(location.GetInjectCondition())

        process = target.LaunchSimple(
            None, None, self.get_process_working_directory())
        self.assertTrue(process, PROCESS_IS_VALID)

        # Falling back to an out of process condition clears the flag, so this
        # is what keeps the rest of the test from passing against a breakpoint
        # the debugger is evaluating itself.
        self.assertTrue(
            location.GetInjectCondition(),
            "the condition was not injected, so it fell back to the debugger",
        )

        # An injected condition traps inside the JIT-ed expression, and lldb does
        # not currently attribute that trap to the breakpoint, so the stop
        # arrives as a plain EXC_BREAKPOINT rather than eStopReasonBreakpoint.
        # Assert the state rather than the reason: tying these tests to the
        # reason would make them fail the day that is improved.
        self.assertState(process.GetState(), lldb.eStateStopped)

        return process, breakpoint, location

    def read_word(self, process, address):
        error = lldb.SBError()
        content = process.ReadMemory(address, 4, error)
        self.assertSuccess(error, "reading the patched instruction")
        return int.from_bytes(content, byteorder="little")

    def branch_instruction(self, target, address):
        """The single instruction at \a address, which has to be a branch."""
        instructions = target.ReadInstructions(
            lldb.SBAddress(address, target), 1)
        self.assertEqual(instructions.GetSize(), 1)
        return instructions.GetInstructionAtIndex(0)

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "x86_64"]))
    @add_test_categories(["pyapi"])
    def test_patched_site_reads_as_a_branch(self):
        """A patched site reads back as the branch to the trampoline.

        The branch is the real code at that address once a condition is
        injected, so a memory read has to show it. It used to be masked with the
        site's saved-opcode buffer, which is never populated for an injected
        site, so the instruction read back as four zero bytes.

        This also catches a trap being installed over the branch, which made
        every hit stop in the debugger and evaluate the condition out of
        process.
        """
        process, _, location = self.arm("counter == 9")
        target = process.GetTarget()
        site = location.GetLoadAddress()

        word = self.read_word(process, site)
        self.assertNotEqual(
            word, 0, "the patched instruction read back as four zero bytes")

        instruction = self.branch_instruction(target, site)
        self.assertIn(
            instruction.GetMnemonic(target),
            ["b", "jmp"],
            "the patched site should hold a branch to the trampoline, got "
            + instruction.GetMnemonic(target),
        )

        # The branch has to land in the trampoline, and the trampoline has to
        # still be described by a module, or the stop cannot be symbolicated and
        # the unwinder cannot get back to the user's frame.
        comment = instruction.GetComment(target)
        operands = instruction.GetOperands(target)
        self.assertTrue(
            "trampoline" in comment or operands,
            "the branch target should resolve to the trampoline module",
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "x86_64"]))
    @add_test_categories(["pyapi"])
    def test_disable_removes_the_patch(self):
        """Disabling an injected breakpoint takes the branch back out.

        A disabled breakpoint must stop evaluating its condition in the
        inferior. Leaving the branch installed also means a detached or killed
        process keeps running code on the debugger's behalf.
        """
        process, breakpoint, location = self.arm("counter == 9")
        target = process.GetTarget()
        site = location.GetLoadAddress()

        patched = self.read_word(process, site)
        self.assertNotEqual(patched, 0)

        breakpoint.SetEnabled(False)
        displaced = self.read_word(process, site)
        self.assertNotEqual(
            displaced,
            patched,
            "disabling the breakpoint left the branch to the trampoline in "
            "place",
        )
        self.assertNotEqual(
            displaced, 0, "disabling the breakpoint zeroed the instruction")

        # And it goes back in. Note this currently rebuilds the site rather than
        # rewriting the branch, so it also covers arming a second time in one
        # session.
        breakpoint.SetEnabled(True)
        self.assertEqual(
            self.read_word(process, site),
            patched,
            "re-enabling the breakpoint did not restore the branch",
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "x86_64"]))
    @expectedFailureAll(
        bugnumber="an injected trap is not recognised as a breakpoint, so "
        "resuming re-executes it"
    )
    @add_test_categories(["pyapi"])
    def test_inferior_resumes_through_the_patch(self):
        """The inferior runs to completion through a patched site.

        The trampoline runs the instruction the branch displaced and then
        branches back to the instruction after it. Getting that second branch
        wrong sends the inferior somewhere arbitrary, which would show up here as
        a crash rather than a clean exit.

        Expected to fail today, and the reason is worth spelling out because it
        is a real gap rather than a test artifact. The condition traps on a
        `brk` compiled into the JIT-ed expression, which is a permanent
        instruction rather than a trap lldb installed, so there is no breakpoint
        site at that address. Nothing advances the pc past it, and resuming
        re-executes the same trap forever: the process cannot make progress from
        an injected stop.

        Advancing past the trap is all that is needed, because the rest of the
        JIT-ed expression returns into the trampoline, which restores the
        registers, runs the displaced instruction and branches back to user
        code. That belongs in StopInfo, alongside attributing the stop to the
        breakpoint so it reports as `breakpoint N.M` rather than
        EXC_BREAKPOINT.
        """
        process, breakpoint, location = self.arm("counter == 9")

        # Leave the patch installed: the point is that the remaining iterations
        # all run through the trampoline and back.
        process.Continue()

        self.assertState(process.GetState(), lldb.eStateExited)
        self.assertEqual(
            process.GetExitStatus(),
            0,
            "the inferior did not exit cleanly after running through the patch",
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "x86_64"]))
    @add_test_categories(["pyapi"])
    def test_condition_holds_at_the_stop(self):
        """The injected condition is true when it stops, and only then."""
        process, breakpoint, _ = self.arm("counter == 9")

        thread = process.GetSelectedThread()
        self.assertTrue(thread and thread.IsValid())

        # The stop lands inside the JIT-ed condition, so the user's frame is
        # further up, reached through the trampoline's unwind plan.
        frame = None
        for candidate in thread.frames:
            if candidate.GetFunctionName() == "main":
                frame = candidate
                break
        self.assertTrue(frame, "no main frame, so the unwind plan is wrong")

        self.assertEqual(
            frame.GetLineEntry().GetLine(),
            line_number("main.c", self.comment),
            "stopped on the wrong line",
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "x86_64"]))
    @add_test_categories(["pyapi"])
    def test_detach_does_not_leave_the_patch_behind(self):
        """Detaching takes the patch out.

        Otherwise the inferior keeps branching to a trampoline owned by a
        debugger that is gone, and executes the injected trap with nothing
        attached to catch it, which kills it.
        """
        self.build()
        target = self.dbg.CreateTarget(self.getBuildArtifact("a.out"))
        self.assertTrue(target, VALID_TARGET)

        # Stop early so there is a live, stopped process to arm and detach.
        entry = target.BreakpointCreateByName("main")
        self.assertTrue(entry, VALID_BREAKPOINT)

        # The inferior writes this once it finishes the loop. It is not this
        # process's child, so its exit status cannot be collected after
        # detaching, and both a clean exit and death by trap simply make the pid
        # go away. The marker is the only thing that distinguishes them.
        marker = self.getBuildArtifact("finished.txt")
        if os.path.exists(marker):
            os.unlink(marker)

        process = target.LaunchSimple(
            [marker], None, self.get_process_working_directory())
        self.assertTrue(process, PROCESS_IS_VALID)
        self.assertState(process.GetState(), lldb.eStateStopped)

        target.BreakpointDelete(entry.GetID())

        breakpoint = target.BreakpointCreateBySourceRegex(
            self.comment, self.source)
        self.assertEqual(breakpoint.GetNumLocations(), 1, VALID_BREAKPOINT)
        location = breakpoint.GetLocationAtIndex(0)
        # True partway through the loop, so the inferior only reaches it after
        # the detach.
        location.SetCondition("counter == 100")
        location.SetInjectCondition(True)
        self.assertTrue(location.GetInjectCondition())

        self.assertSuccess(process.Detach(), "detaching")

        deadline = 100
        while deadline and not os.path.exists(marker):
            time.sleep(0.1)
            deadline -= 1

        self.assertTrue(
            os.path.exists(marker),
            "the detached inferior never finished its loop, so it died on the "
            "injected trap instead of running past a patch that should have "
            "been removed",
        )
