#!/bin/sh
# Build the versioned Brick Pro release zip from a target build.
#
# Usage: scripts/package-target.sh [--build-dir DIR] [--out-dir DIR] [--version V]
#
# The target build is produced by scripts/build-target.sh. This script stages
# the runtime tree, adds assets/config/plugins/apps and the on-device installer,
# then writes:
#   <out-dir>/brickdesk-<version>-aarch64.zip
#   <out-dir>/SHA256SUMS
#
# POSIX sh; runs on the host and inside ci/Dockerfile.toolchain.

set -eu

SCRIPT_DIR=$(CDPATH='' cd "$(dirname "$0")" && pwd) || exit 1
REPO_ROOT=$(CDPATH='' cd "$SCRIPT_DIR/.." && pwd) || exit 1
BUILD_DIR=$REPO_ROOT/build-target
OUT_DIR=
VERSION=

info() {
    printf '==> %s\n' "$*"
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

usage() {
    sed -n '2,14p' "$0"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir) [ "$#" -ge 2 ] || die "--build-dir needs a value"; BUILD_DIR=$2; shift 2 ;;
        --out-dir) [ "$#" -ge 2 ] || die "--out-dir needs a value"; OUT_DIR=$2; shift 2 ;;
        --version) [ "$#" -ge 2 ] || die "--version needs a value"; VERSION=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

[ -d "$BUILD_DIR" ] || die "build directory not found: $BUILD_DIR"
[ -f "$REPO_ROOT/CMakeLists.txt" ] || die "CMakeLists.txt not found under $REPO_ROOT"

if [ -z "$VERSION" ]; then
    VERSION=${BRICKDESK_PACKAGE_VERSION:-}
fi
if [ -z "$VERSION" ]; then
    VERSION=$(sed -n 's/^project(brickdesk VERSION \([0-9][0-9.]*\).*/\1/p' "$REPO_ROOT/CMakeLists.txt" | head -n 1)
fi
[ -n "$VERSION" ] || die "cannot determine the package version; pass --version"

GIT_REV=${BRICKDESK_GIT_REV:-$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)}
PKG_NAME=brickdesk-$VERSION-aarch64
if [ -z "$OUT_DIR" ]; then
    OUT_DIR=$BUILD_DIR/package
fi
mkdir -p "$OUT_DIR" || die "cannot create output directory: $OUT_DIR"
OUT_DIR=$(CDPATH='' cd "$OUT_DIR" && pwd) || die "cannot resolve output directory: $OUT_DIR"
STAGE=$OUT_DIR/$PKG_NAME
ZIP=$OUT_DIR/$PKG_NAME.zip

resolve() {
    # resolve <description> <candidate>...
    description=$1
    shift
    for candidate in "$@"; do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    die "missing $description (looked for: $*)"
}

BIN_BRICKDESK=$(resolve brickdesk "$BUILD_DIR/brickdesk")
BIN_INPUTD=$(resolve brickdesk-inputd "$BUILD_DIR/brickdesk-inputd")
BIN_PERFD=$(resolve brickdesk-perfd "$BUILD_DIR/brickdesk-perfd")
BIN_BD_INPUT=$(resolve bd-input "$BUILD_DIR/bd-input")
BIN_CONFIG=$(resolve brickdesk-config "$BUILD_DIR/brickdesk-config")
BIN_BD_AV=$(resolve bd-av "$BUILD_DIR/bd-av")
BIN_BD_POWER=$(resolve bd-power "$BUILD_DIR/bd-power")
BIN_BD_BATTERY=$(resolve bd-battery "$BUILD_DIR/bd-battery")
BIN_PROBE=$(resolve display_probe "$BUILD_DIR/display_probe" "$BUILD_DIR/bin/display_probe")
BIN_BENCH=$(resolve bench_render "$BUILD_DIR/bench/bench_render" "$BUILD_DIR/bench_render")
BIN_DEMO=$(resolve brickdesk-demo "$BUILD_DIR/brickdesk-demo")
LIB_MENU=$(resolve libbrickdesk-menu.so "$BUILD_DIR/libbrickdesk-menu.so" "$BUILD_DIR/lib/libbrickdesk-menu.so")
LIB_SDL2=$(resolve patched SDL2 \
    "$BUILD_DIR/dist/lib/libSDL2-2.0.so.0" \
    "$BUILD_DIR/sysroot/lib/libSDL2-2.0.so.0" \
    "$REPO_ROOT/build-target/sysroot/lib/libSDL2-2.0.so.0")
if [ -n "${BRICKDESK_HELLO_BIN:-}" ]; then
    HELLO_BIN=$BRICKDESK_HELLO_BIN
    [ -f "$HELLO_BIN" ] || die "BRICKDESK_HELLO_BIN does not exist: $HELLO_BIN"
else
    HELLO_BIN=$(resolve hello plugin binary "$REPO_ROOT/plugins/hello/bin/hello")
