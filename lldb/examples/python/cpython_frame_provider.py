"""
Scripted frame provider that replaces CPython's C interpreter frames with the
Python-level frames they are actually executing.

When you unwind a stopped CPython process, every Python-level call shows up as
an opaque ``Python`_PyEval_EvalFrameDefault`` C frame, interleaved with
interpreter machinery (``_PyFunction_Vectorcall``, ``PyObject_Call``, ...).
This provider reads the interpreter's frames out of target memory and swaps
each eval frame for the Python functions it is running, e.g.::

    frame #30: 0x...  Python`_PyEval_EvalFrameDefault + 17640

becomes::

    frame #30: run_suite at dotest.py:1094

The surrounding CPython machinery frames are collapsed (hidden) so the
backtrace reads as a clean Python call stack. Non-Python frames (liblldb,
dyld, ...) are passed through untouched.

On top of the backtrace, the synthetic frames support ``frame variable``,
which lists the Python frame's fast locals decoded out of target memory
(`PythonFrame.get_variables`).

`PythonStepPlan` sketches Python-level ``thread step-over`` / ``step-in`` /
``step-out`` on top of the same data, but it is not usable yet and is off
unless the provider is registered with ``-k python_stepping -v true``; see
that class for what is missing.

Usage::

    (lldb) command script import /path/to/cpython_frame_provider.py
    (lldb) target frame-provider register -C cpython_frame_provider.CPythonFrameProvider
    (lldb) bt

``bt --provider 0`` shows the raw C frames again (provider ID 0 is the base
unwinder); ``target frame-provider list`` and ``target frame-provider remove
<id>`` manage the registration.

Everything here is a passive memory read. Nothing runs code in the target,
which keeps the provider usable on core files and on threads that cannot run.
It is also a hard requirement rather than a preference: running inferior code
from inside a frame provider re-enters lldb's frame machinery underneath
``Thread::ShouldStop``, which has already cached the current thread plan, and
lldb crashes on the stale pointer. That rules out both expression evaluation
and anything that reaches lldb's type system, since a type lookup can run the
Objective-C runtime's support code in the target.

Supported interpreters
----------------------

CPython has moved the interpreter state around repeatedly, so the offsets are
resolved at runtime rather than hardcoded to one version. `resolve_interpreter`
tries, in order:

1. ``_Py_debug_offsets``, the self-describing offset table CPython 3.13
   embedded at the start of ``_PyRuntime`` precisely so out-of-process tools
   would stop hardcoding offsets.
2. A static table of known-good offsets keyed by version (`_STATIC_LAYOUTS`).

The winning layout is validated against live memory before it is used: a
layout that is even slightly wrong decodes plausible-looking nonsense, which
is worse than declining to provide frames at all. When none validates, the
provider passes the C frames through unchanged.

This has been tested against CPython 3.9 (static table) and 3.14
(``_Py_debug_offsets``), which are the two structurally different eras: up to
3.10 each Python call has its own heap ``PyFrameObject`` and its own C eval
frame, while from 3.11 on frames live in a per-thread data stack and the
interpreter runs Python-to-Python calls without recursing into C. Adding a
version to `_STATIC_LAYOUTS` is a self-contained edit; see the comment there.
"""

import re
import struct

import lldb
from lldb.plugins.scripted_frame_provider import ScriptedFrameProvider
from lldb.plugins.scripted_process import ScriptedFrame
from lldb.plugins.scripted_thread_plan import ScriptedThreadPlan

# The C symbol every executing Python frame runs inside.
EVAL_FRAME_SYMBOL = "_PyEval_EvalFrameDefault"

# Marks the start of the `_Py_DebugOffsets` table embedded in `_PyRuntime`.
DEBUG_OFFSETS_COOKIE = b"xdebugpy"

# How the interpreter stores its frames.
FRAMES_ON_HEAP = "heap"  # <= 3.10: a PyFrameObject per call, linked by f_back.
FRAMES_IN_DATASTACK = "datastack"  # >= 3.11: _PyInterpreterFrame, linked by previous.

# How a code object encodes the bytecode-offset to source-line mapping.
LINES_LNOTAB = "lnotab"  # <= 3.9
LINES_LINETABLE = "linetable"  # 3.10, PEP 626
LINES_LOCATIONS = "locations"  # >= 3.11, PEP 657


class ProviderError(Exception):
    """Raised when the target's interpreter cannot be introspected."""


# ---------------------------------------------------------------------------
# Raw memory access
# ---------------------------------------------------------------------------


class _Reader:
    """Best-effort typed reads out of the target.

    Every accessor reports a falsy value instead of failing. A frame provider
    runs while lldb is formatting a backtrace, so raising through it turns a
    cosmetic problem into an unusable session; callers check for zero and stop
    walking instead.
    """

    def __init__(self, process):
        self.process = process

    def ptr(self, addr):
        error = lldb.SBError()
        value = self.process.ReadPointerFromMemory(addr, error)
        return value if error.Success() else 0

    def uint(self, addr, size):
        error = lldb.SBError()
        value = self.process.ReadUnsignedFromMemory(addr, size, error)
        return value if error.Success() else 0

    def sint(self, addr, size):
        value = self.uint(addr, size)
        sign_bit = 1 << (size * 8 - 1)
        return value - 2 * sign_bit if value & sign_bit else value

    def mem(self, addr, size):
        # A wrong layout turns a length field into an arbitrary 64-bit number,
        # and ReadMemory would happily try to allocate it.
        if size <= 0 or size > 1 << 20:
            return None
        error = lldb.SBError()
        data = self.process.ReadMemory(addr, size, error)
        return data if error.Success() else None

    def cstring(self, addr, limit=4096):
        if not addr:
            return None
        error = lldb.SBError()
        try:
            text = self.process.ReadCStringFromMemory(addr, limit, error)
        except SystemError:
            # SBProcess::ReadCStringFromMemory raises rather than reporting an
            # error for some unreadable addresses, and speculative reads are
            # routine here.
            return None
        return text if error.Success() else None


# ---------------------------------------------------------------------------
# Interpreter layout
# ---------------------------------------------------------------------------

# Offsets every layout must define. Listing them explicitly means a half-filled
# table is rejected up front rather than silently reading offset 0 from the
# middle of a walk.
_REQUIRED_FIELDS = (
    "frame_kind",
    "line_kind",
    "code_filename",
    "code_name",
    "code_linetable",
    "code_firstlineno",
    "code_localsplusnames",
    "unicode_state",
    "unicode_length",
    "unicode_data",
    "bytes_size",
    "bytes_data",
    "tuple_item",
    "float_value",
)

