#!/bin/sh
# Collect one diagnostic bundle from a Brick Pro over adb.
#
# Usage: scripts/device/collect.sh [-o DIR]
# Default output: build-device/diagnostics-YYYYmmdd-HHMMSS/ plus .tar.gz.
#
# Environment: ADB_TIMEOUT (default 10) bounds each adb shell call. A timed-out
# node is recorded as "# timeout after Ns" instead of blocking the bundle.
#
# The bundle is read-only on the device: it copies logs/config and dumps the
# hardware nodes used by power, battery, LED, display, audio, input, Wi-Fi,
# Bluetooth and cpufreq. It never writes sysfs or restarts services.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
# shellcheck source=scripts/device/common.sh
. "$SCRIPT_DIR/common.sh"

OUT=${OUT:-$REPO_ROOT/build-device/diagnostics-$(date +%Y%m%d-%H%M%S)}
ADB_TIMEOUT=${ADB_TIMEOUT:-10}
case "$ADB_TIMEOUT" in
    ''|*[!0-9]*) ADB_TIMEOUT=10 ;;
esac

while [ "$#" -gt 0 ]; do
    case "$1" in
        -o) [ "$#" -ge 2 ] || die "-o needs a directory"; OUT=$2; shift 2 ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

require_device
mkdir -p "$OUT"

TIMEOUT_CMD=
if command -v timeout >/dev/null 2>&1 &&
    timeout --version >/dev/null 2>&1; then
    TIMEOUT_CMD=timeout
else
    warn "GNU timeout unavailable; adb shell calls use a background watchdog"
fi

# A few vendor sysfs nodes (notably /sys/power/wakeup_count) can block forever.
# Keep one stuck attribute from preventing the rest of the diagnostic bundle.
adb_shell_timed() {
    if [ -n "$TIMEOUT_CMD" ]; then
        "$TIMEOUT_CMD" "$ADB_TIMEOUT" "$ADB" shell "$@"
        return $?
    fi
    "$ADB" shell "$@" &
    adb_pid=$!
    ( sleep "$ADB_TIMEOUT"; kill "$adb_pid" 2>/dev/null ) &
    watchdog_pid=$!
    rc=0
    wait "$adb_pid" || rc=$?
    kill "$watchdog_pid" 2>/dev/null || true
    wait "$watchdog_pid" 2>/dev/null || true
    return "$rc"
}

info "pulling logs and kernel facts"
if [ -n "$TIMEOUT_CMD" ]; then
    "$TIMEOUT_CMD" 60 "$SCRIPT_DIR/logs.sh" -o "$OUT/logs" ||
        warn "logs.sh failed or timed out; continuing"
else
    "$SCRIPT_DIR/logs.sh" -o "$OUT/logs" ||
        warn "logs.sh failed; continuing"
fi

save() {
    name=$1
    shift
    raw=$OUT/$name.raw
    tmp=$OUT/$name.tmp
    rm -f "$raw" "$tmp"
    rc=0
    adb_shell_timed "$@" >"$raw" 2>/dev/null || rc=$?
    tr -d '\r' <"$raw" >"$tmp" 2>/dev/null || true
    rm -f "$raw"
    case "$rc" in
        0) ;;
        124|143) printf '# timeout after %ss\n' "$ADB_TIMEOUT" >>"$tmp" ;;
        *) printf '# adb shell rc=%s\n' "$rc" >>"$tmp" ;;
    esac
    if [ -s "$tmp" ]; then
        mv "$tmp" "$OUT/$name"
    else
        rm -f "$tmp"
    fi
}

info "collecting identity and versions"
save identity.txt 'cat /etc/model 2>/dev/null; cat /etc/version 2>/dev/null; uname -a; cat /proc/version; cat /proc/cmdline'
save uptime.txt 'uptime; cat /proc/loadavg'
save memory.txt 'free -m 2>/dev/null || cat /proc/meminfo'
save storage.txt 'df -h; mount'
save processes.txt 'ps 2>/dev/null || ps -ef'

info "collecting power and battery nodes"
save power-supply.txt 'for d in /sys/class/power_supply/*; do echo "== $d"; for a in type present online status capacity voltage_now current_now temp time_to_empty_now time_to_full_now health charge_now charge_full charge_full_design; do [ -f "$d/$a" ] && printf "%s=" "$a" && cat "$d/$a"; done; done'
save power-state.txt 'cat /sys/power/state 2>/dev/null; cat /sys/power/mem_sleep 2>/dev/null; cat /sys/power/wakeup_count 2>/dev/null'
save i2c.txt 'ls -l /dev/i2c* 2>/dev/null'

