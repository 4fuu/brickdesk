#!/bin/sh
# Sample CPU, RSS, frequency, temperature and battery for a running app.
#
# Uses /proc and sysfs directly; top/pidof are not required. The app pid comes
# from `pidof APP_NAME`, then from $DEVICE_TMP/APP_NAME.pid.
#
# Usage: scripts/device/perf.sh [-n COUNT] [-i SECONDS] [-o FILE] [--pid PID]
# Defaults: 30 samples, 1 second apart, printed as CSV.
set -eu

SCRIPT_DIR=$(CDPATH="" cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH="" cd -- "$SCRIPT_DIR/../.." && pwd)
export REPO_ROOT
# shellcheck source=scripts/device/common.sh
. "$SCRIPT_DIR/common.sh"

COUNT=30
INTERVAL=1
OUT=
PID=

while [ "$#" -gt 0 ]; do
    case "$1" in
        -n) [ "$#" -ge 2 ] || die "-n needs a count"; COUNT=$2; shift 2 ;;
        -i) [ "$#" -ge 2 ] || die "-i needs seconds"; INTERVAL=$2; shift 2 ;;
        -o) [ "$#" -ge 2 ] || die "-o needs a file"; OUT=$2; shift 2 ;;
        --pid) [ "$#" -ge 2 ] || die "--pid needs a pid"; PID=$2; shift 2 ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

case "$COUNT" in
    ''|*[!0-9]*) die "count must be a positive integer" ;;
esac
[ "$COUNT" -gt 0 ] || die "count must be greater than zero"

require_device

if [ -z "$PID" ]; then
    PID=$(app_pid)
fi
if [ -z "$PID" ]; then
    die "no $APP_NAME process; start it with scripts/device/run.sh"
fi
info "sampling pid $PID every ${INTERVAL}s, $COUNT sample(s)"

CLK_TCK=$(device_shell_capture "getconf CLK_TCK 2>/dev/null" || true)
case "$CLK_TCK" in
    ''|*[!0-9]*) CLK_TCK=$(getconf CLK_TCK 2>/dev/null || echo 100) ;;
esac
TMP=$(mktemp "${TMPDIR:-/tmp}/brickdesk-perf.XXXXXX.csv")
cleanup() {
    rm -f "$TMP"
}
trap cleanup EXIT INT TERM

printf 'timestamp,sample,cpu_pct,rss_kb,cpu_khz,temp_c,capacity_pct\n' | tee "$TMP"

raw=""
section() {
    printf '%s\n' "$raw" | awk -v tag="@@$1" '$0 == tag { on = 1; next } /^@@/ { on = 0 } on'
}

read_sample() {
    raw=$("$ADB" shell "echo @@STAT; cat /proc/$PID/stat 2>/dev/null; echo @@STATUS; cat /proc/$PID/status 2>/dev/null; echo @@UPTIME; cat /proc/uptime 2>/dev/null; echo @@FREQ; cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq 2>/dev/null; echo @@TEMP; cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null; echo @@BATT; cat /sys/class/power_supply/*/capacity 2>/dev/null" | tr -d '\r')
}

prev_ticks=
prev_uptime=
i=1
while [ "$i" -le "$COUNT" ]; do
    read_sample

    ticks=$(section STAT | head -n 1 | sed 's/.*) //' | awk '{print $12 + $13}')
    uptime_s=$(section UPTIME | head -n 1 | awk '{print $1}')
    rss_kb=$(section STATUS | awk '/^VmRSS:/ {print $2; exit}')
    cpu_khz=$(section FREQ | head -n 1 | tr -d ' ')
    temp_raw=$(section TEMP | head -n 1 | tr -d ' ')
    capacity=$(section BATT | head -n 1 | tr -d ' ')
    temp_c=$(awk -v t="$temp_raw" 'BEGIN { if (t == "" || t !~ /^[0-9]+$/) print "NA"; else printf "%.1f", t / 1000 }')

    cpu_pct=NA
    if [ -n "$prev_ticks" ] && [ -n "$ticks" ] && [ -n "$uptime_s" ]; then
        cpu_pct=$(awk -v t="$ticks" -v pt="$prev_ticks" -v u="$uptime_s" -v pu="$prev_uptime" -v hz="$CLK_TCK" '
            BEGIN {
                if (t !~ /^[0-9]+$/ || pt !~ /^[0-9]+$/ || u !~ /^[0-9.]+$/ || pu !~ /^[0-9.]+$/) { print "NA"; exit }
                dt = t - pt; du = u - pu
                if (dt < 0 || du <= 0) { print "NA"; exit }
                printf "%.1f", dt / hz / du * 100
            }')
    fi

    [ -n "$rss_kb" ] || rss_kb=NA
    [ -n "$cpu_khz" ] || cpu_khz=NA
    [ -n "$capacity" ] || capacity=NA

    printf '%s,%d,%s,%s,%s,%s,%s\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$i" "$cpu_pct" "$rss_kb" "$cpu_khz" "$temp_c" "$capacity" |
        tee -a "$TMP"

    prev_ticks=$ticks
    prev_uptime=$uptime_s
    i=$((i + 1))
    if [ "$i" -le "$COUNT" ]; then
        sleep "$INTERVAL"
    fi
done

echo "--- summary"
awk -F, '
    NR > 1 {
        n++
        if ($3 != "NA" && $3 != "") { cpu += $3; cpu_n++ }
        if ($4 != "NA" && $4 + 0 > rss) { rss = $4 + 0 }
        if ($6 != "NA" && $6 + 0 > temp) { temp = $6 + 0 }
        if ($7 != "NA" && $7 != "") { cap = $7 + 0 }
    }
    END {
        if (cpu_n > 0) printf "samples=%d cpu_avg_pct=%.1f max_rss_kb=%d max_temp_c=%.1f last_capacity_pct=%d\n", n, cpu / cpu_n, rss, temp, cap
        else printf "samples=%d cpu_avg_pct=NA max_rss_kb=%d max_temp_c=%.1f last_capacity_pct=%d\n", n, rss, temp, cap
    }
' "$TMP"

if [ -n "$OUT" ]; then
    cp "$TMP" "$OUT"
    info "CSV saved to $OUT"
fi
