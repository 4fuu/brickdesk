#define _XOPEN_SOURCE 700

#include "brickdesk/config.h"
#include "brickdesk/gfx.h"
#include "brickdesk/home.h"
#include "brickdesk/platform.h"
#include "brickdesk/settings.h"
#include "brickdesk/shell.h"
#include "settings_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef BRICKDESK_TEST_BATTERY_FIXTURE
#define BRICKDESK_TEST_BATTERY_FIXTURE "tests/fixtures/battery/full"
#endif

static int failures;

#define CHECK(condition, ...)                                                  \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "shell_settings_test: FAIL: ");                          \
      fprintf(stderr, __VA_ARGS__);                                            \
      fputc('\n', stderr);                                                     \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

typedef struct test_runtime {
  bd_platform *platform;
  bd_gfx *gfx;
  bd_config *config;
  bd_shell *shell;
  bd_settings *settings;
  bd_settings_system_stub stub;
  char config_dir[512];
  char apps_dir[1024];
  char state_dir[1024];
  char socket_dir[1024];
  char frame_dir[1024];
  char perf_socket[2048];
  char sysfs_root[2048];
  pid_t perfd;
} test_runtime;

static bool make_dir(const char *path) {
  if (mkdir(path, 0777) == 0) {
    return true;
  }
  return errno == EEXIST;
}

static bool join(char *out, size_t size, const char *directory,
                 const char *name) {
  int written = snprintf(out, size, "%s/%s", directory, name);
  return written > 0 && (size_t)written < size;
}

static bool write_text(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs(text, file) >= 0;
  return fclose(file) == 0 && ok;
}

static void sleep_ms(int milliseconds) {
  struct timespec delay;
  delay.tv_sec = milliseconds / 1000;
  delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
  nanosleep(&delay, NULL);
}

static bool wait_for_path(const char *path, int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 10) {
    if (access(path, F_OK) == 0) {
      return true;
    }
    sleep_ms(10);
  }
  return false;
}

static void stop_process(pid_t pid) {
  if (pid <= 0) {
    return;
  }
  kill(pid, SIGTERM);
  for (int attempt = 0; attempt < 200; ++attempt) {
    int status = 0;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD)) {
      return;
    }
    sleep_ms(10);
  }
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
}

static bool prepare_fake_sysfs(const char *root) {
  char path[4096];
  static const char *const DIRS[] = {
      "sys/devices/system/cpu/cpufreq/policy0",
      "sys/devices/system/cpu/cpu1",
      "sys/devices/system/cpu/cpu2",
      "sys/devices/system/cpu/cpu3",
      "proc/sys/vm",
  };
  if (!make_dir(root)) {
    return false;
  }
  for (size_t i = 0; i < sizeof(DIRS) / sizeof(DIRS[0]); ++i) {
    char current[2048];
    snprintf(current, sizeof(current), "%s", root);
    char *parts = (char *)DIRS[i];
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", parts);
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part != NULL;
         part = strtok_r(NULL, "/", &save)) {
      size_t used = strlen(current);
      snprintf(current + used, sizeof(current) - used, "/%s", part);
      if (!make_dir(current)) {
        return false;
      }
    }
  }
  char policy[4096];
  snprintf(policy, sizeof(policy), "%s/sys/devices/system/cpu/cpufreq/policy0",
           root);
  static const struct {
    const char *name;
    const char *value;
  } FILES[] = {
      {"scaling_governor", "schedutil\n"},
      {"scaling_min_freq", "408000\n"},
      {"scaling_max_freq", "1200000\n"},
      {"cpuinfo_min_freq", "408000\n"},
      {"cpuinfo_max_freq", "2000000\n"},
      {"scaling_cur_freq", "1200000\n"},
      {"scaling_available_frequencies",
       "408000 600000 816000 1008000 1200000 1416000 1608000 1800000 "
       "2000000\n"},
      {"scaling_available_governors", "schedutil ondemand performance powersave\n"},
  };
  for (size_t i = 0; i < sizeof(FILES) / sizeof(FILES[0]); ++i) {
    if (!join(path, sizeof(path), policy, FILES[i].name) ||
        !write_text(path, FILES[i].value)) {
      return false;
    }
  }
  if (!join(path, sizeof(path), root, "sys/devices/system/cpu/present") ||
      !write_text(path, "0-3\n")) {
    return false;
  }
  for (int cpu = 1; cpu <= 3; ++cpu) {
    snprintf(path, sizeof(path), "%s/sys/devices/system/cpu/cpu%d/online",
             root, cpu);
    if (!write_text(path, "1\n")) {
      return false;
    }
  }
  if (!join(path, sizeof(path), root, "proc/sys/vm/swappiness") ||
      !write_text(path, "20\n")) {
    return false;
  }
  return true;
}

