#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "brickdesk/desktop.h"
#include "brickdesk/gfx.h"
#include "brickdesk/platform.h"
#include "brickdesk/plugin.h"
#include "brickdesk/theme.h"

#ifndef BRICKDESK_TEST_PLUGIN_DIR
#define BRICKDESK_TEST_PLUGIN_DIR "plugins"
#endif

static uint64_t now_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void remove_tree(const char *path) {
  DIR *directory = opendir(path);
  if (directory == NULL) {
    return;
  }
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 ||
        strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    char child[1024];
    snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    struct stat info;
    if (lstat(child, &info) == 0 && S_ISDIR(info.st_mode)) {
      remove_tree(child);
    } else {
      unlink(child);
    }
  }
  closedir(directory);
  rmdir(path);
}

static bool render_frame(bd_platform *platform, bd_gfx *gfx,
                         bd_desktop *desktop) {
  bd_gfx_begin_frame(gfx);
  bd_desktop_draw(desktop, gfx);
  bd_gfx_end_frame(gfx);
  bd_gfx_stats stats = bd_gfx_get_stats(gfx);
  if (stats.presented) {
    bd_platform_present(platform);
  }
  return stats.presented;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: bd_plugin_capture OUT_DIR\n");
    return 2;
  }
  char template[] = "/tmp/bd-plugin-capture-XXXXXX";
  char *runtime_dir = mkdtemp(template);
  if (runtime_dir == NULL) {
    fprintf(stderr, "plugin_capture: cannot create runtime dir\n");
    return 1;
  }

  bd_platform_config config;
  memset(&config, 0, sizeof(config));
  config.title = "brickdesk plugin capture";
  config.width = 1024;
  config.height = 768;
  config.hidden = true;
  bd_platform *platform = bd_platform_create(&config);
  if (platform == NULL) {
    remove_tree(runtime_dir);
    return 1;
  }
  bd_gfx *gfx = bd_gfx_create(1024, 768);
  if (gfx == NULL) {
    bd_platform_destroy(platform);
    remove_tree(runtime_dir);
    return 1;
  }

  bd_desktop_theme theme;
  theme.dark = true;
  theme.ambient_baked = true;
  theme.accent = bd_color_rgb(224, 108, 74);
  bd_desktop *desktop = bd_desktop_create(gfx, &theme);
  if (desktop == NULL) {
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    remove_tree(runtime_dir);
    return 1;
  }

  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.idle_stop_ms = 0;
  bd_plugin_host_config host_config;
  memset(&host_config, 0, sizeof(host_config));
  host_config.plugin_dir = BRICKDESK_TEST_PLUGIN_DIR;
  host_config.runtime_dir = runtime_dir;
  host_config.limits = &limits;
  host_config.display_width = 1024;
  host_config.display_height = 768;
  host_config.dark = true;
  host_config.accent = 0xe06c4a;
  host_config.locale = "zh-CN";
  bd_plugin_host *host = bd_plugin_host_create(&host_config);
  if (host == NULL) {
    bd_desktop_destroy(desktop);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    remove_tree(runtime_dir);
    return 1;
  }
  char error[256] = {0};
  if (bd_plugin_host_scan(host, error, sizeof(error)) < 0) {
    fprintf(stderr, "plugin_capture: %s\n", error);
  }
  bd_plugin *plugin = bd_plugin_host_find(host, "com.brickdesk.hello");
  if (plugin == NULL) {
    fprintf(stderr, "plugin_capture: com.brickdesk.hello was not discovered\n");
    for (size_t i = 0; i < bd_plugin_host_diagnostic_count(host); ++i) {
      char diagnostic[256];
      if (bd_plugin_host_diagnostic(host, i, diagnostic, sizeof(diagnostic))) {
        fprintf(stderr, "plugin_capture: %s\n", diagnostic);
      }
    }
    bd_desktop_destroy(desktop);
    bd_plugin_host_destroy(host);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    remove_tree(runtime_dir);
    return 1;
  }
  bd_desktop_set_plugin_tile(desktop, plugin);

  uint64_t deadline = now_ms() + 10000;
  int quiet = 0;
  bool ready = false;
  while (now_ms() < deadline) {
    bd_plugin_host_poll(host, 10);
    if (bd_plugin_ui_generation(plugin) > 0 &&
        bd_plugin_tile_seq(plugin) > 0) {
      if (bd_desktop_has_pending_images(desktop)) {
        quiet = 0;
      } else {
        ++quiet;
      }
    }
    if (quiet >= 3) {
      ready = true;
      break;
    }
  }
  if (!ready) {
    fprintf(stderr,
            "plugin_capture: plugin did not become ready (state=%s ui=%d seq=%u)\n",
            bd_plugin_state_name(bd_plugin_state_of(plugin)),
            bd_plugin_ui_generation(plugin), bd_plugin_tile_seq(plugin));
    bd_desktop_destroy(desktop);
    bd_plugin_host_destroy(host);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    remove_tree(runtime_dir);
    return 1;
  }

  for (int frame = 0; frame < 4; ++frame) {
    bd_plugin_host_poll(host, 0);
    bd_gfx_damage_all(gfx);
    render_frame(platform, gfx, desktop);
  }

  char path[2048];
  int written = snprintf(path, sizeof(path), "%s/plugin_hello.png", argv[1]);
  if (written < 0 || (size_t)written >= sizeof(path)) {
    fprintf(stderr, "plugin_capture: output path is too long\n");
    bd_desktop_destroy(desktop);
    bd_plugin_host_destroy(host);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    remove_tree(runtime_dir);
    return 1;
  }
  bool saved = bd_gfx_save_png(gfx, path);
  printf("plugin_capture: state=%s ui_generation=%d frame_seq=%u file=%s\n",
         bd_plugin_state_name(bd_plugin_state_of(plugin)),
         bd_plugin_ui_generation(plugin), bd_plugin_tile_seq(plugin), path);

  const bd_plugin_ui *settings = bd_plugin_ui_settings(plugin);
  bool settings_saved = false;
  if (settings != NULL) {
    bd_gfx_damage_all(gfx);
    bd_gfx_begin_frame(gfx);
    bd_desktop_draw(desktop, gfx);
    bd_desktop_draw_plugin_settings(desktop, gfx, settings);
    bd_gfx_end_frame(gfx);
    bd_platform_present(platform);
    int settings_written =
        snprintf(path, sizeof(path), "%s/plugin_settings.png", argv[1]);
    if (settings_written > 0 && (size_t)settings_written < sizeof(path)) {
      settings_saved = bd_gfx_save_png(gfx, path);
      printf("plugin_capture: settings file=%s\n", path);
    }
  }

  bd_desktop_destroy(desktop);
  bd_plugin_host_destroy(host);
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  remove_tree(runtime_dir);
  return saved && settings_saved ? 0 : 1;
}
