#!/usr/bin/env python3
"""Inspect Heavy Gunner's live appEnemy records dumped from Android memory."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


APP_ENEMY_IN_PAGE_DUMP = 0xE98
GROUP_BASE = APP_ENEMY_IN_PAGE_DUMP + 0x44
CHILD_BASE = GROUP_BASE + 0x1E0
CHILD_STRIDE = 0x118


def u8(data: bytes, offset: int) -> int:
    return data[offset]


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def s32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def q16(data: bytes, offset: int) -> float:
    return s32(data, offset) / 65536.0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--children", type=int, default=16)
    args = parser.parse_args()

    for path in args.files:
        data = path.read_bytes()
        group = GROUP_BASE
        print(
            f"{path.name}: active={u8(data, group)} live={s32(data, group + 4)} "
            f"type={u8(data, group + 9)} pattern={u16(data, group + 0xA)} "
            f"enemy={u16(data, group + 0xC)} total={s32(data, group + 0x12C)} "
            f"base=({q16(data, group + 0x2C):.3f},"
            f"{q16(data, group + 0x30):.3f},{q16(data, group + 0x34):.3f}) "
            f"clock={s32(data, group + 0x5C)} speed={s32(data, group + 0x60)} "
            f"segment={s32(data, group + 0x64)} angle={s32(data, group + 0xB0)}"
        )
        rows = []
        entry_fraction = min(1.0, max(0.0, s32(data, group + 0x5C) /
                                      max(1, s32(data, group + 0x60))))
        for index in range(args.children):
            child = CHILD_BASE + CHILD_STRIDE * index
            if child + CHILD_STRIDE > len(data):
                break
            if not u8(data, child):
                continue
            target = ""
            if s32(data, child + 0xCC) == 0 and entry_fraction > 0.0:
                spawn = [q16(data, child + offset) for offset in (0xC, 0x10, 0x14)]
                current = [q16(data, child + offset) for offset in (0x18, 0x1C, 0x20)]
                endpoint = [start + (value - start) / entry_fraction
                            for start, value in zip(spawn, current)]
                target = (f" target=({endpoint[0]:7.2f},{endpoint[1]:7.2f},"
                          f"{endpoint[2]:7.2f})")
            elif s32(data, child + 0xCC) == 3:
                radius = q16(data, child + 0x98)
                origin_x = q16(data, child + 0xC)
                origin_z = q16(data, child + 0x14)
                if radius > 0.0:
                    start_fraction = 1.0 - ((origin_x * origin_x + origin_z * origin_z) ** 0.5 / radius)
                    target = f" attackStart={start_fraction * s32(data, group + 0x60) / 1000.0:6.3f}s"
            rows.append(
                f" {index:02d}:slot={u16(data, child + 2):03d} state={s32(data, child + 0xCC)} "
                f"t={s32(data, child + 0x48):7d}/{s32(data, child + 0x4C):7d} "
                f"xyz=({q16(data, child + 0x18):8.3f},"
                f"{q16(data, child + 0x1C):8.3f},{q16(data, child + 0x20):8.3f}) "
                f"origin=({q16(data, child + 0xC):8.3f},"
                f"{q16(data, child + 0x10):8.3f},{q16(data, child + 0x14):8.3f}) "
                f"phase=({q16(data, child + 0x5C):6.2f},{q16(data, child + 0x60):6.2f}) "
                f"motion=({q16(data, child + 0x98):7.2f},"
                f"{q16(data, child + 0x9C):7.2f},{q16(data, child + 0xA0):7.2f},"
                f"{q16(data, child + 0xA4):7.2f},{q16(data, child + 0xA8):7.2f},"
                f"{q16(data, child + 0xAC):7.2f},{q16(data, child + 0xB0):7.2f},"
                f"{q16(data, child + 0xB4):7.2f},{q16(data, child + 0xB8):7.2f},"
                f"{q16(data, child + 0xBC):7.2f}) "
                f"dir={struct.unpack_from('<b', data, child + 0xD0)[0]:2d} "
                f"distance=({q16(data, child + 0xD4):7.2f},"
                f"{q16(data, child + 0xD8):7.2f}){target}"
                f" angleOffset={s32(data, child + 0xA4):d}"
            )
        print("\n".join(rows))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
