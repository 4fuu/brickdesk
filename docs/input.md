# Input: keys, hotkeys and `brickdesk-inputd`

This document describes the M1-05 input layer. It is the companion to
`docs/config-reference.md` section 6 (hotkey syntax) and section 7
(`input.ini`).

## 1. Pieces

| Piece | Purpose |
|---|---|
| `include/brickdesk/input.h` | logical keys, hotkey table, pure state machine, evdev/mapping API, socket protocol |
| `src/input/hotkey.c` | `hotkeys.ini` loader, conflict detection, state machine |
| `src/input/evdev.c` | `/dev/input/event*` enumeration, non-blocking reads, `EVIOCGRAB`, resync |
| `src/input/device_map.c` | `input.ini` mapping, built-in fallback, evdev name/code table |
| `src/input/inputd.c` | `brickdesk-inputd` daemon and Unix-socket server |
| `tools/bd-input.c` | `--list`, `--learn`, `--watch`, `--check` |

The same state machine is used by:

- `brickdesk-inputd` for evdev and replay input;
- `brickdesk` for host SDL keyboard input (`src/main.c`);
- `tests/harness` for scenario replay (`tests/harness/main.c`).

That is why a scenario screenshot and a device run agree on tap/chord/hold
semantics even though the host uses SDL scancodes.

## 2. State machine rules

All timestamps are milliseconds from a monotonic clock. The state machine is
pure: `bd_input_state_feed()` takes a timestamped key transition and emits
`bd_input_output` events through a callback; `bd_input_state_advance()` fires
timers. It never reads the clock or IO.

- **Tap**: a single-key binding without `hold_ms`/`(double)` fires on release.
  If the key is a prefix of a sequence or double-tap binding, the tap is
  delayed until that binding's window expires.
- **Chord**: fires when the last key goes down, provided all chord keys were
  pressed within `window_ms` of the first. Releasing in any order does not
  change the fired action; the binding re-arms only after every chord key is
  released.
- **Hold**: a binding with `hold_ms` (or `(hold:N)` or `repeat=true`) fires
  `trigger=hold, phase=down` at the threshold and `phase=up` on release. This
  is the hint-bar semantic: `ui.hint` down shows it, `ui.hint` up hides it.
  Releasing at exactly the threshold counts as held.
- **Repeat**: after the hold threshold, `trigger=repeat, phase=down` fires
  every `repeat_ms` until release. The release still emits one hold-up.
- **Double tap**: the second press within `input.double_tap_ms` (default 300)
  after the first release fires `trigger=double`; a delayed tap on the same
  key is canceled. The second press at exactly the window boundary counts.
- **Sequence**: steps are chords. All keys of a step must be released before
  the next step starts; `sequence_ms` is measured from that release. A step at
  exactly the deadline still counts.
- **Out-of-order / duplicate events**: a timestamp older than the last
  processed time is clamped to that time; duplicate down or up events are
  ignored. `bd_input_state_release_all()` emits release phases and clears all
  state, for `SYN_DROPPED`, ungrab and shutdown.

Conflict handling is described in `docs/config-reference.md` 6.3. In short:
identical combos are errors and the later binding is rejected; prefix/subset
and first-key overlap are warnings, and the runtime priority is completed
chord > hold threshold > double > sequence > delayed tap.

### 2.1 Global keys handled by the shell

These are not hotkey bindings; the shell handles them even while an app owns
the screen:

| input | action |
|---|---|
| `VOLUME_UP` / `VOLUME_DOWN` | change `audio.volume` by 5 and show an OSD notice |
| hold `MENU` + volume | change `display.brightness` by 5 |
| hold `SELECT` + volume | change `display.color_temperature` by 10 |
| `F1` / `F2` | `input.f1_action` / `input.f2_action`, or launch `input.f1_app` / `input.f2_app` when set |
| `HOME` | `input.home_action` (default `recent`) |
| FN switch `gpio243` | `input.fn_switch_action`: `mute`, `led_off`, `performance`, `powersave`, `turbo`, `dpad_joystick`, `none` |