static pid_t start_perfd(const char *perfd, const char *config_dir,
                         const char *sysfs_root, const char *socket_path) {
  pid_t pid = fork();
  if (pid != 0) {
    return pid;
  }
  int null_fd = open("/dev/null", O_WRONLY);
  if (null_fd >= 0) {
    dup2(null_fd, STDOUT_FILENO);
    dup2(null_fd, STDERR_FILENO);
    close(null_fd);
  }
  execl(perfd, perfd, "--config-dir", config_dir, "--sysfs-root", sysfs_root,
        "--socket", socket_path, (char *)NULL);
  _exit(127);
}

static void key_press(bd_shell *shell, bd_key key, uint64_t time_ms) {
  bd_shell_test_key(shell, key, true, time_ms);
  bd_shell_test_key(shell, key, false, time_ms + 10);
}

static bool read_text(const char *path, char *out, size_t out_size) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  bool ok = fgets(out, (int)out_size, file) != NULL;
  fclose(file);
  if (!ok) {
    return false;
  }
  out[strcspn(out, "\r\n")] = '\0';
  return true;
}

static bool setup_runtime(test_runtime *runtime, const char *perfd,
                          int low_battery_percent) {
  memset(runtime, 0, sizeof(*runtime));
  runtime->perfd = -1;
  if (!sf_make_config_dir(runtime->config_dir, sizeof(runtime->config_dir),
                          false)) {
    return false;
  }
  if (!join(runtime->apps_dir, sizeof(runtime->apps_dir), runtime->config_dir,
            "apps") ||
      !make_dir(runtime->apps_dir)) {
    return false;
  }
  char app_dir[2048];
  char manifest[2048];
  if (!join(app_dir, sizeof(app_dir), runtime->apps_dir, "real-app") ||
      !make_dir(app_dir) || !join(manifest, sizeof(manifest), app_dir,
                                  "app.json") ||
      !write_text(manifest,
                  "{\"schema\":1,\"id\":\"test.real\","
                  "\"name\":\"Real App\",\"version\":\"1.0\","
                  "\"type\":\"app\",\"category\":\"app\","
                  "\"exec\":[\"/bin/true\"],\"cwd\":\".\","
                  "\"capabilities\":{},\"hooks\":{},"
                  "\"accent\":\"#336699\"}\n")) {
    return false;
  }
  if (!join(runtime->state_dir, sizeof(runtime->state_dir),
            runtime->config_dir, "state") ||
      !make_dir(runtime->state_dir) ||
      !join(runtime->socket_dir, sizeof(runtime->socket_dir),
            runtime->config_dir, "sockets") ||
      !make_dir(runtime->socket_dir) ||
      !join(runtime->frame_dir, sizeof(runtime->frame_dir),
            runtime->config_dir, "frames") ||
      !make_dir(runtime->frame_dir)) {
    return false;
  }
  snprintf(runtime->perf_socket, sizeof(runtime->perf_socket), "%s/perfd.sock",
           runtime->config_dir);
  snprintf(runtime->sysfs_root, sizeof(runtime->sysfs_root), "%s/sysfs",
           runtime->config_dir);
  if (!prepare_fake_sysfs(runtime->sysfs_root)) {
    return false;
  }
  setenv("BRICKDESK_PERF_SOCKET", runtime->perf_socket, 1);
  runtime->perfd = start_perfd(perfd, runtime->config_dir,
                               runtime->sysfs_root, runtime->perf_socket);
  if (runtime->perfd <= 0 ||
      !wait_for_path(runtime->perf_socket, 3000)) {
    fprintf(stderr, "shell_settings_test: perfd did not start\n");
    return false;
  }

  bd_config_options options;
  memset(&options, 0, sizeof(options));
  options.config_dir = runtime->config_dir;
  options.migrate = false;
  char error[256] = {0};
  runtime->config = bd_config_load(&options, error, sizeof(error));
  if (runtime->config == NULL) {
    fprintf(stderr, "shell_settings_test: config: %s\n", error);
    return false;
  }
  CHECK(bd_config_set(runtime->config, "display.brightness", "80", error,
                      sizeof(error)),
        "seed brightness: %s", error);
  CHECK(bd_config_set(runtime->config, "audio.volume", "40", error,
                      sizeof(error)),
        "seed volume: %s", error);
  char low_battery[16];
  snprintf(low_battery, sizeof(low_battery), "%d", low_battery_percent);
  CHECK(bd_config_set(runtime->config, "power.low_battery_percent", low_battery,
                      error, sizeof(error)),
        "seed low battery threshold %s: %s", low_battery, error);
  CHECK(bd_config_save(runtime->config, error, sizeof(error)),
        "save seeded config: %s", error);

  bd_settings_system system;
  bd_settings_system_stub_init(&runtime->stub, &system);
  runtime->stub.brightness = 80;
  runtime->stub.volume = 40;

  bd_platform_config platform_config;
  memset(&platform_config, 0, sizeof(platform_config));
  platform_config.title = "brickdesk shell settings test";
  platform_config.width = 1024;
  platform_config.height = 768;
  platform_config.hidden = true;
  runtime->platform = bd_platform_create(&platform_config);
  if (runtime->platform == NULL) {
    return false;
  }
  runtime->gfx = bd_gfx_create(1024, 768);
  if (runtime->gfx == NULL) {
    return false;
  }

  bd_shell_config shell_config;
  memset(&shell_config, 0, sizeof(shell_config));
  shell_config.platform = runtime->platform;
  shell_config.gfx = runtime->gfx;
  shell_config.settings = runtime->config;
  shell_config.apps_dir = runtime->apps_dir;
  shell_config.session_socket_dir = runtime->socket_dir;
  shell_config.session_frame_dir = runtime->frame_dir;
  shell_config.session_state_root = runtime->state_dir;
  shell_config.battery_root = BRICKDESK_TEST_BATTERY_FIXTURE;
  shell_config.settings_system = &system;
  shell_config.verbose = false;
  runtime->shell = bd_shell_create(&shell_config);
  if (runtime->shell == NULL) {
    fprintf(stderr, "shell_settings_test: shell create failed\n");
    return false;
  }
  runtime->settings = bd_shell_settings(runtime->shell);
  CHECK(bd_shell_app_count(runtime->shell) == 1,
        "one real app is scanned: %zu", bd_shell_app_count(runtime->shell));
  CHECK(runtime->settings != NULL, "settings page is wired");
  CHECK(!bd_shell_settings_visible(runtime->shell),
        "settings starts closed");
  CHECK(runtime->stub.brightness == 80 && runtime->stub.volume == 40,
        "persisted brightness/volume applied on startup (%d/%d)",
        runtime->stub.brightness, runtime->stub.volume);
  uint64_t battery_next = 0;
  bool battery_ready = false;
  int battery_percent = -1;
  CHECK(bd_shell_test_battery_policy(runtime->shell, &battery_next,
                                     &battery_ready, &battery_percent),
        "battery policy query");
  CHECK(battery_percent == 73, "battery fixture capacity %d", battery_percent);
  CHECK(battery_ready, "low_battery_percent=%d must initialize the policy",
        low_battery_percent);
  struct timespec wall;
  clock_gettime(CLOCK_MONOTONIC, &wall);
  uint64_t wall_now = (uint64_t)wall.tv_sec * 1000u +
                      (uint64_t)wall.tv_nsec / 1000000u;
  if (low_battery_percent <= 20) {
    CHECK(battery_next >= wall_now + 30000,
          "normal battery cadence is not a fast fallback: next=%llu now=%llu",
          (unsigned long long)battery_next, (unsigned long long)wall_now);
  } else {
    /* A high threshold makes the 73% fixture critical; confirmation sampling
     * is expected, but it must still be a valid policy rather than a
     * fallback. */
    CHECK(battery_next > wall_now,
          "high-threshold policy has a live next sample: next=%llu now=%llu",
          (unsigned long long)battery_next, (unsigned long long)wall_now);
  }
  return runtime->settings != NULL;
}

