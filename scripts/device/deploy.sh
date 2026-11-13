#!/bin/sh
# Deploy brickdesk/display_probe, the patched SDL2 runtime, assets and default
# config to the SD card. Only /mnt/SDCARD/brickdesk is written; stock
# partitions are never touched. Existing files in that directory are replaced
# individually; config/local.ini is never pushed.
#
# Usage: scripts/device/deploy.sh
# Environment: ADB, DEVICE_ROOT, BUILD_DIR, SDL2_LIB
set -eu

SCRIPT_DIR=$(CDPATH="" cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH="" cd -- "$SCRIPT_DIR/../.." && pwd)
export REPO_ROOT
# shellcheck source=scripts/device/common.sh
. "$SCRIPT_DIR/common.sh"

DEST=${DEVICE_ROOT:-/mnt/SDCARD/brickdesk}
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
    info "pushing patched SDL2 runtime: $sdl2"
    "$ADB" push "$sdl2" "$DEST/lib/libSDL2-2.0.so.0"
else
    warn "patched libSDL2-2.0.so.0 not found under $BUILD_DIR; the device will use its stock SDL2"
fi

"$ADB" shell "chmod +x '$DEST/bin/display_probe'; if [ -f '$DEST/bin/brickdesk' ]; then chmod +x '$DEST/bin/brickdesk'; fi; sync"

info "deployed to $DEST"
info "run: scripts/device/run.sh --foreground display_probe"
