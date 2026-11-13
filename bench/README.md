# Renderer benchmark

```sh
./build-host/bench/bench_render --frames 120 --idle-ms 3000 --scenario all
./build-host/bench/bench_render --scenario all --json /tmp/bench.json \
  --budget bench/budgets.ini
```

Scenarios:

| name | what it draws |
|---|---|
| `idle` | settled desktop; no damage, no present, no frame clock |
| `focus` | two adjacent tiles change focus; only their damage is redrawn |
| `pan` | the wall origin moves 12/7 px per frame; the viewport is redrawn |
| `blur` | only the top/bottom blur strips are damaged and recomposited |
| `wall5000` | 5,000-item wall scrolls one cell per frame with full redraw |
| `plugin15` | 168x168 plugin tile upload and repaint at 15 fps |
| `settings` | desktop plus the plugin settings overlay |

`--json` writes the stable `brickdesk-bench-v1` document; `--budget` loads
`budgets.ini` and exits 3 when a deterministic limit is exceeded. See
`docs/performance.md` for field definitions, current host numbers, memory
accounting and device follow-ups.

Host timings use Mesa llvmpipe and `glFinish()`, so they include software
rasterization. They are useful for relative comparisons; the device
(PowerVR GE8300) is the frame-time reference. CI records them but does not fail
on wall-clock frame times.
