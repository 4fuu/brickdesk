#!/usr/bin/env python3
"""Pixel comparison for brickdesk screenshot tests.

The comparison works on decoded RGBA pixels, never on PNG bytes. Modes follow
docs/research/architecture.md section 8.5:

  exact      every pixel identical (CI default)
  tolerance  max channel delta + changed-pixel ratio + SSIM + connected
             difference regions

Exit codes: 0 = match, 1 = mismatch, 2 = usage or I/O error.
"""

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, UnidentifiedImageError


def load_rgba(path):
    with Image.open(path) as image:
        return np.array(image.convert("RGBA"), dtype=np.uint8, copy=True)


def parse_mask(text):
    parts = text.split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("mask must be x,y,w,h")
    try:
        values = [int(part, 10) for part in parts]
    except ValueError as exc:
        raise argparse.ArgumentTypeError("mask values must be integers") from exc
    if values[2] <= 0 or values[3] <= 0:
        raise argparse.ArgumentTypeError("mask width and height must be positive")
    return tuple(values)


def apply_masks(image, masks):
    if not masks:
        return image
    height, width = image.shape[:2]
    for x, y, mask_width, mask_height in masks:
        x0 = max(0, x)
        y0 = max(0, y)
        x1 = min(width, x + mask_width)
        y1 = min(height, y + mask_height)
        if x0 < x1 and y0 < y1:
            image[y0:y1, x0:x1, :] = 0
    return image


def luma(image):
    rgba = image.astype(np.float64)
    return 0.2126 * rgba[:, :, 0] + 0.7152 * rgba[:, :, 1] + 0.0722 * rgba[:, :, 2]


def box_filter(values, radius):
    """Mean over a (2*radius+1)^2 window using an integral image."""
    pad = radius
    padded = np.pad(values, pad, mode="edge")
    integral = np.pad(
        padded.cumsum(axis=0).cumsum(axis=1), ((1, 0), (1, 0)), mode="constant"
    )
    window = 2 * radius + 1
    total = (
        integral[window:, window:]
        - integral[:-window, window:]
        - integral[window:, :-window]
        + integral[:-window, :-window]
    )
    return total / float(window * window)


def ssim(expected, actual):
    """Mean windowed SSIM on luminance, 7x7 uniform window (matches 8.5 intent)."""
    x = luma(expected)
    y = luma(actual)
    radius = 3
    ux = box_filter(x, radius)
    uy = box_filter(y, radius)
    vx = box_filter(x * x, radius) - ux * ux
    vy = box_filter(y * y, radius) - uy * uy
    vxy = box_filter(x * y, radius) - ux * uy
    c1 = (0.01 * 255.0) ** 2
    c2 = (0.03 * 255.0) ** 2
    numerator = (2.0 * ux * uy + c1) * (2.0 * vxy + c2)
    denominator = (ux * ux + uy * uy + c1) * (vx + vy + c2)
    return float(np.mean(numerator / denominator))


def difference_extent(mask):
    """Returns the largest width/height of any 4-connected changed region."""
    height, width = mask.shape
    visited = np.zeros_like(mask, dtype=bool)
    max_width = 0
    max_height = 0
    ys, xs = np.nonzero(mask)
    for start_y, start_x in zip(ys.tolist(), xs.tolist()):
        if visited[start_y, start_x]:
            continue
        stack = [(start_y, start_x)]
        visited[start_y, start_x] = True
        min_x = max_x = start_x
        min_y = max_y = start_y
        while stack:
            y, x = stack.pop()
            if x < min_x:
                min_x = x
            if x > max_x:
                max_x = x
            if y < min_y:
                min_y = y
            if y > max_y:
                max_y = y
            for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                if (
                    0 <= ny < height
                    and 0 <= nx < width
                    and mask[ny, nx]
                    and not visited[ny, nx]
                ):
                    visited[ny, nx] = True
                    stack.append((ny, nx))
        component_width = max_x - min_x + 1
        component_height = max_y - min_y + 1
        if component_width > max_width:
            max_width = component_width
        if component_height > max_height:
            max_height = component_height
    return max_width, max_height