_DEFAULT_FIELDS = {
    # PyObject, PyVarObject and the head of PyTypeObject have not been
    # reordered in the lifetime of Python 3, so they are not worth versioning.
    "ob_type": 8,
    "ob_size": 16,
    "tp_name": 24,
    # _PyStackRef (3.14+) tags the low bits of localsplus and f_executable
    # entries. Masking three bits is harmless on versions that store a plain
    # pointer, since PyObject allocations are at least 8-byte aligned.
    "stackref_mask": ~0x7,
    "code_qualname": 0,
    "long_digit": 24,
    "long_size": 16,
    # FRAMES_IN_DATASTACK only.
    "frame_previous": 0,
    "frame_executable": 0,
    "frame_instr_ptr": 0,
    "frame_owner": 0,
    "frame_owner_cstack": -1,
    "frame_is_entry": -1,
    "code_adaptive": 0,
    # FRAMES_ON_HEAP only. `f_lasti` counts bytes before 3.10 and code units
    # from 3.10 on.
    "frame_back": 0,
    "frame_code": 0,
    "frame_lasti": 0,
    "lasti_is_index": False,
    "frame_localsplus": 0,
    # Walking `_PyRuntime` needs all four; when they are missing the
    # interpreter falls back to finding frames on the C stack.
    "tstate_frame": 0,
    "tstate_next": 0,
    "tstate_native_thread_id": 0,
    "runtime_interpreters_head": 0,
    "interp_next": 0,
    "interp_threads_head": 0,
}


class _Layout:
    """Offsets and encodings describing one target interpreter."""

    def __init__(self, version, fields, source):
        missing = [name for name in _REQUIRED_FIELDS if fields.get(name) is None]
        if missing:
            raise ProviderError(
                "incomplete %s layout for Python %d.%d: missing %s"
                % (source, version[0], version[1], ", ".join(missing))
            )
        self.version = version
        self.source = source
        for name, value in _DEFAULT_FIELDS.items():
            setattr(self, name, value)
        for name, value in fields.items():
            setattr(self, name, value)

    def __str__(self):
        return "Python %d.%d from %s" % (
            self.version[0],
            self.version[1],
            self.source,
        )

    @property
    def can_walk_runtime(self):
        """Whether `_PyRuntime` alone is enough to enumerate thread states."""
        return bool(
            self.runtime_interpreters_head
            and self.interp_threads_head
            and self.tstate_frame
        )


# CPython 3.9, verified with `offsetof` against the 3.9.6 headers.
#
# To add a version, compile and run a program like this against its headers and
# transcribe the output:
#
#     #include <Python.h>
#     #include <frameobject.h>
#     int main(void) {
#       printf("f_lasti %zu\n", offsetof(PyFrameObject, f_lasti));
#       /* ...one line per field below... */
#     }
#
#     clang -I"$(python3.X -c 'import sysconfig
#     print(sysconfig.get_paths()["include"])')" offsets.c -o offsets && ./offsets
#
# Versions from 3.11 on keep their frames in `_PyInterpreterFrame`, which lives
# in the internal headers and needs -DPy_BUILD_CORE; 3.13 and later are covered
# by `_Py_debug_offsets` anyway and need no table at all.
_STATIC_LAYOUTS = {
    (3, 9): {
        "frame_kind": FRAMES_ON_HEAP,
        "line_kind": LINES_LNOTAB,
        "frame_back": 24,
        "frame_code": 32,
        "frame_lasti": 104,
        "frame_localsplus": 360,
        "lasti_is_index": False,
        "code_firstlineno": 40,
        "code_localsplusnames": 72,  # co_varnames
        "code_filename": 104,
        "code_name": 112,
        "code_linetable": 120,  # co_lnotab
        "unicode_state": 32,
        "unicode_length": 16,
        "unicode_data": 48,  # sizeof(PyASCIIObject)
        "bytes_size": 16,
        "bytes_data": 32,
        "tuple_item": 24,
        "float_value": 16,
        "long_digit": 24,
        "long_size": 16,  # signed ob_size, as in <= 3.11
    },
}


# `_Py_DebugOffsets` is a flat run of uint64 fields behind an 8-byte cookie, so
# a layout is just "which slot holds which offset". CPython documents the table
# as stable only within a minor version, hence one entry per known version.
def _slots(*names):
    return {name: 8 + 8 * index for index, name in enumerate(names)}


_DEBUG_OFFSETS_LAYOUTS = {
    # Verified against CPython 3.14.7.
    (3, 14): _slots(
        "version",
        "free_threaded",
        "runtime_struct_size",
        "runtime_finalizing",
        "runtime_interpreters_head",
        "interp_struct_size",
        "interp_id",
        "interp_next",
        "interp_threads_head",
        "interp_threads_main",
        "interp_gc",
        "interp_imports_modules",
        "interp_sysdict",
        "interp_builtins",
        "interp_ceval_gil",
        "interp_gil_runtime_state",
        "interp_gil_enabled",
        "interp_gil_locked",
        "interp_gil_holder",
        "interp_code_object_generation",
        "interp_tlbc_generation",
        "tstate_struct_size",
        "tstate_prev",
        "tstate_next",
        "tstate_interp",
        "tstate_frame",
        "tstate_thread_id",
        "tstate_native_thread_id",
        "tstate_datastack_chunk",
        "tstate_status",
        "iframe_struct_size",
        "iframe_previous",
        "iframe_executable",
        "iframe_instr_ptr",
        "iframe_localsplus",
        "iframe_owner",
        "iframe_stackpointer",
        "iframe_tlbc_index",
        "code_struct_size",
        "code_filename",
        "code_name",
        "code_qualname",
        "code_linetable",
        "code_firstlineno",
        "code_argcount",
        "code_localsplusnames",
        "code_localspluskinds",
        "code_adaptive",
        "code_tlbc",
        "object_struct_size",
        "object_ob_type",
        "type_struct_size",
        "type_tp_name",
        "type_tp_repr",
        "type_tp_flags",
        "tuple_struct_size",
        "tuple_ob_item",
        "tuple_ob_size",
        "list_struct_size",
        "list_ob_item",
        "list_ob_size",
        "set_struct_size",
        "set_used",
        "set_table",
        "set_mask",
        "dict_struct_size",
        "dict_ma_keys",
        "dict_ma_values",
        "float_struct_size",
        "float_ob_fval",
        "long_struct_size",
        "long_lv_tag",
        "long_ob_digit",
        "bytes_struct_size",
        "bytes_ob_size",
        "bytes_ob_sval",
        "unicode_struct_size",
        "unicode_state",
        "unicode_length",
        "unicode_asciiobject_size",
    ),
}

