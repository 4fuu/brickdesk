# Bundled fonts

These are subset, static instances of the Google Fonts variable releases:

| File | Family / weight | Source |
|---|---|---|
| `NotoSansSC-500-subset.ttf` | Noto Sans SC 500 | https://github.com/google/fonts/tree/main/ofl/notosanssc |
| `NotoSansSC-700-subset.ttf` | Noto Sans SC 700 | https://github.com/google/fonts/tree/main/ofl/notosanssc |
| `NotoSerifSC-500-subset.ttf` | Noto Serif SC 500 | https://github.com/google/fonts/tree/main/ofl/notoserifsc |
| `Inter-500-subset.ttf` | Inter 500 | https://github.com/google/fonts/tree/main/ofl/inter |

All four are licensed under the SIL Open Font License 1.1. The full license
texts are in `OFL-NotoSansSC.txt`, `OFL-NotoSerifSC.txt` and `OFL-Inter.txt`.

## Subset

The subset covers GB2312 level 1 + level 2 (6,763 Hanzi) plus two
UI-specific characters, ASCII, common Latin-1 punctuation, arrows and the CJK
punctuation used by the UI: 6,904 codepoints in total. It is 6.9 MiB across
the four files (Sans 500 + Sans 700 + Serif 500 + Inter), versus tens of MiB
for the full CJK fonts.

Keeping GB2312 coverage and two Chinese sans weights plus a serif title face
exceeds the 4 MiB soft budget. Dropping `NotoSansSC-700` or limiting the serif
to level 1 would fit under 4 MiB, at the cost of the title hierarchy. The
runtime can optionally load a full `NotoSansSC-Full.ttf`/`.otf` from this
directory (not bundled) as a 20 px last-resort fallback; otherwise missing
glyphs draw a placeholder box and increment `missing_glyphs` in the renderer
stats.

The files are generated from the upstream variable fonts with FontTools:

```sh
python3 -m pip install fonttools brotli
python3 tools/make-gb2312-chars.py
python3 tools/subset-fonts.py
```

The script downloads missing sources to `/tmp/brickdesk-font-sources`, pins
the `wght` axis, disables hinting, drops unused OpenType tables and writes the
checked-in files. stb_truetype does not execute TrueType hinting bytecode, so
dropping hinting only removes dead data.

The UI rasterizes glyphs on demand at integer pixel sizes (12, 16, 20, 34)
into single-channel atlases; no large pre-rendered CJK atlas is bundled.
