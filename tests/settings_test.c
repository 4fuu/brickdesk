#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "brickdesk/config.h"
#include "brickdesk/device_av.h"
#include "brickdesk/gfx.h"
#include "brickdesk/platform.h"
#include "brickdesk/settings.h"
#include "brickdesk/theme.h"
#include "settings_fixture.h"

static int g_failures;

#define CHECK(condition, ...)                     \
  do {                                            \
    if (!(condition)) {                           \
      fprintf(stderr, "settings_test: ");         \
      fprintf(stderr, __VA_ARGS__);               \
      fprintf(stderr, "\n");                      \
      ++g_failures;                               \
    }                                             \
  } while (0)

static bd_config *load_config(const char *dir) {
  bd_config_options options;
  memset(&options, 0, sizeof(options));
  options.config_dir = dir;
  options.migrate = false;
  char error[256] = {0};
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  if (config == NULL) {
    fprintf(stderr, "settings_test: config: %s\n", error);
  }
  return config;
}

static void render(bd_platform *platform, bd_gfx *gfx, bd_settings *settings,
                   uint64_t now, bool full) {
  bd_gfx_begin_frame(gfx);
  bd_settings_update(settings, now);
  if (full) {
    bd_settings_damage(settings);
  }
  bd_settings_draw(settings, gfx);
  bd_gfx_end_frame(gfx);
  if (bd_gfx_get_stats(gfx).presented) {
    bd_platform_present(platform);
  }
}

static int find_row(const bd_settings *settings, const char *key) {
  for (int i = 0; i < bd_settings_row_count(settings); ++i) {
    const char *candidate = bd_settings_row_key(settings, i);
    if (candidate != NULL && strcmp(candidate, key) == 0) {
      return i;
    }
  }
  return -1;
}

static bool focus_row(bd_settings *settings, const char *key) {
  int index = -1;
  for (int i = 0; i < bd_settings_row_count(settings); ++i) {
    const char *candidate = bd_settings_row_key(settings, i);
    if (candidate != NULL && strcmp(candidate, key) == 0) {
      index = i;
      break;
    }
  }
  if (index < 0) {
    fprintf(stderr, "settings_test: row %s not found\n", key);
    return false;
  }
  while (bd_settings_focus(settings) < index) {
    bd_settings_input(settings, BD_SETTINGS_ACTION_DOWN);
  }
  while (bd_settings_focus(settings) > index) {
    bd_settings_input(settings, BD_SETTINGS_ACTION_UP);
  }
  return bd_settings_focus(settings) == index;
}

static bool color_close(bd_color a, bd_color b) {
  return fabsf(a.r - b.r) < 0.01f && fabsf(a.g - b.g) < 0.01f &&
         fabsf(a.b - b.b) < 0.01f;
}

static bool fake_perf_apply(void *user, const char *profile, char *error,
                            size_t error_size) {
  (void)user;
  (void)profile;
  (void)error;
  (void)error_size;
  return true;
}

static bool failing_perf_apply(void *user, const char *profile, char *error,
                               size_t error_size) {
  (void)user;
  (void)profile;
  if (error != NULL && error_size > 0) {
    snprintf(error, error_size, "perfd 未连接");
  }
  return false;
}

static bool write_text_file(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs(text, file) >= 0;
  return fclose(file) == 0 && ok;
}

static bool make_path(const char *path) {
  if (mkdir(path, 0777) == 0) {
    return true;
  }
  return errno == EEXIST;
}

static bool av_set_brightness(void *user, int percent) {
  bd_av_error error;
  return bd_av_brightness_set((bd_av_brightness *)user, percent, &error) ==
         BD_AV_OK;
}

static bool av_get_brightness(void *user, int *percent) {
  bd_av_error error;
  return bd_av_brightness_get((bd_av_brightness *)user, percent, &error) ==
         BD_AV_OK;
}

