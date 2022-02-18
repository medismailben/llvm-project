# TestCGImportedTypes.py
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
Test that we are able to deal with C-imported types (from CoreGraphics)
"""
import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil
import os
import unittest2
import threading

class TestSwiftImportedModulesProgressReporting(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    eBroadcastBitStopProgressThread = (1 << 0)

    def setUp(self):
        TestBase.setUp(self)
        self.progress_events = []

    def fetch_events(self, test_broadcaster):
        listener = lldb.SBListener("lldb-swift.progress.listener")
        listener.StartListeningForEvents(test_broadcaster,
                                         self.eBroadcastBitStopProgressThread)

        progress_broadcaster = self.dbg.GetBroadcaster()
        progress_broadcaster.AddListener(listener, lldb.SBDebugger.eBroadcastBitProgress)

        event = lldb.SBEvent()

        done = False
        while not done:
            if listener.WaitForEvent(1, event):
                event_mask = event.GetType();
                if event.BroadcasterMatchesRef(test_broadcaster):
                    if event_mask & self.eBroadcastBitStopProgressThread:
                        done = True;
                elif event.BroadcasterMatchesRef(progress_broadcaster):
                    message = lldb.SBDebugger().GetProgressFromEvent(event, 0, 0, 0, False);
                    if message:
                        self.progress_events.append((message, event))

    @skipUnlessDarwin
    @swiftTest
    def test_swift_module_loading_progress_report(self):
        """Test that we are able to deal with C-imported types from CoreGraphics"""
        self.build()

        test_broadcaster = lldb.SBBroadcaster('test-broadcaster')
        listener_thread = threading.Thread(target=self.fetch_events,
                                           args=[test_broadcaster])
        listener_thread.start()

        lldbutil.run_to_source_breakpoint(
            self, 'Set breakpoint here', lldb.SBFileSpec('main.swift'))

        test_broadcaster.BroadcastEventByType(self.eBroadcastBitStopProgressThread)
        listener_thread.join()

        import pdb
        pdb.set_trace()

        self.assertTrue(len(self.progress_events) > 0)

        rect = self.frame().FindVariable("cgrect")
        self.assertTrue(rect.IsValid(), "Got the cgrect variable")

        # Track all 'progressStart' events by saving all 'progressId' values.
        progressStart_ids = set()
        # Track all 'progressEnd' events by saving all 'progressId' values.
        progressEnd_ids = set()
        # We will watch for events whose title starts with
        # 'Parsing symbol table for ' and we will save the remainder of the
        # line which will contain the shared library basename. Since we set a
        # breakpoint by name for 'main', we will expect to see progress events
        # for all shared libraries that say that the symbol table is being
        # parsed.
        symtab_progress_shlibs = set()
        # Get a list of modules in the current target so we can verify that
        # we do in fact get a progress event for each shared library.
        target_shlibs = self.vscode.get_modules()

        # Iterate over all progress events and save all start and end IDs, and
        # remember any shared libraries that got symbol table parsing progress
        # events.
        for progress_event in self.progress_events:
            event_type = progress_event['event']
            if event_type == 'progressStart':
                progressStart_ids.add(progress_event['body']['progressId'])
                title = progress_event['body']['title']
                if title.startswith('Parsing symbol table for '):
                    symtab_progress_shlibs.add(title[25:])
            if event_type == 'progressEnd':
                progressEnd_ids.add(progress_event['body']['progressId'])
        # Make sure for each 'progressStart' event, we got a matching
        # 'progressEnd' event.
        self.assertTrue(progressStart_ids == progressEnd_ids,
                        ('Make sure we got a "progressEnd" for each '
                        '"progressStart" event that we have.'))
        # Verify we got a symbol table parsing progress event for each shared
        # library in our target.
        for target_shlib_basename in target_shlibs.keys():
            self.assertTrue(target_shlib_basename in symtab_progress_shlibs,
                            'Make sure we got a symbol table progress event for "%s"' % (target_shlib_basename))
