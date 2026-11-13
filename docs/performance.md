# Performance and memory

`bench/bench_render` is the single renderer benchmark for the desktop. It
drives real renderer, wall, font, image-cache and desktop code paths, emits a
stable JSON document for scripts, and prints a short human summary. It replaces
the old four-scenario loop in the same binary rather than adding a second bench
program. The same binary is cross-compiled for the Brick Pro and shipped in the
release package.

## Running

Build the host tree and run the budget test:

```sh
scripts/build-host.sh
ctest --test-dir build-host -R bench_budget --output-on-failure
```

Run a longer measurement and keep the JSON:

```sh
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy BRICKDESK_HEADLESS=1 \
  ./build-host/bench/bench_render \
  --frames 120 --idle-ms 3000 --scenario all \
  --json /tmp/brickdesk-bench.json
```

Or use the wrapper:

```sh
scripts/bench.sh --frames 120 --idle-ms 3000 --scenario all \
  --json /tmp/brickdesk-bench.json
```

Options:

| option | default | meaning |
|---|---|---|
| `--frames N` | 120 | measured frames per render scenario |
| `--idle-ms N` | 1000 | duration of the idle measurement |
| `--scenario NAME` | `all` | `idle`, `focus`, `pan`, `blur`, `wall5000`, `plugin15`, `settings`, or `all` |
| `--json PATH` | none | write the machine-readable result |
| `--budget PATH` | none | load `bench/budgets.ini` and exit 3 on a violation |

`--scenario full` is accepted as an alias for `wall5000`.

## Scenarios

| name | what it exercises |
|---|---|
| `idle` | settled desktop; no damage, no present, and no frame clock. Wakes only for real events. |
| `focus` | two adjacent covers change focus; only their damage is repainted. |
| `pan` | wall origin moves 12/7 px per frame; the whole viewport is repainted. |
| `blur` | only the floating top and bottom bars are damaged; both are recomposited. |
| `wall5000` | a 5,000-item wall scrolls a full cell vertically and repaints the viewport. |
| `plugin15` | a 168x168 RGBA plugin tile is uploaded and repainted once per iteration. |
| `settings` | desktop plus a declarative plugin settings panel; full-screen overlay composition. |

`plugin15` measures the host side of a dynamic tile: texture upload, damage and
repaint. It deliberately does not start a plugin process or decode images, so
IPC and plugin CPU are outside this number.

## perfd socket

`brickdesk-perfd` listens on a Unix socket (`$BRICKDESK_PERF_SOCKET`, default
`/run/brickdesk/perfd.sock`) and speaks one JSON object per line. The desktop
uses `bd_perf_client_set_global()` from `include/brickdesk/perf_client.h`,
which performs a bounded, non-blocking connect/send/receive round trip and
returns a typed error instead of blocking the event loop.

The settings page's global profile row sends:

```json
{"jsonrpc":"2.0","id":1,"method":"perf.global","profile":"performance"}
```

`perf.global` validates the profile, rejects the request while an app lease
owns the effective profile, sets the daemon's global profile, applies it
through the normal transaction path and returns the confirmed plan. A missing
daemon, an unknown profile, an active lease or a failed sysfs write is reported
back; the settings page then leaves `perf.global_profile` unchanged and shows
the error. `perf.apply`, `perf.set`, `perf.restore`, `perf.get`, `perf.list`,
`perf.capabilities` and `perf.ping` are unchanged.

## Reading the JSON

The schema is `"schema": "brickdesk-bench-v1"`, `"schema_version": 1`. New
fields may be added, but existing names and meanings are stable. One object is
written per selected scenario:

```json
{
  "name": "focus",
  "frames": 60,
  "presented": 60,
  "frame_ms": {"avg": 4.739, "p50": 4.680, "p95": 4.974, "max": 6.268},
  "draw_calls": {"avg": 43.00, "max": 43, "total": 2580},
  "repainted_pixels": {"avg": 61248, "max": 61248, "total": 3674880},
  "blur_passes": {"avg": 0.00, "max": 0, "total": 0},
  "wall": {"visited_items_max": 42, "visible_items_max": 30, "total_items": 5000},
  "idle": {"duration_ms": 0, "cpu_ms": 0.000},
  "memory": {
    "rss_peak_kb": 245488,
    "rss_steady_kb": 245488,
    "peak_reset": false,
    "texture_cache_bytes": 2355984,
    "texture_cache_budget_bytes": 100663296,
    "glyph_atlas_bytes": 4194304,
    "font_data_bytes": 7189592,
    "fbo_bytes": 3538944
  }
}
```

