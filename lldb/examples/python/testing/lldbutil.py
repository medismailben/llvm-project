"""``lldb.SB*``-object helpers for writing lldb scripting-extension tests.

Most functions here operate only on ``lldb.SB*`` objects and plain values —
no ``unittest.TestCase``/test-fixture argument, and no dependency on LLDB's
own checkout-only test suite (``lldbsuite``). They are safe to call from
inside a live lldb session (e.g. a ``command script import``-ed module, or a
running ``ScriptedProcess``/``ScriptedThread``), not just from a
``unittest.TestCase``.

The ``run_to_*_breakpoint`` family is the exception, matching
``lldbsuite.test.lldbutil``'s own shape: they take a ``test`` (an
:class:`lldb.testing.testcase.LLDBTestCase`, which provides ``build()``/
``getBuildArtifact()``/``dbg``/``assertTrue``/``fail``) because they create
a target from a freshly built binary and launch it — existing dotest-style
call sites (``lldbutil.run_to_source_breakpoint(self, ...)``) port with only
an import change. For running to a breakpoint on an *already-existing*
target/process (no build step), see :mod:`lldb.testing.run_to_breakpoint`.

This is a decoupled reimplementation of ``lldbsuite.test.lldbutil`` — not an
import forwarder.
"""

from __future__ import annotations

import lldb


def _enum_names(prefix: str) -> dict[int, str]:
    """Build a mapping of enum value to lowercase name for every ``lldb``
    module attribute starting with ``prefix``.

    Args:
        prefix: The enum family's common prefix, e.g. ``"eState"``.

    Returns:
        A dict mapping each enum's integer value to its name with the
        prefix stripped and lowercased, e.g. ``{lldb.eStateStopped:
        "stopped"}``.
    """
    suffix_start = len(prefix)
    return {
        getattr(lldb, attr): attr[suffix_start:].lower()
        for attr in dir(lldb)
        if attr.startswith(prefix)
    }


_STATE_NAMES = _enum_names(prefix="eState")
_STOP_REASON_NAMES = _enum_names(prefix="eStopReason")


def state_type_to_str(enum: int) -> str:
    """Return the ``lldb.StateType`` name for the given enum value.

    Args:
        enum: A value from the ``lldb.eState*`` family, e.g. ``lldb.eStateStopped``.

    Returns:
        The lowercase state name, e.g. ``"stopped"``.

    Raises:
        ValueError: If ``enum`` is not a known ``lldb.eState*`` value.
    """
    name = _STATE_NAMES.get(enum)
    if name is None:
        raise ValueError(f"Unknown StateType enum: {enum}")
    return name


def stop_reason_to_str(enum: int) -> str:
    """Return the ``lldb.StopReason`` name for the given enum value.

    Args:
        enum: A value from the ``lldb.eStopReason*`` family, e.g.
            ``lldb.eStopReasonBreakpoint``.

    Returns:
        The lowercase stop-reason name, e.g. ``"breakpoint"``.

    Raises:
        ValueError: If ``enum`` is not a known ``lldb.eStopReason*`` value.
    """
    name = _STOP_REASON_NAMES.get(enum)
    if name is None:
        raise ValueError(f"Unknown StopReason enum: {enum}")
    return name


def get_stopped_threads(process: lldb.SBProcess, reason: int) -> list[lldb.SBThread]:
    """Return every thread in ``process`` stopped for the given reason.

    Args:
        process: The process to inspect.
        reason: A value from the ``lldb.eStopReason*`` family.

    Returns:
        The (possibly empty) list of matching threads.
    """
    return [thread for thread in process if thread.GetStopReason() == reason]


def get_stopped_thread(process: lldb.SBProcess, reason: int) -> lldb.SBThread | None:
    """Return the first thread in ``process`` stopped for the given reason.

    Args:
        process: The process to inspect.
        reason: A value from the ``lldb.eStopReason*`` family.

    Returns:
        The first matching thread, or ``None`` if no thread matches.
    """
    threads = get_stopped_threads(process, reason)
    return threads[0] if threads else None


def get_threads_stopped_at_breakpoint_id(
    process: lldb.SBProcess, bpid: int
) -> list[lldb.SBThread]:
    """Return every thread in ``process`` stopped at breakpoint ID ``bpid``.

    Args:
        process: The (stopped) process to inspect.
        bpid: The breakpoint ID, i.e. ``lldb.SBBreakpoint.GetID()``.

    Returns:
        The (possibly empty) list of threads stopped at that breakpoint.
    """
    threads = []
    for thread in get_stopped_threads(process, lldb.eStopReasonBreakpoint):
        # GetStopReasonDataAtIndex returns pairs of (breakpoint ID,
        # breakpoint location ID); scan every ID in the pairs for a match.
        break_ids = [
            thread.GetStopReasonDataAtIndex(idx)
            for idx in range(0, thread.GetStopReasonDataCount(), 2)
        ]
        if bpid in break_ids:
            threads.append(thread)
    return threads


