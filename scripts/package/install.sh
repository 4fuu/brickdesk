#!/bin/sh
# brickdesk device installer.
#
# Run this from the extracted release package on the device:
#   sh /mnt/SDCARD/brickdesk-<version>-aarch64/install.sh
#
# The installer writes only below /mnt/SDCARD:
#   /mnt/SDCARD/brickdesk/                 program, assets, config, plugins, apps
#   /mnt/SDCARD/System/starts/zz_brickdesk.sh  boot hook (last in the starts list)
#   /mnt/SDCARD/brickdesk-uninstall.sh     copy of the uninstaller
#
# Existing config/local.ini, state/ and user apps/plugins are preserved. A
# pre-existing System/starts/zz_brickdesk.sh is backed up as
# brickdesk/state/stock-hook.bak (outside the starts directory, so the stock
# launcher cannot execute the backup) and restored by uninstall.sh.
#
# POSIX sh only; busybox ash must be able to run it.

set -eu

SCRIPT_DIR=$(CDPATH='' cd "$(dirname "$0")" && pwd) || exit 1
SD_ROOT=${BRICKDESK_SD_ROOT:-/mnt/SDCARD}
DEST=${BRICKDESK_ROOT:-$SD_ROOT/brickdesk}
HOOK_DIR=$SD_ROOT/System/starts
HOOK=$HOOK_DIR/zz_brickdesk.sh
HOOK_BACKUP=$DEST/state/stock-hook.bak
UNINSTALLER=$SD_ROOT/brickdesk-uninstall.sh
PAYLOAD=$SCRIPT_DIR/payload
INSTALL_HOOK=1
RESET_FALLBACK=0

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
usage: install.sh [--no-hook] [--reset-fallback] [--help]

  --no-hook         copy the files but do not install the boot hook
  --reset-fallback  clear the crash counter and boot-skip flag, then exit
  --help            show this help

Environment:
  BRICKDESK_SD_ROOT              SD-card root (default /mnt/SDCARD)
  BRICKDESK_ROOT                 install root (default $BRICKDESK_SD_ROOT/brickdesk)
  BRICKDESK_SKIP_DEVICE_CHECK=1  skip the mount/architecture/running checks (tests)
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --no-hook) INSTALL_HOOK=0; shift ;;
        --reset-fallback) RESET_FALLBACK=1; shift ;;
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

if [ "$RESET_FALLBACK" = "1" ]; then
    rm -f "$DEST/state/boot/attempts" "$DEST/state/boot/fallback" "$DEST/config/boot-skip"
    info "cleared the brickdesk crash counter and boot-skip flag"
    exit 0
fi

[ -d "$PAYLOAD" ] || die "missing payload directory: $PAYLOAD"
[ -f "$SCRIPT_DIR/VERSION" ] || die "missing VERSION file in $SCRIPT_DIR"
[ -f "$SCRIPT_DIR/uninstall.sh" ] || die "missing uninstall.sh in $SCRIPT_DIR"

VERSION=$(cat "$SCRIPT_DIR/VERSION" 2>/dev/null || echo unknown)
GIT_REV=$(cat "$SCRIPT_DIR/GIT_REV" 2>/dev/null || echo unknown)

if [ "$SD_ROOT" = "/mnt/SDCARD" ] && [ "${BRICKDESK_SKIP_DEVICE_CHECK:-0}" != "1" ]; then
    if ! grep -q ' /mnt/SDCARD ' /proc/mounts 2>/dev/null; then
        die "/mnt/SDCARD is not mounted; insert the SD card and retry"
    fi
    arch=$(uname -m 2>/dev/null || echo unknown)
    case "$arch" in
        aarch64|arm64) ;;
        *) die "this package is for aarch64; uname -m reports $arch" ;;
    esac
    if command -v pidof >/dev/null 2>&1; then
        for name in brickdesk brickdesk-inputd brickdesk-perfd; do
            if pidof "$name" >/dev/null 2>&1; then
                die "$name is running; close brickdesk and retry (or reboot into the stock UI)"
            fi
        done
    fi
fi

# Refuse to replace a tree that still has live daemon pid files. The checks
# above only run on the real device; this also protects a fake-root test tree.
for name in brickdesk brickdesk-inputd brickdesk-perfd; do
    pidfile=$DEST/state/run/$name.pid
    if [ -f "$pidfile" ]; then
        pid=$(cat "$pidfile" 2>/dev/null || echo '')
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            die "$name is still running (pid $pid); stop it and retry"
        fi
    fi
done

umask 022
mkdir -p "$SD_ROOT" || die "cannot create $SD_ROOT"

NEW=$DEST.new.$$
OLD=$DEST.old.$$
rm -rf "$NEW" "$OLD" 2>/dev/null || true
mkdir -p "$NEW" || die "cannot create staging directory $NEW"

