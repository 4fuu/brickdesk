#!/usr/bin/env bash
# Regenerates host screenshot baselines from the current build.
#
# Usage: scripts/update-baselines.sh [target ...]
# Targets: boot focus_move hint_hold launch_menu primitives text tiles blur
#          plugin_hello plugin_settings menu home settings
# Default: all of them.
# Baseline updates must be reviewed and committed separately from feature work.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build-host"}
BASELINE_DIR=${BASELINE_DIR:-"$ROOT/tests/baselines/host"}

if [ "$#" -eq 0 ]; then
  set -- boot focus_move hint_hold launch_menu primitives text tiles blur plugin_hello plugin_settings menu home settings
fi

scenarios=()
captures=()
menu=0
launch_menu=0
home=0
settings=0
for target in "$@"; do
  case "$target" in
    boot|focus_move|hint_hold) scenarios+=("$target") ;;
    launch_menu) launch_menu=1 ;;
    primitives|text|tiles|blur|plugin_hello|plugin_settings) captures+=("$target") ;;
    menu) menu=1 ;;
    home) home=1 ;;
    settings) settings=1 ;;
    *)
      printf 'update-baselines: unknown target: %s\n' "$target" >&2
      exit 1
      ;;
  esac
done

cmake --build "$BUILD_DIR" --target bd_harness bd_gfx_capture bd_plugin_capture \
  bd_home_capture bd_settings_capture bd_menu_test bd_menu_state_app \
  brickdesk-inputd brickdesk-demo brickdesk-menu
mkdir -p "$BASELINE_DIR"

SCRATCH="$BUILD_DIR/tests/baselines-scratch"
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH"

for scenario in "${scenarios[@]}"; do
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

if [ "$launch_menu" -eq 1 ]; then
  out_dir="$SCRATCH/launch_menu"
  mkdir -p "$out_dir"
  SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy BRICKDESK_HEADLESS=1 \
    "$BUILD_DIR/tests/bd_harness" --scenario "$ROOT/tests/scenarios/launch_menu.json" \
    --out-dir "$out_dir" --config-dir "$ROOT/config" \
    --demo "$BUILD_DIR/brickdesk-demo" \
    --shim "$BUILD_DIR/libbrickdesk-menu.so" \
    --inputd "$BUILD_DIR/brickdesk-inputd" \
    --replay "$ROOT/tests/scenarios/launch_menu.replay"
  image="$out_dir/launch_return.png"
  if [ ! -f "$image" ]; then
    printf 'update-baselines: launch_menu produced no screenshot\n' >&2
    exit 1
  fi
  cp "$image" "$BASELINE_DIR/launch_return.png"
  printf 'baseline: %s\n' "$BASELINE_DIR/launch_return.png"
fi

if [ "${#captures[@]}" -gt 0 ]; then
  out_dir="$SCRATCH/captures"
  mkdir -p "$out_dir"
  SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy BRICKDESK_HEADLESS=1 \
    "$BUILD_DIR/tests/bd_gfx_capture" "$out_dir"
  SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy BRICKDESK_HEADLESS=1 \
    "$BUILD_DIR/tests/bd_plugin_capture" "$out_dir"

  for name in "${captures[@]}"; do
    image="$out_dir/$name.png"
    if [ ! -f "$image" ]; then
      printf 'update-baselines: capture %s was not produced\n' "$name" >&2
      exit 1
    fi
    cp "$image" "$BASELINE_DIR/"
    printf 'baseline: %s\n' "$BASELINE_DIR/$(basename "$image")"
  done
fi

if [ "$home" -eq 1 ]; then
  out_dir="$SCRATCH/home"
  mkdir -p "$out_dir"
  SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy BRICKDESK_HEADLESS=1 \
    "$BUILD_DIR/tests/bd_home_capture" "$out_dir"

  for image in "$out_dir"/home_*.png; do
    if [ ! -e "$image" ]; then
      continue
    fi
    cp "$image" "$BASELINE_DIR/"
    printf 'baseline: %s\n' "$BASELINE_DIR/$(basename "$image")"
  done
fi

if [ "$settings" -eq 1 ]; then
  out_dir="$SCRATCH/settings"
  mkdir -p "$out_dir"
  SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy BRICKDESK_HEADLESS=1 \
    "$BUILD_DIR/tests/bd_settings_capture" "$out_dir"

  for image in "$out_dir"/settings_*.png; do
    if [ ! -e "$image" ]; then
      continue
    fi
    cp "$image" "$BASELINE_DIR/"
    printf 'baseline: %s\n' "$BASELINE_DIR/$(basename "$image")"
  done
fi

if [ "$menu" -eq 1 ]; then
  out_dir="$SCRATCH/menu"
  mkdir -p "$out_dir"
  SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy BRICKDESK_HEADLESS=1 \
    "$BUILD_DIR/tests/bd_menu_test" \
    --demo "$BUILD_DIR/brickdesk-demo" \
    --state-app "$BUILD_DIR/tests/bd_menu_state_app" \
    --inputd "$BUILD_DIR/brickdesk-inputd" \
    --shim "$BUILD_DIR/libbrickdesk-menu.so" \
    --config "$ROOT/config" \
    --assets "$ROOT/assets" \
    --out-dir "$out_dir"
  for name in menu_open menu_focus; do
    image="$out_dir/$name.png"
    if [ ! -f "$image" ]; then
      printf 'update-baselines: %s was not produced\n' "$image" >&2
      exit 1
    fi
    cp "$image" "$BASELINE_DIR/"
    printf 'baseline: %s\n' "$BASELINE_DIR/$name.png"
  done
fi

printf '\nBaselines updated in %s\n' "$BASELINE_DIR"
printf 'Review them with: git status --short %s\n' "$BASELINE_DIR"