| field | meaning |
|---|---|
| `frame_ms` | wall time from `begin_frame` through `end_frame`, present and `glFinish()`, in milliseconds. Includes llvmpipe rasterization on the host. |
| `draw_calls` | per-frame `glDraw*` calls for scene, blur and composite; the final full-screen present copy is not included. |
| `repainted_pixels` | area covered by merged damage rects, not unique pixels; overlapping bars can make this larger than the screen. |
| `blur_passes` | downsample + two separable blur passes + composite draws in the frame. Each bar normally contributes 5. |
| `wall.visited_items_max` | largest number of indexed wall entries examined while building the viewport/prefetch set. |
| `wall.visible_items_max` | entries that intersected the viewport. |
| `rss_peak_kb`, `rss_steady_kb` | `VmHWM` and `VmRSS` from `/proc/self/status`. `clear_refs` resets the peak per scenario where supported; otherwise the peak includes the process lifetime or is the maximum sampled RSS (`peak_reset` says which). |
| `texture_cache_bytes` | resident decoded cover textures in the image cache. |
| `texture_cache_budget_bytes` | configured cache limit. The bench uses the default `ui.cover_cache_mb = 96`. |
| `glyph_atlas_bytes` | allocated font atlas textures (source space, not reserved cell padding). |
| `font_data_bytes` | unique font file bytes held by the renderer, shared by all sizes of the same file. |
| `fbo_bytes` | persistent scene FBO plus the two blur attachments, as RGBA8 logical bytes. |

Host frame times include `glFinish()` on llvmpipe. They are useful for relative
comparisons and regressions, but CI does not fail on them. The device
(PowerVR GE8300) is the frame-time reference.

## Current host numbers

Measured in the orb on 2026-11-13: x86_64, Mesa 22.3.6, llvmpipe
(LLVM 15.0.6), `--frames 60 --idle-ms 3000`. Numbers vary with host load;
p95 is more useful than a single run.

| scenario | avg ms | p50 ms | p95 ms | max ms | draws avg | damage px | blur | visited | RSS steady KiB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| idle | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 239,584 |
| focus | 4.739 | 4.680 | 4.974 | 6.268 | 43.0 | 61,248 | 0 | 42/5000 | 245,488 |
| pan | 28.863 | 27.200 | 43.334 | 44.805 | 53.5 | 786,432 | 10 | 72/5000 | 245,552 |
| blur | 8.349 | 8.227 | 9.234 | 9.991 | 94.0 | 113,664 | 10 | 42/5000 | 245,552 |
| wall5000 | 28.037 | 26.433 | 42.626 | 42.987 | 45.3 | 786,432 | 10 | 49/5000 | 245,552 |
| plugin15 | 4.295 | 4.201 | 4.912 | 6.484 | 46.0 | 28,896 | 0 | n/a | 244,780 |
| settings | 44.197 | 43.509 | 47.997 | 52.492 | 68.0 | 786,432 | 10 | n/a | 244,780 |

Idle measured `presents=0` and `cpu_ms=0.000` over 3,000 ms. The `pan` p95
spike is host scheduling/llvmpipe noise, not a stable device expectation.

## v3 home module (M3-A)

The v3 home page is not wired into `brickdesk` yet, so `bench_render` still
measures the placeholder desktop. `tests/home_scale` is a deterministic
correctness test that also prints host timings; it builds a 5,000-item wall and
drives the same draw/damage paths the event loop will use.

Measured in the orb with `SDL_VIDEODRIVER=offscreen`, Mesa llvmpipe,
2026-11-13:

| metric | value |
|---|---:|
| 5,000-item layout build | 53-74 ms (one-time, startup) |
| query items in viewport + one screen each side | 31 max, 27.1 average |
| drawn-frame visited / visible items | 35 / 14 max |
| focus repaint (two 1x1 tiles, one damage rect) | 76,048 px |
| wall pan frame | 32-41 ms average, 45-85 ms max |
| launch frame | 18-20 ms average, 40-68 ms max |
| resident cover textures | 15 / 6.4 MiB (96 MiB budget) |

The launch frame cost is after the incremental launch painter was added: only
the first frame paints the scrimmed wall, later frames repaint the card and the
opaque app view underneath it. The incremental image differs from a full redraw
only at the card's antialiased border (373 px, max channel delta 34), which the
scale test allows. Host frame times include llvmpipe; the GE8300 is the device
reference and these numbers are not a device budget.

## Budgets

`bench/budgets.ini` contains deterministic host limits. `bench_budget` runs
`ctest` with `--budget` and fails on any violation; it is part of the existing
host workflow because the workflow runs the full `ctest` suite.

