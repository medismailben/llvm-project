"""Turn a ``unittest`` test module into a runnable lldb command.

Generalized from cl4_lldb's ``UnitTestCommandObject``/
``register_unittest_command``. The hardcoded "radar component: ..."
extraction from cl4's original implementation becomes an off-by-default,
named-neutral hook (:attr:`UnitTestCommandObject.result_metadata_key`/
:attr:`UnitTestCommandObject.result_metadata_pattern`) so this stays
Apple-agnostic; a client wanting that behavior sets the two attributes on a
subclass.
"""

from __future__ import annotations

import argparse
import datetime
import io
import json
import os
import re
import sys
import unittest

import lldb

from lldb.testing.command import LldbCommandBase, add_test_parser_args

_TEST_FILE_NAME_ENV_VAR = f"__{__name__}__TEST_FILE_NAME"


class UnitTestCommandObject(LldbCommandBase):
    """Runs a test module's ``unittest`` tests as an lldb command.

    Supports ``--discover`` (list tests), ``--json``/``--xml`` output,
    ``--file`` (save output), and positional test-ID selection. Register an
    instance of this (or a subclass) with :func:`register_unittest_command`.
    """

    result_metadata_key: str | None = None
    """If set, extract a value from the run's text output using
    :attr:`result_metadata_pattern` and attach it to the JSON result under
    this key (e.g. ``"radar"``)."""

    result_metadata_pattern: str | None = None
    """A regex with one capture group, matched against the run's text
    output, whose first match populates :attr:`result_metadata_key`
    (e.g. ``r"^radar component: (.*)$"``)."""

    def __init__(self, debugger: lldb.SBDebugger, internal_dict: dict) -> None:
        """Initialize the command, reading the registering module's name
        from the environment variable set by :func:`register_unittest_command`."""
        self.name = os.environ[_TEST_FILE_NAME_ENV_VAR]
        self.description = "Run lldb script unittest from file."
        year = datetime.datetime.now(tz=datetime.timezone.utc).year
        self._parser = argparse.ArgumentParser(
            description=self.description,
            epilog=f"Copyright (c) {year}.",
        )
        add_test_parser_args(self._parser)

    def main(
        self,
        debugger: lldb.SBDebugger,
        exe_ctx: lldb.SBExecutionContext,
        result: lldb.SBCommandReturnObject,
        args: argparse.Namespace,
    ) -> None:
        loader = unittest.TestLoader()
        suite = loader.loadTestsFromModule(sys.modules[self.name])
        chosen_suite = unittest.TestSuite()
        test_list = []
        chosen_list = []

        for group in suite:
            for test_case in group:
                docstring = getattr(test_case, test_case._testMethodName).__doc__
                info = {"test": test_case.id(), "docstring": docstring or "\n"}
                test_list.append(info)
                if test_case.id() in args.tests:
                    chosen_suite.addTest(test_case)
                    chosen_list.append(info)

        if args.tests:
            suite, test_list = chosen_suite, chosen_list
            if len(args.tests) != len(chosen_list):
                found = ", ".join(item["test"] for item in chosen_list)
                plural = "" if len(args.tests) == 1 else "s"
                if not chosen_list:
                    raise RuntimeError(f"Did not find test{plural}")
                raise RuntimeError(f"Did not find all requested test{plural}. Just found {found}")

        if args.xml:
            try:
                import xmlrunner
            except ImportError:
                raise RuntimeError("Python xmlrunner not installed")

        if args.discover:
            self._handle_discover(args, test_list)
            return

        stream = io.StringIO()
        if args.xml:
            with open(args.xml, "w", encoding="utf-8") as xml_stream:
                test_result = xmlrunner.XMLTestRunner(output=xml_stream, stream=stream).run(suite)
        else:
            test_result = unittest.TextTestRunner(stream=stream).run(suite)
        output = stream.getvalue()

        data = {
            "testsRun": test_result.testsRun,
            "failures": self._serialize(test_result.failures),
            "errors": self._serialize(test_result.errors),
            "skipped": self._serialize(test_result.skipped),
            "output": output,
            "tests": test_list,
        }
        if self.result_metadata_key and self.result_metadata_pattern:
            match = re.search(self.result_metadata_pattern, output, flags=re.MULTILINE)
            if match:
                data[self.result_metadata_key] = match.group(1)
        durations = getattr(test_result, "collectedDurations", None)
        if durations is not None:
            data["collectedDurations"] = self._serialize(durations)

        if args.json:
            print(json.dumps(data, indent=2), file=result)
            if args.file:
                with open(args.file, "w", encoding="utf-8") as f:
                    json.dump(data, f, ensure_ascii=False, indent=2)
        else:
            print(data["output"], file=self.result)
            self._pprint_error("failures", data["failures"])
            self._pprint_error("errors", data["errors"])
            self._pprint_error("skipped", data["skipped"])
            self._pprint_tests(data["tests"])
            if args.file:
                with open(args.file, "w", encoding="utf-8") as f:
                    f.write(self.result.GetOutput())

    def _handle_discover(self, args: argparse.Namespace, test_list: list[dict]) -> None:
        if args.xml:
            raise RuntimeError("--discover does not currently support --xml")
        if args.json:
            data = {"tests": test_list}
            print(json.dumps(data, indent=2), file=self.result)
            if args.file:
                with open(args.file, "w", encoding="utf-8") as f:
                    json.dump(data, f, ensure_ascii=False, indent=2)
        else:
            self._pprint_tests(test_list)
            if args.file:
                with open(args.file, "w", encoding="utf-8") as f:
                    f.write(self.result.GetOutput())

    def _pprint_tests(self, tests: list[dict]) -> None:
        for entry in tests:
            print(f'{entry["test"]}: {entry["docstring"]}', file=self.result)

    def _pprint_error(self, name: str, entries: list[dict]) -> None:
        if entries:
            print(f"Info on {name}:", file=self.result)
        for entry in entries:
            print(entry["test"], file=self.result)
            print(entry["error"], file=self.result)

    @staticmethod
    def _serialize(test_result: list[tuple[unittest.TestCase, str]]) -> list[dict]:
        return [{"test": str(test), "error": error} for test, error in test_result]


def register_unittest_command(
    debugger: lldb.SBDebugger, name: str, cmd_class: type = UnitTestCommandObject
) -> None:
    """Register a test module's ``unittest`` tests as an lldb command.

    Call from the test module's ``__lldb_init_module``:

    .. code-block:: python

        def __lldb_init_module(debugger, internal_dict):
            register_unittest_command(debugger, __name__, UnitTestCommandObject)

    Args:
        debugger: The debugger to register the command on.
        name: The registering module's ``__name__``; also becomes the
            command's name.
        cmd_class: The :class:`UnitTestCommandObject` subclass to use.
    """
    os.environ[_TEST_FILE_NAME_ENV_VAR] = name
    debugger.HandleCommand(
        f"command script add --overwrite --class {name}.{cmd_class.__name__} {name}"
    )
