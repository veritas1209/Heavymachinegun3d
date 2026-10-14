#!/usr/bin/env python3
"""Losslessly rewrite paletted PNG assets as RGBA PNGs.

Heavy Gunner disguises PNG assets with a trailing .jpg extension. The filename
is preserved; only the PNG color type changes from indexed palette to RGBA.
Decoded RGBA bytes are hashed before and after each rewrite to prevent an
accidental visual change.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path

from PIL import Image


def rgba_digest(image: Image.Image) -> str:
    rgba = image.convert("RGBA")
    payload = rgba.tobytes()
    size = rgba.size[0].to_bytes(4, "little") + rgba.size[1].to_bytes(4, "little")
    return hashlib.sha256(size + payload).hexdigest()


def convert(path: Path) -> bool:
    with Image.open(path) as source:
        source.load()
        if source.format != "PNG" or source.mode != "P":
            return False
        expected_digest = rgba_digest(source)
        rgba = source.convert("RGBA")
        dpi = source.info.get("dpi")

    temporary = path.with_name(path.name + ".rgba.tmp")
    save_options: dict[str, object] = {"format": "PNG", "compress_level": 6}
    if dpi is not None:
        save_options["dpi"] = dpi
    rgba.save(temporary, **save_options)

    with Image.open(temporary) as check:
        check.load()
        actual_digest = rgba_digest(check)
        if check.format != "PNG" or check.mode != "RGBA":
            raise RuntimeError(f"Unexpected rewritten format for {path}: {check.format}/{check.mode}")
    if actual_digest != expected_digest:
        raise RuntimeError(f"Decoded pixel mismatch after rewriting {path}")

    os.replace(temporary, path)
    print(f"RGBA {path} pixels={expected_digest}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("roots", nargs="+", type=Path)
    args = parser.parse_args()

    converted = 0
    scanned = 0
    for root in args.roots:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            lower_name = path.name.lower()
            if not (lower_name.endswith(".png") or lower_name.endswith(".png.jpg")):
                continue
            scanned += 1
            if convert(path):
                converted += 1

    print(f"Converted {converted} paletted PNG files; scanned {scanned} files.")
    if converted == 0:
        raise RuntimeError("No paletted PNG assets were converted")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
