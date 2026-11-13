#!/bin/sh
# Build (if needed) and run the host renderer benchmark.
#
# Usage: scripts/bench.sh [bench_render options...]
# Environment: BUILD_DIR (default <repo>/build-host)
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build-host"}

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    "$ROOT/scripts/build-host.sh"
else
    cmake --build "$BUILD_DIR" --parallel
fi

exec env \
    SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" \
    SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}" \
    BRICKDESK_HEADLESS="${BRICKDESK_HEADLESS:-1}" \
    "$BUILD_DIR/bench/bench_render" "$@"
