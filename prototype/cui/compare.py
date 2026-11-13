#!/usr/bin/env python3
"""Builds same-scale and zoomed comparison sheets for the CUI samples."""

from __future__ import annotations

import csv
import os
import sys
from PIL import Image, ImageDraw, ImageFont

BG = (18, 16, 13)
INK = (243, 237, 226)
INK2 = (188, 178, 163)
ACCENT = (224, 122, 95)
FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

BAR_BOX = (622, 166, 1024, 286)
PANEL_BOX = (622, 0, 1024, 768)


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(FONT_BOLD if bold else FONT_PATH, size)


def load(out_dir: str, name: str) -> Image.Image:
    return Image.open(os.path.join(out_dir, f"{name}.png")).convert("RGB")


def read_manifest(out_dir: str) -> dict[str, dict]:
    path = os.path.join(out_dir, "manifest.tsv")
    with open(path, newline="", encoding="utf-8") as handle:
        return {row["name"]: row for row in csv.DictReader(handle, delimiter="\t")}


def crop(image: Image.Image, box: tuple[int, int, int, int],
         scale: int) -> Image.Image:
    piece = image.crop(box)
    if scale != 1:
        piece = piece.resize((piece.width * scale, piece.height * scale),
                             Image.Resampling.NEAREST)
    return piece


