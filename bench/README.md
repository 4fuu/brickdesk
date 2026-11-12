# Renderer benchmark

```sh
./build-host/bench/bench_render --frames 120 --scenario all
```

Scenarios:

| name | what it draws |
|---|---|
| `full` | full-screen redraw of a 5,000-item wall plus floating bars |
| `focus` | two adjacent tiles change focus; only their damage is redrawn |
| `pan` | the wall origin moves 12/7 px per frame; the viewport is redrawn |
| `blur` | only the top/bottom blur strips are damaged and recomposited |

Host timings use Mesa llvmpipe and `glFinish()` after present, so they include
software rasterization and are only useful for relative comparisons. The
device (PowerVR GE8300) is the real budget reference. Each line reports average
and p95 frame time, damage pixels, draw calls, blur passes, visited/total wall
items and resident texture bytes.
