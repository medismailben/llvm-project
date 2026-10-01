"""Assertion mixins for ``unittest.TestCase`` subclasses testing lldb.

Mix :class:`LLDBAssertsMixin` and/or :class:`ExtraAssertsMixin` into any
``unittest.TestCase`` (or use :class:`lldb.testing.testcase.LLDBTestCase`,
which already includes both).
"""

from __future__ import annotations

import json
import uuid

import lldb

from lldb.testing import lldbutil


class LLDBAssertsMixin:
    """Assertions for ``lldb.SBError``/state/stop-reason values."""

    def assertSuccess(self, obj: lldb.SBError, msg: str | None = None) -> None:
        """Fail unless ``obj.Success()``.

        Args:
            obj: The error to check.
            msg: An optional additional failure message.
        """
        if not obj.Success():
            error = obj.GetCString()
            self.fail(self._formatMessage(msg, f"'{error}' is not success"))

    def assertFailure(
        self, obj: lldb.SBError, error_str: str | None = None, msg: str | None = None
    ) -> None:
        """Fail unless ``obj`` is in a failure state.

        Args:
            obj: The error to check.
            error_str: If given, also require ``obj.GetCString() == error_str``.
            msg: An optional additional failure message.
        """
        if obj.Success():
            self.fail(self._formatMessage(msg, "Error not in a fail state"))
        if error_str is not None:
            self.assertEqual(obj.GetCString(), error_str, msg)

    def assertState(self, first: int, second: int, msg: str | None = None) -> None:
        """Fail unless the two ``lldb.eState*`` values are equal.

        Args:
            first: The first state value.
            second: The second state value.
            msg: An optional additional failure message.
        """
        if first != second:
            error = (
                f"{lldbutil.state_type_to_str(first)} ({first}) != "
                f"{lldbutil.state_type_to_str(second)} ({second})"
            )
            self.fail(self._formatMessage(msg, error))

    def assertStopReason(self, first: int, second: int, msg: str | None = None) -> None:
        """Fail unless the two ``lldb.eStopReason*`` values are equal.

        Args:
            first: The first stop-reason value.
            second: The second stop-reason value.
            msg: An optional additional failure message.
        """
        if first != second:
            error = (
                f"{lldbutil.stop_reason_to_str(first)} ({first}) != "
                f"{lldbutil.stop_reason_to_str(second)} ({second})"
            )
            self.fail(self._formatMessage(msg, error))


class ExtraAssertsMixin:
    """General-purpose assertions useful when testing lldb scripting extensions."""

    def assertKeysInDict(
        self, keys, dictionary: dict, msg: str | None = None
    ) -> None:
        """Fail if any of ``keys`` is missing from ``dictionary``.

        Args:
            keys: The keys that must be present.
            dictionary: The dict to check.
            msg: An optional additional failure message.
        """
        for key in keys:
            with self.subTest(key=key):
                self.assertIn(key, dictionary, msg)

    def assertJson(self, val: str | bytes | bytearray, msg: str | None = None) -> None:
        """Fail unless ``val`` is valid JSON.

        Args:
            val: The string/bytes to parse.
            msg: An optional additional failure message.
        """
        try:
            json.loads(val)
        except json.JSONDecodeError as error:
            self.fail(f'{msg or "JSON"}: {error} for "{val}"')

    def assertUuid(self, val, msg: str | None = None) -> None:
        """Fail unless ``val`` is a valid UUID string.

        Args:
            val: The value to check.
            msg: An optional additional failure message.
        """
        try:
            uuid.UUID(str(val))
        except ValueError as error:
            self.fail(f'{msg or "UUID"}: {error} for "{val}"')
