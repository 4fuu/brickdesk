#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brickdesk/config.h"
#include "brickdesk/gfx.h"
#include "brickdesk/platform.h"
#include "brickdesk/plugin.h"
#include "brickdesk/settings.h"
#include "brickdesk/theme.h"
#include "settings_fixture.h"

#ifndef BRICKDESK_TEST_PLUGIN_DIR
#define BRICKDESK_TEST_PLUGIN_DIR "plugins"
#endif

static bool save(bd_gfx *gfx, const char *dir, const char *name) {
  char path[2048];
  int written = snprintf(path, sizeof(path), "%s/%s.png", dir, name);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    fprintf(stderr, "settings_capture: path too long for %s\n", name);
    return false;
  }
  if (!bd_gfx_save_png(gfx, path)) {
    fprintf(stderr, "settings_capture: save %s failed: %s\n", path,
            bd_gfx_last_error(gfx));
    return false;
  }
  printf("capture: %s\n", path);
  return true;
}

static bool capture(bd_platform *platform, bd_gfx *gfx, bd_settings *settings,
                    const char *dir, const char *name, uint64_t now) {
  bd_gfx_begin_frame(gfx);
  bd_settings_update(settings, now);
  bd_settings_damage(settings);
  bd_settings_draw(settings, gfx);
  bd_gfx_end_frame(gfx);
  if (bd_gfx_get_stats(gfx).presented) {
    bd_platform_present(platform);
  }
  return save(gfx, dir, name);
}

static bool open_page(bd_settings *settings, int category) {
  if (!bd_settings_open(settings, category)) {
    fprintf(stderr, "settings_capture: cannot open category %d\n", category);
    return false;
  }
  return true;
}

