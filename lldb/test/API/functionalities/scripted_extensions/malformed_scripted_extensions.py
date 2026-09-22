"""
Intentionally malformed scripted extensions used by
TestScriptedExtensionsDiagnostics.

Each class either omits a required abstract method or raises a Python
exception from one of its affordance methods. The corresponding test asserts
that LLDB surfaces those errors to the user instead of silently swallowing
them.
"""

import lldb
from lldb.plugins.scripted_process import ScriptedProcess, ScriptedThread

# ---------------------------------------------------------------------------
# Scripted Process
# ---------------------------------------------------------------------------


class MissingMethodsScriptedProcess:
    """Missing required abstract method `read_memory_at_address`."""

    def __init__(self, exe_ctx, args):
        self.exe_ctx = exe_ctx
        self.args = args

    def get_scripted_thread_plugin(self):
        return None

    def is_alive(self):
        return True


class ExceptionScriptedProcess:
    """All abstract methods present, but `launch` raises."""

    def __init__(self, exe_ctx, args):
        self.exe_ctx = exe_ctx
        self.args = args

    def get_scripted_thread_plugin(self):
        return None

    def is_alive(self):
        return True

    def read_memory_at_address(self, addr, size, error):
        return None

    def launch(self):
        raise RuntimeError("intentional exception from launch()")


class TypoScriptedProcess:
    """All abstract methods present, but `launch` has a typo (references an
    undefined name) instead of an explicit `raise`, exercising a natural
    Python runtime error (NameError) rather than a deliberately raised
    exception."""

    def __init__(self, exe_ctx, args):
        self.exe_ctx = exe_ctx
        self.args = args

    def get_scripted_thread_plugin(self):
        return None

    def is_alive(self):
        return True

    def read_memory_at_address(self, addr, size, error):
        return None

    def launch(self):
        return this_name_is_never_defined


# ---------------------------------------------------------------------------
# Scripted Thread
# ---------------------------------------------------------------------------


class ExceptionScriptedThread:
    """Missing required abstract method `get_stop_reason`."""

    def __init__(self, process, args):
        self.process = process
        self.args = args

    def get_thread_id(self):
        raise ValueError("intentional exception from get_thread_id()")

    def get_register_context(self):
        return ""

    def get_name(self):
        return "ExceptionScriptedThread"

    def get_state(self):
        return 0


class ThreadListScriptedProcess:
    """All abstract methods present, but `get_threads_info` hands back a
    thread object (`ExceptionScriptedThread`) missing a required abstract
    method."""

    def __init__(self, exe_ctx, args):
        self.exe_ctx = exe_ctx
        self.args = args

    def get_scripted_thread_plugin(self):
        return None

    def is_alive(self):
        return True

    def read_memory_at_address(self, addr, size, error):
        return None

    def launch(self):
        return lldb.SBError()

    def get_threads_info(self):
        return {1: ExceptionScriptedThread(self, None)}


class MissingMethodsScriptedFrame:
    """Missing required abstract method `get_id`."""

    def __init__(self, thread, args):
        self.thread = thread
        self.args = args


class StackFrameScriptedThread(ScriptedThread):
    """A real, valid scripted thread (borrows the base class's default
    register-info plumbing so construction succeeds), but `get_stackframes`
    returns a scripted frame object (`MissingMethodsScriptedFrame`) missing a
    required abstract method."""

    def __init__(self, process, args):
        super().__init__(process, args)

    def get_stop_reason(self):
        return {"type": lldb.eStopReasonTrace, "data": {}}

    def get_register_context(self):
        total_bytes = sum(
            r["bitsize"] // 8 for r in self.get_register_info()["registers"]
        )
        return "\x00" * total_bytes

    def get_stackframes(self):
        return [MissingMethodsScriptedFrame(self, None)]


class StackFrameScriptedProcess(ScriptedProcess):
    """All abstract methods present, but `get_threads_info` hands back a
    thread (`StackFrameScriptedThread`) whose `get_stackframes` yields a
    malformed scripted frame."""

    def __init__(self, exe_ctx, args):
        super().__init__(exe_ctx, args)

    def get_scripted_thread_plugin(self):
        return None

    def is_alive(self):
        return True

    def read_memory_at_address(self, addr, size, error):
        return None

    def get_threads_info(self):
        return {1: StackFrameScriptedThread(self, None)}


# ---------------------------------------------------------------------------
# Scripted Platform
# ---------------------------------------------------------------------------


class MissingMethodsScriptedPlatform:
    """Missing required abstract method `list_processes`."""

    def __init__(self, exe_ctx, args):
        self.exe_ctx = exe_ctx
        self.args = args


class ExceptionScriptedPlatform:
    def __init__(self, exe_ctx, args):
        self.exe_ctx = exe_ctx
        self.args = args

    def list_processes(self):
        raise RuntimeError("intentional exception from list_processes()")

    def get_process_info(self, pid):
        return None

    def launch_process(self, launch_info):
        return None

    def kill_process(self, pid):
        return None


# ---------------------------------------------------------------------------
# Scripted Frame Provider
# ---------------------------------------------------------------------------


