"""
Test the functionality of scripted processes
"""



import lldb
import lldbsuite.test.lldbutil as lldbutil
from lldbsuite.test.lldbtest import *
import os

class TestInteractiveScriptedProcess(TestBase):

    NO_DEBUG_INFO_TESTCASE = True

    def test_passthrough_launch(self):
        """Test a simple pass-through process launch"""
        self.build()
        self.runCmd("file " + self.getBuildArtifact("a.out"), CURRENT_EXECUTABLE_SET)
        self.main_source_file = lldb.SBFileSpec("main.cpp")
        self.script_module = "interactive_scripted_process"
        self.script_file = self.script_module + ".py"
        self.passthrough_launch()

    def duplicate_target(self, driving_target):
        exe = driving_target.executable.fullpath
        triple = driving_target.triple
        return self.dbg.CreateTargetWithFileAndTargetTriple(exe, triple)

    def get_launch_info(self, class_name, script_dict):
        structured_data = lldb.SBStructuredData()
        structured_data.SetFromJSON(json.dumps(script_dict))

        launch_info = lldb.SBLaunchInfo(None)
        launch_info.SetProcessPluginName("ScriptedProcess")
        launch_info.SetScriptedProcessClassName(class_name)
        launch_info.SetScriptedProcessDictionary(structured_data)
        return launch_info

    def passthrough_launch(self):
        """Test that a simple passthrough wrapper functions correctly"""
        # First build the real target:
        lldbutil.run_break_set_by_source_regexp(self, "Break here")
        self.assertEqual(self.dbg.GetNumTargets(), 1)
        real_target_id = 0
        real_target = self.dbg.GetTargetAtIndex(real_target_id)

        # Now source in the scripted module:
        script_path = os.path.join(self.getSourceDir(), self.script_file)
        self.runCmd(f"command script import '{script_path}'")

        scripted_target = self.duplicate_target(real_target)
        self.assertTrue(scripted_target.IsValid(), "duplicate target succeeded")

        mux_class = self.script_module + "." + "MultiplexerScriptedProcess"
        script_dict = {'driving_target_idx' : real_target_id}
        mux_launch_info = self.get_launch_info(mux_class, script_dict)

        self.dbg.SetAsync(True)
        error = lldb.SBError()
        mux_process = scripted_target.Launch(mux_launch_info, error)
        self.assertSuccess(error, "Launched scripted process")

        self.assertTrue(mux_process.IsValid(), "Got a valid process")
        self.assertState(mux_process.GetState(), lldb.eStateStopped, "Process is stopped")

        real_process = real_target.GetProcess()
        self.assertTrue(real_process.IsValid(), "Got a valid process")
        self.assertState(real_process.GetState(), lldb.eStateStopped, "Process is stopped")

        # This is a passthrough, so the two processes should have the same state:
        # Check that we got the right threads:
        self.assertEqual(len(real_process.threads), len(mux_process.threads), "Same number of threads")
        for id in range(0, len(real_process.threads)):
            real_pc = real_process.threads[id].frame[0].pc
            mux_pc = mux_process.threads[id].frame[0].pc
            self.assertEqual(real_pc, mux_pc, f"PC's equal for {id}")


