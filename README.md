# brickdesk

A small, low-overhead gamepad desktop for the TrimUI Brick Pro (Allwinner A133P,
PowerVR GE8300, 1024x768), written in C11 with SDL2 and GLES2.

This repository is at the M1 infrastructure stage: host build, SDL2/GLES2
platform layer, minimal renderer, screenshot/input-replay tests, and host CI.
The cover wall, text, themes and the device platform layer arrive in later
milestones.

## Requirements

Host build:

- CMake 3.16+, a C11 compiler (GCC 7.5 is the target compiler baseline)
- SDL2 development files (2.26.x or newer recommended)
- GLES2 headers and library (Mesa on the host)
- Python 3 with Pillow and numpy (screenshot tests)
- Ninja is optional

On Debian/Ubuntu:

```sh
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libsdl2-dev libegl1-mesa-dev libgles2-mesa-dev libgl1-mesa-dri \
  python3 python3-pil python3-numpy
```

## Build and run

```sh
./scripts/build-host.sh
./build-host/brickdesk --version
```

The host build defaults to `RelWithDebInfo`. To build with warnings as errors
(as CI does):

```sh
BRICKDESK_WERROR=ON ./scripts/build-host.sh
```

Headless run with Mesa llvmpipe:

```sh
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy BRICKDESK_HEADLESS=1 \
  ./build-host/brickdesk --verbose
```

At startup the program prints the GL vendor, renderer and version. `--verbose`
also prints the full extension list, for verifying the device driver once
hardware is available.

The main loop only draws when something changed and otherwise blocks in
`SDL_WaitEventTimeout`; it never runs a fixed 60 Hz redraw loop.

Host keyboard mapping (the device input layer will feed the same logical keys):

| Logical key | Host key |
|---|---|
| D-pad | arrow keys |
| A / B / X / Y | Z / X / C / V |
| L1 / R1 | Q / E |
| SELECT / START | Tab / Enter |
| MENU | Escape |

## Tests

```sh
ctest --test-dir build-host --output-on-failure
```

The suite includes:

- `key_names`: logical key name round-trips
- `smoke_render`: renders a 1024x768 colour + texture frame to
  `build-host/tests/artifacts/smoke.png`
- `version`, `idle_loop`: CLI and no-redraw-at-idle checks
- `scenario.boot`, `scenario.focus_move`: input replay + exact pixel compare
- `determinism_boot`: runs a scenario three times and requires identical pixels
- `imagediff_unit`: image comparison self-tests, including a 1 px shift

Screenshots are decoded RGBA and compared pixel-by-pixel; PNG bytes are never
compared. Failure artifacts (actual, baseline copy, diff) land under
`build-host/tests/artifacts/<scenario>/`.

Update baselines only after reviewing the images, in a separate commit:

```sh
./scripts/update-baselines.sh            # boot and focus_move
./scripts/update-baselines.sh boot       # one scenario
```

## Scenario format

`tests/harness` replays a small JSON scenario through the same logical-key path
as the application. Coordinates, screenshots and keyboard names are
deterministic; no SDL keycodes appear in scenarios.

```json
{
  "name": "focus_move",
  "size": [1024, 768],
  "fixed_dt_ms": 16,
  "steps": [
    { "type": "wait", "frames": 2 },
    { "type": "screenshot", "name": "focus_start" },
    { "type": "key", "key": "right", "action": "down" },
    { "type": "wait", "frames": 2 },
    { "type": "key", "key": "right", "action": "up" },
    { "type": "wait", "ms": 100 },
    { "type": "screenshot", "name": "focus_move" }
  ]
}
```

- `wait` advances the scenario clock by `frames` or by `ms` (converted with
  `fixed_dt_ms`; they are mutually exclusive).
- `key` injects a press or release for a logical key: `A`, `B`, `X`, `Y`, `L1`,
  `R1`, `SELECT`, `START`, `MENU`, `UP`, `DOWN`, `LEFT`, `RIGHT`. Names are
  case-insensitive.
- `screenshot` renders the current state and writes `<name>.png` into
  `--out-dir`; names must be unique and filesystem-safe.

Run the harness directly:

```sh
SDL_VIDEODRIVER=offscreen BRICKDESK_HEADLESS=1 \
  ./build-host/tests/bd_harness --scenario tests/scenarios/boot.json \
  --out-dir /tmp/brickdesk-shots
```

## Layout

```text
cmake/                 build options and generated version header input
include/brickdesk/     public headers (gfx, platform, input, desktop)
src/platform/          SDL2 window + GLES2 context (host_sdl.c also serves
                       target builds until the device layer lands)
src/gfx/               GLES2 renderer and framebuffer capture
src/app/               M1 placeholder desktop scene
src/input/             logical key names
tests/                 harness, scenarios, baselines, image diff self-test
tools/imagediff.py     exact/tolerance pixel comparison + diff images
third_party/           vendored stb and jsmn (licenses included)
```

## License

MIT. See `LICENSE`. Vendored third-party code keeps its own license notices;
see `third_party/README.md`.
