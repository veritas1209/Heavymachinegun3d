#!/usr/bin/env python3
"""Patch the original ARM File::GetSize implementation for compatibility testing.

The 2009 implementation reads st_size from a hard-coded 32-bit struct stat
offset. On the VPhoneOS Android 7 runtime that value is observed as zero, so
MMappedFile::Open reaches mmap with a zero length and the resource image handle
is never created. This diagnostic replacement asks the game's own File::Seek
method for SEEK_END instead. No game logic or asset data is changed.
"""

from __future__ import annotations

import argparse
from pathlib import Path


FILE_GET_SIZE_OFFSET = 0x6F0C0
ORIGINAL = bytes.fromhex(
    "10 b5 9a b0 40 68 69 46 b4 f7 da ec 00 23 00 28 "
    "00 db 0c 9b 1a b0 18 1c 10 bd"
)

# Thumb instructions at 0x6f0c0:
#   push {r4, lr}
#   movs r1, #0
#   movs r2, #2
#   bl   0x6f0dc       ; File::Seek(0, SEEK_END)
#   pop  {r4, pc}
# followed by unreachable NOP padding to retain the original symbol size.
PATCHED = bytes.fromhex(
    "10 b5 00 21 02 22 00 f0 09 f8 10 bd "
    "00 bf 00 bf 00 bf 00 bf 00 bf 00 bf 00 bf"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("library", type=Path)
    args = parser.parse_args()

    data = bytearray(args.library.read_bytes())
    if len(ORIGINAL) != len(PATCHED):
        raise RuntimeError("internal patch length mismatch")
    if len(data) < FILE_GET_SIZE_OFFSET + len(ORIGINAL):
        raise RuntimeError(f"library is too short: {args.library}")

    actual = bytes(
        data[FILE_GET_SIZE_OFFSET : FILE_GET_SIZE_OFFSET + len(ORIGINAL)]
    )
    if actual == PATCHED:
        print(f"Already patched: {args.library}")
        return 0
    if actual != ORIGINAL:
        raise RuntimeError(
            "unexpected File::GetSize bytes at 0x6f0c0: " + actual.hex(" ")
        )
    if bytes(data).count(ORIGINAL) != 1:
        raise RuntimeError("expected the original File::GetSize sequence exactly once")

    data[FILE_GET_SIZE_OFFSET : FILE_GET_SIZE_OFFSET + len(PATCHED)] = PATCHED
    args.library.write_bytes(data)

    verify = args.library.read_bytes()
    actual = verify[FILE_GET_SIZE_OFFSET : FILE_GET_SIZE_OFFSET + len(PATCHED)]
    if actual != PATCHED or len(verify) != len(data):
        raise RuntimeError("post-write verification failed")

    print(
        f"Patched File::GetSize at 0x{FILE_GET_SIZE_OFFSET:x}: "
        f"{args.library} ({len(verify)} bytes unchanged)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
