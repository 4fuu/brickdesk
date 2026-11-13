# App protocol, manifests and the session layer

This document is the contract for M1-06: how brickdesk discovers applications,
launches them, suspends and resumes them, talks to them over IPC, and reads the
last rendered frame during the display handoff.

The implementation lives in:

| File | Responsibility |
|---|---|
| `include/brickdesk/app.h` | manifest, session and last-frame public API |
| `src/session/manifest.c` | `app.json` parsing, app directory scan, image fallback |
| `src/session/session.c` | process lifecycle state machine and events |
| `src/session/app_protocol.c` | newline-delimited JSON-RPC host |
| `src/session/last_frame.c` | read-only last-frame mapping and validation |
| `apps/demo/` | SDL2 + GLES2 reference application |

The session layer is a library used by `brickdesk` (or a small
`brickdesk-session` process later). It is single threaded: drive
`bd_session_poll()` from the desktop poll loop and do not call the session from
inside an event callback.

The desktop owns its own EGL surface/context. On a `LAUNCHING`/`RESUMING` event
it must release those resources before the app draws; on `EXITED`, `CRASHED`
or `BACKGROUND` it may recreate them and take the screen back. The session
only coordinates the app side of the handoff.

## 1. Application directories

A target device scans:

```text
/mnt/SDCARD/brickdesk/apps/
  org.example.game/
    app.json
    art/
      cover-1x1.jpg
      cover-3x2.jpg
      backdrop.jpg
    state/
```

The host build defaults to `<repo>/apps`; both defaults are overridable at
runtime with `BRICKDESK_APPS_DIR` or through `bd_session_config`'s callers.

Each direct child directory is one application. `app.json` is read first;
`manifest.json` (the design.md 4.1 name) is accepted as a fallback. Scanning
reads only manifests, never images. Entries are sorted by `id`; invalid
manifests are reported through `bd_app_scan_diagnostic_fn` and skipped. The
scan is cheap enough for several thousand packages because image existence is
checked lazily by `bd_app_asset_cover()` / `bd_app_asset_backdrop()`.

All paths inside the manifest are relative to the app directory and must not
be absolute, contain `.` / `..` components, or resolve outside the app
directory through a symlink. `exec[0]` may be absolute or relative to `cwd`.

## 2. `app.json` fields

Schema version 1. Unknown fields are ignored. Display strings are truncated to
their buffer sizes; identity and path errors are rejected with
`<manifest>:<line>: <message>`.

| Field | Required | Type / limit | Meaning |
|---|---|---|---|
| `schema` | yes | `1` | manifest version |
| `id` | yes | `[a-z0-9._-]{1,64}` | unique reverse-domain id |
| `name` | yes | 1..95 bytes | tile title |
| `subtitle` | no | 0..127 bytes | subtitle |
| `version` | no | 0..31 bytes | app version |
| `type` | one of `type`/`category` | `app` / `game` | default icon family |
| `category` | one of `type`/`category` | `app`, `game`, `emulator`, `tool`, `service` | menu/perf category |
| `exec` | yes | string array, ≤16 | argv; no shell parsing |
| `cwd` | no | path | working directory; default is the app directory |
| `env` | no | object, ≤32 | extra environment variables (string values) |
| `icon` or `art.icon` | no | relative path | PNG icon |
| `cover` | no | relative path | shorthand for the 1×1 cover |
| `art.covers` | no | object | keys `1x1`, `2x1`, `1x2`, `2x2`, `3x2` |
| `art.backdrop` or `backdrop` | no | relative path | full-screen backdrop |
| `art.fallbackColor` | no | `#RRGGBB` | material colour when `accent` is absent |
| `accent` | no | `#RRGGBB` | UI accent; also used by image fallback |
| `capabilities` | no | object of booleans | see section 3 |
| `hooks` | no | object | see section 3 |
| `menu.mode` | no | `overlay`, `embedded`, `handoff`, `none` | default `overlay` |
| `menu.overlay.safe` | no | bool | shim safe-area hint |
| `menu.overlay.exclude_keys` | no | string array, ≤16 | keys the shim must not intercept |
| `perf.profile` | no | string | default `balanced` |
| `perf.core` | no | string | emulator core id |
| `state.path` | no | relative path | optional state file under `BRICKDESK_STATE_DIR` |
| `state.format` | no | string | app-defined state format id |
| `state.max_slots` | no | 0..32 | default `1` |
| `live.provider` | no | `none`, `builtin.now-playing`, `file` | dynamic tile source |
| `live.state` | no | relative path | provider=`file` JSON path |
| `live.refreshMs` | no | 500..5000 | default 1000 |
| `live.fields` | no | array | `title`, `subtitle`, `progress`, `artwork` |

