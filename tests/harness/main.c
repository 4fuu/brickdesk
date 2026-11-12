#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "brickdesk/config.h"
#include "brickdesk/desktop.h"
#include "brickdesk/gfx.h"
#include "brickdesk/input.h"
#include "brickdesk/platform.h"
#include "brickdesk/theme.h"
#include "scenario.h"

#define BD_PATH_MAX 1024

static void print_usage(FILE *stream) {
  fprintf(stream,
          "usage: bd_harness --scenario FILE --out-dir DIR [--config-dir DIR]\n"
          "                  [--verbose]\n"
          "\n"
          "Replays a JSON input scenario and writes screenshots as PNG.\n");
}

typedef struct harness_input {
  bd_desktop *desktop;
} harness_input;

static void harness_emit(void *user, const bd_input_output *event) {
  harness_input *input = (harness_input *)user;
  if (event->kind == BD_INPUT_OUTPUT_KEY) {
    bd_desktop_handle_key(input->desktop, event->key, event->pressed);
  }
}

static bool make_directory(const char *path) {
  if (mkdir(path, 0777) == 0) {
    return true;
  }
  return errno == EEXIST;
}

static bool join_path(char *out, size_t out_size, const char *directory,
                      const char *name) {
  int written = snprintf(out, out_size, "%s/%s", directory, name);
  return written > 0 && (size_t)written < out_size;
}

static int run_scenario(const bd_scenario *scenario, const char *out_dir,
                        const char *config_dir, bool verbose) {
  bd_platform_config config;
  config.title = "brickdesk harness";
  config.width = scenario->width;
  config.height = scenario->height;
  config.hidden = true;

  bd_platform *platform = bd_platform_create(&config);
  if (platform == NULL) {
    return 1;
  }

  bd_gl_info gl;
  bd_platform_gl_info(platform, &gl);
  bd_gl_print_info(&gl, verbose);
  printf("window: %dx%d\n", bd_platform_width(platform), bd_platform_height(platform));

  if (bd_platform_width(platform) != scenario->width ||
      bd_platform_height(platform) != scenario->height) {
    fprintf(stderr, "harness: scenario asks for %dx%d but drawable is %dx%d\n",
            scenario->width, scenario->height, bd_platform_width(platform),
            bd_platform_height(platform));
    bd_platform_destroy(platform);
    return 1;
  }

  bd_gfx *gfx = bd_gfx_create(scenario->width, scenario->height);
  if (gfx == NULL) {
    fprintf(stderr, "harness: renderer creation failed\n");
    bd_platform_destroy(platform);
    return 1;
  }

  bd_desktop_theme theme;
  theme.dark = true;
  theme.ambient_baked = true;
  const char *accent_hex = bd_config_accent_hex("persimmon", true);
  if (accent_hex == NULL || !bd_color_from_hex(accent_hex, &theme.accent)) {
    fprintf(stderr, "harness: cannot resolve theme accent\n");
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }
  bd_desktop *desktop = bd_desktop_create(gfx, &theme);
  if (desktop == NULL) {
    fprintf(stderr, "harness: out of memory\n");
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }

  bd_config_options config_options;
  config_options.config_dir = config_dir;
  config_options.migrate = false;
  char config_error[256] = {0};
  bd_config *settings =
      bd_config_load(&config_options, config_error, sizeof(config_error));
  if (settings == NULL) {
    fprintf(stderr, "harness: config: %s\n", config_error);
    bd_desktop_destroy(desktop);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }
  bd_hotkey_table hotkeys;
  bd_hotkey_table_load(&hotkeys, settings);
  if (verbose && (hotkeys.diagnostic_count > 0 || hotkeys.conflict_count > 0)) {
    fprintf(stderr, "harness: %zu hotkey diagnostic(s), %zu conflict(s)\n",
            hotkeys.diagnostic_count, hotkeys.conflict_count);
  }
  bd_input_state input_state;
  bd_input_state_init(&input_state, &hotkeys, "global",
                      (int)bd_config_get_int(settings, "input.double_tap_ms", 300));
  harness_input input;
  input.desktop = desktop;

  printf("scenario: %s (%dx%d, fixed_dt_ms=%d, steps=%d)\n", scenario->name,
         scenario->width, scenario->height, scenario->fixed_dt_ms,
         scenario->step_count);

  unsigned long frame = 0;
  int captures = 0;
  int result = 0;

  for (int i = 0; i < scenario->step_count; ++i) {
    const bd_step *step = &scenario->steps[i];
    switch (step->type) {
      case BD_STEP_WAIT:
        frame += (unsigned long)step->frames;
        bd_input_state_advance(&input_state,
                               (uint64_t)frame * (uint64_t)scenario->fixed_dt_ms,
                               harness_emit, &input);
        break;

      case BD_STEP_KEY:
        printf("key: frame=%lu key=%s action=%s\n", frame, bd_key_name(step->key),
               step->pressed ? "down" : "up");
        bd_input_state_feed(&input_state, step->key, step->pressed,
                            (uint64_t)frame * (uint64_t)scenario->fixed_dt_ms,
                            harness_emit, &input);
        break;

      case BD_STEP_SCREENSHOT: {
        bd_input_state_advance(&input_state,
                               (uint64_t)frame * (uint64_t)scenario->fixed_dt_ms,
                               harness_emit, &input);
        int quiet = 0;
        for (int wait = 0; wait < 300 && quiet < 2; ++wait) {
          bd_gfx_begin_frame(gfx);
          bd_desktop_draw(desktop, gfx);
          bd_gfx_end_frame(gfx);
          if (bd_desktop_has_pending_images(desktop)) {
            quiet = 0;
          } else {
            ++quiet;
          }
          bd_event event;
          bd_platform_wait_event(platform, &event, 5);
        }

        char path[BD_PATH_MAX];
        char filename[80];
        snprintf(filename, sizeof(filename), "%s.png", step->screenshot);
        if (!join_path(path, sizeof(path), out_dir, filename)) {
          fprintf(stderr, "harness: screenshot path is too long\n");
          result = 1;
          break;
        }
        if (!bd_gfx_save_png(gfx, path)) {
          fprintf(stderr, "harness: capture '%s' failed: %s\n", step->screenshot,
                  bd_gfx_last_error(gfx));
          result = 1;
          break;
        }
        bd_platform_present(platform);
        printf("capture: frame=%lu name=%s file=%s\n", frame, step->screenshot, path);
        ++captures;
        break;
      }
    }
    if (result != 0) {
      break;
    }
  }

  printf("done: frames=%lu captures=%d\n", frame, captures);

  bd_config_free(settings);
  bd_desktop_destroy(desktop);
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  return result;
}

