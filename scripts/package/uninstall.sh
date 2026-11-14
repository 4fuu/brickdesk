#!/bin/sh
# brickdesk device uninstaller.
#
# Default behaviour removes the program files, removes the boot hook and
# restores a previously backed-up hook from
# /mnt/SDCARD/brickdesk/state/stock-hook.bak. User data stays in place:
#   /mnt/SDCARD/brickdesk/config/local.ini
#   /mnt/SDCARD/brickdesk/config/plugins/
#   /mnt/SDCARD/brickdesk/state/
#   /mnt/SDCARD/brickdesk/logs/
#
# With --purge the whole install tree is removed, including user data.
#
# POSIX sh only; busybox ash must be able to run it.

set -eu

SD_ROOT=${BRICKDESK_SD_ROOT:-/mnt/SDCARD}
DEST=${BRICKDESK_ROOT:-$SD_ROOT/brickdesk}
HOOK_DIR=$SD_ROOT/System/starts
HOOK=$HOOK_DIR/zz_brickdesk.sh
HOOK_BACKUP=$DEST/state/stock-hook.bak
UNINSTALLER=$SD_ROOT/brickdesk-uninstall.sh
PURGE=0
RESTART=1

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

usage() {
    cat <<'EOF'
usage: uninstall.sh [--purge] [--no-restart] [--help]

  --purge       remove the whole install tree, including local.ini and state/
  --no-restart  do not start the stock UI after uninstalling
  --help        show this help

Environment:
  BRICKDESK_SD_ROOT              SD-card root (default /mnt/SDCARD)
  BRICKDESK_ROOT                 install root (default $BRICKDESK_SD_ROOT/brickdesk)
  BRICKDESK_SKIP_DEVICE_CHECK=1  skip the mount/running checks (tests)
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --purge) PURGE=1; shift ;;
        --no-restart) RESTART=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

