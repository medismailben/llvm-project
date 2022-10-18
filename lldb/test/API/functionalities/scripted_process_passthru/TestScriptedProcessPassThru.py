"""
Test python scripted process in lldb
"""

import os, json, tempfile

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil
from lldbsuite.test import lldbtest

class ScriptedProcesPassthruTestCase(TestBase):

    NO_DEBUG_INFO_TESTCASE = True

    def setUp(self):
        TestBase.setUp(self)

    def tearDown(self):
        TestBase.tearDown(self)

    def get_module_with_name(self, target, name):
        for module in target.modules:
            if name in module.GetFileSpec().GetFilename():
                return module
        return None

    @skipUnlessDarwin
    @skipIfOutOfTreeDebugserver
    @skipIfRemote
    def test_launch_scripted_process_stack_frames(self):
        """Test that we can launch an interactive scripted process that is
        mocking another real process."""
        self.build()
        target = self.dbg.CreateTarget(self.getBuildArtifact("a.out"))
        self.assertTrue(target, VALID_TARGET)

        main_module = self.get_module_with_name(target, 'a.out')
        self.assertTrue(main_module, "Invalid main module.")
        error = target.SetModuleLoadAddress(main_module, 0)
        self.assertSuccess(error, "Reloading main module at offset 0 failed.")

        os.environ['SKIP_SCRIPTED_PROCESS_LAUNCH'] = '1'
        def cleanup():
          del os.environ["SKIP_SCRIPTED_PROCESS_LAUNCH"]
        self.addTearDownHook(cleanup)

        scripted_process_example_relpath = 'scripted_process_passthru.py'
        self.runCmd("command script import " + os.path.join(self.getSourceDir(),
                                                            scripted_process_example_relpath))

        parent_target = self.dbg.CreateTarget(None)
        self.assertTrue(corefile_process, PROCESS_IS_VALID)

        structured_data = lldb.SBStructuredData()
        structured_data.SetFromJSON(json.dumps({
            "backing_target_idx" : self.dbg.GetIndexOfTarget(corefile_process.GetTarget())
        }))
        launch_info = lldb.SBLaunchInfo(None)
        launch_info.SetProcessPluginName("ScriptedProcess")
        launch_info.SetScriptedProcessClassName("scripted_process_passthru.ScriptedProcessPassthru")
        launch_info.SetScriptedProcessDictionary(structured_data)

        error = lldb.SBError()
        process = target.Launch(launch_info, error)
        self.assertSuccess(error)
        self.assertTrue(process, PROCESS_IS_VALID)
        self.assertEqual(process.GetProcessID(), 42)

        self.assertEqual(process.GetNumThreads(), 2)
        thread = process.GetSelectedThread()
        self.assertTrue(thread, "Invalid thread.")
        self.assertEqual(thread.GetName(), "ScriptedThreadPassthru.thread-1")

        self.assertTrue(target.triple, "Invalid target triple")
        arch = target.triple.split('-')[0]
        supported_arch = ['x86_64', 'arm64', 'arm64e']
        self.assertIn(arch, supported_arch)
        # When creating a corefile of a arm process, lldb saves the exception
        # that triggers the breakpoint in the LC_NOTES of the corefile, so they
        # can be reloaded with the corefile on the next debug session.
        if arch in 'arm64e':
            self.assertTrue(thread.GetStopReason(), lldb.eStopReasonException)
        # However, it's architecture specific, and corefiles made from intel
        # process don't save any metadata to retrieve to stop reason.
        # To mitigate this, the StackCoreScriptedProcess will report a
        # eStopReasonSignal with a SIGTRAP, mimicking what debugserver does.
        else:
            self.assertTrue(thread.GetStopReason(), lldb.eStopReasonSignal)

        self.assertEqual(thread.GetNumFrames(), 5)
        frame = thread.GetSelectedFrame()
        self.assertTrue(frame, "Invalid frame.")
        func = frame.GetFunction()
        self.assertTrue(func, "Invalid function.")

        self.assertIn("baz", frame.GetFunctionName())
        self.assertEqual(frame.vars.GetSize(), 2)
        self.assertEqual(int(frame.vars.GetFirstValueByName('j').GetValue()), 42 * 42)
        self.assertEqual(int(frame.vars.GetFirstValueByName('k').GetValue()), 42)

        corefile_dylib = self.get_module_with_name(corefile_target, 'libbaz.dylib')
        self.assertTrue(corefile_dylib, "Dynamic library libbaz.dylib not found.")
        scripted_dylib = self.get_module_with_name(target, 'libbaz.dylib')
        self.assertTrue(scripted_dylib, "Dynamic library libbaz.dylib not found.")
        self.assertEqual(scripted_dylib.GetObjectFileHeaderAddress().GetLoadAddress(target),
                         corefile_dylib.GetObjectFileHeaderAddress().GetLoadAddress(target))
