#!/bin/sh
# Deploy brickdesk/display_probe, the fallback SDL2 runtime, assets and default
# config to the SD card. Only /mnt/SDCARD/brickdesk is written; stock
# partitions are never touched. Existing files in that directory are replaced
# individually; config/local.ini is never pushed. If the device has a usable
# stock SDL2, it is copied over the fallback after the patched library is pushed.
#
# Usage: scripts/device/deploy.sh
# Environment: ADB, DEVICE_ROOT, BUILD_DIR, SDL2_LIB, BRICKDESK_STOCK_SDL2
set -eu

SCRIPT_DIR=$(CDPATH="" cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH="" cd -- "$SCRIPT_DIR/../.." && pwd)
export REPO_ROOT
# shellcheck source=scripts/device/common.sh
. "$SCRIPT_DIR/common.sh"

DEST=${DEVICE_ROOT:-/mnt/SDCARD/brickdesk}
STOCK_SDL2=${BRICKDESK_STOCK_SDL2:-/usr/trimui/lib/libSDL2-2.0.so.0}
require_device
ensure_sd_dest

info "device: $("$ADB" get-serialno 2>/dev/null | tr -d '\r')"
info "destination: $DEST"
info "local build: $BUILD_DIR"

require_file "$BUILD_DIR/display_probe"

"$ADB" shell "mkdir -p '$DEST/bin' '$DEST/lib' '$DEST/logs' '$DEST/assets' '$DEST/config'"

info "pushing display_probe"
"$ADB" push "$BUILD_DIR/display_probe" "$DEST/bin/display_probe"

if [ -f "$BUILD_DIR/brickdesk" ]; then
    info "pushing brickdesk"
    "$ADB" push "$BUILD_DIR/brickdesk" "$DEST/bin/brickdesk"
else
    warn "brickdesk is not built yet; deploying display_probe only"
fi

info "pushing assets and default config"
"$ADB" push "$REPO_ROOT/assets/fonts" "$DEST/assets/"
"$ADB" push "$REPO_ROOT/assets/covers" "$DEST/assets/"
for ini in brickdesk.ini perf.ini hotkeys.ini input.ini; do
    if [ -f "$REPO_ROOT/config/$ini" ]; then
        "$ADB" push "$REPO_ROOT/config/$ini" "$DEST/config/$ini"
    fi
done

sdl2=$(local_sdl2_lib || true)
if [ -n "$sdl2" ]; then
    info "pushing fallback patched SDL2 runtime: $sdl2"
    "$ADB" push "$sdl2" "$DEST/lib/libSDL2-2.0.so.0"
else
    warn "patched libSDL2-2.0.so.0 not found under $BUILD_DIR; relying on stock SDL2"
fi

stock_tmp="$DEST/lib/.libSDL2-2.0.so.0.stock.$$"
if "$ADB" shell "[ -r '$STOCK_SDL2' ]" >/dev/null 2>&1; then
    info "copying stock SDL2 from $STOCK_SDL2"
    if "$ADB" shell "cat '$STOCK_SDL2' >'$stock_tmp' && [ -s '$stock_tmp' ] && { chmod 0755 '$stock_tmp' 2>/dev/null || true; } && mv -f '$stock_tmp' '$DEST/lib/libSDL2-2.0.so.0'"; then
        info "stock SDL2 installed at $DEST/lib/libSDL2-2.0.so.0"
    else
        warn "could not copy stock SDL2; keeping the packaged fallback"
        "$ADB" shell "rm -f '$stock_tmp'"
    fi
else
    warn "stock SDL2 not found at $STOCK_SDL2; keeping the packaged fallback"
fi

"$ADB" shell "chmod +x '$DEST/bin/display_probe'; if [ -f '$DEST/bin/brickdesk' ]; then chmod +x '$DEST/bin/brickdesk'; fi; sync"

info "deployed to $DEST"
info "run: scripts/device/run.sh --foreground display_probe"
