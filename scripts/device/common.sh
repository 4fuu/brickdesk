#!/bin/sh
# Shared helpers for scripts/device/*.sh. Source this file; do not execute it.
# The caller owns `set -eu` and defines SCRIPT_DIR/REPO_ROOT.
#
# ADB must be a command name or an absolute path (no extra arguments). Use
# ANDROID_SERIAL to select a device when several are attached.

ADB=${ADB:-adb}
DEVICE_ROOT=${DEVICE_ROOT:-/mnt/SDCARD/brickdesk}
DEVICE_TMP=${DEVICE_TMP:-/tmp/brickdesk}
APP_NAME=${APP_NAME:-brickdesk}
PROBE_NAME=${PROBE_NAME:-display_probe}
BUILD_DIR=${BUILD_DIR:-$REPO_ROOT/build-target}

info() {
    printf '==> %s\n' "$*"
}

warn() {
    printf 'warning: %s\n' "$*" >&2
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

require_device() {
    if ! "$ADB" get-state >/dev/null 2>&1; then
        die "no adb device (ADB=$ADB); connect the Brick Pro and retry"
    fi
}

require_file() {
    [ -f "$1" ] || die "missing local file: $1"
}

device_pidof() {
    "$ADB" shell "pidof '$1' 2>/dev/null" | tr -d '\r' | awk 'NR == 1 {print $1}'
}

device_shell_capture() {
    "$ADB" shell "$1" 2>/dev/null | tr -d '\r'
}

stop_stock_ui() {
    info "stopping stock UI (SIGSTOP runtrimui, SIGKILL MainUI)"
    "$ADB" shell "mkdir -p '$DEVICE_TMP'"
    runtrimui_pid=$(device_pidof runtrimui.sh)
    if [ -n "$runtrimui_pid" ]; then
        # SIGSTOP prevents the launcher from respawning MainUI without the
        # USB re-enumeration caused by /etc/init.d/runtrimui stop.
        "$ADB" shell "printf '%s\\n' '$runtrimui_pid' >'$DEVICE_TMP/runtrimui.pid'; kill -STOP '$runtrimui_pid' 2>/dev/null || true"
    else
        "$ADB" shell "if [ -x /etc/init.d/runtrimui ]; then /etc/init.d/runtrimui stop >/dev/null 2>&1 || true; fi"
    fi
    # MainUI ignores SIGTERM on Brick Pro; SIGKILL is required to release EGL.
    "$ADB" shell "for pid in \$(pidof MainUI 2>/dev/null); do kill -9 \$pid 2>/dev/null || true; done; sleep 1"
}

start_stock_ui() {
    info "starting stock UI"
    resumed=0
    if "$ADB" shell "[ -f '$DEVICE_TMP/runtrimui.pid' ]" >/dev/null 2>&1; then
        runtrimui_pid=$("$ADB" shell "cat '$DEVICE_TMP/runtrimui.pid' 2>/dev/null" | tr -d '\r')
        if [ -n "$runtrimui_pid" ]; then
            "$ADB" shell "kill -CONT '$runtrimui_pid' 2>/dev/null || true; rm -f '$DEVICE_TMP/runtrimui.pid'; sleep 1"
            resumed=1
        fi
    fi
    if [ "$resumed" = "0" ]; then
        "$ADB" shell "if [ -x /etc/init.d/runtrimui ]; then /etc/init.d/runtrimui start >/dev/null 2>&1 || true; fi"
    fi
    "$ADB" shell "sleep 1; if ! pidof MainUI >/dev/null 2>&1; then if [ -x /usr/trimui/bin/runtrimui.sh ]; then nohup /usr/trimui/bin/runtrimui.sh >/tmp/runtrimui.log 2>&1 & else echo 'no stock launcher found' >&2; fi; fi"
}

app_pid() {
    pid=$(device_pidof "$APP_NAME")
    if [ -z "$pid" ]; then
        pid=$(device_shell_capture "cat '$DEVICE_TMP/$APP_NAME.pid' 2>/dev/null")
    fi
    printf '%s' "$pid"
}

pid_alive() {
    [ -n "$1" ] && "$ADB" shell "kill -0 '$1' 2>/dev/null" >/dev/null 2>&1
}

local_sdl2_lib() {
    for candidate in \
        "${SDL2_LIB:-}" \
        "$BUILD_DIR/dist/lib/libSDL2-2.0.so.0" \
        "$BUILD_DIR/sysroot/lib/libSDL2-2.0.so.0" \
        "$REPO_ROOT/build-target/sysroot/lib/libSDL2-2.0.so.0" \
        "/opt/trimui/sysroot/lib/libSDL2-2.0.so.0"; do
        if [ -n "$candidate" ] && [ -e "$candidate" ]; then
            readlink -f "$candidate"
            return 0
        fi
    done
    return 1
}

ensure_sd_dest() {
    if [ "${ALLOW_SYSTEM_DEST:-0}" != "1" ]; then
        case "$DEVICE_ROOT" in
            /mnt/SDCARD/*) ;;
            *) die "DEVICE_ROOT must stay under /mnt/SDCARD (got $DEVICE_ROOT); set ALLOW_SYSTEM_DEST=1 to override" ;;
        esac
    fi
}
