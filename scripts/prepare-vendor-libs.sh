#!/bin/sh
# Create a link directory containing only the SDK's vendor libraries.
#
# The SDK also ships glibc 2.23 stubs (libpthread-2.23.so, libdl-2.23.so).
# Putting $SDK/usr/lib on the linker search path makes ld pick those up and fail
# with __libc_dl_error_tsd@GLIBC_PRIVATE against the toolchain's glibc 2.25.
# Resolving vendor DT_NEEDED entries from this filtered directory instead keeps
# libc/libpthread coming from the 7.5 toolchain.
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
mkdir -p "$DEST"

for lib in "$SDK"/usr/lib/*.so*; do
    [ -e "$lib" ] || continue
    base=$(basename "$lib")
    case "$base" in
        libc.so*|libpthread*|libdl*|libm.so*|librt*|libgcc_s*|ld-*) continue ;;
    esac
    ln -sf "$lib" "$DEST/$base"
done

echo "vendor library links: $DEST"
