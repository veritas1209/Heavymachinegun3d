#!/usr/bin/env python3
"""Patch Heavy Gunner's WrapperRenderer.onDrawFrame with the ART gyro bridge.

The game owns the input file. This tool only injects the small compatibility
bridge developed for this port; it does not contain the original class.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re


METHOD = r'''.method public onDrawFrame(Ljavax/microedition/khronos/opengles/GL10;)V
    .registers 7
    .param p1, "gl"    # Ljavax/microedition/khronos/opengles/GL10;

    .prologue
    .line 400
    iget-boolean v0, p0, Lcom/com2us/wrapper/WrapperRenderer;->isPaused:Z

    if-eqz v0, :cond_5

    .line 409
    :cond_4
    :goto_4
    return-void

    .line 403
    :cond_5
    invoke-virtual {p0}, Lcom/com2us/wrapper/WrapperRenderer;->process()V

    .line 405
    iget-boolean v0, p0, Lcom/com2us/wrapper/WrapperRenderer;->isPaused:Z

    if-nez v0, :cond_4

    .line 408
    iget-object v0, p0, Lcom/com2us/wrapper/WrapperRenderer;->activity:Lcom/com2us/wrapper/WrapperActivity;

    invoke-virtual {v0}, Lcom/com2us/wrapper/WrapperActivity;->getGyroManager()Lcom/com2us/wrapper/GyroManager;

    move-result-object v0

    invoke-virtual {v0}, Lcom/com2us/wrapper/GyroManager;->getGyro()[F

    move-result-object v0

    const/4 v1, 0x0

    aget v1, v0, v1

    const/4 v2, 0x1

    aget v2, v0, v2

    const/4 v3, 0x2

    aget v3, v0, v3

    invoke-static {v1}, Ljava/lang/Float;->floatToRawIntBits(F)I

    move-result v1

    invoke-static {v2}, Ljava/lang/Float;->floatToRawIntBits(F)I

    move-result v2

    invoke-static {v3}, Ljava/lang/Float;->floatToRawIntBits(F)I

    move-result v3

    int-to-char v0, v1
    const v4, -0x20000000
    or-int/2addr v0, v4
    invoke-static {v0}, Lcom/com2us/wrapper/WrapperRenderer;->nativeRender(I)V

    ushr-int/lit8 v0, v1, 0x10
    const v4, -0x1fff0000
    or-int/2addr v0, v4
    invoke-static {v0}, Lcom/com2us/wrapper/WrapperRenderer;->nativeRender(I)V

    int-to-char v0, v2
    const v4, -0x1ffe0000
    or-int/2addr v0, v4
    invoke-static {v0}, Lcom/com2us/wrapper/WrapperRenderer;->nativeRender(I)V

    ushr-int/lit8 v0, v2, 0x10
    const v4, -0x1ffd0000
    or-int/2addr v0, v4
    invoke-static {v0}, Lcom/com2us/wrapper/WrapperRenderer;->nativeRender(I)V

    int-to-char v0, v3
    const v4, -0x1ffc0000
    or-int/2addr v0, v4
    invoke-static {v0}, Lcom/com2us/wrapper/WrapperRenderer;->nativeRender(I)V

    ushr-int/lit8 v0, v3, 0x10
    const v4, -0x1ffb0000
    or-int/2addr v0, v4
    invoke-static {v0}, Lcom/com2us/wrapper/WrapperRenderer;->nativeRender(I)V

    invoke-static {}, Lcom/com2us/wrapper/WrapperRenderer;->nativeReDraw()V

    goto :goto_4
.end method'''


PATTERN = re.compile(
    r"(?ms)^\.method public onDrawFrame\(Ljavax/microedition/khronos/opengles/GL10;\)V\r?\n"
    r".*?^\.end method"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("smali", type=Path, help="path to WrapperRenderer.smali")
    args = parser.parse_args()

    path: Path = args.smali
    source = path.read_text(encoding="utf-8")
    if "const v4, -0x1ffb0000" in source:
        print(f"Already patched: {path}")
        return 0

    patched, count = PATTERN.subn(METHOD, source, count=1)
    if count != 1:
        raise SystemExit("Expected exactly one WrapperRenderer.onDrawFrame method")

    path.write_text(patched, encoding="utf-8", newline="\n")
    print(f"Patched gyro bridge: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
