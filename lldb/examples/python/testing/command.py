"""Testing helpers for lldb command objects (``LldbCommandBase`` subclasses).

Generalized from cl4_lldb's ``cmd_unittest.py`` (``MockExecutionContext``,
``UnitTestLldbAsserts``, ``CommandLldbTest``, ``LldbCommandBase``,
``add_test_parser_args``). Apple/radar-specific behavior (``addRadarRouting``,
the ``shortDescription`` override) is intentionally not part of this module —
a client that wants it subclasses :class:`CommandTestCase` itself and adds
it; ordinary subclassing is the pluggability mechanism here, no hook is
built into the library.
"""

from __future__ import annotations

import argparse
import io
import shlex
import sys
import traceback
import unittest
from contextlib import redirect_stderr, redirect_stdout

import lldb


class MockExecutionContext(lldb.SBExecutionContext):
    """A duck-typed ``SBExecutionContext`` built from a target, process,
    thread, or frame.

    Lets a test call a command object's ``__call__`` directly without going
    through lldb's real command dispatch. If the target has no process yet
    (e.g. a freshly loaded Mach-O with no launch), ``process``/``thread``/
    ``frame`` fall back to invalid placeholder objects instead of raising.
    """

    def __init__(
        self, arg: lldb.SBTarget | lldb.SBProcess | lldb.SBThread | lldb.SBFrame
    ) -> None:
        """Build a mock execution context around ``arg``.

        Args:
            arg: The target/process/thread/frame to derive the context from.

        Raises:
            TypeError: If ``arg`` isn't one of the supported types.
        """
        if isinstance(arg, lldb.SBTarget):
            self._target = arg
        elif isinstance(arg, lldb.SBProcess):
            self._target = arg.target
        elif isinstance(arg, lldb.SBThread):
            self._target = arg.GetProcess().target
        elif isinstance(arg, lldb.SBFrame):
            self._target = arg.GetThread().GetProcess().target
        else:
            raise TypeError("MockExecutionContext() passed unsupported type")

        self.debugger = self._target.GetDebugger()
        self._process = self._target.process
        try:
            self._thread = self._target.process.thread
        except Exception:
            self._thread = lldb.SBThread()
        try:
            self._frame = self._thread.frame[0]
        except Exception:
            self._frame = lldb.SBFrame()

    @property
    def target(self) -> lldb.SBTarget:
        """The target this context was built from."""
        return self._target

    @property
    def process(self) -> lldb.SBProcess:
        """The target's process, or an invalid ``SBProcess`` if none."""
        return self._process

    @property
    def thread(self) -> lldb.SBThread:
        """The process's selected thread, or an invalid ``SBThread`` if none."""
        return self._thread

    @property
    def frame(self) -> lldb.SBFrame:
        """The thread's frame 0, or an invalid ``SBFrame`` if none."""
        return self._frame

    def GetFrame(self) -> lldb.SBFrame:
        return self._frame

    def GetProcess(self) -> lldb.SBProcess:
        return self._process

    def GetTarget(self) -> lldb.SBTarget:
        return self._target

    def GetThread(self) -> lldb.SBThread:
        return self._thread


class CommandAssertsMixin:
    """Assertions for ``lldb.SBCommandReturnObject`` results."""

    def commandResultStr(self, result: lldb.SBCommandReturnObject) -> str:
        """Render ``result``'s error and output streams for a failure message.

        Args:
            result: The command result to render.

        Returns:
            A human-readable dump of ``result``'s error (if any) and output.
        """
        prefix = "" if result.Succeeded() else f'\n{"+" * 80}\n{result.GetError()}'
        return f'{prefix}{"+" * 80}\n{result.GetOutput()}'

    def assertCommandReturn(
        self, result: lldb.SBCommandReturnObject, msg: str | None = None
    ) -> None:
        """Fail unless ``result.Succeeded()``.

        Args:
            result: The command result to check.
            msg: An optional additional failure message.
        """
        if not result.Succeeded():
            self.fail((msg or "") + self.commandResultStr(result))

    def assertCommandFail(
        self, result: lldb.SBCommandReturnObject, msg: str | None = None
    ) -> None:
        """Fail if ``result.Succeeded()`` (use to test error paths).

        Args:
            result: The command result to check.
            msg: An optional additional failure message.
        """
        if result.Succeeded():
            self.fail((msg or "") + self.commandResultStr(result))


