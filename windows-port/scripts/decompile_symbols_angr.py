#!/usr/bin/env python3
"""Generate best-effort C-like output for selected libgame.so symbols with angr."""

from __future__ import annotations

import argparse
import logging
import re
from pathlib import Path

import angr


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SYMBOLS = (
    "pactLoadMesh",
    "pactLoadCamera",
    "pactLoadEffect",
    "pactLoadAction",
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
    parser.add_argument("--symbol", action="append", dest="symbols")
    args = parser.parse_args()

    logging.getLogger("angr").setLevel(logging.ERROR)
    logging.getLogger("cle").setLevel(logging.ERROR)
    args.output.mkdir(parents=True, exist_ok=True)

    project = angr.Project(str(args.library), auto_load_libs=False)
    cfg = project.analyses.CFGFast(normalize=True)
    requested = tuple(args.symbols or DEFAULT_SYMBOLS)

    for name in requested:
        matches = [function for function in cfg.kb.functions.values() if function.name == name]
        if not matches:
            print(f"missing: {name}")
            continue
        function = matches[0]
        decompiler = project.analyses.Decompiler(function, cfg=cfg)
        target = args.output / f"decompile-{safe_name(name)}-{function.addr:08x}.c"
        code = decompiler.codegen.text if decompiler.codegen is not None else "/* no code generated */\n"
        target.write_text(
            f"/* Best-effort binary reconstruction; not original source.\n"
            f" * Symbol: {name}\n * Address: 0x{function.addr:08x}\n"
            f" * CFG size: {function.size} bytes\n */\n\n{code}",
            encoding="utf-8",
            newline="\n",
        )
        print(f"{target.name}: {len(code)} characters")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
