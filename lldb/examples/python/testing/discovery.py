"""Cross-module test discovery and aggregation.

Generalized from cl4_lldb's ``DsymTestRunnerCommand``: walks every loaded
target's modules, finds each module's ``SBSymbolFileSpec`` sibling
``Python/`` directory, and imports/runs every ``test_*.py`` file found
there. This dSYM-based discovery convention is kept as the shipped
*default* strategy (genuinely useful for any dSYM-based debugging workflow),
but is fully overridable: a non-dSYM client (fixed ``sys.path``, plain
``command script import``) overrides :meth:`TestDiscoveryRunnerCommand.discover_test_modules`
instead of using :func:`discover_test_modules_via_dsym`.
"""

from __future__ import annotations

import argparse
import datetime
import json
import re
import shlex
import xml.etree.ElementTree as ET
from pathlib import Path

import lldb

from lldb.testing.command import LldbCommandBase, add_test_parser_args


def discover_test_modules_via_dsym(
    debugger: lldb.SBDebugger,
    only_index: int | None = None,
    *,
    marker: str = "register_unittest_command",
    exclude_marker: str | None = "lldb_freestanding_test",
) -> dict[int, list[Path]]:
    """Find ``test_*.py`` files inside every loaded target's dSYM(s).

    For each module of each target (or just ``only_index``), resolves the
    module's ``SBSymbolFileSpec`` directory to the sibling ``Python/``
    directory inside the dSYM bundle, and globs it (recursively) for
    ``test_*.py`` files containing ``marker``.

    Args:
        debugger: The debugger whose targets to search.
        only_index: If given, search only the target at this index.
        marker: A string that must appear in a file's contents for it to be
            considered a discoverable test module.
        exclude_marker: If given, files containing this string are skipped
            even if they contain ``marker`` (used to ignore legacy/
            incompatible test schemes).

    Returns:
        A dict mapping target index to the list of discovered test-file
        paths for that target.
    """
    found: dict[int, list[Path]] = {}
    seen_dirs: set[Path] = set()
    indices = [only_index] if only_index is not None else range(debugger.GetNumTargets())

    for target_index in indices:
        target = debugger.GetTargetAtIndex(target_index)
        for module_index in range(target.GetNumModules()):
            sym_spec = target.GetModuleAtIndex(module_index).GetSymbolFileSpec()
            if not sym_spec.IsValid():
                continue
            python_dir = Path(sym_spec.GetDirectory()).parent / "Python"
            if not python_dir.is_dir() or python_dir in seen_dirs:
                continue
            seen_dirs.add(python_dir)
            for test_file in sorted(python_dir.rglob("test_*.py")):
                content = test_file.read_text()
                if marker in content and not (exclude_marker and exclude_marker in content):
                    found.setdefault(target_index, []).append(test_file)

    return found


