#!/usr/bin/env python3
"""Disassemble selected ARM/Thumb functions from Heavy Gunner's libgame.so."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "python"))

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_LITTLE_ENDIAN, CS_MODE_THUMB, Cs
from elftools.elf.elffile import ELFFile


DEFAULT_MATCHES = (
    "startApp",
    "WrapperRenderer_nativeInit",
    "STATE_MAINROOM_Initialize",
    "STATE_MAINROOM_Process",
    "STATE_MAINROOM_Draw",
    "STATE_PLAY_Initialize",
    "STATE_PLAY_Process",
    "HG_GUN_RESOURCE_Load",
)


def safe_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "-", name).strip("-").lower()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "library",
        nargs="?",
        type=Path,
        default=ROOT / "extracted" / "apk" / "lib" / "armeabi" / "libgame.so",
    )
    parser.add_argument("--output", type=Path, default=ROOT / "analysis" / "native")
    parser.add_argument("--match", action="append", dest="matches")
    args = parser.parse_args()

    matches = tuple(args.matches or DEFAULT_MATCHES)
    args.output.mkdir(parents=True, exist_ok=True)

    with args.library.open("rb") as stream:
        elf = ELFFile(stream)
        dynsym = elf.get_section_by_name(".dynsym")
        text = elf.get_section_by_name(".text")
        if dynsym is None or text is None:
            raise RuntimeError("Required ELF sections are missing")

        text_address = int(text["sh_addr"])
        text_data = text.data()
        selected = [
            symbol
            for symbol in dynsym.iter_symbols()
            if int(symbol["st_size"]) > 0 and any(match in symbol.name for match in matches)
        ]

        for symbol in selected:
            tagged_address = int(symbol["st_value"])
            address = tagged_address & ~1
            size = int(symbol["st_size"])
            offset = address - text_address
            code = text_data[offset : offset + size]
            thumb = bool(tagged_address & 1)
            mode = (CS_MODE_THUMB if thumb else CS_MODE_ARM) | CS_MODE_LITTLE_ENDIAN
            decoder = Cs(CS_ARCH_ARM, mode)
            # ARM/Thumb functions may contain inline literal pools. Continue past
            # those data words so later branch targets in the same symbol are
            # still included in the report.
            decoder.skipdata = True
            label = next(match for match in matches if match in symbol.name)
            target = args.output / f"disasm-{safe_name(label)}-{address:08x}.txt"
            with target.open("w", encoding="utf-8", newline="\n") as report:
                report.write(f"symbol: {symbol.name}\n")
                report.write(f"address: 0x{address:08x}\n")
                report.write(f"size: {size}\n")
                report.write(f"mode: {'Thumb' if thumb else 'ARM'}\n\n")
                for instruction in decoder.disasm(code, address):
                    encoded = instruction.bytes.hex(" ")
                    report.write(
                        f"0x{instruction.address:08x}: {encoded:<14} "
                        f"{instruction.mnemonic:<9} {instruction.op_str}\n"
                    )
            print(f"{target.name}: {size} bytes, {'Thumb' if thumb else 'ARM'}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
