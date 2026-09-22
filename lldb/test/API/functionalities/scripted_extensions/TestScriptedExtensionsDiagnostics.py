"""
Verify that exceptions raised inside scripted extension affordance methods
(or missing required abstract methods) are surfaced to the user.

For entry points with no return-channel for errors
(`ScriptedProcess::CreateInstance`, `OperatingSystemPython` ctor,
`ScriptedThreadPlan::DidPush`, `BreakpointResolverScripted` ctor,
`ScriptedStackFrameRecognizer` ctor) the diagnostic is broadcast via
`Debugger::ReportError` and asserted on a listener.

`ScriptedThread::Create` and `ScriptedFrame::Create` also return an
`llvm::Expected`, but their only callers
(`ScriptedProcess::DoUpdateThreadList` and
`ScriptedThread::LoadArtificialStackFrames`, respectively) have no
return-channel of their own, so those errors are likewise broadcast via
`Debugger::ReportError` rather than propagated further.

Entry points that already return an `llvm::Expected` / `Status` all the way
to a user-visible surface (`ScriptedFrameProvider::CreateInstance`,
`StopHookScripted::SetScriptCallback`) propagate the detailed error through
their return type; tests for those are tracked as follow-up.

The data formatter entry points (synthetic child providers and summary
providers) report differently again: a ValueObject already knows how to
render its own error, so those failures appear inline where the value or the
summary would have been, and the tests just assert on command output.
"""

import os

import lldb
import lldbsuite.test.lldbutil as lldbutil
from lldbsuite.test.decorators import expectedFailureAll
from lldbsuite.test.lldbtest import TestBase


