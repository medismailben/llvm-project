#!/usr/bin/env python3
"""
Test script to verify Python exceptions in scripted processes are surfaced to users.
This script defines a ScriptedProcess that intentionally raises exceptions.
"""

class TestScriptedProcess:
    """A scripted process that raises exceptions to test error surfacing."""

    def __init__(self, exe_ctx, args):
        """Initialize the scripted process."""
        self.exe_ctx = exe_ctx
        self.args = args
        print("TestScriptedProcess initialized")

    def get_capabilities(self):
        """Return process capabilities - this will raise an exception."""
        # Intentionally raise an exception with a clear traceback
        raise RuntimeError("TEST: This exception should be surfaced to the user with full backtrace!")

    def launch(self):
        """Launch the process - also raises an exception."""
        raise ValueError("TEST: Launch failed with a custom exception!")

    def get_process_id(self):
        """Return process ID."""
        return 12345

    def is_alive(self):
        """Check if process is alive."""
        return True

    def get_threads_info(self):
        """Return thread information - will raise exception."""
        # Use nested function to create deeper traceback
        def nested_function():
            raise TypeError("TEST: Exception from nested function in get_threads_info!")
        nested_function()
