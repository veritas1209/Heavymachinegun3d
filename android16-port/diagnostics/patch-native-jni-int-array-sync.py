#!/usr/bin/env python3
"""Make the legacy JNI int[] out-parameter work on ART.

The 2009 bridge keeps a GetIntArrayElements pointer forever and assumes Java
writes to the array are immediately visible through it. ART may return a copy,
so asset offset/length remain zero. After the original Java call, this patch
gets a fresh view, copies both ints to the caller, and releases it with
JNI_ABORT. The Java method and resource values are otherwise unchanged.
"""

from __future__ import annotations

import argparse
from pathlib import Path


PATCH_OFFSET = 0x5219C
ORIGINAL = bytes.fromhex(
    "2b 6f 81 46 1a 68 32 60 5b 68 73 60 43 46 00 2b "
    "2c d1 69 6e 00 29 19 d0 25 68 35 4a 35 4b bc 20 "
    "40 00 7a 44 2d 58 7b 44 20 1c a8 47 02 1e 1d d0 "
    "48 46 00 28"
)

# Thumb code at 0x5219c:
#   preserve the Java return object in sb
#   GetIntArrayElements(global int[2], NULL)
#   copy offset and length to the native caller's buffer
#   ReleaseIntArrayElements(..., JNI_ABORT)
#   return 0 through the original local-reference cleanup at 0x5220a
PATCHED = bytes.fromhex(
    "81 46 22 68 bb 23 9b 00 d3 58 e9 6e 20 1c 00 22 "
    "98 47 80 46 02 68 32 60 43 68 73 60 22 68 c3 23 "
    "9b 00 d3 58 9c 46 20 1c e9 6e 42 46 02 23 e0 47 "
    "00 25 1c e0"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("library", type=Path)
    args = parser.parse_args()

    if len(ORIGINAL) != len(PATCHED):
        raise RuntimeError("internal patch length mismatch")
    data = bytearray(args.library.read_bytes())
    actual = bytes(data[PATCH_OFFSET : PATCH_OFFSET + len(ORIGINAL)])
    if actual == PATCHED:
        print(f"Already patched: {args.library}")
        return 0
    if actual != ORIGINAL:
        raise RuntimeError(
            f"unexpected JNI bridge bytes at 0x{PATCH_OFFSET:x}: {actual.hex(' ')}"
        )
    data[PATCH_OFFSET : PATCH_OFFSET + len(PATCHED)] = PATCHED
    args.library.write_bytes(data)

    verify = args.library.read_bytes()
    if verify[PATCH_OFFSET : PATCH_OFFSET + len(PATCHED)] != PATCHED:
        raise RuntimeError("post-write verification failed")
    print(f"Patched ART JNI int[] synchronization: {args.library}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
