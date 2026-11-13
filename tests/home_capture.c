#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "brickdesk/app.h"
#include "brickdesk/config.h"
#include "brickdesk/gfx.h"
#include "brickdesk/home.h"
#include "brickdesk/platform.h"
#include "brickdesk/theme.h"
#include "home_fixture.h"

#define CAPTURE_WIDTH 1024
#define CAPTURE_HEIGHT 768
#define CAPTURE_MAX_PATH 1024

/*
 * Test fixture memory readings.  These are deliberately named as fixtures:
 * production readings come from bd_sys_process_tree_rss_mb() and render
 * "unknown" when /proc cannot be sampled.
 */
enum {
  FIXTURE_MEMORY_TOWN_MB = 210,
  FIXTURE_MEMORY_SPACE_MB = 210,
  FIXTURE_MEMORY_MUSIC_MB = 86
};

static bool write_png(bd_gfx *gfx, const char *out_dir, const char *name) {
  char path[CAPTURE_MAX_PATH];
  int written = snprintf(path, sizeof(path), "%s/%s.png", out_dir, name);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    fprintf(stderr, "home_capture: path too long for %s\n", name);
    return false;
  }
  if (!bd_gfx_save_png(gfx, path)) {
    fprintf(stderr, "home_capture: save %s failed: %s\n", path,
            bd_gfx_last_error(gfx));
    return false;
  }
  printf("capture: %s\n", path);
  return true;
}

static void draw_frame(bd_platform *platform, bd_gfx *gfx, bd_home *home,
                       uint64_t now, bool full_damage) {
  bd_gfx_begin_frame(gfx);
  bd_home_update(home, now);
  if (full_damage) {
    bd_home_damage(home);
  }
  bd_home_draw(home, gfx);
  bd_gfx_end_frame(gfx);
  if (bd_gfx_get_stats(gfx).presented) {
    bd_platform_present(platform);
  }
}

static void settle(bd_platform *platform, bd_gfx *gfx, bd_home *home,
                   uint64_t now) {
  int quiet = 0;
  for (int tries = 0; tries < 400 && quiet < 2; ++tries) {
    draw_frame(platform, gfx, home, now, true);
    if (bd_home_has_pending_images(home)) {
      quiet = 0;
    } else {
      ++quiet;
    }
    bd_event event;
    bd_platform_wait_event(platform, &event, 5);
  }
}

static void fill_rect(uint8_t *pixels, int x0, int y0, int w, int h, int r,
                      int g, int b) {
  for (int y = y0; y < y0 + h; ++y) {
    if (y < 0 || y >= CAPTURE_HEIGHT) {
      continue;
    }
    for (int x = x0; x < x0 + w; ++x) {
      if (x < 0 || x >= CAPTURE_WIDTH) {
        continue;
      }
      uint8_t *pixel = pixels + ((size_t)(CAPTURE_HEIGHT - 1 - y) * CAPTURE_WIDTH +
                                 (size_t)x) * 4u;
      pixel[0] = (uint8_t)r;
      pixel[1] = (uint8_t)g;
      pixel[2] = (uint8_t)b;
      pixel[3] = 255;
    }
  }
}

static void fill_circle(uint8_t *pixels, int cx, int cy, int radius, int r,
                        int g, int b) {
  for (int y = cy - radius; y <= cy + radius; ++y) {
    for (int x = cx - radius; x <= cx + radius; ++x) {
      int dx = x - cx;
      int dy = y - cy;
      if (dx * dx + dy * dy <= radius * radius) {
        fill_rect(pixels, x, y, 1, 1, r, g, b);
      }
    }
  }
}