class CommandTestCase(unittest.TestCase, CommandAssertsMixin):
    """Base ``unittest.TestCase`` for testing lldb command objects.

    Generalized from cl4_lldb's ``CommandLldbTest``. Register the command
    object(s) under test with :meth:`addLldbCommand` in ``setUpClass``, then
    call them with :meth:`callLldbClass` (direct call, bypassing lldb's
    dispatch), :meth:`cmdClass` (call a plain method on the instance), or
    :meth:`runCmd`/:meth:`runCmdEx` (through lldb's real command interpreter).
    """

    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        cls.cmd = {}
        cls.debugger = lldb.debugger
        cls.target = cls.debugger.GetSelectedTarget()
        cls.ci = cls.debugger.GetCommandInterpreter()

    def _valid_cmd_name(self, cmd_name: str | None = None) -> str:
        """Resolve ``cmd_name`` to a registered command, defaulting to the
        first one registered if ``cmd_name`` is ``None``."""
        if cmd_name:
            assert cmd_name in self.cmd, (
                "test must call addLldbCommand() for this command name"
            )
        else:
            assert self.cmd, "test must call addLldbCommand() before use"
            cmd_name = next(iter(self.cmd))
        return cmd_name

    @classmethod
    def addLldbCommand(cls, cmd_name: str, cmd) -> None:
        """Register a command object instance under ``cmd_name`` for testing.

        Works like the lldb built-in ``command script add --class
        {module}.{Class} {cmd_name}``, but keeps the instance in-process so
        it can be called directly.

        Args:
            cmd_name: The name to register the command under.
            cmd: The command object instance (any ``LldbCommandBase``-shaped
                object exposing ``__call__(debugger, command, exe_ctx,
                result)``).
        """
        cls.cmd[cmd_name] = cmd

    def cmdClass(self, cmd_name: str | None = None):
        """Return the registered command object instance.

        Args:
            cmd_name: Which registered command to return; defaults to the
                first one registered.
        """
        return self.cmd[self._valid_cmd_name(cmd_name)]

    def callLldbClass(
        self,
        cmd_str: str,
        result: lldb.SBCommandReturnObject | None = None,
        cmd_name: str | None = None,
    ) -> lldb.SBCommandReturnObject:
        """Call the registered command object's ``__call__`` directly.

        Bypasses lldb's real command dispatch, using a
        :class:`MockExecutionContext` built from ``self.target``.

        Args:
            cmd_str: The command's argument string (not including the
                command name itself).
            result: The result object to populate; a fresh one is created
                if not given.
            cmd_name: Which registered command to call; defaults to the
                first one registered.

        Returns:
            The populated ``lldb.SBCommandReturnObject``.
        """
        if result is None:
            result = lldb.SBCommandReturnObject()
        cmd_name = self._valid_cmd_name(cmd_name)
        result.Clear()
        result.SetStatus(lldb.eReturnStatusSuccessFinishResult)
        exe_ctx = MockExecutionContext(self.target)
        stdout_capture = io.StringIO()
        stderr_capture = io.StringIO()
        old_stdout, old_stderr = sys.stdout, sys.stderr
        sys.stdout, sys.stderr = stdout_capture, stderr_capture
        try:
            self.cmd[cmd_name](self.debugger, cmd_str, exe_ctx, result)
        except SystemExit:
            result.SetStatus(lldb.eReturnStatusFailed)
        finally:
            sys.stdout, sys.stderr = old_stdout, old_stderr
            if stdout_capture.getvalue():
                result.AppendMessage(stdout_capture.getvalue())
            if stderr_capture.getvalue():
                result.AppendMessage(stderr_capture.getvalue())
        return result

    def runCmd(
        self, arg_str: str = "", result: lldb.SBCommandReturnObject | None = None
    ) -> lldb.SBCommandReturnObject:
        """Run the default registered command through lldb's real dispatch.

        Args:
            arg_str: The command's argument string.
            result: The result object to populate; a fresh one is created
                if not given.

        Returns:
            The populated ``lldb.SBCommandReturnObject``.
        """
        if result is None:
            result = lldb.SBCommandReturnObject()
        self.runCmdEx(None, arg_str, result)
        return result

    def cmdName(self) -> str:
        """Return the default registered command's name."""
        return self._valid_cmd_name(None)

    def runCmdEx(
        self,
        cmd_name: str | None,
        arg_str: str = "",
        result: lldb.SBCommandReturnObject | None = None,
    ) -> lldb.SBCommandReturnObject:
        """Run a registered command through lldb's real dispatch.

        Args:
            cmd_name: Which registered command to run; ``None`` for the
                first one registered.
            arg_str: The command's argument string.
            result: The result object to populate; a fresh one is created
                if not given.

        Returns:
            The populated ``lldb.SBCommandReturnObject``.
        """
        if result is None:
            result = lldb.SBCommandReturnObject()
        cmd_name = self._valid_cmd_name(cmd_name)
        self.ci.HandleCommand(f"{cmd_name} {arg_str}", result)
        return result


