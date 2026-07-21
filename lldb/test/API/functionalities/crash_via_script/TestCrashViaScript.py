"""
Test that Python code running inside lldb's embedded interpreter can crash the
lldb process, and that we observe it dying from a signal rather than exiting
cleanly.

This is a deliberate negative test: it provokes a segmentation fault so we have
coverage for how lldb terminates when embedded script code faults. It spawns a
real lldb via pexpect so the crash hits the lldb process, not the test runner
that hosts the SB API.
"""

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test.lldbpexpect import PExpectTest


class TestCrashViaScript(PExpectTest):
    # A short timeout so a hang (e.g. the crash not happening) fails fast.
    TIMEOUT = 20

    # A sanitizer would intercept the fault and change the exit behavior, so
    # the signal check below would no longer hold.
    @skipIfAsan
    def test_crash_lldb_via_script(self):
        """Dereference a null pointer from the embedded Python interpreter."""
        import pexpect

        self.launch()

        # ctypes.string_at reads a C string from the given address in lldb's
        # own process. Address 0 is never mapped, so this segfaults the lldb
        # process that hosts the interpreter.
        self.child.sendline("script import ctypes; ctypes.string_at(0)")

        # lldb should die instead of returning to the prompt.
        self.child.expect(pexpect.EOF)
        self.child.wait()

        self.assertFalse(self.child.isalive())
        self.assertIsNotNone(
            self.child.signalstatus,
            "lldb should have terminated due to a signal, not a clean exit",
        )
