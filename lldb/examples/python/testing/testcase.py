"""The "Base" test-case kind: for testing data formatters, scripted processes,
scripted threads, scripted frame providers, and — via
:class:`lldb.testing.session.DebuggerSessionMixin`/
:class:`lldb.testing.build.BuildMixin` — ported ``lldbsuite.test.lldbtest``
tests, with a matching session/build-step contract so existing call sites
(``self.build()``, ``self.runCmd()``, ``self.expect()``, ``self.frame()``,
...) port with an import and base-class change.

Sibling of :class:`lldb.testing.command.CommandTestCase`, not its parent —
a command-test author isn't forced to pull in ``ValueCheck``/``expect_expr``
machinery, and a formatter-test author isn't forced to pull in
``MockExecutionContext``/argparse command-dispatch machinery.
"""

from __future__ import annotations

import unittest

from lldb.testing.asserts import ExtraAssertsMixin, LLDBAssertsMixin
from lldb.testing.build import BuildMixin
from lldb.testing.expect import ExpectMixin
from lldb.testing.session import DebuggerSessionMixin


class LLDBTestCase(
    unittest.TestCase,
    LLDBAssertsMixin,
    ExtraAssertsMixin,
    DebuggerSessionMixin,
    BuildMixin,
    ExpectMixin,
):
    """Base ``unittest.TestCase`` for lldb scripting-extension tests.

    ``setUp``/``tearDown`` (from :class:`DebuggerSessionMixin`) create and
    tear down an isolated ``lldb.SBDebugger`` per test, matching
    ``TestBase``'s isolation model. Provides ``assertSuccess``/
    ``assertFailure``/``assertState``/``assertStopReason``
    (:class:`LLDBAssertsMixin`), ``assertKeysInDict``/``assertJson``/
    ``assertUuid`` (:class:`ExtraAssertsMixin`), ``target()``/``process()``/
    ``thread()``/``frame()``/``runCmd()``/``expect()``
    (:class:`DebuggerSessionMixin`), ``build()``/``getBuildArtifact()``/
    ``getSourceDir()`` (:class:`BuildMixin`), and ``expect_expr()``/
    ``expect_var_path()`` (:class:`ExpectMixin`). Combine with
    :func:`lldb.testing.run_to_breakpoint.run_to_breakpoint` and
    :class:`lldb.testing.value_check.ValueCheck` to test formatters,
    ``ScriptedProcess``/``ScriptedThread`` implementations, and scripted
    frame providers — no LLVM checkout or Makefile required.
    """
