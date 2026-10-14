#!/usr/bin/env python3
"""Use the game's existing native millisecond clock on ART.

The original wrapper keeps a pointer returned by GetLongArrayElements and
expects later Java writes to the long[] to appear through that pointer. ART is
allowed to return a copy, leaving CS_knlCurrentTime permanently at its startup
value. Redirect it to the already shipped getCurrentTime(), which returns the
same wall-clock milliseconds from gettimeofday. No game timing or state logic
is changed.
"""

from __future__ import annotations

import argparse
from pathlib import Path


PATCH_OFFSET = 0x6B140
ORIGINAL = bytes.fromhex(
    "03 4b 7b 44 1b 68 1b 68 18 68 59 68 70 47 c0 46 e2 b4 04 00"
)

# Thumb code at 0x6b140:
#   push {r4, lr}
#   bl   getCurrentTime       ; 0x53008
#   pop  {r4, pc}
#   nop x 6
PATCHED = bytes.fromhex(
    "10 b5 e7 f7 61 ff 10 bd 00 bf 00 bf 00 bf 00 bf 00 bf 00 bf"
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
            f"unexpected CS_knlCurrentTime bytes at 0x{PATCH_OFFSET:x}: "
            f"{actual.hex(' ')}"
        )
    data[PATCH_OFFSET : PATCH_OFFSET + len(PATCHED)] = PATCHED
    args.library.write_bytes(data)
    if args.library.read_bytes()[PATCH_OFFSET : PATCH_OFFSET + len(PATCHED)] != PATCHED:
        raise RuntimeError("post-write verification failed")
    print(f"Patched ART-compatible native current time: {args.library}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
