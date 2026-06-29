# TestPlaygrounds.py
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ------------------------------------------------------------------------------
"""
Test that playgrounds work
"""
import subprocess
import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil
import os
import os.path
import platform
from lldbsuite.test.builders.darwin import get_triple

import sys
if sys.version_info.major == 2:
    import commands as subprocess
else:
    import subprocess


def execute_command(command):
    (exit_status, output) = subprocess.getstatusoutput(command)
    return exit_status


class TestSwiftPlaygrounds(TestBase):
    def get_build_triple(self):
        """We want to build the file with a deployment target earlier than the
           availability set in the source file."""
        if lldb.remote_platform:
            arch = self.getArchitecture()
            vendor, os, version, _ = get_triple()
            # Use ABI-stable deployment targets so modern Swift doesn't require
            # the legacy layouts-arm64.yaml file.  The version must still be
            # below the @available annotation in Contents.swift so that
            # test_no_force_target triggers the expected availability failure.
            # Swift 6's minimum deployment targets are iOS 13 / watchOS 7.
            if os == 'watchos':
                version = '7.0'
            else:
                version = '13.0'
            triple = '{}-{}-{}{}'.format(arch, vendor, os, version)
        else:
            triple = '{}-apple-macosx11.0'.format(platform.machine())
        return triple

    def get_run_triple(self):
        if lldb.remote_platform:
            arch = self.getArchitecture()
            vendor, os, version, _ = get_triple()
            triple = '{}-{}-{}{}'.format(arch, vendor, os, version)
        else:
            version, _, machine = platform.mac_ver()
            triple = '{}-apple-macosx{}'.format(machine, version)
        return triple

    @skipEmbeddedSwift
    @skipUnlessDarwin
    @swiftTest
    @skipIf(setting=('symbols.use-swift-clangimporter', 'false'))
    @skipIf(debug_info=decorators.no_match("dsym"))
    def test_force_target(self):
        """Test that playgrounds work"""
        self.launch(True)
        self.do_basic_test(True)

    @skipEmbeddedSwift
    @skipUnlessDarwin
    @swiftTest
    @skipIf(setting=('symbols.use-swift-clangimporter', 'false'))
    @skipIf(debug_info=decorators.no_match("dsym"))
    def test_no_force_target(self):
        """Test that playgrounds work"""
        self.launch(False)
        self.do_basic_test(False)

    @skipEmbeddedSwift
    @skipUnlessDarwin
    @swiftTest
    @skipIf(setting=('symbols.use-swift-clangimporter', 'false'))
    @skipIf(debug_info=decorators.no_match("dsym"))
    @skipIf(macos_version=["<", "12"])
    def test_concurrency(self):
        """Test that concurrency is available in playgrounds"""
        self.launch(True)
        self.do_concurrency_test()

    @skipEmbeddedSwift
    @skipUnlessDarwin
    @swiftTest
    @skipIf(setting=('symbols.use-swift-clangimporter', 'false'))
    @skipIf(debug_info=decorators.no_match("dsym"))
    def test_import(self):
        """Test that a dylib can be imported in playgrounds"""
        self.launch(True)
        self.do_import_test()
        
    def launch(self, force_target):
        """Test that playgrounds work"""
        self.build(dictionary={
            'TARGET_SWIFTFLAGS':
            '-target {}'.format(self.get_build_triple()),
        })

        # Create the target
        exe = self.getBuildArtifact("PlaygroundStub")
        if force_target:
            target = self.dbg.CreateTargetWithFileAndArch(
                exe, self.get_run_triple())
        else:
            target = self.dbg.CreateTarget(exe)

        self.assertTrue(target, VALID_TARGET)
        # registerSharedLibrariesWithTarget uploads dylibs to the remote device
        # and returns the DYLD_LIBRARY_PATH entry needed to find them.
        env = self.registerSharedLibrariesWithTarget(target,
                                                     ['libPlaygroundsRuntime.dylib'])

        if lldb.remote_platform:
            # PlaygroundStub links AuxSources.framework with install name
            # @executable_path/AuxSources.framework/Versions/A/AuxSources.
            # The executable lands in the test-specific working directory that
            # setUp() set, so upload the framework relative to that same dir.
            exe_dir = lldb.remote_platform.GetWorkingDirectory()
            fw_build = self.getBuildArtifact("AuxSources.framework")
            local_bin = os.path.realpath(os.path.join(fw_build, "AuxSources"))
            remote_fw  = lldbutil.join_remote_paths(exe_dir, "AuxSources.framework")
            remote_ver = lldbutil.join_remote_paths(remote_fw, "Versions")
            remote_a   = lldbutil.join_remote_paths(remote_ver, "A")
            for d in (remote_fw, remote_ver, remote_a):
                lldb.remote_platform.MakeDirectory(d, 0o700)
            remote_bin = lldbutil.join_remote_paths(remote_a, "AuxSources")
            err = lldb.remote_platform.Put(
                lldb.SBFileSpec(local_bin, True),
                lldb.SBFileSpec(remote_bin, False),
            )
            self.assertFalse(err.Fail(), "Failed to upload AuxSources.framework: %s" % err)

        # Set the breakpoints
        breakpoint = target.BreakpointCreateByName('break_here')
        self.assertTrue(breakpoint.GetNumLocations() > 0, VALID_BREAKPOINT)

        # On remote platforms the host cwd does not exist on the device;
        # use the platform working directory instead.
        if lldb.remote_platform:
            wd = lldb.remote_platform.GetWorkingDirectory()
        else:
            wd = os.getcwd()

        process = target.LaunchSimple(None, env, wd)
        self.assertTrue(process, PROCESS_IS_VALID)

        threads = lldbutil.get_threads_stopped_at_breakpoint(
            process, breakpoint)

        self.assertEqual(len(threads), 1)
        # The expression compiler runs on the host — always use the local build
        # dir so it can resolve framework swiftmodules regardless of whether we
        # are testing against a remote device.
        self.expect('settings set target.swift-framework-search-paths "%s"' %
                    self.getBuildDir())

    def execute_code(self, input_file, expect_error=False):
        contents = "syntax error"
        with open(input_file, 'r') as contents_file:
            contents = contents_file.read()

        options = lldb.SBExpressionOptions()
        options.SetLanguage(lldb.eLanguageTypeSwift)
        options.SetPlaygroundTransformEnabled()
        # The concurrency expressions will spawn multiple threads.
        options.SetOneThreadTimeoutInMicroSeconds(1)
        options.SetTryAllThreads(True)
        options.SetAutoApplyFixIts(False)

        res = self.frame().EvaluateExpression(contents, options)

        options = lldb.SBExpressionOptions()
        options.SetLanguage(lldb.eLanguageTypeSwift)
        self.frame().EvaluateExpression("import PlaygroundsRuntime", options)
        ret = self.frame().EvaluateExpression("get_output()", options)
        is_error = res.GetError().Fail() and not (
                     res.GetError().GetType() == 1 and
                     res.GetError().GetError() == 0x1001)
        playground_output = ret.GetSummary()
        with recording(self, self.TraceOn()) as sbuf:
            print("playground result: ", file=sbuf)
            print(str(res), file=sbuf)
            if is_error:
                print("error:", file=sbuf)
                print(str(res.GetError()), file=sbuf)
            else:
                print("playground output:", file=sbuf)
                print(str(ret), file=sbuf)

        if expect_error:
            self.assertTrue(is_error)
            return playground_output
        self.assertFalse(is_error)
        self.assertIsNotNone(playground_output)
        return playground_output
        
    def do_basic_test(self, force_target):
        playground_output = self.execute_code('Contents.swift', not force_target)
        if not force_target:
            # This is expected to fail because the deployment target
            # is less than the availability of the function being
            # called.
            self.assertEqual(playground_output, '""')
            return

        self.assertIn("a=\\'3\\'", playground_output)
        self.assertIn("b=\\'5\\'", playground_output)
        self.assertIn("=\\'8\\'", playground_output)
        self.assertIn("=\\'11\\'", playground_output)

    def do_concurrency_test(self):
        playground_output = self.execute_code('Concurrency.swift')
        self.assertIn("=\\'23\\'", playground_output)

    def do_import_test(self):
        # Test importing a library that adds new Clang options.
        if lldb.remote_platform:
            # AuxSources.framework is already on the device (uploaded in
            # launch()).  Only Dylib.framework needs to be uploaded and
            # pre-loaded here so the JIT linker finds its symbols.
            root_wd = configuration.lldb_platform_working_dir
            fw_build = self.getBuildArtifact("Dylib.framework")
            remote_fw = lldbutil.join_remote_paths(root_wd, "Dylib.framework")
            lldb.remote_platform.MakeDirectory(remote_fw, 0o700)
            local_bin = os.path.realpath(os.path.join(fw_build, "Dylib"))
            remote_bin = lldbutil.join_remote_paths(remote_fw, "Dylib")
            err = lldb.remote_platform.Put(
                lldb.SBFileSpec(local_bin, True),
                lldb.SBFileSpec(remote_bin, False),
            )
            self.assertFalse(err.Fail(), "Failed to upload Dylib.framework: %s" % err)
            self.runCmd("process load " + remote_bin)

        log = self.getBuildArtifact('types.log')
        self.expect('log enable lldb types -f ' + log)
        playground_output = self.execute_code('Import.swift')
        self.assertIn("Hello from the Dylib", playground_output)

        # On remote the framework binaries are at flat paths (not
        # Versions/A/…), so the filecheck patterns don't apply.
        if not lldb.remote_platform:
            self.filecheck_log(log, __file__)
#       CHECK: RegisterSectionModules("AuxSources")
#       CHECK: Playground : true
#       If we wanted this to work, SwiftASTContext would need to find the AuxSources image and switch the symbol context to there.
#       CHECK-NOT: -DHAVE_AUXSOURCES
#       CHECK: Module import remark{{.*}} loaded module 'AuxSources'; source: 'AuxSources', loaded: 'AuxSources'
#       CHECK: New Swift image added{{.*}}Versions/A/Dylib{{.*}}ClangImporter needs to be reinitialized