static void destroy_runtime(test_runtime *runtime) {
  if (runtime->shell != NULL) {
    bd_shell_destroy(runtime->shell);
  }
  if (runtime->config != NULL) {
    bd_config_free(runtime->config);
  }
  if (runtime->gfx != NULL) {
    bd_gfx_destroy(runtime->gfx);
  }
  if (runtime->platform != NULL) {
    bd_platform_destroy(runtime->platform);
  }
  stop_process(runtime->perfd);
  sf_remove_tree(runtime->config_dir);
  memset(runtime, 0, sizeof(*runtime));
  runtime->perfd = -1;
}

static int open_settings_from_home(test_runtime *runtime, uint64_t *now) {
  bd_home *home = bd_shell_home(runtime->shell);
  int index = bd_home_find_item(home, "brickdesk-settings");
  CHECK(index >= 0, "built-in settings tile exists without demo dependency");
  if (index < 0) {
    return -1;
  }
  bd_home_set_focus(home, index);
  bd_shell_test_advance(runtime->shell, *now);
  key_press(runtime->shell, BD_KEY_A, *now);
  *now += 20;
  CHECK(bd_shell_settings_visible(runtime->shell),
        "A on the settings tile opens settings");
  CHECK(!bd_home_surface_visible(home),
        "opening settings hides the home/plugin surface");
  return bd_settings_is_open(runtime->settings) ? 0 : -1;
}

