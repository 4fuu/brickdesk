# In-app menu shim (M1-09)

`libbrickdesk-menu.so` is the mode-A in-app menu from
[architecture.md](research/architecture.md) section 5.4. It is preloaded into
the foreground application, hooks the frame-present call, and draws a GLES2
overlay with the application's own context. The render thread blocks inside the
hook while the menu is visible; no new window, EGL surface or GL context is
created, so opening the menu cannot lose the application's context.

The shim never starts a thread and never draws while the menu is closed. The
closed path is one monotonic clock read per present and a non-blocking inputd
read at most every 50 ms.

| File | Responsibility |
|---|---|
| `src/menu/shim.c` | LD_PRELOAD hooks, open/close lifecycle, input handling |
| `src/menu/render.c` | GLES2 renderer, GL state save/restore, last-frame readback |
| `src/menu/clients.c` | inputd, perfd and session menu clients |
| `include/brickdesk/menu.h` | brightness/volume backend interface and test hooks |
| `tests/menu_test.c` | end-to-end session/inputd test and screenshot capture |
| `tests/menu_state_app.c` | GL state and pixel restoration probe |

## 1. Which path is used

The session injects `BRICKDESK_MENU_MODE` from `app.json` (`menu.mode`, default
`overlay`). The shim only serves `overlay`; all other values leave it inert.

| Path | Chosen when | Behaviour |
|---|---|---|
| A. Overlay shim | `menu.mode=overlay`, the app is dynamically linked to SDL2/EGL, the shim resolves a real present function, a GLES2 context is current, and inputd grants the grab | The shim draws in-process and blocks the render thread. Used for the demo and for third-party SDL2/GLES2 games. |
| C. Pause + display handoff | `menu.mode=handoff`, the app is statically linked or resolves its own present/GL pointers, uses raw fb/DRM, or the overlay grab fails | The session sends `app.pause`/`app.display_release`; the desktop's handoff menu owns the screen. If the app cannot rebuild its context, save state and restart it on resume. If it cannot pause, `SIGSTOP` is the fallback, but `SIGSTOP` does not release the EGL surface: the device probe (`tools/display_probe.c`) must establish whether a second surface can present while the app holds one. M1-09 wires the mode decision and the session pause path; the `brickdesk --menu` UI process itself belongs to desktop integration, so an app that needs C before that lands effectively falls back to D. |
| D. No menu | Neither A nor C applies (static/raw-fb app without protocol support, app owns its input grab, etc.) | inputd performs only system actions (volume, brightness, return-home) and the UI must say so. |

`BRICKDESK_MENU_DISABLE=1` forces D at runtime for one application. A crash or
black screen should be recorded by the session and turn into a C/D launch next
time; M1 does not implement the blacklist file yet.

## 2. Hook selection

The library exports four interposable symbols and resolves the real function
with `dlsym(RTLD_NEXT, ...)` on first use:

1. `SDL_GL_SwapWindow` is the primary hook for SDL2 + OpenGL/OpenGL ES apps.
2. `SDL_RenderPresent` is hooked for `SDL_Renderer` apps. It only draws when
   `SDL_GL_GetCurrentContext()` is non-NULL; software renderers pass through.
3. `eglSwapBuffers` covers apps that use EGL directly.
4. `eglSwapBuffersWithDamageKHR` covers apps that use the damage extension.

A thread-local `in_present` guard prevents a frame from being overlaid twice:
SDL calls into EGL internally, and SDL_RenderPresent calls into
SDL_GL_SwapWindow. The outer hook owns the frame; inner hooks call the real
function and return. A second thread that swaps while the menu is open blocks
on the menu mutex until the menu closes, so only one thread owns the overlay.

The library has no link-time dependency on SDL, EGL or GLES. The present and GL
entry points come from the host process, which keeps `LD_PRELOAD` working for
apps that use only one of the paths. The target workflow checks this with
`readelf -d`.

## 3. Open and close

```text
inputd: Menu hold 600 ms -> action menu.open (phase down)
shim:  send {"cmd":"grab"} and wait for the daemon grab acknowledgement
shim:  read the default framebuffer, write BRICKDESK_LAST_FRAME
shim:  copy the application frame into a background texture
shim:  save GL state, draw the menu, present once
shim:  block in the menu loop, reading key/action lines from inputd
shim:  on close draw the background once and present it
shim:  send {"cmd":"release_all"} then {"cmd":"ungrab"}
```

If the grab is refused (for example another process owns the devices), the
overlay does not open; the frame is presented normally. This avoids a menu that
cannot be controlled. If inputd disappears while the menu is open, the loop
exits and the application resumes.

