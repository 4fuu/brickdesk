# v3 settings page

`src/settings/` implements the v3 settings page as a reusable module. It is not
wired into `src/main.c`; the event-loop task owns that hand-off. The page owns
its category list, rows, focus, scroll, sub-pages and confirm dialog, and talks
to the renderer only through `include/brickdesk/gfx.h`.

## Files

| file | role |
|---|---|
| `include/brickdesk/settings.h` | public API, system seam, status/storage structs |
| `src/settings/settings.c` | page model: schema rows, navigation, values, hotkeys/perf/plugins, damage |
| `src/settings/settings_draw.c` | category list, table, controls, sub-pages, hint strip, dialog |
| `src/settings/settings_internal.h` | shared state, geometry and row structs |
| `src/settings/system_stub.c` | recording brightness/volume/mute backend for M3 |
| `tests/settings_test.c` | schema rows, persistence/reload, hold repeat, damage parity, sub-pages |
| `tests/settings_capture.c` | deterministic screenshots for every category, accent, slider, conflict, plugin and dialog |

## Layout

The page is designed at 1024x768 and follows the v3 settings reference
(`docs/research/img/v3-settings.jpg`), with the later square-corner direction:

- top bar: back chevron at x=34, serif `设置` at x=64, category name after it,
  and Wi-Fi/battery/clock readouts on the right. The settings page never draws
  a project logo; the back arrow, title and category icons are settings chrome.
  There are no search/settings/avatar icons, and the page is full-screen, not a
  side panel.
- left category list at x=28, first row at y=92, 43 px pitch. The selected row
  has a 3x22 px accent bar and an accent label; icons are drawn with renderer
  primitives.
- page title: serif 34 px at x=248, baseline 128; subtitle 14 px at baseline
  156.
- settings table: x=248, y=170, width 748, 1 px border, **no rounded card
  corners**. The height follows the content and caps at y=660; longer pages
  scroll inside the table.
- rows are 74 px when they have a subtitle and 62 px when they do not. Row
  titles are 16 px bold, subtitles 14 px, values 15 px, hints 13 px. Nothing
  is below 13 px.
- the focused row gets a 2 px accent ring (square, radius 2) and a 7% accent
  veil. The table border is redrawn per row so a partial repaint is exact.
- the bottom strip is **not** a persistent Steam-style bar. It is 58 px and
  only visible while Select is held (`bd_settings_set_hint_visible`). It
  mirrors the reference: two left lines for B/←→/A and L1/R1, and the page
  hint behind an accent bullet on the right.

## Schema-driven rows

`bd_config_describe()` in `include/brickdesk/config.h` exposes a key's schema:
type, min/max, enum values, default and length. The page builder maps that to
widgets:

| schema type | row |
|---|---|
| `bool` | toggle (`开`/`关`) |
| `int` / `duration` | slider with the schema range; duration values are shown as ms/s/min |
| `enum` | select, cycled with ←/→ |
| `accent` | six preset swatches plus a custom-colour sub-page |
| `string` / `combo` | read-only value (used by perf/hotkey rows) |

Each category has an ordered preferred key list plus its sections. Keys that
are not preferred are appended automatically when they belong to one of the
page's sections, so a new `[theme]`/`[ui]`/`[audio]`/`[display]`/`[session]`
key appears with just a label-table entry (or its raw key name). Existing keys
that belong elsewhere (for example `ui.language` or `display.width`) are
listed in a small skip set. The label table only carries presentation strings,
never ranges or enum tables.

Rows are rebuilt when the page opens, on `bd_settings_refresh()`, and after a
reset. Dynamic values are read from config at draw time, so a row repaint never
needs a rebuild just to show a new number.

## Pages

| category | rows |
|---|---|
| 外观 | `theme.mode`, `theme.accent` (+ custom RGB sub-page), `theme.ambient`, `ui.animations`, `ui.blur` |
| 声音 | `audio.volume`, `audio.mute` |
| 显示 | `display.brightness` |
| 按键 | one conflict summary plus every `[hotkey.<id>]` combo, action, trigger and warning marker |
| 性能 | `perf.global_profile` and every `[app.<id>] profile` override, both cycled through perf.ini profile names |
| 后台 | `session.max_background`, `session.memory_budget_mb` |
| 插件 | sorted plugin list; A opens the plugin host's declarative settings tree |
| 系统 | version/backend, storage, about and a destructive reset row |

