#!/usr/bin/env python3
"""Turn CS_knlGetResource failures into a distinctive 0x12 fault.

LoadFile ignores CS_knlGetResource's -18 status and continues into image
creation. This diagnostic changes only the two native error paths; successful
resource reads execute the original instructions unchanged.
"""

from __future__ import annotations

import argparse
from pathlib import Path


PATCHES = (
    (0x6B378, bytes.fromhex("12 25 6d 42"), bytes.fromhex("12 20 00 68")),
    (0x6B3BE, bytes.fromhex("dd e7"), bytes.fromhex("db e7")),
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("library", type=Path)
    args = parser.parse_args()

    data = bytearray(args.library.read_bytes())
    for offset, original, patched in PATCHES:
        actual = bytes(data[offset : offset + len(original)])
        if actual == patched:
            continue
        if actual != original:
            raise RuntimeError(f"unexpected bytes at 0x{offset:x}: {actual.hex(' ')}")
        data[offset : offset + len(patched)] = patched
    args.library.write_bytes(data)

    verify = args.library.read_bytes()
    for offset, _, patched in PATCHES:
        if verify[offset : offset + len(patched)] != patched:
            raise RuntimeError(f"post-write verification failed at 0x{offset:x}")

    print(f"Patched CS_knlGetResource failure probe: {args.library}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
