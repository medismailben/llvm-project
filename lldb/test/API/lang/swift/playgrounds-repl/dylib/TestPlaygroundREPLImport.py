from __future__ import print_function
import os
import lldb
import lldbsuite.test.lldbplaygroundrepl as repl
import lldbsuite.test.lldbutil as lldbutil
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *

class TestPlaygroundREPLImport(repl.PlaygroundREPLTest):

    mydir = repl.PlaygroundREPLTest.compute_mydir(__file__)

    def upload_remote_libraries(self):
        """Upload libPlaygroundsRuntime.dylib (via super) and Dylib.framework."""
        super().upload_remote_libraries()
        if not lldb.remote_platform:
            return
        from lldbsuite.test import configuration
        root_wd = configuration.lldb_platform_working_dir

        fw_build_dir = self.getBuildArtifact("Dylib.framework")
        remote_fw_dir = lldbutil.join_remote_paths(root_wd, "Dylib.framework")
        lldb.remote_platform.MakeDirectory(remote_fw_dir, 0o700)

        local_dylib = os.path.realpath(os.path.join(fw_build_dir, "Dylib"))
        remote_dylib = lldbutil.join_remote_paths(remote_fw_dir, "Dylib")
        err = lldb.remote_platform.Put(
            lldb.SBFileSpec(local_dylib, True),
            lldb.SBFileSpec(remote_dylib, False),
        )
        self.assertFalse(err.Fail(), "Failed to upload Dylib.framework/Dylib: %s" % err)

        modules_build_dir = os.path.join(fw_build_dir, "Modules", "Dylib.swiftmodule")
        if os.path.isdir(modules_build_dir):
            remote_mods_parent = lldbutil.join_remote_paths(remote_fw_dir, "Modules")
            remote_mods = lldbutil.join_remote_paths(remote_mods_parent, "Dylib.swiftmodule")
            lldb.remote_platform.MakeDirectory(remote_mods_parent, 0o700)
            lldb.remote_platform.MakeDirectory(remote_mods, 0o700)
            for fname in os.listdir(modules_build_dir):
                local_mod = os.path.realpath(os.path.join(modules_build_dir, fname))
                if not os.path.isfile(local_mod):
                    continue
                remote_mod = lldbutil.join_remote_paths(remote_mods, fname)
                err = lldb.remote_platform.Put(
                    lldb.SBFileSpec(local_mod, True),
                    lldb.SBFileSpec(remote_mod, False),
                )
                self.assertFalse(
                    err.Fail(),
                    "Failed to upload Dylib.swiftmodule/%s: %s" % (fname, err),
                )

    def do_test(self):
        """
        Test importing a library that adds new Clang options.
        """
        # The expression compiler runs on the host — point at the local build dir
        # so it can resolve Dylib.framework's swiftmodule regardless of platform.
        self.expect('settings set target.swift-framework-search-paths "%s"' %
                    self.getBuildDir())
        self.expect('settings set target.use-all-compiler-flags true')

        if lldb.remote_platform:
            # Pre-load Dylib.framework so the JIT linker finds its symbols
            # before evaluating any expression that imports it.
            from lldbsuite.test import configuration
            root_wd = configuration.lldb_platform_working_dir
            fw_binary = lldbutil.join_remote_paths(root_wd, "Dylib.framework", "Dylib")
            self.runCmd('process load ' + fw_binary)

        log = self.getBuildArtifact('types.log')
        self.expect('log enable lldb types -f ' + log)
        result, playground_output = self.execute_code('BeforeImport.swift')
        self.assertIn("persistent", playground_output.GetSummary())
        result, playground_output = self.execute_code('Import.swift')
        self.assertIn("Hello from the Dylib", playground_output.GetSummary())
        self.assertIn("and back again", playground_output.GetSummary())

        # On remote the framework was pre-loaded before logging started, so the
        # "New Swift image added" entry won't appear in the log.
        if not lldb.remote_platform:
            self.filecheck_log(log, __file__)
#       CHECK: New Swift image added{{.*}}Versions/A/Dylib{{.*}}ClangImporter needs to be reinitialized
