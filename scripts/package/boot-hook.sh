#!/bin/sh
# brickdesk boot hook. This file is installed as
# /mnt/SDCARD/brickdesk/boot-hook.sh and is started by the generated
# /mnt/SDCARD/System/starts/zz_brickdesk.sh wrapper.
#
# It starts brickdesk-inputd and brickdesk-perfd, then runs the desktop in the
# foreground. Returning from this script lets the stock launcher continue to
# MainUI. After three failed desktop starts the hook leaves the stock UI in
# control and keeps a counter so later boots skip brickdesk until the counter is
# reset with install.sh --reset-fallback.
#
# POSIX sh only; busybox ash must be able to run it.

SD_ROOT=${BRICKDESK_SD_ROOT:-/mnt/SDCARD}
DEST=${BRICKDESK_ROOT:-$SD_ROOT/brickdesk}
PATH="/usr/sbin:/usr/bin:/sbin:/bin:/usr/trimui/bin${PATH:+:$PATH}"
export PATH
LOG_DIR=$DEST/logs
LOG=$LOG_DIR/boot.log
ATTEMPTS_FILE=$DEST/state/boot/attempts
RUN_DIR=$DEST/state/run
MAX_CRASHES=3
RETRY_DELAY=${BRICKDESK_RETRY_DELAY:-2}
MIN_UPTIME=${BRICKDESK_MIN_UPTIME:-20}
DAEMON_WAIT=${BRICKDESK_DAEMON_WAIT:-1}
STOP_WAIT=${BRICKDESK_STOP_WAIT:-1}
DESKTOP=${BRICKDESK_DESKTOP:-$DEST/bin/brickdesk}

case "$RETRY_DELAY" in
    ''|*[!0-9]*) RETRY_DELAY=2 ;;
esac
case "$MIN_UPTIME" in
    ''|*[!0-9]*) MIN_UPTIME=20 ;;
esac
case "$DAEMON_WAIT" in
    ''|*[!0-9]*) DAEMON_WAIT=1 ;;
esac
case "$STOP_WAIT" in
    ''|*[!0-9]*) STOP_WAIT=1 ;;
esac

if ! mkdir -p "$LOG_DIR" "$DEST/state/boot" "$RUN_DIR" 2>/dev/null; then
    # Without a writable install tree the only safe action is to let the stock
    # launcher continue.
    exit 0
fi

rotate_log() {
    if [ -f "$LOG" ]; then
        size=$(wc -c <"$LOG" 2>/dev/null | tr -d ' ')
        case "$size" in
            ''|*[!0-9]*) size=0 ;;
        esac
        if [ "$size" -gt 262144 ]; then
            mv -f "$LOG" "$LOG.1" 2>/dev/null || true
        fi
    fi
}

log() {
    printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo unknown)" "$*" >>"$LOG"
}

read_attempts() {
    attempts=0
    if [ -f "$ATTEMPTS_FILE" ]; then
        value=$(cat "$ATTEMPTS_FILE" 2>/dev/null || echo 0)
        case "$value" in
            ''|*[!0-9]*) value=0 ;;
        esac
        attempts=$value
    fi
}

write_attempts() {
    printf '%s\n' "$1" >"$ATTEMPTS_FILE" 2>/dev/null || true
}

pid_matches() {
    pid=$1
    name=$2
    [ -n "$pid" ] && [ -r "/proc/$pid/cmdline" ] || return 1
    tr '\000' ' ' <"/proc/$pid/cmdline" 2>/dev/null | grep -q "$name"
}

stop_daemon() {
    name=$1
    pidfile=$RUN_DIR/$name.pid
    [ -f "$pidfile" ] || return 0
    pid=$(cat "$pidfile" 2>/dev/null || echo '')
    if pid_matches "$pid" "$name"; then
        log "stopping $name (pid $pid)"
        kill "$pid" 2>/dev/null || true
        sleep "$STOP_WAIT"
        if pid_matches "$pid" "$name"; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    fi
    rm -f "$pidfile"
}

stop_all_daemons() {
    stop_daemon brickdesk-perfd
    stop_daemon brickdesk-inputd
}

