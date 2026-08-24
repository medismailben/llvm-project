"""
Test Fast Conditional Breakpoints.
"""

from __future__ import print_function

import os
import time
import re
import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class FastConditionalBreakpoitsTestCase(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    def setUp(self):
        # Call super's setUp().
        TestBase.setUp(self)
        self.file = lldb.SBFileSpec("main.c")
        self.comment = "break here for local_count"
        # Two forms on purpose. The command interpreter needs the expression
        # quoted so it survives argument splitting; SBBreakpointLocation takes the
        # expression itself, and passing the quoted form there makes the condition
        # a string literal, which is always true and captures no variables.
        self.condition = "local_count == 9"
        self.quoted_condition = '"' + self.condition + '"'
        self.extra_options = "-c " + self.quoted_condition + " -I"
        self.binary = "a.out"

    @skipIfWindows
    def test_fast_conditional_breakpoint_flag_interpreter(self):
        """Enable fast conditional breakpoints with 'breakpoint modify -c <expr> id -I'."""
        self.build()
        self.enable_fast_conditional_breakpoint(use_interpreter=True)

    @skipIfWindows
    @add_test_categories(["pyapi"])
    def test_fast_conditional_breakpoint_flag_api(self):
        """Exercise fast conditional breakpoints with SB API"""
        self.build()
        self.enable_fast_conditional_breakpoint(use_interpreter=False,
                                                set_thread_index=True)

    @skipIfWindows
    @add_test_categories(["pyapi"])
    def test_fast_conditional_breakpoint(self):
        """Exercice injected breakpoint conditions"""
        self.build()
        self.inject_fast_conditional_breakpoint()

    @skipIfWindows
    @add_test_categories(["pyapi"])
    def test_invalid_fast_conditional_breakpoint(self):
        """Exercice invalid injected breakpoint conditions"""
        self.build()
        self.inject_invalid_fast_conditional_breakpoint()

    def enable_fast_conditional_breakpoint(self, use_interpreter,
                                           set_thread_index=False):
        exe = self.getBuildArtifact(self.binary)
        self.target = self.dbg.CreateTarget(exe)
        self.assertTrue(self.target, VALID_TARGET)

        if use_interpreter:
            lldbutil.run_break_set_by_source_regexp(
                self, self.comment, self.extra_options
            )

            self.runCmd("breakpoint modify " + self.quoted_condition + " 1")

            self.expect("breakpoint list -f", substrs=["(FAST)"])
        else:
            # Now create a breakpoint on main.c by source regex'.
            breakpoint = self.target.BreakpointCreateBySourceRegex(
                self.comment, self.file
            )
            self.assertTrue(
                breakpoint and breakpoint.GetNumLocations() == 1,
                VALID_BREAKPOINT)

            # We didn't associate a thread index with the breakpoint, so it should
            # be invalid.
            self.assertTrue(
                breakpoint.GetThreadIndex() == lldb.UINT32_MAX,
                "the thread index should be invalid",
            )
            # The thread name should be invalid, too.
            self.assertTrue(
                breakpoint.GetThreadName() is None,
                "the thread name should be invalid")

            # Only where the breakpoint is never armed. A condition evaluated
            # in the process runs on whichever thread reaches the site and
            # cannot honour a thread filter, so asking for both is refused and
            # the condition falls back to the debugger.
            if set_thread_index:
                breakpoint.SetThreadIndex(1)
                self.assertTrue(
                    breakpoint.GetThreadIndex() == 1,
                    "the thread index has been set correctly",
                )

            # Get the breakpoint location from breakpoint after we verified that,
            # indeed, it has one location.
            location = breakpoint.GetLocationAtIndex(0)
            self.assertTrue(
                location and location.IsEnabled(), VALID_BREAKPOINT_LOCATION
            )

            # Set the condition on the breakpoint.
            location.SetCondition(self.condition)
            self.expect(
                location.GetCondition(),
                exe=False,
                startstr=self.condition)

            # Set condition on the breakpoint to be injected in-process.
            location.SetInjectCondition(True)
            self.assertTrue(
                location.GetInjectCondition(),
                VALID_BREAKPOINT_LOCATION)

        return self.target.GetBreakpointAtIndex(0)

    def inject_fast_conditional_breakpoint(self):
        # now launch the process, and do not stop at entry point.
        breakpoint = self.enable_fast_conditional_breakpoint(
            use_interpreter=False)
        process = self.target.LaunchSimple(
            None, None, self.get_process_working_directory()
        )
        self.assertTrue(process, PROCESS_IS_VALID)

        # frame #0 should be on self.line and the break condition should hold.
        from lldbsuite.test.lldbutil import get_stopped_thread

        thread = get_stopped_thread(process, lldb.eStopReasonBreakpoint)
        self.assertTrue(
            thread and thread.IsValid(),
            "there should be a thread stopped due to breakpoint condition",
        )

        # The stop lands inside the JIT-ed condition, which was reached from the
        # trampoline, which was branched to from the user's code. Name the frames
        # in the failure message: an equality assertion that only says "False is
        # not true" gives nothing to go on when the stack is not what we expect.
        names = [
            "#%u %s %s at 0x%x"
            % (
                f.GetFrameID(),
                f.GetModule().GetFileSpec().GetFilename(),
                f.GetFunctionName(),
                f.GetPC(),
            )
            for f in thread.frames
        ]

        frame0 = thread.GetFrameAtIndex(0)
        self.assertTrue(frame0 and frame0.IsValid())
        self.assertEqual(frame0.GetFunctionName(), "$__lldb_expr(void*)", names)

        frame1 = thread.GetFrameAtIndex(1)
        self.assertTrue(frame1 and frame1.IsValid())
        self.assertEqual(
            frame1.GetFunctionName(),
            "$__lldb_jitted_conditional_bp_trampoline",
            names,
        )

        frame2 = thread.GetFrameAtIndex(2)
        self.assertTrue(frame2 and frame2.IsValid())
        self.assertEqual(frame2.GetFunctionName(), "main", names)

        self.assertEqual(breakpoint.GetHitCount(), 1)

        line = line_number(self.file.GetFilename(), self.comment)
        self.assertEqual(frame2.GetLineEntry().GetLine(), line)

        # TODO: Check that the variable is actually equal to "9"
        # Currently, the assertion fails because the SBAPI doesn't
        # report the right value for "local_count"
        #var = frame2.FindVariable("local_count")
        #self.assertEqual(var.GetValue(), "9")

    def inject_invalid_fast_conditional_breakpoint(self):
        # now create a breakpoint on main.c by source regex'.
        exe = self.getBuildArtifact(self.binary)
        self.target = self.dbg.CreateTarget(exe)
        self.assertTrue(self.target, VALID_TARGET)
        breakpoint = self.target.BreakpointCreateBySourceRegex(
            self.comment, self.file)
        self.assertTrue(
            breakpoint and breakpoint.GetNumLocations() == 1, VALID_BREAKPOINT
        )

        # set the condition on the breakpoint.
        breakpoint.SetCondition("no_such_variable == not_this_one_either")
        self.expect(
            breakpoint.GetCondition(),
            exe=False,
            startstr="no_such_variable == not_this_one_either",
        )
        # get the breakpoint location from breakpoint after we verified that,
        # indeed, it has one location.
        location = breakpoint.GetLocationAtIndex(0)
        self.assertTrue(
            location and location.IsEnabled(),
            VALID_BREAKPOINT_LOCATION)

        # set condition on the breakpoint to be injected.
        location.SetInjectCondition(True)
        self.assertTrue(
            location.GetInjectCondition(),
            VALID_BREAKPOINT_LOCATION)

        # now launch the process, and do not stop at entry point.
        process = self.target.LaunchSimple(
            None, None, self.get_process_working_directory()
        )
        self.assertTrue(process, PROCESS_IS_VALID)

        # frame #0 should be on self.line1 and the break condition should hold.
        from lldbsuite.test.lldbutil import get_stopped_thread

        # FCB is disabled because the condition is not valid.
        self.assertFalse(
            location.GetInjectCondition(),
            VALID_BREAKPOINT_LOCATION)
        # FCB falls back to regular conditional breakpoint that get hit once.
        self.assertTrue(breakpoint.GetHitCount() == 1)