int main(int argc, char **argv) {
  const char *scenario_path = NULL;
  const char *out_dir = NULL;
  const char *config_dir = NULL;
  bool verbose = false;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (strcmp(arg, "--scenario") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "bd_harness: --scenario needs a path\n");
        return 2;
      }
      scenario_path = argv[++i];
    } else if (strcmp(arg, "--out-dir") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "bd_harness: --out-dir needs a path\n");
        return 2;
      }
      out_dir = argv[++i];
    } else if (strcmp(arg, "--config-dir") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "bd_harness: --config-dir needs a path\n");
        return 2;
      }
      config_dir = argv[++i];
    } else if (strcmp(arg, "--verbose") == 0) {
      verbose = true;
    } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      print_usage(stdout);
      return 0;
    } else {
      fprintf(stderr, "bd_harness: unknown option: %s\n", arg);
      print_usage(stderr);
      return 2;
    }
  }

  if (scenario_path == NULL || out_dir == NULL) {
    fprintf(stderr, "bd_harness: --scenario and --out-dir are required\n");
    print_usage(stderr);
    return 2;
  }

  bd_scenario scenario;
  char error[512] = {0};
  if (!bd_scenario_load(scenario_path, &scenario, error, sizeof(error))) {
    fprintf(stderr, "bd_harness: %s: %s\n", scenario_path, error);
    return 1;
  }

  if (!make_directory(out_dir)) {
    fprintf(stderr, "bd_harness: cannot create output directory '%s': %s\n", out_dir,
            strerror(errno));
    return 1;
  }

  return run_scenario(&scenario, out_dir, config_dir, verbose);
}
