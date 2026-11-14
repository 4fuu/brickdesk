#!/bin/sh
# Stop brickdesk/display_probe and bring the stock MainUI back.
#
# Nothing on a stock partition is modified. The stock launcher is restarted via
# its init script; if that is missing, runtrimui.sh is used as a fallback. Both
# paths are unverified until the device arrives.
#
# Usage: scripts/device/recover.sh
set -eu

SCRIPT_DIR=$(CDPATH="" cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH="" cd -- "$SCRIPT_DIR/../.." && pwd)
export REPO_ROOT
# shellcheck source=scripts/device/common.sh
. "$SCRIPT_DIR/common.sh"

require_device

info "stopping brickdesk/display_probe"
"$ADB" shell "for name in brickdesk display_probe; do for pid in \$(pidof \$name 2>/dev/null); do kill -9 \$pid 2>/dev/null || true; done; done; sleep 1"
"$ADB" shell "rm -f '$DEVICE_TMP/brickdesk.pid' '$DEVICE_TMP/display_probe.pid'"

start_stock_ui
"$ADB" shell sync

sleep 1
mainui=$(device_pidof MainUI)
if [ -n "$mainui" ]; then
    info "stock MainUI is running (pid $mainui)"
else
    warn "MainUI is not running after recovery; inspect with scripts/device/logs.sh"
fi
info "recovery command finished"