# Frame-ownership sentinel marking the boundary between one C-level eval call
# and the next. CPython renumbered the enum in 3.14 when it inserted
# FRAME_OWNED_BY_INTERPRETER; 3.11 used a separate `is_entry` bool instead.
_FRAME_OWNED_BY_CSTACK = {(3, 12): 3, (3, 13): 3, (3, 14): 4}


def _symbol_address(target, name):
    symbols = target.FindSymbols(name)
    if not symbols.GetSize():
        return 0
    address = symbols.GetContextAtIndex(0).GetSymbol().GetStartAddress()
    load_address = address.GetLoadAddress(target)
    return 0 if load_address == lldb.LLDB_INVALID_ADDRESS else load_address


def _layout_from_debug_offsets(target, reader):
    """Strategy 1: read CPython's own offset table out of `_PyRuntime`."""
    runtime = _symbol_address(target, "_PyRuntime")
    if not runtime:
        return None, 0
    if reader.mem(runtime, len(DEBUG_OFFSETS_COOKIE)) != DEBUG_OFFSETS_COOKIE:
        return None, runtime

    hexversion = reader.uint(runtime + 8, 8)
    version = ((hexversion >> 24) & 0xFF, (hexversion >> 16) & 0xFF)

    # Prefer the table for the reported version but try the others too: a newer
    # CPython that only appended fields still parses with an older map, and
    # _sane_debug_offsets rejects a pairing that does not fit.
    candidates = [version] + [key for key in _DEBUG_OFFSETS_LAYOUTS if key != version]
    for candidate in candidates:
        slots = _DEBUG_OFFSETS_LAYOUTS.get(candidate)
        if slots is None:
            continue
        raw = {name: reader.uint(runtime + slot, 8) for name, slot in slots.items()}
        if not _sane_debug_offsets(raw):
            continue

        source = "_Py_debug_offsets"
        if candidate != version:
            source += " (%d.%d table)" % candidate
        fields = {
            "frame_kind": FRAMES_IN_DATASTACK,
            "line_kind": LINES_LOCATIONS,
            "runtime_interpreters_head": raw["runtime_interpreters_head"],
            "interp_next": raw["interp_next"],
            "interp_threads_head": raw["interp_threads_head"],
            "tstate_next": raw["tstate_next"],
            "tstate_frame": raw["tstate_frame"],
            "tstate_native_thread_id": raw["tstate_native_thread_id"],
            "frame_previous": raw["iframe_previous"],
            "frame_executable": raw["iframe_executable"],
            "frame_instr_ptr": raw["iframe_instr_ptr"],
            "frame_localsplus": raw["iframe_localsplus"],
            "frame_owner": raw["iframe_owner"],
            "frame_owner_cstack": _FRAME_OWNED_BY_CSTACK.get(version, 4),
            "code_filename": raw["code_filename"],
            "code_name": raw["code_name"],
            "code_qualname": raw["code_qualname"],
            "code_linetable": raw["code_linetable"],
            "code_firstlineno": raw["code_firstlineno"],
            "code_localsplusnames": raw["code_localsplusnames"],
            "code_adaptive": raw["code_adaptive"],
            "ob_type": raw["object_ob_type"],
            "tp_name": raw["type_tp_name"],
            "ob_size": raw["tuple_ob_size"],
            "tuple_item": raw["tuple_ob_item"],
            "float_value": raw["float_ob_fval"],
            "long_tag": raw["long_lv_tag"],
            "long_digit": raw["long_ob_digit"],
            "bytes_size": raw["bytes_ob_size"],
            "bytes_data": raw["bytes_ob_sval"],
            "unicode_state": raw["unicode_state"],
            "unicode_length": raw["unicode_length"],
            "unicode_data": raw["unicode_asciiobject_size"],
        }
        return _Layout(version, fields, source), runtime

    return None, runtime


def _sane_debug_offsets(raw):
    """Reject a cookie/table pairing whose numbers cannot be struct offsets."""
    for name, value in raw.items():
        # The `struct_size` slots hold sizeof() results, which are legitimately
        # large (PyInterpreterState runs to tens of kilobytes), so only the
        # offsets are worth bounding.
        if name == "version" or name.endswith("struct_size"):
            continue
        if value >= 1 << 16:
            return False
    # `_PyInterpreterFrame.f_executable` legitimately sits at offset 0, but a
    # table read at the wrong place tends to produce long runs of zeroes.
    return bool(raw["tstate_frame"] and raw["code_filename"] and raw["iframe_previous"])


def _detect_version(target, reader):
    """Best-effort (major, minor) for the target's interpreter."""
    # `Py_Version` (3.11+) is a plain PY_VERSION_HEX constant.
    address = _symbol_address(target, "Py_Version")
    if address:
        hexversion = reader.uint(address, 4)
        if hexversion:
            return ((hexversion >> 24) & 0xFF, (hexversion >> 16) & 0xFF)

    # Otherwise the interpreter's own file name carries it: python3.9,
    # libpython3.10.so, Python.framework/Versions/3.9/Python, ...
    for module in target.module_iter():
        spec = module.GetFileSpec()
        for text in (spec.GetFilename() or "", spec.GetDirectory() or ""):
            match = re.search(r"(?:python|Versions/)(\d+)\.(\d+)", text, re.IGNORECASE)
            if match:
                return (int(match.group(1)), int(match.group(2)))
    return None


# ---------------------------------------------------------------------------
# Line tables
# ---------------------------------------------------------------------------


def _decode_lnotab(data, firstlineno, offset):
    """CPython <= 3.9 `co_lnotab`, mirroring PyCode_Addr2Line."""
    line = firstlineno
    addr = 0
    for index in range(0, len(data) - 1, 2):
        addr += data[index]
        if addr > offset:
            break
        delta = data[index + 1]
        if delta >= 0x80:
            delta -= 0x100
        line += delta
    return line


def _decode_linetable(data, firstlineno, offset):
    """CPython 3.10 `co_linetable` (PEP 626)."""
    computed = firstlineno
    start = 0
    index = 0
    while index + 1 < len(data):
        end = start + data[index]
        delta = data[index + 1]
        index += 2
        if delta == 0x80:
            # A -128 delta marks bytecode with no source line at all.
            line = -1
        else:
            computed += delta - 0x100 if delta > 0x80 else delta
            line = computed
        if start <= offset < end:
            return line
        start = end
    return -1


def _read_varint(data, index):
    byte = data[index]
    index += 1
    value = byte & 0x3F
    shift = 0
    while byte & 0x40:
        byte = data[index]
        index += 1
        shift += 6
        value |= (byte & 0x3F) << shift
    return value, index


