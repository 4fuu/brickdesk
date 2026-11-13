# brickdesk plugin API

Status: protocol version 1 (M1). This document describes the implemented
plugin host in `src/plugin/`, the public header `include/brickdesk/plugin.h`,
and the two sample plugins under `plugins/`.

## 1. Model

A plugin is a separate process. It never shares the desktop address space,
never gets a GPU context, and never reads evdev directly. The host:

- scans a plugin directory for `<package>/plugin.json`;
- starts a plugin on demand (its tile enters the prefetch area, or its
  settings page opens);
- talks to it over a per-plugin Unix stream socket using newline-delimited
  JSON;
- validates and renders the plugin's declarative UI;
- reads dynamic tile pixels from a shared-memory RGBA surface;
- stops an idle plugin, restarts a crashed one with backoff, and quarantines
  one that keeps crashing.

A plugin crash, a hang, or a busy loop cannot block the desktop main loop:
all host-side socket IO is non-blocking, and the host only waits in `poll()`
with a deadline.

## 2. Package layout

```text
plugins/
  hello/
    plugin.json
    bin/hello          # executable (built by CMake or plugins/hello/Makefile)
  hello-py/
    plugin.json
    main.py            # executable script, host-only sample
```

The package directory name is only a package name. The stable identity is the
manifest `id`; two packages may not register the same id. Paths in the
manifest are relative to the package directory. Absolute paths, `..`
components, empty components, symlinked package directories, and
non-executable entry files are rejected at scan time.

The default directories are:

| Build | Plugin directory | State directory |
| --- | --- | --- |
| host | `<repo>/plugins` | `/tmp/brickdesk-state` |
| target | `/mnt/SDCARD/brickdesk/plugins` | `/mnt/SDCARD/brickdesk/state` |

Override the plugin directory with `BRICKDESK_PLUGIN_DIR` or the
`plugins.dir` configuration key. Sockets, shared-memory files, per-plugin cache
and data directories live under `BRICKDESK_RUNTIME_DIR` (default
`/run/brickdesk`, falling back to `/tmp/brickdesk`).

Persisted plugin settings are host-owned state: each plugin id gets
`<state-dir>/plugin-settings/<id>/settings.json`. The state directory comes
from `bd_plugin_host_config.state_dir`, then `BRICKDESK_STATE_DIR`, then the
build-time default above. The host never writes settings into the plugin
package directory, and the protocol has no path parameter, so a plugin cannot
choose the location.

## 3. Manifest

`plugin.json` is a single JSON object, at most 64 KB.

```json
{
  "schema": 1,
  "id": "com.example.music",
  "name": "Music",
  "version": "0.1.0",
  "api": 1,
  "runtime": "native",
  "autostart": false,
  "entry": ["bin/music", "--socket", "{socket}"],
  "capabilities": ["tile", "settings", "menu"],
  "permissions": ["files.read:music", "network"],
  "tile": { "w": 1, "h": 2, "refresh_hz": 10 },
  "menu": [{ "id": "now-playing", "label": "Now playing" }],
  "limits": { "memory_mb": 32, "nofile": 64, "nice": 10 },
  "settings": [
    { "key": "quality", "type": "string", "label": "Quality", "default": "high" },
    { "key": "show_meta", "type": "bool", "label": "Show metadata", "default": true }
  ]
}
```

