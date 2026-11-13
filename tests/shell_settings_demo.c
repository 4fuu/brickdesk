#define _XOPEN_SOURCE 700

#include "brickdesk/config.h"
#include "brickdesk/gfx.h"
#include "brickdesk/home.h"
#include "brickdesk/platform.h"
#include "brickdesk/settings.h"
#include "brickdesk/shell.h"
#include "settings_fixture.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool save_frame(bd_shell *shell, bd_gfx *gfx, const char *dir,
                       int *index, uint64_t *now) {
  bd_shell_test_advance(shell, *now);
  bd_shell_iterate(shell, 0);
  bd_shell_render(shell);
  char path[2048];
  int written = snprintf(path, sizeof(path), "%s/frame_%04d.png", dir, *index);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    return false;
  }
  if (!bd_gfx_save_png(gfx, path)) {
    return false;
  }
  ++*index;
  return true;
}

static bool wait_frames(bd_shell *shell, bd_gfx *gfx, const char *dir,
                        int *index, uint64_t *now, int frames) {
  for (int i = 0; i < frames; ++i) {
    *now += 50;
    if (!save_frame(shell, gfx, dir, index, now)) {
      return false;
    }
  }
  return true;
}

static bool key(bd_shell *shell, bd_gfx *gfx, const char *dir, int *index,
                uint64_t *now, bd_key pressed_key) {
  bd_shell_test_key(shell, pressed_key, true, *now);
  *now += 20;
  bd_shell_test_key(shell, pressed_key, false, *now);
  *now += 30;
  return wait_frames(shell, gfx, dir, index, now, 2);
}

