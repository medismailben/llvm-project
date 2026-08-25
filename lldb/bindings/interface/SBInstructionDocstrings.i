%feature("docstring",
"Represents a (machine language) instruction."
) lldb::SBInstruction;

%feature("docstring", "
    The bytes of code a copy of this instruction needs to behave the same way
    from a different address.

    The answer does not depend on where the copy ends up, so a caller can
    reserve room before choosing a destination. It is at least GetByteSize(),
    and larger when behaving the same somewhere else takes a sequence rather
    than a single instruction.

    Returns zero and sets error when this instruction cannot be moved out of
    line at any address. The error text explains why, phrased for a user."
) lldb::SBInstruction::GetRelocatedCodeSize;

%feature("docstring", "
    The bytes of constant data a copy of this instruction needs.

    This data is placed after the relocated code and never executed. Zero is
    also what a refusal reports, so check error rather than the result to tell
    the two apart."
) lldb::SBInstruction::GetRelocatedDataSize;

%feature("docstring", "
    The address this instruction refers to through a PC-relative operand,
    interpreted as if the instruction lived at pc. The result is in the same
    address domain as pc.

    This is what Relocate() has to be told to preserve. It is separate from
    Relocate() because a caller moving a whole range has to redirect a
    reference that lands inside that range to wherever that instruction's copy
    went, which only the caller knows.

    Returns lldb.LLDB_INVALID_ADDRESS when this instruction is not PC-relative,
    or when its target cannot be known without running the program, as is the
    case for an indirect branch."
) lldb::SBInstruction::GetReferencedAddress;

%feature("docstring", "
    Bytes that behave at 'to' as this instruction does at 'from', returned as
    an SBData described with the target's byte order.

    referenced_address is the address the copy has to keep referring to,
    normally GetReferencedAddress(from). It is ignored when this instruction
    refers to nothing, so lldb.LLDB_INVALID_ADDRESS is fine in that case.

    This can fail for an instruction GetRelocatedCodeSize() accepted: whether a
    relocated form exists at all does not depend on the destination, but whether
    it can reach its target from that destination does, and how far the copy
    moves is not known until a destination is chosen. ::

        error = lldb.SBError()
        referenced = inst.GetReferencedAddress(from_addr)
        data = inst.Relocate(target, from_addr, to_addr, referenced, error)
        if error.Success():
            code = bytearray(data.uint8s)"
) lldb::SBInstruction::Relocate;
