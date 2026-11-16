import lldb

# The records live on the lldb module, which isn't reloaded with this script.


def read_header(target):
    """Read the image's Mach-O magic from the process, or say why we can't."""
    header = target.module["libpayload.dylib"].GetObjectFileHeaderAddress()
    error = lldb.SBError()
    magic = target.GetProcess().ReadUnsignedFromMemory(
        header.GetLoadAddress(target), 4, error
    )
    return magic if error.Success() else error.GetCString()


class ReadHeader:
    """A one-shot stop hook that reads the image's Mach-O header at the next
    stop."""

    def __init__(self, target, extra_args, internal_dict):
        pass

    def handle_stop(self, exe_ctx, stream):
        target = exe_ctx.GetTarget()
        index = target.GetDebugger().GetIndexOfTarget(target)
        lldb.payload_magics = getattr(lldb, "payload_magics", []) + [
            (index, read_header(target))
        ]
        return True


def __lldb_init_module(debugger, internal_dict):
    lldb.payload_inits = getattr(lldb, "payload_inits", 0) + 1


def __lldb_module_added_to_target(target, internal_dict):
    index = target.GetDebugger().GetIndexOfTarget(target)
    lldb.payload_targets = getattr(lldb, "payload_targets", []) + [index]
    # LLDB calls this once the process can be read, so read it right away.
    lldb.payload_reads = getattr(lldb, "payload_reads", []) + [
        (index, read_header(target))
    ]

    result = lldb.SBCommandReturnObject()
    target.GetDebugger().GetCommandInterpreter().HandleCommand(
        f"target stop-hook add --one-shot true -P {__name__}.ReadHeader",
        lldb.SBExecutionContext(target),
        result,
    )
