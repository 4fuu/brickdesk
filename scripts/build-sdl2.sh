#!/bin/sh
# Build the TrimUI SDK SDL2 (2.26.1.GE8300) with brickdesk's fbdev-mode patch.
#
# The stock package hardcodes a 1280x720 window in the mali-fbdev driver.
# patches/sdl2/0001-read-fb-mode.patch makes it read /dev/fb0 instead, so the
# same library works on the 1024x768 Brick Pro.
#
# Environment:
#   TRIMUI_TOOLCHAIN  toolchain root (default /opt/trimui/toolchain)
#   TRIMUI_SDK        SDK root (default /opt/trimui/sdk)
#   TRIMUI_SYSROOT    install prefix (default build-target/sysroot)
#   SDL2_SRC          extracted SDL2 source tree (default /opt/trimui/sdl2-src/SDL2-2.26.1)
#   SDL2_TARBALL      tarball to extract when SDL2_SRC is missing
#
# Usage: scripts/build-sdl2.sh [--prefix DIR] [--src DIR] [--build DIR] [--jobs N]
set -eu

SCRIPT_DIR=$(CDPATH="" cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH="" cd -- "$SCRIPT_DIR/.." && pwd)

TRIMUI_TOOLCHAIN=${TRIMUI_TOOLCHAIN:-/opt/trimui/toolchain}
TRIMUI_SDK=${TRIMUI_SDK:-/opt/trimui/sdk}
PREFIX=${TRIMUI_SYSROOT:-$REPO_ROOT/build-target/sysroot}
SRC=${SDL2_SRC:-/opt/trimui/sdl2-src/SDL2-2.26.1}
BUILD_DIR=
JOBS=

need_value() {
    if [ "$#" -lt 2 ]; then
        echo "error: $1 needs a value" >&2
        exit 2
    fi
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix) need_value "$@"; PREFIX=$2; shift 2 ;;
        --src) need_value "$@"; SRC=$2; shift 2 ;;
        --build) need_value "$@"; BUILD_DIR=$2; shift 2 ;;
        --jobs) need_value "$@"; JOBS=$2; shift 2 ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ ! -x "$TRIMUI_TOOLCHAIN/bin/aarch64-linux-gnu-gcc" ]; then
    echo "error: aarch64-linux-gnu-gcc not found under $TRIMUI_TOOLCHAIN" >&2
    exit 1
fi
if [ ! -d "$TRIMUI_SDK/usr/include/EGL" ]; then
    echo "error: EGL headers not found under $TRIMUI_SDK/usr/include" >&2
    exit 1
fi

if [ ! -d "$SRC" ]; then
    tarball=${SDL2_TARBALL:-}
    if [ -z "$tarball" ] && [ -f "$TRIMUI_TOOLCHAIN/../dl/SDL2-2.26.1.GE8300.tgz" ]; then
        tarball=$TRIMUI_TOOLCHAIN/../dl/SDL2-2.26.1.GE8300.tgz
    fi
    if [ -z "$tarball" ] || [ ! -f "$tarball" ]; then
        echo "error: SDL2 source not found: $SRC" >&2
        echo "       extract SDL2-2.26.1.GE8300.tgz and pass --src, or set SDL2_TARBALL" >&2
        exit 1
    fi
    extract_dir=$(dirname "$SRC")
    mkdir -p "$extract_dir"
    tar -xzf "$tarball" -C "$extract_dir"
fi

BUILD_DIR=${BUILD_DIR:-$PREFIX/../sdl2-build}
mkdir -p "$BUILD_DIR" "$PREFIX"

WORK_DIR=$BUILD_DIR/SDL2-2.26.1
rm -rf "$WORK_DIR"
cp -a "$SRC" "$WORK_DIR"

PATCH_FILE=$REPO_ROOT/patches/sdl2/0001-read-fb-mode.patch
echo "==> applying $(basename "$PATCH_FILE")"
(
    cd "$WORK_DIR"
    patch -p1 --batch --forward <"$PATCH_FILE"
)

# Link tests for optional libraries must not see the SDK's glibc 2.23 stubs
# (libpthread-2.23.so / libdl-2.23.so). Expose only vendor libs through a
# filtered directory so the linker resolves libc from the 7.5 toolchain.
VENDOR_LIBS=$BUILD_DIR/vendor-libs
rm -rf "$VENDOR_LIBS"
mkdir -p "$VENDOR_LIBS"
for lib in libasound.so libasound.so.2; do
    if [ -e "$TRIMUI_SDK/usr/lib/$lib" ]; then
        ln -s "$TRIMUI_SDK/usr/lib/$lib" "$VENDOR_LIBS/$lib"
    fi
done

JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

echo "==> configuring SDL2 for aarch64 (prefix $PREFIX)"
(
    cd "$WORK_DIR"
    PATH=$TRIMUI_TOOLCHAIN/bin:$PATH \
    CC=aarch64-linux-gnu-gcc \
    CXX=aarch64-linux-gnu-g++ \
    CFLAGS="-O2 -mcpu=cortex-a53 -I$TRIMUI_SDK/usr/include" \
    LDFLAGS="-L$VENDOR_LIBS" \
    ./configure \
        --host=aarch64-linux-gnu \
        --prefix="$PREFIX" \
        --disable-video-x11 \
        --disable-video-wayland \
        --disable-wayland-shared \
        --disable-video-kmsdrm \
        --disable-video-vulkan \
        --disable-video-rpi \
        --disable-video-vivante \
        --disable-video-directfb \
        --disable-video-cocoa \
        --disable-video-metal \
        --disable-render-metal \
        --disable-video-opengl \
        --enable-video-opengles \
        --enable-video-opengles2 \
        --enable-video-mali \
        --disable-video-dummy \
        --disable-video-offscreen \
        --disable-pulseaudio \
        --disable-pipewire \
        --disable-jack \
        --disable-esd \
        --disable-arts \
        --disable-nas \
        --disable-sndio \
        --disable-hidapi \
        --disable-sensor \
        --disable-power \
        --disable-dbus \
        --disable-ime \
        --disable-ibus \
        --disable-fcitx \
        --disable-libudev \
        --enable-alsa \
        --enable-shared \
        --enable-static
)

echo "==> building SDL2 (-j$JOBS)"
(
    cd "$WORK_DIR"
    PATH=$TRIMUI_TOOLCHAIN/bin:$PATH
    export PATH
    make -j"$JOBS"
    make install
)

# The 2.26.1 install ships libSDL2-2.0.so.0.2600.1 plus the usual symlinks.
# Keep the runtime library next to the probe as well, so deploy.sh can push it
# without depending on the build tree layout.
mkdir -p "$PREFIX/lib"
if [ -e "$PREFIX/lib/libSDL2-2.0.so.0" ]; then
    echo "==> SDL2 installed under $PREFIX"
else
    echo "error: SDL2 install did not produce $PREFIX/lib/libSDL2-2.0.so.0" >&2
    exit 1
fi

# CMake projects linking EGL/GLES need to resolve the vendor DT_NEEDED graph
# without the SDK's glibc stubs. cmake/toolchain-aarch64.cmake looks here first.
"$REPO_ROOT/scripts/prepare-vendor-libs.sh" "$TRIMUI_SDK" "$PREFIX/vendor-lib"

echo "==> checking GLIBC symbol versions in the shared library"
"$REPO_ROOT/ci/check-glibc.sh" --max 2.23 "$PREFIX/lib/libSDL2-2.0.so.0"
