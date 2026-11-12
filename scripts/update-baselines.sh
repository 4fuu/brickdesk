#!/bin/sh
# Regenerates host screenshot baselines from the current build.
#
# Usage: scripts/update-baselines.sh [scenario ...]
# Default scenarios: boot focus_move
# Baseline updates must be reviewed and committed separately from feature work.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build-host"}
BASELINE_DIR=${BASELINE_DIR:-"$ROOT/tests/baselines/host"}

if [ "$#" -eq 0 ]; then
  set -- boot focus_move
fi

cmake --build "$BUILD_DIR" --target bd_harness
mkdir -p "$BASELINE_DIR"

SCRATCH="$BUILD_DIR/tests/baselines-scratch"
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH"

for scenario in "$@"; do
  scenario_file="$ROOT/tests/scenarios/$scenario.json"
  if [ ! -f "$scenario_file" ]; then
    printf 'update-baselines: scenario not found: %s\n' "$scenario_file" >&2
    exit 1
  fi

  out_dir="$SCRATCH/$scenario"
  mkdir -p "$out_dir"
  SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy BRICKDESK_HEADLESS=1 \
    "$BUILD_DIR/tests/bd_harness" --scenario "$scenario_file" --out-dir "$out_dir"

  found=0
  for image in "$out_dir"/*.png; do
    if [ ! -e "$image" ]; then
      continue
    fi
    cp "$image" "$BASELINE_DIR/"
    printf 'baseline: %s\n' "$BASELINE_DIR/$(basename "$image")"
    found=1
  done
  if [ "$found" -eq 0 ]; then
    printf 'update-baselines: scenario %s produced no screenshots\n' "$scenario" >&2
    exit 1
  fi
done

printf '\nBaselines updated in %s\n' "$BASELINE_DIR"
printf 'Review them with: git status --short %s\n' "$BASELINE_DIR"
