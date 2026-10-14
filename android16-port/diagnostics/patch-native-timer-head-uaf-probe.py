#!/usr/bin/env python3
"""Fix the head-node use-after-free in the original TM_Run.

TM_Run removes the head node, frees it, and then dereferences that freed node's
next pointer. The replacement uses the game's own TM_pop_front and TM_front
helpers around the original free, preserving traversal and ownership without a
leak.
"""

from __future__ import annotations

import argparse
from pathlib import Path


PATCH_OFFSET = 0x6C108
ORIGINAL = bytes.fromhex(
    "42 46 bb 58 a2 69 20 1c 1a 60 b7 f7 ec ec 23 1c d2 e7 c0 46"
)

# Thumb code at 0x6c108:
#   mov  r0, r4
#   bl   TM_pop_front
#   blx  free
#   bl   TM_front
#   mov  r3, r0
#   b    0x6c0c0
#   nop
PATCHED = bytes.fromhex(
    "20 1c ff f7 bd ff b7 f7 ee ec ff f7 b1 ff 03 1c d2 e7 00 bf"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("library", type=Path)
    args = parser.parse_args()

    data = bytearray(args.library.read_bytes())
    actual = bytes(data[PATCH_OFFSET : PATCH_OFFSET + len(ORIGINAL)])
    if actual == PATCHED:
        print(f"Already patched: {args.library}")
        return 0
    if actual != ORIGINAL:
        raise RuntimeError(
            f"unexpected TM_Run bytes at 0x{PATCH_OFFSET:x}: {actual.hex(' ')}"
        )
    data[PATCH_OFFSET : PATCH_OFFSET + len(PATCHED)] = PATCHED
    args.library.write_bytes(data)
    if args.library.read_bytes()[PATCH_OFFSET : PATCH_OFFSET + len(PATCHED)] != PATCHED:
        raise RuntimeError("post-write verification failed")
    print(f"Patched TM_Run head-node UAF: {args.library}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
