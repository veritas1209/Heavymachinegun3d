#!/usr/bin/env python3
"""Annotate PC-relative Thumb literals in a symbol from an ELF library."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_LITTLE_ENDIAN, CS_MODE_THUMB, Cs
from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_REG_PC
from elftools.elf.elffile import ELFFile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("library", type=Path)
    parser.add_argument("symbol")
    parser.add_argument("--start", type=lambda value: int(value, 0))
    parser.add_argument("--end", type=lambda value: int(value, 0))
    args = parser.parse_args()

    with args.library.open("rb") as stream:
        elf = ELFFile(stream)
        dynsym = elf.get_section_by_name(".dynsym")
        symbol = next((item for item in dynsym.iter_symbols() if item.name == args.symbol), None)
        if symbol is None:
            raise SystemExit(f"symbol not found: {args.symbol}")
        tagged = int(symbol["st_value"])
        address = tagged & ~1
        size = int(symbol["st_size"])
        start = args.start if args.start is not None else address
        end = args.end if args.end is not None else address + size

        load_segments = [segment for segment in elf.iter_segments() if segment["p_type"] == "PT_LOAD"]

        def read_word(virtual_address: int) -> int | None:
            for segment in load_segments:
                low = int(segment["p_vaddr"])
                high = low + int(segment["p_filesz"])
                if low <= virtual_address and virtual_address + 4 <= high:
                    stream.seek(int(segment["p_offset"]) + virtual_address - low)
                    return struct.unpack("<I", stream.read(4))[0]
            return None

        text = elf.get_section_by_name(".text")
        code_offset = start - int(text["sh_addr"])
        code = text.data()[code_offset : code_offset + (end - start)]
        decoder = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN)
        decoder.detail = True
        decoder.skipdata = True
        for instruction in decoder.disasm(code, start):
            note = ""
            if instruction.mnemonic.startswith("ldr") and len(instruction.operands) >= 2:
                operand = instruction.operands[1]
                if operand.type == ARM_OP_MEM and operand.mem.base == ARM_REG_PC:
                    literal_address = ((instruction.address + 4) & ~3) + operand.mem.disp
                    value = read_word(literal_address)
                    if value is not None:
                        note = f" ; [0x{literal_address:08x}] = 0x{value:08x} ({value})"
            encoded = instruction.bytes.hex(" ")
            print(
                f"0x{instruction.address:08x}: {encoded:<14} "
                f"{instruction.mnemonic:<9} {instruction.op_str}{note}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
