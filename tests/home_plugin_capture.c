#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "brickdesk/config.h"
#include "brickdesk/gfx.h"
#include "brickdesk/home.h"
#include "brickdesk/platform.h"
#include "brickdesk/plugin.h"
#include "brickdesk/shell.h"
#include "brickdesk/theme.h"

#include <errno.h>
#include <ftw.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define TEST_PATH 1024

static int failures = 0;

#define CHECK(condition, ...)                                                  \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "home_plugin_capture: FAIL: ");                          \
      fprintf(stderr, __VA_ARGS__);                                            \
      fputc('\n', stderr);                                                     \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static void sleep_ms(int milliseconds) {
  struct timespec delay;
  delay.tv_sec = milliseconds / 1000;
  delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
  nanosleep(&delay, NULL);
}

static bool make_dir(const char *path) {
  if (mkdir(path, 0777) == 0) {
    return true;
  }
  return errno == EEXIST;
}

static bool join(char *out, size_t size, const char *a, const char *b) {
  int written = snprintf(out, size, "%s/%s", a, b);
  return written > 0 && (size_t)written < size;
}

static bool write_file(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs(text, file) >= 0;
  return fclose(file) == 0 && ok;
}

static int remove_entry(const char *path, const struct stat *info, int flag,
                        struct FTW *state) {
  (void)info;
  (void)flag;
  (void)state;
  return remove(path);
}

