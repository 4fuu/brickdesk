#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "brickdesk/desktop.h"
#include "brickdesk/gfx.h"
#include "brickdesk/platform.h"
#include "brickdesk/version.h"

static void print_usage(FILE *stream) {
  fprintf(stream,
          "usage: brickdesk [options]\n"
          "\n"
          "  --version        print version and exit\n"
          "  --verbose        print GL extensions at startup\n"
          "  --idle-check MS  block for MS milliseconds and report redraws\n"
          "  -h, --help       show this help\n");
}

static double process_cpu_ms(void) {
  return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

static void render_frame(bd_platform *platform, bd_gfx *gfx, bd_desktop *desktop) {
  bd_gfx_begin_frame(gfx);
  bd_desktop_draw(desktop, gfx);
  bd_gfx_end_frame(gfx);
  bd_platform_present(platform);
}

static int run_loop(bd_platform *platform, bd_gfx *gfx, bd_desktop *desktop,
                    int idle_check_ms) {
  const bool idle_check = idle_check_ms > 0;
  bool dirty = true;
  bool initial_presented = false;
  int idle_presents = 0;
  double cpu_start = 0.0;
  uint32_t deadline = 0;

  for (;;) {
    if (dirty) {
      render_frame(platform, gfx, desktop);
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
    }

    bd_event event;
    int result = bd_platform_wait_event(platform, &event, timeout_ms);
    if (result < 0) {
      fprintf(stderr, "platform: event wait failed\n");
      return 1;
    }
    if (result == 0) {
      continue;
    }

    switch (event.type) {
      case BD_EVENT_QUIT:
        return 0;
      case BD_EVENT_REDRAW:
        dirty = true;
        break;
      case BD_EVENT_KEY:
        if (bd_desktop_handle_key(desktop, event.key, event.pressed)) {
          dirty = true;
        }
        break;
      case BD_EVENT_NONE:
      default:
        break;
    }
  }

  printf("idle: duration_ms=%d presents=%d cpu_ms=%.3f\n", idle_check_ms,
         idle_presents, process_cpu_ms() - cpu_start);
  return 0;
}

int main(int argc, char **argv) {
  bool verbose = false;
  int idle_check_ms = 0;

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

  bd_platform_config config;
  config.title = "brickdesk";
  config.width = BD_DEFAULT_WIDTH;
  config.height = BD_DEFAULT_HEIGHT;
  config.hidden = getenv("BRICKDESK_HEADLESS") != NULL;

  bd_platform *platform = bd_platform_create(&config);
  if (platform == NULL) {
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
    return 1;
  }

  bd_desktop *desktop = bd_desktop_create();
  if (desktop == NULL) {
    fprintf(stderr, "brickdesk: out of memory\n");
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }

  int result = run_loop(platform, gfx, desktop, idle_check_ms);

  bd_desktop_destroy(desktop);
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  return result;
}
