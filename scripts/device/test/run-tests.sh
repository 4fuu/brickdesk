#!/bin/sh
# Offline tests for scripts/device/*.sh and the release package installer.
#
# Puts a fake `adb` first in PATH, seeds a fake build tree, then runs every
# device script end to end. It also runs shellcheck when available.
#
# Usage: scripts/device/test/run-tests.sh
set -eu

SCRIPT_DIR=$(CDPATH="" cd -- "$(dirname -- "$0")" && pwd)
DEVICE_DIR=$(CDPATH="" cd -- "$SCRIPT_DIR/.." && pwd)
REPO_ROOT=$(CDPATH="" cd -- "$SCRIPT_DIR/../../.." && pwd)
cd "$REPO_ROOT"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/brickdesk-device-tests.XXXXXX")
cleanup() {
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

pass() {
    printf 'ok: %s\n' "$*"
}

die() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

scripts="$DEVICE_DIR/deploy.sh $DEVICE_DIR/run.sh $DEVICE_DIR/logs.sh $DEVICE_DIR/screenshot.sh $DEVICE_DIR/perf.sh $DEVICE_DIR/bench.sh $DEVICE_DIR/recover.sh $DEVICE_DIR/smoke.sh $DEVICE_DIR/common.sh $SCRIPT_DIR/fake-adb $SCRIPT_DIR/run-tests.sh $SCRIPT_DIR/package-tests.sh $REPO_ROOT/scripts/package-target.sh $REPO_ROOT/scripts/package/install.sh $REPO_ROOT/scripts/package/uninstall.sh $REPO_ROOT/scripts/package/boot-hook.sh $REPO_ROOT/scripts/build-target.sh $REPO_ROOT/scripts/build-sdl2.sh $REPO_ROOT/ci/check-glibc.sh"

printf '== syntax check ==\n'
for script in $scripts; do
    sh -n "$script" || die "syntax error in $script"
done
pass "sh -n"

if command -v shellcheck >/dev/null 2>&1; then
    printf '== shellcheck ==\n'
    # -x follows the common.sh source directive.
    # shellcheck disable=SC2086
    shellcheck -x $scripts || die "shellcheck failed"
    pass "shellcheck"
else
    printf 'warning: shellcheck not installed; skipping\n'
fi

mkdir -p "$TMP/bin" "$TMP/seed/build-target/sysroot/lib" "$TMP/state" "$TMP/logs"
cp "$SCRIPT_DIR/fake-adb" "$TMP/bin/adb"
chmod +x "$TMP/bin/adb"

printf '#!/bin/sh\nexit 0\n' >"$TMP/seed/build-target/display_probe"
printf '#!/bin/sh\nexit 0\n' >"$TMP/seed/build-target/brickdesk"
chmod +x "$TMP/seed/build-target/display_probe" "$TMP/seed/build-target/brickdesk"
printf 'fake SDL2\n' >"$TMP/seed/build-target/sysroot/lib/libSDL2-2.0.so.0.2600.1"
ln -s libSDL2-2.0.so.0.2600.1 "$TMP/seed/build-target/sysroot/lib/libSDL2-2.0.so.0"

export PATH="$TMP/bin:$PATH"
export FAKE_ADB_LOG="$TMP/adb.log"
export FAKE_ADB_STATE="$TMP/state"
export BUILD_DIR="$TMP/seed/build-target"
export DEVICE_ROOT=/mnt/SDCARD/brickdesk
export ALLOW_SYSTEM_DEST=0

printf '== deploy ==\n'
"$DEVICE_DIR/deploy.sh" >"$TMP/deploy.out"
grep -q 'pushing display_probe' "$TMP/deploy.out" || die "deploy did not push display_probe"
grep -q '/mnt/SDCARD/brickdesk/bin/display_probe' "$TMP/adb.log" || die "deploy destination missing"
pass "deploy"

printf '== foreground probe ==\n'
"$DEVICE_DIR/run.sh" --foreground display_probe >"$TMP/probe.txt"
grep -q 'RESULT: PASS' "$TMP/probe.txt" || die "probe output missing PASS"
pass "run --foreground"

printf '== background app ==\n'
"$DEVICE_DIR/run.sh" brickdesk >"$TMP/run.out"
grep -q 'running as pid' "$TMP/run.out" || die "run did not report a pid"
pass "run"

printf '== screenshot ==\n'
"$DEVICE_DIR/screenshot.sh" -o "$TMP/shot.png" >"$TMP/screenshot.out"
if [ -f "$TMP/shot.png" ]; then
    pass "screenshot (png)"
elif [ -f "$TMP/shot.ppm" ]; then
    pass "screenshot (ppm fallback)"
else
    die "screenshot produced no file"
fi

printf '== logs ==\n'
"$DEVICE_DIR/logs.sh" -o "$TMP/logs" >"$TMP/logs.out"
[ -f "$TMP/logs/logs/brickdesk.log" ] || die "logs.sh did not pull the app log"
pass "logs"

printf '== perf ==\n'
"$DEVICE_DIR/perf.sh" -n 2 -i 1 -o "$TMP/perf.csv" >"$TMP/perf.out"
[ -f "$TMP/perf.csv" ] || die "perf.csv missing"
[ "$(wc -l <"$TMP/perf.csv")" -eq 3 ] || die "perf.csv should have 3 lines"
grep -q 'samples=2' "$TMP/perf.out" || die "perf summary missing"
pass "perf"

printf '== recover ==\n'
"$DEVICE_DIR/recover.sh" >"$TMP/recover.out"
grep -q 'stock MainUI is running' "$TMP/recover.out" || die "recover did not report MainUI"
pass "recover"

printf '== smoke ==\n'
"$DEVICE_DIR/smoke.sh" -o "$TMP/smoke" >"$TMP/smoke.out"
grep -q 'SMOKE PASS' "$TMP/smoke.out" || die "smoke did not pass"
grep -q 'RESULT: PASS' "$TMP/smoke/probe.txt" || die "smoke probe output missing"
pass "smoke"

printf '== write-scope check ==\n'
awk '
    $1 == "push" {
        if ($3 !~ /^\/mnt\/SDCARD\/brickdesk\//) {
            printf "unexpected push destination: %s\n", $3
            bad = 1
        }
    }
    END { exit bad }
' "$TMP/adb.log" || die "a script pushed outside the SD-card app directory"
pass "only /mnt/SDCARD/brickdesk was written"

printf '== package install/uninstall ==\n'
"$SCRIPT_DIR/package-tests.sh"

printf '== all device script tests passed ==\n'