class TestDiscoveryRunnerCommand(LldbCommandBase):
    """Discovers and runs every registered test module across loaded targets.

    Register an instance of this (or a subclass) as an lldb command, e.g.
    ``command script add --class mymodule.TestDiscoveryRunnerCommand
    my_test_runner``. Supports ``-t``/``--target`` to scope to one target,
    and ``-C``/``--commands`` to list discovered test commands without
    running them, in addition to the standard test-runner arguments from
    :func:`lldb.testing.command.add_test_parser_args`.
    """

    def __init__(self, debugger: lldb.SBDebugger, internal_dict: dict) -> None:
        self.description = "Run and discover lldb unit tests across targets."
        year = datetime.datetime.now(tz=datetime.timezone.utc).year
        self._parser = argparse.ArgumentParser(
            description=self.description, epilog=f"Copyright (c) {year}."
        )
        add_test_parser_args(self._parser)
        self._parser.add_argument(
            "-C",
            "--commands",
            action="store_true",
            help="list discovered test commands by target without running them",
        )
        self._parser.add_argument(
            "-t",
            "--target",
            metavar="TARGET",
            type=str,
            default=None,
            help="target index or target.GetLabel() to run tests against",
        )

    def discover_test_modules(
        self, debugger: lldb.SBDebugger, only_index: int | None = None
    ) -> dict[int, list[Path]]:
        """Discover test modules to run.

        The default implementation delegates to
        :func:`discover_test_modules_via_dsym`. Override this method for a
        non-dSYM discovery strategy (e.g. globbing a fixed directory on
        ``sys.path``); everything else (importing, running, aggregating) is
        unchanged.

        Args:
            debugger: The debugger whose targets to search.
            only_index: If given, search only the target at this index.

        Returns:
            A dict mapping target index to the list of discovered test-file
            paths for that target.
        """
        return discover_test_modules_via_dsym(debugger, only_index)

    def _find_target(self, debugger: lldb.SBDebugger, target_id: str):
        try:
            index = int(target_id)
            target = debugger.GetTargetAtIndex(index)
            if target.IsValid():
                return target, index
        except ValueError:
            pass
        for index in range(debugger.GetNumTargets()):
            target = debugger.GetTargetAtIndex(index)
            if str(target.GetLabel()) == target_id:
                return target, index
        return None, None

    def _import_test_module(self, debugger: lldb.SBDebugger, test_file: Path) -> str:
        """Import ``test_file`` the same way ``command script import`` does.

        Re-executing the module's body on reload redefines its test classes
        fresh, so ``setUpClass`` picks up the currently selected target.

        Returns:
            The registered lldb command name (the module's filename stem).
        """
        result = lldb.SBCommandReturnObject()
        debugger.GetCommandInterpreter().HandleCommand(
            f"command script import {shlex.quote(str(test_file))}", result
        )
        if not result.Succeeded():
            raise RuntimeError(f"Failed to import {test_file.name}: {result.GetError()}")
        return test_file.stem

    def _aggregate_json(self, aggregated: dict, module_data: dict) -> None:
        """Merge a per-module JSON result dict into the running total.

        Any ``result_metadata_key`` value on the module's data (e.g.
        ``"radar"``) is stamped onto each of that module's failure/error/
        skipped entries so callers can tell which module a failure came
        from; the top-level metadata key itself is not copied into
        ``aggregated``.
        """
        for key in ("failures", "errors", "skipped"):
            for item in module_data.get(key, []):
                for meta_key, meta_value in module_data.items():
                    if meta_key in ("testsRun", "failures", "errors", "skipped",
                                    "output", "tests", "collectedDurations"):
                        continue
                    item.setdefault(meta_key, meta_value)

        if not aggregated:
            aggregated["testsRun"] = module_data.get("testsRun", 0)
            for key in ("failures", "errors", "skipped", "tests"):
                aggregated[key] = list(module_data.get(key, []))
            aggregated["output"] = module_data.get("output", "")
            if "collectedDurations" in module_data:
                aggregated["collectedDurations"] = list(module_data["collectedDurations"])
            return

        aggregated["testsRun"] = aggregated.get("testsRun", 0) + module_data.get("testsRun", 0)
        for key in ("failures", "errors", "skipped", "tests"):
            aggregated.setdefault(key, []).extend(module_data.get(key, []))
        aggregated["output"] = aggregated.get("output", "") + module_data.get("output", "")
        if "collectedDurations" in module_data:
            aggregated.setdefault("collectedDurations", []).extend(module_data["collectedDurations"])

    @staticmethod
    def _parse_test_counts(output: str) -> tuple[int, int, int]:
        """Extract ``(tests_run, failures, errors)`` from unittest text output."""
        ran = re.search(r"Ran (\d+) test", output)
        fail = re.search(r"failures=(\d+)", output)
        err = re.search(r"errors=(\d+)", output)
        return (
            int(ran.group(1)) if ran else 0,
            int(fail.group(1)) if fail else 0,
            int(err.group(1)) if err else 0,
        )

    @staticmethod
    def _json_to_xml(cmd_name: str, data: dict) -> ET.Element:
        """Build a JUnit-compatible ``<testsuite>`` element from a module's JSON result."""

        def normalize(test_str: str) -> str:
            match = re.match(r"^(\w+)\s+\((.+)\)$", test_str)
            return f"{match.group(2)}.{match.group(1)}" if match else test_str

        failures_by_id = {normalize(d["test"]): d["error"] for d in data.get("failures", [])}
        errors_by_id = {normalize(d["test"]): d["error"] for d in data.get("errors", [])}
        skipped_by_id = {normalize(d["test"]): d.get("error", "") for d in data.get("skipped", [])}
        durations = {d["test"]: d["error"] for d in data.get("collectedDurations", [])}
        time_match = re.search(r"Ran \d+ tests? in ([\d.]+)s", data.get("output", ""))

        suite = ET.Element(
            "testsuite",
            {
                "name": cmd_name,
                "tests": str(data.get("testsRun", 0)),
                "failures": str(len(failures_by_id)),
                "errors": str(len(errors_by_id)),
                "skipped": str(len(skipped_by_id)),
                "time": time_match.group(1) if time_match else "0",
            },
        )
        for entry in data.get("tests", []):
            test_id = entry["test"]
            classname, _, name = test_id.rpartition(".")
            testcase_el = ET.SubElement(
                suite,
                "testcase",
                {"classname": classname, "name": name, "time": str(durations.get(test_id, 0))},
            )
            if test_id in failures_by_id:
                text = failures_by_id[test_id]
                ET.SubElement(
                    testcase_el, "failure", {"message": text.strip().splitlines()[-1]}
                ).text = text
            elif test_id in errors_by_id:
                text = errors_by_id[test_id]
                ET.SubElement(
                    testcase_el, "error", {"message": text.strip().splitlines()[-1]}
                ).text = text
            elif test_id in skipped_by_id:
                ET.SubElement(testcase_el, "skipped").text = skipped_by_id[test_id]
        return suite

    def main(
        self,
        debugger: lldb.SBDebugger,
        exe_ctx: lldb.SBExecutionContext,
        result: lldb.SBCommandReturnObject,
        args: argparse.Namespace,
    ) -> None:
        original_target = debugger.GetSelectedTarget()

        target_index = None
        if args.target is not None:
            target, target_index = self._find_target(debugger, args.target)
            if target is None:
                raise RuntimeError(f"--target '{args.target}' not found")

        flag_parts = []
        if args.discover:
            flag_parts.append("--discover")
        if args.json or args.xml:
            flag_parts.append("--json")
        forwarded = " ".join(list(args.tests) + flag_parts)

        try:
            targets_to_run = self.discover_test_modules(debugger, target_index)

            if args.commands:
                for index, test_files in sorted(targets_to_run.items()):
                    target = debugger.GetTargetAtIndex(index)
                    label = target.GetLabel() or str(index)
                    commands = ", ".join(f.stem for f in test_files)
                    print(f"target #{index} ({label}): {commands}", file=result)
                return

            all_stems = {f.stem for files in targets_to_run.values() for f in files}
            ci = debugger.GetCommandInterpreter()
            aggregated = {} if (args.json or args.xml) else None
            xml_suites = [] if args.xml else None
            total_run = total_failures = total_errors = 0

            for index, test_files in sorted(targets_to_run.items()):
                debugger.SetSelectedTarget(debugger.GetTargetAtIndex(index))
                seen: set[str] = set()
                for test_file in test_files:
                    try:
                        cmd_name = self._import_test_module(debugger, test_file)
                    except RuntimeError as error:
                        result.AppendMessage(f"Warning: {error}\n")
                        result.SetStatus(lldb.eReturnStatusFailed)
                        continue
                    if cmd_name in seen:
                        continue
                    seen.add(cmd_name)

                    if args.tests:
                        if cmd_name in args.tests:
                            cmd_forwarded = " ".join(flag_parts)
                        else:
                            relevant = [
                                t for t in args.tests
                                if t.startswith(cmd_name + ".") or t.split(".")[0] not in all_stems
                            ]
                            if not relevant:
                                continue
                            cmd_forwarded = " ".join(relevant + flag_parts)
                    else:
                        cmd_forwarded = forwarded

                    module_result = lldb.SBCommandReturnObject()
                    ci.HandleCommand(f"{cmd_name} {cmd_forwarded}", module_result)
                    output = module_result.GetOutput()
                    if output:
                        if args.json or args.xml:
                            try:
                                module_data = json.loads(output.rstrip("\n"))
                                self._aggregate_json(aggregated, module_data)
                                if args.xml:
                                    xml_suites.append(self._json_to_xml(cmd_name, module_data))
                            except json.JSONDecodeError:
                                result.AppendMessage(output.rstrip() + "\n\n")
                        else:
                            n_run, n_fail, n_err = self._parse_test_counts(output)
                            total_run += n_run
                            total_failures += n_fail
                            total_errors += n_err
                            result.AppendMessage(output.rstrip() + "\n\n")
                    if not module_result.Succeeded():
                        result.SetStatus(lldb.eReturnStatusFailed)
                        if module_result.GetError():
                            result.AppendMessage(module_result.GetError())

            if args.xml and xml_suites:
                root = ET.Element("testsuites")
                for suite in xml_suites:
                    root.append(suite)
                ET.indent(root)
                ET.ElementTree(root).write(args.xml, encoding="unicode", xml_declaration=True)

            if (args.json or args.xml) and aggregated:
                n_run = aggregated.get("testsRun", 0)
                n_fail = len(aggregated.get("failures", []))
                n_err = len(aggregated.get("errors", []))
                n_skip = len(aggregated.get("skipped", []))
                n_pass = n_run - n_fail - n_err - n_skip
                total_run, total_failures, total_errors = n_run, n_fail, n_err
                if args.json:
                    json_str = json.dumps(aggregated, indent=2)
                    print(json_str, file=result)
                    if args.file:
                        with open(args.file, "w", encoding="utf-8") as f:
                            f.write(json_str + "\n")
            else:
                n_pass = total_run - total_failures - total_errors

            summary = f"Ran {total_run} tests: {n_pass} passed"
            if total_failures:
                summary += f", {total_failures} failed"
            if total_errors:
                summary += f", {total_errors} errors"
            if total_failures or total_errors:
                result.SetError(summary + "\n")
                result.SetStatus(lldb.eReturnStatusFailed)
            elif not args.json:
                print(summary, file=result)
        finally:
            debugger.SetSelectedTarget(original_target)
