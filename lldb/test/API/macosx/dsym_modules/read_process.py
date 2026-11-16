import lldb

# Records what __lldb_module_added_to_target read from the process, on the lldb
# module, which isn't reloaded with this script.


def __lldb_module_added_to_target(target, internal_dict):
    process = target.GetProcess()
    if not process.IsValid():
        return
    sp = process.GetThreadAtIndex(0).GetFrameAtIndex(0).GetSP()
    error = lldb.SBError()
    process.ReadMemory(sp, 8, error)
    lldb.read_process_results = getattr(lldb, "read_process_results", []) + [
        error.Success() or error.GetCString()
    ]
