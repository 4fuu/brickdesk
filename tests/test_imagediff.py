#!/usr/bin/env python3
"""Self-tests for tools/imagediff.py.

Runs the real CLI through subprocesses so the exit codes and diff output are
covered, not just the metric helpers.
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image


def save(path, pixels):
    Image.fromarray(pixels, mode="RGBA").save(path)


def run(imagediff, *args):
    return subprocess.run(
        [sys.executable, str(imagediff), *map(str, args)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--imagediff", required=True)
    args = parser.parse_args()
    imagediff = args.imagediff

    failures = []

    def expect(name, condition, detail=""):
        if condition:
            print(f"ok: {name}")
        else:
            failures.append(name)
            print(f"FAIL: {name} {detail}")

    with tempfile.TemporaryDirectory(prefix="bd-imagediff-") as tmp:
        root = Path(tmp)

        base = np.zeros((64, 64, 4), dtype=np.uint8)
        base[:, 32:, :3] = 255
        base[:, :, 3] = 255
        base_path = root / "base.png"
        save(base_path, base)

        same_path = root / "same.png"
        save(same_path, base.copy())
        result = run(imagediff, "--expected", base_path, "--actual", same_path)
        expect("identical images pass exact", result.returncode == 0, result.stdout + result.stderr)

        shifted_path = root / "shifted.png"
        save(shifted_path, np.roll(base, 1, axis=1))
        diff_path = root / "shifted.diff.png"
        result = run(
            imagediff,
            "--expected", base_path,
            "--actual", shifted_path,
            "--diff", diff_path,
        )
        expect("1px horizontal shift fails exact", result.returncode == 1, result.stdout + result.stderr)
        expect("1px shift writes diff image", diff_path.is_file())
        result = run(
            imagediff,
            "--mode", "tolerance",
            "--expected", base_path,
            "--actual", shifted_path,
        )
        expect("1px shift fails tolerance", result.returncode == 1, result.stdout + result.stderr)

        one_pixel = base.copy()
        one_pixel[10, 10, 0] = 1
        one_pixel_path = root / "one_pixel.png"
        save(one_pixel_path, one_pixel)
        result = run(imagediff, "--expected", base_path, "--actual", one_pixel_path)
        expect("single channel delta fails exact", result.returncode == 1, result.stdout + result.stderr)
        result = run(
            imagediff,
            "--mode", "tolerance",
            "--expected", base_path,
            "--actual", one_pixel_path,
        )
        expect("single +1 delta passes tolerance", result.returncode == 0, result.stdout + result.stderr)

        masked = base.copy()
        masked[5:15, 5:15, :3] = 255
        masked_path = root / "masked.png"
        save(masked_path, masked)
        result = run(imagediff, "--expected", base_path, "--actual", masked_path)
        expect("unmasked change fails exact", result.returncode == 1, result.stdout + result.stderr)
        result = run(
            imagediff,
            "--expected", base_path,
            "--actual", masked_path,
            "--mask", "0,0,20,20",
        )
        expect("mask makes that change pass", result.returncode == 0, result.stdout + result.stderr)

        small = np.zeros((32, 32, 4), dtype=np.uint8)
        small[:, :, 3] = 255
        small_path = root / "small.png"
        save(small_path, small)
        result = run(imagediff, "--expected", base_path, "--actual", small_path)
        expect("dimension mismatch fails", result.returncode == 1, result.stdout + result.stderr)

        result = run(
            imagediff,
            "--expected", root / "missing.png",
            "--actual", same_path,
        )
        expect("missing input is a usage error", result.returncode == 2, result.stdout + result.stderr)

    if failures:
        print(f"imagediff self-test: {len(failures)} failure(s)")
        return 1
    print("imagediff self-test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
