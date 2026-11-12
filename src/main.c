#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "brickdesk/config.h"
#include "brickdesk/desktop.h"
#include "brickdesk/gfx.h"
#include "brickdesk/input.h"
#include "brickdesk/platform.h"
#include "brickdesk/plugin.h"
#include "brickdesk/theme.h"
#include "brickdesk/version.h"

static void print_usage(FILE *stream) {
  fprintf(stream,
          "usage: brickdesk [options]\n"
          "\n"
          "  --version        print version and exit\n"
          "  --verbose        print GL extensions at startup\n"
          "  --idle-check MS  block for MS milliseconds and report redraws\n"
          "  --config-dir DIR load configuration from DIR\n"
          "  -h, --help       show this help\n");
}

static double process_cpu_ms(void) {
  return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

static void render_frame(bd_platform *platform, bd_gfx *gfx, bd_desktop *desktop,
                         bool verbose) {
  bd_gfx_begin_frame(gfx);
  bd_desktop_draw(desktop, gfx);
  bd_gfx_end_frame(gfx);
  bd_gfx_stats stats = bd_gfx_get_stats(gfx);
  if (stats.presented) {
    bd_platform_present(platform);
  }
  if (verbose) {
    printf("frame: damage=%lldpx rects=%d draws=%d verts=%d glyphs=%d missing=%d "
           "blur=%d full=%d presented=%d\n",
           (long long)stats.damage_pixels, stats.damage_rects, stats.draw_calls,
           stats.vertices, stats.glyphs_rasterized, stats.missing_glyphs,
           stats.blur_passes, stats.full_redraw ? 1 : 0, stats.presented ? 1 : 0);
  }
}

typedef struct main_input {
  bd_desktop *desktop;
  const bd_hotkey_table *hotkeys;
  bool *dirty;
  bool verbose;
} main_input;

static void main_emit(void *user, const bd_input_output *event) {
  main_input *input = (main_input *)user;
  if (event->kind == BD_INPUT_OUTPUT_KEY) {
    if (bd_desktop_handle_key(input->desktop, event->key, event->pressed)) {
      *input->dirty = true;
    }
    return;
  }
  if (input->verbose && event->binding >= 0 &&
      (size_t)event->binding < input->hotkeys->count) {
    const bd_hotkey *hotkey = &input->hotkeys->items[event->binding];
    printf("hotkey: %s trigger=%s phase=%s\n", hotkey->action,
           bd_hotkey_trigger_name((int)event->trigger),
           event->pressed ? "down" : "up");
  }
}

static int run_loop(bd_platform *platform, bd_gfx *gfx, bd_desktop *desktop,
                    bd_input_state *input_state,
                    const bd_hotkey_table *hotkeys, bd_plugin_host *plugins,
                    int idle_check_ms, bool verbose) {
  const bool idle_check = idle_check_ms > 0;
  bool dirty = true;
  bool initial_presented = false;
  int idle_presents = 0;
  double cpu_start = 0.0;
  uint32_t deadline = 0;
  main_input input;
  input.desktop = desktop;
  input.hotkeys = hotkeys;
  input.dirty = &dirty;
  input.verbose = verbose;

  for (;;) {
    if (plugins != NULL) {
      if (bd_plugin_host_poll(plugins, 0) < 0) {
        fprintf(stderr, "brickdesk: plugin poll failed\n");
      }
      if (bd_plugin_host_take_dirty(plugins)) {
        dirty = true;
      }
    }
    if (dirty) {
      render_frame(platform, gfx, desktop, verbose);
      dirty = false;
      if (initial_presented) {
        if (idle_check) {
          ++idle_presents;
        }
      } else {
        initial_presented = true;
        if (idle_check) {
          cpu_start = process_cpu_ms();
          deadline = bd_platform_ticks_ms() + (uint32_t)idle_check_ms;
        }
      }
    }

    int timeout_ms = -1;
    if (idle_check) {
      int32_t remaining = (int32_t)(deadline - bd_platform_ticks_ms());
      if (remaining <= 0) {
        break;
      }
      timeout_ms = (int)remaining;
    } else if (bd_desktop_has_pending_images(desktop)) {
      /* Decode results are uploaded in begin_frame; poll while work exists. */
      timeout_ms = 16;
    }
    if (plugins != NULL) {
      int plugin_timeout = bd_plugin_host_next_timeout(plugins);
      if (plugin_timeout >= 0 && (timeout_ms < 0 || plugin_timeout < timeout_ms)) {
        timeout_ms = plugin_timeout;
      }
    }
    uint64_t hotkey_deadline = 0;
    if (bd_input_state_next_deadline(input_state, &hotkey_deadline)) {
      uint64_t now = bd_platform_ticks_ms();
      uint64_t remaining = hotkey_deadline > now ? hotkey_deadline - now : 0;
      int hotkey_timeout = remaining > 60000 ? 60000 : (int)remaining;
      if (timeout_ms < 0 || hotkey_timeout < timeout_ms) {
        timeout_ms = hotkey_timeout;
      }
    }

    bd_event event;
    int result = bd_platform_wait_event(platform, &event, timeout_ms);
    if (result < 0) {
      fprintf(stderr, "platform: event wait failed\n");
      return 1;
    }
    if (result == 0) {
      if (!idle_check && bd_desktop_has_pending_images(desktop)) {
        dirty = true;
      }
      bd_input_state_advance(input_state, bd_platform_ticks_ms(), main_emit,
                             &input);
      continue;
    }

    switch (event.type) {
      case BD_EVENT_QUIT:
        return 0;
      case BD_EVENT_REDRAW:
        dirty = true;
        break;
      case BD_EVENT_KEY:
        bd_input_state_feed(input_state, event.key, event.pressed,
                            bd_platform_ticks_ms(), main_emit, &input);
        break;
      case BD_EVENT_NONE:
      default:
        break;
    }
    bd_input_state_advance(input_state, bd_platform_ticks_ms(), main_emit,
                           &input);
  }

  printf("idle: duration_ms=%d presents=%d cpu_ms=%.3f\n", idle_check_ms,
         idle_presents, process_cpu_ms() - cpu_start);
  return 0;
}

int main(int argc, char **argv) {
  bool verbose = false;
  int idle_check_ms = 0;
  const char *config_dir = NULL;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (strcmp(arg, "--version") == 0) {
      printf("brickdesk %s (%s)\n", BRICKDESK_VERSION, BRICKDESK_BACKEND_NAME);
      return 0;
    }
    if (strcmp(arg, "--verbose") == 0) {
      verbose = true;
      continue;
    }
    if (strcmp(arg, "--config-dir") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "brickdesk: --config-dir needs a directory\n");
        return 2;
      }
      config_dir = argv[++i];
      continue;
    }
    if (strcmp(arg, "--idle-check") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "brickdesk: --idle-check needs a duration in milliseconds\n");
        return 2;
      }
      char *end = NULL;
      long value = strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || value <= 0 || value > 600000) {
        fprintf(stderr, "brickdesk: invalid --idle-check duration: %s\n", argv[i]);
        return 2;
      }
      idle_check_ms = (int)value;
      continue;
    }
    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      print_usage(stdout);
      return 0;
    }
    fprintf(stderr, "brickdesk: unknown option: %s\n", arg);
    print_usage(stderr);
    return 2;
  }

  bd_config_options config_options = {config_dir, true};
  char config_error[256];
  bd_config *settings =
      bd_config_load(&config_options, config_error, sizeof(config_error));
  if (settings == NULL) {
    fprintf(stderr, "brickdesk: config: %s\n", config_error);
    return 1;
  }
  if (bd_config_diagnostic_count(settings) > 0) {
    fprintf(stderr,
            "brickdesk: config: %zu diagnostic(s) in %s; "
            "run brickdesk-config check\n",
            bd_config_diagnostic_count(settings), bd_config_dir(settings));
  }

  const char *theme_mode = bd_config_get_string(settings, "theme.mode", "dark");
  bool dark = strcmp(theme_mode, "light") != 0;
  const char *accent_name = bd_config_get_string(settings, "theme.accent", "persimmon");
  const char *accent_hex = bd_config_accent_hex(accent_name, dark);
  const char *ambient = bd_config_get_string(settings, "theme.ambient", "baked");
  bd_desktop_theme theme;
  theme.dark = dark;
  theme.ambient_baked = strcmp(ambient, "baked") == 0;
  if (accent_hex == NULL || !bd_color_from_hex(accent_hex, &theme.accent)) {
    fprintf(stderr, "brickdesk: config: invalid theme accent '%s'\n", accent_name);
    bd_config_free(settings);
    return 1;
  }
  printf("theme: mode=%s accent=%s ambient=%s\n", theme_mode, accent_hex, ambient);

  bd_platform_config config;
  config.title = "brickdesk";
  config.width = BD_DEFAULT_WIDTH;
  config.height = BD_DEFAULT_HEIGHT;
  config.hidden = getenv("BRICKDESK_HEADLESS") != NULL;

  bd_platform *platform = bd_platform_create(&config);
  if (platform == NULL) {
    bd_config_free(settings);
    return 1;
  }

  bd_gl_info gl;
  bd_platform_gl_info(platform, &gl);
  bd_gl_print_info(&gl, verbose);
  printf("window: %dx%d\n", bd_platform_width(platform), bd_platform_height(platform));

  bd_gfx *gfx = bd_gfx_create(bd_platform_width(platform), bd_platform_height(platform));
  if (gfx == NULL) {
    fprintf(stderr, "brickdesk: renderer creation failed\n");
    bd_platform_destroy(platform);
    bd_config_free(settings);
    return 1;
  }

  long cover_cache_mb = bd_config_get_int(settings, "ui.cover_cache_mb", 16);
  if (cover_cache_mb < 1) {
    cover_cache_mb = 1;
  }
  bd_gfx_image_cache_set_budget(gfx, (size_t)cover_cache_mb * 1024u * 1024u);

  bd_desktop *desktop = bd_desktop_create(gfx, &theme);
  if (desktop == NULL) {
    fprintf(stderr, "brickdesk: out of memory\n");
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    bd_config_free(settings);
    return 1;
  }

  bd_hotkey_table hotkeys;
  bd_hotkey_table_load(&hotkeys, settings);
  if (verbose &&
      (hotkeys.diagnostic_count > 0 || hotkeys.conflict_count > 0)) {
    fprintf(stderr, "brickdesk: %zu hotkey diagnostic(s), %zu conflict(s)\n",
            hotkeys.diagnostic_count, hotkeys.conflict_count);
  }
  bd_input_state input_state;
  bd_input_state_init(&input_state, &hotkeys, "global",
                      (int)bd_config_get_int(settings, "input.double_tap_ms",
                                             300));

  bd_plugin_host *plugin_host = NULL;
  bool plugins_enabled =
      bd_config_get_bool(settings, "plugins.enabled", true) && idle_check_ms == 0;
  if (plugins_enabled) {
    const char *configured_dir = bd_config_get_string(settings, "plugins.dir", "");
    bd_plugin_limits limits = bd_plugin_default_limits();
    long idle_stop_ms = bd_config_get_int(settings, "plugins.idle_stop", 60000);
    if (idle_stop_ms >= 0 && idle_stop_ms <= 86400000) {
      limits.idle_stop_ms = (int)idle_stop_ms;
    }
    bd_plugin_host_config plugin_config;
    memset(&plugin_config, 0, sizeof(plugin_config));
    plugin_config.plugin_dir =
        configured_dir != NULL && configured_dir[0] != '\0' ? configured_dir : NULL;
    plugin_config.limits = &limits;
    plugin_config.display_width = bd_platform_width(platform);
    plugin_config.display_height = bd_platform_height(platform);
    plugin_config.dark = theme.dark;
    plugin_config.accent = ((uint32_t)(theme.accent.r * 255.0f + 0.5f) << 16) |
                           ((uint32_t)(theme.accent.g * 255.0f + 0.5f) << 8) |
                           (uint32_t)(theme.accent.b * 255.0f + 0.5f);
    plugin_config.locale = bd_config_get_string(settings, "ui.language", "zh-CN");
    plugin_config.verbose = verbose;
    plugin_host = bd_plugin_host_create(&plugin_config);
    if (plugin_host == NULL) {
      fprintf(stderr, "brickdesk: plugin host unavailable\n");
    } else {
      char plugin_error[256] = {0};
      int plugin_count =
          bd_plugin_host_scan(plugin_host, plugin_error, sizeof(plugin_error));
      if (plugin_count < 0) {
        fprintf(stderr, "brickdesk: plugins: %s\n", plugin_error);
      } else {
        for (size_t i = 0; i < bd_plugin_host_diagnostic_count(plugin_host); ++i) {
          char diagnostic[256];
          if (bd_plugin_host_diagnostic(plugin_host, i, diagnostic,
                                        sizeof(diagnostic))) {
            fprintf(stderr, "brickdesk: plugin: %s\n", diagnostic);
          }
        }
        bd_plugin *tile_plugin = NULL;
        for (size_t i = 0; i < bd_plugin_host_count(plugin_host); ++i) {
          const bd_plugin_manifest *manifest =
              bd_plugin_host_manifest(plugin_host, i);
          if (manifest != NULL &&
              (manifest->capabilities & BD_PLUGIN_CAP_TILE) != 0) {
            tile_plugin = bd_plugin_host_at(plugin_host, i);
            break;
          }
        }
        if (tile_plugin != NULL) {
          bd_desktop_set_plugin_tile(desktop, tile_plugin);
        } else if (verbose) {
          fprintf(stderr, "brickdesk: no tile plugin found in %s\n",
                  configured_dir != NULL && configured_dir[0] != '\0'
                      ? configured_dir
                      : BRICKDESK_PLUGIN_DIR);
        }
        for (size_t i = 0; i < bd_plugin_host_count(plugin_host); ++i) {
          const bd_plugin_manifest *manifest =
              bd_plugin_host_manifest(plugin_host, i);
          if (manifest != NULL && manifest->autostart) {
            bd_plugin_activate(bd_plugin_host_at(plugin_host, i));
          }
        }
      }
    }
  }

  int result = run_loop(platform, gfx, desktop, &input_state, &hotkeys,
                        plugin_host, idle_check_ms, verbose);

  bd_desktop_destroy(desktop);
  bd_plugin_host_destroy(plugin_host);
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  bd_config_free(settings);
  return result;
}
