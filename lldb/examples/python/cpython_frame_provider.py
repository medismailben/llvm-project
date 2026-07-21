"""
Scripted frame provider that replaces CPython's C interpreter frames with the
Python-level frames they are actually executing.

When you unwind a stopped CPython process, every Python-level call shows up as an
opaque ``Python`_PyEval_EvalFrameDefault`` C frame, interleaved with interpreter
machinery (``_PyFunction_Vectorcall``, ``PyObject_Call``, ...).  This provider
walks the interpreter's ``PyFrameObject`` chain out of target memory and swaps
each ``_PyEval_EvalFrameDefault`` frame for the corresponding Python function,
e.g.::

    frame #30: 0x...  Python`_PyEval_EvalFrameDefault + 17640

becomes::

    frame #30: run_suite at dotest.py:1094

The surrounding CPython machinery frames are collapsed (hidden) so the backtrace
reads as a clean Python call stack. Non-Python frames (liblldb, dyld, ...) are
passed through untouched.

Usage:

    (lldb) command script import /path/to/cpython_frame_provider.py
    (lldb) target frame-provider register -C cpython_frame_provider.CPython39FrameProvider
    (lldb) bt

To go back to the raw C frames, unregister the provider or use `bt --provider none`.

IMPORTANT: The struct offsets below are specific to CPython 3.9 on a 64-bit,
little-endian, release build (no `--with-trace-refs`). They will not match other
Python versions; 3.11+ in particular replaced `PyFrameObject` with an in-thread
`_PyInterpreterFrame` stack and would need a different walker.
"""

import lldb
from lldb.plugins.scripted_process import ScriptedFrame
from lldb.plugins.scripted_frame_provider import ScriptedFrameProvider

# The C symbol every executing Python frame runs inside.
EVAL_FRAME_SYMBOL = "_PyEval_EvalFrameDefault"

# CPython 3.9 struct field offsets (64-bit). See Include/cpython/*.h in the
# matching CPython source. Kept as named constants so a version bump is a
# localized edit rather than a scatter of magic numbers.
#
# struct _ts (PyThreadState):
TSTATE_FRAME = 24  # PyFrameObject *frame
#
# PyFrameObject (PyObject_VAR_HEAD is 24 bytes):
FRAME_F_BACK = 24  # struct _frame *f_back
FRAME_F_CODE = 32  # PyCodeObject *f_code
FRAME_F_LASTI = 104  # int f_lasti (byte offset of last executed instruction)
#
# PyCodeObject (PyObject_HEAD is 16 bytes):
CODE_CO_FIRSTLINENO = 40  # int co_firstlineno
CODE_CO_FILENAME = 104  # PyObject *co_filename
CODE_CO_NAME = 112  # PyObject *co_name
CODE_CO_LNOTAB = 120  # PyObject *co_lnotab (bytes)
#
# PyBytesObject (for co_lnotab): PyObject_VAR_HEAD (24) then Py_hash_t.
BYTES_OB_SIZE = 16  # Py_ssize_t ob_size
BYTES_OB_SVAL = 32  # char ob_sval[]
#
# Compact-ASCII PyUnicodeObject payload starts right after the PyASCIIObject
# header. Python identifiers and most source paths are ASCII, so reading a C
# string here recovers the text without decoding PEP 393 kinds.
UNICODE_ASCII_DATA = 48


class PythonFrame(ScriptedFrame):
    """A synthetic frame standing in for one CPython interpreter frame."""

    def __init__(self, thread, idx, name, hidden, filename=None, line=0):
        super().__init__(thread, lldb.SBStructuredData())
        self.idx = idx
        self.name = name
        self.hidden = hidden
        self.filename = filename
        self.line = line

    def get_id(self):
        return self.idx

    def get_pc(self):
        # No PC: a real address makes ScriptedFrame::Create resolve a symbol
        # context from it (ScriptedFrame.cpp), and the `bt` formatter renders
        # that symbol ("_PyEval_EvalFrameDefault + N") instead of our name.
        # LLDB_INVALID_ADDRESS skips symbolication so get_function_name() wins.
        return lldb.LLDB_INVALID_ADDRESS

    def get_function_name(self):
        return self.name

    def get_symbol_context(self):
        # With no PC, a synthetic line entry is the only way LLDB learns this
        # frame's source location, so `frame select`/`source list` can show the
        # actual .py source. The file:line in `bt` also comes from here, not the
        # function name.
        if not self.filename:
            return None
        line_entry = lldb.SBLineEntry()
        line_entry.SetFileSpec(lldb.SBFileSpec(self.filename, True))
        line_entry.SetLine(self.line)
        sym_ctx = lldb.SBSymbolContext()
        sym_ctx.SetLineEntry(line_entry)
        return sym_ctx

    def is_artificial(self):
        return False

    def is_hidden(self):
        return self.hidden

    def get_register_context(self):
        return None