| budget | value | why |
|---|---:|---|
| `idle_frames_max` | 0 | a settled desktop must not swap a frame |
| `idle_cpu_ms_max` | 100 ms / 3 s | allows a few wakeups; measured 0 ms after the host wait fix |
| `focus_repainted_pixels_max` | 80,000 px | two 168 px tiles plus 4 px alignment is 61,248 px today |
| `wall5000_visited_items_max` | 96 items | viewport plus one-cell prefetch visits 42-72; catches manifest-sized scans |
| `rss_peak_kb_max` | 327,680 KiB (320 MiB) | host desktop peaks at 235-245 MiB, dominated by llvmpipe; guards large leaks/caches |
| `texture_cache_slack_bytes` | 0 | renderer accounting must stay inside its configured budget |

RSS is a host regression guard, not a device memory budget. Set the device
ceiling after measuring the real binary with the default configuration.

## Where host RSS goes

Idle desktop with the default configuration: about 235-245 MiB RSS. Most of it
is the host GL stack, not brickdesk:

| region | approx KiB | notes |
|---|---:|---|
| `libLLVM-15.so.1` code/rodata | 122,872 | llvmpipe JIT compiler |
| `swrast_dri.so` code/rodata | 24,032 | Mesa software rasterizer |
| `libz3.so.4` code/rodata | 22,004 | LLVM dependency |
| Mesa/LLVM anonymous mappings and JIT | ~21,000 | tile buffers, shader JIT |
| glibc heap | ~18,400 | app and driver allocations; includes renderer below |
| other libraries, stack, page tables | ~10,000 | SDL2, libc, etc. |

Renderer-owned logical bytes in the same idle run:

| allocation | bytes | KiB |
|---|---:|---:|
| unique font file data | 7,189,592 | 7,021 |
| glyph atlases (4 x 512² + 1 x 1024², L8) | 2,097,152 | 2,048 |
| scene FBO (1024 x 768 RGBA8) | 3,145,728 | 3,072 |
| blur attachments (2 x 256 x 192 RGBA8) | 393,216 | 384 |
| resident cover textures (14 desktop items) | 2,243,088 | 2,190 |
| **renderer total** | **15,068,776** | **14,715** |

The remaining heap is mostly Mesa/SDL allocations and allocator overhead, not
duplicate app-owned images.

### Fonts

All five `bd_font` objects are loaded eagerly at startup: title (Noto Sans SC
700, 20 px), body (Noto Sans SC 500, 18 px), badge (Noto Sans SC 500, 13 px),
serif (Noto Serif SC 500, 34 px) and Latin (Inter 500, 16 px). The optional
full CJK fallback is not present in `assets/fonts`, so it is not loaded.

The font files on disk total 6.9 MiB. Before M1-13 the body and badge fonts
each read their own copy of `NotoSansSC-500-subset.ttf`, so the renderer held
9,310,968 bytes. Font files are now reference-counted by path: the two sizes
share the immutable bytes, and `bd_gfx_memory_stats.font_data_bytes` reports
7,189,592 bytes. That is 2,121,376 bytes (2.02 MiB) removed from the renderer.
The atlases stay separate because glyphs are rasterized at different pixel
sizes; they are included in `glyph_atlas_bytes`.

### Cover textures and decode buffers

Cover images are decoded and bilinearly resized on the renderer worker thread.
The decoded result buffer is freed in `bd_gfx_image_poll()` immediately after
`glTexImage2D`/`glTexSubImage2D`; the cache retains only the GL texture and its
logical byte count, not a second CPU copy. The result queue is capped at 8, so
startup can transiently hold up to roughly 4.5 MiB of 384x384 RGBA results plus
one in-flight source decode. That memory is gone once the visible set settles.

Entries are keyed by path and target size. The same source can therefore be
decoded at two sizes (for example a 3x2 hero and a 1x1 tile), which is required
to avoid upscaling and is not a duplicate of the same texture. Idle resident
bytes are 2.2 MiB against the default 96 MiB budget.

## Fixes made in M1-13

1. **Shared font file data.** `bd_font_file` is reference-counted per path.
   Saved 2,121,376 bytes of renderer-owned font data; `tests/smoke_render.c`
   asserts that a second size shares the bytes and that both are released.
   Five `brickdesk --idle-check` runs gave 229.5 MiB (old) versus 227.9 MiB
   (new) median maximum RSS; run-to-run llvmpipe noise is about +/-3 MiB, so
   total RSS cannot resolve a 2 MiB change, but the renderer accounting can.
