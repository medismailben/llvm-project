#!/usr/bin/env python3
"""Lit-facing entry point for lldb/test/API, drop-in replacement for dotest.py.

lit's `LLDBTest` test format (lldb/test/API/lldbtest.py) spawns exactly this
calling convention per test file:

    [python_executable] + dotest_cmd + [testPath, "-p", testFile]

and parses two things out of the subprocess's *stderr*: a line matching
`^Ran (\\d+) tests? in ` and a line matching `^(?:OK|FAILED)(?: \\((.*)\\))?\\r?$`
(plus an optional `^Skip breakdown \\(unsupported=(\\d+), skipped=(\\d+)\\)\\r?$`
line), together with the process exit code. That text is produced entirely
by stdlib `unittest.TextTestRunner`'s own summary formatting -- it has
nothing to do with dotest.py specifically.

This script preserves that exact calling convention (`lit.cfg.py` points
`dotest_cmd[0]` here instead of at dotest.py) and dispatches per file:

- If the target test file imports `lldb.testing` (i.e. it's been ported off
  lldbsuite.test), run it directly via stdlib `unittest` against the
  already-built `lldb` module -- no lldbsuite, no build()/decorator/category
  matrix.
- Otherwise, re-exec the real dotest.py with an identical argv. Unported
  files are completely unaffected: same process, same behavior, same output.
"""

from __future__ import annotations

import importlib
import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REAL_DOTEST_PY = os.path.join(_HERE, "dotest.py")


def _find_flag_value(argv: list[str], flag: str) -> str | None:
    """Return the value following the last occurrence of ``flag`` in ``argv``."""
    for index in range(len(argv) - 1, -1, -1):
        if argv[index] == flag and index + 1 < len(argv):
            return argv[index + 1]
    return None


def _find_test_path_and_file(argv: list[str]) -> tuple[str, str] | None:
    """Recover ``(testPath, testFile)`` from lit's ``[testPath, "-p", testFile]`` triple."""
    for index in range(len(argv) - 1, -1, -1):
        if argv[index] == "-p" and index > 0 and index + 1 < len(argv):
            return argv[index - 1], argv[index + 1]
    return None


def _is_ported(test_path: str, test_file: str) -> bool:
    """Sniff whether ``test_file`` has been ported to lldb.testing."""
    try:
        with open(os.path.join(test_path, test_file), encoding="utf-8") as f:
            content = f.read()
    except OSError:
        return False
    return "lldb.testing" in content


def _delegate_to_legacy_dotest(argv: list[str]) -> None:
    """Re-exec the real dotest.py with an identical argv; never returns."""
    os.execv(sys.executable, [sys.executable, _REAL_DOTEST_PY] + argv)


def _run_with_lldb_testing(argv: list[str], test_path: str, test_file: str) -> int:
    """Run ``test_file`` via stdlib unittest, printing lit's expected summary
    to stderr, and return a process exit code (0 on success)."""
    lldb_python_dir = _find_flag_value(argv, "--lldb-python-dir")
    if lldb_python_dir and lldb_python_dir not in sys.path:
        sys.path.insert(0, lldb_python_dir)

    build_dir = _find_flag_value(argv, "--build-dir")
    if build_dir:
        os.environ["LLDB_TESTING_BUILD_DIR"] = build_dir

    sys.path.insert(0, test_path)
    module_name, _ = os.path.splitext(test_file)
    module = importlib.import_module(module_name)

    suite = unittest.defaultTestLoader.loadTestsFromModule(module)
    result = unittest.TextTestRunner(stream=sys.stderr, verbosity=1).run(suite)

    if result.skipped:
        # lldb.testing has no "unsupported vs. genuinely skipped" distinction
        # (see lldbsuite.test.skip_reason.UnsupportedReason); report every
        # skip as a real skip.
        print(
            f"Skip breakdown (unsupported=0, skipped={len(result.skipped)})",
            file=sys.stderr,
        )
    return 0 if result.wasSuccessful() else 1


def main(argv: list[str]) -> int:
    located = _find_test_path_and_file(argv)
    if located is None:
        # Malformed invocation (shouldn't happen under lit) -- fall back to
        # the legacy engine, which has its own error handling for this.
        _delegate_to_legacy_dotest(argv)
        return 1  # unreachable, execv replaces the process

    test_path, test_file = located
    if not _is_ported(test_path, test_file):
        _delegate_to_legacy_dotest(argv)
        return 1  # unreachable

    return _run_with_lldb_testing(argv, test_path, test_file)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
