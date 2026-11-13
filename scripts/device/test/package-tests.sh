#!/bin/sh
# Offline package/install/uninstall tests.
#
# Builds a fake target tree, runs scripts/package-target.sh, extracts the zip
# into a fake SD-card root, then exercises install, upgrade, boot-hook fallback
# and uninstall. No adb and no hardware are involved.
#
# Usage: scripts/device/test/package-tests.sh
set -eu

SCRIPT_DIR=$(CDPATH='' cd "$(dirname "$0")" && pwd) || exit 1
REPO_ROOT=$(CDPATH='' cd "$SCRIPT_DIR/../../.." && pwd) || exit 1
cd "$REPO_ROOT"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/brickdesk-package-tests.XXXXXX")
SD=$TMP/sd
FAKE_BUILD=$TMP/fake-build
PKG_OUT=$TMP/pkg-out
EVENTS=$TMP/events.log

cleanup() {
    for name in brickdesk-inputd brickdesk-perfd brickdesk; do
        pidfile=$SD/brickdesk/state/run/$name.pid
        if [ -f "$pidfile" ]; then
            pid=$(cat "$pidfile" 2>/dev/null || echo '')
            if [ -n "$pid" ]; then
                kill -9 "$pid" 2>/dev/null || true
            fi
        fi
    done
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

make_exit_script() {
    path=$1
    {
        printf '#!/bin/sh\n'
        printf 'exit 0\n'
    } >"$path"
    chmod +x "$path"
}

# shellcheck disable=SC2016 # the generated script expands these at runtime
make_daemon() {
    path=$1
    name=$2
    {
        printf '#!/bin/sh\n'
        printf 'printf "%%s\\n" "%s start" >> "${FAKE_EVENT_LOG:?}"\n' "$name"
        printf 'while :; do sleep 1; done\n'
    } >"$path"
    chmod +x "$path"
}

# shellcheck disable=SC2016 # the generated script expands these at runtime
make_desktop() {
    path=$1
    {
        printf '#!/bin/sh\n'
        printf 'printf "%%s\\n" "desktop start" >> "${FAKE_EVENT_LOG:?}"\n'
        printf 'case "${FAKE_DESKTOP_MODE:-ok}" in\n'
        printf '    crash) exit 1 ;;\n'
        printf '    *) exit 0 ;;\n'
        printf 'esac\n'
    } >"$path"
    chmod +x "$path"
}

printf '== fake target build ==\n'
mkdir -p "$FAKE_BUILD/dist/lib" "$FAKE_BUILD/bench" "$TMP/hello/bin"
for name in brickdesk brickdesk-inputd brickdesk-perfd bd-input brickdesk-config bd-av bd-power display_probe bench_render brickdesk-demo; do
    make_exit_script "$FAKE_BUILD/$name"
done
make_desktop "$FAKE_BUILD/brickdesk"
make_daemon "$FAKE_BUILD/brickdesk-inputd" inputd
make_daemon "$FAKE_BUILD/brickdesk-perfd" perfd
cp "$FAKE_BUILD/bench_render" "$FAKE_BUILD/bench/bench_render"
printf 'fake menu shim\n' >"$FAKE_BUILD/libbrickdesk-menu.so"
printf 'fake SDL2\n' >"$FAKE_BUILD/dist/lib/libSDL2-2.0.so.0"
make_exit_script "$TMP/hello/bin/hello"
printf '{"schema":1,"id":"com.brickdesk.hello"}\n' >"$TMP/hello/plugin.json"

printf '== package ==\n'
BRICKDESK_HELLO_BIN=$TMP/hello/bin/hello \
BRICKDESK_HELLO_MANIFEST=$TMP/hello/plugin.json \
    "$REPO_ROOT/scripts/package-target.sh" \
    --build-dir "$FAKE_BUILD" --out-dir "$PKG_OUT" --version 0.1.0 >"$TMP/package.log"
ZIP=$PKG_OUT/brickdesk-0.1.0-aarch64.zip
[ -f "$ZIP" ] || die "package zip missing"
[ -f "$PKG_OUT/SHA256SUMS" ] || die "outer SHA256SUMS missing"
grep -q 'brickdesk-0.1.0-aarch64.zip' "$PKG_OUT/SHA256SUMS" || die "SHA256SUMS does not name the zip"
python3 - "$ZIP" "$TMP/extracted" <<'PY'
import sys
import zipfile

with zipfile.ZipFile(sys.argv[1]) as zf:
    names = zf.namelist()
    required = [
        "brickdesk-0.1.0-aarch64/install.sh",
        "brickdesk-0.1.0-aarch64/uninstall.sh",
        "brickdesk-0.1.0-aarch64/payload/bin/brickdesk",
        "brickdesk-0.1.0-aarch64/payload/bin/bd-power",
        "brickdesk-0.1.0-aarch64/payload/bin/bench_render",
        "brickdesk-0.1.0-aarch64/docs/device-power.md",
        "brickdesk-0.1.0-aarch64/payload/lib/libSDL2-2.0.so.0",
        "brickdesk-0.1.0-aarch64/payload/assets/fonts/NotoSansSC-500-subset.ttf",
        "brickdesk-0.1.0-aarch64/payload/plugins/hello/plugin.json",
        "brickdesk-0.1.0-aarch64/payload/apps/brickdesk-demo/app.json",
    ]
    missing = [name for name in required if name not in names]
    if missing:
        raise SystemExit("zip is missing: %s" % ", ".join(missing))
    zf.extractall(sys.argv[2])
PY
PKG=$TMP/extracted/brickdesk-0.1.0-aarch64
[ -d "$PKG/payload" ] || die "package did not extract"
( cd "$PKG" && sha256sum -c SHA256SUMS >/dev/null ) || die "package SHA256SUMS does not verify"
pass "package zip layout and checksums"

printf '== package with a relative output directory ==\n'
( cd "$TMP" && \
  BRICKDESK_HELLO_BIN=$TMP/hello/bin/hello \
  BRICKDESK_HELLO_MANIFEST=$TMP/hello/plugin.json \
    "$REPO_ROOT/scripts/package-target.sh" \
    --build-dir "$FAKE_BUILD" --out-dir relative-out --version 0.1.0 >/dev/null )
[ -f "$TMP/relative-out/brickdesk-0.1.0-aarch64.zip" ] || die "relative out-dir did not produce a zip"
[ -f "$TMP/relative-out/SHA256SUMS" ] || die "relative out-dir did not produce SHA256SUMS"
pass "relative --out-dir"

printf '== first install ==\n'
mkdir -p "$SD/System/starts"
printf '#!/bin/sh\n# stock hook\nexit 0\n' >"$SD/System/starts/zz_brickdesk.sh"
cp "$SD/System/starts/zz_brickdesk.sh" "$TMP/stock-hook"
BRICKDESK_SD_ROOT=$SD BRICKDESK_SKIP_DEVICE_CHECK=1 sh "$PKG/install.sh" >"$TMP/install1.log"
[ -x "$SD/brickdesk/bin/brickdesk" ] || die "desktop was not installed"
[ -f "$SD/brickdesk/bin/bench_render" ] || die "bench_render was not installed"
[ -x "$SD/brickdesk/bin/bd-power" ] || die "bd-power was not installed"
[ -f "$SD/brickdesk/assets/fonts/NotoSansSC-500-subset.ttf" ] || die "fonts were not installed"
[ -f "$SD/brickdesk/plugins/hello/bin/hello" ] || die "hello plugin was not installed"
[ -f "$SD/brickdesk-uninstall.sh" ] || die "uninstaller copy was not installed"
grep -q 'brickdesk-boot-hook' "$SD/System/starts/zz_brickdesk.sh" || die "boot hook missing marker"
grep -q 'stock hook' "$SD/brickdesk/state/stock-hook.bak" || die "stock hook was not backed up"
[ ! -f "$SD/System/starts/zz_brickdesk.sh.brickdesk-orig" ] || die "backup was left in System/starts"
grep -q '/mnt/SDCARD/brickdesk/apps/brickdesk-demo/brickdesk-demo' \
    "$SD/brickdesk/apps/brickdesk-demo/app.json" || die "demo manifest path is wrong"
[ "$(cat "$SD/brickdesk/VERSION")" = "0.1.0" ] || die "installed VERSION is wrong"
pass "install and stock-hook backup"

printf '== upgrade keeps user data ==\n'
printf '[ui]\ntheme = light\n' >"$SD/brickdesk/config/local.ini"
mkdir -p "$SD/brickdesk/state/demo"
printf 'keep me\n' >"$SD/brickdesk/state/demo/state.txt"
printf '0.1.1\n' >"$PKG/VERSION"
printf '0.1.1\n' >"$PKG/payload/VERSION"
BRICKDESK_SD_ROOT=$SD BRICKDESK_SKIP_DEVICE_CHECK=1 sh "$PKG/install.sh" >"$TMP/install2.log"
grep -q 'theme = light' "$SD/brickdesk/config/local.ini" || die "local.ini was not preserved"
[ "$(cat "$SD/brickdesk/state/demo/state.txt")" = "keep me" ] || die "app state was not preserved"
[ "$(cat "$SD/brickdesk/VERSION")" = "0.1.1" ] || die "upgrade did not replace VERSION"
grep -q 'stock hook' "$SD/brickdesk/state/stock-hook.bak" || die "upgrade overwrote the stock hook backup"
pass "upgrade keeps local.ini and state"

printf '== healthy boot hook ==\n'
: >"$EVENTS"
FAKE_EVENT_LOG=$EVENTS FAKE_DESKTOP_MODE=ok \
BRICKDESK_SD_ROOT=$SD BRICKDESK_DAEMON_WAIT=1 BRICKDESK_STOP_WAIT=0 \
BRICKDESK_MIN_UPTIME=999 \
    sh "$SD/System/starts/zz_brickdesk.sh" >"$TMP/hook-ok.log"
[ "$(cat "$SD/brickdesk/state/boot/attempts")" = "0" ] || die "healthy run did not reset attempts"
grep -q 'inputd start' "$EVENTS" || die "inputd did not start"
grep -q 'perfd start' "$EVENTS" || die "perfd did not start"
grep -q 'desktop start' "$EVENTS" || die "desktop did not start"
awk '
    /starting brickdesk-inputd/ && !i { i = NR }
    /starting brickdesk-perfd/ && !p { p = NR }
    /starting desktop/ && !d { d = NR }
    END { exit !(i && p && d && i < p && p < d) }
' "$SD/brickdesk/logs/boot.log" || die "inputd/perfd did not start before the desktop"
pass "inputd/perfd start before the desktop"

printf '== crash-loop fallback ==\n'
: >"$EVENTS"
printf '0\n' >"$SD/brickdesk/state/boot/attempts"
FAKE_EVENT_LOG=$EVENTS FAKE_DESKTOP_MODE=crash \
BRICKDESK_SD_ROOT=$SD BRICKDESK_DAEMON_WAIT=0 BRICKDESK_STOP_WAIT=0 \
BRICKDESK_RETRY_DELAY=0 BRICKDESK_MIN_UPTIME=999 \
    sh "$SD/System/starts/zz_brickdesk.sh" >"$TMP/hook-crash.log"
[ "$(cat "$SD/brickdesk/state/boot/attempts")" = "3" ] || die "crash counter is not 3"
[ "$(grep -c 'desktop start' "$EVENTS")" -eq 3 ] || die "desktop did not run exactly three times"
pass "three crashes fall back to the stock UI"

printf '== fallback persists across boots ==\n'
: >"$EVENTS"
FAKE_EVENT_LOG=$EVENTS FAKE_DESKTOP_MODE=ok \
BRICKDESK_SD_ROOT=$SD BRICKDESK_DAEMON_WAIT=0 BRICKDESK_STOP_WAIT=0 \
    sh "$SD/System/starts/zz_brickdesk.sh" >"$TMP/hook-skip.log"
[ ! -s "$EVENTS" ] || die "desktop started while the fallback counter was active"
grep -q 'fallback active' "$SD/brickdesk/logs/boot.log" || die "fallback skip was not logged"
pass "fallback persists until reset"

printf '== reset fallback ==\n'
BRICKDESK_SD_ROOT=$SD BRICKDESK_SKIP_DEVICE_CHECK=1 sh "$PKG/install.sh" --reset-fallback >"$TMP/reset.log"
[ ! -f "$SD/brickdesk/state/boot/attempts" ] || die "reset did not clear the crash counter"
pass "reset fallback"

printf '== boot-skip flag ==\n'
: >"$EVENTS"
: >"$SD/brickdesk/config/boot-skip"
FAKE_EVENT_LOG=$EVENTS FAKE_DESKTOP_MODE=ok \
BRICKDESK_SD_ROOT=$SD BRICKDESK_DAEMON_WAIT=0 BRICKDESK_STOP_WAIT=0 \
    sh "$SD/System/starts/zz_brickdesk.sh" >"$TMP/hook-skip-flag.log"
[ ! -s "$EVENTS" ] || die "desktop started while boot-skip was present"
grep -q 'skip requested' "$SD/brickdesk/logs/boot.log" || die "boot-skip was not logged"
rm -f "$SD/brickdesk/config/boot-skip"
pass "config/boot-skip skips brickdesk"

printf '== uninstall restores the stock hook ==\n'
BRICKDESK_SD_ROOT=$SD BRICKDESK_SKIP_DEVICE_CHECK=1 \
    sh "$SD/brickdesk-uninstall.sh" --no-restart >"$TMP/uninstall.log"
cmp -s "$TMP/stock-hook" "$SD/System/starts/zz_brickdesk.sh" || die "stock hook was not restored"
[ ! -f "$SD/brickdesk/state/stock-hook.bak" ] || die "hook backup was not removed"
[ ! -d "$SD/brickdesk/bin" ] || die "program files were not removed"
[ ! -f "$SD/brickdesk/VERSION" ] || die "VERSION was not removed"
grep -q 'theme = light' "$SD/brickdesk/config/local.ini" || die "uninstall removed local.ini"
[ "$(cat "$SD/brickdesk/state/demo/state.txt")" = "keep me" ] || die "uninstall removed app state"
[ ! -f "$SD/brickdesk-uninstall.sh" ] || die "uninstaller copy was not removed"
pass "uninstall restores the original hook and keeps user data"

printf '== purge ==\n'
BRICKDESK_SD_ROOT=$SD BRICKDESK_SKIP_DEVICE_CHECK=1 sh "$PKG/install.sh" >"$TMP/install3.log"
printf '[ui]\ntheme = dark\n' >"$SD/brickdesk/config/local.ini"
BRICKDESK_SD_ROOT=$SD BRICKDESK_SKIP_DEVICE_CHECK=1 \
    sh "$SD/brickdesk-uninstall.sh" --purge --no-restart >"$TMP/purge.log"
[ ! -d "$SD/brickdesk" ] || die "purge left the install tree"
pass "purge removes user data"

printf '== all package tests passed ==\n'
