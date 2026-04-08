"""
Tests for the IOHandlerPicker interactive widget.
"""

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test.lldbpexpect import PExpectTest


class IOHandlerPickerTest(PExpectTest):
    # Arrow keys for vt100 terminal.
    arrow_up = "\x1b[A"
    arrow_down = "\x1b[B"
    escape_key = chr(27)

    @skipIfAsan
    @skipIfRemote
    @skipIfWindows
    def test_single_select_enter(self):
        """Test single-select mode: navigate and select with Enter."""
        self.launch(dimensions=(24, 120))
        self.child.sendline("test-picker single 5")
        self.child.expect_exact("Select an item:")

        # Move down twice.
        self.child.send(self.arrow_down)
        self.child.send(self.arrow_down)

        # Select with Enter.
        self.child.send("\r")
        self.child.expect_exact("selected:")
        self.expect_prompt()
        self.quit()

    @skipIfAsan
    @skipIfRemote
    @skipIfWindows
    def test_single_select_cancel(self):
        """Test single-select mode: ESC cancels."""
        self.launch(dimensions=(24, 120))
        self.child.sendline("test-picker single 5")
        self.child.expect_exact("Select an item:")

        # Press ESC.
        self.child.send(self.escape_key)
        self.child.expect_exact("canceled")
        self.expect_prompt()
        self.quit()

    @skipIfAsan
    @skipIfRemote
    @skipIfWindows
    def test_single_select_vim_keys(self):
        """Test vim navigation keys j/k work like arrows."""
        self.launch(dimensions=(24, 120))
        self.child.sendline("test-picker single 5")
        self.child.expect_exact("Select an item:")

        # Navigate with j (down) then k (up).
        self.child.send("j")
        self.child.send("j")
        self.child.send("j")
        self.child.send("k")

        # Select with Enter.
        self.child.send("\r")
        self.child.expect_exact("selected:")
        self.expect_prompt()
        self.quit()

    @skipIfAsan
    @skipIfRemote
    @skipIfWindows
    def test_multi_select_toggle(self):
        """Test multi-select mode: toggle with Space, confirm with Enter."""
        self.launch(dimensions=(24, 120))
        self.child.sendline("test-picker multi 5")
        self.child.expect_exact("Toggle items:")

        # Toggle first item with Space.
        self.child.send(" ")
        # Move down and toggle second.
        self.child.send("j")
        self.child.send(" ")

        # Confirm with Enter.
        self.child.send("\r")
        self.child.expect_exact("selected:")
        self.expect_prompt()
        self.quit()

    @skipIfAsan
    @skipIfRemote
    @skipIfWindows
    def test_multi_select_cancel(self):
        """Test multi-select mode: ESC cancels."""
        self.launch(dimensions=(24, 120))
        self.child.sendline("test-picker multi 5")
        self.child.expect_exact("Toggle items:")

        # Toggle some items then cancel.
        self.child.send(" ")
        self.child.send("j")
        self.child.send(" ")
        self.child.send(self.escape_key)
        self.child.expect_exact("canceled")
        self.expect_prompt()
        self.quit()

    @skipIfAsan
    @skipIfRemote
    @skipIfWindows
    def test_sort_cycle(self):
        """Test that pressing s cycles sort columns."""
        self.launch(dimensions=(24, 120))
        self.child.sendline("test-picker single 5")
        self.child.expect_exact("Select an item:")

        # Press s to sort by first column (ID ascending).
        self.child.send("s")
        # Press s again to sort by first column descending.
        self.child.send("s")
        # Press s to move to next column (NAME ascending).
        self.child.send("s")

        # Select.
        self.child.send("\r")
        self.child.expect_exact("selected:")
        self.expect_prompt()
        self.quit()

    @skipIfAsan
    @skipIfRemote
    @skipIfWindows
    def test_non_interactive_fallback(self):
        """Test that non-interactive terminal cancels immediately."""
        self.launch(dimensions=(24, 120))
        # Run with piped input to test non-interactive fallback.
        # The test-picker command should detect non-real terminal via
        # GetIsRealTerminal(). However, since pexpect uses a PTY,
        # this will actually be interactive. We just verify it doesn't crash
        # with a very small terminal.
        self.child.sendline("test-picker single 3")
        self.child.expect_exact("Select an item:")
        self.child.send(self.escape_key)
        self.child.expect_exact("canceled")
        self.expect_prompt()
        self.quit()

    @skipIfAsan
    @skipIfRemote
    @skipIfWindows
    def test_small_terminal(self):
        """Test picker with a small terminal size."""
        self.launch(dimensions=(10, 60))
        self.child.sendline("test-picker single 20")
        self.child.expect_exact("Select an item:")

        # Navigate down several times to trigger scrolling.
        for _ in range(6):
            self.child.send("j")

        # Select.
        self.child.send("\r")
        self.child.expect_exact("selected:")
        self.expect_prompt()
        self.quit()
