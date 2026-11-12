#!/bin/sh
# One-shot device smoke test:
#   deploy -> stop stock UI -> display_probe (foreground) -> pull logs -> recover
#
# Output goes to build-device/smoke-YYYYmmdd-HHMMSS/ by default. The probe output
# is kept verbatim; the summary extracts the lines that matter for display.
#
# Usage: scripts/device/smoke.sh [-o DIR]
set -eu

SCRIPT_DIR=$(CDPATH="" cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH="" cd -- "$SCRIPT_DIR/../.." && pwd)
# shellcheck source=scripts/device/common.sh
. "$SCRIPT_DIR/common.sh"

OUT=${OUT:-$REPO_ROOT/build-device/smoke-$(date +%Y%m%d-%H%M%S)}
RECOVERED=0
PROBE_RC=1

while [ "$#" -gt 0 ]; do
    case "$1" in
        -o) [ "$#" -ge 2 ] || die "-o needs a directory"; OUT=$2; shift 2 ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

mkdir -p "$OUT"

# shellcheck disable=SC2317 # called through the EXIT/INT/TERM trap
cleanup() {
    rc=$?
    trap - EXIT INT TERM
    if [ "$RECOVERED" -eq 0 ]; then
        info "restoring stock UI"
        "$SCRIPT_DIR/recover.sh" || warn "recover.sh failed; run it manually"
    fi
    if [ "$rc" -eq 0 ] && [ "$PROBE_RC" -ne 0 ]; then
        rc=$PROBE_RC
    fi
    exit "$rc"
}
trap cleanup EXIT INT TERM

info "smoke output: $OUT"

if ! "$SCRIPT_DIR/deploy.sh" >"$OUT/deploy.txt" 2>&1; then
    cat "$OUT/deploy.txt"
    die "deploy failed"
fi
cat "$OUT/deploy.txt"

info "running display_probe (this stops the stock UI)"
if "$SCRIPT_DIR/run.sh" --foreground display_probe >"$OUT/probe.txt" 2>&1; then
    PROBE_RC=0
else
    PROBE_RC=$?
fi
cat "$OUT/probe.txt"

RECOVERED=1
info "restoring stock UI"
if ! "$SCRIPT_DIR/recover.sh"; then
    RECOVERED=0
    warn "recover.sh failed; run it manually"
fi

"$SCRIPT_DIR/logs.sh" -o "$OUT/logs" >/dev/null 2>&1 || warn "log collection failed"

echo "--- key lines"
grep -E '^(fb0_res|fb0_bpp|sdl_video_driver|sdl_window_size|egl_vendor|egl_version|egl_surface_size|egl_buffer_age|egl_proc_swap_with_damage|gl_renderer|gl_version|swap_ms_p50|swap_ms_p95|RESULT|FAIL:)' \
    "$OUT/probe.txt" || true

if [ "$PROBE_RC" -eq 0 ]; then
    echo "SMOKE PASS"
else
    echo "SMOKE FAIL (display_probe exit $PROBE_RC)"
fi

exit "$PROBE_RC"
