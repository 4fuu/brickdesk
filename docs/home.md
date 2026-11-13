# v3 home module

`src/home/` implements the v3 home page as a reusable module. It is wired into
the event loop by `src/app/shell.c`: scanned apps become wall items, A launches
through the session, background apps persist as running tiles, and START opens
the quick panel. The module owns the wall, focus, chrome overlays and
transitions, and talks to the renderer only through `include/brickdesk/gfx.h`.

## Files

| file | role |
|---|---|
| `include/brickdesk/home.h` | public API and item/style/status structs |
| `src/home/home.c` | layout, column index, focus, running apps, input, timing, damage |
| `src/home/home_draw.c` | wall tiles, live tile, top bar, hint bar, quick panel, dialog, launch/return |
| `src/home/home_internal.h` | shared state and layout constants |
| `tests/home_capture.c` | deterministic screenshots for the required states, plus hint and return |
| `tests/home_scale.c` | 5,000-item layout/traversal/damage/performance checks |

## Layout

- Four fixed rows, 188 px cells, 5 px gutters and 5 px corner radius; the wall
  grows to the right and never scrolls vertically.
- Every item is placed by the prototype's global first-fit scan (column first,
  then row), so later small tiles backfill earlier holes. Appending never moves
  an existing tile. The same ordered list always produces the same coordinates.
- Live items are forced to 2x2. Other spans are clamped to 1..3 x 1..2.
- Items are indexed by the wall columns they touch (`bd_home_query_indexed`),
  not by manifest order. A viewport plus one screen on each side examines only
  the items in about 16 columns, so draw/visit cost is independent of the total
  item count. `bd_home_scale` compares the optimized layout with a literal
  first-fit implementation for 5,000 items.
- The quick panel is a 402 px edge panel on the right: full screen height,
  flush to the screen edge, square outer corners and no outer margin. Its inner
  cards, thumbnails and selected row keep the wall's 5 px radius; text keeps
  16–18 px insets.

## Damage and time

Call `bd_home_update(home, now_ms)` once per frame, then `bd_home_draw()`. The
module queues its own damage:

- focus change: previous + next tile only (measured 76,048 px for two 1x1 tiles,
  including the 4 px alignment margin);
- pan: full viewport for the 220 ms animation;
- quick panel slide: only the moving panel strip; the scrim is constant while
  the panel is open;
- hint bar: bottom strip while it animates;
- launch: the first frame paints the scrimmed wall, then only the growing card;
  once the card is nearly full, the app view becomes the opaque backdrop and the
  card fades over it. The incremental result differs from a full redraw only at
  the card's antialiased border (373 px, max delta 34 on llvmpipe), which the
  scale test allows;
- return, dialog and the 180 ms tile close: full screen / tile as appropriate.

`bd_home_needs_frame_until()` returns the latest animation deadline, or
`now + 16 ms` while image decodes are pending. The event loop can block in
`SDL_WaitEventTimeout` with that timeout and otherwise stay idle.

## Running apps

`bd_home_set_running()` reads the app's last frame through the M1-06
`bd_app_last_frame_*` API, flips the bottom-first GL rows, expands RGBA8888 or
RGB565 to RGBA8, and owns the resulting texture until the app is cleared. A
running tile keeps only the top-right "运行中 · NN" badge (the top-left number
would duplicate it), keeps the bottom-left title and subtitle, and draws a
bottom-half scrim so arbitrary bright frames stay readable. It never reorders
the wall. `bd_home_start_return()` focuses/centres the tile and shrinks the last
frame into it over 420 ms.

`bd_home_confirm_dialog()` starts the 180 ms close animation and clears the
running entry when it finishes. The loop sees `BD_HOME_DIALOG_RESULT_CONFIRM`
and terminates the process through the session. The quick panel lists running
apps and supports resume and end.

The shell samples each running app's memory from `/proc`: `bd_sys_process_tree_rss_mb()`
sums `VmRSS` for the root pid and all descendants, so the displayed value is a
process-tree RSS, not a single-process estimate. `-1` means the tree could not
be sampled; the panel then shows "内存未知" and the aggregate bar is empty.
Tests and demos pass explicitly named fixture values; the launch-menu capture
sets `BRICKDESK_TEST_MEMORY_MB=132` so its screenshot text is deterministic,
while `tests/shell_quota_test.c` reads the real `/proc` value. A memory reading
is never an eviction input: `bd_session_suggest_eviction()` chooses the least
recently backgrounded app.

## Background quota

When the session already holds `session.max_background` apps, the shell opens
the "后台已满" choice instead of launching. It names the LRU app and the app
that would start; cancel keeps both, confirm terminates the LRU process and
retries the launch once the session drops it. If termination fails, the dialog
stays open with an error line, the pending launch is retained and the user can
retry or cancel; the pending launch is never retried automatically while the
failure is showing. `bd_home_open_background_full()` owns the dialog,
`bd_home_set_dialog_error()` reports the failure, and
`bd_home_take_dialog_result()` reports the choice.

## Animations

| animation | duration | easing |
|---|---:|---|
| wall pan | 220 ms | `cubic-bezier(.22, 1, .36, 1)` |
| launch open | 420 ms | same; app view cross-fades from 300 ms |
| return | 420 ms | same; scrim fades over the first 180 ms |
| quick panel | 200 ms | `cubic-bezier(.2, .7, .2, 1)` |
| hint bar | 200 ms | same |
| dialog | 180 ms | ease-out |
| tile close | 180 ms | scale 0.95 + fade |

All start functions use the timestamp from the last `bd_home_update()`, so call
update once before starting an animation.

## Theme and config

