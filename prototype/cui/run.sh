#!/usr/bin/env bash
# Builds, renders, verifies and compares the prototype CUI samples.
# Usage: prototype/cui/run.sh [OUT_DIR]
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build-host"}
OUT=${1:-prototype/cui/samples}

cd "$ROOT"
"$ROOT/prototype/cui/build.sh"

mkdir -p "$OUT"
rm -f "$OUT"/*.png "$OUT"/*.tsv "$OUT"/*.txt "$OUT"/*.log

# Force the Mesa context to the exact GLES2 / GLSL ES 1.00 profile the sample
# must run on.  This is host validation, not a GE8300 measurement.
MESA_GLES_VERSION_OVERRIDE=2.0 \
SDL_VIDEODRIVER=offscreen \
SDL_AUDIODRIVER=dummy \
BRICKDESK_HEADLESS=1 \
  "$BUILD_DIR/cui_samples" "$OUT" 2>&1 | tee "$OUT/run.log"

python3 prototype/cui/verify.py "$OUT" | tee "$OUT/verify.log"
python3 prototype/cui/compare.py "$OUT"
