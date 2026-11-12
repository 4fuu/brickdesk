#!/bin/sh
# Cross-compile the target artifacts with the official TrimUI GCC 7.5 toolchain.
#
# Works inside the ci/Dockerfile.toolchain image and directly on a machine that
# has the toolchain and SDK unpacked. Builds (in order):
#   1. the patched SDL2 runtime into the target sysroot, unless it is present;
#   2. tools/display_probe;
#   3. the project's CMake target, once CMakeLists.txt exists (M1-A skeleton);
#   4. a distributable lib/ directory and SHA256SUMS.
#
# Environment:
#   TRIMUI_TOOLCHAIN  toolchain root (default /opt/trimui/toolchain)
#   TRIMUI_SDK        SDK root (default /opt/trimui/sdk)
#   TRIMUI_SYSROOT    sysroot with the patched SDL2
#                     (default /opt/trimui/sysroot, else build-target/sysroot)
#
# Usage: scripts/build-target.sh [--build-dir DIR] [--skip-cmake]
set -eu

SCRIPT_DIR=$(CDPATH="" cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH="" cd -- "$SCRIPT_DIR/.." && pwd)

TRIMUI_TOOLCHAIN=${TRIMUI_TOOLCHAIN:-/opt/trimui/toolchain}
TRIMUI_SDK=${TRIMUI_SDK:-/opt/trimui/sdk}
BUILD_DIR=$REPO_ROOT/build-target
SYSROOT=${TRIMUI_SYSROOT:-}
SKIP_CMAKE=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR=$2; shift 2 ;;
        --sysroot) SYSROOT=$2; shift 2 ;;
        --skip-cmake) SKIP_CMAKE=1; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$SYSROOT" ]; then
    if [ -e "$BUILD_DIR/sysroot/lib/libSDL2-2.0.so.0" ]; then
        SYSROOT=$BUILD_DIR/sysroot
    elif [ -e "/opt/trimui/sysroot/lib/libSDL2-2.0.so.0" ]; then
        SYSROOT=/opt/trimui/sysroot
    else
        SYSROOT=$BUILD_DIR/sysroot
    fi
fi

CC=$TRIMUI_TOOLCHAIN/bin/aarch64-linux-gnu-gcc
if [ ! -x "$CC" ]; then
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        CC=$(command -v aarch64-linux-gnu-gcc)
    else
        echo "error: aarch64-linux-gnu-gcc not found (TRIMUI_TOOLCHAIN=$TRIMUI_TOOLCHAIN)" >&2
        exit 1
    fi
fi

mkdir -p "$BUILD_DIR"

if [ ! -e "$SYSROOT/lib/libSDL2-2.0.so.0" ] || [ ! -d "$SYSROOT/include/SDL2" ]; then
    echo "==> patched SDL2 missing from $SYSROOT, building it"
    "$REPO_ROOT/scripts/build-sdl2.sh" --prefix "$SYSROOT" --build "$BUILD_DIR/sdl2-build"
fi

# Resolve the vendor libraries' DT_NEEDED entries without exposing the SDK's
# glibc 2.23 stubs (libpthread-2.23.so, libdl-2.23.so). The image sysroot already
# contains the filtered directory; older or hand-made sysroots get one built in
# the build tree.
if [ -e "$SYSROOT/vendor-lib/lib/libEGL.so" ]; then
    VENDOR_LIBS=$SYSROOT/vendor-lib/lib
else
    VENDOR_LIBS=$BUILD_DIR/vendor-libs
    "$REPO_ROOT/scripts/prepare-vendor-libs.sh" "$TRIMUI_SDK" "$VENDOR_LIBS"
fi

echo "==> building display_probe"
"$CC" -std=c11 -O2 -g -Wall -Wextra \
    -I"$SYSROOT/include/SDL2" \
    -isystem "$TRIMUI_SDK/usr/include" \
    "$REPO_ROOT/tools/display_probe.c" \
    -o "$BUILD_DIR/display_probe" \
    -L"$SYSROOT/lib" -lSDL2 \
    "$TRIMUI_SDK/usr/lib/libEGL.so" \
    "$TRIMUI_SDK/usr/lib/libGLESv2.so" \
    -Wl,-rpath-link,"$VENDOR_LIBS"

if [ "$SKIP_CMAKE" -eq 0 ] && [ -f "$REPO_ROOT/CMakeLists.txt" ]; then
    echo "==> building CMake project for target"
    TRIMUI_SYSROOT="$SYSROOT" cmake \
        -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/toolchain-aarch64.cmake" \
        -DBRICKDESK_BACKEND=target \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build "$BUILD_DIR"
fi

echo "==> staging runtime libraries"
rm -rf "$BUILD_DIR/dist"
mkdir -p "$BUILD_DIR/dist/lib" "$BUILD_DIR/dist/bin"
cp -a "$SYSROOT/lib/libSDL2-2.0.so.0"* "$BUILD_DIR/dist/lib/"
cp -a "$BUILD_DIR/display_probe" "$BUILD_DIR/dist/bin/"
for binary in brickdesk brickdesk-inputd bd-input; do
    if [ -e "$BUILD_DIR/$binary" ]; then
        cp -a "$BUILD_DIR/$binary" "$BUILD_DIR/dist/bin/"
    fi
done

echo "==> checking GLIBC requirements"
CHECK_TARGETS="$BUILD_DIR/display_probe"
for binary in brickdesk brickdesk-inputd bd-input; do
    if [ -e "$BUILD_DIR/$binary" ]; then
        CHECK_TARGETS="$CHECK_TARGETS $BUILD_DIR/$binary"
    fi
done
# shellcheck disable=SC2086
"$REPO_ROOT/ci/check-glibc.sh" --max 2.23 $CHECK_TARGETS | tee "$BUILD_DIR/glibc-report.txt"
"$REPO_ROOT/ci/check-glibc.sh" --max 2.23 "$SYSROOT/lib/libSDL2-2.0.so.0" |
    tee -a "$BUILD_DIR/glibc-report.txt"

echo "==> writing SHA256SUMS"
(
    cd "$BUILD_DIR/dist"
    sums=$BUILD_DIR/.SHA256SUMS.tmp
    rm -f "$sums"
    find . -type f ! -name SHA256SUMS -exec sha256sum {} \; | sort -k2 >"$sums"
    mv "$sums" SHA256SUMS
)

echo "==> target artifacts in $BUILD_DIR/dist"
find "$BUILD_DIR/dist" -type f -o -type l | sort
