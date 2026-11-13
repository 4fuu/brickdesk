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
#include "settings_fixture.h"

static bool save_frame(bd_platform *platform, bd_gfx *gfx,
                       bd_settings *settings, const char *dir, int index,
                       uint64_t now) {
  bd_gfx_begin_frame(gfx);
  bd_settings_update(settings, now);
  bd_settings_damage(settings);
  bd_settings_draw(settings, gfx);
  bd_gfx_end_frame(gfx);
  if (bd_gfx_get_stats(gfx).presented) {
    bd_platform_present(platform);
  }
  char path[2048];
  int written = snprintf(path, sizeof(path), "%s/frame_%04d.png", dir, index);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    return false;
  }
  return bd_gfx_save_png(gfx, path);
}

static bool advance(bd_platform *platform, bd_gfx *gfx, bd_settings *settings,
                    const char *dir, int *index, uint64_t *now, int frames) {
  for (int i = 0; i < frames; ++i) {
    *now += 50;
    if (!save_frame(platform, gfx, settings, dir, *index, *now)) {
      return false;
    }
    ++*index;
  }
  return true;
}

static bool focus_key(bd_settings *settings, const char *key) {
  for (int i = 0; i < bd_settings_row_count(settings); ++i) {
    const char *candidate = bd_settings_row_key(settings, i);
    if (candidate != NULL && strcmp(candidate, key) == 0) {
      while (bd_settings_focus(settings) < i) {
        bd_settings_input(settings, BD_SETTINGS_ACTION_DOWN);
      }
      while (bd_settings_focus(settings) > i) {
        bd_settings_input(settings, BD_SETTINGS_ACTION_UP);
      }
      return true;
    }
  }
  return false;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: bd_settings_demo OUT_DIR\n");
    return 2;
  }
  const char *out_dir = argv[1];
  char config_dir[1024] = {0};
  if (!sf_make_config_dir(config_dir, sizeof(config_dir), true)) {
    fprintf(stderr, "settings_demo: cannot create config dir\n");
    return 1;
  }
  char plugin_dir[2048] = {0};
  if (!sf_make_fixture_plugin(plugin_dir, sizeof(plugin_dir), "long")) {
    fprintf(stderr, "settings_demo: cannot create plugin dir\n");
    sf_remove_tree(config_dir);
    return 1;
  }
  char runtime[] = "/tmp/bd-settings-demo-runtime-XXXXXX";
  if (mkdtemp(runtime) == NULL) {
    fprintf(stderr, "settings_demo: cannot create runtime\n");
    sf_remove_tree(plugin_dir);
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
    fprintf(stderr, "settings_demo: config: %s\n", error);
    sf_remove_tree(runtime);
    sf_remove_tree(plugin_dir);
    sf_remove_tree(config_dir);
    return 1;
  }
  bd_platform_config platform_config;
  memset(&platform_config, 0, sizeof(platform_config));
  platform_config.title = "brickdesk settings demo";
  platform_config.width = 1024;
  platform_config.height = 768;
  platform_config.hidden = true;
  bd_platform *platform = bd_platform_create(&platform_config);
  bd_gfx *gfx = platform != NULL ? bd_gfx_create(1024, 768) : NULL;
  if (platform == NULL || gfx == NULL) {
    bd_config_free(config);
    sf_remove_tree(runtime);
    sf_remove_tree(plugin_dir);
    sf_remove_tree(config_dir);
    return 1;
  }

  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.idle_stop_ms = 0;
  bd_plugin_host_config host_config;
  memset(&host_config, 0, sizeof(host_config));
  host_config.plugin_dir = plugin_dir;
  host_config.runtime_dir = runtime;
  host_config.limits = &limits;
  host_config.display_width = 1024;
  host_config.display_height = 768;
  host_config.dark = true;
  host_config.accent = 0xe07a5f;
  host_config.locale = "zh-CN";
  bd_plugin_host *host = bd_plugin_host_create(&host_config);
  bd_plugin *long_plugin = NULL;
  if (host != NULL) {
    (void)bd_plugin_host_scan(host, error, sizeof(error));
    long_plugin = bd_plugin_host_find(host, "com.test.long");
  }
  bool plugin_ready = false;
  if (long_plugin != NULL) {
    bd_plugin_activate(long_plugin);
    uint64_t deadline = sf_now_ms() + 5000;
    while (sf_now_ms() < deadline) {
      bd_plugin_host_poll(host, 20);
      if (bd_plugin_ui_settings(long_plugin) != NULL) {
        plugin_ready = true;
        break;
      }
    }
  }
  if (!plugin_ready) {
    fprintf(stderr, "settings_demo: long plugin not ready\n");
  }

  bd_settings_style style;
  memset(&style, 0, sizeof(style));
  bd_color accent;
  if (!bd_color_from_hex(bd_config_accent_hex("persimmon", true), &accent)) {
    accent = bd_color_rgb(224, 122, 95);
  }
  style.accent_base = accent;
  style.width = 1024;
  style.height = 768;
  bd_settings *settings = bd_settings_create(gfx, config, &style);
  if (settings == NULL) {
    fprintf(stderr, "settings_demo: settings create failed\n");
    if (host != NULL) {
      bd_plugin_host_destroy(host);
    }
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    bd_config_free(config);
    sf_remove_tree(runtime);
    sf_remove_tree(plugin_dir);
    sf_remove_tree(config_dir);
    return 1;
  }
  bd_settings_system_stub stub;
  bd_settings_system system;
  bd_settings_system_stub_init(&stub, &system);
  bd_settings_set_system(settings, &system);
  bd_settings_status status;
  memset(&status, 0, sizeof(status));
  status.clock = "22:26";
  status.wifi = true;
  status.battery_percent = 82;
  bd_settings_set_status(settings, &status);
  bd_settings_storage storage = {58 * 1024, 12 * 1024};
  bd_settings_set_storage(settings, &storage);
  if (host != NULL) {
    bd_settings_set_plugins(settings, host);
  }

  int index = 0;
  uint64_t now = 1000;
  bool ok = bd_settings_open(settings, BD_SETTINGS_CATEGORY_APPEARANCE) &&
            focus_key(settings, "theme.accent") &&
            advance(platform, gfx, settings, out_dir, &index, &now, 10);
  bd_settings_set_hint_visible(settings, true);
  ok = ok && advance(platform, gfx, settings, out_dir, &index, &now, 18);
  bd_settings_set_hint_visible(settings, false);
  ok = ok && advance(platform, gfx, settings, out_dir, &index, &now, 6);

  ok = ok && bd_settings_input(settings, BD_SETTINGS_ACTION_CATEGORY_NEXT) &&
       advance(platform, gfx, settings, out_dir, &index, &now, 6);
  bd_settings_hold(settings, BD_SETTINGS_ACTION_RIGHT, true);
  ok = ok && advance(platform, gfx, settings, out_dir, &index, &now, 18);
  bd_settings_hold(settings, BD_SETTINGS_ACTION_RIGHT, false);
  ok = ok && advance(platform, gfx, settings, out_dir, &index, &now, 4);

  ok = ok && bd_settings_input(settings, BD_SETTINGS_ACTION_CATEGORY_NEXT) &&
       advance(platform, gfx, settings, out_dir, &index, &now, 6);
  bd_settings_hold(settings, BD_SETTINGS_ACTION_LEFT, true);
  ok = ok && advance(platform, gfx, settings, out_dir, &index, &now, 14);
  bd_settings_hold(settings, BD_SETTINGS_ACTION_LEFT, false);
  ok = ok && advance(platform, gfx, settings, out_dir, &index, &now, 4);

  ok = ok && bd_settings_input(settings, BD_SETTINGS_ACTION_CATEGORY_NEXT) &&
       advance(platform, gfx, settings, out_dir, &index, &now, 10);
  ok = ok && bd_settings_input(settings, BD_SETTINGS_ACTION_CATEGORY_NEXT) &&
       advance(platform, gfx, settings, out_dir, &index, &now, 6);
  ok = ok && bd_settings_input(settings, BD_SETTINGS_ACTION_CATEGORY_NEXT) &&
       advance(platform, gfx, settings, out_dir, &index, &now, 6);
  ok = ok && bd_settings_input(settings, BD_SETTINGS_ACTION_CATEGORY_NEXT) &&
       advance(platform, gfx, settings, out_dir, &index, &now, 6);

  if (plugin_ready && focus_key(settings, "!plugin.com.test.long")) {
    ok = ok && advance(platform, gfx, settings, out_dir, &index, &now, 4);
    ok = ok && bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT) &&
         advance(platform, gfx, settings, out_dir, &index, &now, 5);
    for (int i = 0; i < 20; ++i) {
      bd_settings_input(settings, BD_SETTINGS_ACTION_DOWN);
      ok = ok && advance(platform, gfx, settings, out_dir, &index, &now, 2);
    }
    ok = ok && advance(platform, gfx, settings, out_dir, &index, &now, 8);
    ok = ok && bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT) &&
         advance(platform, gfx, settings, out_dir, &index, &now, 12);
  } else {
    fprintf(stderr, "settings_demo: long plugin page not reached\n");
    ok = false;
  }

  printf("settings_demo: frames=%d plugin_ready=%d\n", index, plugin_ready);
  bd_settings_destroy(settings);
  if (host != NULL) {
    bd_plugin_host_stop_all(host);
    bd_plugin_host_destroy(host);
  }
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  bd_config_free(config);
  sf_remove_tree(runtime);
  sf_remove_tree(plugin_dir);
  sf_remove_tree(config_dir);
  return ok ? 0 : 1;
}
