#ifndef BRICKDESK_SHELL_H
#define BRICKDESK_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "brickdesk/app.h"
#include "brickdesk/config.h"
#include "brickdesk/desktop.h"
#include "brickdesk/gfx.h"
#include "brickdesk/platform.h"
#include "brickdesk/plugin.h"

/*
 * Desktop runtime: one event loop for SDL, inputd, the session manager and the
 * plugin host. The loop polls every pollable source once and otherwise blocks
 * until the nearest deadline (hotkey hold, plugin heartbeat, session timeout).
 */
typedef struct bd_shell bd_shell;

typedef enum bd_shell_animation_kind {
  BD_SHELL_ANIMATION_NONE = 0,
  BD_SHELL_ANIMATION_LAUNCH,
  BD_SHELL_ANIMATION_RETURN
} bd_shell_animation_kind;

/*
 * Placeholder for the M3 home-page transition. M2 records the launch/return
 * state and exposes its progress; it deliberately draws no zoom or fade yet.
 */
typedef struct bd_shell_animation {
  bd_shell_animation_kind kind;
  uint64_t started_ms;
  uint32_t duration_ms;
  float progress;
  char app_id[BD_APP_ID_MAX];
} bd_shell_animation;

typedef struct bd_shell_config {
  bd_platform *platform;
  bd_gfx *gfx;
  bd_desktop *desktop;
  const bd_config *settings;
  bd_plugin_host *plugins; /* optional, borrowed */
  const char *apps_dir;    /* NULL: BRICKDESK_APPS_DIR / build default */
  const char *input_socket; /* NULL: env, /run/brickdesk, then /tmp */
  const char *session_socket_dir;
  const char *session_frame_dir;
  const char *session_state_root;
  const char *menu_shim;    /* NULL: autodetect next to the binary */
  bool verbose;
} bd_shell_config;

/* Requires a live platform, renderer and desktop. Returns NULL on failure. */
bd_shell *bd_shell_create(const bd_shell_config *config);
void bd_shell_destroy(bd_shell *shell);

/*
 * Runs one loop turn: services, redraw when dirty, then one poll() over the
 * SDL wake pipe, inputd, session and plugin descriptors. timeout_ms caps the
 * wait; pass -1 to wait for the computed deadline. Returns 1 after work, 0 on
 * timeout, -1 when the shell should exit.
 */
int bd_shell_iterate(bd_shell *shell, int timeout_ms);
/* Runs the loop for at least duration_ms. Returns false when asked to quit. */
bool bd_shell_run_for(bd_shell *shell, int duration_ms);

/* Draws immediately when the desktop owns the screen; returns presented. */
bool bd_shell_render(bd_shell *shell);
bool bd_shell_visible(const bd_shell *shell);
bool bd_shell_quit_requested(const bd_shell *shell);
bool bd_shell_has_pending_images(const bd_shell *shell);
bool bd_shell_hint_visible(const bd_shell *shell);
uint64_t bd_shell_present_count(const bd_shell *shell);
const bd_shell_animation *bd_shell_animation_state(const bd_shell *shell);
/* "inputd" or "sdl" for startup logs. */
const char *bd_shell_input_source(const bd_shell *shell);

size_t bd_shell_app_count(const bd_shell *shell);
const bd_app *bd_shell_app_at(const bd_shell *shell, size_t index);

/*
 * Test hooks: the scenario harness feeds timestamped logical keys through the
 * same hotkey state machine even when a real inputd is present. Actions still
 * go through the single dispatch path used by the daemon and the SDL fallback.
 */
void bd_shell_test_key(bd_shell *shell, bd_key key, bool pressed,
                       uint64_t time_ms);
void bd_shell_test_advance(bd_shell *shell, uint64_t time_ms);

#endif /* BRICKDESK_SHELL_H */