def _read_svarint(data, index):
    value, index = _read_varint(data, index)
    return (-(value >> 1) if value & 1 else value >> 1), index


def _decode_locations(data, firstlineno, offset):
    """CPython >= 3.11 `co_linetable` (PEP 657 location table)."""
    computed = firstlineno
    start = 0
    index = 0
    while index < len(data):
        first = data[index]
        index += 1
        if not first & 0x80:
            break
        code = (first >> 3) & 0x0F
        end = start + ((first & 0x07) + 1) * 2
        if code == 15:  # PY_CODE_LOCATION_INFO_NONE
            line = -1
        elif code == 14:  # PY_CODE_LOCATION_INFO_LONG
            delta, index = _read_svarint(data, index)
            computed += delta
            for _ in range(3):  # end line, column, end column
                _, index = _read_varint(data, index)
            line = computed
        elif code == 13:  # PY_CODE_LOCATION_INFO_NO_COLUMNS
            delta, index = _read_svarint(data, index)
            computed += delta
            line = computed
        elif 10 <= code <= 12:  # PY_CODE_LOCATION_INFO_ONE_LINE0..2
            computed += code - 10
            index += 2
            line = computed
        else:  # short forms, same line
            index += 1
            line = computed
        if start <= offset < end:
            return line
        start = end
    return -1


_LINE_DECODERS = {
    LINES_LNOTAB: _decode_lnotab,
    LINES_LINETABLE: _decode_linetable,
    LINES_LOCATIONS: _decode_locations,
}


# ---------------------------------------------------------------------------
# Reading the interpreter
# ---------------------------------------------------------------------------