def get_threads_stopped_at_breakpoint(
    process: lldb.SBProcess, bkpt: lldb.SBBreakpoint
) -> list[lldb.SBThread]:
    """Return every thread in ``process`` stopped at breakpoint ``bkpt``.

    Args:
        process: The (stopped) process to inspect.
        bkpt: The breakpoint to check against.

    Returns:
        The (possibly empty) list of threads stopped at ``bkpt``.
    """
    return get_threads_stopped_at_breakpoint_id(process, bkpt.GetID())


def get_one_thread_stopped_at_breakpoint_id(
    process: lldb.SBProcess, bpid: int, require_exactly_one: bool = True
) -> lldb.SBThread | None:
    """Return the single thread stopped at breakpoint ID ``bpid``.

    Args:
        process: The (stopped) process to inspect.
        bpid: The breakpoint ID, i.e. ``lldb.SBBreakpoint.GetID()``.
        require_exactly_one: If ``True`` (the default), return ``None``
            when more than one thread is stopped at the breakpoint instead
            of picking one arbitrarily.

    Returns:
        The matching thread, or ``None`` if zero threads match (or more
        than one matches and ``require_exactly_one`` is ``True``).
    """
    threads = get_threads_stopped_at_breakpoint_id(process, bpid)
    if not threads:
        return None
    if require_exactly_one and len(threads) != 1:
        return None
    return threads[0]


def get_one_thread_stopped_at_breakpoint(
    process: lldb.SBProcess, bkpt: lldb.SBBreakpoint, require_exactly_one: bool = True
) -> lldb.SBThread | None:
    """Return the single thread stopped at breakpoint ``bkpt``.

    Args:
        process: The (stopped) process to inspect.
        bkpt: The breakpoint to check against.
        require_exactly_one: If ``True`` (the default), return ``None``
            when more than one thread is stopped at the breakpoint instead
            of picking one arbitrarily.

    Returns:
        The matching thread, or ``None`` if zero threads match (or more
        than one matches and ``require_exactly_one`` is ``True``).
    """
    return get_one_thread_stopped_at_breakpoint_id(
        process, bkpt.GetID(), require_exactly_one
    )


def continue_to_breakpoint(
    process: lldb.SBProcess, bkpt: lldb.SBBreakpoint
) -> list[lldb.SBThread] | None:
    """Continue ``process`` and return the threads stopped at ``bkpt``.

    Args:
        process: The process to continue.
        bkpt: The breakpoint to check for after continuing.

    Returns:
        The list of threads stopped at ``bkpt`` if the process stopped
        again, or ``None`` if it did not end up in the stopped state
        (e.g. it exited or crashed elsewhere).
    """
    process.Continue()
    if process.GetState() != lldb.eStateStopped:
        return None
    return get_threads_stopped_at_breakpoint(process, bkpt)


def _run_to_breakpoint_wait(
    test, target: lldb.SBTarget, breakpoint: lldb.SBBreakpoint,
    launch_info: lldb.SBLaunchInfo | None, only_one_thread: bool,
) -> tuple[lldb.SBTarget, lldb.SBProcess, lldb.SBThread, lldb.SBBreakpoint]:
    """Launch ``target`` and wait for ``breakpoint`` to be hit. Shared by
    the ``run_to_*_breakpoint`` family below."""
    if not launch_info:
        launch_info = target.GetLaunchInfo()

    error = lldb.SBError()
    process = target.Launch(launch_info, error)
    test.assertTrue(
        process,
        f"Could not create a valid process for {target.GetExecutable().GetFilename()}: {error.GetCString()}",
    )
    test.assertFalse(error.Fail(), f"Process launch failed: {error.GetCString()}")

    if process.GetState() != lldb.eStateStopped:
        process.Continue()
    threads = get_threads_stopped_at_breakpoint(process, breakpoint)

    num_threads = len(threads)
    if only_one_thread:
        test.assertEqual(
            num_threads, 1, f"Expected 1 thread to stop at breakpoint, {num_threads} did."
        )
    else:
        test.assertGreater(num_threads, 0, "No threads stopped at breakpoint")

    return target, process, threads[0], breakpoint