def compose(out_dir: str, rows: list[tuple[str, str, tuple[int, int, int, int]]],
            title: str, filename: str, scale: int = 1,
            label_width: int = 230, note: str | None = None) -> str:
    images = [(label, crop(load(out_dir, name), box, scale))
              for label, name, box in rows]
    row_h = max(image.height for _, image in images)
    width = 24 + label_width + max(image.width for _, image in images) + 24
    height = 46 + len(images) * (row_h + 14) + (28 if note else 0) + 12
    sheet = Image.new("RGB", (width, height), BG)
    draw = ImageDraw.Draw(sheet)
    draw.text((24, 14), title, font=font(18, True), fill=INK)
    y = 46
    for label, image in images:
        draw.text((24, y + image.height // 2), label, font=font(14),
                  fill=INK2, anchor="lm")
        sheet.paste(image, (24 + label_width, y))
        draw.rectangle((24 + label_width - 1, y - 1,
                        24 + label_width + image.width, y + image.height),
                       outline=(65, 58, 49))
        y += row_h + 14
    if note:
        draw.text((24, y + 2), note, font=font(13), fill=INK2)
    path = os.path.join(out_dir, filename)
    sheet.save(path)
    print(f"comparison: {path} ({sheet.width}x{sheet.height})")
    return path


def bar_box(row: dict) -> tuple[int, int, int, int]:
    """Header + strip + all of row 0, without cutting the next row's text."""
    return (622, 166, 1024, int(row["rows_start"]) + 48 + 8)


def summary(out_dir: str, manifest: dict[str, dict]) -> None:
    path = os.path.join(out_dir, "summary.txt")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("CUI static dither samples\n")
        handle.write("1x 1024x768, dark warm palette, persimmon accent\n\n")
        for name, row in manifest.items():
            handle.write(
                f"{name}: mode={row['mode']} value={row['value']} "
                f"cell={row['cell']}px bar={row['bar_w']}x{row['bar_h']} "
                f"tex={row['tex_w']}x{row['tex_h']} "
                f"lit={row['lit']}/{row['total']} density={row['density']} "
                f"memory={row['memory_mb']}MB rows_start={row['rows_start']} "
                f"idle_presents={row['idle_presents']} "
                f"idle_draws={row['idle_draw_calls']}\n")
    print(f"summary: {path}")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: compare.py SAMPLE_DIR", file=sys.stderr)
        return 2
    out_dir = sys.argv[1]
    manifest = read_manifest(out_dir)

    compose(
        out_dir,
        [
            ("CURRENT  366x2 solid", "00_current_506", bar_box(manifest["00_current_506"])),
            ("2 px cells  / 10 px bar", "02_cell2_506", bar_box(manifest["02_cell2_506"])),
            ("3 px cells  / 12 px bar", "06_cell3_506", bar_box(manifest["06_cell3_506"])),
            ("4 px cells  / 16 px bar", "10_cell4_506", bar_box(manifest["10_cell4_506"])),
        ],
        "Quick panel memory strip - 506 MB - 1x, same scale",
        "compare_bar_1x.png",
        note="Crops are 1024x768 device pixels at 1:1; no resampling.",
    )

    compose(
        out_dir,
        [
            ("CURRENT  unknown (empty)", "00b_current_unknown", bar_box(manifest["00b_current_unknown"])),
            ("3 px  /  0 MB", "05_cell3_zero", bar_box(manifest["05_cell3_zero"])),
            ("3 px  /  506 MB", "06_cell3_506", bar_box(manifest["06_cell3_506"])),
            ("3 px  /  1024 MB", "07_cell3_full", bar_box(manifest["07_cell3_full"])),
            ("3 px  /  unknown (25%, rejected)", "08_cell3_unknown", bar_box(manifest["08_cell3_unknown"])),
        ],
        "Value states at 3 px cells - 1x, same scale",
        "compare_values_1x.png",
        note="Product preference: unknown stays an empty track; 25% is rejected evidence.",
    )

    panes = [
        ("CURRENT", "00_current_506"),
        ("2 px cells", "02_cell2_506"),
        ("3 px cells", "06_cell3_506"),
        ("4 px cells", "10_cell4_506"),
    ]
    pane_images = [(label, crop(load(out_dir, name), PANEL_BOX, 1))
                   for label, name in panes]
    width = 16 + len(pane_images) * (402 + 16)
    height = 52 + 768 + 16
    sheet = Image.new("RGB", (width, height), BG)
    draw = ImageDraw.Draw(sheet)
    draw.text((16, 10), "Quick panel - full height - 1x, same scale (506 MB)",
              font=font(17, True), fill=INK)
    x = 16
    for label, image in pane_images:
        draw.text((x, 32), label, font=font(13, True), fill=INK2)
        sheet.paste(image, (x, 52))
        draw.rectangle((x - 1, 51, x + image.width, 52 + image.height),
                       outline=(65, 58, 49))
        x += 402 + 16
    path = os.path.join(out_dir, "compare_panels_1x.png")
    sheet.save(path)
    print(f"comparison: {path} ({sheet.width}x{sheet.height})")

    zoom_rows = [
        ("2 px cells", "02_cell2_506"),
        ("3 px cells", "06_cell3_506"),
        ("4 px cells", "10_cell4_506"),
    ]
    # 4x nearest-neighbour crop from x=632..1008; the height follows the bar.
    pieces = []
    for label, name in zoom_rows:
        row = manifest[name]
        h = int(row["bar_h"])
        box = (640, 194, 1008, 196 + h + 2)
        pieces.append((label, crop(load(out_dir, name), box, 4)))
    row_h = max(image.height for _, image in pieces)
    width = 24 + max(image.width for _, image in pieces) + 24
    height = 46 + len(pieces) * (row_h + 28) + 24
    sheet = Image.new("RGB", (width, height), BG)
    draw = ImageDraw.Draw(sheet)
    draw.text((24, 14), "Dither grid - 4x nearest-neighbour (inspection only)",
              font=font(17, True), fill=INK)
    y = 46
    for label, image in pieces:
        draw.text((24, y), label, font=font(13, True), fill=ACCENT)
        sheet.paste(image, (24, y + 20))
        y += row_h + 28
    path = os.path.join(out_dir, "zoom_grid_4x.png")
    sheet.save(path)
    print(f"comparison: {path} ({sheet.width}x{sheet.height})")

    summary(out_dir, manifest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