static void test_real_sysfs_transaction(bd_platform *platform, bd_gfx *gfx,
                                        bd_settings *settings,
                                        bd_config *config) {
  (void)platform;
  (void)gfx;
  char root[] = "/tmp/bd-settings-sysfs-XXXXXX";
  char *temp = mkdtemp(root);
  CHECK(temp != NULL, "fake sysfs temp dir");
  if (temp == NULL) {
    return;
  }
  char path[1024];
  snprintf(path, sizeof(path), "%s/sys", temp);
  CHECK(make_path(path), "fake sysfs sys");
  snprintf(path, sizeof(path), "%s/sys/class", temp);
  CHECK(make_path(path), "fake sysfs class");
  snprintf(path, sizeof(path), "%s/sys/class/backlight", temp);
  CHECK(make_path(path), "fake sysfs backlight");
  snprintf(path, sizeof(path), "%s/sys/class/backlight/test", temp);
  CHECK(make_path(path), "fake sysfs device");
  char brightness_path[2048];
  char max_path[2048];
  snprintf(brightness_path, sizeof(brightness_path), "%s/brightness", path);
  snprintf(max_path, sizeof(max_path), "%s/max_brightness", path);
  CHECK(write_text_file(brightness_path, "64\n"), "fake brightness");
  CHECK(write_text_file(max_path, "255\n"), "fake max brightness");

  bd_av_brightness brightness;
  bd_av_error error;
  bd_av_brightness_config av_config;
  memset(&av_config, 0, sizeof(av_config));
  av_config.sysfs_root = temp;
  CHECK(bd_av_brightness_probe(&brightness, &av_config, &error) == BD_AV_OK,
        "probe fake backlight: %s", error.message);

  bd_settings_system system;
  memset(&system, 0, sizeof(system));
  system.user = &brightness;
  system.set_brightness = av_set_brightness;
  system.get_brightness = av_get_brightness;
  bd_settings_set_system(settings, &system);
  CHECK(bd_config_set(config, "display.brightness", "70", NULL, 0),
        "set config brightness");
  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_DISPLAY),
        "open display for fake sysfs");
  CHECK(focus_row(settings, "display.brightness"),
        "focus brightness for fake sysfs");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_RIGHT),
        "write fake sysfs brightness");
  int percent = -1;
  bd_av_error read_error;
  CHECK(bd_av_brightness_get(&brightness, &percent, &read_error) == BD_AV_OK &&
            percent == 26,
        "fake sysfs readback %d", percent);
  CHECK(bd_config_get_int(config, "display.brightness", -1) == 26,
        "fake sysfs persisted brightness %ld",
        bd_config_get_int(config, "display.brightness", -1));

  CHECK(chmod(brightness_path, 0444) == 0, "make brightness read-only");
  long saved = bd_config_get_int(config, "display.brightness", -1);
  CHECK(!bd_settings_input(settings, BD_SETTINGS_ACTION_RIGHT),
        "read-only fake sysfs must reject the write");
  CHECK(bd_config_get_int(config, "display.brightness", -1) == saved,
        "failed fake sysfs write must not save %ld",
        bd_config_get_int(config, "display.brightness", -1));
  chmod(brightness_path, 0644);
  sf_remove_tree(temp);
}

static void test_schema_describe(bd_config *config) {
  bd_config_schema_info info;
  CHECK(bd_config_describe(config, "theme.mode", &info), "describe theme.mode");
  CHECK(info.type == BD_CONFIG_ENUM, "theme.mode should be an enum");
  CHECK(info.enum_count == 1, "theme.mode enum count %zu", info.enum_count);
  CHECK(!info.required, "theme.mode should not be required");

  CHECK(bd_config_describe(config, "display.brightness", &info),
        "describe display.brightness");
  CHECK(info.type == BD_CONFIG_INT && info.min_value == 0 &&
            info.max_value == 100,
        "brightness range %ld..%ld", info.min_value, info.max_value);

  CHECK(bd_config_describe(config, "audio.volume", &info),
        "describe audio.volume");
  CHECK(info.type == BD_CONFIG_INT && info.min_value == 0 &&
            info.max_value == 100,
        "volume range %ld..%ld", info.min_value, info.max_value);

  CHECK(bd_config_describe(config, "hotkey.menu.combo", &info),
        "describe dynamic hotkey key");
  CHECK(info.type == BD_CONFIG_COMBO, "hotkey combo type %d", (int)info.type);

  CHECK(bd_config_describe(config, "app.org.libretro.retroarch.profile", &info),
        "describe dynamic app key");
  CHECK(info.type == BD_CONFIG_STRING, "app profile type %d", (int)info.type);

  CHECK(!bd_config_describe(config, "no.such.key", &info),
        "unknown key should not describe");
}

static void test_rows(bd_settings *settings) {
  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_APPEARANCE),
        "open appearance");
  static const char *const EXPECTED[] = {"theme.accent", "theme.ambient",
                                         "ui.animations", "ui.blur"};
  CHECK(bd_settings_row_count(settings) == 4, "appearance rows %d",
        bd_settings_row_count(settings));
  for (int i = 0; i < 4 && i < bd_settings_row_count(settings); ++i) {
    CHECK(strcmp(bd_settings_row_key(settings, i), EXPECTED[i]) == 0,
          "appearance row %d is %s", i, bd_settings_row_key(settings, i));
  }

  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_SOUND),
        "open sound");
  CHECK(bd_settings_row_count(settings) == 2, "sound rows %d",
        bd_settings_row_count(settings));
  CHECK(strcmp(bd_settings_row_key(settings, 0), "audio.volume") == 0,
        "sound row 0 %s", bd_settings_row_key(settings, 0));
  CHECK(strcmp(bd_settings_row_key(settings, 1), "audio.mute") == 0,
        "sound row 1 %s", bd_settings_row_key(settings, 1));

  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_DISPLAY),
        "open display");
  CHECK(bd_settings_row_count(settings) == 9, "display rows %d",
        bd_settings_row_count(settings));
  CHECK(strcmp(bd_settings_row_key(settings, 0), "display.brightness") == 0,
        "display row 0 %s", bd_settings_row_key(settings, 0));

  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_BACKGROUND),
        "open background");
  CHECK(bd_settings_row_count(settings) == 2, "background rows %d",
        bd_settings_row_count(settings));
  CHECK(strcmp(bd_settings_row_key(settings, 0), "session.max_background") == 0,
        "background row 0 %s", bd_settings_row_key(settings, 0));
  CHECK(strcmp(bd_settings_row_key(settings, 1),
               "session.memory_budget_mb") == 0,
        "background row 1 %s", bd_settings_row_key(settings, 1));
}

