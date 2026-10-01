"""A pexpect-driven, out-of-process test base for driving the ``lldb`` CLI
as an external subprocess.

Generalizes ``lldbsuite.test.lldbtest.py``'s ad hoc pexpect-driven-CLI
pattern (``sendline``/``expect_exact``/``expect(pexpect.EOF)``) into a
reusable class, and documents/wraps the bootstrap pattern cl4's own
(now-deleted) ``scripted_process_test.py`` used by hand: locating ``lldb``'s
Python path via ``lldb -P``/``--python-path`` before ``import lldb``, for
harnesses (e.g. CI) that can't attach to a live dSYM-loaded lldb session.

Use this when you need to test end-to-end CLI behavior (banners,
interactive prompts, Ctrl-C handling) that in-process ``SBDebugger`` testing
can't reach.
"""

from __future__ import annotations

import subprocess
import sys
import unittest

from lldb.testing.asserts import ExtraAssertsMixin, LLDBAssertsMixin

DEFAULT_PROMPT = "(lldb) "


def ensure_lldb_importable(lldb_executable: str = "lldb") -> None:
    """Make ``import lldb`` succeed even outside a running lldb session.

    If ``lldb`` is already importable (e.g. this code is running inside a
    live lldb session that ``command script import``-ed it), this is a
    no-op. Otherwise, runs ``<lldb_executable> -P``/``--python-path`` to
    find the installed ``lldb`` package's directory and appends it to
    ``sys.path``.

    Args:
        lldb_executable: The ``lldb`` binary to query for its Python path.

    Raises:
        RuntimeError: If ``lldb`` is still not importable afterward.
    """
    try:
        import lldb  # noqa: F401

        return
    except ImportError:
        pass

    python_path = subprocess.check_output([lldb_executable, "-P"]).decode().strip()
    if python_path and python_path not in sys.path:
        sys.path.append(python_path)

    try:
        import lldb  # noqa: F401
    except ImportError as error:
        raise RuntimeError(
            f"Could not import lldb after adding '{python_path}' to sys.path"
        ) from error


class LLDBDriverProcess:
    """Wraps a ``pexpect.spawn`` session driving the ``lldb`` command-line tool."""

    def __init__(
        self,
        lldb_executable: str = "lldb",
        args: list[str] | None = None,
        timeout: float = 30.0,
        prompt: str = DEFAULT_PROMPT,
    ) -> None:
        """Spawn ``lldb_executable`` under pexpect.

        Args:
            lldb_executable: The ``lldb`` binary to spawn.
            args: Extra command-line arguments to pass.
            timeout: Default timeout, in seconds, for ``expect`` calls.
            prompt: The interactive prompt to wait for after each command.
        """
        import pexpect

        self.prompt = prompt
        self._child = pexpect.spawn(
            lldb_executable, args or [], timeout=timeout, encoding="utf-8"
        )
        self._child.expect_exact(self.prompt)

    def send_command(self, command: str) -> str:
        """Send ``command`` and return the output produced before the next prompt.

        Args:
            command: The lldb command line to send.

        Returns:
            Everything printed between sending the command and the next
            prompt, with the echoed command line stripped.
        """
        self._child.sendline(command)
        self._child.expect_exact(self.prompt)
        output = self._child.before or ""
        # The pty echoes the command itself back as the first line.
        _, _, rest = output.partition("\n")
        return rest

    def close(self) -> None:
        """Ask lldb to quit and close the underlying pexpect session."""
        import pexpect

        if not self._child.isalive():
            self._child.close()
            return
        try:
            self._child.sendline("settings set interpreter.prompt-on-quit false")
            self._child.sendline("quit")
            self._child.expect(pexpect.EOF)
        except (ValueError, pexpect.ExceptionPexpect):
            pass  # already terminated
        finally:
            self._child.close()


class PExpectTestCase(unittest.TestCase, LLDBAssertsMixin, ExtraAssertsMixin):
    """Base ``unittest.TestCase`` for driving the ``lldb`` CLI out-of-process.

    ``setUp`` spawns an :class:`LLDBDriverProcess` as ``self.driver``; use
    ``self.driver.send_command(...)`` to interact with it. Override
    :attr:`lldb_executable`/:attr:`lldb_args` to customize what gets spawned.
    """

    lldb_executable: str = "lldb"
    lldb_args: list[str] = ["--no-lldbinit"]

    def setUp(self) -> None:
        super().setUp()
        ensure_lldb_importable(self.lldb_executable)
        self.driver = LLDBDriverProcess(self.lldb_executable, self.lldb_args)

    def tearDown(self) -> None:
        self.driver.close()
        super().tearDown()