static void test_home_quick(test_runtime *runtime, uint64_t *now) {
  bd_shell_test_advance(runtime->shell, *now);
  key_press(runtime->shell, BD_KEY_START, *now);
  *now += 20;
  CHECK(bd_home_quick_open(bd_shell_home(runtime->shell)),
        "Start opens the quick panel");
  bd_shell_test_advance(runtime->shell, *now);
  bd_shell_iterate(runtime->shell, 0);
  key_press(runtime->shell, BD_KEY_RIGHT, *now);
  *now += 20;
  CHECK(runtime->stub.brightness == 90,
        "quick brightness step reads the real backend: %d",
        runtime->stub.brightness);
  CHECK(bd_config_get_int(runtime->config, "display.brightness", -1) == 90,
        "quick brightness persists: %ld",
        bd_config_get_int(runtime->config, "display.brightness", -1));
  char local[2048];
  snprintf(local, sizeof(local), "%s/local.ini", runtime->config_dir);
  CHECK(sf_file_contains(local, "brightness = 90"),
        "local.ini has quick brightness");
  key_press(runtime->shell, BD_KEY_B, *now);
  *now += 250;
  bd_shell_test_advance(runtime->shell, *now);
  bd_shell_iterate(runtime->shell, 0);
  CHECK(!bd_home_quick_open(bd_shell_home(runtime->shell)),
        "B closes the quick panel");
}

static void test_global_keys(test_runtime *runtime, uint64_t *now) {
  bd_shell_test_advance(runtime->shell, *now);
  int volume = runtime->stub.volume;
  key_press(runtime->shell, BD_KEY_VOLUME_UP, *now);
  *now += 20;
  CHECK(runtime->stub.volume == volume + 5,
        "volume key: %d from %d", runtime->stub.volume, volume);
  CHECK(bd_config_get_int(runtime->config, "audio.volume", -1) == volume + 5,
        "volume key persisted: %ld",
        bd_config_get_int(runtime->config, "audio.volume", -1));

  int brightness = runtime->stub.brightness;
  bd_shell_test_key(runtime->shell, BD_KEY_MENU, true, *now);
  key_press(runtime->shell, BD_KEY_VOLUME_UP, *now);
  bd_shell_test_key(runtime->shell, BD_KEY_MENU, false, *now + 10);
  *now += 30;
  CHECK(runtime->stub.brightness == brightness + 5,
        "menu+volume brightness: %d from %d", runtime->stub.brightness,
        brightness);

  int temperature =
      (int)bd_config_get_int(runtime->config, "display.color_temperature", 0);
  bd_shell_test_key(runtime->shell, BD_KEY_SELECT, true, *now);
  key_press(runtime->shell, BD_KEY_VOLUME_DOWN, *now);
  bd_shell_test_key(runtime->shell, BD_KEY_SELECT, false, *now + 10);
  *now += 30;
  CHECK(bd_config_get_int(runtime->config, "display.color_temperature", 0) ==
            temperature - 10,
        "select+volume colour temperature: %ld",
        bd_config_get_int(runtime->config, "display.color_temperature", 0));
}

