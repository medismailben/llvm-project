"""A per-test ``lldb.SBDebugger`` session: setup/teardown, command running,
and target/process/thread/frame convenience accessors.

Mirrors ``lldbsuite.test.lldbtest.TestBase``'s session lifecycle (a fresh,
isolated ``SBDebugger`` per test, run synchronously) closely enough that
existing dotest-style tests can move to
:class:`lldb.testing.testcase.LLDBTestCase` with an import and base-class
change, not a rewrite.
"""

from __future__ import annotations

import re

import lldb


class DebuggerSessionMixin:
    """Mix into a ``unittest.TestCase`` (see
    :class:`lldb.testing.testcase.LLDBTestCase`, which already includes this).
    """

    def setUp(self) -> None:
        super().setUp()
        self.dbg = lldb.SBDebugger.Create()
        self.dbg.SetAsync(False)
        self.ci = self.dbg.GetCommandInterpreter()
        if not self.ci:
            raise RuntimeError("Could not get the command interpreter")
        self.res = lldb.SBCommandReturnObject()

    def tearDown(self) -> None:
        for index in range(self.dbg.GetNumTargets()):
            target = self.dbg.GetTargetAtIndex(index)
            process = target.GetProcess()
            if process.IsValid():
                process.Kill()
        lldb.SBDebugger.Destroy(self.dbg)
        super().tearDown()

    def target(self) -> lldb.SBTarget:
        """The debugger's currently selected target."""
        return self.dbg.GetSelectedTarget()

    def process(self) -> lldb.SBProcess:
        """The selected target's process."""
        return self.target().GetProcess()

    def thread(self) -> lldb.SBThread:
        """The process's selected thread."""
        return self.process().GetSelectedThread()

    def frame(self) -> lldb.SBFrame:
        """The selected thread's selected frame."""
        return self.thread().GetSelectedFrame()

    def runCmd(self, cmd: str, msg: str | None = None, check: bool = True) -> None:
        """Run ``cmd`` through the command interpreter.

        Args:
            cmd: The lldb command to run.
            msg: An optional additional failure message, used if ``check``
                fails.
            check: If ``True`` (the default), fail the test unless the
                command succeeded.
        """
        if cmd is None:
            raise ValueError("cmd must not be None")
        self.ci.HandleCommand(cmd, self.res)
        if check and not self.res.Succeeded():
            output = ""
            if self.res.GetOutput():
                output += f"\nCommand output:\n{self.res.GetOutput()}"
            if self.res.GetError():
                output += f"\nError output:\n{self.res.GetError()}"
            self.assertTrue(
                self.res.Succeeded(), (msg or f"Command '{cmd}' failed.") + output
            )

    def expect(
        self,
        string: str | lldb.SBCommandReturnObject,
        msg: str | None = None,
        patterns: list[str] | None = None,
        startstr: str | None = None,
        endstr: str | None = None,
        substrs: list[str] | None = None,
        error: bool = False,
        ordered: bool = True,
        matching: bool = True,
        exe: bool = True,
    ) -> None:
        """Run a command (or check a string) and match its output.

        Ask the command interpreter to handle ``string`` and then check its
        return status. The output is expected to start with ``startstr``,
        end with ``endstr``, contain the substrings in ``substrs``, and
        regex-match the patterns in ``patterns``. When ``matching`` and
        ``ordered`` are both true (the default), the ``substrs``/``patterns``
        entries must appear in the output in the order given.

        Args:
            string: The lldb command to run, or (if ``exe`` is ``False``) a
                literal string or ``lldb.SBCommandReturnObject`` to check
                directly without running anything.
            msg: An optional additional failure message.
            patterns: Regexes that must (or must not, if ``matching`` is
                ``False``) appear in the output, in order.
            startstr: A string the output must (or must not) start with.
            endstr: A string the output must (or must not) end with.
            substrs: Substrings that must (or must not) appear in the
                output, in order.
            error: If ``True``, ``string`` is expected to fail, and its
                error stream is checked instead of its output stream.
            ordered: If ``True`` (the default), ``substrs``/``patterns``
                must appear in the given order.
            matching: If ``False``, the checks are inverted: failure means
                a match was found where none was expected.
            exe: If ``False``, ``string`` is treated as a literal
                string/result to check rather than a command to run.
        """
        if msg and not (patterns or startstr or endstr or substrs or error):
            raise AssertionError("expect() missing a matcher argument")
        assert not isinstance(patterns, str), "patterns must be a collection of strings"
        assert not isinstance(substrs, str), "substrs must be a collection of strings"

        if exe:
            self.runCmd(string, msg=msg, check=not error)
            output = self.res.GetError() if error else self.res.GetOutput()
            if error:
                self.assertFalse(
                    self.res.Succeeded(), f"Command '{string}' is expected to fail!"
                )
        elif isinstance(string, lldb.SBCommandReturnObject):
            output = string.GetOutput()
        else:
            output = string

        expecting = "Expecting" if matching else "Not expecting"

        def found(is_match: bool) -> str:
            return "was found" if is_match else "was not found"

        matched = matching
        log_lines = [f'{"Ran command" if exe else "Checking string"}: "{string}"', ""]

        if startstr:
            matched = output.startswith(startstr)
            log_lines.append(f'{expecting} start string: "{startstr}" ({found(matched)})')
        if endstr and matched == matching:
            matched = output.endswith(endstr)
            log_lines.append(f'{expecting} end string: "{endstr}" ({found(matched)})')
        if substrs and matched == matching:
            start = 0
            for substr in substrs:
                index = output.find(substr, start)
                matched = index != -1
                start = index + len(substr) if ordered and matched else 0
                log_lines.append(f'{expecting} sub string: "{substr}" ({found(matched)})')
                if matched != matching:
                    break
        if patterns and matched == matching:
            start = 0
            for pattern in patterns:
                match = re.compile(pattern).search(output, start)
                matched = bool(match)
                start = match.end() if ordered and matched else 0
                log_lines.append(f'{expecting} regex pattern: "{pattern}" ({found(matched)})')
                if matched != matching:
                    break

        if msg is not None and matched != matching:
            log_lines.append(msg)
        if matched != matching:
            self.fail("\n".join(log_lines))
