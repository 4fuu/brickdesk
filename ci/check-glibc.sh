#!/bin/sh
# Fail when a cross-compiled binary needs a GLIBC symbol version newer than the
# device provides. The Brick Pro is treated conservatively as glibc 2.23.
#
# Usage: ci/check-glibc.sh [--max VERSION] BINARY [BINARY...]
set -eu

MAX=2.23
if [ "${1:-}" = "--max" ]; then
    if [ "$#" -lt 3 ]; then
        echo "usage: $0 [--max VERSION] BINARY [BINARY...]" >&2
        exit 2
    fi
    MAX=$2
    shift 2
fi

if [ "$#" -eq 0 ]; then
    echo "usage: $0 [--max VERSION] BINARY [BINARY...]" >&2
    exit 2
fi

if command -v aarch64-linux-gnu-readelf >/dev/null 2>&1; then
    READELF=aarch64-linux-gnu-readelf
elif [ -n "${TRIMUI_TOOLCHAIN:-}" ] && [ -x "$TRIMUI_TOOLCHAIN/bin/aarch64-linux-gnu-readelf" ]; then
    READELF=$TRIMUI_TOOLCHAIN/bin/aarch64-linux-gnu-readelf
else
    READELF=readelf
fi

status=0
for binary in "$@"; do
    if [ ! -f "$binary" ]; then
        echo "error: no such file: $binary" >&2
        status=1
        continue
    fi

    versions=$("$READELF" --version-info "$binary" 2>/dev/null |
        grep -o 'GLIBC_[0-9][0-9.]*' | sort -Vu | tr '\n' ' ')
    max=$("$READELF" --version-info "$binary" 2>/dev/null |
        grep -o 'GLIBC_[0-9][0-9.]*' | sort -Vu | tail -n 1)

    if [ -z "$max" ]; then
        echo "$binary: no GLIBC version references (static or non-glibc binary)"
        continue
    fi
    max=${max#GLIBC_}

    # `sort -V` puts the larger version last, so max <= MAX iff sorting both
    # values leaves MAX last.
    highest=$(printf '%s\n%s\n' "$max" "$MAX" | sort -V | tail -n 1)
    if [ "$highest" = "$MAX" ]; then
        echo "$binary: max GLIBC $max (limit $MAX) OK"
    else
        echo "$binary: max GLIBC $max EXCEEDS limit $MAX (needs: $versions)" >&2
        status=1
    fi

    if "$READELF" --version-info "$binary" 2>/dev/null | grep -q 'GLIBC_PRIVATE'; then
        echo "$binary: ERROR: references GLIBC_PRIVATE; it will not load on the device" >&2
        status=1
    fi

    echo "$binary: NEEDED"
    "$READELF" -d "$binary" 2>/dev/null | grep NEEDED || true
done

exit "$status"
