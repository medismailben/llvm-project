"""
Test the CPython scripted frame provider example against the interpreter lldb
itself is running, which is the one Python guaranteed to be present.
"""

import os
import sys

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *


class TestCPythonFrameProvider(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

    EXAMPLE = os.path.join(
        os.environ.get("LLDB_SRC", ""),
        "examples",
        "python",
        "cpython_frame_provider.py",
    )

    def setUp(self):
        TestBase.setUp(self)
        if not os.path.exists(self.EXAMPLE):
            self.skipTest("cannot locate cpython_frame_provider.py")
        self.workload = os.path.join(self.getSourceDir(), "workload.py")

    def run_to_python_stack(self):
        """Stop the inferior interpreter with workload.py's frames on the stack.

        `getppid` is the marker: CPython does not call it during start-up, so
        the first hit is workload.py's own call and the Python stack is
        exactly outer -> middle -> leaf.
        """
        target = self.dbg.CreateTarget(sys.executable)
        self.assertTrue(target, "created a target for the running interpreter")
        target.BreakpointCreateByName("getppid")

        process = target.LaunchSimple([self.workload], None, self.getBuildDir())
        self.assertTrue(process, PROCESS_IS_VALID)

        eval_symbol = "_PyEval_EvalFrameDefault"
        while process.GetState() == lldb.eStateStopped:
            thread = process.GetSelectedThread()
            if thread.GetStopReason() == lldb.eStopReasonBreakpoint and any(
                frame.GetFunctionName() == eval_symbol for frame in thread
            ):
                return target, process, thread
            process.Continue()
        self.fail("never stopped with the interpreter on the stack")

    def register_provider(self, target, process):
        """Register the provider, skipping if it cannot read this interpreter.

        Resolving has to happen first: once the provider is live, iterating a
        thread yields its own output rather than the interpreter's C frames,
        so the check would be looking at the wrong stack.
        """
        sys.path.insert(0, os.path.dirname(self.EXAMPLE))
        import cpython_frame_provider

        try:
            interpreter = cpython_frame_provider.resolve_interpreter(target, process)
        except cpython_frame_provider.ProviderError as error:
            self.skipTest("unsupported interpreter: %s" % error)

        self.runCmd("command script import " + self.EXAMPLE)
        self.runCmd(
            "target frame-provider register -C "
            "cpython_frame_provider.CPythonFrameProvider"
        )
        return interpreter

    def python_frames(self, thread):
        """The provider's synthetic frames, innermost first."""
        return [
            frame
            for frame in thread
            if frame.IsSynthetic()
            and frame.GetLineEntry().GetFileSpec().GetFilename() == "workload.py"
        ]

    def test_backtrace_shows_python_frames(self):
        """The eval frames are replaced by the Python functions they run."""
        target, process, thread = self.run_to_python_stack()
        interpreter = self.register_provider(target, process)

        frames = self.python_frames(thread)
        self.assertEqual(
            [frame.GetFunctionName() for frame in frames],
            ["leaf", "middle", "outer", "<module>"],
            "backtrace reads as the Python call stack",
        )

        # The line numbers come from the interpreter's own line table, so
        # each frame landing on the call it is blocked in is what proves the
        # decoder matches this CPython.
        source = open(self.workload).read().splitlines()

        def line_of(text):
            return next(i + 1 for i, l in enumerate(source) if l.strip() == text)

        expected = {
            "leaf": line_of("return os.getppid()"),
            "middle": line_of("return leaf(count, inner_label)"),
            "outer": line_of("return middle(3)"),
            "<module>": line_of("outer()"),
        }
        for frame in frames:
            self.assertEqual(
                frame.GetLineEntry().GetLine(),
                expected[frame.GetFunctionName()],
                "%s stopped on the call it is executing" % frame.GetFunctionName(),
            )

    def test_frame_variable_shows_python_locals(self):
        """`frame variable` decodes the Python frame's fast locals."""
        target, process, thread = self.run_to_python_stack()
        self.register_provider(target, process)

        leaf = self.python_frames(thread)[0]
        self.assertEqual(leaf.GetFunctionName(), "leaf")

        values = {
            value.GetName(): value
            for value in leaf.GetVariables(True, True, True, True)
        }
        self.assertIn("count", values, "arguments are visible")
        self.assertIn("numbers", values, "locals are visible")

        # Scalars are projected onto the matching native type; everything else
        # onto the text its repr would start with.
        self.assertEqual(values["count"].GetValueAsSigned(), 3)
        self.assertEqual(values["ratio"].GetValue(), "1.5")
        self.assertEqual(values["flag"].GetValue(), "true")
        self.assertIn("from-middle", values["label"].GetSummary())
        self.assertIn("None", values["nothing"].GetSummary())
        self.assertIn("list object at", values["numbers"].GetSummary())

    def test_declines_instead_of_guessing(self):
        """A layout that cannot decode the target must not be used."""
        target, process, thread = self.run_to_python_stack()
        interpreter = self.register_provider(target, process)

        sys.path.insert(0, os.path.dirname(self.EXAMPLE))
        import cpython_frame_provider

        # Shifting one offset is enough to make every decode fail, and the
        # self-check is what stands between that and a backtrace of nonsense.
        interpreter.layout.code_filename += 8
        self.assertFalse(
            interpreter.self_check(), "a corrupted layout fails validation"
        )
