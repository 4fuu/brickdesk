#!/usr/bin/env python3
"""Expands tools/font-subset-chars.txt with GB2312 level 1 + level 2 Hanzi.

Idempotent: existing extra characters are preserved, and the result is written
sorted by codepoint. Run this before tools/subset-fonts.py when changing the
font coverage.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHAR_FILE = ROOT / "tools" / "font-subset-chars.txt"


def gb2312_hanzi() -> set[str]:
    characters: set[str] = set()
    for high in range(0xB0, 0xF8):
        for low in range(0xA1, 0xFF):
            try:
                character = bytes([high, low]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if "\u4e00" <= character <= "\u9fff":
                characters.add(character)
    return characters


def main() -> None:
    existing = set(CHAR_FILE.read_text(encoding="utf-8").strip())
    gb = gb2312_hanzi()
    merged = existing | gb
    CHAR_FILE.write_text("".join(sorted(merged)) + "\n", encoding="utf-8")
    print(f"{len(merged)} characters ({len(gb)} GB2312 Hanzi)")


if __name__ == "__main__":
    main()
