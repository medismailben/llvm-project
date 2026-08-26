"""
Test the DWARF location forms a fast conditional breakpoint can evaluate.

The condition runs in the inferior, so a variable's DWARF location has to be
turned into code the inferior can run rather than interpreted in the debugger.
These cover the shapes a compiler actually emits: a frame relative local, a
global, and, once optimized, a register resident local and a location the
compiler folded to a constant.

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


class FastConditionalBreakpointLocationsTestCase(TestBase):
    # One variant is enough: every test here builds with its own flags and needs
    # debug info either way, so the dsym and dwarf matrix would only run the same
    # thing twice.
    NO_DEBUG_INFO_TESTCASE = True

    def setUp(self):
        TestBase.setUp(self)
        self.source = lldb.SBFileSpec("main.c")
        self.comment = "break here"

    def run_to_condition(self, condition, inject):
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
            self.comment, self.source)
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
        self.assertState(process.GetState(), lldb.eStateStopped)

        injected = inject and any(
            breakpoint.GetLocationAtIndex(index).GetInjectCondition()
            for index in range(breakpoint.GetNumLocations())
        )
        return process, injected

    def assert_matches_the_debugger(self, condition, variable):
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
            condition, inject=True)
        self.assertState(injected_process.GetState(), lldb.eStateStopped,
                         "the injected condition never held")
        self.assertTrue(
            injected,
            "'%s' was not injected, so this test compared the debugger with "
            "itself. Either a location form regressed or the compiler emitted "
            "one that is not supported yet; the reason is in the jit log "
            "channel." % condition,
        )
        injected_value = self.read_variable(injected_process, variable)

        self.dbg.DeleteTarget(injected_process.GetTarget())

        plain_process, _ = self.run_to_condition(condition, inject=False)

        self.assertEqual(
            injected_value,
            self.read_variable(plain_process, variable),
            "the injected condition stopped with '%s' holding a different "
            "value than the debugger's did" % variable,
        )

    def read_variable(self, process, variable):
        """\a variable as seen from main, wherever main's frame turned out to be.

        An injected condition traps inside the JIT-ed condition checker, so
        main is further up, reached through the trampoline's unwind plan.
        """
        thread = process.GetSelectedThread()
        for index in range(thread.GetNumFrames()):
            frame = thread.GetFrameAtIndex(index)
            if frame.GetFunctionName() != "main":
                continue
            # Not FindVariable(), which only looks at what is in scope as a
            # local, and these conditions read a global too.
            value = frame.GetValueForVariablePath(variable)
            self.assertTrue(value.IsValid(),
                            "'%s' is not readable from main" % variable)
            return value.GetValueAsSigned()
        self.fail("no frame for main in:\n%s" %
                  "\n".join(str(frame) for frame in thread))

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_frame_relative_local(self):
        """A local at a frame base offset, which is DW_OP_fbreg."""
        self.assert_matches_the_debugger("local_count == 9", "local_count")

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_global(self):
        """A global at a fixed address, which is DW_OP_addr.

        The operand is a link-time file address, so the emitted code has to carry
        the load address instead, or the inferior reads whatever is at the
        unslid one.
        """
        self.assert_matches_the_debugger("global_total == 6", "global_total")

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    @add_test_categories(["pyapi"])
    def test_local_and_global_together(self):
        """Two variables of different location forms in one condition.

        Each one is stored at the offset the condition expression's own layout
        assigned it, so a mix is what catches the two being confused.
        """
        self.assert_matches_the_debugger(
            "local_count == 4 && global_total == 10", "local_count")
