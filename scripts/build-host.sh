#!/bin/sh
# Build brickdesk for the host (SDL2 + Mesa GLES2).
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build-host"}
BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo}
WERROR=${BRICKDESK_WERROR:-OFF}

if [ -z "${CMAKE_GENERATOR:-}" ] && command -v ninja >/dev/null 2>&1; then
  CMAKE_GENERATOR=Ninja
fi

set -- -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DBRICKDESK_WERROR="$WERROR"
if [ -n "${CMAKE_GENERATOR:-}" ]; then
  set -- "$@" -G "$CMAKE_GENERATOR"
fi

cmake "$@"
cmake --build "$BUILD_DIR" --parallel

printf '\nBuild complete: %s\n' "$BUILD_DIR"
printf 'Run tests with: ctest --test-dir %s --output-on-failure\n' "$BUILD_DIR"
