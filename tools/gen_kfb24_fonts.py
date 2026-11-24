#!/usr/bin/env python3
"""Generate the kfb24 CJK bitmap fonts from Noto Sans SC.

The inventory is every non-ASCII character that appears anywhere under main/
(display strings, log strings, comments) plus printable ASCII and common CJK
punctuation. Scanning comments and logs too keeps the static glyph check green
when a string moves between a log and a label.

Usage:
    python3 tools/gen_kfb24_fonts.py --font /path/NotoSansSC-Regular.otf

Noto Sans SC is licensed under the SIL Open Font License 1.1; the license text
is kept in assets/fonts/OFL.txt. lv_font_conv is pinned to the version recorded
in tools/requirements-fonts.txt.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "main"
OUT_DIR = MAIN / "fonts"
SIZES = (14, 16, 22)
CONVERTER = "lv_font_conv@1.5.3"
PUNCTUATION = "，。！？：；、·—…（）【】「」『』《》〈〉“”‘’￥％"


def inventory() -> str:
    chars: set[str] = set(PUNCTUATION)
    for path in MAIN.rglob("*"):
        if path.suffix not in {".c", ".h"}:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for char in text:
            code = ord(char)
            if 0x3400 <= code <= 0x9FFF or 0xF900 <= code <= 0xFAFF or code > 0xFFFF:
                chars.add(char)
            elif 0x3000 <= code <= 0x303F or 0xFF00 <= code <= 0xFFEF:
                chars.add(char)
    return "".join(sorted(chars))


def generate(font: Path, size: int, symbols: str) -> Path:
    output = OUT_DIR / f"kfb24_{size}.c"
    npx = shutil.which("npx") or shutil.which("npx.cmd")
    if not npx:
        raise SystemExit("npx not found; install Node.js to run lv_font_conv")
    command = [
        npx, "--yes", CONVERTER,
        "--font", str(font),
        "--size", str(size),
        "--bpp", "4",
        "--format", "lvgl",
        "--no-compress",
        "--lv-include", "lvgl.h",
        "--lv-font-name", f"kfb24_font_{size}",
        "--range", "0x20-0x7E",
        "--symbols", symbols,
        "--output", str(output),
    ]
    subprocess.run(command, check=True, cwd=ROOT)
    # lv_font_conv writes the absolute source path into the header comment.
    # Keep generated files reproducible and free of developer paths.
    text = output.read_text(encoding="utf-8")
    output.write_text(text.replace(str(font), "NotoSansSC-Regular.otf"),
                      encoding="utf-8")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", default=os.environ.get("KFB24_TTF"),
                        help="Noto Sans SC regular OTF/TTF")
    args = parser.parse_args()
    if not args.font:
        parser.error("--font or KFB24_TTF is required")
    font = Path(args.font)
    if not font.is_file():
        parser.error(f"font not found: {font}")

    symbols = inventory()
    if not symbols:
        parser.error("no CJK characters found under main/")
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    print(f"{len(symbols)} non-ASCII glyphs, sizes {SIZES}")
    for size in SIZES:
        output = generate(font, size, symbols)
        print(f"wrote {output.relative_to(ROOT)} ({output.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