class ExceptionScriptedFrameProvider:
    def __init__(self, frames, args):
        self.frames = frames
        self.args = args

    def get_num_frames(self):
        raise RuntimeError("intentional exception from get_num_frames()")

    def get_frame_at_index(self, idx):
        return None


# ---------------------------------------------------------------------------
# Scripted Thread Plan
# ---------------------------------------------------------------------------


class ExceptionScriptedThreadPlan:
    def __init__(self, thread_plan, args):
        self.thread_plan = thread_plan
        self.args = args

    def explains_stop(self, event):
        raise RuntimeError("intentional exception from explains_stop()")

    def should_stop(self, event):
        return True

    def is_stale(self):
        return False


class ExceptionInitScriptedThreadPlan:
    """`__init__` raises."""

    def __init__(self, thread_plan, args):
        raise RuntimeError("intentional exception from __init__()")


# ---------------------------------------------------------------------------
# Scripted Breakpoint Resolver
# ---------------------------------------------------------------------------


class ExceptionScriptedBreakpointResolver:
    def __init__(self, bkpt, args):
        self.bkpt = bkpt
        self.args = args

    def __callback__(self, sym_ctx):
        raise RuntimeError("intentional exception from __callback__()")

    def get_short_help(self):
        return "Exception breakpoint resolver"


class ExceptionInitScriptedBreakpointResolver:
    """`__init__` raises."""

    def __init__(self, bkpt, args):
        raise RuntimeError("intentional exception from __init__()")


# ---------------------------------------------------------------------------
# Scripted Stop Hook
# ---------------------------------------------------------------------------


class ExceptionScriptedStopHook:
    def __init__(self, target, args):
        self.target = target
        self.args = args

    def handle_stop(self, exe_ctx, stream):
        raise RuntimeError("intentional exception from handle_stop()")


# ---------------------------------------------------------------------------
# Scripted Stack Frame Recognizer
# ---------------------------------------------------------------------------


class ExceptionScriptedStackFrameRecognizer:
    """`__init__` raises."""

    def __init__(self):
        raise RuntimeError("intentional exception from __init__()")


# ---------------------------------------------------------------------------
# Synthetic Child Provider
# ---------------------------------------------------------------------------


class ExceptionInitSynthProvider:
    """`__init__` raises, so the provider never comes into existence."""

    def __init__(self, valobj, internal_dict):
        raise RuntimeError("intentional exception from __init__()")


class ExceptionUpdateSynthProvider:
    """`update` raises. Deliberately keeps a safe default in `__init__` and
    computes nothing, which is the shape real providers have: the accessors
    read state that `update` was supposed to fill in, so a swallowed failure
    here looks exactly like a legitimately empty container."""

    def __init__(self, valobj, internal_dict):
        self.valobj = valobj
        self.count = 0

    def update(self):
        raise RuntimeError("intentional exception from update()")

    def num_children(self):
        return self.count

    def get_child_at_index(self, idx):
        return None


class ExceptionNumChildrenSynthProvider:
    """`num_children` raises."""

    def __init__(self, valobj, internal_dict):
        self.valobj = valobj

    def num_children(self):
        raise RuntimeError("intentional exception from num_children()")

    def get_child_at_index(self, idx):
        return None


class ExceptionOneChildSynthProvider:
    """Reports three children but raises for the middle one. The other two must
    still show, and the failing one must not silently disappear."""

    def __init__(self, valobj, internal_dict):
        self.valobj = valobj

    def num_children(self):
        return 3

    def get_child_at_index(self, idx):
        if idx == 1:
            raise RuntimeError("intentional exception from get_child_at_index")
        return self.valobj.GetChildMemberWithName("first")


class NoUpdateSynthProvider:
    """A valid provider that doesn't implement the optional `update`, to pin
    down that "unimplemented" stays distinct from "raised"."""

    def __init__(self, valobj, internal_dict):
        self.valobj = valobj

    def num_children(self):
        return 1

    def get_child_at_index(self, idx):
        return self.valobj.GetChildMemberWithName("first")


# ---------------------------------------------------------------------------
# Summary Provider
# ---------------------------------------------------------------------------


def exception_summary(valobj, internal_dict):
    raise RuntimeError("intentional exception from summary()")


def none_summary(valobj, internal_dict):
    """Returning None is not a failure: it summarizes as "None"."""
    return None


class ExceptionGetSummaryProvider:
    """Class-based summary provider whose `get_summary` raises. Constructed
    with no arguments, per `examples/python/templates/scripted_string_summary.py`."""

    def get_summary(self, valobj, options):
        raise RuntimeError("intentional exception from get_summary()")


# ---------------------------------------------------------------------------
# Operating System
# ---------------------------------------------------------------------------


class MissingMethodsOperatingSystem:
    """Missing required abstract method `get_thread_info`."""

    def __init__(self, process):
        self.process = process


class ExceptionOperatingSystem:
    def __init__(self, process):
        self.process = process

    def get_thread_info(self):
        raise RuntimeError("intentional exception from get_thread_info()")

    def get_register_info(self):
        return {}

    def get_register_data(self, tid):
        return b""