static void test_select_hint(test_runtime *runtime, uint64_t *now) {
  bd_shell_test_advance(runtime->shell, *now);
  bd_shell_test_key(runtime->shell, BD_KEY_SELECT, true, *now);
  bd_shell_test_advance(runtime->shell, *now + 400);
  *now += 410;
  CHECK(bd_settings_hint_visible(runtime->settings),
        "Select hold shows the settings hint strip");
  bd_shell_test_key(runtime->shell, BD_KEY_SELECT, false, *now);
  bd_shell_test_advance(runtime->shell, *now + 10);
  *now += 20;
  CHECK(!bd_settings_hint_visible(runtime->settings),
        "Select release hides the settings hint strip");
}

static void test_slider_repeat(test_runtime *runtime, uint64_t *now) {
  key_press(runtime->shell, BD_KEY_R1, *now);
  *now += 20;
  key_press(runtime->shell, BD_KEY_R1, *now);
  *now += 20;
  CHECK(bd_settings_category_index(runtime->settings) ==
            BD_SETTINGS_CATEGORY_DISPLAY,
        "R1 twice reaches display: %d",
        bd_settings_category_index(runtime->settings));
  CHECK(bd_settings_focus(runtime->settings) == 0 &&
            strcmp(bd_settings_row_key(runtime->settings, 0),
                   "display.brightness") == 0,
        "brightness row focused");

  bd_shell_test_advance(runtime->shell, *now);
  bd_shell_iterate(runtime->shell, 0);
  int start = runtime->stub.brightness;
  bd_shell_test_key(runtime->shell, BD_KEY_LEFT, true, *now);
  CHECK(runtime->stub.brightness == start - 1,
        "first press steps once: %d from %d", runtime->stub.brightness, start);
  bd_shell_test_advance(runtime->shell, *now + 390);
  *now += 390;
  bd_shell_iterate(runtime->shell, 0);
  CHECK(runtime->stub.brightness == start - 1,
        "no repeat before the delay: %d", runtime->stub.brightness);
  bd_shell_test_advance(runtime->shell, *now + 10);
  *now += 10;
  bd_shell_iterate(runtime->shell, 0);
  CHECK(runtime->stub.brightness == start - 2,
        "one repeat after the delay: %d", runtime->stub.brightness);
  bd_shell_test_key(runtime->shell, BD_KEY_LEFT, false, *now);
  bd_shell_test_advance(runtime->shell, *now + 500);
  *now += 500;
  bd_shell_iterate(runtime->shell, 0);
  CHECK(runtime->stub.brightness == start - 2,
        "release stops repeats: %d", runtime->stub.brightness);
  CHECK(bd_config_get_int(runtime->config, "display.brightness", -1) ==
            start - 2,
        "slider value persisted: %ld",
        bd_config_get_int(runtime->config, "display.brightness", -1));
}