class _Interpreter:
    """Decodes CPython objects and frames out of target memory."""

    # Upper bound on a frame chain, so a corrupt or mid-update pointer cannot
    # turn a backtrace into an infinite loop.
    MAX_FRAMES = 4096

    def __init__(self, process, layout, runtime):
        self.process = process
        self.target = process.GetTarget()
        self.layout = layout
        self.runtime = runtime
        self.reader = _Reader(process)
        self.frame_type = _symbol_address(self.target, "PyFrame_Type")

    # -- objects ------------------------------------------------------------

    def type_name(self, obj):
        type_object = self.reader.ptr(obj + self.layout.ob_type)
        if not type_object:
            return None
        return self.reader.cstring(
            self.reader.ptr(type_object + self.layout.tp_name), 128
        )

    def str_value(self, obj):
        """Decode a `str`, or None when it is not a form we can read."""
        if not obj:
            return None
        layout = self.layout
        state = self.reader.uint(obj + layout.unicode_state, 1)
        length = self.reader.uint(obj + layout.unicode_length, 8)
        compact = (state >> 5) & 1
        if not compact or length > (1 << 20):
            # Legacy and non-compact strings keep their data elsewhere;
            # the caller's display fallback beats decoding the wrong bytes.
            return None
        if (state >> 6) & 1:  # ascii
            data = self.reader.mem(obj + layout.unicode_data, length)
            return data.decode("ascii", "replace") if data is not None else None
        # Compact non-ASCII payloads start after the larger
        # PyCompactUnicodeObject header, and the kind field gives the width.
        kind = (state >> 2) & 0x7
        if kind not in (1, 2, 4):
            return None
        data = self.reader.mem(obj + layout.unicode_data + 24, length * kind)
        if data is None:
            return None
        encoding = {1: "latin-1", 2: "utf-16-le", 4: "utf-32-le"}[kind]
        return data.decode(encoding, "replace")

    def bytes_value(self, obj):
        if not obj:
            return None
        size = self.reader.uint(obj + self.layout.bytes_size, 8)
        return self.reader.mem(obj + self.layout.bytes_data, size)

    def tuple_items(self, obj):
        if not obj:
            return []
        size = self.reader.sint(obj + self.layout.ob_size, 8)
        if size <= 0 or size > self.MAX_FRAMES:
            return []
        base = obj + self.layout.tuple_item
        return [self.reader.ptr(base + 8 * index) for index in range(size)]

    def int_value(self, obj):
        """Decode an `int` that fits comfortably in a machine word, else None."""
        layout = self.layout
        if hasattr(layout, "long_tag"):
            # 3.12 replaced the signed ob_size with a packed tag: the low two
            # bits carry the sign (1 meaning zero), the rest the digit count.
            tag = self.reader.uint(obj + layout.long_tag, 8)
            count = tag >> 3
            sign = 0 if (tag & 3) == 1 else (-1 if (tag & 3) == 2 else 1)
        else:
            size = self.reader.sint(obj + layout.long_size, 8)
            count = abs(size)
            sign = (size > 0) - (size < 0)
        if count == 0 or sign == 0:
            return 0
        if count > 2:  # over 60 bits; no native type to project it onto
            return None
        value = 0
        for index in range(count):
            digit = self.reader.uint(obj + layout.long_digit + 4 * index, 4)
            value |= digit << (30 * index)
        return sign * value

    def float_value(self, obj):
        data = self.reader.mem(obj + self.layout.float_value, 8)
        return struct.unpack("<d", data)[0] if data else None

    # -- frames -------------------------------------------------------------

    def code_of(self, frame):
        if self.layout.frame_kind == FRAMES_ON_HEAP:
            return self.reader.ptr(frame + self.layout.frame_code)
        executable = self.reader.ptr(frame + self.layout.frame_executable)
        return executable & self.layout.stackref_mask

    def instruction_offset(self, frame, code):
        """Byte offset into the bytecode of the instruction `frame` is at."""
        layout = self.layout
        if layout.frame_kind == FRAMES_ON_HEAP:
            lasti = self.reader.sint(frame + layout.frame_lasti, 4)
            if lasti < 0:
                return -1
            return lasti * 2 if layout.lasti_is_index else lasti
        instr = self.reader.ptr(frame + layout.frame_instr_ptr)
        if not instr:
            return -1
        return instr - (code + layout.code_adaptive)

    def line_of(self, code, offset):
        firstlineno = self.reader.uint(code + self.layout.code_firstlineno, 4)
        if offset < 0:
            return firstlineno
        data = self.bytes_value(self.reader.ptr(code + self.layout.code_linetable))
        if not data:
            return firstlineno
        line = _LINE_DECODERS[self.layout.line_kind](data, firstlineno, offset)
        return firstlineno if line < 0 else line

    def describe(self, frame):
        """(filename, function name, line) for one frame, or None.

        Doubles as the layout self-check: a wrong offset leads here with a
        pointer that the interpreter does not agree is a code object.
        """
        if not frame:
            return None
        code = self.code_of(frame)
        if not code or self.type_name(code) != "code":
            return None
        layout = self.layout
        filename = self.str_value(self.reader.ptr(code + layout.code_filename))
        name = None
        if layout.code_qualname:
            name = self.str_value(self.reader.ptr(code + layout.code_qualname))
        if not name:
            name = self.str_value(self.reader.ptr(code + layout.code_name))
        offset = self.instruction_offset(frame, code)
        return (
            filename or "<unknown>",
            name or "<unknown>",
            self.line_of(code, offset),
        )

    def next_frame(self, frame):
        field = (
            self.layout.frame_back
            if self.layout.frame_kind == FRAMES_ON_HEAP
            else self.layout.frame_previous
        )
        return self.reader.ptr(frame + field)

    def is_entry_frame(self, frame):
        """True for the shim frame marking a C-level eval boundary."""
        layout = self.layout
        if layout.frame_kind == FRAMES_ON_HEAP:
            return False
        if layout.frame_is_entry >= 0:  # 3.11 used a dedicated bool
            return bool(self.reader.uint(frame + layout.frame_is_entry, 1))
        if layout.frame_owner_cstack < 0:
            return False
        owner = self.reader.uint(frame + layout.frame_owner, 1)
        return owner == layout.frame_owner_cstack

    def fast_locals(self, frame):
        """[(name, PyObject *)] for the frame's fast locals."""
        layout = self.layout
        code = self.code_of(frame)
        if not code:
            return []
        names = self.tuple_items(self.reader.ptr(code + layout.code_localsplusnames))
        base = frame + layout.frame_localsplus
        locals_list = []
        for index, name_object in enumerate(names):
            name = self.str_value(name_object)
            if not name:
                continue
            value = self.reader.ptr(base + 8 * index) & layout.stackref_mask
            locals_list.append((name, value))
        return locals_list

    # -- finding frames -----------------------------------------------------

    def thread_states(self):
        """Every PyThreadState, walked from `_PyRuntime`.

        Only available when the layout knows its way around the runtime
        struct, but when it is this needs no inferior execution at all, so it
        also works on core files.
        """
        layout = self.layout
        if not self.runtime or not layout.can_walk_runtime:
            return []
        states = []
        interpreter = self.reader.ptr(self.runtime + layout.runtime_interpreters_head)
        seen_interpreters = set()
        while interpreter and interpreter not in seen_interpreters:
            seen_interpreters.add(interpreter)
            state = self.reader.ptr(interpreter + layout.interp_threads_head)
            seen_states = set()
            while state and state not in seen_states:
                seen_states.add(state)
                states.append(state)
                state = self.reader.ptr(state + layout.tstate_next)
            interpreter = self.reader.ptr(interpreter + layout.interp_next)
        return states

    def thread_state_for(self, thread):
        """The PyThreadState running on `thread`, or 0."""
        if not self.layout.tstate_native_thread_id:
            return 0
        for state in self.thread_states():
            native = self.reader.uint(state + self.layout.tstate_native_thread_id, 8)
            if native == thread.GetThreadID():
                return state
        return 0

    def frame_object_on_stack(self, sbframe):
        """The PyFrameObject an eval frame is running, found on the C stack.

        `_PyEval_EvalFrameDefault` takes its frame as an argument and spills it
        to its own stack frame, so a word-by-word scan of that frame recovers
        it without debug info, without knowing where the thread state lives,
        and without running anything in the target. Only the pre-3.11 frame
        layout is a PyObject, so only it can be recognised this way.
        """
        if self.layout.frame_kind != FRAMES_ON_HEAP:
            return 0
        low, high = sbframe.GetSP(), sbframe.GetCFA()
        if not low or not high or high <= low or high - low > 1 << 16:
            return 0
        data = self.reader.mem(low, (high - low) & ~0x7)
        if not data:
            return 0
        for offset in range(0, len(data) - 7, 8):
            candidate = struct.unpack_from("<Q", data, offset)[0]
            if not candidate or candidate & 0x7:
                continue
            if self.frame_type:
                if self.reader.ptr(candidate + self.layout.ob_type) != self.frame_type:
                    continue
            elif self.type_name(candidate) != "frame":
                continue
            if self.describe(candidate):
                return candidate
        return 0

    def chunks_for_thread(self, thread):
        """Python frames grouped by the C eval frame that is running them.

        One chunk per `_PyEval_EvalFrameDefault` frame, in the same order, so
        the caller can substitute them one for one. Before 3.11 every chunk
        holds exactly one frame; from 3.11 on the interpreter runs
        Python-to-Python calls without recursing into C, so a single chunk can
        be arbitrarily deep.
        """
        eval_frames = [
            frame for frame in thread if frame.GetFunctionName() == EVAL_FRAME_SYMBOL
        ]
        if not eval_frames:
            return []

        if self.layout.frame_kind == FRAMES_ON_HEAP:
            chunks = []
            for sbframe in eval_frames:
                frame = self.frame_object_on_stack(sbframe)
                info = self.describe(frame)
                chunks.append([(frame,) + info] if info else [])
            return chunks

        state = self.thread_state_for(thread)
        return self.walk(state) if state else []

    def walk(self, tstate):
        """Frames reachable from `tstate`, youngest first, grouped by eval call."""
        chunks = []
        current = []
        frame = self.reader.ptr(tstate + self.layout.tstate_frame)
        seen = set()
        while frame and frame not in seen and len(seen) < self.MAX_FRAMES:
            seen.add(frame)
            if self.is_entry_frame(frame):
                # The shim runs no Python itself; it just closes off the frames
                # belonging to this eval call.
                if current:
                    chunks.append(current)
                    current = []
            else:
                info = self.describe(frame)
                if info is None:
                    break
                current.append((frame,) + info)
            frame = self.next_frame(frame)
        if current:
            chunks.append(current)
        return chunks

    def self_check(self):
        """Whether this layout can actually decode the target's frames."""
        for thread in self.process:
            # Prefer the runtime walk. It does not look at the thread's C
            # frames, which have already been rewritten by this very provider
            # whenever the check runs from somewhere other than the provider
            # itself.
            state = self.thread_state_for(thread)
            if state and any(self.walk(state)):
                return True
            if any(self.chunks_for_thread(thread)):
                return True
        return False


