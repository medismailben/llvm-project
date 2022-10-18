import os,json,struct,signal

from typing import Any, Dict

import lldb
from lldb.plugins.scripted_process import LiveScriptedProcess
from lldb.plugins.scripted_process import ScriptedThread

class ScriptedProcessPassthru(LiveScriptedProcess):
    def __init__(self, target: lldb.SBTarget, args : lldb.SBStructuredData):
        super().__init__(target, args)

        self.parent_target = self.parent_process.GetTarget()
        for parent_thread in self.parent_process:
            structured_data = lldb.SBStructuredData()
            structured_data.SetFromJSON(json.dumps({
                "thread_idx" : parent_thread.GetIndexID()
            }))

            self.threads[parent_thread.GetThreadID()] = ScriptedThreadPassthru(self, structured_data)

    def get_memory_region_containing_address(self, addr: int) -> lldb.SBMemoryRegionInfo:
        mem_region = lldb.SBMemoryRegionInfo()
        error = self.parent_process.GetMemoryRegionInfo(addr, mem_region)
        if error.Fail():
            return None
        return mem_region

    def get_thread_with_id(self, tid: int):
        return {}

    def get_registers_for_thread(self, tid: int):
        return {}

    def read_memory_at_address(self, addr: int, size: int) -> lldb.SBData:
        data = lldb.SBData()
        error = lldb.SBError()
        bytes_read = self.parent_process.ReadMemory(addr, size, error)

        if error.Fail():
            return data

        data.SetDataWithOwnership(error, bytes_read,
                                  self.parent_target.GetByteOrder(),
                                  self.parent_target.GetAddressByteSize())

        return data

    def get_loaded_images(self):
        return self.loaded_images

    def get_process_id(self) -> int:
        return 42

    def should_stop(self) -> bool:
        return True

    def is_alive(self) -> bool:
        return True

    def get_scripted_thread_plugin(self):
        return ScriptedThreadPassThru.__module__ + "." + ScriptedThreadPassthru.__name__


class ScriptedThreadPassthru(ScriptedThread):
    def __init__(self, process, args):
        super().__init__(process, args)
        thread_idx = args.GetValueForKey("thread_idx")
        self.is_stopped = False

        def extract_value_from_structured_data(data, default_val):
            if data and data.IsValid():
                if data.GetType() == lldb.eStructuredDataTypeInteger:
                    return data.GetIntegerValue(default_val)
                if data.GetType() == lldb.eStructuredDataTypeString:
                    return int(data.GetStringValue(100))
            return None

        #TODO: Change to Walrus operator (:=) with oneline if assignment
        # Requires python 3.8
        val = extract_value_from_structured_data(thread_idx, 0)
        if val is not None:
            self.idx = val

        self.parent_target = self.scripted_process.parent_target
        self.parent_process = self.parent_target.GetProcess()
        self.parent_thread = self.parent_process.GetThreadByIndexID(self.idx)

        if self.parent_thread:
            self.id = self.parent_thread.GetThreadID()

    def get_thread_id(self) -> int:
        return self.id

    def get_name(self) -> str:
        return ScriptedThreadPassthru.__name__ + ".thread-" + str(self.idx)

    def get_stop_reason(self) -> Dict[str, Any]:
        stop_reason = { "type": lldb.eStopReasonInvalid, "data": {  }}

        if self.parent_thread and self.parent_thread.IsValid() \
            and self.get_thread_id() == self.parent_thread.GetThreadID():
            stop_reason["type"] = lldb.eStopReasonNone

            parent_stop_reason = self.parent_thread.GetStopReason()

            if parent_stop_reason is not lldb.eStopReasonNone \
                    or parent_stop_reason is not lldb.eStopReasonInvalid:
                if 'arm64' in self.scripted_process.arch:
                    stop_reason["type"] = lldb.eStopReasonException
                    stop_reason["data"]["desc"] = self.parent_thread.GetStopDescription(100)
                elif self.scripted_process.arch == 'x86_64':
                    stop_reason["type"] = lldb.eStopReasonSignal
                    stop_reason["data"]["signal"] = signal.SIGTRAP
                else:
                    stop_reason["type"] = self.parent_thread.GetStopReason()

        return stop_reason

    def get_register_context(self) -> str:
        if not self.parent_thread or self.parent_thread.GetNumFrames() == 0:
            return None
        frame = self.parent_thread.GetFrameAtIndex(0)

        GPRs = None
        registerSet = frame.registers # Returns an SBValueList.
        for regs in registerSet:
            if 'general purpose' in regs.name.lower():
                GPRs = regs
                break

        if not GPRs:
            return None

        for reg in GPRs:
            self.register_ctx[reg.name] = int(reg.value, base=16)

        return struct.pack("{}Q".format(len(self.register_ctx)), *self.register_ctx.values())


def __lldb_init_module(debugger, dict):
    if not 'SKIP_SCRIPTED_PROCESS_LAUNCH' in os.environ:
        debugger.HandleCommand(
            "process launch -C %s.%s" % (__name__,
                                     ScriptedProcessPassthru.__name__))
    else:
        print("Name of the class that will manage the scripted process: '%s.%s'"
                % (__name__, ScriptedProcessPassthru.__name__))
