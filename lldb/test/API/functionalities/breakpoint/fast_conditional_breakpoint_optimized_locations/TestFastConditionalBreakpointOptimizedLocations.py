"""
Test the DWARF location forms optimized code produces.

The unoptimized cases are next door in fast_conditional_breakpoint_locations.
These need their own directory because a test class gets one build, and what
makes these interesting is the optimization level: a local stops being a frame
offset and becomes a bare register, DW_OP_regN, or gets folded to a constant,
DW_OP_consts followed by DW_OP_stack_value, or gets split across two registers,
DW_OP_piece. None of the three is an address, so none could be handed to the
condition at all before locations were lowered generally.

Each test is differential. The same condition is armed twice, once injected and
once left to the debugger, and the two are required to stop in the same place
with the same value. That is what catches a location lowered to the wrong
address: a wrong address does not usually fail to compile, it just answers the
condition on the wrong bytes.
"""

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class FastConditionalBreakpointOptimizedLocationsTestCase(TestBase):
    # One variant is enough: every test here builds with its own flags and needs
    # debug info either way, so the dsym and dwarf matrix would only run the same
    # thing twice.
    NO_DEBUG_INFO_TESTCASE = True

    def setUp(self):
        TestBase.setUp(self)
        self.source = lldb.SBFileSpec("main.c")
        self.comment = "break here"

    def run_to_condition(self, condition, inject, comment=None):
        """Arm \a condition at the marker and run to it.

        Returns the (process, injected) pair, where `injected` says whether the
        condition was still injected once the process was running. Injection
        clears its own flag when it falls back, so that is the only honest
        answer to whether the inferior evaluated the condition or the debugger
        did.

        The caller has already built: a test method gets one build, so the two
        halves of a differential test share the binary, which is what makes them
        comparable.
        """
        target = self.dbg.CreateTarget(self.getBuildArtifact("a.out"))
        self.assertTrue(target, VALID_TARGET)

        breakpoint = target.BreakpointCreateBySourceRegex(
            comment or self.comment, self.source)
        self.assertGreater(breakpoint.GetNumLocations(), 0, VALID_BREAKPOINT)

        # Optimized code can put the marker at more than one address, and the
        # condition has to hold at whichever one is reached, so every location
        # gets it.
        for index in range(breakpoint.GetNumLocations()):
            location = breakpoint.GetLocationAtIndex(index)
            location.SetCondition(condition)
            if inject:
                location.SetInjectCondition(True)

        process = target.LaunchSimple(
            None, None, self.get_process_working_directory())
        self.assertTrue(process, PROCESS_IS_VALID)

        injected = inject and any(
            breakpoint.GetLocationAtIndex(index).GetInjectCondition()
            for index in range(breakpoint.GetNumLocations())
        )
        return process, injected

    def assert_matches_the_debugger(self, condition, variable,
                                    comment=None, function="main"):
        """The injected condition has to stop where the debugger's would.

        Compared on the value of the variable the condition tests, not on the
        pc or the line. An injected condition traps inside the JIT-ed checker,
        so the user's frame is synthesized from the trampoline's unwind plan and
        symbolicated as a return address, which puts it one instruction before
        the site and often on the previous line. That is a separate defect in
        the frame the stop reports, not in the location this is testing.

        The value is the assertion that matters anyway: a location lowered to
        the wrong address does not usually fail to compile, it just answers the
        condition on the wrong bytes, and each condition here is only true on
        one iteration.
        """
        self.build()

        injected_process, injected = self.run_to_condition(
            condition, inject=True, comment=comment)
        self.assertState(injected_process.GetState(), lldb.eStateStopped,
                         "the injected condition never held")
        self.assertTrue(
            injected,
            "'%s' was not injected, so this test compared the debugger with "
            "itself. Either a location form regressed or the compiler emitted "
            "one that is not supported yet; the reason is in the jit log "
            "channel." % condition,
        )
        injected_value = self.read_variable(injected_process, variable, function)

        self.dbg.DeleteTarget(injected_process.GetTarget())

        plain_process, _ = self.run_to_condition(
            condition, inject=False, comment=comment)
        self.assertState(plain_process.GetState(), lldb.eStateStopped,
                         "the debugger's own condition never held")

        self.assertEqual(
            injected_value,
            self.read_variable(plain_process, variable, function),
            "the injected condition stopped with '%s' holding a different "
            "value than the debugger's did" % variable,
        )

    def read_variable(self, process, variable, function="main"):
        """\a variable as seen from \a function, wherever its frame turned out to be.

        An injected condition traps inside the JIT-ed condition checker, so the
        user's function is further up, reached through the trampoline's unwind
        plan.
        """
        thread = process.GetSelectedThread()
        for index in range(thread.GetNumFrames()):
            frame = thread.GetFrameAtIndex(index)
            if frame.GetFunctionName() != function:
                continue
            # Not FindVariable(), which only looks at what is in scope as a
            # local, and these conditions read a global too.
            value = frame.GetValueForVariablePath(variable)
            self.assertTrue(value.IsValid(),
                            "'%s' is not readable from main" % variable)
            return value.GetValueAsSigned()
        self.fail("no frame for '%s' in:\n%s" %
                  (function, "\n".join(str(frame) for frame in thread)))

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_register_resident_local(self):
        """A local the optimizer moved into a register, which is DW_OP_regN.

        It works by handing over the address of the slot the trampoline saved
        that register into, so nothing is copied and no scratch is needed.
        """
        self.assert_matches_the_debugger("local_count == 9", "local_count")

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_global_from_optimized_code(self):
        """A global read where the loop may have been unrolled."""
        self.assert_matches_the_debugger("global_total == 6", "global_total")

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_composite_local(self):
        """A struct split across two registers, which is DW_OP_piece.

        Checked by whether the process stops at all rather than by reading the
        variable back, because the site is not in the outermost function and the
        frame for the function it *is* in does not survive the unwind out of the
        trampoline. That is a separate defect, and it would make this test fail
        for a reason that has nothing to do with locations.

        Stopping and not stopping are both asserted, and that is what makes this
        discriminate. Both conditions read both halves of the struct: a piece
        taken from the wrong register, or written at the wrong offset in the
        variable, leaves the true condition false and nothing stops. A condition
        that is not really being evaluated stops on the false one.
        """
        marker = "split across registers"
        self.build()

        stopped, injected = self.run_to_condition(
            "pair.lo == 9 && pair.hi == 909", inject=True, comment=marker)
        self.assertTrue(injected, "the composite condition was not injected")
        self.assertState(
            stopped.GetState(), lldb.eStateStopped,
            "a condition that holds on one iteration did not stop, so the "
            "struct was assembled from the wrong bytes",
        )
        self.dbg.DeleteTarget(stopped.GetTarget())

        ran, injected = self.run_to_condition(
            "pair.lo == 9 && pair.hi == 908", inject=True, comment=marker)
        self.assertTrue(injected, "the composite condition was not injected")
        self.assertState(
            ran.GetState(), lldb.eStateExited,
            "a condition that never holds stopped anyway",
        )
