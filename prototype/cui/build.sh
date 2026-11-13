#!/usr/bin/env bash
# Builds the prototype-only CUI dither sample renderer against an existing
# host build tree.  It does not touch the project CMake files.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build-host"}
OUT_BIN=${OUT_BIN:-"$BUILD_DIR/cui_samples"}
CC=${CC:-cc}

if [ ! -f "$BUILD_DIR/libbrickdesk_home.a" ] ||
   [ ! -f "$BUILD_DIR/libbrickdesk_core.a" ]; then
  printf 'build.sh: missing host libraries in %s\n' "$BUILD_DIR" >&2
  printf 'build.sh: run scripts/build-host.sh first, or set BUILD_DIR\n' >&2
  exit 1
fi

OBJ_DIR="$BUILD_DIR/cui-prototype"
mkdir -p "$OBJ_DIR"

CFLAGS=(
  -std=c11 -O2 -g -Wall -Wextra
  -I"$ROOT/include"
  -I"$BUILD_DIR/generated/include"
  -I"$ROOT/src/home"
  -I"$ROOT/tests"
  -DBRICKDESK_ASSET_DIR="\"$ROOT/assets\""
)
LIBS=(
  -Wl,--start-group
  "$BUILD_DIR/libbrickdesk_home.a"
  "$BUILD_DIR/libbrickdesk_core.a"
  "$BUILD_DIR/libbrickdesk_config.a"
  "$BUILD_DIR/libbrickdesk_input.a"
  "$BUILD_DIR/libbrickdesk_plugin.a"
  "$BUILD_DIR/libbrickdesk_session.a"
  "$BUILD_DIR/libbd_stb.a"
  "$BUILD_DIR/libbd_inih.a"
  -Wl,--end-group
  $(pkg-config --libs sdl2)
  -lGLESv2
  -lm
)

# shellcheck disable=SC2086
"$CC" "${CFLAGS[@]}" $(pkg-config --cflags sdl2) \
  -c "$ROOT/prototype/cui/cui_dither.c" -o "$OBJ_DIR/cui_dither.o"
# shellcheck disable=SC2086
"$CC" "${CFLAGS[@]}" $(pkg-config --cflags sdl2) \
  -c "$ROOT/prototype/cui/cui_samples.c" -o "$OBJ_DIR/cui_samples.o"
# shellcheck disable=SC2086
"$CC" "$OBJ_DIR/cui_dither.o" "$OBJ_DIR/cui_samples.o" -o "$OUT_BIN" \
  "${LIBS[@]}"

printf 'built: %s\n' "$OUT_BIN"
