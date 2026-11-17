"""
Tests for the IOHandlerPicker interactive widget.

`test-picker` lists items in descending id order (5, 4, 3, 2, 1 for a count of
5), and multi-select mode pre-selects ids 1, 4, 7, ...
"""

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test.lldbpexpect import PExpectTest


@skipIfAsan
@skipIfRemote
@skipIfWindows
class IOHandlerPickerTest(PExpectTest):
    arrow_down = "\x1b[B"
    arrow_right = "\x1b[C"
    escape_key = "\x1b"

    def start_picker(
        self, command, header, dimensions=(24, 120), setup=(), executable=None
    ):
        if command.startswith("test-picker") and not (
            self.dbg.GetCommandInterpreter().CommandExists("test-picker")
        ):
            self.skipTest("test-picker requires a build with assertions")
        self.launch(executable=executable, dimensions=dimensions)
        # launch() runs "settings clear --all", which resets the terminal size.
        height, width = dimensions
        self.expect(f"settings set term-height {height}")
        self.expect(f"settings set term-width {width}")
        for setup_command, patterns in setup:
            self.expect(setup_command, patterns)
        self.child.sendline(command)
        self.child.expect_exact(header)

    def finish(self, *expected):
        for line in expected:
            self.child.expect_exact(line)
        self.expect_prompt()
        self.quit()

    def test_single_select_arrows(self):
        """Navigate with arrow keys and select with Enter."""
        self.start_picker("test-picker single 5", "Select an item:")
        self.child.send(self.arrow_down)
        self.child.send(self.arrow_down)
        self.child.send("\r")
        self.finish("selected: 3")

    def test_single_select_vim_keys(self):
        """Navigate with j/k and select with Enter."""
        self.start_picker("test-picker single 5", "Select an item:")
        self.child.send("jjk")
        self.child.send("\r")
        self.finish("selected: 4")

    def test_home_end_tilde_sequences(self):
        """Home and End also work as "ESC [ 1 ~" and "ESC [ 4 ~"."""
        self.start_picker("test-picker single 5", "Select an item:")
        # Home, End, then up: the second-to-last item.
        self.child.send("jj\x1b[1~\x1b[4~k\r")
        self.finish("selected: 2")

    def test_single_select_cancel(self):
        """ESC cancels without selecting anything."""
        self.start_picker("test-picker single 5", "Select an item:")
        self.child.send(self.escape_key)
        self.finish("canceled")

    def test_multi_select_toggle(self):
        """Space toggles items and Enter confirms the checked set."""
        self.start_picker("test-picker multi 5", "Toggle items:")
        # Check 5, then uncheck the pre-selected 4. Pre-selected 1 stays.
        self.child.send(" j ")
        self.child.send("\r")
        self.finish("selected: 5", "selected: 1")

    def test_multi_select_cancel(self):
        """ESC discards toggles."""
        self.start_picker("test-picker multi 5", "Toggle items:")
        self.child.send(" j ")
        self.child.send(self.escape_key)
        self.finish("canceled")

    def test_confirm_side_by_side(self):
        """The confirm picker shows both answers on one line, and Enter picks
        the default one."""
        self.start_picker("test-picker confirm", "Are you sure?")
        # Only spaces and styles between the two answers.
        self.child.expect(r"\[ Yes \](?:\x1b\[[0-9;]*m| )*\[ No \]")
        self.child.send("\r")
        self.finish("answer: yes")

    def test_confirm_switch(self):
        """Arrow keys and Tab switch between the answers."""
        self.start_picker("test-picker confirm", "Are you sure?")
        self.child.send(self.arrow_right)
        self.child.send("\r")
        self.finish("answer: no")

    def test_confirm_shortcuts(self):
        """y and n answer right away."""
        self.start_picker("test-picker confirm", "Are you sure?")
        self.child.send("n")
        self.finish("answer: no")

    def test_confirm_cancel(self):
        """ESC cancels the confirm picker."""
        self.start_picker("test-picker confirm", "Are you sure?")
        self.child.send("\t")
        self.child.send(self.escape_key)
        self.finish("canceled")

    def test_sort_ascending(self):
        """The first s sorts by the first column, ascending."""
        self.start_picker("test-picker single 5", "Select an item:")
        self.child.send("s")
        self.child.expect_exact("ID ^")
        self.child.send("j\r")
        self.finish("selected: 2")

    def test_sort_cycle(self):
        """Each s advances the sort: ID asc, ID desc, NAME asc, NAME desc."""
        self.start_picker("test-picker single 5", "Select an item:")
        self.child.send("ssss")
        self.child.expect_exact("NAME v")
        # NAME descending: echo (5), delta (4), ...
        self.child.send("j\r")
        self.finish("selected: 4")

    def test_small_terminal_scrolls(self):
        """Moving past the viewport scrolls the list."""
        self.start_picker(
            "test-picker single 20", "Select an item:", dimensions=(12, 60)
        )
        self.child.send("j" * 6)
        self.child.expect_exact("[more above]")
        self.child.send("\r")
        self.finish("selected: 14")

    def test_dialog_centered(self):
        """The dialog is drawn in the middle of the screen."""
        self.start_picker("test-picker single 5", "Select an item:")
        # The dialog is 11 lines high, with (24 - 11) / 2 lines above it, and
        # leaves a tenth of the terminal's width on each side.
        self.child.send("j")
        self.child.expect_exact("\x1b[7;13H")
        self.child.send("\r")
        self.finish("selected: 4")

    def test_restores_screen(self):
        """Closing the dialog redraws what LLDB printed under it."""
        self.start_picker(
            "test-picker single 5",
            "Select an item:",
            setup=[("help test-picker", ["Test the IOHandlerPicker"])],
        )
        self.child.send("\r")
        # The earlier command and its output, then the picker's command.
        self.finish(
            "help test-picker",
            "Test the IOHandlerPicker",
            "test-picker single 5",
            "selected: 5",
        )

    def test_dumb_terminal(self):
        """A terminal with TERM=dumb doesn't handle escape codes, so the
        picker doesn't open."""
        if not self.dbg.GetCommandInterpreter().CommandExists("test-picker"):
            self.skipTest("test-picker requires a build with assertions")
        self.launch(run_under=["env", "TERM=dumb"])
        self.child.sendline("test-picker single 5")
        self.child.expect_exact("canceled")
        self.expect_prompt()
        self.quit()

    def test_resize_narrower(self):
        """Shrinking the terminal's width clips the dialog to the new width."""
        self.start_picker("test-picker single 5", "Select an item:")
        self.child.setwinsize(24, 40)
        # The content is wider than the terminal, so the dialog drops its
        # margins and starts at the first column, on the same line as before.
        self.child.expect_exact("\x1b[7;1H")
        self.child.send("j\r")
        self.finish("selected: 4")

    def test_resize_shorter(self):
        """Shrinking the terminal's height shrinks the list's viewport."""
        self.start_picker("test-picker single 20", "Select an item:")
        self.child.setwinsize(12, 120)
        # Twelve lines fit the dialog's six lines of chrome and four rows, with
        # a line of the screen visible above and below it.
        self.child.expect_exact("Select an item:")
        self.child.send("j" * 4)
        self.child.expect_exact("[more above]")
        self.child.send("\r")
        self.finish("selected: 16")

    def test_long_values_fit(self):
        """A column with long values narrows to fit the dialog, so the columns
        after it stay visible."""
        self.start_picker(
            "test-picker single 5 100", "Select an item:", dimensions=(25, 80)
        )
        # The middle of echo (5) is cut out, so both of its ends show, and so
        # does its VALUE.
        self.child.expect("echo-+\u2026-+echo  9595".encode())
        self.child.send("\r")
        self.finish("selected: 5")

    def test_resize_too_short_cancels(self):
        """Shrinking the terminal below the dialog's minimum height cancels
        the picker."""
        self.start_picker("test-picker single 5", "Select an item:")
        # LLDB reads a terminal shorter than 10 lines as 10 lines.
        self.child.setwinsize(8, 120)
        self.finish("canceled")

    def test_resize_too_narrow_cancels(self):
        """Shrinking the terminal below the dialog's minimum width cancels the
        picker."""
        self.start_picker("test-picker single 5", "Select an item:")
        self.child.setwinsize(24, 39)
        self.finish("canceled")

    def test_statusline_stays(self):
        """The dialog draws above the statusline instead of removing it."""
        self.start_picker(
            "test-picker single 5",
            "Select an item:",
            setup=[("settings set show-statusline true", ["\x1b[1;23r"])],
        )
        self.child.send("j\r")
        self.child.expect_exact("selected: 4")
        # Resetting the scroll region to the whole terminal would remove the
        # statusline.
        self.assertNotIn(b"\x1b[1;24r", self.child.before)
        self.expect_prompt()
        self.quit()

    def test_search_filters_rows(self):
        """/ filters rows to those with a column containing the query."""
        self.start_picker("test-picker single 26", "Select an item:")
        # Matches quebec (17) and echo (5). Enter applies the filter.
        self.child.send("/ec\r")
        self.child.expect_exact("filter: ec  (24 hidden)")
        self.child.send("j\r")
        self.finish("selected: 5")

    def test_search_typing_is_not_navigation(self):
        """While searching, j/k/s/Space are part of the query and case is
        ignored."""
        self.start_picker("test-picker single 26", "Select an item:")
        self.child.send("/jULIET\r\r")
        self.finish("selected: 10")

    def test_search_backspace(self):
        """Backspace edits the query and brings back matches."""
        self.start_picker("test-picker single 26", "Select an item:")
        self.child.send("/ecx")
        self.child.expect_exact("no matches  (26 hidden)")
        self.child.send("\x7f\r\r")
        self.finish("selected: 17")

    def test_search_ignores_unhandled_sequences(self):
        """Ignored escape sequences don't edit the query."""
        self.start_picker("test-picker single 26", "Select an item:")
        # Delete is "ESC [ 3 ~".
        self.child.send("/ec\x1b[3~\r")
        self.child.expect_exact("filter: ec  (24 hidden)")
        self.child.send("j\r")
        self.finish("selected: 5")

    def test_search_escape_clears(self):
        """ESC while searching clears the filter instead of canceling."""
        self.start_picker("test-picker single 26", "Select an item:")
        self.child.send("/zz")
        self.child.expect_exact("/zz_")
        self.child.send(self.escape_key)
        self.child.expect_exact("/: search")
        self.child.send("\r")
        self.finish("selected: 26")

    def test_incremental_search(self):
        """Ctrl+S filters the rows like / and moves from match to match."""
        self.start_picker("test-picker single 26", "Select an item:")
        # quebec (17), then echo (5).
        self.child.send("\x13ec")
        self.child.expect_exact("I-search: ec_")
        self.child.expect_exact("1/2  (24 hidden)")
        self.child.send("\x13")
        self.child.expect_exact("2/2  (24 hidden)")
        # Enter ends the search and keeps the filter, and the next one selects.
        self.child.send("\r")
        self.child.expect_exact("filter: ec  (24 hidden)")
        self.child.send("\r")
        self.finish("selected: 5")

    def test_incremental_search_backward(self):
        """Ctrl+R starts from the last match, and wraps around."""
        self.start_picker("test-picker single 26", "Select an item:")
        self.child.send("\x12ec")
        self.child.expect_exact("I-search backward: ec_")
        self.child.expect_exact("2/2  (24 hidden)")
        self.child.send("\x12\x12")
        self.child.expect_exact("2/2  (24 hidden)")
        self.child.send("\x12\r\r")
        self.finish("selected: 17")

    def test_incremental_search_cancel(self):
        """ESC clears the filter and puts the cursor back."""
        self.start_picker("test-picker single 26", "Select an item:")
        self.child.send("j\x13ec")
        self.child.expect_exact("1/2  (24 hidden)")
        self.child.send(self.escape_key)
        self.child.expect_exact("/: search")
        self.child.expect_exact("2/26")
        self.child.send("\r")
        self.finish("selected: 25")

    def test_incremental_search_failing(self):
        """A query without a match hides every row until Backspace fixes it."""
        self.start_picker("test-picker single 26", "Select an item:")
        self.child.send("\x13eczz")
        self.child.expect_exact("failing I-search: eczz_")
        self.child.expect_exact("no matches")
        self.child.send("\x7f\x7f")
        self.child.expect_exact("1/2  (24 hidden)")
        self.child.send("\r\r")
        self.finish("selected: 17")

    def test_search_multi_keeps_hidden_selection(self):
        """Items hidden by the filter stay checked in multi-select mode."""
        self.start_picker("test-picker multi 5", "Toggle items:")
        # Matches echo (5) and charlie (3); check 5. 4 and 1 are hidden but
        # stay pre-selected.
        self.child.send("/ch\r \r")
        self.finish("selected: 5", "selected: 4", "selected: 1")

    def test_process_attach_picker(self):
        """`process attach` without a pid, name, or target opens the picker."""
        self.start_picker("process attach", "Select a process to attach to:")
        self.child.send(self.escape_key)
        self.finish("no process selected")

    def start_stopped_picker(self, command, header):
        """Stop in `inner`, called from `outer` and `main`, then run
        `command`."""
        self.build()
        self.start_picker(
            command,
            header,
            executable=self.getBuildArtifact("a.out"),
            setup=[
                ("breakpoint set -n inner", ["Breakpoint 1"]),
                ("run", ["stop reason = breakpoint 1"]),
            ],
        )

    def test_frame_select_picker(self):
        """`frame select` without an index picks a frame, starting from the
        selected one."""
        self.start_stopped_picker("frame select", "Select a frame:")
        # The preview shows the source of the frame under the cursor.
        self.child.expect_exact("// break here")
        self.child.send("j")
        self.child.expect_exact("return inner(x) * 2;")
        self.child.send("\r")
        self.finish("frame #1", "outer")

    def test_frame_select_picker_cancel(self):
        """Canceling the frame picker keeps the selected frame."""
        self.start_stopped_picker("frame select", "Select a frame:")
        self.child.send(self.escape_key)
        self.finish("no frame selected")

    def test_frame_select_picker_resize(self):
        """Every resize redraws the picker. `frame select` holds the target's
        API lock while the picker waits, and redrawing the statusline on resize
        mustn't wait for it."""
        self.build()
        self.start_picker(
            "frame select",
            "Select a frame:",
            executable=self.getBuildArtifact("a.out"),
            setup=[
                ("settings set show-statusline true", None),
                ("breakpoint set -n inner", ["Breakpoint 1"]),
                ("run", ["stop reason = breakpoint 1"]),
            ],
        )
        for rows, cols in [(24, 100), (30, 120), (24, 120)]:
            self.child.setwinsize(rows, cols)
            self.child.expect_exact("Select a frame:")
        self.child.send("j\r")
        self.finish("frame #1", "outer")

    def test_thread_select_picker(self):
        """`thread select` without an index or ID picks a thread."""
        self.start_stopped_picker("thread select", "Select a thread:")
        self.child.send(self.escape_key)
        self.finish("no thread selected")

    def test_target_select_picker(self):
        """`target select` without an index picks a target, starting from the
        selected one."""
        self.build()
        exe = self.getBuildArtifact("a.out")
        self.start_picker(
            "target select",
            "Select a target:",
            setup=[
                (f"target create {exe}", ["Current executable set"]),
                (f"target create {exe}", ["Current executable set"]),
            ],
        )
        # The second target is selected, so k moves to the first one.
        self.child.send("k\r")
        self.finish("* target #0")

    def test_platform_select_picker(self):
        """`platform select` without a name picks a platform."""
        self.start_picker("platform select", "Select a platform:")
        self.child.send(self.escape_key)
        self.finish("no platform selected")