fi
if [ -n "${BRICKDESK_HELLO_MANIFEST:-}" ]; then
    HELLO_MANIFEST=$BRICKDESK_HELLO_MANIFEST
    [ -f "$HELLO_MANIFEST" ] || die "BRICKDESK_HELLO_MANIFEST does not exist: $HELLO_MANIFEST"
else
    HELLO_MANIFEST=$(resolve hello plugin manifest "$REPO_ROOT/plugins/hello/plugin.json")
fi

[ -d "$REPO_ROOT/assets/fonts" ] || die "missing assets/fonts"
[ -d "$REPO_ROOT/assets/covers" ] || die "missing assets/covers"
[ -f "$REPO_ROOT/config/brickdesk.ini" ] || die "missing config/brickdesk.ini"
[ -f "$REPO_ROOT/apps/demo/app.json" ] || die "missing apps/demo/app.json"

info "packaging brickdesk $VERSION (git $GIT_REV)"
info "build: $BUILD_DIR"
info "output: $OUT_DIR"

rm -rf "$STAGE" "$ZIP" "$OUT_DIR/SHA256SUMS"
mkdir -p "$STAGE/payload/bin" "$STAGE/payload/lib" "$STAGE/payload/assets" \
    "$STAGE/payload/config" "$STAGE/payload/plugins/hello/bin" \
    "$STAGE/payload/apps/brickdesk-demo" "$STAGE/docs"

copy_bin() {
    src=$1
    dst=$2
    cp "$src" "$dst" || die "cannot copy $src"
    chmod 0755 "$dst" 2>/dev/null || true
}

copy_bin "$BIN_BRICKDESK" "$STAGE/payload/bin/brickdesk"
copy_bin "$BIN_INPUTD" "$STAGE/payload/bin/brickdesk-inputd"
copy_bin "$BIN_PERFD" "$STAGE/payload/bin/brickdesk-perfd"
copy_bin "$BIN_BD_INPUT" "$STAGE/payload/bin/bd-input"
copy_bin "$BIN_CONFIG" "$STAGE/payload/bin/brickdesk-config"
copy_bin "$BIN_BD_AV" "$STAGE/payload/bin/bd-av"
copy_bin "$BIN_BD_POWER" "$STAGE/payload/bin/bd-power"
copy_bin "$BIN_BD_BATTERY" "$STAGE/payload/bin/bd-battery"
copy_bin "$BIN_PROBE" "$STAGE/payload/bin/display_probe"
copy_bin "$BIN_BENCH" "$STAGE/payload/bin/bench_render"
copy_bin "$BIN_DEMO" "$STAGE/payload/bin/brickdesk-demo"
copy_bin "$LIB_MENU" "$STAGE/payload/lib/libbrickdesk-menu.so"
# cp follows the symlink, so the package has a real file for FAT32 SD cards.
copy_bin "$LIB_SDL2" "$STAGE/payload/lib/libSDL2-2.0.so.0"

cp -R "$REPO_ROOT/assets/fonts" "$STAGE/payload/assets/" || die "cannot copy assets/fonts"
cp -R "$REPO_ROOT/assets/covers" "$STAGE/payload/assets/" || die "cannot copy assets/covers"

for ini in brickdesk.ini perf.ini hotkeys.ini input.ini; do
    cp "$REPO_ROOT/config/$ini" "$STAGE/payload/config/$ini" || die "cannot copy config/$ini"
done

cp "$HELLO_MANIFEST" "$STAGE/payload/plugins/hello/plugin.json" || die "cannot copy hello manifest"
copy_bin "$HELLO_BIN" "$STAGE/payload/plugins/hello/bin/hello"

cp "$REPO_ROOT/apps/demo/app.json" "$STAGE/payload/apps/brickdesk-demo/app.json" ||
    die "cannot copy the demo app manifest"
grep -q '/mnt/SDCARD/brickdesk/apps/brickdesk-demo/brickdesk-demo' \
    "$STAGE/payload/apps/brickdesk-demo/app.json" ||
    die "demo app manifest does not point at the installed location"
copy_bin "$BIN_DEMO" "$STAGE/payload/apps/brickdesk-demo/brickdesk-demo"

cp "$REPO_ROOT/scripts/package/boot-hook.sh" "$STAGE/payload/boot-hook.sh" ||
    die "cannot copy boot-hook.sh"
chmod 0755 "$STAGE/payload/boot-hook.sh" 2>/dev/null || true

printf '%s\n' "$VERSION" >"$STAGE/VERSION"
printf '%s\n' "$GIT_REV" >"$STAGE/GIT_REV"
printf '%s\n' "$VERSION" >"$STAGE/payload/VERSION"

sed "s/@VERSION@/$VERSION/g" "$REPO_ROOT/scripts/package/README.txt" >"$STAGE/README.txt" ||
    die "cannot generate README.txt"
cp "$REPO_ROOT/scripts/package/install.sh" "$STAGE/install.sh" || die "cannot copy install.sh"
cp "$REPO_ROOT/scripts/package/uninstall.sh" "$STAGE/uninstall.sh" || die "cannot copy uninstall.sh"
chmod 0755 "$STAGE/install.sh" "$STAGE/uninstall.sh" 2>/dev/null || true

