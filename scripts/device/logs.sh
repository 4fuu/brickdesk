#!/bin/sh
# Pull application logs, kernel messages and basic system facts from the device.
#
# Usage: scripts/device/logs.sh [-o DIR]
# Default output: build-device/logs
set -eu

SCRIPT_DIR=$(CDPATH="" cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH="" cd -- "$SCRIPT_DIR/../.." && pwd)
# shellcheck source=scripts/device/common.sh
. "$SCRIPT_DIR/common.sh"

DEST=${DEVICE_ROOT:-/mnt/SDCARD/Apps/brickdesk}
OUT=${OUT:-$REPO_ROOT/build-device/logs}

while [ "$#" -gt 0 ]; do
    case "$1" in
        -o) [ "$#" -ge 2 ] || die "-o needs a directory"; OUT=$2; shift 2 ;;
        -h|--help) sed -n '2,6p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

require_device
mkdir -p "$OUT"

if "$ADB" shell "[ -d '$DEST/logs' ]" >/dev/null 2>&1; then
    info "pulling $DEST/logs"
    "$ADB" pull "$DEST/logs" "$OUT" >/dev/null
else
    warn "no log directory at $DEST/logs"
fi

info "collecting kernel and system information"
if "$ADB" shell "dmesg >/dev/null 2>&1" >/dev/null 2>&1; then
    "$ADB" shell dmesg 2>/dev/null | tr -d '\r' >"$OUT/dmesg.txt" || true
    [ -s "$OUT/dmesg.txt" ] || rm -f "$OUT/dmesg.txt"
else
    warn "dmesg not readable (adb root may be required)"
fi

if "$ADB" shell "command -v logread >/dev/null 2>&1" >/dev/null 2>&1; then
    "$ADB" shell logread 2>/dev/null | tr -d '\r' >"$OUT/logread.txt" || true
fi

{
    echo "## uname"
    "$ADB" shell "uname -a" 2>/dev/null || true
    echo "## /proc/version"
    "$ADB" shell "cat /proc/version" 2>/dev/null || true
    echo "## /proc/cpuinfo"
    "$ADB" shell "cat /proc/cpuinfo" 2>/dev/null || true
    echo "## mounts"
    "$ADB" shell "mount" 2>/dev/null || true
    echo "## df"
    "$ADB" shell "df -h" 2>/dev/null || true
    echo "## processes"
    "$ADB" shell "ps" 2>/dev/null || true
} | tr -d '\r' >"$OUT/system.txt"

info "recent application log"
found=0
for file in "$OUT"/logs/*.log "$OUT"/*.log; do
    [ -f "$file" ] || continue
    found=1
    echo "--- $file"
    tail -n 40 "$file"
done
if [ "$found" -eq 0 ]; then
    warn "no application log files found under $OUT"
fi

info "saved to $OUT"
