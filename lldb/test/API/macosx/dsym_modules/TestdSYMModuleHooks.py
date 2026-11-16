"""
Test when LLDB runs the hooks of a Python module in a dSYM.
"""

import os, shutil

import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
from lldbsuite.test import lldbutil


@requireDarwin
class TestdSYMModuleHooks(TestBase):
    SHARED_BUILD_TESTCASE = False
    COUNTS = (
        "module_hooks_inits",
        "module_hooks_targets",
        "read_header_runs",
        "read_header_magic",
        "read_process_results",
    )
    MH_MAGIC_64 = 0xFEEDFACF

    def setUp(self):
        TestBase.setUp(self)
        for name in self.COUNTS:
            if hasattr(lldb, name):
                delattr(lldb, name)

    def counts(self):
        return (
            getattr(lldb, "module_hooks_inits", 0),
            getattr(lldb, "module_hooks_targets", 0),
        )

    @no_debug_info_test
    def test_scripts_run_once_per_target(self):
        """The module is seen again when the process loads it and gives it a
        load address, but its script only runs once for the target."""
        self.runCmd("settings set target.load-script-from-symbol-file true")
        self.build_dsym("has_hooks", "module_hooks.py")
        log = self.enable_script_log()
        lldbutil.run_to_source_breakpoint(
            self,
            "Set a breakpoint here",
            lldb.SBFileSpec("main.c"),
            exe_name="has_hooks",
        )
        self.assertEqual(self.counts(), (1, 1))
        self.assertRegex(
            self.read_script_log(log),
            r"Skipping '.*' for '.*': this target already loaded it",
        )

    @no_debug_info_test
    def test_one_shot_stop_hook_reads_module(self):
        """A one-shot stop hook that __lldb_module_added_to_target adds reads
        the module from memory at the first stop where it is loaded, and LLDB
        deletes it afterwards."""
        self.runCmd("settings set target.load-script-from-symbol-file true")
        self.build_dsym("reads_header", "read_header_hook.py")
        lldbutil.run_to_source_breakpoint(
            self,
            "Set a breakpoint here",
            lldb.SBFileSpec("main.c"),
            exe_name="reads_header",
        )
        self.assertEqual(getattr(lldb, "read_header_magic", None), self.MH_MAGIC_64)
        self.expect("target stop-hook list", substrs=["No stop hooks."])

    @no_debug_info_test
    def test_script_reads_core_file(self):
        """LLDB loads a module's dSYM script once the core file is loaded, so
        __lldb_module_added_to_target can read the process."""
        self.build_dsym("reads_process", "read_process.py")
        target, process, _, _ = lldbutil.run_to_source_breakpoint(
            self,
            "Set a breakpoint here",
            lldb.SBFileSpec("main.c"),
            exe_name="reads_process",
        )
        core = self.getBuildArtifact("reads_process.core")
        self.runCmd(f"process save-core {core}")
        process.Kill()
        self.dbg.DeleteTarget(target)

        self.runCmd("settings set target.load-script-from-symbol-file true")
        target = self.dbg.CreateTarget(None)
        self.assertTrue(target.LoadCore(core), PROCESS_IS_VALID)
        self.assertEqual(getattr(lldb, "read_process_results", None), [True])

    def enable_script_log(self):
        log = self.getBuildArtifact("script.log")
        self.runCmd(f"log enable -f {log} lldb script")
        return log

    def read_script_log(self, log):
        self.runCmd("log disable lldb script")
        with open(log) as f:
            return f.read()

    def build_dsym(self, name, script):
        self.build(debug_info="dsym", dictionary={"EXE": name})
        executable = self.getBuildArtifact(name)
        python_dir_path = os.path.join(
            self.getBuildArtifact(name + ".dSYM"), "Contents", "Resources", "Python"
        )
        if not os.path.exists(python_dir_path):
            os.mkdir(python_dir_path)
        shutil.copy(
            os.path.join(self.getSourceDir(), script),
            os.path.join(python_dir_path, name + ".py"),
        )
        return executable
