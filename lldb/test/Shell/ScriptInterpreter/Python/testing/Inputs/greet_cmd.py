"""A tiny lldb command used to smoke-test lldb.testing.command.CommandTestCase."""

import argparse

import lldb
from lldb.testing.command import LldbCommandBase


class GreetCommand(LldbCommandBase):
    def __init__(self, debugger: lldb.SBDebugger, internal_dict: dict) -> None:
        self.description = "Print a greeting."
        self._parser = argparse.ArgumentParser(description=self.description)
        self._parser.add_argument("--name", default="world")

    def main(self, debugger, exe_ctx, result, args) -> None:
        print(f"hello {args.name}", file=result)


def __lldb_init_module(debugger: lldb.SBDebugger, internal_dict: dict) -> None:
    debugger.HandleCommand(
        f"command script add --overwrite --class {__name__}.GreetCommand greet"
    )
