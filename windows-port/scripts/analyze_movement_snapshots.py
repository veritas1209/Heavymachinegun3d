"""Compare captured TAG_HG_ENEMYGROUP snapshots without mutating them."""

from pathlib import Path
import struct
import sys


def main() -> None:
    paths = [Path(arg) for arg in sys.argv[1:]]
    if len(paths) < 2:
        raise SystemExit("usage: analyze_movement_snapshots.py SNAPSHOT...")
    snapshots = [path.read_bytes() for path in paths]
    if len({len(data) for data in snapshots}) != 1:
        raise SystemExit("snapshot sizes differ")

    changed = [
        offset
        for offset in range(len(snapshots[0]))
        if len({data[offset] for data in snapshots}) > 1
    ]
    ranges: list[tuple[int, int]] = []
    if changed:
        start = previous = changed[0]
        for offset in changed[1:]:
            if offset != previous + 1:
                ranges.append((start, previous))
                start = offset
            previous = offset
        ranges.append((start, previous))

    print(f"changed bytes: {len(changed)}, ranges: {len(ranges)}")
    for start, end in ranges:
        print(f"  0x{start:04x}-0x{end:04x} ({end - start + 1})")

    aligned = sorted({offset & ~3 for offset in changed})
    print(f"changed aligned dwords: {len(aligned)}")
    for offset in aligned:
        values = []
        for data in snapshots:
            signed = struct.unpack_from("<i", data, offset)[0]
            unsigned = struct.unpack_from("<I", data, offset)[0]
            values.append(f"{signed:11d} (0x{unsigned:08x})")
        print(f"  0x{offset:04x}: " + " | ".join(values))


if __name__ == "__main__":
    main()