Volume/brightness changes go through the same `bd_settings_system` backend as
the settings page and are persisted to `local.ini`.  The OSD is the shared
notice layer; it queues while an app is foreground and appears when the desktop
returns.  The FN switch is polled every 200 ms; the first read only records the
current position, and each later change runs the configured action and a double
haptic pulse.

The `dpad_joystick` and `turbo` actions, and the matching `input.dpad_to_joystick`
/ `input.turbo` settings, write the stock control files:

```text
/tmp/trimui_inputd/input_dpad_to_joystick
/tmp/trimui_inputd/turbo_a ... turbo_r2
```

The file exists = enabled.  `BRICKDESK_INPUTD_CONTROL_DIR` overrides the
directory for tests.  These are the same files NextUI's tg5040 backend touches;
the vendor daemon is expected to watch them.  brickdesk does not run or replace
`trimui_inputd`.

## 3. `brickdesk-inputd`

```sh
brickdesk-inputd [--config-dir DIR] [--socket PATH] [--device-dir DIR]
                 [--replay FILE] [--replay-fast] [--exit-after-replay]
                 [--no-grab] [--verbose]
```

- Default is passive: it opens every `/dev/input/event*` read-only and never
  grabs. Games/foreground apps still receive every key.
- The daemon blocks in `poll()`; the timeout is the next hotkey/replay/device
  scan deadline, capped at 500 ms for signals. It does not busy-wait.
- It watches the device directory with `inotify` and rescans on create,
  delete, move or attribute changes. If `inotify` or the directory is
  unavailable, it falls back to periodic scans with bounded exponential
  backoff (250 ms, doubling to 5 s). Starting with no device directory or no
  devices is supported; the socket stays open and devices that appear later
  are picked up.
- If `/dev/input` does not exist, it logs a warning and keeps the socket open;
  use `--replay` on a host.
- `--device-dir DIR` overrides `/dev/input`. It is also the entry point for
  the hardware-free FIFO backend described in 3.3.
- Default socket path: `$BRICKDESK_INPUT_SOCKET`, else
  `/run/brickdesk/inputd.sock`; if `/run/brickdesk` cannot be created,
  `/tmp/brickdesk-inputd.sock`.
- The socket is `SOCK_STREAM`, mode 0660. A slow client is disconnected; the
  daemon never blocks on a subscriber.

### 3.1 Grab / ungrab

`{"cmd":"grab"}` asks inputd to `EVIOCGRAB` every open device. This is for the
menu: while it is open the game should not receive input. If any device fails
(e.g. `EBUSY` because the game grabbed it first), inputd rolls back and sends
an error.

A device added while the grab is active is grabbed before it is registered. If
that fails, inputd does not add it to the poll set and broadcasts
`hotplug_grab_failed` with the path and `strerror`; it never reports the grab
as covering a device it could not acquire. The device is retried after
ungrab. Removing a device during a grab releases only that device's logical
keys and holds; the grab stays active for the remaining devices (and for
later hotplug devices).

`{"cmd":"ungrab"}` waits until all logical keys are released, then ungrabs. If
keys are still held after 500 ms it force-ungrabs so the device cannot get
stuck, and reports `"forced":true`. On ungrab inputd sends `release_all` so a
menu shim can flush its own key state. On daemon shutdown it ungrabs.

`--no-grab` rejects grab requests.

### 3.2 Socket protocol

Newline-delimited JSON, one object per line, maximum 4096 bytes. This is the
M1 subset; it is intentionally small enough for the menu shim and `bd-input`.

Client to daemon:

```json
{"cmd":"subscribe","actions":true,"keys":false}
{"cmd":"grab"}
{"cmd":"ungrab"}
{"cmd":"release_all"}
{"cmd":"set_scope","scope":"app:org.libretro.retroarch"}
{"cmd":"reload"}
{"cmd":"ping"}
```

Daemon to client:

```json
{"type":"hello","version":1,"pid":123,"socket":"/run/brickdesk/inputd.sock","devices":["TrimUI Gamepad"]}
{"type":"action","binding":"hotkey.menu","action":"menu.open","combo":"MENU","scope":"global","trigger":"hold","phase":"down","key":"MENU","source":"TrimUI Gamepad","time_ms":1234}
{"type":"key","key":"UP","pressed":true,"source":"TrimUI Gamepad","time_ms":1235}
{"type":"switch","switch":"headphone","value":1,"source":"gpio-keys","time_ms":1236}
{"type":"grab","active":true,"pending":false,"forced":false}
{"type":"release_all","time_ms":1237}
{"type":"scope","scope":"app:org.libretro.retroarch"}
{"type":"reloaded"}
{"type":"device","action":"added","device":"TrimUI Gamepad","path":"/dev/input/event2","grabbed":false,"time_ms":1238}
{"type":"device","action":"removed","device":"TrimUI Gamepad","path":"/dev/input/event2","reason":"hangup","time_ms":1239}
{"type":"warning","message":"hotplug_grab_failed","device":"/dev/input/event5","error":"Device or resource busy","time_ms":1240}
{"type":"pong","time_ms":1238}
{"type":"error","cmd":"grab","error":"Device or resource busy","device":"/dev/input/event2"}
{"type":"warning","message":"syn_dropped","device":"TrimUI Gamepad","time_ms":1239}
```

- Action events go to clients with `actions=true` (the default).
- Key events go to clients with `keys=true`, and always to everyone while
  grabbed (the menu needs the keys the hotkey layer normally reserves).
- `device` added/removed lines are additive diagnostic notifications. They are
  broadcast to every client; older clients can ignore them. `hello` still
  lists the currently registered devices only, so a rescan or replug cannot
  duplicate a name in the list.
- `hotplug_grab_failed` is broadcast once per rejected path until ungrab.
- `source` is `EVIOCGNAME`; for replay it is `"replay"`.
- `trigger` is `tap`, `chord`, `hold`, `repeat`, `double` or `sequence`.
  `phase` is `down` or `up`; non-hold actions normally emit only `down`.
- `switch` names: `headphone`, `tablet`, `lid`, or a numeric code.
  `SW_HEADPHONE_INSERT` keeps using the existing `switch` line; no separate
  audio backend or message is involved.
- `set_scope` replaces the daemon's hotkey context.  Every device's input state
  is released and re-armed with the new scope, then `suppress` bindings are
  recomputed.  The daemon echoes `scope` so a reconnecting client can confirm
  the active context.
- `reload` re-reads `config/` and `local.ini`, reloads `hotkeys.ini` and
  `input.ini`, re-arms every device and recomputes `suppress`.  Errors are
  returned as `{"type":"error","cmd":"reload","error":"..."}`; success sends
  `{"type":"reloaded"}`.

### 3.3 Device loss and the FIFO test backend

When a device read returns `POLLHUP`/`POLLERR`, `POLLNVAL`, or an error, inputd:

1. releases only that device's logical pressed keys and active holds (the
   global `release_all` notification is not sent, so keys held on another
   device stay down);
2. closes the fd and removes it from the poll set and `hello` list;
3. leaves the grab active for the remaining devices;
4. schedules a bounded reopen scan even if no directory event follows. This
   covers an fd invalidated by resume/error while the node still exists. A
   candidate that opens but is immediately `POLLHUP` (for example a FIFO
   without a writer) is closed again instead of being registered and removed
   repeatedly; the next retry uses the same 250 ms..5 s backoff. On kernels
   where `poll()` reports neither `POLLIN` nor `POLLHUP` for a writerless
   FIFO, readiness uses `read()`: EOF means no writer and `EAGAIN` means a
   writer is present. A byte consumed by that probe is replayed by
   `bd_evdev_read()` as part of the first event.

A later rescan reopens the path, re-runs `input.ini` matching and registers it
exactly once. Node identity (device/inode) distinguishes a replug from the
same still-open node, so an unchanged rescan never duplicates fds or events.
An `event*` node that fails to open during a scan (for example a transient
permission error or a symlink target that is not ready) marks the scan
incomplete and is retried with the same bounded backoff, even when the
directory watch stays valid and no later directory event arrives.

