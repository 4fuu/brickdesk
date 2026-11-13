#!/bin/sh
# Grab the visible framebuffer through display_probe --dump-fb and pull it back.
#
# Caveat: on some PowerVR configurations /dev/fb0 is not the scanout buffer, so
# the result may be black or stale. The probe prints the fb0 mode and geometry;
# compare it with what the panel shows. When brickdesk grows a `--screenshot`
# path, prefer that for app-level captures.
#
# Usage: scripts/device/screenshot.sh [-o FILE] [--keep-ppm]
set -eu

SCRIPT_DIR=$(CDPATH="" cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH="" cd -- "$SCRIPT_DIR/../.." && pwd)
# shellcheck source=scripts/device/common.sh
. "$SCRIPT_DIR/common.sh"

DEST=${DEVICE_ROOT:-/mnt/SDCARD/brickdesk}
OUT=${OUT:-$REPO_ROOT/build-device/screenshot-$(date +%Y%m%d-%H%M%S).png}
KEEP_PPM=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        -o) [ "$#" -ge 2 ] || die "-o needs a file"; OUT=$2; shift 2 ;;
        --keep-ppm) KEEP_PPM=1; shift ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

require_device
ensure_sd_dest

if ! "$ADB" shell "[ -x '$DEST/bin/display_probe' ]" >/dev/null 2>&1; then
    die "$DEST/bin/display_probe missing; run scripts/device/deploy.sh"
fi

mkdir -p "$(dirname "$OUT")"
tmp_ppm=$(mktemp "${TMPDIR:-/tmp}/brickdesk-fb.XXXXXX.ppm")
remote_ppm=$DEVICE_TMP/frame.ppm
cleanup() {
    rm -f "$tmp_ppm"
}
trap cleanup EXIT INT TERM

"$ADB" shell "mkdir -p '$DEVICE_TMP'"
info "reading /dev/fb0 through display_probe"
"$ADB" shell "cd '$DEST' && LD_LIBRARY_PATH='$DEST/lib:/usr/trimui/lib:/usr/lib:/lib' '$DEST/bin/display_probe' --dump-fb '$remote_ppm'"

info "pulling $remote_ppm"
"$ADB" pull "$remote_ppm" "$tmp_ppm" >/dev/null
"$ADB" shell "rm -f '$remote_ppm'"

case "$OUT" in
    *.png)
        converted=0
        if command -v ffmpeg >/dev/null 2>&1 && ffmpeg -loglevel error -y -i "$tmp_ppm" "$OUT" >/dev/null 2>&1; then
            converted=1
        elif command -v magick >/dev/null 2>&1 && magick "$tmp_ppm" "$OUT" >/dev/null 2>&1; then
            converted=1
        elif command -v convert >/dev/null 2>&1 && convert "$tmp_ppm" "$OUT" >/dev/null 2>&1; then
            converted=1
        fi
        if [ "$converted" -eq 0 ]; then
            rm -f "$OUT"
            OUT=${OUT%.png}.ppm
            cp "$tmp_ppm" "$OUT"
            warn "no working PPM-to-PNG converter; saved PPM instead: $OUT"
        fi
        ;;
    *)
        cp "$tmp_ppm" "$OUT"
        ;;
esac

if [ "$KEEP_PPM" = "1" ] && [ "$OUT" != "${OUT%.ppm}.ppm" ]; then
    cp "$tmp_ppm" "${OUT%.png}.ppm"
fi

info "screenshot: $OUT"
