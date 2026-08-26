"""
Test the `instruction` command family.

These cover the four subcommands that report what the disassembler knows about
an instruction: `info`, `relocate`, `reach` and `patch-site`. They are the
observable form of the questions the code that patches a site out of line asks,
so the assertions here are about the answers, not about the formatting: an
address that is rewritten, a refusal that names the right reason, a verdict that
follows from the checks above it.

The test suite turns color off, so all but the coloring test read the plain
output.
"""

import re

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class InstructionCommandTestCase(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

    def make_target(self):
        """A target with no process.

        Everything except patch-site answers from the module on disk, and
        launching would only add a trap to the memory these commands read back.
        """
        self.build()
        target = self.dbg.CreateTarget(self.getBuildArtifact("a.out"))
        self.assertTrue(target, VALID_TARGET)
        return target

    def instructions_in_main(self, target):
        main = target.FindFunctions("main").GetContextAtIndex(0).GetFunction()
        self.assertTrue(main, "no main to disassemble")
        return list(main.GetInstructions(target))

    def find_instruction(self, target, predicate, description):
        """The first instruction in main that satisfies \a predicate.

        Searched for rather than hardcoded: which line holds a call and what the
        compiler emits around it are both free to change, and an address baked
        into the test would then describe some other instruction entirely while
        still looking like it passed.
        """
        for instruction in self.instructions_in_main(target):
            if predicate(instruction):
                return instruction
        self.fail("no %s in main" % description)

    def address_of(self, target, instruction):
        """The address to hand the command.

        A target with no process has no load addresses, and the commands fall
        back to file addresses in exactly the same way, so whichever domain the
        target is in is the one the test has to ask about.
        """
        address = instruction.GetAddress().GetLoadAddress(target)
        if address == lldb.LLDB_INVALID_ADDRESS:
            address = instruction.GetAddress().GetFileAddress()
        self.assertNotEqual(address, lldb.LLDB_INVALID_ADDRESS)
        return address

    def call_instruction(self, target):
        return self.find_instruction(
            target,
            lambda i: i.DoesBranch() and i.GetMnemonic(target) == "bl",
            "call instruction",
        )

    def run_command(self, command):
        self.runCmd(command)
        return self.res.GetOutput()

    def line_starting_with(self, output, prefix):
        """The single line of \a output that begins with \a prefix."""
        matches = [
            line for line in output.splitlines() if line.strip().startswith(prefix)
        ]
        self.assertEqual(
            len(matches),
            1,
            "expected exactly one '%s' line in:\n%s" % (prefix, output),
        )
        return matches[0]

    def assertEveryRequirementMet(self, output):
        """No requirement line carries the fail mark.

        The mark is a checkmark or an ASCII column depending on what the
        terminal can render, so both spellings have to be ruled out. Each is
        printed in its own two-space-padded column, which is what keeps this
        from matching an 'x' in the surrounding prose.
        """
        for fail_mark in ["  ✘  ", "  x  "]:
            self.assertNotIn(
                fail_mark,
                output,
                "a requirement was not met:\n%s" % output,
            )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_info_distinguishes_a_call_from_an_ordinary_instruction(self):
        """`info` reports the properties the disassembler was asked for.

        A plugin that answered every question the same way would still print a
        well formed report, so the two instructions are described in the same
        run and their answers are required to differ.
        """
        target = self.make_target()

        call = self.call_instruction(target)
        # adr, adrp and the literal loads are the non-branch forms that still
        # name an address, and the point of this half is an instruction that
        # names nothing at all.
        ordinary = self.find_instruction(
            target,
            lambda i: not i.DoesBranch()
            and not i.GetMnemonic(target).startswith(("adr", "ldr", "prfm")),
            "non-branch instruction",
        )

        self.expect(
            "instruction info %#x" % self.address_of(target, ordinary),
            substrs=[
                "size            4 bytes",
                "branches        no",
                "call            no",
                "position        independent",
            ],
        )
        # A non-branch has nothing to refer to, so the line is left out rather
        # than printed empty.
        self.assertNotIn(
            "refers to",
            self.res.GetOutput(),
            "an ordinary instruction refers to something",
        )

        self.expect(
            "instruction info %#x" % self.address_of(target, call),
            substrs=[
                "size            4 bytes",
                "branches        yes",
                "call            yes",
                "return          no",
                "position        may depend on the pc",
                # The call has to name what it calls, since that is the address
                # a relocated copy must keep referring to.
                "refers to       %#x" % int(call.GetOperands(target), 16),
                "relocatable     yes",
            ],
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_info_count_describes_several_instructions(self):
        """--count describes that many instructions, not just the first."""
        target = self.make_target()
        instructions = self.instructions_in_main(target)
        self.assertGreater(len(instructions), 2)

        output = self.run_command(
            "instruction info -c 2 %#x" % self.address_of(target, instructions[0])
        )
        for instruction in instructions[:2]:
            self.assertIn("%#x" % self.address_of(target, instruction), output)
        self.assertEqual(
            output.count("size            4 bytes"),
            2,
            "--count 2 did not describe two instructions:\n%s" % output,
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_relocate_rewrites_a_call_to_keep_its_target(self):
        """A call moved a short distance still calls the same function.

        This is the assertion the rest of the file is built around, because it
        is evidence rather than a claim: the relocated bytes are disassembled
        back at the destination, and a copy that had merely been memcpy'd there
        would decode as a call to somewhere else. The displacement is encoded in
        the instruction, so keeping the same target across a move is only
        possible if the operand was re-encoded.
        """
        target = self.make_target()
        call = self.call_instruction(target)
        origin = self.address_of(target, call)
        callee = int(call.GetOperands(target), 16)

        # Far enough to change the encoding, near enough to stay in range.
        destination = origin + 16
        output = self.run_command("instruction relocate %#x -o 16" % origin)

        self.assertIn("relocatable     yes", output)
        self.assertIn("moves to        %#x" % destination, output)
        self.assertIn("relocated code  ", output)

        reads_as = self.line_starting_with(output, "reads as")
        self.assertIn(
            "%#x" % destination,
            reads_as,
            "the relocated bytes were not decoded at the destination:\n%s" % output,
        )
        self.assertIn(
            "bl %#x" % callee,
            reads_as,
            "the relocated call no longer targets %#x, so its displacement was "
            "copied rather than re-encoded:\n%s" % (callee, output),
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_relocate_refuses_a_destination_out_of_range(self):
        """A call cannot be moved further from its target than it can reach.

        The refusal has to be the second tier: a relocated form of this call
        does exist, it is this destination that does not work, and a caller that
        cannot tell the two apart would give up on a site that a nearer
        trampoline would have handled.
        """
        target = self.make_target()
        origin = self.address_of(target, self.call_instruction(target))

        # A branch reaches +/-128MiB, so 256MiB away is out of range no matter
        # which direction the callee lies in.
        output = self.run_command("instruction relocate %#x -o %d" % (origin, 1 << 28))

        self.assertIn("relocatable     yes", output)
        self.assertIn("refused         ", output)
        self.assertNotIn(
            "no, at any address",
            output,
            "an out-of-range destination was reported as a form that can never "
            "be relocated:\n%s" % output,
        )
        self.assertNotIn(
            "reads as",
            output,
            "a refused relocation still produced code:\n%s" % output,
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_relocate_refuses_a_form_with_no_relocated_encoding(self):
        """adrp has no out of line form, at any address.

        The first tier of refusal. It is reported before a destination is even
        considered, so moving the trampoline cannot help and the wording has to
        say so.
        """
        target = self.make_target()
        adrp = self.find_instruction(
            target, lambda i: i.GetMnemonic(target) == "adrp", "adrp"
        )

        output = self.run_command(
            "instruction relocate %#x -o 16" % self.address_of(target, adrp)
        )

        self.assertIn("relocatable     no, at any address", output)
        self.assertNotIn(
            "refused",
            output,
            "a form with no relocated encoding was reported as a destination "
            "problem:\n%s" % output,
        )
        self.assertNotIn("moves to", output)

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_relocate_requires_a_destination(self):
        """Neither --to nor --offset is an error rather than a default."""
        target = self.make_target()
        origin = self.address_of(target, self.call_instruction(target))
        self.expect(
            "instruction relocate %#x" % origin,
            error=True,
            substrs=["a destination is required"],
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_reach_reports_the_range_of_a_branch(self):
        """The default form is the unconditional branch, which reaches 128MiB."""
        target = self.make_target()
        origin = self.address_of(target, self.call_instruction(target))

        self.expect(
            "instruction reach %#x" % origin,
            substrs=[
                "branch (b, bl)",
                "encoding        signed 26 bit immediate scaled by 4",
                "measured from   %#x (the instruction itself)" % origin,
                "reach           +/-128MiB",
                "reaches         ",
                "alignment       targets must be 4 byte aligned",
            ],
        )
        # No target was named, so no verdict about one may be printed.
        self.assertNotIn("in reach", self.res.GetOutput())

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_reach_answers_about_a_particular_target(self):
        """--target is answered with the displacement, and by how much it misses."""
        target = self.make_target()
        origin = self.address_of(target, self.call_instruction(target))

        self.expect(
            "instruction reach %#x -t %#x" % (origin, origin + 16),
            substrs=[
                "target          %#x  in reach, displacement +16 bytes"
                % (origin + 16)
            ],
        )

        output = self.run_command(
            "instruction reach %#x -t %#x" % (origin, origin + (1 << 28))
        )
        # The forward limit is the largest multiple of four a signed 26 bit
        # immediate can name, which is one instruction short of 128MiB, so the
        # shortfall is stated rather than approximated.
        forward_limit = ((1 << 25) - 1) * 4
        self.assertIn(
            "out of reach, %d bytes too far forward" % ((1 << 28) - forward_limit),
            output,
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_reach_refuses_a_form_this_architecture_does_not_have(self):
        """A form from another architecture is refused, not answered.

        Reach is a property of an encoding, so an answer borrowed from x86-64's
        rel8 would be believed and would be wrong. The refusal lists what this
        architecture does have, so the question can be asked again.
        """
        target = self.make_target()
        origin = self.address_of(target, self.call_instruction(target))

        self.expect(
            "instruction reach %#x -k short" % origin,
            error=True,
            substrs=[
                "no 'short' form",
                "it has: branch, conditional, test-branch, literal, address, page",
            ],
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_patch_site_reports_a_site_that_can_be_patched(self):
        """A call site can be patched, and an adrp site cannot.

        Both verdicts come from one run, because a report that always said yes
        would satisfy the first half on its own.

        This is the one subcommand that needs a live, stopped process: one of
        the requirements is that no thread is executing inside the range, which
        is a question about threads, and the analysis reads the code that is
        mapped rather than the code on disk.
        """
        self.build()
        target, process, _, _ = lldbutil.run_to_name_breakpoint(self, "main")

        # The analysis disassembles the function out of live memory, so leaving
        # the trap in would have it describe the breakpoint rather than the
        # program. The process stays stopped where it is.
        target.DeleteAllBreakpoints()

        call = self.call_instruction(target)
        site = self.address_of(target, call)
        self.assertNotEqual(
            site,
            process.GetSelectedThread().GetFrameAtIndex(0).GetPC(),
            "the process is stopped on the site under test",
        )

        output = self.run_command("instruction patch-site %#x" % site)
        self.assertIn(
            "patch size      4 bytes (the width of this ABI's branch)", output
        )
        self.assertIn(
            "the branch displaces whole instructions, and each of them can run "
            "out of line",
            output,
        )
        self.assertIn(
            "nothing in the function branches into the bytes the branch would "
            "overwrite",
            output,
        )
        self.assertIn(
            "no thread is stopped inside the bytes the branch would overwrite", output
        )
        self.assertEveryRequirementMet(output)

        # The displaced instruction is the call itself, and it is the pc
        # relative one, which is what decides between copying it and rewriting
        # it.
        self.assertIn("displaces       4 bytes, 4 bytes of relocated code", output)
        self.assertIn("may depend on the pc, 4 bytes relocated", output)

        self.assertIn(
            "4 bytes at %#x can be replaced with a branch." % site,
            output,
            "a patchable site was refused:\n%s" % output,
        )

        # And the negative. adrp has no out of line form, so the site fails the
        # first requirement and the verdict has to follow it.
        adrp = self.find_instruction(
            target, lambda i: i.GetMnemonic(target) == "adrp", "adrp"
        )
        adrp_site = self.address_of(target, adrp)
        output = self.run_command("instruction patch-site %#x" % adrp_site)
        self.assertIn("cannot be moved out of line", output)
        self.assertIn(
            "4 bytes at %#x cannot be replaced with a branch." % adrp_site,
            output,
            "a site holding an instruction that cannot run out of line was "
            "accepted:\n%s" % output,
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_register_live_tells_a_read_from_a_write(self):
        """A register a later instruction reads is live; one it overwrites is not.

        Both directions come from one run, because a report that always said
        live would satisfy the first half on its own, and a report that always
        said dead would satisfy the second.

        Needs a live process: the walk reads the code that is mapped, and
        deciding whether a callee may clobber a register needs the ABI this
        process runs under.
        """
        self.build()
        target, _, _, _ = lldbutil.run_to_name_breakpoint(self, "main")
        target.DeleteAllBreakpoints()

        # The instruction feeding the loop comparison: it writes a register that
        # the compare right after it reads back.
        compare = self.find_instruction(
            target,
            lambda i: i.GetMnemonic(target) == "subs",
            "compare",
        )
        reader = self.address_of(target, compare)

        register = compare.GetOperands(target).split(",")[0].strip()

        output = self.run_command(
            "instruction register-live -r %s %#x" % (register, reader)
        )
        self.assertIn("%s may still be needed here" % register, output)
        self.assertIn("read by the instruction", output)
        # The listing is what makes a live answer actionable, so it has to name
        # the instruction rather than only report the verdict.
        self.assertIn("is named by these instructions in 'main'", output)
        self.assertIn("reads", output)

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_register_live_sweeps_every_register_by_default(self):
        """With no arguments, report which registers are free.

        The answer is checked against the calling convention rather than
        against a captured list: on AAPCS64 x0 through x18 are caller saved and
        x19 through x28 are not, so in a leaf function, where the only path
        forward is the return, the split has to fall exactly there. A sweep that
        reported everything free would pass a weaker test and be useless.
        """
        self.build()
        target, _, _, _ = lldbutil.run_to_name_breakpoint(self, "helper")
        target.DeleteAllBreakpoints()

        output = self.run_command("instruction register-live")

        # Compared without the callee saved mark, which this test is not about.
        def names(line):
            return [name.rstrip("*") for name in line.split()[1:]]

        free = names(self.line_starting_with(output, "free"))
        live = names(self.line_starting_with(output, "live"))

        for caller_saved in ["x0", "x9", "x18"]:
            self.assertIn(caller_saved, free)
        for callee_saved in ["x19", "x28", "fp", "lr", "sp"]:
            self.assertIn(callee_saved, live)

        # A 32 bit half is the same register asked about twice, so only the
        # whole register is listed.
        self.assertNotIn("w0", output)

        # pc and the flags are named by the register context and not by the
        # disassembler, so they are reported as unanswerable rather than free.
        self.assertIn("pc", self.line_starting_with(output, "unknown"))

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_register_live_reports_the_calling_convention(self):
        """Say which side of the calling convention a register is on.

        Whether a register is callee saved is most of the reason it is
        unavailable, so reporting the verdict without it leaves the reader to
        look it up. The mark comes from the same authority the walk consults, so
        the two cannot disagree.
        """
        self.build()
        target, _, _, _ = lldbutil.run_to_name_breakpoint(self, "helper")
        target.DeleteAllBreakpoints()

        swept = self.run_command("instruction register-live")

        # In a leaf function nothing reads these, so they are unavailable purely
        # because their values belong to the caller, and the mark is what says
        # so.
        live = self.line_starting_with(swept, "live").split()
        self.assertIn("x19*", live)
        self.assertIn("x28*", live)
        self.assertIn("* callee saved", swept)

        # A caller saved register carries no mark, so the two groups stay
        # distinguishable within one list.
        free = self.line_starting_with(swept, "free").split()
        self.assertIn("x9", free)
        self.assertNotIn("x9*", free)

        # Naming one register states the convention in words rather than as a
        # mark, since there is no list to scan.
        live_detail = self.run_command("instruction register-live -r x20")
        self.assertIn(
            "x20 is callee saved, so its value belongs to this function's "
            "caller",
            live_detail,
        )

        free_detail = self.run_command("instruction register-live -r x9")
        self.assertIn(
            "x9 is caller saved, so a callee may clobber it anyway", free_detail
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_register_live_follows_the_selected_frame(self):
        """With no address, ask about the selected frame's pc.

        Going up a frame asks about that frame's return address, which is a
        different question with a different answer: the register holding the
        callee's result is live there, because the caller is about to use it,
        while it was free inside the callee.
        """
        self.build()
        target, process, _, _ = lldbutil.run_to_name_breakpoint(self, "helper")
        target.DeleteAllBreakpoints()

        in_callee = self.run_command("instruction register-live")
        self.assertIn("x0", self.line_starting_with(in_callee, "free").split())

        thread = process.GetSelectedThread()
        self.assertTrue(
            thread.SetSelectedFrame(1), "could not select the calling frame"
        )

        in_caller = self.run_command("instruction register-live")
        # The report is about the caller now, so it names a different address.
        self.assertNotEqual(
            in_callee.splitlines()[0],
            in_caller.splitlines()[0],
            "selecting the calling frame did not change the address reported",
        )
        self.assertIn(
            "x0",
            self.line_starting_with(in_caller, "live").split(),
            "the returned value is not live at the address it returns to",
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_register_live_respects_the_calling_convention(self):
        """A call ends the path for a volatile register, not for a saved one.

        The walk stops caring about a caller saved register once a path reaches
        a call, since the callee may destroy it anyway. A callee saved register
        is the opposite: the callee is entitled to hand it back untouched, so a
        patch that overwrote it before the call would be observable afterwards.

        Reporting a callee saved register dead is the direction that corrupts
        the program being debugged, which is why it is worth a test of its own.
        """
        self.build()
        target, _, _, _ = lldbutil.run_to_name_breakpoint(self, "main")
        target.DeleteAllBreakpoints()

        # A site before the call, so every path from it reaches one.
        call = self.call_instruction(target)
        site = self.address_of(target, call)

        # x9 through x15 are caller saved on AAPCS64, x19 through x28 are not.
        volatile = self.run_command("instruction register-live -r x9 %#x" % site)
        self.assertIn("x9 holds no value the program still needs here", volatile)

        saved = self.run_command("instruction register-live -r x19 %#x" % site)
        self.assertIn("x19 may still be needed here", saved)
        self.assertIn("a call that must preserve it", saved)

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_register_live_ends_a_path_at_a_return(self):
        """A return ends a path rather than reading as an unresolved branch.

        Which instructions end a path is decided with the predicates MC derives
        from the instruction description. The older classification was an x86
        opcode table that answered Unknown on AArch64, where it took a ret for a
        branch whose destination could not be named and reported every register
        live because of it.
        """
        self.build()
        target, _, _, _ = lldbutil.run_to_name_breakpoint(self, "main")
        target.DeleteAllBreakpoints()

        ret = self.find_instruction(
            target, lambda i: i.GetMnemonic(target) == "ret", "return"
        )

        # Just before the return, so the only path forward is off the end of the
        # function. A caller saved register cannot be needed there.
        site = self.address_of(target, ret) - 4

        output = self.run_command("instruction register-live -r x9 %#x" % site)
        self.assertIn("x9 holds no value the program still needs here", output)
        self.assertNotIn("unresolved branch", output)

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_patch_site_takes_a_size(self):
        """--size overrides the ABI's branch width, and is reported as such."""
        self.build()
        target, _, _, _ = lldbutil.run_to_name_breakpoint(self, "main")
        target.DeleteAllBreakpoints()

        site = self.address_of(target, self.call_instruction(target))
        output = self.run_command("instruction patch-site %#x -s 8" % site)

        self.assertIn("patch size      8 bytes (requested)", output)
        # Two instructions are displaced by eight bytes rather than the one a
        # four byte branch displaces.
        self.assertIn("displaces       8 bytes", output)

    def test_disassemble_is_reachable_both_ways(self):
        """`disassemble` and `instruction disassemble` are the same command.

        The top level spelling is the one every user and every script has, and
        moving the command under `instruction` must not have taken it away.
        """
        target = self.make_target()

        top_level = self.run_command("disassemble --name main")
        under_instruction = self.run_command("instruction disassemble --name main")

        self.assertIn("main", top_level)
        self.assertIn("main", under_instruction)
        self.assertEqual(
            top_level,
            under_instruction,
            "the two spellings disassembled differently",
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e"]))
    def test_instructions_are_colored_like_the_disassembler(self):
        """An operand is painted the same color here as by `disassemble`.

        These commands ask the disassembler for its colored spelling rather than
        painting operands themselves, so that reading `instruction info` and
        reading `disassemble` do not require learning two schemes. Nothing
        enforces that from the C++ side: a print site that assembled its own
        escape would still look reasonable on its own, and would only be caught
        by comparing the two commands, which is what this does.
        """
        target = self.make_target()
        call = self.call_instruction(target)
        site = self.address_of(target, call)

        # The suite turns color off for every other test, so this one turns it
        # back on for the commands it runs and puts it back afterwards.
        self.runCmd("settings set use-color true")
        self.addTearDownHook(
            lambda: self.runCmd("settings set use-color false", check=False)
        )

        disassembled = self.run_command("disassemble -c 1 -s %#x" % site)
        info = self.run_command("instruction info %#x" % site)
        relocated = self.run_command("instruction relocate %#x -o 16" % site)

        # The operand as the disassembler paints it, escapes and all. Captured
        # from `disassemble` rather than written out, so that this keeps testing
        # that the two agree even if the disassembler's own color changes.
        callee = call.GetOperands(target)
        colored = re.search(
            r"(\x1b\[[0-9;]*m)+" + re.escape(callee) + r"(\x1b\[[0-9;]*m)+",
            disassembled,
        )
        self.assertTrue(
            colored,
            "`disassemble` did not color the branch target, so there is no "
            "reference coloring to match:\n%r" % disassembled,
        )

        for command, output in [("info", info), ("relocate", relocated)]:
            self.assertIn(
                colored.group(0),
                output,
                "`instruction %s` did not paint %s the way `disassemble` does, "
                "so it is coloring operands itself instead of asking the "
                "disassembler:\n%r" % (command, callee, output),
            )

        # And color off stays plain, so no escape can leak into the output the
        # rest of this file matches against.
        self.runCmd("settings set use-color false")
        plain = self.run_command("instruction info %#x" % site)
        self.assertNotIn(
            "\x1b[",
            plain,
            "an escape sequence survived `use-color false`:\n%r" % plain,
        )