New config keys added for this page:

- `[audio] volume` (0..100, default 70) and `[audio] mute` (bool, false);
- `[session] memory_budget_mb` (128..1024, default 560);
- `[perf] global_profile` (string, default `default`).

The keys page reads the table through `bd_hotkey_table_load()`, so duplicates,
prefixes and first-key overlaps come from the input layer rather than being
recomputed here. The conflict summary uses the first conflict and a count, and
the affected bindings carry a warning dot. Combo editing is not implemented
(the stretch goal); the rows are read-only.

The performance page loads `perf.ini` through `bd_perf_policy_load()`, so the
profile names are the real ones, including `inherit`-only profiles. The app
rows are the effective `app.<id>.profile` keys from the loaded config. The
global row writes `perf.global_profile`; perfd does not read it yet.

The plugin page uses `bd_plugin_ui_settings()` and `bd_plugin_ui_draw()`. It
calls `bd_plugin_activate()` and `bd_plugin_set_settings_open(true)` on open,
focuses toggle/slider/select/button nodes with ↑↓, sends `ui.activate` on A,
and closes the settings page on B. `bd_plugin_ui_measure()` sizes the sub-page
frame to the declared tree (capped at the table viewport), and
`bd_plugin_ui_node_rect()` drives focus-following scroll: moving to a control
outside the table scrolls the tree until the whole control is visible. Plugin
controls keep the host renderer's own radii; only the settings frame follows
the square direction.

## Values and persistence

Every edit goes through `bd_config_set()` and then `bd_config_save()`; the
value is in `local.ini` as soon as the row changes. Brightness, volume and mute
are applied first through `bd_settings_system`; if that hook rejects the value,
the config is not changed. A missing hook is treated as success so the page
works on a host without a device backend.

The system stub (`bd_settings_system_stub_init`) records the last value and
call counts. M4 replaces it with the backlight and mixer implementations.

Accent changes rebuild the palette from `bd_config_accent_hex()` and repaint
the whole page, so the focus ring, swatches, toggles, sliders, icons and hint
bullet recolour immediately. Theme mode changes also rebuild the palette and
the light/dark page. The custom-colour sub-page edits R/G/B and writes
`#rrggbb` on every step.

The reset row opens a confirm dialog (default focus on 取消). Confirming writes
the schema defaults for static keys and saves, then re-applies brightness,
volume and mute through the system seam. Dynamic sections (`hotkey.*`,
`profile.*`, `app.*`, `core.*`, `device.*`) are deliberately left alone; the
dialog says so. `bd_config` still has no delete-override API, so reset writes
defaults rather than deleting `local.ini` lines.

## Controls

| input | action |
|---|---|
| ↑ / ↓ | move focus; scroll the table when needed |
| ← / → | toggle, step a slider, cycle an enum, cycle accent presets |
| hold ← / → | first step, then repeat after `input.repeat_delay_ms` and every `input.repeat_interval_ms` |
| A | toggle, open a sub-page/plugin, or confirm reset |
| B | leave the sub-page/plugin; close the page at the root |
| L1 / R1 | previous/next category (root only) |

The caller should use `bd_settings_hold(LEFT/RIGHT, true/false)` for the press
and release edges instead of sending the same action twice. `update(t)` drives
the repeat; `needs_frame_until()` returns the next repeat deadline, otherwise
the page is idle.

## Damage

The module queues its own damage and tracks dirty rows:

- focus change: the previous and next row only (about 110 KiB for two 74 px
  rows at 748 px wide);
- value change: the changed row only;
- scroll or plugin-node focus: the table only;
- hint show/hide: the 58 px bottom strip;
- accent/theme change, category switch, open/close or reset: the full page.