static void test_focus_damage(bd_platform *platform, bd_gfx *gfx,
                              bd_settings *settings) {
  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_APPEARANCE),
        "open appearance for damage");
  render(platform, gfx, settings, 1000, true);
  bd_gfx_stats full = bd_gfx_get_stats(gfx);
  CHECK(full.full_redraw, "first appearance frame should be full");
  CHECK(full.damage_pixels == 1024 * 768, "full damage pixels %lld",
        (long long)full.damage_pixels);

  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_DOWN),
        "move focus to ambient");
  render(platform, gfx, settings, 1016, false);
  bd_gfx_stats partial = bd_gfx_get_stats(gfx);
  CHECK(!partial.full_redraw, "focus move should not redraw the page");
  CHECK(partial.damage_pixels > 50000 && partial.damage_pixels < 200000,
        "focus damage pixels %lld should be two rows",
        (long long)partial.damage_pixels);

  bd_image partial_image;
  memset(&partial_image, 0, sizeof(partial_image));
  CHECK(bd_gfx_read_image(gfx, &partial_image), "read partial frame");

  render(platform, gfx, settings, 1032, true);
  bd_image full_image;
  memset(&full_image, 0, sizeof(full_image));
  CHECK(bd_gfx_read_image(gfx, &full_image), "read full frame");
  CHECK(partial_image.width == full_image.width &&
            partial_image.height == full_image.height,
        "image dimensions differ");
  if (partial_image.pixels != NULL && full_image.pixels != NULL) {
    size_t bytes = (size_t)partial_image.width * (size_t)partial_image.height * 4u;
    size_t mismatches = 0;
    int max_delta = 0;
    const uint8_t *a = partial_image.pixels;
    const uint8_t *b = full_image.pixels;
    for (size_t i = 0; i < bytes; ++i) {
      int delta = (int)a[i] - (int)b[i];
      if (delta < 0) {
        delta = -delta;
      }
      if (delta > 0) {
        ++mismatches;
        if (delta > max_delta) {
          max_delta = delta;
        }
      }
    }
    CHECK(mismatches == 0, "partial frame differs from full: %zu bytes, max %d",
          mismatches, max_delta);
  }
  bd_image_free(&partial_image);
  bd_image_free(&full_image);
}

static void test_values(bd_platform *platform, bd_gfx *gfx,
                        bd_settings *settings, bd_config *config,
                        const char *dir, bd_settings_system_stub *stub) {
  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_APPEARANCE),
        "open appearance for values");
  CHECK(focus_row(settings, "theme.accent"), "focus theme.accent");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_RIGHT),
        "cycle accent");
  CHECK(strcmp(bd_config_get_string(config, "theme.accent", ""), "coral") == 0,
        "accent should be coral, got %s",
        bd_config_get_string(config, "theme.accent", ""));
  bd_color coral;
  CHECK(bd_color_from_hex(bd_config_accent_hex("coral", true), &coral),
        "coral hex");
  CHECK(color_close(bd_settings_accent(settings), coral),
        "settings accent should follow config");
  char local[8192];
  snprintf(local, sizeof(local), "%s/local.ini", dir);
  CHECK(sf_file_contains(local, "accent = coral"),
        "local.ini should contain accent = coral");
  render(platform, gfx, settings, 1100, false);
  CHECK(bd_gfx_get_stats(gfx).full_redraw,
        "accent change should recolour the whole page");

  CHECK(focus_row(settings, "ui.animations"), "focus ui.animations");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT),
        "toggle animations off");
  CHECK(!bd_config_get_bool(config, "ui.animations", true),
        "animations should be off");
  CHECK(sf_file_contains(local, "animations = false"),
        "local.ini should contain animations = false");

  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_SOUND),
        "open sound for volume");
  CHECK(focus_row(settings, "audio.volume"), "focus volume");
  bd_settings_update(settings, 2000);
  bd_settings_hold(settings, BD_SETTINGS_ACTION_RIGHT, true);
  CHECK(bd_config_get_int(config, "audio.volume", -1) == 71,
        "volume after press %ld", bd_config_get_int(config, "audio.volume", -1));
  bd_settings_update(settings, 2300);
  CHECK(bd_config_get_int(config, "audio.volume", -1) == 71,
        "volume should wait for repeat delay");
  bd_settings_update(settings, 2400);
  CHECK(bd_config_get_int(config, "audio.volume", -1) == 72,
        "volume after first repeat %ld",
        bd_config_get_int(config, "audio.volume", -1));
  bd_settings_update(settings, 2480);
  CHECK(bd_config_get_int(config, "audio.volume", -1) == 73,
        "volume after second repeat %ld",
        bd_config_get_int(config, "audio.volume", -1));
  bd_settings_hold(settings, BD_SETTINGS_ACTION_RIGHT, false);
  CHECK(sf_file_contains(local, "volume = 73"),
        "local.ini should contain volume = 73");
  CHECK(stub->volume == 73 && stub->volume_calls == 3,
        "stub volume %d calls %d", stub->volume, stub->volume_calls);

  bd_config *reloaded = load_config(dir);
  CHECK(reloaded != NULL, "reload config");
  if (reloaded != NULL) {
    CHECK(bd_config_get_int(reloaded, "audio.volume", -1) == 73,
          "reloaded volume %ld",
          bd_config_get_int(reloaded, "audio.volume", -1));
    CHECK(strcmp(bd_config_get_string(reloaded, "theme.accent", ""), "coral") ==
              0,
          "reloaded accent %s",
          bd_config_get_string(reloaded, "theme.accent", ""));
    bd_config_free(reloaded);
  }

  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_DISPLAY),
        "open display for brightness");
  CHECK(focus_row(settings, "display.brightness"), "focus brightness");
  render(platform, gfx, settings, 3000, true);
  bd_settings_hold(settings, BD_SETTINGS_ACTION_LEFT, true);
  render(platform, gfx, settings, 3016, false);
  bd_gfx_stats value = bd_gfx_get_stats(gfx);
  CHECK(!value.full_redraw && value.damage_pixels > 0 &&
            value.damage_pixels < 120000,
        "value change should repaint one row (%lld px)",
        (long long)value.damage_pixels);
  bd_settings_hold(settings, BD_SETTINGS_ACTION_LEFT, false);
  CHECK(bd_config_get_int(config, "display.brightness", -1) == 69,
        "brightness %ld", bd_config_get_int(config, "display.brightness", -1));
  CHECK(stub->brightness == 69 && stub->brightness_calls == 1,
        "stub brightness %d calls %d", stub->brightness,
        stub->brightness_calls);
}