class CPython39FrameProvider(ScriptedFrameProvider):
    """Replace `_PyEval_EvalFrameDefault` C frames with Python-level frames."""

    def __init__(self, input_frames, args):
        super().__init__(input_frames, args)
        # Output plan, one entry per input frame: either an int (pass the input
        # frame through) or a PythonFrame. Built lazily on first access.
        self._plan = None

    @staticmethod
    def get_description():
        return "Replace CPython interpreter C frames with Python-level frames"

    @staticmethod
    def applies_to_thread(thread):
        # Only touch threads that are actually running the Python interpreter.
        for frame in thread:
            if frame.GetFunctionName() == EVAL_FRAME_SYMBOL:
                return True
        return False

    def get_frame_at_index(self, index):
        if self._plan is None:
            self._plan = self._build_plan()
        if index < 0 or index >= len(self._plan):
            return None
        return self._plan[index]

    # --- plan construction ---------------------------------------------------

    def _build_plan(self):
        # Python frames, youngest first: [(filename, funcname, line), ...].
        py_frames = self._walk_python_frames()

        plan = []
        pyi = 0
        for i in range(len(self.input_frames)):
            frame = self.input_frames[i]
            name = frame.GetFunctionName()

            if name == EVAL_FRAME_SYMBOL:
                if pyi < len(py_frames):
                    filename, funcname, line = py_frames[pyi]
                    pyi += 1
                    # The name is just the function; the file:line comes from the
                    # synthetic line entry in PythonFrame.get_symbol_context().
                    plan.append(
                        PythonFrame(self.thread, i, funcname, False, filename, line)
                    )
                else:
                    # More eval frames than decoded Python frames: the walk fell
                    # short. Show the raw C frame rather than hiding it, so a
                    # mismatch is visible instead of silently dropping frames.
                    plan.append(i)
            elif self._is_cpython_internal(frame):
                # Collapse the interpreter machinery between Python frames so the
                # backtrace reads as a pure Python call stack.
                plan.append(PythonFrame(self.thread, i, name or "<cpython>", True))
            else:
                # Non-Python frame (liblldb, dyld, libsystem, ...): pass through.
                plan.append(i)
        return plan

    @staticmethod
    def _is_cpython_internal(frame):
        module = frame.GetModule()
        if not module:
            return False
        filename = module.GetFileSpec().GetFilename()
        return filename == "Python"

    # --- CPython memory decoding ---------------------------------------------

    def _walk_python_frames(self):
        frames = []
        frame_ptr = self._current_frame_object()
        while frame_ptr:
            info = self._decode_frame(frame_ptr)
            if info is None:
                break
            frames.append(info)
            frame_ptr = self._read_ptr(frame_ptr + FRAME_F_BACK)
        return frames

    def _current_frame_object(self):
        """Return the address of the topmost PyFrameObject, or 0 on failure.

        Discovering the current thread state without debug info is the one step
        that needs the interpreter's help, so we call PyGILState_GetThisThreadState
        (a pure lookup that neither acquires the GIL nor allocates) and read its
        `frame` field. Everything after this is a passive memory read.
        """
        frame0 = self.thread.GetFrameAtIndex(0)
        if not frame0.IsValid():
            return 0
        value = frame0.EvaluateExpression(
            "(void *)PyGILState_GetThisThreadState()"
        )
        if not value.IsValid() or value.GetError().Fail():
            return 0
        tstate = value.GetValueAsUnsigned(0)
        if not tstate:
            return 0
        return self._read_ptr(tstate + TSTATE_FRAME)

    def _decode_frame(self, frame_ptr):
        code_ptr = self._read_ptr(frame_ptr + FRAME_F_CODE)
        if not code_ptr:
            return None
        filename = self._read_pystr(code_ptr + CODE_CO_FILENAME) or "<unknown>"
        funcname = self._read_pystr(code_ptr + CODE_CO_NAME) or "<unknown>"
        lasti = self._read_int(frame_ptr + FRAME_F_LASTI)
        line = self._addr2line(code_ptr, lasti)
        return (filename, funcname, line)

    def _addr2line(self, code_ptr, lasti):
        """Reimplements PyCode_Addr2Line: map a bytecode offset to a source line."""
        first_line = self._read_uint(code_ptr + CODE_CO_FIRSTLINENO, 4)
        if lasti < 0:
            return first_line
        lnotab_ptr = self._read_ptr(code_ptr + CODE_CO_LNOTAB)
        if not lnotab_ptr:
            return first_line
        size = self._read_uint(lnotab_ptr + BYTES_OB_SIZE, 8)
        if size <= 0:
            return first_line
        data = self._read_mem(lnotab_ptr + BYTES_OB_SVAL, size)
        if data is None:
            return first_line

        line = first_line
        addr = 0
        for i in range(0, len(data) - 1, 2):
            addr += data[i]
            if addr > lasti:
                break
            line_incr = data[i + 1]
            if line_incr >= 0x80:  # signed char
                line_incr -= 0x100
            line += line_incr
        return line

    # --- raw memory helpers --------------------------------------------------

    def _read_ptr(self, addr):
        error = lldb.SBError()
        val = self.process.ReadPointerFromMemory(addr, error)
        return val if error.Success() else 0

    def _read_uint(self, addr, size):
        error = lldb.SBError()
        val = self.process.ReadUnsignedFromMemory(addr, size, error)
        return val if error.Success() else 0

    def _read_int(self, addr):
        val = self._read_uint(addr, 4)
        return val - 0x100000000 if val >= 0x80000000 else val

    def _read_mem(self, addr, size):
        error = lldb.SBError()
        data = self.process.ReadMemory(addr, size, error)
        return data if error.Success() else None

    def _read_pystr(self, ptr_field):
        obj = self._read_ptr(ptr_field)
        if not obj:
            return None
        error = lldb.SBError()
        text = self.process.ReadCStringFromMemory(
            obj + UNICODE_ASCII_DATA, 4096, error
        )
        return text if error.Success() and text else None


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand(
        "target frame-provider register -C %s.CPython39FrameProvider" % __name__
    )
    print(
        "Registered CPython39FrameProvider. Run 'bt' to see Python-level frames "
        "(or 'bt --provider none' for the raw C frames)."
    )