def write_diff(actual, expected, path):
    height, width = actual.shape[:2]
    gray = actual[:, :, :3].astype(np.uint16).sum(axis=2) // 3
    dimmed = (gray // 4).astype(np.uint8)
    output = np.empty((height, width, 4), dtype=np.uint8)
    output[:, :, 0] = dimmed
    output[:, :, 1] = dimmed
    output[:, :, 2] = dimmed
    output[:, :, 3] = 255
    changed = np.any(actual != expected, axis=2)
    output[changed] = (255, 0, 0, 255)
    Image.fromarray(output, mode="RGBA").save(path)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Compare two brickdesk screenshots")
    parser.add_argument("--expected", required=True)
    parser.add_argument("--actual", required=True)
    parser.add_argument("--mode", choices=("exact", "tolerance"), default="exact")
    parser.add_argument("--diff", default=None, help="diff PNG path (default: <actual>.diff.png)")
    parser.add_argument("--mask", action="append", type=parse_mask, default=[],
                        help="mask rectangle x,y,w,h (repeatable)")
    parser.add_argument("--max-delta", type=int, default=2)
    parser.add_argument("--max-diff-ratio", type=float, default=0.0005)
    parser.add_argument("--min-ssim", type=float, default=0.9995)
    parser.add_argument("--max-cluster", type=int, default=3)
    args = parser.parse_args(argv)

    try:
        expected = apply_masks(load_rgba(args.expected), args.mask)
        actual = apply_masks(load_rgba(args.actual), args.mask)
    except (OSError, UnidentifiedImageError) as exc:
        print(f"imagediff: cannot read image: {exc}", file=sys.stderr)
        return 2

    if expected.shape != actual.shape:
        print(
            "imagediff: FAIL dimension mismatch: "
            f"expected={expected.shape[1]}x{expected.shape[0]} "
            f"actual={actual.shape[1]}x{actual.shape[0]}"
        )
        return 1

    height, width = actual.shape[:2]
    total_pixels = width * height
    changed = np.any(expected != actual, axis=2)
    diff_pixels = int(changed.sum())
    max_delta = int(
        np.abs(expected.astype(np.int16) - actual.astype(np.int16)).max()
    ) if diff_pixels else 0

    diff_path = args.diff
    if diff_path is None:
        actual_path = Path(args.actual)
        diff_path = str(actual_path.with_suffix(".diff.png"))

    if args.mode == "exact":
        passed = diff_pixels == 0
        status = "PASS" if passed else "FAIL"
        print(
            f"imagediff: exact {status} expected={args.expected} actual={args.actual} "
            f"size={width}x{height} diff_pixels={diff_pixels} max_delta={max_delta}"
        )
    else:
        ratio = diff_pixels / float(total_pixels)
        if diff_pixels:
            similarity = ssim(expected, actual)
            cluster_width, cluster_height = difference_extent(changed)
        else:
            similarity = 1.0
            cluster_width = 0
            cluster_height = 0
        passed = (
            max_delta <= args.max_delta
            and ratio <= args.max_diff_ratio
            and similarity >= args.min_ssim
            and cluster_width <= args.max_cluster
            and cluster_height <= args.max_cluster
        )
        status = "PASS" if passed else "FAIL"
        print(
            f"imagediff: tolerance {status} expected={args.expected} actual={args.actual} "
            f"size={width}x{height} diff_pixels={diff_pixels} "
            f"ratio={ratio * 100.0:.4f}% max_delta={max_delta} "
            f"ssim={similarity:.6f} cluster={cluster_width}x{cluster_height}"
        )

    if not passed:
        print(f"imagediff: diff image: {diff_path}")
        try:
            write_diff(actual, expected, diff_path)
        except OSError as exc:
            print(f"imagediff: cannot write diff image: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