static void test_pages(bd_settings *settings, bd_config *config) {
  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_KEYS), "open keys");
  bd_settings_stats stats = bd_settings_get_stats(settings);
  CHECK(stats.hotkeys == 5, "hotkeys %d", stats.hotkeys);
  CHECK(stats.conflicts >= 1, "conflicts %d", stats.conflicts);
  CHECK(strcmp(bd_settings_row_key(settings, 0), "!conflict.0") == 0,
        "first keys row %s", bd_settings_row_key(settings, 0));
  int menu = find_row(settings, "!hotkey.menu");
  CHECK(menu >= 0, "hotkey.menu row");
  CHECK(find_row(settings, "!hotkey.screenshot") >= 0,
        "hotkey.screenshot row");

  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_PERFORMANCE),
        "open performance");
  CHECK(strcmp(bd_settings_row_key(settings, 0), "perf.global_profile") == 0,
        "perf row 0 %s", bd_settings_row_key(settings, 0));
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_RIGHT),
        "cycle global profile");
  CHECK(strcmp(bd_config_get_string(config, "perf.global_profile", ""),
               "auto") == 0,
        "global profile %s",
        bd_config_get_string(config, "perf.global_profile", ""));
  int app = find_row(settings, "app.org.libretro.retroarch.profile");
  CHECK(app >= 0, "per-app profile row");
  if (app >= 0) {
    CHECK(focus_row(settings, "app.org.libretro.retroarch.profile"),
          "focus app profile");
    CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_RIGHT),
          "cycle app profile");
    CHECK(strcmp(bd_config_get_string(
                     config, "app.org.libretro.retroarch.profile", ""),
                 "powersave") == 0,
          "app profile %s",
          bd_config_get_string(config, "app.org.libretro.retroarch.profile", ""));
  }

  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_APPEARANCE),
        "open appearance for last row");
  int last = bd_settings_row_count(settings) - 1;
  CHECK(last >= 0, "appearance has rows");
  if (last >= 0) {
    CHECK(focus_row(settings, bd_settings_row_key(settings, last)),
          "focus last appearance row");
    bd_settings_stats last_stats = bd_settings_get_stats(settings);
    CHECK(bd_settings_focus(settings) == last,
          "last row focus %d != %d", bd_settings_focus(settings), last);
    CHECK(last_stats.focus_visible, "last row is not fully visible");
  }

  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_PLUGINS),
        "open empty plugins page");
  bd_settings_stats empty = bd_settings_get_stats(settings);
  CHECK(empty.plugins == 0 && empty.rows == 1,
        "empty plugins rows %d plugins %d", empty.rows, empty.plugins);
  CHECK(empty.focus_visible, "empty plugin row is not fully visible");

  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_BACKGROUND),
        "open background");
  CHECK(focus_row(settings, "session.max_background"), "focus max background");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_RIGHT),
        "increase max background");
  CHECK(bd_config_get_int(config, "session.max_background", -1) == 4,
        "max background %ld",
        bd_config_get_int(config, "session.max_background", -1));
}