if [ -f "$REPO_ROOT/docs/install.md" ]; then
    cp "$REPO_ROOT/docs/install.md" "$STAGE/docs/install.md" || die "cannot copy docs/install.md"
fi
if [ -f "$REPO_ROOT/docs/device-av.md" ]; then
    cp "$REPO_ROOT/docs/device-av.md" "$STAGE/docs/device-av.md" || die "cannot copy docs/device-av.md"
fi
if [ -f "$REPO_ROOT/docs/device-power.md" ]; then
    cp "$REPO_ROOT/docs/device-power.md" "$STAGE/docs/device-power.md" || die "cannot copy docs/device-power.md"
fi
if [ -f "$REPO_ROOT/docs/device-battery.md" ]; then
    cp "$REPO_ROOT/docs/device-battery.md" "$STAGE/docs/device-battery.md" || die "cannot copy docs/device-battery.md"
fi

check_aarch64() {
    # Reject host ELF binaries in a device package. Shell scripts are allowed
    # for the offline package tests.
    if command -v file >/dev/null 2>&1; then
        description=$(file -L -b "$1" 2>/dev/null || true)
        case "$description" in
            *ELF*)
                case "$description" in
                    *aarch64*) ;;
                    *) die "not an aarch64 binary: $1 ($description)" ;;
                esac
                ;;
        esac
    fi
}

for artifact in "$BIN_BRICKDESK" "$BIN_INPUTD" "$BIN_PERFD" "$BIN_BD_INPUT" \
    "$BIN_CONFIG" "$BIN_BD_AV" "$BIN_BD_POWER" "$BIN_BD_BATTERY" \
    "$BIN_PROBE" "$BIN_BENCH" "$BIN_DEMO" "$LIB_MENU" "$LIB_SDL2" \
    "$HELLO_BIN"; do
    check_aarch64 "$artifact"
done

for rel in \
    payload/bin/brickdesk \
    payload/bin/brickdesk-inputd \
    payload/bin/brickdesk-perfd \
    payload/bin/bd-input \
    payload/bin/brickdesk-config \
    payload/bin/bd-av \
    payload/bin/bd-power \
    payload/bin/bd-battery \
    payload/bin/display_probe \
    payload/bin/bench_render \
    payload/bin/brickdesk-demo \
    payload/lib/libbrickdesk-menu.so \
    payload/lib/libSDL2-2.0.so.0 \
    payload/assets/fonts/NotoSansSC-500-subset.ttf \
    payload/assets/covers/cover-01.png \
    payload/config/brickdesk.ini \
    payload/config/perf.ini \
    payload/config/hotkeys.ini \
    payload/config/input.ini \
    payload/plugins/hello/plugin.json \
    payload/plugins/hello/bin/hello \
    payload/apps/brickdesk-demo/app.json \
    payload/apps/brickdesk-demo/brickdesk-demo \
    payload/boot-hook.sh \
    docs/device-power.md \
    docs/device-battery.md \
    install.sh \
    uninstall.sh; do
    [ -f "$STAGE/$rel" ] || die "package is missing $rel"
done

if find "$STAGE" -type l | grep -q .; then
    die "package contains a symlink; FAT32 SD cards cannot preserve it"
fi

info "writing package checksums"
sums=$OUT_DIR/.SHA256SUMS.$$
rm -f "$sums"
( cd "$STAGE" && find . -type f ! -name SHA256SUMS -exec sha256sum {} \; | sort -k2 >"$sums" ) ||
    die "cannot write the package SHA256SUMS"
mv "$sums" "$STAGE/SHA256SUMS" || die "cannot install the package SHA256SUMS"

info "creating $ZIP"
python3 - "$STAGE" "$ZIP" <<'PY'
import os
import sys
import zipfile

stage = os.path.abspath(sys.argv[1])
target = os.path.abspath(sys.argv[2])
parent = os.path.dirname(stage)
arc_root = os.path.basename(stage)
stamp = (2026, 1, 1, 0, 0, 0)

with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
    for root, dirs, files in os.walk(stage):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(root, name)
            if os.path.islink(path):
                raise SystemExit("symlink in package: %s" % path)
            rel = os.path.relpath(path, parent)
            mode = os.stat(path).st_mode & 0o777
            info = zipfile.ZipInfo(rel, date_time=stamp)
            info.create_system = 3
            info.external_attr = (mode & 0xFFFF) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(path, "rb") as src, zf.open(info, "w") as dst:
                while True:
                    chunk = src.read(1024 * 1024)
                    if not chunk:
                        break
                    dst.write(chunk)
PY
[ -f "$ZIP" ] || die "zip was not created"

( cd "$OUT_DIR" && sha256sum "$PKG_NAME.zip" >SHA256SUMS ) || die "cannot write $OUT_DIR/SHA256SUMS"

info "package: $ZIP"
du -h "$ZIP" 2>/dev/null || true
info "checksums: $OUT_DIR/SHA256SUMS"
cat "$OUT_DIR/SHA256SUMS"
