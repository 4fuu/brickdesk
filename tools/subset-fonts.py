#!/usr/bin/env python3
"""Builds the small OFL font subsets shipped in assets/fonts/.

The device bundles three families:

  * Noto Sans SC 500 for Chinese UI text
  * Noto Serif SC 500 for page titles
  * Inter 500 for Latin text (with Noto Sans SC as a runtime fallback)

Sources are the variable Google Fonts releases.  The script pins the weight,
drops hinting and unused OpenType tables, then subsets to the characters in
tools/font-subset-chars.txt plus ASCII, Latin-1 punctuation and the CJK
punctuation actually used by the UI.

Usage:
    python3 tools/subset-fonts.py [--source-dir DIR] [--chars FILE]

If a source file is missing it is downloaded from Google Fonts into
--source-dir (default: /tmp/brickdesk-font-sources).  Requires fontTools and
brotli (pip install fonttools brotli).  Generated files are checked in so a
normal firmware build does not need Python.
"""

from __future__ import annotations

import argparse
import sys
import urllib.request
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "assets" / "fonts"
CHAR_FILE = ROOT / "tools" / "font-subset-chars.txt"

SOURCES = {
    "NotoSansSC-var.ttf": "https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf",
    "NotoSerifSC-var.ttf": "https://raw.githubusercontent.com/google/fonts/main/ofl/notoserifsc/NotoSerifSC%5Bwght%5D.ttf",
    "Inter-var.ttf": "https://raw.githubusercontent.com/google/fonts/main/ofl/inter/Inter%5Bopsz,wght%5D.ttf",
}

# (source, output, variation axes)
TARGETS = (
    ("NotoSansSC-var.ttf", "NotoSansSC-500-subset.ttf", {"wght": 500}),
    ("NotoSansSC-var.ttf", "NotoSansSC-700-subset.ttf", {"wght": 700}),
    ("NotoSerifSC-var.ttf", "NotoSerifSC-500-subset.ttf", {"wght": 500}),
    ("Inter-var.ttf", "Inter-500-subset.ttf", {"opsz": 20, "wght": 500}),
)

# ASCII, Latin-1 punctuation/symbols and the CJK punctuation the UI uses.
EXTRA_CODEPOINTS = set(range(0x20, 0x7F))
EXTRA_CODEPOINTS.update(
    [
        0x00A0, 0x00B7, 0x00C0, 0x00C9, 0x00D7, 0x00E0, 0x00E9, 0x00F7,
        0x2010, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2026, 0x2030, 0x2190, 0x2191, 0x2192, 0x2193, 0x2212, 0x2261,
        0x3000, 0x3001, 0x3002, 0x300A, 0x300B, 0x300C, 0x300D, 0x300E,
        0x300F, 0x3010, 0x3011, 0xFF01, 0xFF08, 0xFF09, 0xFF0C, 0xFF0E,
        0xFF1A, 0xFF1B, 0xFF1F, 0xFF5E,
    ]
)


def fetch_missing(source_dir: Path) -> None:
    source_dir.mkdir(parents=True, exist_ok=True)
    for name, url in SOURCES.items():
        destination = source_dir / name
        if destination.exists():
            continue
        print(f"download: {name}")
        with urllib.request.urlopen(url, timeout=60) as response:
            destination.write_bytes(response.read())


def read_codepoints(path: Path) -> set[int]:
    text = path.read_text(encoding="utf-8")
    return {ord(ch) for ch in text if not ch.isspace()} | EXTRA_CODEPOINTS


def build(source: Path, output: Path, axes: dict[str, float], codepoints: set[int]) -> None:
    font = TTFont(source)
    if "fvar" in font:
        instantiateVariableFont(font, axes, inplace=True)

    options = subset.Options()
    options.hinting = False
    options.desubroutinize = True
    options.name_IDs = ["*"]
    options.name_languages = ["*"]
    options.layout_features = ["kern", "liga", "clig", "calt", "ccmp"]
    options.drop_tables += [
        "FFTM", "DSIG", "hdmx", "VDMX", "gasp", "LTSH", "PCLT", "VORG",
        "BASE", "JSTF", "MATH", "GDEF", "GPOS", "GSUB",
    ]
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=codepoints)
    subsetter.subset(font)
    font.save(output)
    print(f"{output.name}: {output.stat().st_size / 1024:.0f} KiB")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=Path("/tmp/brickdesk-font-sources"))
    parser.add_argument("--chars", type=Path, default=CHAR_FILE)
    args = parser.parse_args()

    fetch_missing(args.source_dir)
    codepoints = read_codepoints(args.chars)
    print(f"characters: {len(codepoints)}")
    FONT_DIR.mkdir(parents=True, exist_ok=True)
    for source_name, output_name, axes in TARGETS:
        build(args.source_dir / source_name, FONT_DIR / output_name, axes, codepoints)
    return 0


if __name__ == "__main__":
    sys.exit(main())
