# v3 home module

`src/home/` implements the v3 home page as a reusable module. It is not wired
into `src/main.c` yet; the event-loop task owns that hand-off. The module owns
the wall, focus, chrome overlays and transitions, and talks to the renderer only
through `include/brickdesk/gfx.h`.

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
RGB565 to RGBA8, and owns the resulting texture until the app is cleared. The
tile keeps its normal top-left number badge, bottom-left bold title and
subtitle, and adds a top-right "running" badge. It never reorders the wall.
`bd_home_start_return()` focuses/centres the tile and shrinks the last frame into
it over 420 ms.

`bd_home_confirm_dialog()` starts the 180 ms close animation and clears the
running entry when it finishes. The quick panel lists running apps with memory
estimates and supports resume and end.

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
- The wall stays dark in both themes. The light theme changes the chrome,
  quick panel and dialog. Its top bar is deliberately a warm translucent paper
  with a softer fade than the near-white v3 reference.
- `bd_home_set_blur_enabled(false)` replaces the top/hint strip blur with plain
  gradients. Map this to `ui.blur` in the event loop.
- The quick panel's "N / M" limit comes from `bd_home_status.max_background`;
  pass `session.max_background` (default 3).
- `src/config/schema.c` and `config/brickdesk.ini` now default
  `session.max_background` to 3. A new `ui.blur` bool defaults to true.

## Integration

```c
bd_home_style style = {
    .accent_base = accent,          /* bd_config_accent_hex(..., dark) */
    .dark = dark,
    .blur_enabled = bd_config_get_bool(settings, "ui.blur", true),
};
bd_home *home = bd_home_create(gfx, &style);
bd_home_set_items(home, items, count);
bd_home_set_running(home, id, last_frame_path, paused, mem_mb);
bd_home_set_status(home, &status);   /* clock, wifi, battery, max background */

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
```

## Known differences from the v3 prototype

- 3x1 titles use the 22 px face (v3 uses 23 px); the difference is a font-size
  detail, not a layout rule.
- Quick-panel icons are small axis-aligned primitives instead of the prototype's
  SVG set. Their hit areas and labels match.
- The ambient backdrop is a background gradient; the wall covers almost all of
  it. There is no per-frame realtime blur or full-screen FBO.
- Demo covers in `assets/covers/home-*.jpg` are 256x256 crops generated from the
  existing `prototype/bg` reference images (about 70 KiB total).
- The light top bar is intentionally softer/darker than `v3-home-light.jpg`.