# Keep user-owned trees across an upgrade. Package files are copied afterwards
# so an updated hello plugin or demo app replaces the old one.
for keep in apps plugins themes screenshots; do
    if [ -d "$DEST/$keep" ]; then
        mkdir -p "$NEW/$keep"
        cp -R "$DEST/$keep"/. "$NEW/$keep"/ 2>/dev/null || warn "could not preserve $keep/"
    fi
done

info "installing brickdesk $VERSION into $DEST"
cp -R "$PAYLOAD"/. "$NEW"/ || {
    rm -rf "$NEW"
    die "cannot copy the payload"
}

# User data has the highest priority and is never shipped in the payload.
if [ -f "$DEST/config/local.ini" ]; then
    mkdir -p "$NEW/config"
    cp "$DEST/config/local.ini" "$NEW/config/local.ini" || warn "could not preserve config/local.ini"
fi
if [ -d "$DEST/config/plugins" ]; then
    mkdir -p "$NEW/config"
    cp -R "$DEST/config/plugins" "$NEW/config/plugins" 2>/dev/null || warn "could not preserve config/plugins/"
fi
if [ -d "$DEST/state" ]; then
    mkdir -p "$NEW/state"
    cp -R "$DEST/state"/. "$NEW/state"/ 2>/dev/null || warn "could not preserve state/"
fi
if [ -d "$DEST/logs" ]; then
    mkdir -p "$NEW/logs"
    cp -R "$DEST/logs"/. "$NEW/logs"/ 2>/dev/null || warn "could not preserve logs/"
fi

# A fresh install is a fresh attempt: do not inherit a previous crash-loop
# fallback or stale pid files.
rm -rf "$NEW/state/boot" "$NEW/state/run"
rm -f "$NEW/state/zz_brickdesk.sh.tmp."* 2>/dev/null || true
mkdir -p "$NEW/state/boot" "$NEW/state/run" "$NEW/logs"

# Keep the original stock hook outside System/starts. Anything inside the
# starts directory may be executed by the stock launcher, including backups.
if [ "$INSTALL_HOOK" = "1" ] && [ -f "$HOOK" ] && ! grep -q 'brickdesk-boot-hook' "$HOOK"; then
    if [ ! -f "$NEW/state/stock-hook.bak" ]; then
        cp "$HOOK" "$NEW/state/stock-hook.bak" || {
            rm -rf "$NEW"
            die "cannot back up the existing boot hook"
        }
        info "backed up the existing hook to $HOOK_BACKUP"
    fi
fi

( cd "$PAYLOAD" && find . -type f | sed 's#^\./##' | sort ) >"$NEW/.brickdesk-manifest" ||
    die "cannot write the install manifest"

{
    printf 'version=%s\n' "$VERSION"
    printf 'git=%s\n' "$GIT_REV"
    printf 'installed=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo unknown)"
} >"$NEW/.brickdesk-version"

for dir in "$NEW/bin" "$NEW/plugins/hello/bin" "$NEW/apps/brickdesk-demo"; do
    [ -d "$dir" ] || continue
    find "$dir" -type f -exec chmod +x {} \; 2>/dev/null || true
done
chmod +x "$NEW/boot-hook.sh" 2>/dev/null || true

if [ -d "$DEST" ]; then
    mv "$DEST" "$OLD" || {
        rm -rf "$NEW"
        die "cannot move the existing install out of the way"
    }
fi
if ! mv "$NEW" "$DEST"; then
    if [ -d "$OLD" ]; then
        mv "$OLD" "$DEST" 2>/dev/null || true
    fi
    die "cannot activate the new install"
fi
rm -rf "$OLD" 2>/dev/null || true

if [ "$INSTALL_HOOK" = "1" ]; then
    [ -x "$DEST/boot-hook.sh" ] || die "installed boot hook is missing: $DEST/boot-hook.sh"
    mkdir -p "$HOOK_DIR" || die "cannot create $HOOK_DIR"
    hook_tmp=$DEST/state/zz_brickdesk.sh.tmp.$$
    {
        printf '#!/bin/sh\n'
        printf '# brickdesk-boot-hook v1\n'
        printf 'BRICKDESK_ROOT=%s\n' "'$DEST'"
        printf 'export BRICKDESK_ROOT\n'
        printf 'exec sh %s\n' "'$DEST/boot-hook.sh'"
    } >"$hook_tmp" || die "cannot write the boot hook"
    chmod +x "$hook_tmp" 2>/dev/null || true
    mv "$hook_tmp" "$HOOK" || die "cannot install the boot hook"
    info "installed boot hook: $HOOK"
else
    info "skipped the boot hook (--no-hook)"
fi

cp "$SCRIPT_DIR/uninstall.sh" "$UNINSTALLER" || warn "could not install $UNINSTALLER"
chmod +x "$UNINSTALLER" 2>/dev/null || true

sync 2>/dev/null || true

info "installed brickdesk $VERSION"
printf '  program:   %s\n' "$DEST"
printf '  boot hook: %s\n' "$HOOK"
printf '  uninstall: sh %s\n' "$UNINSTALLER"
printf 'Reboot to start brickdesk. After three failed starts the stock UI is used.\n'