typedef struct reset_fail_av {
  int brightness_calls;
  int volume_calls;
  int mute_calls;
} reset_fail_av;

static bool reset_fail_brightness(void *user, int percent) {
  (void)percent;
  ((reset_fail_av *)user)->brightness_calls++;
  return false;
}

static bool reset_fail_volume(void *user, int percent) {
  (void)percent;
  ((reset_fail_av *)user)->volume_calls++;
  return false;
}

static bool reset_fail_mute(void *user, bool muted) {
  (void)muted;
  ((reset_fail_av *)user)->mute_calls++;
  return false;
}

static bool reset_fail_brightness_get(void *user, int *percent) {
  (void)user;
  *percent = 80;
  return true;
}

static bool reset_fail_volume_get(void *user, int *percent) {
  (void)user;
  *percent = 40;
  return true;
}

static bool reset_fail_mute_get(void *user, bool *muted) {
  (void)user;
  *muted = true;
  return true;
}

typedef struct reset_perf_probe {
  int calls;
  char last[64];
  bool reject;
} reset_perf_probe;

static bool reset_perf_apply(void *user, const char *profile, char *error,
                             size_t error_size) {
  reset_perf_probe *probe = (reset_perf_probe *)user;
  probe->calls++;
  snprintf(probe->last, sizeof(probe->last), "%s", profile);
  if (probe->reject) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "perfd 拒绝重置");
    }
    return false;
  }
  return true;
}

static void confirm_reset(bd_settings *settings) {
  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_SYSTEM),
        "open system for reset");
  CHECK(focus_row(settings, "!reset"), "focus reset row");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT),
        "open reset confirm");
  CHECK(bd_settings_confirm_open(settings), "reset confirm open");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_RIGHT),
        "focus reset action");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT),
        "confirm reset");
  CHECK(!bd_settings_confirm_open(settings), "reset confirm closes");
}

static void test_reset_service_failures(bd_settings *settings,
                                        bd_config *config,
                                        bd_settings_system *working_system) {
  char error[128] = {0};
  CHECK(bd_config_set(config, "display.brightness", "80", error,
                      sizeof(error)),
        "reset failure seed brightness");
  CHECK(bd_config_set(config, "audio.volume", "40", error, sizeof(error)),
        "reset failure seed volume");
  CHECK(bd_config_set(config, "audio.mute", "true", error, sizeof(error)),
        "reset failure seed mute");
  CHECK(bd_config_set(config, "perf.global_profile", "balanced", error,
                      sizeof(error)),
        "reset failure seed profile");

  reset_fail_av fail_av;
  memset(&fail_av, 0, sizeof(fail_av));
  bd_settings_system failing_system;
  memset(&failing_system, 0, sizeof(failing_system));
  failing_system.user = &fail_av;
  failing_system.set_brightness = reset_fail_brightness;
  failing_system.set_volume = reset_fail_volume;
  failing_system.set_muted = reset_fail_mute;
  failing_system.get_brightness = reset_fail_brightness_get;
  failing_system.get_volume = reset_fail_volume_get;
  failing_system.get_muted = reset_fail_mute_get;
  bd_settings_set_system(settings, &failing_system);

  reset_perf_probe perf_probe;
  memset(&perf_probe, 0, sizeof(perf_probe));
  perf_probe.reject = true;
  bd_settings_perf rejected = {.user = &perf_probe,
                               .apply_global_profile = reset_perf_apply};
  bd_settings_set_perf(settings, &rejected);
  confirm_reset(settings);

  CHECK(fail_av.brightness_calls > 0 && fail_av.volume_calls > 0 &&
            fail_av.mute_calls > 0,
        "reset calls each AV setter (%d/%d/%d)", fail_av.brightness_calls,
        fail_av.volume_calls, fail_av.mute_calls);
  CHECK(bd_config_get_int(config, "display.brightness", -1) == 80,
        "failed AV reset keeps brightness %ld",
        bd_config_get_int(config, "display.brightness", -1));
  CHECK(bd_config_get_int(config, "audio.volume", -1) == 40,
        "failed AV reset keeps volume %ld",
        bd_config_get_int(config, "audio.volume", -1));
  CHECK(bd_config_get_bool(config, "audio.mute", false),
        "failed AV reset keeps mute");
  CHECK(strcmp(bd_config_get_string(config, "perf.global_profile", ""),
               "balanced") == 0,
        "rejected perf reset keeps profile %s",
        bd_config_get_string(config, "perf.global_profile", ""));
  CHECK(perf_probe.calls == 1 && strcmp(perf_probe.last, "default") == 0,
        "reset applies the perf default through confirmation (calls=%d last=%s)",
        perf_probe.calls, perf_probe.last);
  CHECK(bd_settings_last_error(settings)[0] != '\0',
        "partial reset surfaces an error");

  CHECK(bd_config_set(config, "display.brightness", "80", error,
                      sizeof(error)),
        "disconnected reset seed brightness");
  CHECK(bd_config_set(config, "audio.volume", "40", error, sizeof(error)),
        "disconnected reset seed volume");
  CHECK(bd_config_set(config, "perf.global_profile", "performance", error,
                      sizeof(error)),
        "disconnected reset seed profile");
  bd_settings_set_system(settings, working_system);
  bd_settings_set_perf(settings, NULL);
  confirm_reset(settings);

  CHECK(bd_config_get_int(config, "display.brightness", -1) == 70,
        "working AV still resets brightness to %ld",
        bd_config_get_int(config, "display.brightness", -1));
  CHECK(bd_config_get_int(config, "audio.volume", -1) == 70,
        "working AV still resets volume to %ld",
        bd_config_get_int(config, "audio.volume", -1));
  CHECK(strcmp(bd_config_get_string(config, "perf.global_profile", ""),
               "performance") == 0,
        "disconnected perf keeps profile %s",
        bd_config_get_string(config, "perf.global_profile", ""));
  CHECK(strstr(bd_settings_last_error(settings), "perf") != NULL,
        "disconnected perf reset error %s",
        bd_settings_last_error(settings));
}

