#!/bin/sh
# Run the staged bench_render on the device and pull its JSON result.
#
# The release package installs bench_render in $DEVICE_ROOT/bin and ships
# assets/ next to it, so a packaged install needs no extra staging. For a
# development tree, deploy the binary and assets manually; see
# docs/performance.md.
#
# Usage: scripts/device/bench.sh [--scenario NAME] [--frames N] [--idle-ms N]
# Environment: ADB, DEVICE_ROOT, DEVICE_TMP
set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd)
export REPO_ROOT
# shellcheck source=scripts/device/common.sh
. "$SCRIPT_DIR/common.sh"

ARGS="--scenario all --frames 120 --idle-ms 3000"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --scenario) [ "$#" -ge 2 ] || die "--scenario needs a value"; ARGS="$ARGS --scenario $2"; shift 2 ;;
        --frames) [ "$#" -ge 2 ] || die "--frames needs a value"; ARGS="$ARGS --frames $2"; shift 2 ;;
        --idle-ms) [ "$#" -ge 2 ] || die "--idle-ms needs a value"; ARGS="$ARGS --idle-ms $2"; shift 2 ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

require_device
DEST=${DEVICE_ROOT:-/mnt/SDCARD/brickdesk}
BIN="$DEST/bin/bench_render"
if ! "$ADB" shell "[ -x '$BIN' ]" >/dev/null 2>&1; then
    die "$BIN is missing; install the release package or stage bench_render first (docs/performance.md)"
fi

"$ADB" shell "mkdir -p '$DEVICE_TMP'"
stop_stock_ui
info "running $BIN $ARGS"
"$ADB" shell "cd '$DEST' && LD_LIBRARY_PATH='/usr/trimui/lib:$DEST/lib:/usr/lib:/lib' '$BIN' $ARGS --json '$DEVICE_TMP/bench.json'"

OUT=${OUT:-"$REPO_ROOT/build-device/bench.json"}
mkdir -p "$(dirname -- "$OUT")"
"$ADB" pull "$DEVICE_TMP/bench.json" "$OUT" >/dev/null
info "JSON saved to $OUT"
info "run scripts/device/recover.sh when you are done with the device"