static void remove_tree(const char *path) {
  nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

enum { FIXTURE_APP_COUNT = 24 };

static bool prepare_apps(const char *root, char *apps, size_t apps_size) {
  if (!join(apps, apps_size, root, "apps") || !make_dir(apps)) {
    return false;
  }
  for (int i = 0; i < FIXTURE_APP_COUNT; ++i) {
    char name[16];
    char app_dir[TEST_PATH];
    char manifest[TEST_PATH];
    snprintf(name, sizeof(name), "fixture%02d", i);
    if (!join(app_dir, sizeof(app_dir), apps, name) ||
        !make_dir(app_dir) ||
        !join(manifest, sizeof(manifest), app_dir, "app.json")) {
      return false;
    }
    char json[512];
    int needed = snprintf(
        json, sizeof(json),
        "{\"schema\":1,\"id\":\"test.fixture%02d\",\"name\":\"Fixture %02d\","
        "\"subtitle\":\"plugin tile test\",\"type\":\"app\","
        "\"category\":\"app\",\"exec\":[\"/bin/true\"],\"cwd\":\".\","
        "\"capabilities\":{},\"hooks\":{},\"accent\":\"#5b6b7a\"}\n",
        i, i);
    if (needed <= 0 || (size_t)needed >= sizeof(json) ||
        !write_file(manifest, json)) {
      return false;
    }
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: home_plugin_capture OUT_DIR\n");
    return 2;
  }
  char root[] = "/tmp/bd_home_plugin.XXXXXX";
  if (mkdtemp(root) == NULL) {
    fprintf(stderr, "home_plugin_capture: mkdtemp failed: %s\n",
            strerror(errno));
    return 1;
  }
  char apps[TEST_PATH];
  if (!prepare_apps(root, apps, sizeof(apps))) {
    fprintf(stderr, "home_plugin_capture: cannot prepare apps\n");
    remove_tree(root);
    return 1;
  }

  bd_platform_config platform_config;
  memset(&platform_config, 0, sizeof(platform_config));
  platform_config.title = "brickdesk plugin home";
  platform_config.width = 1024;
  platform_config.height = 768;
  platform_config.hidden = true;
  bd_platform *platform = bd_platform_create(&platform_config);
  if (platform == NULL) {
    remove_tree(root);
    return 1;
  }
  bd_gfx *gfx = bd_gfx_create(1024, 768);
  if (gfx == NULL) {
    bd_platform_destroy(platform);
    remove_tree(root);
    return 1;
  }

  bd_config_options config_options;
  memset(&config_options, 0, sizeof(config_options));
  config_options.config_dir = BRICKDESK_TEST_CONFIG_DIR;
  config_options.migrate = false;
  char config_error[256] = {0};
  bd_config *config =
      bd_config_load(&config_options, config_error, sizeof(config_error));
  if (config == NULL) {
    fprintf(stderr, "home_plugin_capture: config: %s\n", config_error);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    remove_tree(root);
    return 1;
  }

  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.idle_stop_ms = 300;
  bd_plugin_host_config plugin_config;
  memset(&plugin_config, 0, sizeof(plugin_config));
  plugin_config.plugin_dir = BRICKDESK_TEST_PLUGIN_DIR;
  plugin_config.limits = &limits;
  plugin_config.display_width = 1024;
  plugin_config.display_height = 768;
  plugin_config.dark = true;
  plugin_config.accent = 0xe07a5f;
  plugin_config.locale = "zh-CN";
  plugin_config.verbose = true;
  bd_plugin_host *plugin_host = bd_plugin_host_create(&plugin_config);
  if (plugin_host == NULL) {
    fprintf(stderr, "home_plugin_capture: plugin host unavailable\n");
    bd_config_free(config);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    remove_tree(root);
    return 1;
  }
  char plugin_error[256] = {0};
  int plugin_count =
      bd_plugin_host_scan(plugin_host, plugin_error, sizeof(plugin_error));
  CHECK(plugin_count > 0, "scan plugins: %s", plugin_error);

  bd_plugin *hello = bd_plugin_host_find(plugin_host, "com.brickdesk.hello");
  CHECK(hello != NULL, "hello plugin discovered");
  CHECK(hello == NULL || bd_plugin_state_of(hello) != BD_PLUGIN_RUNNING,
        "tile plugin must not autostart before its tile is visible");

  bd_shell_config shell_config;
  memset(&shell_config, 0, sizeof(shell_config));
  shell_config.platform = platform;
  shell_config.gfx = gfx;
  shell_config.settings = config;
  shell_config.plugins = plugin_host;
  shell_config.apps_dir = apps;
  shell_config.verbose = true;
  shell_config.attach_plugin_tile = true;
  bd_shell *shell = bd_shell_create(&shell_config);
  if (shell == NULL) {
    fprintf(stderr, "home_plugin_capture: shell unavailable\n");
    bd_plugin_host_destroy(plugin_host);
    bd_config_free(config);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    remove_tree(root);
    return 1;
  }

  bd_home *home = bd_shell_home(shell);
  int plugin_index = bd_home_item_count(home) - 1;
  CHECK(bd_home_item_count(home) == FIXTURE_APP_COUNT + 2,
        "home should have all apps, settings and the plugin tile, got %d",
        bd_home_item_count(home));
  CHECK(hello != NULL && bd_plugin_state_of(hello) != BD_PLUGIN_RUNNING,
        "shell creation alone must not start the plugin tile");
  bd_rect plugin_rect;
  CHECK(bd_home_item_rect(home, plugin_index, &plugin_rect),
        "plugin item rect");
  CHECK(plugin_rect.x >= 1024,
        "plugin tile starts outside the viewport, got x=%d", plugin_rect.x);
  CHECK(bd_shell_test_session(shell) != NULL, "shell session");

  /* An off-screen tile must not activate the plugin. */
  bd_shell_render(shell);
  for (int i = 0; i < 30; ++i) {
    bd_shell_iterate(shell, 5);
    sleep_ms(5);
  }
  CHECK(hello != NULL && bd_plugin_state_of(hello) != BD_PLUGIN_RUNNING,
        "off-screen plugin tile must not start");

  /* Bring the tile into view; the home callback starts it on demand. */
  bd_home_set_focus(home, plugin_index);
  bd_shell_render(shell);
  for (int i = 0; i < 500; ++i) {
    bd_shell_iterate(shell, 10);
    if (hello != NULL && bd_plugin_state_of(hello) == BD_PLUGIN_RUNNING &&
        bd_plugin_ui_tile(hello) != NULL) {
      break;
    }
    sleep_ms(10);
  }
  CHECK(hello != NULL && bd_plugin_state_of(hello) == BD_PLUGIN_RUNNING,
        "hello plugin starts when its home tile is visible");
  CHECK(hello != NULL && bd_plugin_ui_tile(hello) != NULL,
        "hello plugin tile UI arrived");
  CHECK(bd_session_running_count(bd_shell_test_session(shell)) == 0,
        "plugin tile must not enter the session launch path");
  bd_rect visible_rect;
  CHECK(bd_home_item_rect(home, plugin_index, &visible_rect),
        "focused plugin item rect");
  CHECK(visible_rect.x - bd_home_get_stats(home).pan_x < 1024,
        "focused plugin tile is on screen");

  for (int i = 0; i < 40; ++i) {
    bd_shell_iterate(shell, 10);
    if (!bd_shell_has_pending_images(shell)) {
      break;
    }
    sleep_ms(5);
  }
  bd_shell_render(shell);

  char output[TEST_PATH];
  if (!join(output, sizeof(output), argv[1], "home_plugin.png")) {
    CHECK(false, "output path too long");
  } else if (!bd_gfx_save_png(gfx, output)) {
    CHECK(false, "save %s: %s", output, bd_gfx_last_error(gfx));
  } else {
    printf("capture: %s\n", output);
  }

  /* Moving the tile out of the viewport must release visibility. */
  bd_home_set_focus(home, 0);
  bd_shell_render(shell);
  for (int i = 0; i < 200; ++i) {
    bd_shell_iterate(shell, 5);
    if (hello == NULL || bd_plugin_state_of(hello) == BD_PLUGIN_STOPPED) {
      break;
    }
    sleep_ms(5);
  }
  CHECK(hello != NULL && bd_plugin_state_of(hello) == BD_PLUGIN_STOPPED,
        "off-screen plugin tile stops after its idle timeout");

  /* A hidden home must release visibility the same way. */
  bd_home_set_focus(home, plugin_index);
  bd_shell_render(shell);
  for (int i = 0; i < 500; ++i) {
    bd_shell_iterate(shell, 10);
    if (hello != NULL && bd_plugin_state_of(hello) == BD_PLUGIN_RUNNING) {
      break;
    }
    sleep_ms(10);
  }
  CHECK(hello != NULL && bd_plugin_state_of(hello) == BD_PLUGIN_RUNNING,
        "hello restarts when its tile returns");
  bd_home_set_surface_visible(home, false);
  for (int i = 0; i < 200; ++i) {
    bd_plugin_host_poll(plugin_host, 0);
    if (hello == NULL || bd_plugin_state_of(hello) == BD_PLUGIN_STOPPED) {
      break;
    }
    sleep_ms(5);
  }
  CHECK(hello != NULL && bd_plugin_state_of(hello) == BD_PLUGIN_STOPPED,
        "hidden home stops the plugin tile after its idle timeout");

  bd_shell_destroy(shell);
  bd_plugin_host_destroy(plugin_host);
  bd_config_free(config);
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  remove_tree(root);
  if (failures != 0) {
    fprintf(stderr, "home_plugin_capture: %d failure(s)\n", failures);
    return 1;
  }
  printf("home_plugin_capture: ok\n");
  return 0;
}
