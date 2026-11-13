#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "brickdesk/config.h"
#include "brickdesk/gfx.h"
#include "brickdesk/platform.h"
#include "brickdesk/plugin.h"
#include "brickdesk/shell.h"
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

static int run_loop(bd_shell *shell, int idle_check_ms, bool verbose) {
  (void)verbose;
  const bool idle_check = idle_check_ms > 0;
  double cpu_start = 0.0;
  uint64_t presents_start = 0;

  /* Draw the initial desktop before measuring an idle interval. */
  if (bd_shell_iterate(shell, 0) < 0) {
    return 0;
  }
  if (idle_check) {
    /*
     * Cover decodes start on the first frame.  Let them finish before the
     * measurement window so the idle check measures a settled home page.
     */
    for (int i = 0; i < 240 && bd_shell_has_pending_images(shell); ++i) {
      if (bd_shell_iterate(shell, 0) < 0) {
        return 0;
      }
      struct timespec delay;
      delay.tv_sec = 0;
      delay.tv_nsec = 5000000L;
      nanosleep(&delay, NULL);
    }
    cpu_start = process_cpu_ms();
    presents_start = bd_shell_present_count(shell);
  }

  uint64_t deadline = 0;
  if (idle_check) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    deadline = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u +
               (uint64_t)idle_check_ms;
  }

  for (;;) {
    int timeout = -1;
    if (idle_check) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      uint64_t current =
          (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
      if (current >= deadline) {
        break;
      }
      uint64_t remaining = deadline - current;
      timeout = remaining > 60000 ? 60000 : (int)remaining;
    }
    if (bd_shell_iterate(shell, timeout) < 0) {
      break;
    }
  }

  if (idle_check) {
    uint64_t presented = bd_shell_present_count(shell) - presents_start;
    printf("idle: duration_ms=%d presents=%llu cpu_ms=%.3f\n", idle_check_ms,
           (unsigned long long)presented, process_cpu_ms() - cpu_start);
  }
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

  const char *accent_name = bd_config_get_string(settings, "theme.accent", "persimmon");
  const char *accent_hex = bd_config_accent_hex(accent_name, true);
  const char *ambient = bd_config_get_string(settings, "theme.ambient", "baked");
  bd_color theme_accent;
  if (accent_hex == NULL ||
      !bd_color_from_hex(accent_hex, &theme_accent)) {
    fprintf(stderr, "brickdesk: config: invalid theme accent '%s'\n", accent_name);
    bd_config_free(settings);
    return 1;
  }
  printf("theme: mode=dark accent=%s ambient=%s\n", accent_hex, ambient);

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

  bd_plugin_host *plugin_host = NULL;
  bool plugins_enabled = bd_config_get_bool(settings, "plugins.enabled", true);
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
    plugin_config.dark = true;
    plugin_config.accent = ((uint32_t)(theme_accent.r * 255.0f + 0.5f) << 16) |
                           ((uint32_t)(theme_accent.g * 255.0f + 0.5f) << 8) |
                           (uint32_t)(theme_accent.b * 255.0f + 0.5f);
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

  bd_shell_config shell_config;
  memset(&shell_config, 0, sizeof(shell_config));
  shell_config.platform = platform;
  shell_config.gfx = gfx;
  shell_config.settings = settings;
  shell_config.plugins = plugin_host;
  shell_config.verbose = verbose;
  shell_config.attach_plugin_tile = idle_check_ms == 0;
  bd_shell *shell = bd_shell_create(&shell_config);
  if (shell == NULL) {
    fprintf(stderr, "brickdesk: cannot create desktop loop\n");
    bd_plugin_host_destroy(plugin_host);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    bd_config_free(settings);
    return 1;
  }
  printf("input: source=%s\n", bd_shell_input_source(shell));

  int result = run_loop(shell, idle_check_ms, verbose);

  bd_shell_destroy(shell);
  bd_plugin_host_destroy(plugin_host);
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  bd_config_free(settings);
  return result;
}