For hardware-free lifecycle tests, `--device-dir DIR` accepts `eventN` FIFOs
instead of character devices. A FIFO with no writer is not registered while
the writer is absent; keep the writer fd open while the device is present.
Capabilities come from `DIR/eventN.meta`:

```ini
# all fields optional except that a FIFO needs at least one capability
name=Fake Gamepad
vendor=0xbeef
product=0x0001
keys=BTN_SOUTH,BTN_EAST,BTN_MODE
abs=ABS_HAT0X,ABS_HAT0Y
sw=SW_HEADPHONE_INSERT
# deny simulates EVIOCGRAB returning EBUSY; allow (default) succeeds
grab=allow
```

The writer sends binary `struct input_event` records. This path is test-only:
it never creates a uinput node and is not used for `/dev/input` character
devices.

## 4. Replay files

`--replay FILE` replaces evdev with a text file. It is real-time by default
(timestamps are relative to daemon start); `--replay-fast` processes it
immediately and exits. `--exit-after-replay` exits at EOF.

Replay events are applied at their scheduled timestamps. Pending hotkey timers
are advanced to the event timestamp before the event is fed, so a hold that
expires at the same timestamp as the key release fires its action first. This
matters for the Menu hold (600 ms) used by the in-app menu test; a late poll
must not silently drop the action.

```text
# time_ms kind name value
0     key A down
20    key A up
500   key BTN_SOUTH down      # raw evdev name or decimal/0x code
520   key BTN_SOUTH up
700   abs ABS_HAT0Y -1
720   abs ABS_HAT0Y 0
900   sw headphone 1
1000  quit
```

Logical names (`A`, `MENU`, `VOLUME_UP`, ...) bypass `input.ini`; raw names
are translated through the mapping. A `quit` record lets pending hold/repeat
timers fire up to that timestamp.

## 5. Tools

```sh
bd-input --list [--device-dir DIR]
bd-input --learn [--device PATH] [--config-dir DIR]
bd-input --watch [--socket PATH]
bd-input --check [--config-dir DIR]
```

- `--list` prints name, IDs, capabilities and the matched `input.ini` mapping
  (with `file:line`).
- `--learn` waits for one key or HAT move and prints the raw code, the logical
  key, and the config line to use. This is the first thing to run when the
  device arrives.
- `--watch` connects to inputd, subscribes to actions and keys, and prints the
  JSON lines.
- `--check` loads `hotkeys.ini`, prints diagnostics/conflicts with
  `file:line`, and exits non-zero on errors or rejected duplicates.

## 6. Host/desktop integration

`brickdesk` connects to inputd when its socket exists and consumes both
`action` and `key` lines; SDL keyboard scancodes (Z/X/C/V, arrows, Q/E, Tab,
Enter, Escape) feed the same state machine when inputd is not running. Both
sources converge on one dispatch path: reserved bindings become actions and
unreserved keys become navigation.

Connection establishment is part of the desktop loop, not startup-only. The
socket is non-blocking from `connect()` onward, and the subscription
write is non-blocking too. If inputd is absent, a stale socket exists, or the
listen backlog is full, the shell stays on SDL keys and retries after 250 ms,
500 ms, 1 s, 2 s, 4 s, then every 5 s. The retry deadline is merged into the
same `poll()` timeout as session, plugin and hotkey timers, so an unreachable
daemon wakes the loop on that bounded cadence instead of busy-polling. A late
daemon start, a dropped client connection and a daemon restart all reconnect
without restarting the desktop.

Each accepted connection sends one side-effect-free subscription:

```json
{"cmd":"subscribe","actions":true,"keys":true}
```

The subscription restores action and key delivery without touching daemon
device state. The shell intentionally does not send `release_all` here: the
daemon applies that command to every device and broadcasts it to every client,
so it would release a game's or menu client's held input. `switch` lines
(including `headphone`) and every other protocol message are unchanged.