def resolve_interpreter(target, process):
    """A validated `_Interpreter` for the target, or raise ProviderError."""
    reader = _Reader(process)

    from_offsets, runtime = _layout_from_debug_offsets(target, reader)
    version = from_offsets.version if from_offsets else _detect_version(target, reader)

    candidates = []
    if from_offsets:
        candidates.append(from_offsets)
    if version:
        static = _STATIC_LAYOUTS.get(version)
        if static:
            candidates.append(_Layout(version, static, "static table"))

    if not candidates:
        raise ProviderError(
            "no layout available for %s"
            % ("Python %d.%d" % version if version else "an unrecognised Python")
        )

    for layout in candidates:
        interpreter = _Interpreter(process, layout, runtime)
        if interpreter.self_check():
            return interpreter

    raise ProviderError(
        "no layout decoded the target's frames (tried: %s)"
        % ", ".join(layout.source for layout in candidates)
    )


# ---------------------------------------------------------------------------
# Synthetic frames
# ---------------------------------------------------------------------------


class CollapsedFrame(ScriptedFrame):
    """A real C frame kept in the stack but hidden from the backtrace.

    The interpreter machinery between two Python calls is noise in a Python
    backtrace, but it is still part of the thread's real stack: lldb reads the
    caller's PC and CFA out of it to build a step-out plan, so replacing it
    with an address-less placeholder would break stepping through Python
    entirely. Mirroring the wrapped frame and setting `is_hidden` keeps the
    stack walkable while dropping it from what the user sees.
    """

    def __init__(self, thread, idx, wrapped):
        super().__init__(thread, lldb.SBStructuredData())
        self.idx = idx
        self.wrapped = wrapped
        self.name = wrapped.GetFunctionName() or "<cpython>"

    def get_id(self):
        return self.idx

    def get_pc(self):
        return self.wrapped.GetPC()

    def get_cfa(self):
        return self.wrapped.GetCFA()

    def get_function_name(self):
        return self.name

    def get_symbol_context(self):
        return self.wrapped.GetSymbolContext(lldb.eSymbolContextEverything)

    def is_artificial(self):
        return False

    def is_hidden(self):
        return True

    def get_register_context(self):
        return _packed_registers(self.wrapped, self.get_register_info())