static bool focus_key(bd_settings *settings, const char *key) {
  int index = -1;
  for (int i = 0; i < bd_settings_row_count(settings); ++i) {
    const char *candidate = bd_settings_row_key(settings, i);
    if (candidate != NULL && strcmp(candidate, key) == 0) {
      index = i;
      break;
    }
  }
  if (index < 0) {
    fprintf(stderr, "settings_capture: row %s not found\n", key);
    return false;
  }
  while (bd_settings_focus(settings) < index) {
    bd_settings_input(settings, BD_SETTINGS_ACTION_DOWN);
  }
  while (bd_settings_focus(settings) > index) {
    bd_settings_input(settings, BD_SETTINGS_ACTION_UP);
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: bd_settings_capture OUT_DIR\n");
    return 2;
  }
  const char *out_dir = argv[1];

  char config_dir[1024] = {0};
  if (!sf_make_config_dir(config_dir, sizeof(config_dir), true)) {
    fprintf(stderr, "settings_capture: cannot create config dir\n");
    return 1;
  }
  char runtime_dir[] = "/tmp/bd-settings-runtime-XXXXXX";
  if (mkdtemp(runtime_dir) == NULL) {
    fprintf(stderr, "settings_capture: cannot create runtime dir\n");
    sf_remove_tree(config_dir);
    return 1;
  }

  bd_config_options options;
  memset(&options, 0, sizeof(options));
  options.config_dir = config_dir;
  options.migrate = false;
  char error[256] = {0};
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  if (config == NULL) {
    fprintf(stderr, "settings_capture: config: %s\n", error);
    sf_remove_tree(runtime_dir);
    sf_remove_tree(config_dir);
    return 1;
  }

  bd_platform_config platform_config;
  memset(&platform_config, 0, sizeof(platform_config));
  platform_config.title = "brickdesk settings capture";
  platform_config.width = 1024;
  platform_config.height = 768;
  platform_config.hidden = true;
  bd_platform *platform = bd_platform_create(&platform_config);
  bd_gfx *gfx = platform != NULL ? bd_gfx_create(1024, 768) : NULL;
  if (platform == NULL || gfx == NULL) {
    bd_config_free(config);
    sf_remove_tree(runtime_dir);
    sf_remove_tree(config_dir);
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
  host_config.accent = 0xe07a5f;
  host_config.locale = "zh-CN";
  bd_plugin_host *host = bd_plugin_host_create(&host_config);
  if (host == NULL) {
    fprintf(stderr, "settings_capture: plugin host create failed\n");
  } else if (bd_plugin_host_scan(host, error, sizeof(error)) < 0) {
    fprintf(stderr, "settings_capture: plugin scan: %s\n", error);
  }
  bd_plugin *hello = host != NULL ? bd_plugin_host_find(host, "com.brickdesk.hello")
                                  : NULL;
  bool hello_ready = false;
  if (hello != NULL) {
    bd_plugin_activate(hello);
    uint64_t deadline = sf_now_ms() + 10000;
    while (sf_now_ms() < deadline) {
      bd_plugin_host_poll(host, 20);
      if (bd_plugin_ui_settings(hello) != NULL) {
        hello_ready = true;
        break;
      }
    }
  }
  if (!hello_ready) {
    fprintf(stderr, "settings_capture: hello plugin settings UI not ready\n");
  }

  bd_settings_style style;
  memset(&style, 0, sizeof(style));
  style.dark = true;
  bd_color accent;
  if (!bd_color_from_hex(bd_config_accent_hex("persimmon", true), &accent)) {
    accent = bd_color_rgb(224, 122, 95);
  }
  style.accent_base = accent;
  style.width = 1024;
  style.height = 768;
  bd_settings *settings = bd_settings_create(gfx, config, &style);
  if (settings == NULL) {
    fprintf(stderr, "settings_capture: settings create failed\n");
    if (host != NULL) {
      bd_plugin_host_destroy(host);
    }
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    bd_config_free(config);
    sf_remove_tree(runtime_dir);
    sf_remove_tree(config_dir);
    return 1;
  }
  bd_settings_status status;
  memset(&status, 0, sizeof(status));
  status.clock = "22:26";
  status.wifi = true;
  status.battery_percent = 82;
  bd_settings_set_status(settings, &status);
  bd_settings_storage storage = {58 * 1024, 12 * 1024};
  bd_settings_set_storage(settings, &storage);
  bd_settings_system_stub stub;
  bd_settings_system system;
  bd_settings_system_stub_init(&stub, &system);
  bd_settings_set_system(settings, &system);
  if (host != NULL) {
    bd_settings_set_plugins(settings, host);
  }

  bool ok = true;
  uint64_t now = 1000;

  ok = open_page(settings, BD_SETTINGS_CATEGORY_APPEARANCE) &&
       focus_key(settings, "theme.accent") &&
       capture(platform, gfx, settings, out_dir, "settings_appearance", now) &&
       ok;
  now += 16;
  bd_settings_set_hint_visible(settings, true);
  ok = capture(platform, gfx, settings, out_dir, "settings_hint", now) && ok;
  bd_settings_set_hint_visible(settings, false);
  now += 16;

  ok = open_page(settings, BD_SETTINGS_CATEGORY_SOUND) &&
       capture(platform, gfx, settings, out_dir, "settings_sound", now) && ok;
  now += 16;
  ok = open_page(settings, BD_SETTINGS_CATEGORY_DISPLAY) &&
       capture(platform, gfx, settings, out_dir, "settings_display", now) && ok;
  now += 16;
  bd_settings_update(settings, now);
  bd_settings_hold(settings, BD_SETTINGS_ACTION_RIGHT, true);
  bd_settings_update(settings, now + 400);
  bd_settings_hold(settings, BD_SETTINGS_ACTION_RIGHT, false);
  now += 416;
  ok = capture(platform, gfx, settings, out_dir, "settings_brightness", now) &&
       ok;
  now += 16;

  ok = open_page(settings, BD_SETTINGS_CATEGORY_KEYS) &&
       capture(platform, gfx, settings, out_dir, "settings_keys", now) && ok;
  now += 16;
  ok = open_page(settings, BD_SETTINGS_CATEGORY_PERFORMANCE) &&
       capture(platform, gfx, settings, out_dir, "settings_performance", now) &&
       ok;
  now += 16;
  ok = open_page(settings, BD_SETTINGS_CATEGORY_BACKGROUND) &&
       capture(platform, gfx, settings, out_dir, "settings_background", now) &&
       ok;
  now += 16;

  ok = open_page(settings, BD_SETTINGS_CATEGORY_PLUGINS) &&
       capture(platform, gfx, settings, out_dir, "settings_plugins", now) && ok;
  now += 16;
  if (hello_ready && focus_key(settings, "!plugin.com.brickdesk.hello")) {
    if (bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT)) {
      for (int i = 0; i < 5; ++i) {
        bd_plugin_host_poll(host, 20);
      }
      bd_settings_refresh(settings);
      ok = capture(platform, gfx, settings, out_dir,
                   "settings_plugin_settings", now) &&
           ok;
    } else {
      fprintf(stderr, "settings_capture: cannot open hello settings\n");
      ok = false;
    }
  } else {
    ok = false;
  }
  now += 16;

  ok = open_page(settings, BD_SETTINGS_CATEGORY_SYSTEM) &&
       capture(platform, gfx, settings, out_dir, "settings_system", now) && ok;
  now += 16;
  if (focus_key(settings, "!reset") &&
      bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT)) {
    ok = capture(platform, gfx, settings, out_dir, "settings_confirm", now) &&
         ok;
  } else {
    ok = false;
  }
  now += 16;

  ok = open_page(settings, BD_SETTINGS_CATEGORY_APPEARANCE) &&
       focus_key(settings, "theme.accent") &&
       bd_settings_input(settings, BD_SETTINGS_ACTION_RIGHT) &&
       capture(platform, gfx, settings, out_dir, "settings_accent_coral", now) &&
       ok;
  now += 16;
  ok = focus_key(settings, "theme.accent") &&
       bd_settings_input(settings, BD_SETTINGS_ACTION_LEFT) &&
       focus_key(settings, "theme.mode") &&
       bd_settings_input(settings, BD_SETTINGS_ACTION_RIGHT) &&
       capture(platform, gfx, settings, out_dir, "settings_light", now) && ok;
  now += 16;
  (void)bd_settings_input(settings, BD_SETTINGS_ACTION_LEFT); /* dark again */
  now += 16;

  {
    char long_dir[2048] = {0};
    char long_runtime[] = "/tmp/bd-settings-long-XXXXXX";
    if (sf_make_fixture_plugin(long_dir, sizeof(long_dir), "long") &&
        mkdtemp(long_runtime) != NULL) {
      bd_plugin_host_config long_config = host_config;
      long_config.plugin_dir = long_dir;
      long_config.runtime_dir = long_runtime;
      bd_plugin_host *long_host = bd_plugin_host_create(&long_config);
      bd_plugin *long_plugin = NULL;
      if (long_host != NULL) {
        (void)bd_plugin_host_scan(long_host, error, sizeof(error));
        long_plugin = bd_plugin_host_find(long_host, "com.test.long");
      }
      bool long_ready = false;
      if (long_plugin != NULL) {
        bd_plugin_activate(long_plugin);
        uint64_t deadline = sf_now_ms() + 5000;
        while (sf_now_ms() < deadline) {
          bd_plugin_host_poll(long_host, 20);
          if (bd_plugin_ui_settings(long_plugin) != NULL) {
            long_ready = true;
            break;
          }
        }
      }
      if (long_host != NULL && long_ready) {
        bd_settings_set_plugins(settings, long_host);
        bool opened = open_page(settings, BD_SETTINGS_CATEGORY_PLUGINS) &&
                      focus_key(settings, "!plugin.com.test.long") &&
                      bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT);
        for (int i = 0; opened && i < 30; ++i) {
          bd_settings_input(settings, BD_SETTINGS_ACTION_DOWN);
        }
        ok = opened &&
             capture(platform, gfx, settings, out_dir,
                     "settings_plugin_long", now) &&
             ok;
        bd_settings_set_plugins(settings, NULL);
      } else {
        fprintf(stderr, "settings_capture: long plugin not ready\n");
        ok = false;
      }
      if (long_host != NULL) {
        bd_plugin_host_stop_all(long_host);
        bd_plugin_host_destroy(long_host);
      }
      sf_remove_tree(long_runtime);
      sf_remove_tree(long_dir);
    } else {
      fprintf(stderr, "settings_capture: cannot create long plugin\n");
      ok = false;
    }
  }

  bd_settings_destroy(settings);
  if (host != NULL) {
    bd_plugin_host_stop_all(host);
    bd_plugin_host_destroy(host);
  }
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  bd_config_free(config);
  sf_remove_tree(runtime_dir);
  sf_remove_tree(config_dir);
  return ok ? 0 : 1;
}