case "$DEST" in
    "$SD_ROOT"/*) ;;
    *) die "BRICKDESK_ROOT must stay below $SD_ROOT (got $DEST)" ;;
esac
case "$DEST" in
    *..*) die "BRICKDESK_ROOT must not contain '..' (got $DEST)" ;;
esac
[ "$DEST" != "$SD_ROOT" ] || die "refusing to uninstall the SD-card root"

if [ "$SD_ROOT" = "/mnt/SDCARD" ] && [ "${BRICKDESK_SKIP_DEVICE_CHECK:-0}" != "1" ]; then
    if ! grep -q ' /mnt/SDCARD ' /proc/mounts 2>/dev/null; then
        die "/mnt/SDCARD is not mounted; insert the SD card and retry"
    fi
fi

if [ "$PURGE" = "1" ]; then
    warn "--purge removes config/local.ini, app state and logs under $DEST"
fi

pid_matches() {
    pid=$1
    name=$2
    [ -n "$pid" ] && [ -r "/proc/$pid/cmdline" ] || return 1
    tr '\000' ' ' <"/proc/$pid/cmdline" 2>/dev/null | grep -q "$name"
}

stop_pid() {
    pid=$1
    name=$2
    pid_matches "$pid" "$name" || return 0
    # brickdesk and the stock MainUI can ignore SIGTERM while holding EGL.
    # Uninstall is destructive; use SIGKILL and wait for the process to leave.
    kill -9 "$pid" 2>/dev/null || true
    tries=0
    while [ "$tries" -lt 3 ] && pid_matches "$pid" "$name"; do
        sleep 1
        tries=$((tries + 1))
    done
    if pid_matches "$pid" "$name"; then
        warn "could not stop $name (pid $pid)"
        return 1
    fi
}

info "stopping brickdesk processes"
for name in brickdesk brickdesk-perfd brickdesk-inputd; do
    pidfile=$DEST/state/run/$name.pid
    if [ -f "$pidfile" ]; then
        pid=$(cat "$pidfile" 2>/dev/null || echo '')
        stop_pid "$pid" "$name"
        rm -f "$pidfile"
    fi
done

if command -v pidof >/dev/null 2>&1; then
    for name in brickdesk brickdesk-perfd brickdesk-inputd; do
        pidof "$name" 2>/dev/null | tr ' ' '\n' | while IFS= read -r pid; do
            [ -n "$pid" ] || continue
            kill -9 "$pid" 2>/dev/null || true
        done
    done
elif command -v killall >/dev/null 2>&1; then
    for name in brickdesk brickdesk-perfd brickdesk-inputd; do
        killall -9 "$name" >/dev/null 2>&1 || true
    done
fi
sleep 1

if [ -f "$HOOK" ]; then
    if grep -q 'brickdesk-boot-hook' "$HOOK"; then
        rm -f "$HOOK"
        info "removed boot hook: $HOOK"
    else
        warn "$HOOK is not managed by brickdesk; leaving it in place"
    fi
fi
if [ -f "$HOOK_BACKUP" ]; then
    if [ ! -f "$HOOK" ] || grep -q 'brickdesk-boot-hook' "$HOOK"; then
        mv "$HOOK_BACKUP" "$HOOK"
        info "restored the original boot hook from $HOOK_BACKUP"
    else
        warn "keeping $HOOK_BACKUP because $HOOK belongs to another program"
    fi
fi
rmdir "$HOOK_DIR" 2>/dev/null || true

if [ "$PURGE" = "1" ]; then
    info "purging $DEST"
    rm -rf "${DEST:?}"
else
    info "removing program files from $DEST (keeping local.ini and state/)"
    if [ -f "$DEST/.brickdesk-manifest" ]; then
        while IFS= read -r rel; do
            case "$rel" in
                ''|/*|*..*) continue ;;
            esac
            rm -f "$DEST/$rel"
        done <"$DEST/.brickdesk-manifest"
    else
        # Older installs without a manifest: remove the known program trees.
        rm -rf "${DEST:?}/bin" "${DEST:?}/lib" "${DEST:?}/assets" "${DEST:?}/plugins" "${DEST:?}/apps" "${DEST:?}/share"
        rm -f "$DEST/config/brickdesk.ini" "$DEST/config/perf.ini" \
            "$DEST/config/hotkeys.ini" "$DEST/config/input.ini"
    fi
    rm -f "$DEST/.brickdesk-manifest" "$DEST/.brickdesk-version" "$DEST/VERSION" "$DEST/boot-hook.sh"
    rm -rf "$DEST/state/boot" "$DEST/state/run"
    for dir in "$DEST/apps" "$DEST/plugins" "$DEST/assets" "$DEST/lib" "$DEST/bin" \
        "$DEST/share" "$DEST/config" "$DEST/state" "$DEST/logs" "$DEST"; do
        rmdir "$dir" 2>/dev/null || true
    done
fi

rm -f "$UNINSTALLER" 2>/dev/null || true
sync 2>/dev/null || true

if [ "$RESTART" = "1" ] && [ "$SD_ROOT" = "/mnt/SDCARD" ] && [ "${BRICKDESK_SKIP_DEVICE_CHECK:-0}" != "1" ]; then
    info "restoring the stock UI"
    if command -v pidof >/dev/null 2>&1 && pidof MainUI >/dev/null 2>&1; then
        info "stock MainUI is already running"
    else
        if [ -x /etc/init.d/runtrimui ]; then
            /etc/init.d/runtrimui start >/dev/null 2>&1 || true
        fi
        if command -v pidof >/dev/null 2>&1 && ! pidof MainUI >/dev/null 2>&1; then
            if [ -x /usr/trimui/bin/runtrimui.sh ] && command -v nohup >/dev/null 2>&1; then
                if [ -d "$DEST/logs" ]; then
                    runtrimui_log=$DEST/logs/runtrimui.log
                else
                    runtrimui_log=/dev/null
                fi
                nohup /usr/trimui/bin/runtrimui.sh >"$runtrimui_log" 2>&1 &
            fi
        fi
        sleep 1
        if command -v pidof >/dev/null 2>&1 && pidof MainUI >/dev/null 2>&1; then
            info "stock MainUI is running"
        else
            warn "MainUI is not running yet; reboot the device to return to the stock UI"
        fi
    fi
fi

info "brickdesk uninstalled"
if [ "$PURGE" != "1" ] && [ -f "$DEST/config/local.ini" ]; then
    printf '  kept user config: %s\n' "$DEST/config/local.ini"
fi
