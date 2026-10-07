#!/usr/bin/env python3
"""Write a copy of the Warband 1.174 macOS binary with hook entry points.

Each hooked function's first 6 bytes (push rbp; mov rbp,rsp; push r15) are
replaced by `jmp qword ptr [rip+slot]`, where the slots are 8-byte pointers in
unused zero-filled space at the end of the __DATA segment.  fastswap.dylib
fills the slots at startup.  Patching on disk (rather than in memory) lets
Rosetta translate the patched binary ahead of time once and cache it; writing
to code pages at runtime makes it fall back to slower JIT translation.

The patched binary must only be run with fastswap.dylib loaded (the wrapper
takes care of that): with empty slots the hooked functions would jump to 0.

usage: make_patched.py <original binary> <output>
"""
import struct
import sys

# Must match HOOKS in src/patch.c (same order: slot i belongs to hook i).
HOOKS = [
    0x100904054,  # texture loader (texhook.c)
    0x1009E1750,  # loading state machine step (loadstep.c)
    0x100923954,  # case-insensitive fopen (fopenhook.c)
    0x100A454B4,  # WaitForEvent (loadstep.c)
]
SLOT_BASE = 0x103B92B90  # past the end of __DATA,__common (0x103b92b88), segment ends 0x103b93000
TEXT_VMADDR = 0x100000000  # __TEXT fileoff 0
PROLOGUE = bytes([0x55, 0x48, 0x89, 0xE5, 0x41, 0x57])  # push rbp; mov rbp,rsp; push r15
PROLOGUE_REST = bytes([0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53])  # push r14..r12; push rbx


def main(src, dst):
    data = bytearray(open(src, "rb").read())
    for i, addr in enumerate(HOOKS):
        off = addr - TEXT_VMADDR
        if data[off:off + 13] != PROLOGUE + PROLOGUE_REST:
            sys.exit(f"unexpected bytes at {addr:#x}: not the expected 1.174 binary")
        disp = (SLOT_BASE + 8 * i) - (addr + 6)
        data[off:off + 6] = b"\xff\x25" + struct.pack("<i", disp)
    open(dst, "wb").write(data)


if __name__ == "__main__":
    main(*sys.argv[1:])