def add_test_parser_args(parser: argparse.ArgumentParser) -> None:
    """Add the standard test-command arguments to ``parser``.

    Adds ``tests`` (positional, ``nargs="*"``), ``-d``/``--discover``,
    ``-f``/``--file``, ``-j``/``--json``, and ``-x``/``--xml``. Shared by
    :class:`lldb.testing.runner.UnitTestCommandObject` and
    :class:`lldb.testing.discovery.TestDiscoveryRunnerCommand`.

    Args:
        parser: The parser to add arguments to.
    """
    parser.add_argument(
        "tests",
        metavar="TEST_NAME",
        type=str,
        nargs="*",
        help="optional subset of tests to run (can be repeated)",
    )
    parser.add_argument(
        "-d", "--discover", action="store_true", help="output info on tests"
    )
    parser.add_argument("-f", "--file", metavar="FILE", type=str, help="output to a file")
    parser.add_argument(
        "-j", "--json", action="store_true", help="output as serialized JSON"
    )
    parser.add_argument(
        "-x",
        "--xml",
        metavar="FILE",
        type=str,
        help="output as XML to the given FILE (requires xmlrunner)",
    )


class LldbCommandBase:
    """Base class for lldb command objects.

    Provides the standard ``__call__`` dispatch (argparse wrapping,
    exception-to-``result.SetError`` handling) and ``get_short_help``/
    ``get_long_help``. Subclasses must set ``self.description`` and
    ``self._parser`` in ``__init__``, and implement ``main()``.
    """

    def __call__(
        self,
        debugger: lldb.SBDebugger,
        command: str,
        exe_ctx: lldb.SBExecutionContext,
        result: lldb.SBCommandReturnObject,
    ) -> None:
        """Parse ``command`` and dispatch to :meth:`main`, catching errors.

        Args:
            debugger: The debugger the command was invoked on.
            command: The raw argument string typed after the command name.
            exe_ctx: The execution context to run against.
            result: The result object to populate.
        """
        try:
            output = io.StringIO()
            with redirect_stdout(output), redirect_stderr(output):
                args = self._parser.parse_args(shlex.split(command))
        except SystemExit:
            result.SetError(output.getvalue())
            return
        self.result = result
        try:
            self.main(debugger, exe_ctx, result, args)
        except RuntimeError as error:
            result.SetError(str(error))
        except SystemExit as error:
            result.SetError(f"SystemExit as: {error}")
        except Exception:
            result.SetError(traceback.format_exc())

    def get_short_help(self) -> str:
        """Return the one-line command description."""
        return self.description

    def get_long_help(self) -> str:
        """Return the full ``--help`` text."""
        return self._parser.format_help()