def run_to_breakpoint_make_target(test, exe_name: str = "a.out", in_cwd: bool = True) -> lldb.SBTarget:
    """Build (if needed) and create a target for ``test``.

    Args:
        test: An :class:`lldb.testing.testcase.LLDBTestCase` (or any object
            exposing ``getBuildArtifact``/``dbg``/``assertTrue``).
        exe_name: The build artifact's filename.
        in_cwd: If ``True`` (the default), resolve ``exe_name`` via
            ``test.getBuildArtifact()``; otherwise treat it as a literal path.

    Returns:
        The created ``lldb.SBTarget``.
    """
    exe = test.getBuildArtifact(exe_name) if in_cwd else exe_name
    target = test.dbg.CreateTarget(exe)
    test.assertTrue(target, f"Target: {exe_name} is not valid.")
    return target


def run_to_name_breakpoint(
    test, bkpt_name: str, launch_info: lldb.SBLaunchInfo | None = None,
    exe_name: str = "a.out", bkpt_module: str | None = None, in_cwd: bool = True,
    only_one_thread: bool = True,
) -> tuple[lldb.SBTarget, lldb.SBProcess, lldb.SBThread, lldb.SBBreakpoint]:
    """Build, launch, and run ``test`` to a breakpoint set by function name.

    Args:
        test: An :class:`lldb.testing.testcase.LLDBTestCase`.
        bkpt_name: The function name to break on.
        launch_info: Launch options; ``target.GetLaunchInfo()`` is used if
            not given.
        exe_name: The build artifact's filename.
        bkpt_module: Restrict the breakpoint to this module, if given.
        in_cwd: See :func:`run_to_breakpoint_make_target`.
        only_one_thread: If ``True`` (the default), fail unless exactly one
            thread stopped at the breakpoint.

    Returns:
        ``(target, process, thread, breakpoint)``.
    """
    target = run_to_breakpoint_make_target(test, exe_name, in_cwd)
    breakpoint = target.BreakpointCreateByName(bkpt_name, bkpt_module)
    test.assertTrue(
        breakpoint.GetNumLocations() > 0,
        f"No locations found for name breakpoint: '{bkpt_name}'.",
    )
    return _run_to_breakpoint_wait(test, target, breakpoint, launch_info, only_one_thread)


def run_to_source_breakpoint(
    test, bkpt_pattern: str, source_spec: lldb.SBFileSpec,
    launch_info: lldb.SBLaunchInfo | None = None, exe_name: str = "a.out",
    bkpt_module: str | None = None, in_cwd: bool = True, only_one_thread: bool = True,
) -> tuple[lldb.SBTarget, lldb.SBProcess, lldb.SBThread, lldb.SBBreakpoint]:
    """Build, launch, and run ``test`` to a breakpoint set by source regex.

    Args mirror :func:`run_to_name_breakpoint`, with ``bkpt_pattern``/
    ``source_spec`` replacing ``bkpt_name`` (see
    ``lldb.SBTarget.BreakpointCreateBySourceRegex``).

    Returns:
        ``(target, process, thread, breakpoint)``.
    """
    target = run_to_breakpoint_make_target(test, exe_name, in_cwd)
    breakpoint = target.BreakpointCreateBySourceRegex(bkpt_pattern, source_spec, bkpt_module)
    test.assertTrue(
        breakpoint.GetNumLocations() > 0,
        f'No locations found for source breakpoint: "{bkpt_pattern}", '
        f'file: "{source_spec.GetFilename()}", dir: "{source_spec.GetDirectory()}"',
    )
    return _run_to_breakpoint_wait(test, target, breakpoint, launch_info, only_one_thread)


def run_to_line_breakpoint(
    test, source_spec: lldb.SBFileSpec, line: int, column: int = 0,
    launch_info: lldb.SBLaunchInfo | None = None, exe_name: str = "a.out",
    in_cwd: bool = True, only_one_thread: bool = True,
) -> tuple[lldb.SBTarget, lldb.SBProcess, lldb.SBThread, lldb.SBBreakpoint]:
    """Build, launch, and run ``test`` to a breakpoint set by file/line/column.

    Args mirror :func:`run_to_name_breakpoint`, with ``source_spec``/``line``/
    ``column`` replacing ``bkpt_name``.

    Returns:
        ``(target, process, thread, breakpoint)``.
    """
    target = run_to_breakpoint_make_target(test, exe_name, in_cwd)
    breakpoint = target.BreakpointCreateByLocation(source_spec, line, column)
    test.assertTrue(
        breakpoint.GetNumLocations() > 0,
        f'No locations found for line breakpoint: "{source_spec.GetFilename()}:{line}".',
    )
    return _run_to_breakpoint_wait(test, target, breakpoint, launch_info, only_one_thread)