static bool write_demo_frame(const char *path, int variant) {
  uint8_t *pixels = (uint8_t *)malloc((size_t)CAPTURE_WIDTH * CAPTURE_HEIGHT * 4u);
  if (pixels == NULL) {
    return false;
  }
  for (int y = 0; y < CAPTURE_HEIGHT; ++y) {
    int r;
    int g;
    int b;
    if (variant == 0) {
      if (y < 520) {
        r = 54 + y * 190 / 520;
        g = 40 + y * 92 / 520;
        b = 72 + y * 16 / 520;
      } else {
        int ground = (y - 520) * 255 / (CAPTURE_HEIGHT - 520);
        r = 34 + ground / 3;
        g = 58 + ground / 2;
        b = 38 + ground / 4;
      }
      if (((y / 9) % 2) == 0 && y > 560 && y < 700) {
        int hill = 555 + ((y / 17) % 3) * 6;
        if (y > hill) {
          r = 44;
          g = 72;
          b = 48;
        }
      }
    } else if (variant == 1) {
      r = 12 + y * 18 / (CAPTURE_HEIGHT - 1);
      g = 18 + y * 34 / (CAPTURE_HEIGHT - 1);
      b = 32 + y * 44 / (CAPTURE_HEIGHT - 1);
    } else {
      r = 38 + y * 52 / (CAPTURE_HEIGHT - 1);
      g = 18 + y * 18 / (CAPTURE_HEIGHT - 1);
      b = 54 + y * 44 / (CAPTURE_HEIGHT - 1);
    }
    fill_rect(pixels, 0, y, CAPTURE_WIDTH, 1, r, g, b);
  }

  if (variant == 0) {
    fill_circle(pixels, 748, 432, 62, 245, 196, 120);
    fill_rect(pixels, 448, 486, 184, 106, 58, 42, 36);
    fill_rect(pixels, 474, 512, 40, 32, 236, 190, 112);
    fill_rect(pixels, 548, 512, 40, 32, 220, 160, 90);
    fill_rect(pixels, 520, 456, 40, 34, 74, 52, 40);
    for (int x = 0; x < CAPTURE_WIDTH; x += 34) {
      fill_rect(pixels, x, 610 + (x / 34) % 3 * 4, 3, 52, 70, 54, 40);
    }
  } else if (variant == 1) {
    for (int i = 0; i < 90; ++i) {
      int x = (i * 137 + 41) % CAPTURE_WIDTH;
      int y = (i * 233 + 17) % 520;
      int size = (i % 7 == 0) ? 3 : 2;
      fill_rect(pixels, x, y, size, size, 220, 226, 236);
    }
    fill_circle(pixels, 706, 318, 128, 70, 132, 130);
    fill_circle(pixels, 736, 286, 62, 38, 74, 76);
  } else {
    fill_circle(pixels, 648, 380, 154, 18, 14, 22);
    fill_circle(pixels, 648, 380, 142, 42, 32, 54);
    fill_circle(pixels, 648, 380, 34, 210, 180, 90);
    fill_circle(pixels, 648, 380, 6, 20, 16, 16);
  }

  char error[128];
  uint64_t seq = 1;
  bool ok = bd_app_last_frame_write(path, CAPTURE_WIDTH, CAPTURE_HEIGHT,
                                    BD_APP_PIXEL_RGBA8888, pixels,
                                    (size_t)CAPTURE_WIDTH * 4u, &seq, error,
                                    sizeof(error));
  if (!ok) {
    fprintf(stderr, "home_capture: last frame write failed: %s\n", error);
  }
  free(pixels);
  return ok;
}

static bool g_quick_available = true;
static int g_quick_brightness = 70;
static int g_quick_volume = 45;

static bool capture_quick_read(void *user, bd_home_quick_control control,
                               int *percent) {
  (void)user;
  if (!g_quick_available || percent == NULL) {
    return false;
  }
  *percent =
      control == BD_HOME_QUICK_BRIGHTNESS ? g_quick_brightness : g_quick_volume;
  return true;
}