The menu is a toggle while open: a second `menu.open` down edge, `B`, `MENU`, or
selecting **resume** closes it. `A` activates the focused row; `UP`/`DOWN` move;
`LEFT`/`RIGHT` adjust brightness, volume or the performance profile.

## 4. Items

| Item | Action |
|---|---|
| 继续游戏 | Close the overlay and return to the application. |
| 返回桌面 | Send `background` on the session menu socket, close the overlay, and let the desktop call `bd_session_background()`. |
| 退出应用 | Send `quit` on the session menu socket; the desktop calls `bd_session_terminate()`, which runs the declared save hook before the quit hook. If the session socket is unavailable the shim falls back to `SIGTERM` after closing the overlay. |
| 亮度 | `bd_menu_system_ops.brightness_get/set`; the M1 backend is an in-memory stub. |
| 音量 | `bd_menu_system_ops.volume_get/set`; the M1 backend is an in-memory stub. |
| 性能模式 | `perf.list` gives the names, `perf.get` gives the current one, and `perf.apply` changes it. The menu never invents profile names. The perfd connection stays open while the app runs because the applied profile is a lease owned by that connection. |

Brightness and volume are deliberately behind
`include/brickdesk/menu.h` so M4 can install the real backlight and ALSA
backends without touching the shim. `bd_menu_set_system_ops()` copies a
`bd_menu_system_ops` table; `NULL` restores the stub. The session's
`BRICKDESK_*` environment is the only input; the shim does not read app files
or perform plugin calls.

Background and quit are reported to the session through the menu control
socket described in [app-protocol.md](app-protocol.md). The shim closes the
overlay before the desktop acts so the application can process
`app.display_release`/`app.pause`/`app.quit` from its own event loop. Calling
those hooks while the render thread is still blocked in the overlay would
deadlock until the pause timeout.

## 5. Visual style

The overlay is drawn in top-left 1024x768 design coordinates and scaled by
`min(width/1024, height/768)`; the 1024x768 device is 1:1. It follows the dark
tokens from design.md: warm ink surfaces, a 62% scrim over the game frame, the
persimmon accent from `bd_config_accent_hex()`/`theme.accent`, 20 px panel
corners, 12 px row corners and the Noto SC subset for Chinese. The app's frame
is the background; there is no live blur.

## 6. GL state

`bd_menu_gl_create()` saves the application state before touching GL and
`bd_menu_gl_destroy()` restores it before deleting the menu objects. Covered
values:

- viewport, scissor box and scissor test;
- framebuffer binding (drawing always targets the default framebuffer);
- current program;
- array and element buffer bindings;
- every vertex attribute's enabled flag, buffer, size, type, normalized flag,
  stride and pointer (up to the driver's attribute count, capped at 8);
- active texture and the texture binding of unit 0 (other units are untouched);
- blend enable, source/destination RGB+alpha factors and equations;
- depth, cull and stencil tests;
- clear colour, unpack and pack alignment.

The background is captured with `glReadPixels` and uploaded with
`glTexImage2D`. `glCopyTexImage2D` is not used because it is unreliable on
multisampled/single-buffered EGL configurations; the readback costs one frame
only when the menu opens.

## 7. Environment

| Variable | Meaning |
|---|---|
| `BRICKDESK_MENU_MODE` | `overlay`, `embedded`, `handoff`, `none`; only `overlay` activates the shim. |
| `BRICKDESK_MENU_DISABLE` | `1` disables the overlay in this process. |
| `BRICKDESK_INPUT_SOCKET` | inputd socket; defaults to `/run/brickdesk/inputd.sock`, then `/tmp/brickdesk-inputd.sock`. |
| `BRICKDESK_MENU_SOCKET` | Session menu control socket; injected by `bd_session`. |
| `BRICKDESK_PERF_SOCKET` | perfd socket; defaults to `/run/brickdesk/perfd.sock`, then `/tmp/brickdesk-perfd.sock`. |
| `BRICKDESK_LAST_FRAME` | Last-frame file written when the menu opens. |
| `BRICKDESK_CONFIG_DIR` | Configuration directory; the build-time directory is the fallback. |
| `BRICKDESK_ASSET_DIR` | Font directory root; defaults to `/mnt/SDCARD/brickdesk/assets`, then the build-time directory. |
| `BRICKDESK_APP_ID` | Sent with menu actions; injected by the session. |
| `BRICKDESK_MENU_DEBUG` | Logs lifecycle decisions to stderr. |
| `BRICKDESK_MENU_CAPTURE` | Test-only directory; every menu redraw writes `menu-NNNN.png`. |

The desktop adds `LD_PRELOAD=<libbrickdesk-menu.so>` and the input/config/perf
socket variables above when an app's `menu.mode` is `overlay`. It resolves the
library next to `brickdesk` (host build), through `BRICKDESK_MENU_SHIM`, or from
the staged `lib/` directory on the device. Other menu modes launch without the
preload; their handoff UI is not part of M2-E.

## 8. Last frame

On open the shim reads the default framebuffer and publishes it with
`bd_app_last_frame_write()` using the M1-06 header (RGBA8888, 1024x768 on the
device, `frame_seq` incremented). The row order is the raw
`glReadPixels` order (bottom row first), matching the demo and the existing
format. The desktop can show this frame while the app is paused or backgrounded.

## 9. Tests

`tests/menu_test.c` runs three phases:

1. `apps/demo` under `bd_session` with `libbrickdesk-menu.so` preloaded and a
   real `brickdesk-inputd --replay` stream. It checks the menu grab/ungrab,
   the background/resume path, the menu control action, the saved state, and
   the last-frame header, and copies `menu_open.png`/`menu_focus.png`.
2. A second fresh demo run selects **quit** and checks the session save/quit
   path and exit status.
3. `tests/menu_state_app.c` opens and closes the menu, then compares the full
   GL snapshot and the rendered pixels against the pre-menu state.

The screenshots are compared by `tests/run_menu_test.cmake` with the same
tolerance as the other host screenshots (max delta 1, 0.1% of pixels, SSIM
0.9999, cluster 4). Regenerate them with:

```sh
scripts/update-baselines.sh menu
```

The demo accepts `BRICKDESK_DEMO_WIDTH`, `BRICKDESK_DEMO_HEIGHT` and
`BRICKDESK_DEMO_FIXED_TIME_MS` so the test background is deterministic.

The inputd replay path applies each replay event at its scheduled timestamp and
advances pending hold timers to that timestamp before the event. Without that
ordering a late poll could stamp a 600 ms Menu hold with the poll time and let
the release at 600 ms cancel the action (see `docs/input.md`).

## 10. Deviations and open items

- The mode-C menu UI process (`brickdesk --menu`) is not part of M1-09. The
  shim detects that it has no usable hook and stays inert; the session can
  already pause or SIGSTOP the app, but there is no second menu renderer yet.
  Until desktop integration adds it, a non-overlay app gets mode D.
- Application probing (`ldd` for libSDL2/libEGL), a first-run blacklist and the
  crash-to-C fallback are not implemented. `menu.mode` in app.json is
  authoritative; `BRICKDESK_MENU_DISABLE=1` is the manual escape hatch.
- Brightness and volume are the in-memory M1 stubs. The interface is ready for
  the M4 backlight/ALSA backends.
- The shim does not pause application audio while the menu is open; audio
  threads keep running until the app's own pause hook runs after the menu
  closes.
- The last frame follows the demo's row order (raw `glReadPixels`, bottom row
  first). The format itself does not declare an orientation.
- If a game dynamic-loads EGL with `RTLD_LOCAL`, `dlsym(RTLD_NEXT)` may not see
  `eglSwapBuffers`; SDL apps still use the SDL hook. That case is mode C/D.

## 11. Device confirmation checklist

These cannot be answered on host:

- The real Menu key code and the 600 ms hold threshold; inputd's device map
  must deliver `action=menu.open` on the Brick Pro.
- `EVIOCGRAB`/ungrab while the game owns the device, and whether any game's
  own input thread stays stuck after the release.
- PowerVR's GLES2 state queries and attribute-pointer restore; especially
  `GL_VERTEX_ATTRIB_ARRAY_POINTER` after a game used client-side pointers.
- Whether the game reads back the framebuffer immediately after a swap (the
  overlay would contaminate that read).
- Context recreation after `app.display_release` for apps other than the demo.
- perfd socket permissions and the profile list on the real sysfs tree.
- The M4 backlight/ALSA backends and the real `bd_menu_system_ops` values.
- The 1024x768 EGL drawable size and colour format; the shim scales from the
  queried drawable and falls back to 1024x768.

Known limits, also listed in architecture.md 5.4: apps that static-link
SDL/EGL or call a dynamically loaded present pointer are not hookable; apps
that grab input or use raw fb/DRM are mode C/D; a game with multiple GL
contexts must swap on the thread the shim sees; audio threads keep running
while the menu is open; the menu does not pause audio on its own.
