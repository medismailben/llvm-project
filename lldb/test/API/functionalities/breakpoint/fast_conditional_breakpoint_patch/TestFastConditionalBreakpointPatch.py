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
from lldbsuite.test import lldbutil


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

        # The stop reason itself is asserted by
        # test_stop_is_attributed_to_the_breakpoint, so that a regression there
        # does not fail every test in this file.
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
        #
        # A branch is asserted rather than the same branch. Re-arming allocates
        # a fresh trampoline, and where that lands depends on what is mapped at
        # the time, so the displacement is free to differ from the first one.
        # Requiring the identical word would be testing that the allocator is
        # deterministic, which it is not and does not promise to be.
        breakpoint.SetEnabled(True)
        rearmed = self.read_word(process, site)
        self.assertNotEqual(
            rearmed,
            displaced,
            "re-enabling the breakpoint did not restore a branch",
        )
        self.assertEqual(
            rearmed & 0xFC000000,
            0x14000000,
            "re-enabling the breakpoint left something other than a branch at "
            "the site",
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "x86_64"]))
    @add_test_categories(["pyapi"])
    def test_inferior_resumes_through_the_patch(self):
        """The inferior runs to completion through a patched site.

        Two things have to hold. The trampoline has to run the instruction the
        branch displaced and then branch back to the instruction after it;
        getting that second branch wrong sends the inferior somewhere arbitrary,
        which shows up here as a crash rather than a clean exit. And the pc has
        to be advanced past the trap the condition stopped on, which is an
        instruction compiled into the JIT-ed expression rather than a trap lldb
        installed, so nothing lifts it: leaving the pc on it means resuming
        re-executes the same trap forever.
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
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_call_site_can_be_patched(self):
        """A breakpoint whose displaced instruction is a call still works.

        A branch means something different from the trampoline than it did where
        it was, so copying it there verbatim would send the inferior to the wrong
        place. It has to be rewritten to keep referring to what it did, and this
        is the case that proves the rewrite rather than asserting it: if the
        relocated call is wrong the inferior never finishes its loop.
        """
        self.build()
        target = self.dbg.CreateTarget(self.getBuildArtifact("a.out"))
        self.assertTrue(target, VALID_TARGET)

        # Find a call rather than assuming where one is: which line holds it and
        # what the compiler emits around it are both free to change.
        main = target.FindFunctions("main").GetContextAtIndex(0).GetFunction()
        self.assertTrue(main, "no main to disassemble")

        call = None
        for instruction in main.GetInstructions(target):
            if instruction.DoesBranch() and instruction.GetMnemonic(target) == "bl":
                call = instruction
                break
        self.assertTrue(call, "no call instruction in main to patch")

        # By SBAddress rather than by a raw address: an address on a target
        # with no process carries the section it belongs to, and a breakpoint
        # created from the bare integer stays unresolved once the module loads,
        # so no site is ever built and nothing is patched.
        breakpoint = target.BreakpointCreateBySBAddress(call.GetAddress())
        self.assertEqual(breakpoint.GetNumLocations(), 1, VALID_BREAKPOINT)

        location = breakpoint.GetLocationAtIndex(0)
        location.SetCondition("counter == 9")
        location.SetInjectCondition(True)

        marker = self.getBuildArtifact("finished.txt")
        if os.path.exists(marker):
            os.unlink(marker)

        process = target.LaunchSimple(
            [marker], None, self.get_process_working_directory())
        self.assertTrue(process, PROCESS_IS_VALID)
        self.assertState(process.GetState(), lldb.eStateStopped)

        self.assertTrue(
            location.GetInjectCondition(),
            "the call site was refused, so the relocation path was not "
            "exercised",
        )

        # The remaining iterations all run through the trampoline, where the
        # relocated call has to reach the same function it did before.
        process.Continue()
        self.assertState(process.GetState(), lldb.eStateExited)
        self.assertEqual(process.GetExitStatus(), 0)
        self.assertTrue(
            os.path.exists(marker),
            "the inferior did not finish, so the relocated call went somewhere "
            "other than where the original did",
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "x86_64"]))
    @add_test_categories(["pyapi"])
    def test_stop_is_attributed_to_the_breakpoint(self):
        """An injected stop reports as the breakpoint that caused it.

        The condition traps inside the JIT-ed expression, at an address no
        breakpoint site is registered under, so the stop has to be matched back
        to its site by trap address. Without that it arrives as a bare
        EXC_BREAKPOINT: no breakpoint stop reason, no hit count, and nothing to
        advance the pc past the trap.
        """
        process, breakpoint, _ = self.arm("counter == 9")

        thread = lldbutil.get_stopped_thread(
            process, lldb.eStopReasonBreakpoint)
        self.assertTrue(
            thread,
            "the stop was not attributed to the breakpoint, so it arrived as a "
            "plain exception",
        )
        self.assertEqual(
            breakpoint.GetHitCount(),
            1,
            "the breakpoint's hit count was not maintained",
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "x86_64"]))
    @add_test_categories(["pyapi"])
    def test_backtrace_walks_through_the_patched_function(self):
        """The patched function is in the backtrace, at the site, and unwinds out.

        Three things, because each failed separately at some point and only the
        first is visible without looking past it:

        The function has to be there at all. The trampoline is branched to rather
        than called, so an unwind rule that reads the saved lr as a return address
        skips it: with a leaf function that produces a backtrace with the user's
        code missing entirely.

        Its pc has to be the site, not near it. The plan reports a constant, so
        this is exact rather than off by an instruction.

        And the frame above it has to be reachable, which is what needs the
        patched function's callee-saved registers described. An unoptimized
        function's CFA is fp relative, so without fp the walk stops at the patched
        function and everything that called it is lost.
        """
        process, _, location = self.arm("counter == 9")
        thread = process.GetSelectedThread()

        frames = [thread.GetFrameAtIndex(i)
                  for i in range(thread.GetNumFrames())]
        names = [f.GetFunctionName() for f in frames]
        self.assertIn(
            "main", names,
            "the patched function is not in the backtrace:\n%s" % "\n".join(
                str(f) for f in frames))

        main_index = names.index("main")
        main_frame = frames[main_index]

        self.assertEqual(
            main_frame.GetPC(),
            location.GetLoadAddress(),
            "the patched function's frame reports a pc other than the site",
        )

        self.assertGreater(
            len(frames), main_index + 1,
            "nothing above the patched function, so the unwind stopped there "
            "instead of walking out of it",
        )

        # The variable the condition tested has to be readable from that frame,
        # which is the practical reason any of this matters.
        counter = main_frame.GetValueForVariablePath("counter")
        self.assertTrue(counter.IsValid(), "counter is not readable from main")
        # The site is the start of the line, so the increment in it has not run.
        self.assertEqual(counter.GetValueAsSigned(), 9,
                         "counter reads wrong from the patched frame")

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