static bool wait_for_plugin_long(bd_plugin_host *host, bd_plugin *plugin,
                                uint64_t timeout_ms) {
  uint64_t deadline = sf_now_ms() + timeout_ms;
  while (sf_now_ms() < deadline) {
    bd_plugin_host_poll(host, 10);
    if (bd_plugin_ui_settings(plugin) != NULL) {
      return true;
    }
  }
  return bd_plugin_ui_settings(plugin) != NULL;
}

static void test_plugin_scroll(bd_platform *platform, bd_gfx *gfx,
                               bd_settings *settings) {
  char plugin_dir[2048] = {0};
  if (!sf_make_fixture_plugin(plugin_dir, sizeof(plugin_dir), "long")) {
    CHECK(false, "long plugin: create fixture directory");
    return;
  }
  char runtime_template[] = "/tmp/bd-settings-long-runtime-XXXXXX";
  char *runtime_dir = mkdtemp(runtime_template);
  if (runtime_dir == NULL) {
    CHECK(false, "long plugin: create runtime directory");
    sf_remove_tree(plugin_dir);
    return;
  }
  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.idle_stop_ms = 0;
  bd_plugin_host_config host_config;
  memset(&host_config, 0, sizeof(host_config));
  host_config.plugin_dir = plugin_dir;
  host_config.runtime_dir = runtime_dir;
  host_config.limits = &limits;
  host_config.display_width = 1024;
  host_config.display_height = 768;
  host_config.dark = true;
  host_config.accent = 0xe07a5f;
  host_config.locale = "zh-CN";
  bd_plugin_host *host = bd_plugin_host_create(&host_config);
  CHECK(host != NULL, "long plugin: host");
  if (host == NULL) {
    sf_remove_tree(runtime_dir);
    sf_remove_tree(plugin_dir);
    return;
  }
  char error[256] = {0};
  CHECK(bd_plugin_host_scan(host, error, sizeof(error)) == 1,
        "long plugin: scan (%s)", error);
  bd_plugin *plugin = bd_plugin_host_find(host, "com.test.long");
  CHECK(plugin != NULL, "long plugin: find");
  if (plugin == NULL) {
    bd_plugin_host_destroy(host);
    sf_remove_tree(runtime_dir);
    sf_remove_tree(plugin_dir);
    return;
  }
  bd_plugin_activate(plugin);
  CHECK(wait_for_plugin_long(host, plugin, 5000),
        "long plugin: settings UI did not arrive");
  bd_settings_set_plugins(settings, host);
  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_PLUGINS),
        "long plugin: open plugins page");
  CHECK(focus_row(settings, "!plugin.com.test.long"),
        "long plugin: focus row");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT),
        "long plugin: open settings subpage");
  CHECK(bd_settings_subpage(settings), "long plugin: subpage open");

  bd_settings_stats stats = bd_settings_get_stats(settings);
  CHECK(stats.plugin_nodes >= 10, "long plugin: nodes %d", stats.plugin_nodes);
  int last = stats.plugin_nodes - 1;
  for (int i = 0; i < stats.plugin_nodes + 4; ++i) {
    bd_settings_input(settings, BD_SETTINGS_ACTION_DOWN);
  }
  stats = bd_settings_get_stats(settings);
  CHECK(bd_settings_focus(settings) == last, "long plugin: focus %d != %d",
        bd_settings_focus(settings), last);
  CHECK(stats.scroll > 0, "long plugin: expected scrolling, got %d",
        stats.scroll);
  CHECK(stats.focus_visible, "long plugin: last control is not visible");

  for (int i = 0; i < stats.plugin_nodes; ++i) {
    bd_settings_input(settings, BD_SETTINGS_ACTION_UP);
  }
  stats = bd_settings_get_stats(settings);
  CHECK(bd_settings_focus(settings) == 0,
        "long plugin: scroll back focus %d != 0", bd_settings_focus(settings));
  CHECK(stats.focus_visible,
        "long plugin: first control is not visible after scrolling back");

  for (int i = 0; i < stats.plugin_nodes - 1; ++i) {
    bd_settings_input(settings, BD_SETTINGS_ACTION_DOWN);
  }
  stats = bd_settings_get_stats(settings);
  CHECK(bd_settings_focus(settings) == last,
        "long plugin: return focus %d != %d", bd_settings_focus(settings),
        last);
  CHECK(stats.focus_visible,
        "long plugin: last control is not visible after returning");
  render(platform, gfx, settings, 6000, true);

  const bd_plugin_ui *ui = bd_plugin_ui_settings(plugin);
  CHECK(ui != NULL, "long plugin: UI");
  int generation = bd_plugin_ui_generation(plugin);
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT),
        "long plugin: activate last control");
  uint64_t deadline = sf_now_ms() + 2000;
  while (sf_now_ms() < deadline &&
         bd_plugin_ui_generation(plugin) == generation) {
    bd_plugin_host_poll(host, 10);
  }
  CHECK(bd_plugin_ui_generation(plugin) > generation,
        "long plugin: fixture did not receive ui.activate");

  bd_settings_input(settings, BD_SETTINGS_ACTION_CANCEL);
  bd_settings_set_plugins(settings, NULL);
  bd_plugin_host_stop_all(host);
  bd_plugin_host_destroy(host);
  sf_remove_tree(runtime_dir);
  sf_remove_tree(plugin_dir);
}