`settings_test` moves focus once and checks that the damage is two rows, then
captures the partial frame and forces a full redraw: the two images are
byte-identical. The table border is redrawn inside every row so the partial
repaint also restores the 1 px frame exactly.

## Integration sketch

```c
bd_settings_style style = {
    .dark = true,
    .accent_base = accent,          /* bd_config_accent_hex(..., dark) */
    .width = 1024,
    .height = 768,
};
bd_settings *settings = bd_settings_create(gfx, config, &style);
bd_settings_set_status(settings, &status);       /* clock, wifi, battery */
bd_settings_set_storage(settings, &storage);     /* statvfs values, optional */
bd_settings_set_system(settings, &system);       /* M4 backends */
bd_settings_set_plugins(settings, plugin_host);  /* optional */
bd_settings_open(settings, BD_SETTINGS_CATEGORY_APPEARANCE);

/* per frame */
bd_settings_update(settings, now_ms);
bd_settings_draw(settings, gfx);
uint64_t wake_at;
if (bd_settings_needs_frame_until(settings, &wake_at)) {
  /* block until min(wake_at, next input) */
}

/* input */
bd_settings_hold(settings, BD_SETTINGS_ACTION_RIGHT, pressed);
bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT);
```

The loop should send L1/R1 as `CATEGORY_PREV/NEXT` and map B to `CANCEL`.
When `bd_settings_input(CANCEL)` closes the page, the module has already queued
a full-screen damage; the loop should draw the home page next. Destroy the
settings object before the plugin host and the config it points at.

## Tests

- `settings_test`: schema metadata (including dynamic keys), page row keys,
  two-row focus damage, byte-identical partial/full frames, accent live
  recolour, toggle persistence, volume hold repeat (400 ms delay, 80 ms
  interval), `local.ini` contents and reload, brightness through the stub,
  hotkey conflict counts, perf/app profile cycling, background limits, reset
  confirm/cancel, custom accent sub-page, hint strip damage and B navigation.
  A long 16-control plugin tree verifies that focus-following scroll reaches
  and activates the last control.
- `settings_capture`: 15 screenshots (`settings_appearance`, `_hint`, `_sound`,
  `_display`, `_brightness`, `_keys`, `_performance`, `_background`,
  `_plugins`, `_plugin_settings`, `_plugin_long`, `_system`, `_confirm`,
  `_accent_coral`, `_light`). They are compared with `tools/imagediff.py` in
  tolerance mode (max delta 2, SSIM 0.9995), like the other screenshot tests.

Run:

```sh
BRICKDESK_WERROR=ON ./scripts/build-host.sh
ctest --test-dir build-host --output-on-failure
```

Update the screenshots after reviewing them:

```sh
scripts/update-baselines.sh settings
```

## Differences from v3 and open items

- the table is square with a 1 px border, not the reference's 12 px rounded
  card, following the later square-corner direction; row controls keep small
  radii, toggles 4 px and swatches circles.
- the bottom hint is hold-to-show only; there is no persistent Steam-style
  footer. The reference's static footer text is not drawn unless Select is
  held.
- the accent row has six preset swatches plus a custom RGB sub-page instead of
  the reference's static swatch strip; the custom colour has no automatic
  contrast guard.
- hotkey editing is not implemented; the list and conflicts are read-only.
- the plugin sub-page renders the host's tree, scrolls to the focused node and
  can send `ui.activate`, but does not persist plugin settings (the host has no
  settings storage yet).
- `perf.global_profile` is stored but perfd does not consume it; the settings
  page does not talk to the perfd socket.
- brightness/volume/mute use the stub; real backlight/mixer backends arrive in
  M4. The module is not wired into `src/main.c`, and nothing was validated on
  GE8300 hardware. Device items: real backlight/mixer behaviour, plugin start
  latency, the 13/14 px text at 3.95 inches, scroll behaviour with 64 hotkeys,
  and the reset write cost (all static keys) on the SD card. Plugin settings
  scrolling is covered on the host with a 16-control tree; deeply nested row
  layouts are the remaining edge case.
