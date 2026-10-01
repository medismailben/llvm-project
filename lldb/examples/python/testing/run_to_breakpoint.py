"""Helpers for driving a target/process to a breakpoint in a test.

Unlike ``lldbsuite.test.lldbutil``'s ``run_to_*_breakpoint`` family, these
functions are centered on an *already-existing* target or process (e.g. one
already attached via a ``ScriptedProcess``, or already connected to a live
or mocked gdb-remote stub) rather than on building a fresh target from a
compiled binary and launching it. :func:`create_target_and_run_to_breakpoint`
is the one explicit opt-in for that "build a target from a binary path"
model, for clients that do have a compiled binary to launch fresh.

Every function accepts a ``fail`` callback (defaulting to raising
``AssertionError``) instead of hard-depending on a ``unittest.TestCase``, so
they work standalone outside of any test framework — e.g. as a sanity check
inside a ``ScriptedProcess.__init__``. A test author who wants failures
routed through normal test reporting passes ``fail=self.fail``.
"""

from __future__ import annotations

from typing import Callable

import lldb

from lldb.testing.lldbutil import get_threads_stopped_at_breakpoint

FailFn = Callable[[str], None]


def _raise_assertion_error(message: str) -> None:
    """Default ``fail`` callback: raise ``AssertionError(message)``."""
    raise AssertionError(message)


def _create_breakpoint(
    target: lldb.SBTarget,
    *,
    name: str | None,
    source_pattern: str | None,
    source_spec: lldb.SBFileSpec | None,
    file_line: tuple[lldb.SBFileSpec, int, int] | None,
    module: str | None,
    fail: FailFn,
) -> lldb.SBBreakpoint:
    """Create exactly one of a name/source-regex/file-line breakpoint."""
    selected = [
        value
        for value in (name, source_pattern, file_line)
        if value is not None
    ]
    if len(selected) != 1:
        fail(
            "Specify exactly one of name=, source_pattern=, or file_line= "
            f"(got {len(selected)})"
        )

    if name is not None:
        return target.BreakpointCreateByName(name, module)
    if source_pattern is not None:
        if source_spec is None:
            fail("source_pattern= requires source_spec=")
        return target.BreakpointCreateBySourceRegex(
            source_pattern, source_spec, module
        )
    spec, line, column = file_line
    return target.BreakpointCreateByLocation(spec, line, column)


def _wait_for_breakpoint_hit(
    process: lldb.SBProcess,
    bkpt: lldb.SBBreakpoint,
    *,
    only_one_thread: bool,
    fail: FailFn,
) -> tuple[lldb.SBProcess, lldb.SBThread, lldb.SBBreakpoint]:
    """Continue ``process`` (if not already stopped) until ``bkpt`` is hit."""
    threads = get_threads_stopped_at_breakpoint(process, bkpt)
    if not threads:
        if process.GetState() != lldb.eStateStopped:
            process.Continue()
        threads = get_threads_stopped_at_breakpoint(process, bkpt)

    num_threads = len(threads)
    if only_one_thread and num_threads != 1:
        fail(f"Expected exactly 1 thread to stop at breakpoint, {num_threads} did.")
    elif num_threads == 0:
        fail("No threads stopped at breakpoint.")

    return process, threads[0], bkpt


def run_to_breakpoint(
    target_or_process: lldb.SBTarget | lldb.SBProcess,
    *,
    name: str | None = None,
    source_pattern: str | None = None,
    source_spec: lldb.SBFileSpec | None = None,
    file_line: tuple[lldb.SBFileSpec, int, int] | None = None,
    module: str | None = None,
    only_one_thread: bool = True,
    fail: FailFn = _raise_assertion_error,
) -> tuple[lldb.SBProcess, lldb.SBThread, lldb.SBBreakpoint]:
    """Set a breakpoint on an existing target and run/continue to it.

    Never calls ``target.Launch()`` and never reads a build artifact — this
    is the live-process / ``ScriptedProcess`` / already-attached path.
    Exactly one of ``name``, ``source_pattern`` (with ``source_spec``), or
    ``file_line`` must be given.

    Args:
        target_or_process: The target (if the process hasn't been launched
            yet by the caller) or the already-running process to use.
        name: Set a breakpoint by function name.
        source_pattern: Set a breakpoint by source regex; requires
            ``source_spec``.
        source_spec: The source file to search when using ``source_pattern``.
        file_line: ``(file_spec, line, column)`` to set a breakpoint by
            file/line/column. ``column`` may be ``0``.
        module: Restrict the breakpoint to this module name, if given.
        only_one_thread: If ``True`` (the default), fail unless exactly one
            thread stopped at the breakpoint.
        fail: Called with an error message on failure; defaults to raising
            ``AssertionError``.

    Returns:
        ``(process, thread, breakpoint)`` for the thread that hit the
        breakpoint.
    """
    if isinstance(target_or_process, lldb.SBProcess):
        process = target_or_process
        target = process.GetTarget()
    else:
        target = target_or_process
        process = target.GetProcess()

    bkpt = _create_breakpoint(
        target,
        name=name,
        source_pattern=source_pattern,
        source_spec=source_spec,
        file_line=file_line,
        module=module,
        fail=fail,
    )
    if bkpt.GetNumLocations() == 0:
        fail(f"No locations found for breakpoint (name={name!r}, source_pattern={source_pattern!r})")

    return _wait_for_breakpoint_hit(
        process, bkpt, only_one_thread=only_one_thread, fail=fail
    )


