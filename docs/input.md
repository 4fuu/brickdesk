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

## 3. `brickdesk-inputd`

```sh
brickdesk-inputd [--config-dir DIR] [--socket PATH] [--replay FILE]
                 [--replay-fast] [--exit-after-replay] [--no-grab] [--verbose]
```

- Default is passive: it opens every `/dev/input/event*` read-only and never
  grabs. Games/foreground apps still receive every key.
- The daemon blocks in `poll()`; the timeout is the next hotkey/replay
  deadline, capped at 500 ms for signals. It does not busy-wait.
- If `/dev/input` does not exist, it logs a warning and keeps the socket open;
  use `--replay` on a host.
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
{"type":"pong","time_ms":1238}
{"type":"error","cmd":"grab","error":"Device or resource busy","device":"/dev/input/event2"}
{"type":"warning","message":"syn_dropped","device":"TrimUI Gamepad","time_ms":1239}
```

- Action events go to clients with `actions=true` (the default).
- Key events go to clients with `keys=true`, and always to everyone while
  grabbed (the menu needs the keys the hotkey layer normally reserves).
- `source` is `EVIOCGNAME`; for replay it is `"replay"`.
- `trigger` is `tap`, `chord`, `hold`, `repeat`, `double` or `sequence`.
  `phase` is `down` or `up`; non-hold actions normally emit only `down`.
- `switch` names: `headphone`, `tablet`, `lid`, or a numeric code.

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

`brickdesk` still uses SDL keyboard scancodes (Z/X/C/V, arrows, Q/E, Tab,
Enter, Escape) but feeds the resulting logical keys through the same state
machine. `tests/harness` loads the same `config/hotkeys.ini` and feeds
scenario key events at `frame * fixed_dt_ms`; the existing baseline images are
unchanged because arrow keys are not reserved hotkeys.

`ui.hint` is emitted as a hold action but the M1 desktop has no hint bar yet;
the menu/hint UI consumes it in M1-09. `menu.open`, `perf.cycle` and
`session.return_home` are likewise forwarded to subscribers, not executed by
inputd.

## 7. Known deviations / device checks

- `suppress=true` (per-binding grab + uinput forwarding) is not implemented.
  Only the explicit menu `grab`/`ungrab` path exists.
- `scope=app:`/`mode:` is parsed and conflict-checked but inputd activates
  global scope only; there is no session context yet.
- `uinput` is not created by inputd. Any uinput-created device that appears in
  `/dev/input` is read like a normal device; `--replay` covers host testing.
- Hotplug/suspend-resume is not handled; if an fd dies, restart inputd.
- Real key codes, whether SDL's mali driver grabs devices, and the
  `EVIOCGRAB` effect on running games must be confirmed on hardware with
  `bd-input --list/--learn` and the checklist in
  `docs/research/architecture.md` 4.9.