- The caller passes one accent base (from `bd_config_accent_hex()`), and the
  module derives the palette with `bd_accent_palette_from()`. The focus ring is
  a fixed warm white, not an accent, so it reads on any cover.
- The desktop is dark-only. `theme.mode` has one effective value, `dark`; old
  `light`/`auto` values in `local.ini` are normalized to `dark` on load. The
  quick panel has no theme switch.
- The top bar keeps category labels (18 px), Wi-Fi and battery status glyphs,
  the clock (16 px) and the text running count (13 px). The left project mark
  and "brickdesk" wordmark are both removed; there are no search/gear/avatar
  buttons. Wi-Fi/battery are drawn by the shared `bd_ui_draw_wifi()` /
  `bd_ui_draw_battery()` helpers in `src/app/ui.c`; settings and the quick
  panel use the same marks. When the platform exposes no battery or wireless
  interface, the glyph gets a slash and the quick panel says "未知"; the shell
  never substitutes 82% or an assumed connected state.
- Type sizes are CSS pixels. The bundled Noto SC subsets report a 1.448 em
  ascent+descent span, so the module loads each face at 1.448× the nominal size.
  13 px is the smallest face used by badges, meta values and keycaps.
- `bd_home_set_blur_enabled(false)` replaces the top/hint strip blur with plain
  gradients. Map this to `ui.blur` in the event loop.
- `bd_home_set_ambient_solid(true)` switches `theme.ambient=solid` to a flat
  backdrop; `baked` keeps the default vertical gradient. The shell applies both
  from the config change callback, so a settings edit repaints without a
  restart.
- The quick panel's "N / M" limit comes from `bd_home_status.max_background`;
  the shell passes `session.max_background` (default 3).
- Brightness and volume values come from `bd_home_quick_backend`: the shell
  reads the real `device_av` value, applies a 10% step, persists the confirmed
  read-back through config and returns it to the panel. A missing device shows
  "未知" and an adjust attempt does nothing; there is no software-only
  success. Wi-Fi/BT connection management is not wired this round, so the BT
  row reads "不可用" and neither control toggles a fake state.
- The quick panel has no persistent A/X/B footer. The hint bar, including its
  quick-panel actions, is drawn only while the Select hold action is active and
  is removed on release.
- `src/config/schema.c` and `config/brickdesk.ini` default
  `session.max_background` to 3; `ui.blur` defaults to true.

## Integration

```c
bd_home_style style = {
    .accent_base = accent,          /* bd_config_accent_hex(..., true) */
    .blur_enabled = bd_config_get_bool(settings, "ui.blur", true),
};
bd_home *home = bd_home_create(gfx, &style);
bd_home_set_items(home, items, count);
bd_home_set_running(home, id, last_frame_path, paused, mem_mb); /* -1 unknown */
bd_home_set_status(home, &status);   /* clock, wifi_known/wifi, battery, max */

/* per frame */
bd_home_update(home, now_ms);
bd_home_draw(home, gfx);
uint64_t wake_at;
if (bd_home_needs_frame_until(home, &wake_at)) {
  /* block until min(wake_at, next input) */
}

/* input */
bd_home_input(home, BD_HOME_ACTION_RIGHT);

/* hand-off */
if (bd_home_launch_complete(home)) {
  const char *id = bd_home_launch_target(home);
  /* hide the home and let the app draw */
}

/* what A would open, without starting the animation */
const char *id = bd_home_selected_id(home);

/* after bd_home_input(), act on a dialog choice */
bd_home_dialog_result choice = bd_home_take_dialog_result(home);
if (choice == BD_HOME_DIALOG_RESULT_CONFIRM) {
  /* terminate the app (or the LRU app) through the session */
}
```

`src/app/shell.c` is the reference caller: it maps keys to
`bd_home_action`, intercepts A to launch through the session first, hides the
home on `bd_home_launch_complete()`, starts the return on a session background
event, and feeds the dialog result back into the session.

## Known differences from the v3 prototype

- 3x1 titles use the 22 px face (v3 uses 23 px); the difference is a font-size
  detail, not a layout rule.
- The quick panel follows the newer edge-panel direction: 402 px wide, full
  height, flush right, square outline, 5 px inner cards. The v3 mockup used a
  394 px floating panel with a 16 px radius and an 8 px margin.
- The top bar drops the v3 four-square brand mark and the wordmark. It keeps
  category labels, Wi-Fi, battery, clock and text running count.
- Quick-panel icons are small axis-aligned primitives instead of the prototype's
  SVG set. Their hit areas and labels match.
- The ambient backdrop is a background gradient; the wall covers almost all of
  it. There is no per-frame realtime blur or full-screen FBO.
- Demo covers in `assets/covers/home-*.jpg` are 256x256 crops generated from the
  existing `prototype/bg` reference images (about 70 KiB total).

## Plugin tile

The production home appends the first tile-capable plugin as a custom-draw wall
item.  The item is passive: A never sends it through the session launch path.
Its callback calls `bd_plugin_set_tile_visible()` with the tile's viewport
state, so the plugin host starts it on demand when the tile becomes visible and
starts its idle-stop timer when it leaves the viewport or the home surface is
hidden.  Visible frames go through `bd_plugin_tile_begin_frame()`; a changed
frame/UI queues tile damage before `bd_plugin_tile_draw()`.  Off-screen items
are not drawn.

`tests/home_plugin_capture.c` scans the real `plugins/` directory and runs the
real `hello` plugin through this production home path.  It fills the wall so the
tile starts outside the viewport, asserts the plugin stays stopped, focuses the
tile to start it, and captures `home_plugin.png`.  It then checks that moving
the tile off-screen or hiding the home stops it again, and that the plugin never
enters the session.  It replaces the old `bd_desktop_set_plugin_tile()` path
rather than testing that legacy scene.