start_daemon() {
    name=$1
    bin=$DEST/bin/$name
    pidfile=$RUN_DIR/$name.pid
    if [ -f "$pidfile" ]; then
        pid=$(cat "$pidfile" 2>/dev/null || echo '')
        if pid_matches "$pid" "$name"; then
            log "$name already running (pid $pid)"
            return 0
        fi
        rm -f "$pidfile"
    fi
    if [ ! -x "$bin" ]; then
        log "warning: $name is missing or not executable: $bin"
        return 1
    fi
    log "starting $name"
    "$bin" >>"$LOG_DIR/$name.log" 2>&1 &
    pid=$!
    printf '%s\n' "$pid" >"$pidfile"
    sleep "$DAEMON_WAIT"
    if pid_matches "$pid" "$name"; then
        log "$name running (pid $pid)"
        return 0
    fi
    log "warning: $name exited during startup"
    rm -f "$pidfile"
    return 1
}

rotate_log
log "boot hook start"

if [ "${BRICKDESK_BOOT_SKIP:-0}" = "1" ] || [ -f "$DEST/config/boot-skip" ]; then
    log "skip requested; leaving the stock UI in control"
    exit 0
fi

read_attempts
if [ "$attempts" -ge "$MAX_CRASHES" ]; then
    log "fallback active after $attempts failed start(s); leaving the stock UI in control"
    exit 0
fi

if [ ! -x "$DESKTOP" ]; then
    log "error: desktop is missing or not executable: $DESKTOP"
    write_attempts "$MAX_CRASHES"
    exit 0
fi

# If the stock UI is already running, release the display before brickdesk
# starts. The stock launcher starts MainUI again when this hook returns.
if command -v pidof >/dev/null 2>&1; then
    if pidof MainUI >/dev/null 2>&1; then
        log "stopping stock MainUI before starting brickdesk"
        pidof MainUI 2>/dev/null | tr ' ' '\n' | while IFS= read -r pid; do
            [ -n "$pid" ] || continue
            kill "$pid" 2>/dev/null || true
        done
        sleep 1
    fi
elif command -v killall >/dev/null 2>&1; then
    log "stopping stock MainUI before starting brickdesk (if running)"
    killall MainUI >/dev/null 2>&1 || true
    sleep 1
fi

export LD_LIBRARY_PATH="$DEST/lib:/usr/trimui/lib:/usr/lib:/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export BRICKDESK_ASSET_DIR="$DEST/assets"
export HOME="$DEST"
export PATH="$DEST/bin:/usr/trimui/bin:/usr/bin:/bin:/sbin:/usr/sbin"

if ! cd "$DEST" 2>/dev/null; then
    log "error: cannot enter $DEST"
    exit 0
fi

while :; do
    start_daemon brickdesk-inputd || true
    start_daemon brickdesk-perfd || true
    log "starting desktop (attempt $((attempts + 1)))"
    start_epoch=$(date +%s 2>/dev/null || echo 0)
    "$DESKTOP" >>"$LOG_DIR/brickdesk.log" 2>&1
    rc=$?
    end_epoch=$(date +%s 2>/dev/null || echo 0)
    uptime=$((end_epoch - start_epoch))
    [ "$uptime" -ge 0 ] || uptime=0
    stop_all_daemons

    if [ -f /tmp/poweroff ] || [ -f /tmp/reboot ]; then
        log "power/reboot requested; not retrying brickdesk"
        break
    fi

    if [ "$rc" -eq 0 ] || [ "$uptime" -ge "$MIN_UPTIME" ]; then
        log "desktop exited rc=$rc after ${uptime}s; resetting the crash counter"
        write_attempts 0
        break
    fi

    attempts=$((attempts + 1))
    write_attempts "$attempts"
    log "desktop crashed rc=$rc after ${uptime}s (attempt $attempts/$MAX_CRASHES)"
    if [ "$attempts" -ge "$MAX_CRASHES" ]; then
        log "crash limit reached; falling back to the stock UI"
        break
    fi
    sleep "$RETRY_DELAY"
done

log "boot hook end; the stock launcher may continue"
exit 0