| Field | Required | Rules |
| --- | --- | --- |
| `schema` | yes | Must be `1`. |
| `id` | yes | `[a-z0-9._-]{1,64}`, must not start with `.` or `-`; unique. |
| `name` | yes | 1–64 bytes. |
| `version` | yes | 1–32 bytes, free-form. |
| `api` | yes | Must be `1`; the host refuses newer protocol versions. |
| `runtime` | no | `native`, `python`, `shell`, or `lua`; informational. |
| `autostart` | no | Boolean; starts without a visible tile. Default false. |
| `entry` | yes | Array of 1–8 strings. `entry[0]` is a relative executable path. |
| `capabilities` | no | `tile`, `menu`, `settings`, `hotkey`, `background`. Unknown values reject the manifest. |
| `permissions` | no | Up to 8 short strings, shown to the user. M1 does not enforce them. |
| `tile` | required with `tile` | `w`/`h` are 1 or 2; `refresh_hz` is 1–15. |
| `menu` | no | Up to 8 objects with `id` and `label`; declares host menu entries. |
| `limits.memory_mb` | no | 1–256; applied as `RLIMIT_AS`. Default 64. |
| `limits.nofile` | no | 8–256; applied as `RLIMIT_NOFILE`. Default 64. |
| `limits.nice` | no | 0–19; applied with `setpriority`. Default 10. |
| `settings` | no | Up to 8 objects: `key`, `type` (`bool`, `int`, `float`, `string`), optional `label`, optional `default`. |

`entry` placeholders:

| Placeholder | Value |
| --- | --- |
| `{socket}` | The Unix socket the host is listening on for this plugin. |
| `{plugin_dir}` | The package directory (also the child's working directory). |
| `{data_dir}` | Per-plugin writable data directory. |
| `{cache_dir}` | Per-plugin cache directory. |

The same values are exported as `BRICKDESK_PLUGIN_SOCKET`,
`BRICKDESK_PLUGIN_DIR`, `BRICKDESK_PLUGIN_DATA`, `BRICKDESK_PLUGIN_CACHE`,
and `BRICKDESK_PLUGIN_ID`.

Invalid manifests are skipped, not fatal. The host records a diagnostic with
the package name and reason.

## 4. Transport

- One `AF_UNIX`, `SOCK_STREAM` socket per plugin, created by the host before
  the child starts.
- The plugin connects to `BRICKDESK_PLUGIN_SOCKET` (or the `{socket}` argv
  value).
- Messages are UTF-8 JSON objects separated by `\n`. There is no length
  prefix. This matches `brickdesk-inputd` and `brickdesk-perfd`.
- A line may be at most 64 KB. A longer line is a protocol error and the
  plugin is stopped.
- Requests use JSON-RPC 2.0 style `id`; notifications omit `id`. The host
  replies to plugin requests, and the plugin replies to `host.hello` and
  `host.ping`.

Error codes: `-32600` invalid request, `-32601` unknown method, `-32602` bad
parameters, `-32603` internal error (including settings persistence failures),
`-32005` internal/out-of-memory. The host logs protocol errors and keeps
running; a malformed line does not take down the desktop.

## 5. Lifecycle

| Phase | Behavior |
| --- | --- |
| Discovery | Scan packages, validate manifests, sort by `id`. |
| Start | `fork` + `execv` with `cwd` = package dir, `PR_SET_PDEATHSIG`, `RLIMIT_AS`, `RLIMIT_NOFILE`, `nice`, and a private process group. |
| Handshake | Host sends `host.hello`; `plugin.ready` must arrive within 2 s. Timeout kills and restarts. |
| Heartbeat | `host.ping` every 5 s; 15 s without any message restarts the plugin. |
| Idle | No visible tile, no open settings page, no `background` or `autostart`: stop after 60 s. |
| Crash | Exponential restart delay 1 s, 2 s, 4 s, capped at 8 s. At most 3 restarts in 60 s; the next crash quarantines the plugin. |
| Shutdown | `host.shutdown`, then 3 s grace, `SIGTERM`, 500 ms, `SIGKILL`. |

The tile still occupies its wall slot while stopped or quarantined; the host
draws a fallback with the plugin name and state. A quarantined plugin can be
retried with `bd_plugin_reset_crash_limit()` and `bd_plugin_activate()`.

## 6. Host to plugin

### `host.hello` (request, id = 1)

```json
{"jsonrpc":"2.0","id":1,"method":"host.hello","params":{
  "api":1,
  "plugin_id":"com.example.music",
  "display":{"w":1024,"h":768,"scale":1.0},
  "theme":{"name":"default","dark":true,"accent":"#e06c4a"},
  "locale":"zh-CN",
  "paths":{"data":"/run/brickdesk/plugin-data/com.example.music",
           "cache":"/run/brickdesk/com.example.music"},
  "surface":"/run/brickdesk/p3-com.example.music.shm",
  "session_id":"4f2a10c8"
}}
```

Reply:

```json
{"jsonrpc":"2.0","id":1,"result":{"pid":1234,"api":1,
  "capabilities":["tile","settings"]}}
```

A `plugin.ready` notification is also accepted. The host does not verify the
reported pid against the child pid; it is diagnostic data.

### `host.ping` (request)

```json
{"jsonrpc":"2.0","id":7,"method":"host.ping"}
```

Reply with any result, for example `{"pong":true}`. Any inbound message also
refreshes the heartbeat.

### `host.event` (notification)

```json
{"jsonrpc":"2.0","method":"host.event","params":{
  "type":"settings.changed","key":"show_meta","value":false
}}
```

Events used in M1:

| `type` | Extra fields |
| --- | --- |
| `tile.visible` | `visible`: boolean. Sent when the tile enters or leaves the prefetch area. |
| `settings.visible` | `visible`: boolean. Sent when a settings page opens or closes. |
| `settings.changed` | `key`, `value` typed according to the manifest setting. |
| `ui.activate` | `node`: node id, `event`: `"activate"`; sent by `bd_plugin_activate_node()` when a settings button or row is activated. |

`host.action` (request) and `theme.changed`/`system.suspend`/`system.resume`
are reserved for later protocol versions. A plugin must ignore unknown event
types.

### `host.shutdown` (notification)

Ask the plugin to exit. The host sends `SIGTERM` after the grace period and
`SIGKILL` after that.

## 7. Plugin to host

### `plugin.ui.set` (notification or request)

```json
{"jsonrpc":"2.0","method":"plugin.ui.set","params":{
  "tile": { "type":"column", "children":[ ... ] },
  "settings": { "type":"column", "children":[ ... ] }
}}
```

`tile` and/or `settings` may be present. Omitting one keeps the previous tree
for that surface. The whole message (both trees) is limited to 48 KB, each
tree to 96 nodes and depth 8, and containers to 24 children. A rejected tree
leaves the previous tree in place and increments the host's protocol error
counter.

### `plugin.tile.acquire` (optional request)

```json
{"jsonrpc":"2.0","id":3,"method":"plugin.tile.acquire","params":{"w":168,"h":168}}
```

The surface is already mapped by the host and its path was sent in
`host.hello`. This request records the dimensions in the header, resets the
sequence to 0, and returns `w`, `h`, `stride`, and the surface path. A plugin
may skip it and write the header directly.

### `plugin.tile.frame` (notification)

```json
{"jsonrpc":"2.0","method":"plugin.tile.frame","params":{"seq":12}}
```

Announce a completed frame. The host only marks the tile dirty when the header
sequence number actually changed, and it reads at most `refresh_hz` times per
second.

### `plugin.settings.get` / `plugin.settings.set` (requests)

```json
{"jsonrpc":"2.0","id":4,"method":"plugin.settings.get","params":{"key":"show_meta"}}
{"jsonrpc":"2.0","id":5,"method":"plugin.settings.set",
 "params":{"key":"show_meta","value":false}}
```

`get` returns `{"key":..., "value":...}` with the value typed according to the
manifest. Values are loaded from the host-owned settings file when the host
scans plugins, so they survive a desktop restart.

`set` validates the type, writes the new value atomically, and then sends a
`settings.changed` event back. A normal success returns `{}`.

If the temporary file, its fsync, its close, or the rename fails before the new
file is in place, the value stays unchanged and the host returns `-32603` with
the I/O error message.

If the rename succeeded but opening, fsyncing, or closing the settings
directory fails, the new file is already visible. The host keeps the new value
in memory, still sends `settings.changed`, returns `{"durable":false}`, and
records a diagnostic instead of reporting a plain success. A crash or power
loss before the directory entry is durable may restore the old file, so a
caller that needs a durable acknowledgement can retry the same `set`. The
host-side `bd_plugin_set_setting()` follows the same rule: it returns true
because the value is applied, and exposes the sync failure through
`bd_plugin_host_diagnostic()`.

The settings file is a single JSON object. Values are encoded as strings and
validated against the manifest when read:

```json
{"version":1,"settings":{"show_meta":"false","quality":"high"}}
```

The host writes a temporary file in the same directory, fsyncs it, renames it
over the old file, and fsyncs the directory. A missing file uses the manifest
defaults. A corrupt or unreadable file also keeps the defaults and adds a scan
diagnostic; it never prevents the plugin from loading. Unknown keys are
ignored; a file with an unsupported version falls back to the defaults.

### `plugin.log` / `plugin.toast`

```json
{"jsonrpc":"2.0","method":"plugin.log","params":{"level":"info","message":"ready"}}
{"jsonrpc":"2.0","method":"plugin.toast","params":{"message":"Saved"}}
```

Logs go to the desktop's stderr in verbose mode. `toast` is logged in M1; the
desktop does not draw a toast overlay yet.

## 8. Declarative UI

The host owns fonts, colors, layout, focus, and rendering. A plugin references
no font files, colors, or GPU resources.

Node types:

| `type` | Fields | Notes |
| --- | --- | --- |
| `column` | `children`, `padding`, `gap` | Vertical layout. |
| `row` | `children`, `padding`, `gap` | Horizontal layout; icons get 28 px, the rest share the remaining width. |
| `text` | `text`, `style` | `style`: `title`, `body`, `caption`, `mono`. Newlines up to 4 lines. |
| `icon` | `name` | The host draws a rounded badge with the first character. |
| `progress` | `value`, `max`, optional `text` | 0 <= `value` <= `max`. |
| `toggle` | `id`, `label`, `value` | Interactive; `id` is required. |
| `slider` | `id`, `label`, `value`, `min`, `max`, `step` | Interactive; `min` < `max`, `step` > 0. |
| `select` | `id`, `label`, `value`, `options` | `value` must be one of `options` (1–12 strings). |
| `button` | `id`, `label`, `action` | Interactive; `id` and `action` are required. |
| `spacer` | — | Flexible space. |

Toggles share the native settings switch geometry: a 44×24 px track with a
4 px radius and an 18×18 px knob. The renderer reserves a 12 px label gap and
4 px right inset so the switch stays clear of the row focus outline. Colours
come from the host theme; plugins cannot override the switch shape.

Common optional fields: `id` (up to 39 bytes), `padding` and `gap` (0–64).
Strings are limited to 127 bytes; icon names, actions, and ids to 39 bytes;
select options to 31 bytes. Unknown node types, missing required fields,
non-finite numbers, out-of-range values, and over-depth/over-count trees are
rejected by the whole `plugin.ui.set`.

Tile example (168×168 px, 1×1 cell) from `plugins/hello`:

```json
{"type":"column","padding":14,"gap":8,"children":[
  {"type":"row","gap":8,"children":[
    {"type":"icon","name":"hello"},
    {"type":"text","text":"你好插件","style":"title"}
  ]},
  {"type":"text","text":"独立进程 · JSON IPC","style":"caption"},
  {"type":"progress","value":62,"max":100},
  {"type":"text","text":"来自 C 插件","style":"caption"}
]}
```

Settings example:

```json
{"type":"column","padding":20,"gap":14,"children":[
  {"type":"text","text":"设置","style":"title"},
  {"type":"toggle","id":"show_meta","label":"显示副标题","value":true},
  {"type":"slider","id":"pace","label":"节奏","value":3,"min":1,"max":5,"step":1},
  {"type":"select","id":"mode","label":"模式","value":"calm",
   "options":["calm","bright","night"]},
  {"type":"button","id":"refresh","label":"刷新","action":"refresh"}
]}
```

User activation is sent back as `host.event`. Setting changes use
`settings.changed`; buttons and explicit row activations use `ui.activate`
through `bd_plugin_activate_node()`. Menu routing is reserved for the menu work
(M1-09); the renderer and event API are already available.

## 9. Shared-memory frames

The host creates one file per plugin in the runtime directory and sends its
path as `surface` in `host.hello`. The file is at least:

```text
offset 0                          offset 32
+---------------------------------+---------------------------+
| header (8 x uint32 little endian)| RGBA8 pixels, top row first|
+---------------------------------+---------------------------+
```

Header:

| Offset | Field | Value |
| --- | --- | --- |
| 0 | `magic` | `0x4C544442` (`\"BDTL\"` little-endian) |
| 4 | `version` | `1` |
| 8 | `width` | 1–400 |
| 12 | `height` | 1–400 |
| 16 | `stride` | `width * 4` |
| 20 | `format` | `1` = RGBA8888 |
| 24 | `seq` | Frame sequence, starts at 0; increments after each completed frame |
| 28 | `reserved` | 0 |

Rules:

- Map the file `PROT_READ | PROT_WRITE`, `MAP_SHARED`.
- Write the width, height, stride, format, and pixels first, then store `seq`
  with release ordering. The host treats a `seq` change as a new frame.
- The host validates the magic, version, format, dimensions, and stride before
  reading. Invalid headers are ignored and counted as protocol errors.
- The host reads at most `refresh_hz` frames per second (15 fps maximum) and
  only marks the tile dirty when `seq` changed. A static frame costs nothing
  after the first upload.
- Pixels are scaled to the tile rectangle. 168×168 for a 1×1 cell is a good
  choice; the maximum is 400×400.
- A plugin that never draws a frame can still use the declarative text UI.

The host does not allocate two buffers and does not use `memfd`/`SCM_RIGHTS`
in v1. A file path is simpler to use from any language, and the `seq` field is
the commit point. This is a deliberate deviation from the architecture note;
see section 13.

## 10. Resource limits and safety

- `RLIMIT_AS` from `limits.memory_mb` (default 64 MB, sample 32 MB).
- `RLIMIT_NOFILE` from `limits.nofile` (default 64).
- `nice` from `limits.nice` (default 10).
- `PR_SET_PDEATHSIG(SIGKILL)` so a plugin cannot outlive the desktop.
- The plugin runs in its own process group; the host sends signals to the pid,
  not the group, and kills only its own child.
- The host never executes a shell: `entry` is an argv array and `execv` is
  called directly. `{socket}` and the other placeholders are substituted as
  whole argv elements, not concatenated into a shell command.
- Package paths are validated; symlinked package directories are skipped and
  the entry must be executable.
- The socket and shared-memory file are mode `0660`.
- `permissions` are declarative only in M1. There is no seccomp/user-namespace
  sandbox yet.
- The host does not yet enforce a global RSS budget across plugins
  (`plugins.max_total_rss_mb` is read by the config schema but not applied);
  each plugin's `RLIMIT_AS` is enforced.

## 11. Host C API

```c
#include "brickdesk/plugin.h"

bd_plugin_limits limits = bd_plugin_default_limits();
bd_plugin_host_config config = {0};
config.plugin_dir = "/mnt/SDCARD/brickdesk/plugins";
config.runtime_dir = "/run/brickdesk";
config.limits = &limits;
config.display_width = 1024;
config.display_height = 768;
config.dark = true;
config.accent = 0xe06c4a;
config.locale = "zh-CN";

bd_plugin_host *host = bd_plugin_host_create(&config);
char error[256];
bd_plugin_host_scan(host, error, sizeof(error));

bd_plugin *plugin = bd_plugin_host_find(host, "com.example.music");
bd_plugin_set_tile_visible(plugin, true);     /* on-demand start */
while (running) {
  bd_plugin_host_poll(host, 0);               /* non-blocking */
  if (bd_plugin_host_take_dirty(host)) {
    /* redraw the plugin tile */
  }
  /* sleep in poll()/platform event wait until host_next_timeout() */
}
bd_desktop_destroy(desktop);                  /* frees the tile texture */
bd_plugin_host_destroy(host);
```

`bd_plugin_tile_begin_frame()` uploads a changed frame and returns true when
the caller must damage the tile rect; `bd_plugin_tile_draw()` draws the frame,
the declarative tree, or the state fallback. The desktop calls these from
`src/app/desktop.c`. `bd_plugin_ui_draw()` renders a tree with a focused node
and scroll offset, and `bd_plugin_ui_measure()` returns the height the tree
needs at a given width for frame sizing.

The client SDK used by C plugins is in the same header:
`bd_plugin_client_connect`, `bd_plugin_client_poll`,
`bd_plugin_client_set_ui`, `bd_plugin_client_tile_frame`,
`bd_plugin_client_notify`, and `bd_plugin_client_log`. The desktop-side
`bd_plugin_activate_node()` sends `ui.activate` for a node id.

## 12. Minimal C plugin

```c
#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <unistd.h>
#include "brickdesk/plugin.h"

static void on_event(void *user, const char *type, const char *params) {
  (void)user;
  if (strcmp(type, "settings.changed") == 0) {
    /* params is the raw params object: {"type":...,"key":...,"value":...} */
  }
}

int main(void) {
  bd_plugin_client *client = bd_plugin_client_connect(NULL, 5000);
  if (client == NULL || !bd_plugin_client_ready(client)) {
    return 1;
  }
  bd_plugin_client_set_event_handler(client, on_event, NULL);
  bd_plugin_client_set_ui(client,
      "{\"type\":\"column\",\"padding\":14,\"gap\":8,\"children\":["
      "{\"type\":\"text\",\"text\":\"Hello\",\"style\":\"title\"},"
      "{\"type\":\"progress\",\"value\":50,\"max\":100}]}",
      "{\"type\":\"column\",\"padding\":20,\"children\":["
      "{\"type\":\"toggle\",\"id\":\"on\",\"label\":\"Enabled\","
      "\"value\":true}]}");

  while (!bd_plugin_client_shutdown_requested(client)) {
    if (bd_plugin_client_poll(client, 500) < 0) {
      break;
    }
  }
  bd_plugin_client_destroy(client);
  return 0;
}
```

Manifest:

```json
{
  "schema": 1,
  "id": "com.example.hello",
  "name": "Hello",
  "version": "0.1.0",
  "api": 1,
  "entry": ["hello"],
  "capabilities": ["tile", "settings"],
  "tile": { "w": 1, "h": 1, "refresh_hz": 15 },
  "settings": [{ "key": "on", "type": "bool", "default": true }]
}
```

Build the SDK and plugin with CMake; CMake writes the sample binary to
`plugins/hello/bin/hello`. `plugins/hello/Makefile` is a standalone fallback
that compiles the same client sources. `plugins/hello-py/main.py` is the same
protocol in Python and is used by the host tests but is not packaged for the
device.

## 13. Deviations and limits in M1

| Area | Architecture note | Implementation |
| --- | --- | --- |
| Framing | 4-byte length prefix | Newline-delimited JSON, matching inputd/perfd |
| Tile transfer | `memfd` + `SCM_RIGHTS` | Host-created file path in `host.hello`; plugin maps it |
| Package identity | Directory name equals `id` | Directory is a package name; `id` is validated and unique |
| Menu/hotkey UI | Menu items and hotkeys declared | Capabilities and events exist; menu routing is M1-09 |
| Images in UI | `image` node with host decoding | Not implemented; use icon/text/progress in M1 |
| Global RSS | 128 MB total, LRU stop | Per-process `RLIMIT_AS` only |
| Persistence | Settings persisted | Per-plugin `plugin-settings/<id>/settings.json` under the host state root |
| Protocol patching | `plugin.ui.patch` | Whole-tree `plugin.ui.set` only |

These are intentionally visible so plugins and tests can rely on the
implemented contract rather than the research drafts.