static void test_theme_sync(test_runtime *runtime, uint64_t *now) {
  key_press(runtime->shell, BD_KEY_B, *now);
  *now += 20;
  CHECK(!bd_shell_settings_visible(runtime->shell), "B closes settings");
  CHECK(bd_home_surface_visible(bd_shell_home(runtime->shell)),
        "B restores the home/plugin surface");
  if (open_settings_from_home(runtime, now) != 0) {
    return;
  }
  bd_shell_test_advance(runtime->shell, *now);
  key_press(runtime->shell, BD_KEY_RIGHT, *now);
  *now += 20;
  const char *accent = bd_config_get_string(runtime->config, "theme.accent", "");
  CHECK(strcmp(accent, "coral") == 0, "accent cycled to coral, got %s", accent);
  bd_color coral;
  CHECK(bd_color_from_hex(bd_config_accent_hex("coral", true), &coral),
        "coral hex");
  bd_color home_accent = bd_home_accent_base(bd_shell_home(runtime->shell));
  CHECK(home_accent.r == coral.r && home_accent.g == coral.g &&
            home_accent.b == coral.b,
        "home accent follows settings immediately");
  bd_color page_accent = bd_settings_accent(runtime->settings);
  CHECK(page_accent.r == coral.r && page_accent.g == coral.g &&
            page_accent.b == coral.b,
        "settings accent recoloured immediately");

  key_press(runtime->shell, BD_KEY_DOWN, *now);
  *now += 20;
  key_press(runtime->shell, BD_KEY_RIGHT, *now);
  *now += 20;
  CHECK(strcmp(bd_config_get_string(runtime->config, "theme.ambient", ""),
               "solid") == 0,
        "ambient cycled to solid: %s",
        bd_config_get_string(runtime->config, "theme.ambient", ""));
  CHECK(bd_home_ambient_solid(bd_shell_home(runtime->shell)),
        "home ambient follows settings immediately");
}

static void test_perf_apply(test_runtime *runtime, uint64_t *now) {
  CHECK(bd_shell_open_settings(runtime->shell,
                               BD_SETTINGS_CATEGORY_PERFORMANCE),
        "open performance");
  bd_shell_test_advance(runtime->shell, *now);
  bd_shell_iterate(runtime->shell, 0);
  CHECK(strcmp(bd_settings_row_key(runtime->settings, 0),
               "perf.global_profile") == 0,
        "performance row 0");
  key_press(runtime->shell, BD_KEY_RIGHT, *now);
  *now += 20;
  CHECK(strcmp(bd_config_get_string(runtime->config, "perf.global_profile", ""),
               "auto") == 0,
        "perfd-confirmed profile saved: %s",
        bd_config_get_string(runtime->config, "perf.global_profile", ""));
  char path[4096];
  char value[128];
  snprintf(path, sizeof(path),
           "%s/sys/devices/system/cpu/cpufreq/policy0/scaling_governor",
           runtime->sysfs_root);
  CHECK(read_text(path, value, sizeof(value)) && strcmp(value, "schedutil") == 0,
        "auto applied governor %s", value);

  stop_process(runtime->perfd);
  runtime->perfd = -1;
  key_press(runtime->shell, BD_KEY_RIGHT, *now);
  *now += 20;
  CHECK(strcmp(bd_config_get_string(runtime->config, "perf.global_profile", ""),
               "auto") == 0,
        "disconnected perfd must not save: %s",
        bd_config_get_string(runtime->config, "perf.global_profile", ""));
  CHECK(bd_settings_last_error(runtime->settings)[0] != '\0',
        "disconnected perfd reports an error");
}

static int find_settings_row(bd_settings *settings, const char *key) {
  for (int i = 0; i < bd_settings_row_count(settings); ++i) {
    const char *candidate = bd_settings_row_key(settings, i);
    if (candidate != NULL && strcmp(candidate, key) == 0) {
      return i;
    }
  }
  return -1;
}

