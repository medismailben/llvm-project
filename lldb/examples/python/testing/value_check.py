"""Recursive assertion helper for checking ``lldb.SBValue`` contents.

:class:`ValueCheck` is the natural fit for data-formatter test authors who
need to check an ``SBValue``'s name/value/type/summary/children/dereference
recursively — e.g. to verify a synthetic-children or summary provider
produces the expected structure.
"""

from __future__ import annotations

import re

import lldb


class ValueCheck:
    """Describes the expected shape of an ``lldb.SBValue``.

    Every constructor argument is optional; only the properties that are
    set are checked by :meth:`check_value`. ``value`` and ``summary`` may
    be a literal string (exact match) or a compiled regex (matched with
    ``assertRegex``).
    """

    def __init__(
        self,
        name: str | None = None,
        value: str | re.Pattern | None = None,
        type: str | None = None,
        summary: str | re.Pattern | None = None,
        children: list["ValueCheck"] | None = None,
        dereference: "ValueCheck | None" = None,
    ) -> None:
        """Initialize a ``ValueCheck``.

        Args:
            name: The expected ``SBValue.GetName()``.
            value: The expected ``SBValue.GetValue()`` (string or regex).
            type: The expected ``SBValue.GetDisplayTypeName()``.
            summary: The expected ``SBValue.GetSummary()`` (string or regex).
            children: The expected children, checked positionally and for
                an exact count.
            dereference: A ``ValueCheck`` applied to ``val.Dereference()``.
        """
        self.expect_name = name
        self.expect_value = value
        self.expect_type = type
        self.expect_summary = summary
        self.children = children
        self.dereference = dereference

    def check_value(self, test_base, val: lldb.SBValue, error_msg: str = "") -> None:
        """Assert that ``val`` matches every property set on this check.

        Args:
            test_base: Any object exposing ``assertSuccess``/``assertEqual``/
                ``assertRegex`` (e.g. a ``unittest.TestCase`` mixed with
                :class:`lldb.testing.asserts.LLDBAssertsMixin`).
            val: The value to check.
            error_msg: Extra context prefixed to any failure message.
        """
        this_error_msg = f"{error_msg}\nChecking SBValue: {val}"

        test_base.assertSuccess(val.GetError())

        if self.expect_name is not None:
            test_base.assertEqual(self.expect_name, val.GetName(), this_error_msg)
        if self.expect_value is not None:
            if isinstance(self.expect_value, re.Pattern):
                test_base.assertRegex(val.GetValue(), self.expect_value, this_error_msg)
            else:
                test_base.assertEqual(self.expect_value, val.GetValue(), this_error_msg)
        if self.expect_type is not None:
            test_base.assertEqual(
                self.expect_type, val.GetDisplayTypeName(), this_error_msg
            )
        if self.expect_summary is not None:
            if isinstance(self.expect_summary, re.Pattern):
                test_base.assertRegex(
                    val.GetSummary(), self.expect_summary, this_error_msg
                )
            else:
                test_base.assertEqual(
                    self.expect_summary, val.GetSummary(), this_error_msg
                )
        if self.children is not None:
            self.check_value_children(test_base, val, error_msg)
        if self.dereference is not None:
            self.dereference.check_value(test_base, val.Dereference(), error_msg)

    def check_value_children(
        self, test_base, val: lldb.SBValue, error_msg: str = ""
    ) -> None:
        """Assert that ``val``'s children match :attr:`children` exactly.

        Args:
            test_base: Any object exposing ``assertEqual`` (see
                :meth:`check_value`).
            val: The value whose children to check.
            error_msg: Extra context prefixed to any failure message.
        """
        this_error_msg = f"{error_msg}\nChecking SBValue: {val}"
        test_base.assertEqual(len(self.children), val.GetNumChildren(), this_error_msg)

        for index, expected_child in enumerate(self.children):
            actual_child = val.GetChildAtIndex(index)
            child_error = f"Checking child with index {index}:\n{error_msg}"
            expected_child.check_value(test_base, actual_child, child_error)