If only `category` is present, `type` is derived (`game` stays `game`,
everything else becomes `app`), and the reverse. This keeps manifests from both
architecture.md 5.8 and design.md 4.2 valid.

### 2.1 Image fallback

`bd_app_asset_cover()` returns the first usable item:

| Requested span | Cover candidates |
|---|---|
| `1x1` | `1x1` |
| `2x1` | `2x1`, `1x1` |
| `1x2` | `1x2`, `1x1` |
| `2x2` | `2x2`, `1x1` |
| `3x2` | `3x2`, `2x1`, `1x1` |

Then `art.icon`, then the accent/fallback colour with the first UTF-8 code
point of `name`, then a neutral tile (`BD_APP_ASSET_NEUTRAL`). Missing or
undecodable files are treated as absent. The returned `bd_app_asset.path` is an
absolute, symlink-checked path; `kind` tells the caller which visual to paint.

`bd_app_asset_backdrop()` tries `art.backdrop`, then the largest available cover
(`3x2`, `2x2`, `2x1`, `1x2`, `1x1`), then the material colour.

## 3. Capabilities and hooks

```json
{
  "capabilities": {
    "pause": true,
    "resume": true,
    "save_state": true,
    "load_state": true,
    "screenshot": true,
    "background": false,
    "display_release": true
  },
  "hooks": {
    "pause":  { "type": "ipc", "method": "app.pause",   "timeout_ms": 300 },
    "resume": { "type": "ipc", "method": "app.resume",  "timeout_ms": 2000 },
    "save":   { "type": "ipc", "method": "state.save",  "timeout_ms": 5000 },
    "load":   { "type": "ipc", "method": "state.load",  "timeout_ms": 5000 },
    "quit":   { "type": "ipc", "method": "app.quit",    "timeout_ms": 3000 },
    "screenshot": { "type": "ipc", "method": "app.screenshot" }
  }
}
```

A missing `type` is inferred from `method` (`ipc`) or `exec` (`command`). A
missing method defaults to the names shown above. `timeout_ms = 0` uses the
session default for that hook.

The session uses capabilities and hooks as follows:

| Operation | With IPC | Without IPC |
|---|---|---|
| background | `app.display_release` when `display_release=true`, then the pause hook | `SIGSTOP` to the whole process group |
| resume | `app.display_acquire` when `display_release=true`, then the resume hook | `SIGCONT` |
| terminate | save hook, then quit hook, then `SIGTERM`/`SIGKILL` | `SIGTERM`/`SIGKILL` |
| explicit save/load | `state.save` / `state.load` | `BD_SESSION_UNSUPPORTED` |

Command hooks are parsed and preserved in `bd_app`, but this milestone does not
execute them; the session falls back to SIGSTOP/SIGTERM instead. This is the
main deliberate gap relative to architecture.md 5.9.

`category=service` or `capabilities.background=true` marks an app as allowed to
keep running; the session itself does not stop background audio/video. The
desktop decides how to display the background indicator.

## 4. Environment injected before `exec`

| Variable | Value |
|---|---|
| `BRICKDESK_APP_ID` | manifest id |
| `BRICKDESK_SESSION_ID` | per-instance id (`<session>-<counter>`) |
| `BRICKDESK_SESSION_SOCKET` | Unix socket path |
| `BRICKDESK_STATE_DIR` | `<state_root>/<app-id>` (created 0700) |
| `BRICKDESK_STATE_FILE` | `<state-dir>/<state.path>` when declared |
| `BRICKDESK_MENU_MODE` | `overlay`, `embedded`, `handoff`, `none` |
| `BRICKDESK_MENU_SOCKET` | menu control socket (see section 6.1) |
| `BRICKDESK_LAST_FRAME` | last-frame file path (see section 5) |

Manifest `env` values are applied first; `BRICKDESK_*` values cannot be
overridden by the manifest. The child runs in its own session/process group
(`setsid`), gets `PR_SET_PDEATHSIG=SIGTERM` on Linux, and has inherited
session socket descriptors closed by `FD_CLOEXEC`.

## 5. Last-frame shared memory

The in-app menu shim (M1-09), or the demo before handoff, writes the last
rendered frame to a file-backed shared mapping:

```text
<frame_dir>/last-frame-<BRICKDESK_SESSION_ID>.bin
```

