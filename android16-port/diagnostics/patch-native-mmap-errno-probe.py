#!/usr/bin/env python3
"""Expose MMappedFile::Open's mmap errno as the diagnostic crash address.

The original loader silently returns false when mmap returns MAP_FAILED. This
probe keeps the original call and branches only on that failure path. It then
loads errno and deliberately faults at that small numeric address, allowing the
existing Android tombstone to report the otherwise-hidden errno value.
"""

from __future__ import annotations

import argparse
from pathlib import Path


FAILURE_BRANCH_OFFSET = 0x6F604
FAILURE_BRANCH_ORIGINAL = bytes.fromhex("00 23")  # movs r3, #0
FAILURE_BRANCH_PATCHED = bytes.fromhex("09 e0")   # b 0x6f61a

ERRNO_FAULT_OFFSET = 0x6F61E
ERRNO_FAULT_ORIGINAL = bytes.fromhex("16 23 03 60")  # errno = EINVAL
ERRNO_FAULT_PATCHED = bytes.fromhex("00 68 00 68")   # errno -> fault address


def replace_exact(data: bytearray, offset: int, original: bytes, patched: bytes) -> None:
    if len(original) != len(patched):
        raise RuntimeError("internal patch length mismatch")
    actual = bytes(data[offset : offset + len(original)])
    if actual == patched:
        return
    if actual != original:
        raise RuntimeError(f"unexpected bytes at 0x{offset:x}: {actual.hex(' ')}")
    data[offset : offset + len(patched)] = patched


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("library", type=Path)
    args = parser.parse_args()

    data = bytearray(args.library.read_bytes())
    replace_exact(data, FAILURE_BRANCH_OFFSET, FAILURE_BRANCH_ORIGINAL, FAILURE_BRANCH_PATCHED)
    replace_exact(data, ERRNO_FAULT_OFFSET, ERRNO_FAULT_ORIGINAL, ERRNO_FAULT_PATCHED)
    args.library.write_bytes(data)

    verify = args.library.read_bytes()
    if (
        verify[FAILURE_BRANCH_OFFSET : FAILURE_BRANCH_OFFSET + 2] != FAILURE_BRANCH_PATCHED
        or verify[ERRNO_FAULT_OFFSET : ERRNO_FAULT_OFFSET + 4] != ERRNO_FAULT_PATCHED
    ):
        raise RuntimeError("post-write verification failed")

    print(f"Patched mmap failure errno probe: {args.library}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
