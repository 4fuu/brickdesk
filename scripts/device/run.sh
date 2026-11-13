#!/bin/sh
# Launch a deployed binary with the patched SDL2 on LD_LIBRARY_PATH.
#
# The stock UI is stopped first by default because the PowerVR NULL WSEGL stack
# has no compositor: two EGL/display owners cannot coexist. recover.sh starts
# the stock UI again.
#
# Usage:
#   scripts/device/run.sh [--foreground] [binary]
#     binary defaults to brickdesk; a bare name is resolved in $DEVICE_ROOT/bin
#   scripts/device/run.sh --foreground display_probe
#
# Environment: ADB, DEVICE_ROOT, STOP_STOCK=0, SDL_VIDEODRIVER
set -eu

SCRIPT_DIR=$(CDPATH="" cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH="" cd -- "$SCRIPT_DIR/../.." && pwd)
# shellcheck source=scripts/device/common.sh
. "$SCRIPT_DIR/common.sh"

DEST=${DEVICE_ROOT:-/mnt/SDCARD/brickdesk}
FOREGROUND=0
BINARY=$APP_NAME

while [ "$#" -gt 0 ]; do
    case "$1" in
        --foreground) FOREGROUND=1; shift ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        -*) die "unknown option: $1" ;;
        *) BINARY=$1; shift ;;
    esac
done

require_device
ensure_sd_dest

case "$BINARY" in
    /*) BIN=$BINARY ;;
    *) BIN=$DEST/bin/$BINARY ;;
esac

if ! "$ADB" shell "[ -x '$BIN' ]" >/dev/null 2>&1; then
    die "$BIN is not executable on the device; run scripts/device/deploy.sh first"
fi

NAME=$(basename "$BIN")
LOG=$DEST/logs/$NAME.log
PIDFILE=$DEVICE_TMP/$NAME.pid
LD_PATH=$DEST/lib:/usr/trimui/lib:/usr/lib:/lib

if [ "${STOP_STOCK:-1}" = "1" ]; then
    stop_stock_ui
fi

"$ADB" shell "mkdir -p '$DEST/logs' '$DEVICE_TMP'"

if [ "$FOREGROUND" = "1" ]; then
    info "running $BIN in the foreground"
    rc=0
    if [ -n "${SDL_VIDEODRIVER:-}" ]; then
        "$ADB" shell "cd '$DEST' && LD_LIBRARY_PATH='$LD_PATH' SDL_VIDEODRIVER='$SDL_VIDEODRIVER' '$BIN'" || rc=$?
    else
        "$ADB" shell "cd '$DEST' && LD_LIBRARY_PATH='$LD_PATH' '$BIN'" || rc=$?
    fi
    exit "$rc"
fi

running=$(device_pidof "$NAME" || true)
if [ -n "$running" ]; then
    info "$NAME is already running as pid $running"
    exit 0
fi

info "starting $BIN"
if [ -n "${SDL_VIDEODRIVER:-}" ]; then
    "$ADB" shell "cd '$DEST' && LD_LIBRARY_PATH='$LD_PATH' SDL_VIDEODRIVER='$SDL_VIDEODRIVER' nohup '$BIN' </dev/null >>'$LOG' 2>&1 & echo \$! >'$PIDFILE'"
else
    "$ADB" shell "cd '$DEST' && LD_LIBRARY_PATH='$LD_PATH' nohup '$BIN' </dev/null >>'$LOG' 2>&1 & echo \$! >'$PIDFILE'"
fi

sleep 2
pid=$(device_shell_capture "cat '$PIDFILE' 2>/dev/null")
if [ -z "$pid" ]; then
    pid=$(device_pidof "$NAME")
fi

if pid_alive "$pid"; then
    info "$NAME running as pid $pid; log: $LOG"
    "$ADB" shell "tail -n 20 '$LOG' 2>/dev/null" || true
else
    warn "$NAME exited during startup; last log lines:"
    "$ADB" shell "tail -n 40 '$LOG' 2>/dev/null" || true
    exit 1
fi