static bool capture_quick_adjust(void *user, bd_home_quick_control control,
                                 int delta, int *percent) {
  (void)user;
  if (!g_quick_available || percent == NULL) {
    return false;
  }
  int value = (control == BD_HOME_QUICK_BRIGHTNESS ? g_quick_brightness
                                                   : g_quick_volume) +
              delta;
  if (value < 0) {
    value = 0;
  }
  if (value > 100) {
    value = 100;
  }
  if (control == BD_HOME_QUICK_BRIGHTNESS) {
    g_quick_brightness = value;
  } else {
    g_quick_volume = value;
  }
  *percent = value;
  return true;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: bd_home_capture OUT_DIR\n");
    return 2;
  }
  const char *out_dir = argv[1];

  bd_platform_config config;
  config.title = "brickdesk home capture";
  config.width = CAPTURE_WIDTH;
  config.height = CAPTURE_HEIGHT;
  config.hidden = true;
  bd_platform *platform = bd_platform_create(&config);
  if (platform == NULL) {
    return 1;
  }
  bd_gfx *gfx = bd_gfx_create(CAPTURE_WIDTH, CAPTURE_HEIGHT);
  if (gfx == NULL) {
    bd_platform_destroy(platform);
    return 1;
  }
  bd_gfx_image_cache_set_budget(gfx, 96u * 1024u * 1024u);

  bd_color accent;
  const char *hex = bd_config_accent_hex("persimmon", true);
  if (hex == NULL || !bd_color_from_hex(hex, &accent)) {
    accent = bd_color_rgb(224, 122, 95);
  }
  bd_home_style style;
  memset(&style, 0, sizeof(style));
  style.accent_base = accent;
  style.blur_enabled = true;
  style.width = CAPTURE_WIDTH;
  style.height = CAPTURE_HEIGHT;
  bd_home *home = bd_home_create(gfx, &style);
  if (home == NULL) {
    fprintf(stderr, "home_capture: home create failed\n");
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }

  bd_home_item items[HOME_DEMO_COUNT];
  int count = 0;
  home_demo_items(items, &count);
  if (!bd_home_set_items(home, items, count)) {
    fprintf(stderr, "home_capture: set items failed\n");
    return 1;
  }
  bd_home_status status;
  memset(&status, 0, sizeof(status));
  /* Fixture status for deterministic screenshots; production samples sysfs. */
  status.clock = "22:26";
  status.wifi_known = true;
  status.wifi = true;
  status.battery_percent = 82;
  status.max_background = 3;
  bd_home_set_status(home, &status);
  bd_home_quick_backend quick_backend;
  memset(&quick_backend, 0, sizeof(quick_backend));
  quick_backend.read = capture_quick_read;
  quick_backend.adjust = capture_quick_adjust;
  bd_home_set_quick_backend(home, &quick_backend);

  char temp_dir[] = "/tmp/bd_home_capture.XXXXXX";
  if (mkdtemp(temp_dir) == NULL) {
    fprintf(stderr, "home_capture: mkdtemp failed\n");
    return 1;
  }
  char town_path[CAPTURE_MAX_PATH];
  char space_path[CAPTURE_MAX_PATH];
  char music_path[CAPTURE_MAX_PATH];
  snprintf(town_path, sizeof(town_path), "%s/last-frame-town.bin", temp_dir);
  snprintf(space_path, sizeof(space_path), "%s/last-frame-space.bin", temp_dir);
  snprintf(music_path, sizeof(music_path), "%s/last-frame-music.bin", temp_dir);
  if (!write_demo_frame(town_path, 0) || !write_demo_frame(space_path, 1) ||
      !write_demo_frame(music_path, 2)) {
    return 1;
  }

  uint64_t now = 1000;
  bd_home_update(home, now);
  bd_home_set_focus(home, 5);
  now += 300;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_live_focus")) {
    return 1;
  }

  bd_home_set_focus(home, 7);
  now += 300;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_dark")) {
    return 1;
  }

  status.battery_percent = 100;
  bd_home_set_status(home, &status);
  now += 300;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_status_online")) {
    return 1;
  }

  status.wifi = false;
  status.battery_percent = 12;
  bd_home_set_status(home, &status);
  now += 300;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_status_offline")) {
    return 1;
  }

  status.wifi_known = false;
  status.battery_percent = -1;
  bd_home_set_status(home, &status);
  now += 300;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_status_unknown")) {
    return 1;
  }

  status.wifi_known = true;
  status.wifi = true;
  status.battery_percent = 82;
  bd_home_set_status(home, &status);

  bd_home_update(home, now);
  if (!bd_home_set_running(home, "demo-03", town_path, true,
                           FIXTURE_MEMORY_TOWN_MB)) {
    fprintf(stderr, "home_capture: town running failed\n");
    return 1;
  }
  if (!bd_home_start_return(home, "demo-03")) {
    fprintf(stderr, "home_capture: return failed\n");
    return 1;
  }
  now += 420;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_running")) {
    return 1;
  }

  bd_home_update(home, now);
  if (!bd_home_start_return(home, "demo-03")) {
    fprintf(stderr, "home_capture: second return failed\n");
    return 1;
  }
  now += 200;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_return_200")) {
    return 1;
  }
  now += 300;
  bd_home_update(home, now);

  bd_home_set_running(home, "demo-10", space_path, true, FIXTURE_MEMORY_SPACE_MB);
  bd_home_set_running(home, "music", music_path, true, FIXTURE_MEMORY_MUSIC_MB);
  if (!bd_home_open_quick(home)) {
    fprintf(stderr, "home_capture: quick open failed\n");
    return 1;
  }
  now += 220;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_quick_running")) {
    return 1;
  }
  if (bd_home_selected_id(home) == NULL ||
      strcmp(bd_home_selected_id(home), "demo-03") != 0) {
    fprintf(stderr, "home_capture: quick first-row id is wrong\n");
    return 1;
  }
  for (int i = 0; i < 3; ++i) {
    bd_home_input(home, BD_HOME_ACTION_DOWN);
  }
  if (bd_home_selected_id(home) != NULL) {
    fprintf(stderr, "home_capture: brightness row should have no app id\n");
    return 1;
  }
  for (int i = 0; i < 3; ++i) {
    bd_home_input(home, BD_HOME_ACTION_UP);
  }
  if (bd_home_selected_id(home) == NULL ||
      strcmp(bd_home_selected_id(home), "demo-03") != 0) {
    fprintf(stderr, "home_capture: quick navigation did not return to row 0\n");
    return 1;
  }
  if (!bd_home_input(home, BD_HOME_ACTION_END) || !bd_home_dialog_open(home)) {
    fprintf(stderr, "home_capture: quick end did not name the selected app\n");
    return 1;
  }
  bd_home_input(home, BD_HOME_ACTION_CANCEL);
  if (bd_home_take_dialog_result(home) != BD_HOME_DIALOG_RESULT_CANCEL) {
    fprintf(stderr, "home_capture: quick end cancel result missing\n");
    return 1;
  }
  now += 200;
  bd_home_update(home, now);

  g_quick_brightness = 0;
  g_quick_volume = 100;
  bd_home_refresh_quick(home);
  bd_home_damage(home);
  now += 220;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_quick_values_extreme")) {
    return 1;
  }
  g_quick_brightness = 9;
  g_quick_volume = 99;
  bd_home_refresh_quick(home);
  bd_home_damage(home);
  now += 220;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_quick_values_high")) {
    return 1;
  }
  g_quick_brightness = 70;
  g_quick_volume = 45;
  bd_home_refresh_quick(home);
  bd_home_damage(home);
  now += 220;
  settle(platform, gfx, home, now);

  status.wifi_known = false;
  status.wifi = false;
  status.battery_percent = -1;
  bd_home_set_status(home, &status);
  bd_home_damage(home);
  now += 220;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_quick_status_unknown")) {
    return 1;
  }
  status.wifi_known = true;
  status.wifi = true;
  status.battery_percent = 82;
  bd_home_set_status(home, &status);
  g_quick_available = false;
  bd_home_refresh_quick(home);
  bd_home_damage(home);
  now += 220;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_quick_controls_unknown")) {
    return 1;
  }
  g_quick_available = true;
  bd_home_refresh_quick(home);
  bd_home_damage(home);
  now += 220;
  settle(platform, gfx, home, now);

  bd_home_close_quick(home);
  now += 220;
  bd_home_update(home, now);
  if (!bd_home_set_running_memory(home, "music", -1)) {
    fprintf(stderr, "home_capture: unknown memory fixture failed\n");
    return 1;
  }
  if (!bd_home_open_quick(home) ||
      !bd_home_set_running_memory(home, "music", -1)) {
    fprintf(stderr, "home_capture: unknown memory panel failed\n");
    return 1;
  }
  now += 220;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_quick_unknown_memory")) {
    return 1;
  }
  bd_home_close_quick(home);
  now += 220;
  bd_home_update(home, now);
  bd_home_set_running_memory(home, "music", FIXTURE_MEMORY_MUSIC_MB);
  if (!bd_home_open_background_full(home, "demo-04", "demo-03", 3)) {
    fprintf(stderr, "home_capture: background full dialog failed\n");
    return 1;
  }
  now += 200;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_background_full")) {
    return 1;
  }
  bd_home_set_dialog_error(home, "无法结束「小镇四季」，请重试或取消");
  now += 200;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_background_full_error")) {
    return 1;
  }
  bd_home_input(home, BD_HOME_ACTION_CANCEL);
  if (bd_home_take_dialog_result(home) != BD_HOME_DIALOG_RESULT_CANCEL) {
    fprintf(stderr, "home_capture: background full cancel result missing\n");
    return 1;
  }
  now += 200;
  bd_home_update(home, now);
  if (!bd_home_open_background_full(home, "demo-04", "demo-03", 3)) {
    fprintf(stderr, "home_capture: second background full dialog failed\n");
    return 1;
  }
  bd_home_input(home, BD_HOME_ACTION_RIGHT);
  if (!bd_home_input(home, BD_HOME_ACTION_ACCEPT) ||
      bd_home_take_dialog_result(home) != BD_HOME_DIALOG_RESULT_CONFIRM) {
    fprintf(stderr, "home_capture: background full confirm result missing\n");
    return 1;
  }
  if (bd_home_running_count(home) != 3 || bd_home_dialog_open(home)) {
    fprintf(stderr, "home_capture: confirmation changed the running list\n");
    return 1;
  }
  now += 200;
  bd_home_update(home, now);
  if (!bd_home_open_end_dialog(home, "demo-03")) {
    fprintf(stderr, "home_capture: end dialog failed\n");
    return 1;
  }
  now += 200;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_dialog")) {
    return 1;
  }
  bd_home_input(home, BD_HOME_ACTION_RIGHT);
  if (!bd_home_input(home, BD_HOME_ACTION_ACCEPT)) {
    fprintf(stderr, "home_capture: confirm did not start closing\n");
    return 1;
  }
  now += 200;
  bd_home_update(home, now);
  if (bd_home_running_count(home) != 2 || bd_home_dialog_open(home)) {
    fprintf(stderr, "home_capture: close confirm did not clear the app\n");
    return 1;
  }
  now += 200;
  bd_home_update(home, now);
  bd_home_clear_running(home, "demo-03");
  bd_home_clear_running(home, "demo-10");
  bd_home_clear_running(home, "music");
  bd_home_set_focus(home, 5);
  bd_home_set_hint_visible(home, true);
  now += 220;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_hint")) {
    return 1;
  }

  bd_home_set_hint_visible(home, false);
  now += 220;
  bd_home_update(home, now);
  if (!bd_home_start_launch(home, "music")) {
    fprintf(stderr, "home_capture: launch failed\n");
    return 1;
  }
  uint64_t launch_start = now;
  now += 90;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_launch_090")) {
    return 1;
  }

  now = launch_start + 500;
  bd_home_update(home, now);
  if (!bd_home_launch_complete(home)) {
    fprintf(stderr, "home_capture: launch did not complete\n");
    return 1;
  }
  if (!bd_home_start_return(home, "music")) {
    fprintf(stderr, "home_capture: return after launch failed\n");
    return 1;
  }
  now += 420;
  bd_home_update(home, now);
  if (!bd_home_start_launch(home, "music")) {
    fprintf(stderr, "home_capture: second launch failed\n");
    return 1;
  }
  now += 340;
  settle(platform, gfx, home, now);
  if (!write_png(gfx, out_dir, "home_launch_340")) {
    return 1;
  }

  bd_home_destroy(home);
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  unlink(town_path);
  unlink(space_path);
  unlink(music_path);
  rmdir(temp_dir);
  return 0;
}