`frame_dir` defaults to the session socket directory (`/run/brickdesk` on the
device, then `/tmp`). The file starts with this 56-byte header, followed by
`data_size` pixel bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `magic` `0x52464442` (file bytes `BDFR`) |
| 4 | 2 | `version` = 1 |
| 6 | 2 | `header_size` = `sizeof(bd_app_last_frame_header)` |
| 8 | 4 | `width` |
| 12 | 4 | `height` |
| 16 | 4 | `format`: `1` RGBA8888, `2` RGB565 |
| 20 | 4 | `stride` bytes per row |
| 24 | 8 | `frame_seq` |
| 32 | 8 | `timestamp_ms` (CLOCK_MONOTONIC, advisory) |
| 40 | 8 | `data_size` pixel bytes |
| 48 | 4 | `checksum` (reserved, 0) |
| 52 | 4 | `flags` (`WRITING` bit 0) |

Writer protocol:

1. map the file read/write and set `flags |= WRITING`;
2. write the pixels;
3. memory fence, publish the completed header, increment `frame_seq`, clear
   `WRITING`.

The reader (`bd_app_last_frame_open`, `bd_app_last_frame_copy`) maps read-only
and rejects bad magic, version, header size, dimensions, format, stride, and
data size. `bd_app_last_frame_copy()` copies a tightly packed snapshot and
retries if `frame_seq` changed or `WRITING` was set during the copy.

This is a file-backed shared mapping rather than a `shm_open()` object so the
same path works on the device's tmpfs and in host tests; it is still shared
memory when mapped `MAP_SHARED`.

## 6. Application IPC

Transport: Unix `SOCK_STREAM`, UTF-8 JSON-RPC 2.0 subset, one JSON object per
line (`\n`), maximum line 64 KiB. This matches `brickdesk-inputd`'s newline
style rather than the length-prefixed plugin transport.

The app connects to `BRICKDESK_SESSION_SOCKET` and starts with:

```json
{"jsonrpc":"2.0","id":1,"method":"app.hello","params":{
  "pid":2345,"protocol":1,
  "capabilities":["pause","resume","save_state","load_state",
                  "screenshot","display_release"]}}
```

Host reply:

```json
{"jsonrpc":"2.0","id":1,"result":{
  "session_id":"demo-00","menu_mode":"overlay",
  "state_dir":"/mnt/SDCARD/brickdesk/state/dev.brickdesk.demo"}}
```

If no `app.hello` arrives within `hello_ms` (default 2000 ms), the session
marks the app foreground anyway and later falls back to SIGSTOP for suspension.
This preserves compatibility with apps that do not speak the protocol.

Host → app:

| Method | Params | Default timeout | Meaning |
|---|---|---|---|
| `app.display_release` | `{}` | 500 ms | destroy/release the EGL surface/context |
| `app.pause` | `{}` | 300 ms | stop rendering/logic, prepare menu |
| `app.display_acquire` | `{}` | 500 ms | recreate display resources |
| `app.resume` | `{}` | 2000 ms | resume |
| `state.save` | `{"slot":N}` | 5000 ms | save; result may include `{ok,bytes,screenshot}` |
| `state.load` | `{"slot":N}` | 5000 ms | load |
| `app.quit` | `{"save":true}` | 3000 ms | quit cleanly |
| `app.screenshot` | `{}` | 500 ms | reserved; the menu may request a screenshot |
| `app.ping` | `{}` | 500 ms | reserved heartbeat; M1 does not schedule it |

App → host:

| Method | Meaning |
|---|---|
| `app.hello` | handshake |
| `app.toast` | request a toast |
| `app.state` | report a state change |
| `app.log` | structured log |
| `app.perf.request` | running perf-profile request; M1 replies with “unsupported” |

Requests carry an integer `id`; responses use `result` or `error`. Error codes
are the architecture.md 3.5 set: `-32600`, `-32601`, `-32602`, `-32001`,
`-32002`, `-32003`, `-32004`, `-32005`. A timeout is reported to the session as
a hook failure with code `-32003`; pending requests are discarded when the
connection closes.

The reference app implements hello, pause/resume, display release/acquire,
save/load, screenshot and quit.

### 6.1 Menu control channel

The in-app menu shim cannot use the application's JSON-RPC connection: it is
not the app and must not interleave frames on that socket. The session opens a
second, one-shot control socket at
`<socket_dir>/menu-<session_id>.sock` and injects it as
`BRICKDESK_MENU_SOCKET`. The shim connects, writes one line and closes:

```json
{"type":"menu","app":"dev.brickdesk.demo","action":"background"}
```

`action` is `background` or `quit`. The session accepts at most four pending
clients, reads the first line (2 s deadline), matches `app` to a tracked
instance, and emits `BD_SESSION_EVENT_MENU_ACTION` with `method` set to the
action and `params` to the raw line. Like every session callback, the event
runs inside `bd_session_poll()`; record it and call
`bd_session_background()` or `bd_session_terminate()` after the poll returns.

The shim closes the overlay before the desktop acts, so the application can
process `app.display_release`, `app.pause` and `app.quit` from its own loop.
If the socket is missing or the request fails, the background item reports
that the desktop is unavailable and the quit item falls back to `SIGTERM`.

## 7. Lifecycle

Per-app states:

```text
idle -> launching -> foreground -> suspending -> background -> resuming -> foreground
                                   \-> terminating -> exited
non-terminating state -------------> crashed (unexpected exit/signal)
```

Transitions:

| Action | Sequence |
|---|---|
| launch | `IDLE → LAUNCHING`; after `app.hello` or the hello timeout, `LAUNCHING → FOREGROUND` |
| background | `FOREGROUND → SUSPENDING`; `display_release`, pause hook, or SIGSTOP; `SUSPENDING → BACKGROUND` |
| resume | `BACKGROUND → RESUMING`; perf apply, SIGCONT if stopped, `display_acquire`, resume hook; `RESUMING → FOREGROUND` |
| terminate | `TERMINATING`; SIGCONT if stopped; save hook → quit hook → SIGTERM (`term_ms`) → SIGKILL (`kill_ms`); `TERMINATING → EXITED` |
| unexpected exit | `EXITED` for code 0, `CRASHED` for non-zero or a signal |

### 7.1 Light-sleep freeze

`bd_session_freeze()` and `bd_session_thaw()` support the RAM-only light-sleep
path. They are not `app.pause`/`state.save` and do not protect against power
loss. Freeze sends `SIGSTOP` to the foreground instance's process group and
waits (bounded by `freeze_ms`) for `waitpid(WUNTRACED)` to report that the
managed leader stopped. It returns `BD_SESSION_OK` only after that report.

A frozen instance stays `FOREGROUND` with the same id and pid: no `STATE` or
`BACKGROUND` event is emitted, `background_count` and the background limit are
unchanged, the perf lease is not touched, and no IPC hook runs. The public
snapshot reports `frozen = true`.

Thaw sends `SIGCONT` only for the instance this session froze. An instance
stopped by someone else, or by the background path, is never continued by
thaw; it returns `BD_SESSION_INVALID_STATE`. Both operations are idempotent:
freezing a frozen app and thawing a running foreground app return
`BD_SESSION_OK`. If `SIGCONT` is rejected, thaw keeps the freeze ownership,
returns `BD_SESSION_IO_ERROR` with `frozen` still true, and a later thaw
retries it. Ownership clears only after `SIGCONT` is delivered or the instance
exits.

Freezing while the app is launching, suspending, resuming or terminating
returns `BD_SESSION_BUSY`; thawing a non-foreground instance returns
`BD_SESSION_INVALID_STATE`. A hook/state transfer pending during either call
returns `BD_SESSION_BUSY` and leaves the state unchanged. A state transfer on a
frozen app also returns `BD_SESSION_BUSY` so it cannot wait on a stopped
process. `background()` and `terminate()` continue a frozen app before their
normal hooks; if that continuation is rejected they return
`BD_SESSION_IO_ERROR` without changing the lifecycle state, so the call can be
retried. `bd_session_destroy()` kills a frozen app; `bd_session_poll()` keeps
reaping exits while it is frozen. If stop confirmation times out or the child
exits during freeze, the session tries to roll back with `SIGCONT`. If that
rollback is also rejected, the instance stays marked `frozen` and the call
returns an error so `thaw()` can retry instead of claiming the app resumed.

Default timeouts: hello 2000, pause 300, resume 2000, save 5000, quit 3000,
SIGTERM grace 1000, SIGKILL grace 1000, display 500, freeze 1000 ms. The
manifest hook `timeout_ms` overrides the matching default;
`bd_session_timeouts` overrides all defaults for tests or a future daemon.

Only one foreground app is allowed. Launching while another app is
foreground/launching/terminating returns `BD_SESSION_BUSY`; background the
current app first. Background apps do not count as foreground.

