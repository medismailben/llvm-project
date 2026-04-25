"""
Frame provider whose get_frame_at_index calls SBFrame::IsValid on input frames.

When a client thread accesses frames through this provider while the PST is
mid-expression-eval (RunThreadPlan), the following deadlock occurs:

  - Client thread: holds ProcessRunLock read lock (from outer SB API call),
    enters get_frame_at_index, calls SBFrame.IsValid ->
    GetStoppedExecutionContext -> ReadTryLock (re-entrant, blocked by
    pending writer)

  - Override PST: finishing RunPrivateStateThread, calls SetStopped ->
    pthread_rwlock_wrlock (blocked by client thread's read lock)

The re-entrant read lock blocks because macOS uses a write-preferring rwlock.
"""

import lldb
from lldb.plugins.scripted_frame_provider import ScriptedFrameProvider


class SBAPIAccessInGetFrameProvider(ScriptedFrameProvider):
    """Provider that calls SBFrame.IsValid from get_frame_at_index."""

    @staticmethod
    def get_description():
        return "Provider that accesses SB API in get_frame_at_index"

    def get_frame_at_index(self, idx):
        if idx < len(self.input_frames):
            frame = self.input_frames.GetFrameAtIndex(idx)
            # This call triggers GetStoppedExecutionContext ->
            # ProcessRunLock::ReadTryLock. If the current thread already
            # holds the read lock (from the outer SB API entry point),
            # and a writer is pending, this re-entrant read lock blocks.
            frame.IsValid()
            return idx
        return None
