#!/usr/bin/env python3
"""Validates the prototype CUI dither sample matrix.

This checks the rendered pixels, the manifest geometry, the Bayer density,
the rounded ends and the idle (zero-redraw) evidence.  It is intentionally
independent of the C implementation: the expected Bayer mask is rebuilt here
from CUI's documented bit formula.
"""

from __future__ import annotations

import csv
import math
import os
import sys
from PIL import Image

ACCENT = (224, 122, 95)
UNKNOWN = (143, 134, 120)
SURFACE = (27, 24, 20)
WIDTH = 1024
HEIGHT = 768
SHOTS = 14
IDLE_FRAMES = 120

errors: list[str] = []


def fail(message: str) -> None:
    errors.append(message)
    print(f"FAIL: {message}")


def near(pixel, color, tolerance=2) -> bool:
    return all(abs(int(pixel[i]) - int(color[i])) <= tolerance for i in range(3))


def bayer8(x: int, y: int) -> float:
    qx = x & 7
    qy = y & 7
    a = qx ^ qy
    m = (
        ((a & 1) << 5)
        | ((qy & 1) << 4)
        | ((a & 2) << 2)
        | ((qy & 2) << 1)
        | ((a & 4) >> 1)
        | ((qy & 4) >> 2)
    )
    return (m + 0.5) / 64.0