static bool focus_settings_row(bd_settings *settings, const char *key) {
  int index = find_settings_row(settings, key);
  if (index < 0) {
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

static void test_hotkey_edit(test_runtime *runtime, uint64_t *now) {
  bd_settings *page = runtime->settings;
  CHECK(bd_shell_open_settings(runtime->shell, BD_SETTINGS_CATEGORY_KEYS),
        "open keys for editing");
  CHECK(focus_settings_row(page, "!hotkey.menu"), "focus menu binding");
  key_press(runtime->shell, BD_KEY_A, *now);
  *now += 20;
  CHECK(strcmp(bd_settings_row_key(page, 0), "!hotkey.combo") == 0,
        "hotkey editor opened, row 0 %s", bd_settings_row_key(page, 0));
  key_press(runtime->shell, BD_KEY_A, *now);
  *now += 20;
  CHECK(bd_settings_hotkey_capturing(page), "shell starts combo capture");
  bd_shell_test_key(runtime->shell, BD_KEY_A, false, *now);
  *now += 10;
  bd_shell_test_key(runtime->shell, BD_KEY_Y, true, *now);
  *now += 10;
  bd_shell_test_key(runtime->shell, BD_KEY_Y, false, *now);
  *now += 20;
  CHECK(!bd_settings_hotkey_capturing(page), "capture finishes on release");
  CHECK(strcmp(bd_config_get_string(runtime->config, "hotkey.menu.combo", ""),
               "Y") == 0,
        "shell capture saved Y: %s",
        bd_config_get_string(runtime->config, "hotkey.menu.combo", ""));

  key_press(runtime->shell, BD_KEY_B, *now);
  *now += 20;
  CHECK(!bd_settings_subpage(page), "B leaves the hotkey editor");
  key_press(runtime->shell, BD_KEY_B, *now);
  *now += 20;
  CHECK(!bd_shell_settings_visible(runtime->shell),
        "B closes settings after editing");
}

typedef struct keyboard_result {
  bool called;
  bool accepted;
  char text[BD_KEYBOARD_TEXT_MAX + 1];
} keyboard_result;

static void keyboard_done(void *user, bool accepted, const char *text) {
  keyboard_result *result = (keyboard_result *)user;
  result->called = true;
  result->accepted = accepted;
  snprintf(result->text, sizeof(result->text), "%s",
           text != NULL ? text : "");
}

static void test_keyboard(test_runtime *runtime, uint64_t *now) {
  keyboard_result result;
  memset(&result, 0, sizeof(result));
  CHECK(bd_shell_prompt_text(runtime->shell, "测试输入", "ab", 8, false,
                             keyboard_done, &result),
        "open on-screen keyboard");
  CHECK(bd_shell_keyboard_open(runtime->shell), "keyboard is open");
  bd_shell_test_key(runtime->shell, BD_KEY_A, true, *now);
  *now += 10;
  bd_shell_test_key(runtime->shell, BD_KEY_A, false, *now);
  *now += 10;
  bd_shell_test_key(runtime->shell, BD_KEY_Y, true, *now);
  *now += 10;
  bd_shell_test_key(runtime->shell, BD_KEY_Y, false, *now);
  *now += 10;
  CHECK(result.called && result.accepted, "keyboard accepted");
  CHECK(strcmp(result.text, "ab1") == 0, "keyboard text %s", result.text);
  CHECK(!bd_shell_keyboard_open(runtime->shell), "keyboard closed");
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: bd_shell_settings_test PERFD\n");
    return 2;
  }
  test_runtime runtime;
  if (!setup_runtime(&runtime, argv[1], 20)) {
    destroy_runtime(&runtime);
    return 1;
  }
  uint64_t now = 1000;
  bd_shell_test_advance(runtime.shell, now);
  test_home_quick(&runtime, &now);
  test_global_keys(&runtime, &now);
  if (open_settings_from_home(&runtime, &now) == 0) {
    test_select_hint(&runtime, &now);
    test_slider_repeat(&runtime, &now);
    test_theme_sync(&runtime, &now);
    test_perf_apply(&runtime, &now);
    test_hotkey_edit(&runtime, &now);
    test_keyboard(&runtime, &now);
    key_press(runtime.shell, BD_KEY_B, now);
    now += 20;
    CHECK(!bd_shell_settings_visible(runtime.shell),
          "final B returns to the home page");
  }
  destroy_runtime(&runtime);

  test_runtime high;
  if (!setup_runtime(&high, argv[1], 100)) {
    destroy_runtime(&high);
    return 1;
  }
  uint64_t high_next = 0;
  bool high_ready = false;
  int high_percent = -1;
  CHECK(bd_shell_test_battery_policy(high.shell, &high_next, &high_ready,
                                     &high_percent),
        "high threshold battery query");
  CHECK(high_ready, "low_battery_percent=100 must initialize the policy");
  destroy_runtime(&high);

  if (failures != 0) {
    fprintf(stderr, "shell_settings_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("shell_settings_test: ok\n");
  return 0;
}
