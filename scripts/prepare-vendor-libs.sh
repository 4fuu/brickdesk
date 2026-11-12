#!/bin/sh
# Create a link directory containing only the SDK's vendor libraries.
#
# The SDK also ships glibc 2.23 stubs (libpthread-2.23.so, libdl-2.23.so).
# Putting $SDK/usr/lib on the linker search path makes ld pick those up and fail
# with __libc_dl_error_tsd@GLIBC_PRIVATE against the toolchain's glibc 2.25.
# Resolving vendor DT_NEEDED entries from this filtered directory instead keeps
# libc/libpthread coming from the 7.5 toolchain.
#
# Real files are hard-linked (copied across filesystems) so CMake's find_library
# returns this directory instead of resolving a symlink back into $SDK/usr/lib.
# Symlinks are recreated relative to the directory.
#
# Usage: scripts/prepare-vendor-libs.sh SDK_DIR DEST_DIR
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 SDK_DIR DEST_DIR" >&2
    exit 2
fi

SDK=$1
DEST=$2

if [ ! -d "$SDK/usr/lib" ]; then
    echo "error: SDK library directory not found: $SDK/usr/lib" >&2
    exit 1
fi

rm -rf "$DEST"
mkdir -p "$DEST/lib"
LIBDIR=$DEST/lib

skip() {
    case "$1" in
        libc.so*|libpthread*|libdl*|libm.so*|librt*|libgcc_s*|libSDL2*|ld-*) return 0 ;;
        *) return 1 ;;
    esac
}

# Pass 1: real files. Hard links keep the image small; the copy fallback covers
# destinations on another filesystem (for example a build directory on a mount).
for lib in "$SDK"/usr/lib/*.so*; do
    [ -f "$lib" ] || continue
    [ -L "$lib" ] && continue
    base=$(basename "$lib")
    if skip "$base"; then
        continue
    fi
    ln "$lib" "$LIBDIR/$base" 2>/dev/null || cp -p "$lib" "$LIBDIR/$base"
done

# Pass 2: symlinks, recreated relative to files that exist in DEST.
for lib in "$SDK"/usr/lib/*.so*; do
    [ -L "$lib" ] || continue
    base=$(basename "$lib")
    if skip "$base"; then
        continue
    fi
    target=$(readlink -f "$lib")
    target_base=$(basename "$target")
    if [ -e "$LIBDIR/$target_base" ]; then
        ln -sf "$target_base" "$LIBDIR/$base"
    fi
done

echo "vendor library prefix: $DEST (libraries in $LIBDIR)"