def read_manifest(path: str) -> list[dict]:
    with open(path, newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def read_key_values(path: str) -> dict:
    values = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if "=" in line:
                key, value = line.split("=", 1)
                values[key] = value
    return values


def check_gl(out_dir: str) -> None:
    path = os.path.join(out_dir, "gl.txt")
    if not os.path.exists(path):
        fail("missing gl.txt")
        return
    gl = read_key_values(path)
    version = gl.get("version", "")
    shading = gl.get("shading_language", "")
    if not version.startswith("OpenGL ES 2.0"):
        fail(f"expected an ES 2.0 context, got {version!r}")
    if "GLSL ES 1.0" not in shading:
        fail(f"expected GLSL ES 1.0, got {shading!r}")
    print(f"gl: {version} / {shading}")


def check_selfcheck(out_dir: str) -> None:
    path = os.path.join(out_dir, "selfcheck.txt")
    if not os.path.exists(path):
        fail("missing selfcheck.txt")
        return
    values = read_key_values(path)
    for key in ("bayer_permutation", "bayer_subset", "density_clamp"):
        if values.get(key) != "ok":
            fail(f"self-check {key} is {values.get(key)!r}")
    print("self-check: bayer permutation/subset/clamp ok")


def check_log(out_dir: str) -> None:
    path = os.path.join(out_dir, "run.log")
    if not os.path.exists(path):
        fail("missing run.log")
        return
    text = open(path, encoding="utf-8").read()
    if "shots: 14, failures: 0" not in text:
        fail("run.log does not report 14 shots and zero failures")


def check_idle(row: dict, name: str) -> None:
    if int(row["idle_frames"]) != IDLE_FRAMES:
        fail(f"{name}: idle frame count is {row['idle_frames']}")
    for key in ("idle_presents", "idle_draw_calls", "idle_damage_rects",
                "idle_bakes"):
        if int(row[key]) != 0:
            fail(f"{name}: {key}={row[key]} (expected 0)")


def check_common_geometry(row: dict, name: str) -> tuple[int, int, int, int]:
    x = int(row["bar_x"])
    y = int(row["bar_y"])
    w = int(row["bar_w"])
    h = int(row["bar_h"])
    cell = int(row["cell"])
    if y != 196:
        fail(f"{name}: bar y={y}, expected 196")
    if cell == 4:
        if (x, w) != (641, 364):
            fail(f"{name}: 4 px bar is {x},{w}, expected 641,364")
    else:
        if (x, w) != (640, 366):
            fail(f"{name}: bar is {x},{w}, expected 640,366")
    if int(row["rows_start"]) != 204 + (h if row["mode"] == "dither" else 0):
        fail(f"{name}: rows_start={row['rows_start']}")
    if int(row["controls_y"]) != int(row["rows_start"]) + 174:
        fail(f"{name}: controls_y={row['controls_y']}")
    if int(row["controls_y"]) + 4 * 48 > HEIGHT:
        fail(f"{name}: controls overflow the panel")
    if row["mode"] == "dither" and y + h + 8 > int(row["rows_start"]):
        fail(f"{name}: dither bar has less than 8 px before the rows")
    return x, y, w, h


def check_current(row: dict, name: str, image: Image.Image) -> None:
    x, y, w, h = check_common_geometry(row, name)
    memory = int(row["memory_mb"])
    if int(row["bakes"]) != 0 or int(row["uploads"]) != 0:
        fail(f"{name}: current shot should not bake a texture")
    if h != 2:
        fail(f"{name}: current bar height is {h}, expected 2")
    if image.getpixel((x + w // 2, y + 2)) != SURFACE:
        fail(f"{name}: pixel below the 2 px bar is not panel surface")
    if memory < 0:
        for px in range(x, x + w):
            for py in range(y, y + h):
                if near(image.getpixel((px, py)), ACCENT, 6):
                    fail(f"{name}: unknown current bar has accent pixels")
                    return
        return
    expected_fill = w * min(memory, 1024) // 1024
    for px in range(x, x + expected_fill):
        if not near(image.getpixel((px, y + 1)), ACCENT, 2):
            fail(f"{name}: accent fill ends early at x={px}")
            break
    for px in range(x + expected_fill + 1, x + w):
        if near(image.getpixel((px, y + 1)), ACCENT, 6):
            fail(f"{name}: accent fill continues past x={px}")
            break


def sample_center_y(y: int, row: int, cell: int, rows: int) -> int:
    # The first/last pixel row of a rounded pill is antialiased.  Sample the
    # second pixel row for the top edge and the second-to-last for the bottom.
    offset = cell // 2
    if row == 0:
        offset = min(offset, 1) if cell > 1 else 0
    elif row == rows - 1:
        offset = max(cell // 2 - 1, 0)
    return y + row * cell + offset


def check_dither(row: dict, name: str, image: Image.Image) -> None:
    x, y, w, h = check_common_geometry(row, name)
    cell = int(row["cell"])
    density = float(row["density"])
    columns = math.ceil(w / cell)
    rows = h // cell
    if int(row["tex_w"]) != columns or int(row["tex_h"]) != rows:
        fail(f"{name}: texture {row['tex_w']}x{row['tex_h']} != {columns}x{rows}")
    total = columns * rows
    if int(row["total"]) != total:
        fail(f"{name}: total cells {row['total_cells']} != {total}")
    expected_lit = 0
    for oy in range(rows):
        for ox in range(columns):
            if bayer8(ox, oy) < density:
                expected_lit += 1
    if int(row["lit"]) != expected_lit:
        fail(f"{name}: lit cells {row['lit']} != expected {expected_lit}")
    if int(row["bakes"]) != 1 or int(row["uploads"]) != 1:
        fail(f"{name}: expected one bake and one upload")

    color = UNKNOWN if row["value"] == "unknown" else ACCENT
    if row["value"] == "unknown":
        for px in range(x, x + w):
            for py in range(y, y + h):
                if near(image.getpixel((px, py)), ACCENT, 6):
                    fail(f"{name}: unknown bar leaks accent")
                    return

    # Interior cells: exact solid block, aligned to the bar origin.  This is
    # the grid test; edge cells are checked by the rounded-corner checks.
    for oy in range(1, rows - 1):
        for ox in range(2, columns - 2):
            cx = x + ox * cell + cell // 2
            cy = sample_center_y(y, oy, cell, rows)
            lit = bayer8(ox, oy) < density
            pixel = image.getpixel((cx, cy))
            if lit:
                if not near(pixel, color, 2):
                    fail(f"{name}: lit cell {ox},{oy} is {pixel}, expected {color}")
                    return
                for by in range(y + oy * cell, y + (oy + 1) * cell):
                    for bx in range(x + ox * cell, x + (ox + 1) * cell):
                        if not near(image.getpixel((bx, by)), color, 2):
                            fail(f"{name}: cell {ox},{oy} is not a solid block")
                            return
            elif near(pixel, color, 8):
                fail(f"{name}: unlit cell {ox},{oy} contains the lit colour")
                return

    if density == 0.0:
        if int(row["lit"]) != 0:
            fail(f"{name}: zero density has lit cells")
        for px in range(x, x + w):
            if near(image.getpixel((px, y + h // 2)), color, 6):
                fail(f"{name}: zero density has dither pixels")
                return
    elif density == 1.0:
        if int(row["lit"]) != total:
            fail(f"{name}: full density is not fully lit")
        # The pill's straight interior must be solid; only the rounded caps and
        # the one-pixel AA edge are allowed to differ.
        for px in range(x + h, x + w - h):
            if not near(image.getpixel((px, y + h // 2)), color, 2):
                fail(f"{name}: full bar is not solid at x={px}")
                return
        if near(image.getpixel((x + 1, y + 1)), color, 8):
            fail(f"{name}: top-left corner is not rounded")
        if near(image.getpixel((x + w - 2, y + 1)), color, 8):
            fail(f"{name}: top-right corner is not rounded")
        if near(image.getpixel((x, y)), color, 8):
            fail(f"{name}: outer corner is not rounded")

    print(
        f"{name}: {cell}px cells, {columns}x{rows} texels, "
        f"{row['lit']}/{total} lit ({density:.3f}), {w}x{h}, rows at "
        f"{row['rows_start']}"
    )


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: verify.py SAMPLE_DIR", file=sys.stderr)
        return 2
    out_dir = sys.argv[1]
    manifest_path = os.path.join(out_dir, "manifest.tsv")
    if not os.path.exists(manifest_path):
        print(f"verify: missing {manifest_path}", file=sys.stderr)
        return 2

    check_gl(out_dir)
    check_selfcheck(out_dir)
    check_log(out_dir)
    rows = read_manifest(manifest_path)
    if len(rows) != SHOTS:
        fail(f"manifest has {len(rows)} shots, expected {SHOTS}")
    names = {row["name"] for row in rows}
    if len(names) != len(rows):
        fail("duplicate shot names")

    for row in rows:
        name = row["name"]
        path = os.path.join(out_dir, row["file"])
        if not os.path.exists(path):
            fail(f"{name}: missing {path}")
            continue
        image = Image.open(path).convert("RGB")
        if image.size != (WIDTH, HEIGHT):
            fail(f"{name}: image is {image.size}, expected 1024x768")
            continue
        check_idle(row, name)
        if row["mode"] == "current":
            check_current(row, name, image)
        else:
            check_dither(row, name, image)

    if errors:
        print(f"\n{len(errors)} verification failure(s)")
        return 1
    print(f"\nall {len(rows)} shots verified: pixels, layout, grid, idle")
    return 0


if __name__ == "__main__":
    sys.exit(main())