static void test_reset_and_subpages(bd_platform *platform, bd_gfx *gfx,
                                    bd_settings *settings, bd_config *config) {
  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_SYSTEM),
        "open system");
  CHECK(focus_row(settings, "!reset"), "focus reset row");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT),
        "open reset confirm");
  CHECK(bd_settings_confirm_open(settings), "confirm should be open");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT),
        "cancel reset");
  CHECK(!bd_settings_confirm_open(settings), "confirm should close");
  CHECK(strcmp(bd_config_get_string(config, "theme.accent", ""), "coral") == 0,
        "cancel should keep the accent");

  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT),
        "reopen reset confirm for focus roundtrip");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_RIGHT),
        "focus reset button");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_LEFT),
        "return focus to cancel");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT),
        "cancel after focus roundtrip");
  CHECK(!bd_settings_confirm_open(settings),
        "focus roundtrip should cancel");
  CHECK(strcmp(bd_config_get_string(config, "theme.accent", ""), "coral") == 0,
        "roundtrip cancel should keep the accent");

  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT),
        "reopen reset confirm for B");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_CANCEL),
        "B closes reset confirm");
  CHECK(!bd_settings_confirm_open(settings), "B should cancel");
  CHECK(strcmp(bd_config_get_string(config, "theme.accent", ""), "coral") == 0,
        "B cancel should keep the accent");

  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT),
        "reopen reset confirm");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_RIGHT),
        "focus reset button");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT), "reset");
  CHECK(!bd_settings_confirm_open(settings), "confirm should close after reset");
  CHECK(strcmp(bd_config_get_string(config, "theme.accent", ""), "persimmon") ==
            0,
        "reset accent %s", bd_config_get_string(config, "theme.accent", ""));
  CHECK(bd_config_get_int(config, "audio.volume", -1) == 70,
        "reset volume %ld", bd_config_get_int(config, "audio.volume", -1));
  CHECK(bd_config_get_int(config, "display.brightness", -1) == 70,
        "reset brightness %ld",
        bd_config_get_int(config, "display.brightness", -1));
  CHECK(bd_config_get_bool(config, "ui.animations", false),
        "reset animations");
  CHECK(strcmp(bd_config_get_string(config, "perf.global_profile", ""),
               "default") == 0,
        "reset profile %s",
        bd_config_get_string(config, "perf.global_profile", ""));

  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_APPEARANCE),
        "open appearance for accent subpage");
  CHECK(focus_row(settings, "theme.accent"), "focus accent");
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_ACCEPT),
        "open accent subpage");
  CHECK(bd_settings_subpage(settings), "accent subpage should be open");
  CHECK(strcmp(bd_settings_row_key(settings, 0), "!accent.r") == 0,
        "accent row 0 %s", bd_settings_row_key(settings, 0));
  CHECK(bd_settings_row_count(settings) == 3, "accent rows %d",
        bd_settings_row_count(settings));
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_RIGHT),
        "increase red");
  const char *accent = bd_config_get_string(config, "theme.accent", "");
  CHECK(accent[0] == '#', "custom accent %s", accent);
  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_CANCEL),
        "leave accent subpage");
  CHECK(!bd_settings_subpage(settings), "subpage should close");
  CHECK(strcmp(bd_settings_row_key(settings, bd_settings_focus(settings)),
               "theme.accent") == 0,
        "focus should return to accent");

  render(platform, gfx, settings, 3900, false);
  bd_settings_set_hint_visible(settings, true);
  render(platform, gfx, settings, 4000, false);
  bd_gfx_stats hint = bd_gfx_get_stats(gfx);
  CHECK(hint.damage_pixels == 1024 * 60, "hint damage %lld",
        (long long)hint.damage_pixels);
  bd_settings_set_hint_visible(settings, false);
  render(platform, gfx, settings, 4016, false);
  CHECK(bd_gfx_get_stats(gfx).damage_pixels == 1024 * 60,
        "hiding the hint should repaint its strip");

  CHECK(bd_settings_input(settings, BD_SETTINGS_ACTION_CANCEL),
        "close settings");
  CHECK(!bd_settings_is_open(settings), "settings should be closed");
}

