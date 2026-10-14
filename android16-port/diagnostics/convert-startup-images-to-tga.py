#!/usr/bin/env python3
"""Rewrite only the startup PNG payloads as pixel-identical RGBA TGA.

The original filenames are retained because Heavy Gunner detects image formats
from their byte signatures rather than their extensions. This is a diagnostic
for separating the bundled PNG decoder from the common image/GL path.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path

from PIL import Image


TARGETS = {
    "Logo.png.jpg",
    "UI_loading_eng.png.jpg",
    "Logo.png",
    "UI_loading_kor.png",
}


def rgba_digest(image: Image.Image) -> str:
    rgba = image.convert("RGBA")
    size = rgba.width.to_bytes(4, "little") + rgba.height.to_bytes(4, "little")
    return hashlib.sha256(size + rgba.tobytes()).hexdigest()


def convert(path: Path) -> None:
    with Image.open(path) as source:
        source.load()
        if source.format != "PNG":
            raise RuntimeError(f"Expected PNG payload: {path} ({source.format})")
        expected = rgba_digest(source)
        rgba = source.convert("RGBA")

    temporary = path.with_name(path.name + ".tga.tmp")
    rgba.save(temporary, format="TGA", compression=None)
    with Image.open(temporary) as check:
        check.load()
        actual = rgba_digest(check)
        if check.format != "TGA" or check.mode != "RGBA":
            raise RuntimeError(f"Unexpected TGA result: {path} ({check.format}/{check.mode})")
    if actual != expected:
        raise RuntimeError(f"Decoded pixel mismatch after TGA rewrite: {path}")

    os.replace(temporary, path)
    print(f"TGA {path} pixels={expected}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("roots", nargs="+", type=Path)
    args = parser.parse_args()

    converted = 0
    for root in args.roots:
        for name in sorted(TARGETS):
            path = root / name
            if path.is_file():
                convert(path)
                converted += 1
    if converted != 4:
        raise RuntimeError(f"Expected four startup images, converted {converted}")
    print("Converted four startup images to pixel-identical RGBA TGA payloads.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