def _packed_registers(frame, register_info):
    """`frame`'s general-purpose registers, packed in register_info order."""
    registers = {}
    for register_set in frame.registers:
        if "general purpose" in register_set.name.lower():
            for register in register_set:
                registers[register.name] = (
                    int(register.value, 16) if register.value else 0,
                    register.GetByteSize(),
                )
            break
    if not registers:
        return None

    def read(entry):
        # A register set reports a register under the name lldb displays,
        # which can be an alias of the architectural name the register info
        # uses. The register info carries that alias in "alt-name".
        if entry["name"] in registers:
            return registers[entry["name"]]
        return registers.get(entry.get("alt-name"), (0, entry["bitsize"] // 8))

    sizes = {1: "B", 2: "H", 4: "I", 8: "Q"}
    layout = ""
    values = []
    for entry in register_info["registers"]:
        value, size = read(entry)
        layout += sizes[size]
        values.append(value)
    return struct.pack(layout, *values)


class PythonFrame(ScriptedFrame):
    """A synthetic frame standing in for one CPython interpreter frame."""

    def __init__(
        self,
        thread,
        idx,
        name,
        interpreter,
        frame_addr,
        filename,
        line,
        parent_addr=0,
        python_stepping=False,
    ):
        super().__init__(thread, lldb.SBStructuredData())
        self.idx = idx
        self.name = name
        self.interpreter = interpreter
        self.frame_addr = frame_addr
        self.parent_addr = parent_addr
        self.filename = filename
        self.line = line
        self.python_stepping = python_stepping

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
        # actual .py source. The file:line in `bt` also comes from here, not
        # the function name.
        if not self.filename:
            return None
        line_entry = lldb.SBLineEntry()
        line_entry.SetFileSpec(lldb.SBFileSpec(self.filename, True))
        line_entry.SetLine(self.line)
        symbol_context = lldb.SBSymbolContext()
        symbol_context.SetLineEntry(line_entry)
        return symbol_context

    def is_artificial(self):
        # lldb will not step out to an artificial frame, and these stand in for
        # real Python calls the user can return to.
        return False

    def is_hidden(self):
        return False

    def get_register_context(self):
        return None

    # -- variables ----------------------------------------------------------

    def get_variables(self, filters=None):
        """The Python frame's fast locals, decoded into SBValues.

        `filters` is accepted because the base class declares it, but the C++
        side dispatches this with no arguments.
        """
        if self.interpreter is None or not self.frame_addr:
            return None
        values = lldb.SBValueList()
        for name, obj in self.interpreter.fast_locals(self.frame_addr):
            value = self._make_value(name, obj)
            if value and value.IsValid():
                values.Append(value)
        return values if values.GetSize() else None

    def _make_value(self, name, obj):
        """Render one PyObject as a native SBValue.

        Python values have no C type to borrow, so the common scalars are
        projected onto the equivalent native type and everything else onto the
        text its repr would start with. That keeps `frame variable` readable
        without dragging a type system into an example.
        """
        if not obj:
            # CPython leaves the slot NULL for a local that is not bound yet.
            return self._string_value(name, "<unbound>")

        type_name = self.interpreter.type_name(obj) or "object"
        if type_name == "bool":
            value = self.interpreter.int_value(obj)
            if value is not None:
                return self._scalar_value(name, lldb.eBasicTypeBool, "B", value)
        elif type_name == "int":
            value = self.interpreter.int_value(obj)
            if value is not None:
                return self._scalar_value(name, lldb.eBasicTypeLongLong, "q", value)
        elif type_name == "float":
            value = self.interpreter.float_value(obj)
            if value is not None:
                return self._scalar_value(name, lldb.eBasicTypeDouble, "d", value)
        elif type_name == "str":
            text = self.interpreter.str_value(obj)
            if text is not None:
                return self._string_value(name, text)
        elif type_name == "bytes":
            raw = self.interpreter.bytes_value(obj)
            if raw is not None:
                return self._string_value(name, repr(raw))
        elif type_name == "NoneType":
            return self._string_value(name, "None")

        return self._string_value(name, "<%s object at 0x%x>" % (type_name, obj))

    def _scalar_value(self, name, basic_type, pack_format, value):
        target = self.interpreter.target
        raw = struct.pack("<" + pack_format, value)
        data = lldb.SBData()
        error = lldb.SBError()
        data.SetData(error, raw, target.GetByteOrder(), target.GetAddressByteSize())
        if error.Fail():
            return None
        return target.CreateValueFromData(name, data, target.GetBasicType(basic_type))

    def _string_value(self, name, text):
        target = self.interpreter.target
        raw = text.encode("utf-8", "replace")[:1024] + b"\0"
        data = lldb.SBData()
        error = lldb.SBError()
        data.SetData(error, raw, target.GetByteOrder(), target.GetAddressByteSize())
        if error.Fail():
            return None
        char_type = target.GetBasicType(lldb.eBasicTypeChar)
        return target.CreateValueFromData(name, data, char_type.GetArrayType(len(raw)))

    # -- stepping -----------------------------------------------------------

    def get_plan_spec_for_step_type(self, step_type):
        """Step in units of Python source lines instead of machine code.

        Off unless the provider was registered with `-k python_stepping -v
        true`, because `PythonStepPlan` is not usable yet; see the comment on
        that class. An empty class name means "use lldb's own algorithms".
        """
        if not self.python_stepping or not self.frame_addr:
            return {"class_name": ""}
        return {
            "class_name": "%s.PythonStepPlan" % __name__,
            "extra_args": {
                "step_type": int(step_type),
                "frame_addr": self.frame_addr,
                "parent_addr": self.parent_addr,
                "line": self.line,
            },
        }


# ---------------------------------------------------------------------------
# Stepping
# ---------------------------------------------------------------------------


def _structured_bool(args, key):
    if not args or not args.IsValid():
        return False
    value = args.GetValueForKey(key)
    if not value or not value.IsValid():
        return False
    return value.GetStringValue(16).lower() in ("true", "1", "yes")


def _structured_int(args, key):
    if not args or not args.IsValid():
        return 0
    value = args.GetValueForKey(key)
    return value.GetUnsignedIntegerValue(0) if value and value.IsValid() else 0


class PythonStepPlan(ScriptedThreadPlan):
    """Steps a thread by Python source lines rather than machine instructions.

    The interpreter tracks its progress through a code object in one word per
    frame (`f_lasti` before 3.11, `instr_ptr` after) and updates it as it
    executes, so "run until this Python frame reaches a new line" is exactly a
    write watchpoint on that word. That is both precise and cheap: the
    watchpoint only fires when *that* frame runs, so stepping over a call that
    executes millions of bytecodes costs no stops at all.

    - step-over watches the stepping frame (reaching a new line) and its caller
      (the frame returned).
    - step-in also stops when a different Python frame starts running, caught
      by watching the thread state's current-frame pointer where the layout
      knows where it is, and otherwise by breaking on the eval function, which
      pre-3.11 interpreters enter once per Python call.
    - step-out watches only the caller, which advances exactly once the
      stepping frame returns.

    NOT USABLE YET, and off unless the provider is registered with
    `-k python_stepping -v true`. The design needs a watchpoint that resumes
    the target instead of reporting a stop, so that the plan alone decides
    when the step is over, and SB has no way to ask for one: there is no
    `SBWatchpoint::SetShouldStop` and no watchpoint callback. Every
    intermediate hit therefore surfaces to the user as "stop reason =
    watchpoint N", and on arm64 the hardware slot keeps firing after
    `DeleteWatchpoint`, which shows up as a spurious `EXC_BREAKPOINT` on the
    following resume. The rest of the plan is exercised and correct: lldb does
    route the stops through `explains_stop`/`should_stop`, and the line and
    frame-return tests below decide correctly. Keeping it here documents the
    approach and leaves one API addition between this and working Python-level
    stepping.
    """

    def __init__(self, thread_plan, extra_args):
        super().__init__(thread_plan)
        self.thread = thread_plan.GetThread()
        self.process = self.thread.GetProcess()
        self.target = self.process.GetTarget()
        self.watchpoints = []
        self.breakpoints = []
        self.done = False
        self.interpreter = None

        self.step_type = _structured_int(extra_args, "step_type")
        self.frame_addr = _structured_int(extra_args, "frame_addr")
        self.parent_addr = _structured_int(extra_args, "parent_addr")
        self.start_line = _structured_int(extra_args, "line")

        self.interpreter = CPythonFrameProvider.interpreter_for(
            self.target, self.process
        )
        if self.interpreter is None:
            thread_plan.SetPlanComplete(False)
            return

        layout = self.interpreter.layout
        if layout.frame_kind == FRAMES_ON_HEAP:
            self.instr_field, self.instr_size = layout.frame_lasti, 4
        else:
            self.instr_field, self.instr_size = layout.frame_instr_ptr, 8

        # Sampling the caller now is what makes "the frame returned" testable
        # later, since the caller only advances once the callee is gone.
        self.parent_instr = (
            self._instruction_pointer(self.parent_addr) if self.parent_addr else 0
        )

        if self.step_type != lldb.eStepTypeOut:
            self._watch(self.frame_addr + self.instr_field, self.instr_size)
        if self.parent_addr:
            self._watch(self.parent_addr + self.instr_field, self.instr_size)
        if self.step_type == lldb.eStepTypeInto:
            self._watch_for_calls()

        if not self.watchpoints and not self.breakpoints:
            # With nothing to resume on, an incomplete plan would hang the
            # thread; report failure so lldb surfaces it to the user.
            thread_plan.SetPlanComplete(False)

    def _watch_for_calls(self):
        state = self.interpreter.thread_state_for(self.thread)
        if state:
            self._watch(state + self.interpreter.layout.tstate_frame, 8)
            return
        breakpoint = self.target.BreakpointCreateByName(EVAL_FRAME_SYMBOL)
        if breakpoint.IsValid() and breakpoint.GetNumLocations():
            breakpoint.SetThreadID(self.thread.GetThreadID())
            self.breakpoints.append(breakpoint.GetID())

    def _watch(self, address, size):
        error = lldb.SBError()
        options = lldb.SBWatchpointOptions()
        options.SetWatchpointTypeWrite(lldb.eWatchpointWriteTypeOnModify)
        watchpoint = self.target.WatchpointCreateByAddress(
            address, size, options, error
        )
        if error.Success() and watchpoint.IsValid():
            self.watchpoints.append(watchpoint.GetID())

    def _clear(self):
        for watchpoint_id in self.watchpoints:
            self.target.DeleteWatchpoint(watchpoint_id)
        for breakpoint_id in self.breakpoints:
            self.target.BreakpointDelete(breakpoint_id)
        self.watchpoints = []
        self.breakpoints = []

    def __del__(self):
        # The plan can be dropped without ever completing, and leaking a
        # hardware watchpoint would break the next step.
        try:
            self._clear()
        except Exception:
            pass

    # -- plan interface -----------------------------------------------------

    def explains_stop(self, event):
        reason = self.thread.GetStopReason()
        if not self.thread.GetStopReasonDataCount():
            return False
        stop_id = self.thread.GetStopReasonDataAtIndex(0)
        if reason == lldb.eStopReasonWatchpoint:
            return stop_id in self.watchpoints
        if reason == lldb.eStopReasonBreakpoint:
            return stop_id in self.breakpoints
        return False

    def is_stale(self):
        return self.done

    def should_step(self):
        return False

    def should_stop(self, event):
        if not self.explains_stop(event):
            return False

        if self.step_type == lldb.eStepTypeOut:
            stop = self._returned()
        elif self.step_type == lldb.eStepTypeInto and self._entered_new_frame():
            stop = True
        else:
            stop = self._reached_new_line() or self._returned()

        if stop:
            self._finish()
        return stop

    def _instruction_pointer(self, frame):
        return self.interpreter.reader.uint(frame + self.instr_field, self.instr_size)

    def _returned(self):
        """Whether the stepping frame has finished.

        Testing the frame itself does not work: CPython recycles frames, so
        the memory keeps decoding as a plausible frame after the call returns.
        The caller's instruction pointer, on the other hand, only moves once
        the callee is gone, which is exactly the event to wait for.
        """
        if not self.parent_addr:
            return False
        return self._instruction_pointer(self.parent_addr) != self.parent_instr

    def _entered_new_frame(self):
        state = self.interpreter.thread_state_for(self.thread)
        if state:
            frame = self.interpreter.reader.ptr(
                state + self.interpreter.layout.tstate_frame
            )
            return bool(frame) and frame != self.frame_addr
        # The eval breakpoint only fires on a new Python call, so reaching it
        # is the answer by itself.
        return self.thread.GetStopReason() == lldb.eStopReasonBreakpoint

    def _reached_new_line(self):
        info = self.interpreter.describe(self.frame_addr)
        return info is not None and info[2] != self.start_line

    def _finish(self):
        self.done = True
        self._clear()
        self.thread_plan.SetPlanComplete(True)

    def stop_description(self, stream):
        info = self.interpreter.describe(self.frame_addr) if self.interpreter else None
        if info:
            stream.Print("step to %s at %s:%d" % (info[1], info[0], info[2]))
        else:
            stream.Print("step out of Python frame")


# ---------------------------------------------------------------------------
# The provider
# ---------------------------------------------------------------------------


class CPythonFrameProvider(ScriptedFrameProvider):
    """Replace `_PyEval_EvalFrameDefault` C frames with Python-level frames."""

    # Resolving a layout costs a scan of every candidate, and it cannot change
    # over the life of a process, so the result is shared by the provider and
    # by the step plans it hands out.
    _interpreters = {}

    def __init__(self, input_frames, args):
        super().__init__(input_frames, args)
        # Output plan, one entry per output frame: either an int (pass the
        # input frame at that index through) or a PythonFrame. Built lazily.
        self._plan = None
        self.python_stepping = _structured_bool(args, "python_stepping")

    @staticmethod
    def get_description():
        return "Replace CPython interpreter C frames with Python-level frames"

    @staticmethod
    def applies_to_thread(thread):
        target = thread.GetProcess().GetTarget()
        # Checking for the symbol first keeps a target with no interpreter in
        # it from paying for a full unwind of every thread on every stop.
        if not target.FindSymbols(EVAL_FRAME_SYMBOL).GetSize():
            return False
        for frame in thread:
            if frame.GetFunctionName() == EVAL_FRAME_SYMBOL:
                return True
        return False

    @classmethod
    def interpreter_for(cls, target, process):
        """The `_Interpreter` for `process`, or None if it cannot be read."""
        key = process.GetProcessID()
        cached = cls._interpreters.get(key)
        if cached is not None:
            # A failure is only cached for the stop it happened on: the
            # interpreter may simply not have been initialised yet.
            interpreter, stop_id = cached
            if interpreter is not None or stop_id == process.GetStopID():
                return interpreter

        try:
            interpreter = resolve_interpreter(target, process)
        except ProviderError:
            interpreter = None
        cls._interpreters[key] = (interpreter, process.GetStopID())
        return interpreter

    def get_frame_at_index(self, index):
        if self._plan is None:
            self._plan = self._build_plan()
        if index < 0 or index >= len(self._plan):
            return None
        return self._plan[index]

    # -- plan construction --------------------------------------------------

    def _passthrough(self):
        return list(range(len(self.input_frames)))

    def _build_plan(self):
        interpreter = self.interpreter_for(self.target, self.process)
        if interpreter is None:
            return self._passthrough()

        chunks = interpreter.chunks_for_thread(self.thread)
        if not chunks:
            # Hiding the machinery while showing raw eval frames would produce
            # a stack that is neither the C nor the Python view of the thread.
            return self._passthrough()

        plan = []
        chunk_index = 0
        # A frame's caller is the next Python frame on the thread, which for
        # the oldest frame of a chunk lives in the chunk after it.
        ordered = [entry[0] for chunk in chunks for entry in chunk]
        callers = dict(zip(ordered, ordered[1:]))

        for index in range(len(self.input_frames)):
            frame = self.input_frames[index]
            name = frame.GetFunctionName()

            if name == EVAL_FRAME_SYMBOL:
                chunk = chunks[chunk_index] if chunk_index < len(chunks) else None
                chunk_index += 1
                if not chunk:
                    # Nothing decoded for this eval frame: show the raw C frame
                    # so the gap is visible instead of silently dropped.
                    plan.append(index)
                    continue
                for frame_addr, filename, function, line in chunk:
                    plan.append(
                        PythonFrame(
                            self.thread,
                            len(plan),
                            function,
                            interpreter,
                            frame_addr,
                            filename,
                            line,
                            parent_addr=callers.get(frame_addr, 0),
                            python_stepping=self.python_stepping,
                        )
                    )
            elif self._is_cpython_internal(frame):
                # Collapse the interpreter machinery between Python frames so
                # the backtrace reads as a pure Python call stack.
                plan.append(CollapsedFrame(self.thread, len(plan), frame))
            else:
                # Non-Python frame (liblldb, dyld, libsystem, ...).
                plan.append(index)
        return plan

    @staticmethod
    def _is_cpython_internal(frame):
        module = frame.GetModule()
        if not module:
            return False
        filename = module.GetFileSpec().GetFilename() or ""
        return filename == "Python" or filename.startswith("libpython")


def __lldb_init_module(debugger, internal_dict):
    # Registering here would make a plain `command script import` change what
    # `bt` prints, so the example only loads the classes and leaves the
    # decision to the user.
    print(
        "Loaded %s. Register it with:\n"
        "    target frame-provider register -C %s.CPythonFrameProvider"
        % (__name__, __name__)
    )
