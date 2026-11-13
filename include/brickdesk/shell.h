#ifndef BRICKDESK_SHELL_H
#define BRICKDESK_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "brickdesk/app.h"
#include "brickdesk/config.h"
#include "brickdesk/gfx.h"
#include "brickdesk/home.h"
#include "brickdesk/platform.h"
#include "brickdesk/plugin.h"
#include "brickdesk/settings.h"

/*
 * Desktop runtime: one event loop for SDL, inputd, the session manager, the
 * plugin host and the v3 home page. The loop polls every pollable source once
 * and otherwise blocks until the nearest deadline (hotkey hold, plugin
 * heartbeat, session timeout, home animation or image decode).
 */
typedef struct bd_shell bd_shell;

typedef struct bd_shell_config {
  bd_platform *platform;
  bd_gfx *gfx;
  bd_config *settings;
  bd_plugin_host *plugins; /* optional, borrowed */
  const char *apps_dir;    /* NULL: BRICKDESK_APPS_DIR / build default */
  const char *input_socket; /* NULL: env, /run/brickdesk, then /tmp */
  const char *session_socket_dir;
  const char *session_frame_dir;
  const char *session_state_root;
  const char *menu_shim;    /* NULL: autodetect next to the binary */
  /* Optional device-test roots; NULL uses the real /sys, /dev/snd and /proc. */
  const char *battery_root;
  const char *battery_name;
  const char *av_sysfs_root;
  const char *av_snd_root;
  const char *av_proc_cards;
  const char *av_speaker_mute;
  /*
   * Explicit test/capture backend.  Production leaves this NULL and the shell
   * probes device_av; a missing device is reported as unavailable, never as a
   * silent stub success.
   */
  const bd_settings_system *settings_system;
  bool verbose;
  /* Attach the first tile-capable plugin to the production home wall.
   * Production main enables it; the idle-check path leaves it off so the
   * measurement stays at zero presents. */
  bool attach_plugin_tile;
} bd_shell_config;

/* Requires a live platform, renderer and config. Returns NULL on failure. */
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
/* "inputd" or "sdl" for startup logs. */
const char *bd_shell_input_source(const bd_shell *shell);

size_t bd_shell_app_count(const bd_shell *shell);
const bd_app *bd_shell_app_at(const bd_shell *shell, size_t index);
/* Borrowed home page; settings, status and theme changes go through it. */
bd_home *bd_shell_home(bd_shell *shell);
/* Borrowed settings page and the home->settings entry/exit path. */
bd_settings *bd_shell_settings(bd_shell *shell);
bool bd_shell_settings_visible(const bd_shell *shell);
bool bd_shell_open_settings(bd_shell *shell, int category);

/*
 * Test hooks: the scenario harness feeds timestamped logical keys through the
 * same hotkey state machine even when a real inputd is present. Actions still
 * go through the single dispatch path used by the daemon and the SDL fallback.
 */
void bd_shell_test_key(bd_shell *shell, bd_key key, bool pressed,
                       uint64_t time_ms);
void bd_shell_test_advance(bd_shell *shell, uint64_t time_ms);
/* Test-only access to the live session and the same launch path as A. */
bd_session *bd_shell_test_session(bd_shell *shell);
bool bd_shell_test_launch(bd_shell *shell, const char *app_id);

#endif /* BRICKDESK_SHELL_H */