The shell still resets its own source-switch state. It releases the SDL hotkey
state, releases settings slider holds, clears a partially received line and
hides the hint strip on both directions of the switch. This prevents a hold,
repeat or delayed tap from surviving the change and keeps SDL and inputd from
driving the same key state at once, without affecting other inputd clients.

`tests/shell_inputd_test.c` drives a forked Unix-socket peer and a real
`brickdesk-inputd --replay` through late start, disconnect/restart, malformed
and split lines, interrupted holds, a full accept queue and idle CPU. It also
connects an independent subscriber directly to inputd while the shell uses a
forwarding socket, and checks that
the shell connecting and reconnecting never emits a `release_all` that would
end that subscriber's held input. The 400 ms Select hold (`hotkey.hint` ->
`ui.hint`) shows the hint bar and its release hides it; while Select is held,
D-pad events still reach the desktop.
`tests/harness` feeds scenario key events at `frame * fixed_dt_ms` through a
separate test state so hold timing stays deterministic. `menu.open`,
`perf.cycle` and `session.return_home` are forwarded by inputd; the desktop
executes `session.return_home` when an app owns the screen.

A binding with `hotkey.<id>.plugin` set is routed to that plugin instead of the
normal action dispatcher. `action_id` selects a declared menu entry (sent as
`menu.activate`) or, for plugins with the `hotkey` capability, an action
(sent as `hotkey` with `pressed`). If the plugin is stopped, the binding starts
it and shows a startup notice; the event is not replayed after handshake.

### 6.4 设置页重绑定

设置页“按键”分类中的每个 `[hotkey.<id>]` 现在都是可操作行。按 A 打开编辑页：

- “组合键”：按 A 开始录制，按下目标组合键后全部松开即写入
  `hotkey.<id>.combo`；B 取消。录制会绕过 inputd 的热键匹配，已绑定的组合键
  不会在录制时触发动作。
- 冲突校验：与其它绑定完全相同的组合键（按键顺序不同也算）会被拒绝并显示
  重复的绑定 id，不会写入 `local.ini`。
- “作用域”：左右切换 `global`、`mode:desktop`、`mode:game` 和已安装应用的
  `app:<id>`，写入 `hotkey.<id>.scope`。
- “独占输入”：切换 `hotkey.<id>.suppress`；开启后由 inputd grab 物理设备并
  经 uinput 转发，只有该绑定使用的键被抑制。
- 离开设置页时桌面重新加载 `hotkeys.ini`/`local.ini` 并向 inputd 发送
  `reload`，新绑定立即生效。

## 7. Known deviations / device checks

- `suppress=true` creates one uinput mirror per grabbed device.  The mirror
  forwards `EV_KEY`/`EV_ABS`/`EV_SW`; force feedback, LED and repeat (`value=2`)
  events are not mirrored yet.  If `/dev/uinput` cannot be opened the daemon
  leaves the physical device ungrabbed and logs one error instead of stealing
  input without a replacement node.
- `scope=app:`/`mode:` is active: the shell sends the current context with
  inputd's `set_scope` command (`mode:desktop`, `mode:game`, `app:<id>`).
  `mode:game` covers every `app:<id>` context.  `set_scope` and `reload` are
  daemon commands; the shell sends `reload` after leaving the hotkey editor so
  a new binding takes effect without restarting inputd.
- `uinput`-created devices appear in `/dev/input` like any other device and are
  read normally by the daemon.  The mirror is named `brickdesk <source name>`;
  its device ids copy the physical node.
- Hotplug and fd-loss recovery are implemented in the daemon, but real USB
  removal/reinsert, `udev` timing, suspend/resume fd invalidation and
  `EVIOCGRAB` against a running game still need hardware confirmation.
- Shell reconnect, a late daemon start and a full socket accept queue are
  host-tested with real processes. Boot ordering under the device init script,
  systemd/socket activation and resume ordering still need confirmation on
  hardware.
- Real key codes, whether SDL's mali driver grabs devices, and the
  `EVIOCGRAB` effect on running games must be confirmed on hardware with
  `bd-input --list/--learn` and the checklist in
  `docs/research/architecture.md` 4.9.