int main(void) {
  char dir[1024] = {0};
  if (!sf_make_config_dir(dir, sizeof(dir), true)) {
    fprintf(stderr, "settings_test: cannot create config dir\n");
    return 1;
  }
  bd_config *config = load_config(dir);
  if (config == NULL) {
    sf_remove_tree(dir);
    return 1;
  }

  bd_platform_config platform_config;
  memset(&platform_config, 0, sizeof(platform_config));
  platform_config.title = "brickdesk settings test";
  platform_config.width = 1024;
  platform_config.height = 768;
  platform_config.hidden = true;
  bd_platform *platform = bd_platform_create(&platform_config);
  bd_gfx *gfx = platform != NULL ? bd_gfx_create(1024, 768) : NULL;
  if (platform == NULL || gfx == NULL) {
    bd_config_free(config);
    sf_remove_tree(dir);
    return 1;
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
    fprintf(stderr, "settings_test: settings create failed\n");
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    bd_config_free(config);
    sf_remove_tree(dir);
    return 1;
  }
  bd_settings_status status;
  memset(&status, 0, sizeof(status));
  status.clock = "22:26";
  status.wifi_known = true;
  status.wifi = true;
  status.battery_percent = 82;
  bd_settings_set_status(settings, &status);
  bd_settings_storage storage = {58 * 1024, 12 * 1024};
  bd_settings_set_storage(settings, &storage);
  bd_settings_system_stub stub;
  bd_settings_system system;
  bd_settings_system_stub_init(&stub, &system);
  bd_settings_set_system(settings, &system);
  bd_settings_perf perf = {.user = NULL,
                           .apply_global_profile = fake_perf_apply};
  bd_settings_set_perf(settings, &perf);

  test_schema_describe(config);
  test_rows(settings);
  test_focus_damage(platform, gfx, settings);
  test_values(platform, gfx, settings, config, dir, &stub);
  test_real_sysfs_transaction(platform, gfx, settings, config);
  /* The fake sysfs tree is gone; restore the explicit test backend. */
  bd_settings_set_system(settings, &system);
  test_pages(settings, config);
  test_plugin_scroll(platform, gfx, settings);
  test_reset_and_subpages(platform, gfx, settings, config);
  test_reset_service_failures(settings, config, &system);

  const char *profile =
      bd_config_get_string(config, "perf.global_profile", "default");
  char saved_profile[64];
  snprintf(saved_profile, sizeof(saved_profile), "%s", profile);
  bd_settings_perf rejected = {.user = NULL,
                               .apply_global_profile = failing_perf_apply};
  bd_settings_set_perf(settings, &rejected);
  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_PERFORMANCE),
        "open performance for rejection");
  CHECK(focus_row(settings, "perf.global_profile"),
        "focus perf for rejection");
  CHECK(!bd_settings_input(settings, BD_SETTINGS_ACTION_RIGHT),
        "rejected perf must not report success");
  CHECK(strcmp(bd_config_get_string(config, "perf.global_profile", ""),
               saved_profile) == 0,
        "rejected perf must not save %s",
        bd_config_get_string(config, "perf.global_profile", ""));
  CHECK(strcmp(bd_settings_last_error(settings), "perfd 未连接") == 0,
        "perf error %s", bd_settings_last_error(settings));

  bd_settings_set_perf(settings, NULL);
  CHECK(!bd_settings_input(settings, BD_SETTINGS_ACTION_RIGHT),
        "missing perf service must not report success");
  CHECK(strcmp(bd_config_get_string(config, "perf.global_profile", ""),
               saved_profile) == 0,
        "missing perf must not save");

  int saved_brightness = (int)bd_config_get_int(config, "display.brightness", -1);
  bd_settings_set_system(settings, NULL);
  CHECK(bd_settings_open(settings, BD_SETTINGS_CATEGORY_DISPLAY),
        "open display without a system backend");
  CHECK(focus_row(settings, "display.brightness"),
        "focus brightness without a backend");
  CHECK(!bd_settings_input(settings, BD_SETTINGS_ACTION_LEFT),
        "missing device must not report a write");
  CHECK(bd_config_get_int(config, "display.brightness", -1) ==
            saved_brightness,
        "missing device must not save brightness");
  bd_settings_set_system(settings, &system);

  bd_settings_destroy(settings);
  bd_config_free(config);
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  sf_remove_tree(dir);

  if (g_failures != 0) {
    fprintf(stderr, "settings_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("settings_test: all checks passed\n");
  return 0;
}
