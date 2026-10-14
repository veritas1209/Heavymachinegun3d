#!/usr/bin/env python3
"""Refresh the legacy JNI float[3] accelerometer bridge on ART.

The original 2009 code keeps the pointer returned by GetFloatArrayElements
for the lifetime of the process. Dalvik exposed Java writes through that
pointer, while ART may return a detached copy. WrapperRenderer sends each raw
float as two independently tagged 16-bit nativeRender(int) packets. Tags use
the 0xE user-address range, which cannot be a valid 32-bit Android timer
pointer. The thunk updates one halfword per packet without retaining transient
state. Normal timer calls take a register-exact path through the original
nativeRender body; input dispatch and redraw remain unchanged.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


NATIVE_RENDER_OFFSET = 0x502E0
ORIGINAL_NATIVE_RENDER_HEAD = bytes.fromhex("10 b5 10 1c")
PATCHED_NATIVE_RENDER_HEAD = bytes.fromhex("65 f0 92 bb")

# Use the zero-filled alignment gap after the read/execute LOAD segment,
# between .ARM.exidx and the next page-aligned read/write LOAD segment.  This
# is not part of an original function or data object.  Extending the first
# segment by only the thunk length makes these existing file bytes executable
# without moving any section, relocation, or following segment.
CAVE_OFFSET = 0xB5A08
ORIGINAL_CAVE = bytes(64)
PATCHED_CAVE = bytes.fromhex(
    # Packet tag check.  Normal timer pointers never enter the special path.
    "10 1c 00 0f 0e 28 03 d0 "
    # Register-exact original nativeRender prologue and calls.
    "10 b5 10 1c 9a f7 66 bc "
    # Locate the detached jgyro buffer through its GOT entry.
    "06 48 78 44 00 68 00 68 "
    # Bits 18..16 select one of six halfwords; bits 15..0 are the payload.
    "13 46 1b 0c 07 21 0b 40 5b 00 "
    "12 04 12 0c c2 52 70 47 "
    # Alignment, GOT-relative delta, and unused zero-filled tail.
    "00 bf fa 0c 00 00 "
    "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
)

PT_LOAD = 1
LOAD_RX_FLAGS = 5
ORIGINAL_RX_LOAD_SIZE = CAVE_OFFSET
PATCHED_RX_LOAD_SIZE = CAVE_OFFSET + len(PATCHED_CAVE)

JGYRO_GOT_OFFSET = 0xB6718
EXPECTED_JGYRO_ADDRESS = 0x006E2578


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("library", type=Path)
    args = parser.parse_args()

    if len(ORIGINAL_NATIVE_RENDER_HEAD) != len(PATCHED_NATIVE_RENDER_HEAD):
        raise RuntimeError("nativeRender entry patch length mismatch")
    if len(ORIGINAL_CAVE) != len(PATCHED_CAVE):
        raise RuntimeError("gyro thunk patch length mismatch")

    data = bytearray(args.library.read_bytes())
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise RuntimeError("expected a little-endian ELF32 library")

    phoff = struct.unpack_from("<I", data, 0x1C)[0]
    phentsize = struct.unpack_from("<H", data, 0x2A)[0]
    phnum = struct.unpack_from("<H", data, 0x2C)[0]
    rx_load_header = None
    rx_load_size = None
    for index in range(phnum):
        offset = phoff + index * phentsize
        fields = struct.unpack_from("<IIIIIIII", data, offset)
        p_type, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz, p_flags, _p_align = fields
        if p_type == PT_LOAD and p_offset == 0 and p_vaddr == 0 and p_flags == LOAD_RX_FLAGS:
            if p_filesz != p_memsz:
                raise RuntimeError("read/execute LOAD file and memory sizes differ")
            rx_load_header = offset
            rx_load_size = p_filesz
            break
    if rx_load_header is None:
        raise RuntimeError("read/execute LOAD segment was not found")

    render_head = bytes(
        data[
            NATIVE_RENDER_OFFSET : NATIVE_RENDER_OFFSET
            + len(ORIGINAL_NATIVE_RENDER_HEAD)
        ]
    )
    cave = bytes(data[CAVE_OFFSET : CAVE_OFFSET + len(ORIGINAL_CAVE)])

    if (
        render_head == PATCHED_NATIVE_RENDER_HEAD
        and cave == PATCHED_CAVE
        and rx_load_size == PATCHED_RX_LOAD_SIZE
    ):
        print(f"Already patched: {args.library}")
        return 0
    if render_head != ORIGINAL_NATIVE_RENDER_HEAD:
        raise RuntimeError(
            f"unexpected nativeRender bytes at 0x{NATIVE_RENDER_OFFSET:x}: "
            f"{render_head.hex(' ')}"
        )
    if cave != ORIGINAL_CAVE:
        raise RuntimeError(
            f"unexpected gyro thunk bytes at 0x{CAVE_OFFSET:x}: {cave.hex(' ')}"
        )
    if rx_load_size != ORIGINAL_RX_LOAD_SIZE:
        raise RuntimeError(
            f"unexpected read/execute LOAD size: 0x{rx_load_size:x}"
        )

    got_value = int.from_bytes(data[JGYRO_GOT_OFFSET : JGYRO_GOT_OFFSET + 4], "little")
    if got_value != EXPECTED_JGYRO_ADDRESS:
        raise RuntimeError(
            f"unexpected jgyroPtr GOT value: 0x{got_value:08x}"
        )

    data[
        NATIVE_RENDER_OFFSET : NATIVE_RENDER_OFFSET + len(PATCHED_NATIVE_RENDER_HEAD)
    ] = PATCHED_NATIVE_RENDER_HEAD
    data[CAVE_OFFSET : CAVE_OFFSET + len(PATCHED_CAVE)] = PATCHED_CAVE
    struct.pack_into("<I", data, rx_load_header + 16, PATCHED_RX_LOAD_SIZE)
    struct.pack_into("<I", data, rx_load_header + 20, PATCHED_RX_LOAD_SIZE)
    args.library.write_bytes(data)

    verify = args.library.read_bytes()
    if (
        verify[
            NATIVE_RENDER_OFFSET : NATIVE_RENDER_OFFSET
            + len(PATCHED_NATIVE_RENDER_HEAD)
        ]
        != PATCHED_NATIVE_RENDER_HEAD
    ):
        raise RuntimeError("nativeRender post-write verification failed")
    if verify[CAVE_OFFSET : CAVE_OFFSET + len(PATCHED_CAVE)] != PATCHED_CAVE:
        raise RuntimeError("gyro thunk post-write verification failed")
    if struct.unpack_from("<I", verify, rx_load_header + 16)[0] != PATCHED_RX_LOAD_SIZE:
        raise RuntimeError("read/execute LOAD file-size verification failed")
    if struct.unpack_from("<I", verify, rx_load_header + 20)[0] != PATCHED_RX_LOAD_SIZE:
        raise RuntimeError("read/execute LOAD memory-size verification failed")
    print(f"Patched ART JNI float[3] accelerometer synchronization: {args.library}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