info "collecting LED, motor and GPIO nodes"
save led-anim.txt 'for f in /sys/class/led_anim/*; do [ -f "$f" ] && printf "%s=" "${f##*/}" && cat "$f"; done'
save motor.txt 'for f in /sys/class/motor/*; do [ -f "$f" ] && printf "%s=" "${f##*/}" && cat "$f"; done; for f in /sys/class/gpio/gpio227/*; do [ -f "$f" ] && printf "%s=" "${f##*/}" && cat "$f"; done'
save leds.txt 'ls -l /sys/class/leds 2>/dev/null; for d in /sys/class/leds/*; do [ -d "$d" ] || continue; echo "== $d"; for a in brightness max_brightness trigger; do [ -f "$d/$a" ] && printf "%s=" "$a" && cat "$d/$a"; done; done'

info "collecting display and audio nodes"
save display.txt 'for f in /sys/class/disp/disp/attr/*; do [ -f "$f" ] && printf "%s=" "${f##*/}" && cat "$f"; done; cat /sys/class/backlight/*/brightness 2>/dev/null; cat /sys/class/backlight/*/max_brightness 2>/dev/null'
save audio.txt 'ls -l /dev/snd 2>/dev/null; cat /proc/asound/cards 2>/dev/null; for f in /sys/class/speaker/*; do [ -f "$f" ] && printf "%s=" "${f##*/}" && cat "$f"; done'
save audio-mixer.txt 'amixer 2>/dev/null || true; amixer -D bluealsa 2>/dev/null || true'

info "collecting input, network and frequency nodes"
save input.txt 'cat /proc/bus/input/devices; ls -l /dev/input'
save network.txt 'ls -l /sys/class/net; for d in /sys/class/net/*; do echo "== $d"; for a in address operstate carrier; do [ -f "$d/$a" ] && printf "%s=" "$a" && cat "$d/$a"; done; done; ip addr 2>/dev/null || ifconfig -a 2>/dev/null'
save wifi.txt 'wpa_cli -i wlan0 status 2>/dev/null || true; iw dev 2>/dev/null || true'
save bluetooth.txt 'hciconfig -a 2>/dev/null || true; hcitool dev 2>/dev/null || true; ps | grep -E "bluetooth|bluealsa" 2>/dev/null || true'
save cpufreq.txt 'for d in /sys/devices/system/cpu/cpufreq/policy*; do [ -d "$d" ] || continue; echo "== $d"; for a in scaling_governor scaling_available_governors scaling_cur_freq scaling_min_freq scaling_max_freq; do [ -f "$d/$a" ] && printf "%s=" "$a" && cat "$d/$a"; done; done'
save thermal.txt 'for z in /sys/class/thermal/thermal_zone*; do [ -d "$z" ] || continue; echo "== $z"; cat "$z/type" 2>/dev/null; cat "$z/temp" 2>/dev/null; done'

info "collecting brickdesk config and helper output"
if [ -n "$TIMEOUT_CMD" ]; then
    "$TIMEOUT_CMD" 30 "$ADB" pull "$DEVICE_ROOT/config" "$OUT/config" >/dev/null 2>&1 ||
        warn "cannot pull $DEVICE_ROOT/config (or timed out)"
else
    "$ADB" pull "$DEVICE_ROOT/config" "$OUT/config" >/dev/null 2>&1 ||
        warn "cannot pull $DEVICE_ROOT/config"
fi
adb_shell_timed "du -sk '$DEVICE_ROOT/state' '$DEVICE_ROOT/logs' 2>/dev/null; ls -l '$DEVICE_ROOT/state' 2>/dev/null" 2>/dev/null |
    tr -d '\r' >"$OUT/brickdesk-state.txt" || true
save bd-power.txt "for b in '$DEVICE_ROOT/bin/bd-power' /mnt/SDCARD/brickdesk/bin/bd-power; do [ -x \"\$b\" ] && \"\$b\"; done"
save bd-battery.txt "for b in '$DEVICE_ROOT/bin/bd-battery' /mnt/SDCARD/brickdesk/bin/bd-battery; do [ -x \"\$b\" ] && \"\$b\"; done"
save bd-feedback.txt "for b in '$DEVICE_ROOT/bin/bd-feedback' /mnt/SDCARD/brickdesk/bin/bd-feedback; do [ -x \"\$b\" ] && \"\$b\" probe; done"
save sysfs-tree.txt 'ls -l /sys/class/led_anim /sys/class/motor /sys/class/disp/disp/attr /sys/class/speaker 2>/dev/null'

info "writing bundle"
ARCHIVE=$OUT.tar.gz
tar -czf "$ARCHIVE" -C "$(dirname "$OUT")" "$(basename "$OUT")"
info "bundle: $ARCHIVE"