2. **Blur dependency is a real intersection.** The damage finalizer used
   `bd_rect_overlaps_or_touches(..., 0)`, so a damage rect ending exactly on a
   floating bar's edge recomposited the whole bar. It now requires a non-empty
   intersection. A 168 px tile above the bottom bar no longer drags in a
   5-pass blur: `plugin15` damage fell from 153,216 to 28,896 px, blur passes
   from 5 to 0, and the same 20-frame all-scenario run fell from 8.300 ms to
   4.504 ms average on llvmpipe.
3. **Host offscreen idle wait.** SDL's `offscreen` video driver spins inside
   `SDL_WaitEventTimeout`; `brickdesk --idle-check 4000` burned 2,990 ms of
   process CPU while presenting 0 frames. The host platform layer now sleeps
   and polls only for that driver. The same idle check reports 0.000 ms CPU
   and 0 presents. Device fbdev/mali paths still use SDL's blocking wait.

## Performance policy additions (WP-22)

- `auto` profile: `schedutil` governors the CPU; perfd re-evaluates the battery
  every 5 seconds. On battery at or below 10%, every profile (including an
  explicit performance lease) is capped at the highest available step
  <= 816 MHz. Plugging in or recovering above 10% lifts the cap on the next
  apply.
- Custom frequency steps are `perf.ini` profiles: `min_khz`/`max_khz` are
  clamped to `cpuinfo_min_freq`/`cpuinfo_max_freq` and snapped down to an
  available `scaling_available_frequencies` entry. Add as many
  `[profile.<name>]` sections as needed; the settings page cycles them.
- Multiple `cpufreq/policy*` directories are counted and reported by
  `perf.get`/capabilities. TG4040/A133 exposes one cluster (`policy0`), so the
  transaction writes the selected policy; if a future kernel exposes more, the
  probe still reports the count but only the selected policy is controlled.
- CPU/GPU temperatures come from `/sys/class/thermal/thermal_zone*`, classified
  by the zone `type` string (`cpu`/`soc` vs `gpu`). The settings System page
  shows each rounded °C reading only when the zone exists; on a host without
  thermal zones the rows are omitted, not filled with a fake value.
- Low-battery capping is applied after profile resolution and before the sysfs
  transaction, so a failed API write rolls back to the previous frequency.

## Device measurements to take first

`bench_render` is cross-compiled for aarch64 and shipped in the release package
as `/mnt/SDCARD/brickdesk/bin/bench_render`; the package also ships
`assets/` so the baked `BRICKDESK_ASSET_DIR` resolves. The target workflow adds
`bench_render` to its `GLIBC <= 2.23` check. `scripts/device/bench.sh` runs the
installed binary and pulls its JSON; the JSON schema and scenario names are the
same as on the host.

On the real unit, measure in this order:

1. **Idle**: `--scenario idle --idle-ms 3000`. Expect 0 presents. Sample CPU
   with `scripts/device/perf.sh -n 10 -i 1`; idle CPU should be near zero.
2. **Focus / pan / blur** at 60 frames. Record avg/p50/p95 and draw calls.
   Compare against 4.7 / 28.9 / 8.3 ms host numbers only as a relative signal.
3. **wall5000**: confirm visited items stays around 42-72, not 5,000, and that
   texture cache stays below 96 MiB.
4. **plugin15**: once the plugin host is included, add IPC and real plugin CPU;
   the host-side upload/repaint number is a lower bound.
5. **settings**: full-screen overlay composition; record p95 and temperature.
6. **Memory**: `VmRSS` and `VmHWM` from `/proc/<pid>/status`, plus the JSON
   renderer accounting. Set the device RSS ceiling from these numbers, not from
   the host 320 MiB guard.
7. **Thermals**: run the pan/wall5000 loop for 5 minutes with
   `scripts/device/perf.sh` sampling frequency and temperature. The host cannot
   model PowerVR DVFS or thermal throttling.

`bench_render` calls `glFinish()` so its numbers include GPU completion. For
present latency on the device, cross-check `display_probe`'s `swap_ms` rather
than treating bench frame time as vsync-to-photon latency.

## CI behavior

- The host workflow builds with `-DBRICKDESK_WERROR=ON` and runs `ctest`.
- `bench_budget` runs `bench_render --scenario all --frames 20 --idle-ms 3000`
  with `bench/budgets.ini` and writes `tests/artifacts/bench.json`.
- `bench_smoke` remains a fast smoke test with 8 frames and a 500 ms idle.
- Wall-clock frame times are recorded but never gate CI.
- The target workflow cross-builds `bench_render`, checks it against
  `GLIBC <= 2.23`, and packages it with the release zip.
