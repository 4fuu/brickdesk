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

The subset contains the 2,061 common Chinese characters in
`tools/font-subset-chars.txt`, ASCII, common Latin-1 punctuation, arrows and
the CJK punctuation used by the UI. It is roughly 2 MiB in total, versus tens
of MiB for the full CJK fonts.

The files are generated from the upstream variable fonts with FontTools:

```sh
python3 -m pip install fonttools brotli
python3 tools/subset-fonts.py
```

The script downloads missing sources to `/tmp/brickdesk-font-sources`, pins
the `wght` axis, disables hinting, drops unused OpenType tables and writes the
checked-in files. stb_truetype does not execute TrueType hinting bytecode, so
dropping hinting only removes dead data.

The UI rasterizes glyphs on demand at integer pixel sizes (12, 16, 20, 34)
into single-channel atlases; no large pre-rendered CJK atlas is bundled.