Background limit (default 3; read `session.max_background` from `brickdesk.ini`
and pass it as `bd_session_config.max_background`) applies to apps in `BACKGROUND`, whether stopped by SIGSTOP or IPC-paused. When
full, `bd_session_background()` returns
`BD_SESSION_NEEDS_CONFIRM`, emits `BD_SESSION_EVENT_NEEDS_ATTENTION` with the
oldest app id, and changes nothing. The caller may terminate that app and retry;
the session never kills it on its own.

Events (`bd_session_event_fn`):

- `BD_SESSION_EVENT_STATE`: `previous`, `state`;
- `BD_SESSION_EVENT_EXIT`: exit code or terminating signal;
- `BD_SESSION_EVENT_CRASH`: unexpected non-zero/signal;
- `BD_SESSION_EVENT_HOOK`: hook name and `hook_ok` after response/timeout;
- `BD_SESSION_EVENT_NOTIFICATION`: app toast/log/state/perf request;
- `BD_SESSION_EVENT_PROTOCOL_ERROR`: malformed line or oversized request;
- `BD_SESSION_EVENT_NEEDS_ATTENTION`: background limit, with suggestion;
- `BD_SESSION_EVENT_MENU_ACTION`: the in-app menu asked for `background` or
  `quit`; `method` is the action, `app_id`/`pid` identify the instance.

`bd_session_poll()` returns the milliseconds until the next timer (or 100 ms
while any app is active, so exits are noticed) and `-1` when idle. Call it from
the desktop loop; it drives protocol I/O, hook timeouts, `waitpid`, and SIGTERM
escalation.

## 8. Demo and tests

Build and run the host test directly:

```sh
BRICKDESK_WERROR=ON ./scripts/build-host.sh
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy BRICKDESK_HEADLESS=1 \
  ./build-host/tests/bd_session_test \
    ./build-host/brickdesk-demo \
    ./build-host/tests/bd_session_child
```

`apps/demo/app.json` points at
`/mnt/SDCARD/brickdesk/apps/brickdesk-demo/brickdesk-demo` for the
device. For a host run, copy `build-host/brickdesk-demo` next to a copy of
`apps/demo/app.json`, or point a test manifest's `exec` at the host binary
directly.

`tests/session_child.c` is a smaller protocol peer used to force SIGSTOP,
crashes, immediate exit, heartbeat ticks, ignored SIGTERM and hook timeouts.
The test verifies `/proc/<pid>/stat` state `T` while suspended, `R/S` after
resume, exit codes/signals, the background limit, manifest errors, image
fallback and last-frame validation. It also covers light-sleep freeze with a
real ticking child: execution stops and resumes, foreground identity and the
background quota stay unchanged at a full quota, polling stays live, duplicate
freeze/thaw calls are idempotent, pending hooks report busy, an externally
stopped app is never thawed, a rejected SIGCONT keeps ownership for a
successful retry, and background/terminate retries work after rejected
continuations. It also covers termination and destroy while frozen.

`tests/menu_test.c` covers the menu shim end to end: a real
`brickdesk-inputd --replay` stream opens the overlay, the menu control socket
reports `background` and `quit`, the demo's state file is written, the
last-frame header is valid, and the two menu screenshots are compared with
`tests/baselines/host/menu_*.png`. `tests/menu_state_app.c` verifies the GL
state and pixels after the menu closes. See `docs/menu-shim.md`.

## 9. Device probes and known limits

- SIGSTOP freezes a process but does **not** release its EGL surface/context.
  The desktop can only take over the display reliably for apps that declare
  `display_release` and acknowledge `app.display_release`; the M1 display probe
  (`tools/display_probe.c`) must confirm what the PowerVR driver does when a
  second process creates a surface while the first still holds one.
- If `app.pause` times out, the session SIGSTOPs the process; if the driver does
  not allow another surface, the menu must use a different fallback (save and
  quit) on that device.
- `bd_session_freeze()` is a RAM-only light-sleep primitive: the foreground
  app stays in RAM and its process group is SIGSTOPed. It does not call
  `app.pause`/`state.save`, does not touch the PMIC or any power rail, and does
  not protect state across a power loss.
- Command hooks are parsed but not executed in M1.
- `app.perf.request` from an app is rejected for now; default perf apply/restore
  before launch/after exit is wired through `bd_session_perf_ops`.
- Last frame is a snapshot for the return animation, not a live compositor; the
  desktop should not expect new frames after suspension.
- `bd_session_clear_terminated()` removes exited/crashed entries from the
  running list; an exited instance keeps its exit status until then.
