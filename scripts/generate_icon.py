#!/usr/bin/env python3
"""Render the approved SVG into a multi-resolution Windows icon."""
from pathlib import Path
import struct
import subprocess


root = Path(__file__).resolve().parent.parent
source = root / "docs/images/logo.svg"
sizes = (16, 20, 24, 32, 40, 48, 64, 128, 256)
header = struct.pack("<HHH", 0, 1, len(sizes))
offset = len(header) + 16 * len(sizes)
entries, images = [], []
for size in sizes:
    png = subprocess.check_output(
        ["rsvg-convert", "-w", str(size), "-h", str(size), str(source)], timeout=15
    )
    entries.append(
        struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(png), offset)
    )
    images.append(png)
    offset += len(png)
(root / "src/shiguang.ico").write_bytes(header + b"".join(entries + images))
