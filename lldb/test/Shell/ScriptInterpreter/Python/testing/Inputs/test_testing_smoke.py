"""Smoke test for lldb.testing.command, run as an lldb command via
lldb.testing.runner.register_unittest_command."""

import greet_cmd
import lldb
from lldb.testing.command import CommandTestCase
from lldb.testing.runner import UnitTestCommandObject, register_unittest_command


class TestGreetCommand(CommandTestCase):
    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        cls.addLldbCommand("greet", greet_cmd.GreetCommand(cls.debugger, {}))

    def test_custom_name(self) -> None:
        """custom name is greeted"""
        result = self.callLldbClass("--name lldb")
        self.assertCommandReturn(result)
        self.assertIn("hello lldb", result.GetOutput())

    def test_default_greeting(self) -> None:
        """default greeting says hello world"""
        result = self.callLldbClass("")
        self.assertCommandReturn(result)
        self.assertIn("hello world", result.GetOutput())


def __lldb_init_module(debugger: lldb.SBDebugger, internal_dict: dict) -> None:
    register_unittest_command(debugger, __name__, UnitTestCommandObject)
