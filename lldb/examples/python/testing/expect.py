"""``expect_expr``/``expect_var_path`` helpers for checking evaluated values.

Signatures match ``lldbsuite.test.lldbtest.TestBase.expect_expr``/
``expect_var_path`` exactly (``expr``/``var_path`` first, ``frame`` an
optional keyword) so existing dotest-style call sites port with only an
import and base-class change. ``frame`` defaults to ``self.frame()`` (see
:class:`lldb.testing.session.DebuggerSessionMixin`) when not given
explicitly, e.g. to check a synthetic frame that isn't the selected one.
"""

from __future__ import annotations

import lldb

from lldb.testing.value_check import ValueCheck


class ExpectMixin:
    """Mix into a ``unittest.TestCase`` alongside
    :class:`lldb.testing.asserts.LLDBAssertsMixin` and
    :class:`lldb.testing.session.DebuggerSessionMixin` (or use
    :class:`lldb.testing.testcase.LLDBTestCase`, which already includes both).
    """

    def expect_expr(
        self,
        expr: str,
        *,
        result_summary: str | None = None,
        result_value: str | None = None,
        result_type: str | None = None,
        result_children: list[ValueCheck] | None = None,
        options: lldb.SBExpressionOptions | None = None,
        frame: lldb.SBFrame | None = None,
    ) -> lldb.SBValue:
        """Evaluate ``expr`` in ``frame`` and check the result.

        Args:
            expr: The expression to evaluate.
            result_summary: The expected summary of the result.
            result_value: The expected value of the result.
            result_type: The expected display type name of the result.
            result_children: The expected children of the result.
            options: Expression-evaluation options; a default
                ``SBExpressionOptions`` (fix-its disabled, breakpoints
                ignored) is used if not given.
            frame: The frame to evaluate the expression in; defaults to
                ``self.frame()``.

        Returns:
            The evaluated ``lldb.SBValue``.
        """
        self.assertTrue(
            expr.strip() == expr,
            f"Expression contains trailing/leading whitespace: '{expr}'",
        )

        if frame is None:
            frame = self.frame()

        if options is None:
            options = lldb.SBExpressionOptions()
            options.SetAutoApplyFixIts(False)
            options.SetIgnoreBreakpoints(True)

        options.SetLanguage(frame.GuessLanguage())
        eval_result = frame.EvaluateExpression(expr, options)

        ValueCheck(
            type=result_type,
            value=result_value,
            summary=result_summary,
            children=result_children,
        ).check_value(self, eval_result)
        return eval_result

    def expect_var_path(
        self,
        var_path: str,
        *,
        summary: str | None = None,
        value: str | None = None,
        type: str | None = None,
        children: list[ValueCheck] | None = None,
        frame: lldb.SBFrame | None = None,
    ) -> lldb.SBValue:
        """Look up ``var_path`` in ``frame`` and check the result.

        Args:
            var_path: The variable path, e.g. ``"foo->bar[2]"``. See
                ``lldb.SBFrame.GetValueForVariablePath``.
            summary: The expected summary of the result.
            value: The expected value of the result.
            type: The expected display type name of the result.
            children: The expected children of the result.
            frame: The frame to resolve the variable path in; defaults to
                ``self.frame()``.

        Returns:
            The resolved ``lldb.SBValue``.
        """
        self.assertTrue(
            var_path.strip() == var_path,
            f"Variable path contains trailing/leading whitespace: '{var_path}'",
        )

        if frame is None:
            frame = self.frame()

        eval_result = frame.GetValueForVariablePath(var_path)
        ValueCheck(type=type, value=value, summary=summary, children=children).check_value(
            self, eval_result
        )
        return eval_result