static bool hold_key(bd_shell *shell, bd_gfx *gfx, const char *dir, int *index,
                     uint64_t *now, bd_key pressed_key, int frames) {
  bd_shell_test_key(shell, pressed_key, true, *now);
  bool ok = wait_frames(shell, gfx, dir, index, now, frames);
  bd_shell_test_key(shell, pressed_key, false, *now + 10);
  *now += 20;
  return ok && wait_frames(shell, gfx, dir, index, now, 3);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: bd_shell_settings_demo OUT_DIR\n");
    return 2;
  }
  const char *out_dir = argv[1];
  if (mkdir(out_dir, 0777) != 0 && errno != EEXIST) {
    fprintf(stderr, "shell_settings_demo: cannot create %s\n", out_dir);
    return 1;
  }
  char config_dir[512] = {0};
  if (!sf_make_config_dir(config_dir, sizeof(config_dir), false)) {
    fprintf(stderr, "shell_settings_demo: config dir\n");
    return 1;
  }
  char apps_dir[1024];
  char state_dir[1024];
  char sockets[1024];
  char frames[1024];
  snprintf(apps_dir, sizeof(apps_dir), "%s/apps", config_dir);
  snprintf(state_dir, sizeof(state_dir), "%s/state", config_dir);
  snprintf(sockets, sizeof(sockets), "%s/sockets", config_dir);
  snprintf(frames, sizeof(frames), "%s/frames", config_dir);
  mkdir(apps_dir, 0777);
  mkdir(state_dir, 0777);
  mkdir(sockets, 0777);
  mkdir(frames, 0777);

  bd_config_options options;
  memset(&options, 0, sizeof(options));
  options.config_dir = config_dir;
  options.migrate = false;
  char error[256] = {0};
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  if (config == NULL) {
    fprintf(stderr, "shell_settings_demo: config: %s\n", error);
    return 1;
  }
  bd_settings_system_stub stub;
  bd_settings_system system;
  bd_settings_system_stub_init(&stub, &system);
  stub.brightness = 70;
  stub.volume = 45;

  bd_platform_config platform_config;
  memset(&platform_config, 0, sizeof(platform_config));
  platform_config.title = "brickdesk settings demo";
  platform_config.width = 1024;
  platform_config.height = 768;
  platform_config.hidden = true;
  bd_platform *platform = bd_platform_create(&platform_config);
  bd_gfx *gfx = platform != NULL ? bd_gfx_create(1024, 768) : NULL;
  if (platform == NULL || gfx == NULL) {
    return 1;
  }

  bd_shell_config shell_config;
  memset(&shell_config, 0, sizeof(shell_config));
  shell_config.platform = platform;
  shell_config.gfx = gfx;
  shell_config.settings = config;
  shell_config.apps_dir = apps_dir;
  shell_config.session_socket_dir = sockets;
  shell_config.session_frame_dir = frames;
  shell_config.session_state_root = state_dir;
  shell_config.settings_system = &system;
  shell_config.verbose = false;
  bd_shell *shell = bd_shell_create(&shell_config);
  if (shell == NULL) {
    return 1;
  }
  bd_home *home = bd_shell_home(shell);
  bd_home_status status;
  memset(&status, 0, sizeof(status));
  status.clock = "22:26";
  status.wifi_known = true;
  status.wifi = true;
  status.battery_percent = 82;
  status.max_background = 3;
  bd_home_set_status(home, &status);

  int index = 0;
  uint64_t now = 1000;
  bool ok = wait_frames(shell, gfx, out_dir, &index, &now, 12);
  int settings_index = bd_home_find_item(home, "brickdesk-settings");
  if (!ok || settings_index < 0) {
    return 1;
  }
  for (int step = 0; step < 48 && ok; ++step) {
    if (bd_home_focus(home) == settings_index) {
      break;
    }
    int before = bd_home_focus(home);
    ok = key(shell, gfx, out_dir, &index, &now, BD_KEY_RIGHT);
    if (ok && bd_home_focus(home) == before) {
      ok = key(shell, gfx, out_dir, &index, &now, BD_KEY_DOWN);
    }
    if (ok && bd_home_focus(home) == before) {
      ok = key(shell, gfx, out_dir, &index, &now, BD_KEY_UP);
    }
  }
  if (!ok || bd_home_focus(home) != settings_index) {
    fprintf(stderr, "shell_settings_demo: cannot focus settings tile\n");
    return 1;
  }
  ok = wait_frames(shell, gfx, out_dir, &index, &now, 4) &&
       key(shell, gfx, out_dir, &index, &now, BD_KEY_A) &&
       wait_frames(shell, gfx, out_dir, &index, &now, 10);
  bd_shell_test_key(shell, BD_KEY_SELECT, true, now);
  ok = ok && wait_frames(shell, gfx, out_dir, &index, &now, 14);
  bd_shell_test_key(shell, BD_KEY_SELECT, false, now + 10);
  now += 30;
  ok = ok && wait_frames(shell, gfx, out_dir, &index, &now, 6);
  ok = ok && key(shell, gfx, out_dir, &index, &now, BD_KEY_R1);
  ok = ok && key(shell, gfx, out_dir, &index, &now, BD_KEY_R1);
  ok = ok && hold_key(shell, gfx, out_dir, &index, &now, BD_KEY_LEFT, 12);
  ok = ok && key(shell, gfx, out_dir, &index, &now, BD_KEY_B);
  ok = ok && wait_frames(shell, gfx, out_dir, &index, &now, 6);
  ok = ok && key(shell, gfx, out_dir, &index, &now, BD_KEY_A);
  ok = ok && wait_frames(shell, gfx, out_dir, &index, &now, 8);
  ok = ok && key(shell, gfx, out_dir, &index, &now, BD_KEY_RIGHT);
  ok = ok && key(shell, gfx, out_dir, &index, &now, BD_KEY_RIGHT);
  ok = ok && wait_frames(shell, gfx, out_dir, &index, &now, 8);
  ok = ok && key(shell, gfx, out_dir, &index, &now, BD_KEY_B);
  ok = ok && wait_frames(shell, gfx, out_dir, &index, &now, 12);

  printf("shell_settings_demo: frames=%d ok=%d\n", index, ok ? 1 : 0);
  bd_shell_destroy(shell);
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  bd_config_free(config);
  sf_remove_tree(config_dir);
  return ok ? 0 : 1;
}