class TestScriptedExtensionsDiagnostics(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

    def setUp(self):
        TestBase.setUp(self)
        self.broadcaster = self.dbg.GetBroadcaster()
        self.listener = lldbutil.start_listening_from(
            self.broadcaster,
            lldb.SBDebugger.eBroadcastBitWarning | lldb.SBDebugger.eBroadcastBitError,
        )
        script_path = os.path.join(
            self.getSourceDir(), "malformed_scripted_extensions.py"
        )
        self.runCmd("command script import " + script_path)

    def assert_diagnostic(self, expected_substring):
        event = lldbutil.fetch_next_event(self, self.listener, self.broadcaster)
        data = lldb.SBDebugger.GetDiagnosticFromEvent(event)
        self.assertTrue(data.IsValid(), "event has diagnostic data")
        message = data.GetValueForKey("message").GetStringValue(4096)
        self.assertIn(expected_substring, message)

    def create_target(self):
        self.build()
        target = self.dbg.CreateTarget(self.getBuildArtifact("a.out"))
        self.assertTrue(target, "valid target")
        return target

    # ------------------------------------------------------------------
    # ScriptedProcess - reports via ScriptedProcess::CreateInstance
    # ------------------------------------------------------------------

    def test_scripted_process_missing_methods(self):
        """A ScriptedProcess missing abstract methods should emit a
        diagnostic naming the missing method."""
        target = self.create_target()
        launch_info = lldb.SBLaunchInfo(None)
        launch_info.SetProcessPluginName("ScriptedProcess")
        launch_info.SetScriptedProcessClassName(
            "malformed_scripted_extensions.MissingMethodsScriptedProcess"
        )
        error = lldb.SBError()
        target.Launch(launch_info, error)
        self.assertTrue(error.Fail(), "launch should fail")
        self.assert_diagnostic("read_memory_at_address")

    # ------------------------------------------------------------------
    # ScriptedProcess::launch() - reports via the normal SBError return
    # channel (Process::DoLaunch), unlike CreateInstance's construction
    # failures above, which have no return channel and go through
    # Debugger::ReportError instead.
    # ------------------------------------------------------------------

    def test_scripted_process_launch_exception(self):
        """An explicitly raised exception from `launch()` should surface
        through the normal SBError return channel."""
        target = self.create_target()
        launch_info = lldb.SBLaunchInfo(None)
        launch_info.SetProcessPluginName("ScriptedProcess")
        launch_info.SetScriptedProcessClassName(
            "malformed_scripted_extensions.ExceptionScriptedProcess"
        )
        error = lldb.SBError()
        target.Launch(launch_info, error)
        self.assertTrue(error.Fail(), "launch should fail")
        self.assertIn("intentional exception from launch()", error.GetCString())

    def test_scripted_process_launch_runtime_error(self):
        """A natural Python runtime error (e.g. a typo'd name), as opposed
        to a deliberately raised exception, should surface just as
        readably: this is the other half of what users hit in practice --
        either an implementation function returns something that doesn't
        make sense, or (this case) they mistyped a name and Python
        complains about it at runtime."""
        target = self.create_target()
        launch_info = lldb.SBLaunchInfo(None)
        launch_info.SetProcessPluginName("ScriptedProcess")
        launch_info.SetScriptedProcessClassName(
            "malformed_scripted_extensions.TypoScriptedProcess"
        )
        error = lldb.SBError()
        target.Launch(launch_info, error)
        self.assertTrue(error.Fail(), "launch should fail")
        self.assertIn("NameError", error.GetCString())
        self.assertIn("this_name_is_never_defined", error.GetCString())

    # ------------------------------------------------------------------
    # BreakpointResolverScripted - reports via
    # BreakpointResolverScripted::CreateImplementationIfNeeded.
    # `m_error` is set but never surfaced to the user, so ReportError is
    # the only user-visible channel.
    # ------------------------------------------------------------------

    def test_scripted_breakpoint_resolver_init_failure(self):
        target = self.create_target()
        target.BreakpointCreateFromScript(
            "malformed_scripted_extensions.ExceptionInitScriptedBreakpointResolver",
            lldb.SBStructuredData(),
            lldb.SBFileSpecList(),
            lldb.SBFileSpecList(),
        )
        self.assert_diagnostic("intentional exception from __init__()")

    # ------------------------------------------------------------------
    # ScriptedThreadPlan - reports via ScriptedThreadPlan::DidPush. The
    # plan stores the error in m_error_str but never surfaces it; the
    # diagnostic is the user-visible channel.
    # ------------------------------------------------------------------

    def test_scripted_thread_plan_init_failure(self):
        target = self.create_target()
        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertTrue(process, "valid process")
        thread = process.GetSelectedThread()
        self.assertTrue(thread, "valid thread")
        thread.StepUsingScriptedThreadPlan(
            "malformed_scripted_extensions.ExceptionInitScriptedThreadPlan"
        )
        self.assert_diagnostic("intentional exception from __init__()")

    # ------------------------------------------------------------------
    # ScriptedThread - reports via ScriptedProcess::DoUpdateThreadList,
    # which propagates ScriptedThread::Create's Expected error through
    # Debugger::ReportError (DoUpdateThreadList has no return-channel back
    # to its caller).
    # ------------------------------------------------------------------

    def test_scripted_thread_missing_methods(self):
        """A scripted thread object returned from `get_threads_info()` that
        is missing a required abstract method should emit a diagnostic
        naming the missing method."""
        target = self.create_target()
        launch_info = lldb.SBLaunchInfo(None)
        launch_info.SetProcessPluginName("ScriptedProcess")
        launch_info.SetScriptedProcessClassName(
            "malformed_scripted_extensions.ThreadListScriptedProcess"
        )
        error = lldb.SBError()
        target.Launch(launch_info, error)
        self.assert_diagnostic("get_stop_reason")

    # ------------------------------------------------------------------
    # ScriptedFrame - reports via ScriptedThread::LoadArtificialStackFrames,
    # which propagates ScriptedFrame::Create's Expected error through
    # Debugger::ReportError (LoadArtificialStackFrames' return value is
    # discarded by its only caller, RefreshStateAfterStop).
    # ------------------------------------------------------------------

    def test_scripted_frame_missing_methods(self):
        """A scripted frame object returned from a thread's
        `get_stackframes()` that is missing a required abstract method
        should emit a diagnostic naming the missing method."""
        target = self.create_target()
        launch_info = lldb.SBLaunchInfo(None)
        launch_info.SetProcessPluginName("ScriptedProcess")
        launch_info.SetScriptedProcessClassName(
            "malformed_scripted_extensions.StackFrameScriptedProcess"
        )
        error = lldb.SBError()
        target.Launch(launch_info, error)
        self.assert_diagnostic("get_id")

    # ------------------------------------------------------------------
    # ScriptedStackFrameRecognizer - reports via
    # ScriptedStackFrameRecognizer's constructor, which has no
    # error-return channel back to `frame recognizer add`.
    # ------------------------------------------------------------------

    def test_scripted_stack_frame_recognizer_init_failure(self):
        target = self.create_target()
        self.runCmd(
            "frame recognizer add -l "
            "malformed_scripted_extensions.ExceptionScriptedStackFrameRecognizer "
            "-s a.out -n main"
        )
        self.assert_diagnostic("intentional exception from __init__()")

    # ------------------------------------------------------------------
    # The remaining entry point has no plugin implementation yet.
    # ------------------------------------------------------------------

    def test_operating_system_missing_methods(self):
        self.build()
        lldbutil.run_to_source_breakpoint(self, "break here", lldb.SBFileSpec("main.c"))
        os_plugin_path = os.path.join(
            self.getSourceDir(), "os_plugin_missing_methods.py"
        )
        self.runCmd(
            "settings set target.process.python-os-plugin-path " + os_plugin_path
        )
        self.runCmd("thread list")
        self.assert_diagnostic("get_thread_info")

    @expectedFailureAll(bugnumber="ScriptedPlatform has no plugin implementation yet")
    def test_scripted_platform_missing_methods(self):
        self.assert_diagnostic("list_processes")

    # ------------------------------------------------------------------
    # Data formatters - reported inline, in place of the value or the
    # summary, the way every other ValueObject error is rendered.
    #
    # Formatter registrations are global and outlive the debugger, so each of
    # these clears them again on the way out.
    # ------------------------------------------------------------------

    def run_to_breakpoint_with_formatters(self):
        self.build()
        lldbutil.run_to_source_breakpoint(self, "break here", lldb.SBFileSpec("main.c"))

        def cleanup():
            self.runCmd("type summary clear", check=False)
            self.runCmd("type synth clear", check=False)

        self.addTearDownHook(cleanup)

    def test_synth_provider_init_exception(self):
        """A provider whose `__init__` raises used to silently fall back to the
        raw children, which is indistinguishable from no formatter at all."""
        self.run_to_breakpoint_with_formatters()
        self.runCmd(
            "type synthetic add -l "
            "malformed_scripted_extensions.ExceptionInitSynthProvider Pair"
        )
        self.expect(
            "frame variable pair",
            substrs=["RuntimeError", "intentional exception from __init__()"],
        )
        # And must not present the unformatted children as if all were well.
        self.expect("frame variable pair", matching=False, substrs=["first = 11"])

    def test_synth_provider_update_exception(self):
        """`update` raising used to be logged and nothing more, leaving an
        empty aggregate that reads as a legitimately empty container."""
        self.run_to_breakpoint_with_formatters()
        self.runCmd(
            "type synthetic add -l "
            "malformed_scripted_extensions.ExceptionUpdateSynthProvider Pair"
        )
        self.expect(
            "frame variable pair",
            substrs=["update", "intentional exception from update()"],
        )

    def test_synth_provider_without_update(self):
        """Not implementing the optional `update` is not a failure, and must
        stay distinguishable from one that raised."""
        self.run_to_breakpoint_with_formatters()
        self.runCmd(
            "type synthetic add -l "
            "malformed_scripted_extensions.NoUpdateSynthProvider Pair"
        )
        self.expect("frame variable pair", substrs=["first = 11"])
        self.expect(
            "frame variable pair", matching=False, substrs=["error", "Traceback"]
        )

    def test_synth_provider_num_children_exception(self):
        self.run_to_breakpoint_with_formatters()
        self.runCmd(
            "type synthetic add -l "
            "malformed_scripted_extensions.ExceptionNumChildrenSynthProvider Pair"
        )
        self.expect(
            "frame variable pair",
            substrs=["intentional exception from num_children()"],
        )

    def test_synth_provider_one_child_exception(self):
        """A child that can't be produced must be reported in place, not
        skipped. ValueObjectPrinter drops a null child without comment, so this
        used to show two children out of the three num_children promised."""
        self.run_to_breakpoint_with_formatters()
        self.runCmd(
            "type synthetic add -l "
            "malformed_scripted_extensions.ExceptionOneChildSynthProvider Pair"
        )
        self.expect(
            "frame variable pair",
            substrs=["intentional exception from get_child_at_index"],
        )
        # The children that *can* be produced are still shown.
        self.expect("frame variable pair", substrs=["first = 11"])

    def test_synth_provider_num_children_error_not_cached(self):
        """A failed child count must not be cached as a successful zero.

        `SBValue.GetNumChildren()` (no max) is the path that used to do that,
        after which the error was gone for the rest of the stop and
        `frame variable` printed an empty aggregate with no diagnostic."""
        self.run_to_breakpoint_with_formatters()
        self.runCmd(
            "type synthetic add -l "
            "malformed_scripted_extensions.ExceptionNumChildrenSynthProvider Pair"
        )
        pair = self.frame().FindVariable("pair")
        self.assertTrue(pair.IsValid(), "valid SBValue")
        self.assertEqual(pair.GetNumChildren(), 0)
        self.expect(
            "frame variable pair",
            substrs=["intentional exception from num_children()"],
        )

    def test_synth_provider_child_index_absent(self):
        """A name the provider doesn't know is an ordinary "no such member",
        and must not take the debugger down with it: consuming that error used
        to abort in cantFail."""
        self.run_to_breakpoint_with_formatters()
        self.runCmd(
            "type synthetic add -l "
            "malformed_scripted_extensions.ChildIndexSynthProvider Pair"
        )
        self.expect("frame variable pair.first", substrs=["11"])
        self.expect(
            "frame variable pair.nope",
            error=True,
            substrs=["is not a member of"],
        )

    def test_synth_provider_child_index_exception(self):
        """`get_child_index` raising is a broken provider, not an absent
        member, and has to say so rather than report "no member named"."""
        self.run_to_breakpoint_with_formatters()
        self.runCmd(
            "type synthetic add -l "
            "malformed_scripted_extensions.ExceptionChildIndexSynthProvider Pair"
        )
        self.expect(
            "frame variable pair.nope",
            error=True,
            substrs=["intentional exception from get_child_index()"],
        )

    def test_summary_function_exception(self):
        """An exception from a `-F` summary function used to produce no summary
        and no error, with only a bare traceback on stderr."""
        self.run_to_breakpoint_with_formatters()
        self.runCmd(
            "type summary add -F malformed_scripted_extensions.exception_summary Pair"
        )
        self.expect(
            "frame variable pair",
            substrs=["RuntimeError", "intentional exception from summary()"],
        )

    def test_summary_function_returning_none(self):
        """Returning None is a value here, not a failure."""
        self.run_to_breakpoint_with_formatters()
        self.runCmd(
            "type summary add -F malformed_scripted_extensions.none_summary Pair"
        )
        self.expect("frame variable pair", substrs=["None"])

    def test_summary_function_not_found(self):
        self.run_to_breakpoint_with_formatters()
        self.runCmd(
            "type summary add -F malformed_scripted_extensions.no_such_summary Pair"
        )
        self.expect("frame variable pair", substrs=["could not find summary function"])

    def test_class_summary_get_summary_exception(self):
        """The class-based path already reported this; pin it down so the two
        summary flavors don't drift apart again.

        There's no `type summary add` flag for a class-based provider yet, so
        register it through the API."""
        self.run_to_breakpoint_with_formatters()
        summary = lldb.SBTypeSummary.CreateWithClassName(
            "malformed_scripted_extensions.ExceptionGetSummaryProvider"
        )
        self.assertTrue(summary.IsValid(), "valid SBTypeSummary")
        self.dbg.GetDefaultCategory().AddTypeSummary(
            lldb.SBTypeNameSpecifier("Pair"), summary
        )
        self.expect(
            "frame variable pair",
            substrs=["intentional exception from get_summary()"],
        )