def run_to_name_breakpoint(
    target_or_process: lldb.SBTarget | lldb.SBProcess,
    bkpt_name: str,
    *,
    module: str | None = None,
    only_one_thread: bool = True,
    fail: FailFn = _raise_assertion_error,
) -> tuple[lldb.SBProcess, lldb.SBThread, lldb.SBBreakpoint]:
    """Run/continue an existing target/process to a breakpoint set by name.

    See :func:`run_to_breakpoint` for the shared parameters and return value.
    """
    return run_to_breakpoint(
        target_or_process,
        name=bkpt_name,
        module=module,
        only_one_thread=only_one_thread,
        fail=fail,
    )


def run_to_source_breakpoint(
    target_or_process: lldb.SBTarget | lldb.SBProcess,
    bkpt_pattern: str,
    source_spec: lldb.SBFileSpec,
    *,
    module: str | None = None,
    only_one_thread: bool = True,
    fail: FailFn = _raise_assertion_error,
) -> tuple[lldb.SBProcess, lldb.SBThread, lldb.SBBreakpoint]:
    """Run/continue an existing target/process to a source-regex breakpoint.

    See :func:`run_to_breakpoint` for the shared parameters and return value.
    """
    return run_to_breakpoint(
        target_or_process,
        source_pattern=bkpt_pattern,
        source_spec=source_spec,
        module=module,
        only_one_thread=only_one_thread,
        fail=fail,
    )


def run_to_line_breakpoint(
    target_or_process: lldb.SBTarget | lldb.SBProcess,
    source_spec: lldb.SBFileSpec,
    line: int,
    column: int = 0,
    *,
    only_one_thread: bool = True,
    fail: FailFn = _raise_assertion_error,
) -> tuple[lldb.SBProcess, lldb.SBThread, lldb.SBBreakpoint]:
    """Run/continue an existing target/process to a file/line breakpoint.

    See :func:`run_to_breakpoint` for the shared parameters and return value.
    """
    return run_to_breakpoint(
        target_or_process,
        file_line=(source_spec, line, column),
        only_one_thread=only_one_thread,
        fail=fail,
    )


def create_target_and_run_to_breakpoint(
    debugger: lldb.SBDebugger,
    exe_path: str,
    *,
    name: str | None = None,
    source_pattern: str | None = None,
    source_spec: lldb.SBFileSpec | None = None,
    launch_info: lldb.SBLaunchInfo | None = None,
    only_one_thread: bool = True,
    fail: FailFn = _raise_assertion_error,
) -> tuple[lldb.SBTarget, lldb.SBProcess, lldb.SBThread, lldb.SBBreakpoint]:
    """Create a fresh target from a binary, launch it, and run to a breakpoint.

    Explicit opt-in variant matching the "build a target from a binary"
    model: ``debugger.CreateTarget(exe_path)``, set the breakpoint, then
    ``target.Launch(...)``. ``exe_path`` must already be a valid, already-
    built binary path — ``lldb.testing`` has no build-step concept of its
    own.

    Args:
        debugger: The debugger to create the target on.
        exe_path: Path to an already-built executable.
        name: Set a breakpoint by function name.
        source_pattern: Set a breakpoint by source regex; requires
            ``source_spec``.
        source_spec: The source file to search when using ``source_pattern``.
        launch_info: Launch options; a default ``SBLaunchInfo`` is used if
            not given.
        only_one_thread: If ``True`` (the default), fail unless exactly one
            thread stopped at the breakpoint.
        fail: Called with an error message on failure; defaults to raising
            ``AssertionError``.

    Returns:
        ``(target, process, thread, breakpoint)`` for the thread that hit
        the breakpoint.
    """
    target = debugger.CreateTarget(exe_path)
    if not target.IsValid():
        fail(f"Could not create target for: {exe_path}")

    bkpt = _create_breakpoint(
        target,
        name=name,
        source_pattern=source_pattern,
        source_spec=source_spec,
        file_line=None,
        module=None,
        fail=fail,
    )
    if bkpt.GetNumLocations() == 0:
        fail(f"No locations found for breakpoint (name={name!r}, source_pattern={source_pattern!r})")

    error = lldb.SBError()
    process = target.Launch(launch_info or lldb.SBLaunchInfo(None), error)
    if error.Fail():
        fail(f"Process launch failed for {exe_path}: {error.GetCString()}")

    _, thread, _ = _wait_for_breakpoint_hit(
        process, bkpt, only_one_thread=only_one_thread, fail=fail
    )
    return target, process, thread, bkpt
