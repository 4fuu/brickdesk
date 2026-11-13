#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "brickdesk/desktop.h"
#include "brickdesk/gfx.h"
#include "brickdesk/platform.h"
#include "brickdesk/plugin.h"
#include "brickdesk/theme.h"

#ifndef BRICKDESK_TEST_FIXTURE
#define BRICKDESK_TEST_FIXTURE "bd_plugin_fixture"
#endif
#ifndef BRICKDESK_TEST_PLUGIN_DIR
#define BRICKDESK_TEST_PLUGIN_DIR "plugins"
#endif

static int g_failures;

static void check(bool condition, const char *format, ...) {
  if (condition) {
    return;
  }
  ++g_failures;
  va_list args;
  va_start(args, format);
  fputs("FAIL: ", stderr);
  vfprintf(stderr, format, args);
  fputc('\n', stderr);
  va_end(args);
}

static uint64_t now_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void sleep_ms(int milliseconds) {
  struct timespec delay;
  delay.tv_sec = milliseconds / 1000;
  delay.tv_nsec = (long)(milliseconds % 1000) * 1000 * 1000;
  nanosleep(&delay, NULL);
}

static bool make_dir(const char *path) {
  if (mkdir(path, 0777) == 0) {
    return true;
  }
  return errno == EEXIST;
}

static bool write_text(const char *path, const char *text) {
  FILE *file = fopen(path, "wb");
  if (file == NULL) {
    return false;
  }
  bool ok = fwrite(text, 1, strlen(text), file) == strlen(text);
  fclose(file);
  return ok;
}

static void remove_tree(const char *path) {
  DIR *directory = opendir(path);
  if (directory != NULL) {
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
      if (strcmp(entry->d_name, ".") == 0 ||
          strcmp(entry->d_name, "..") == 0) {
        continue;
      }
      char child[1024];
      snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
      struct stat info;
      if (lstat(child, &info) == 0 && S_ISDIR(info.st_mode)) {
        remove_tree(child);
      } else {
        unlink(child);
      }
    }
    closedir(directory);
  }
  rmdir(path);
}

static char *make_root(void) {
  char template[] = "/tmp/bd-plugin-test-XXXXXX";
  char *root = mkdtemp(template);
  if (root == NULL) {
    return NULL;
  }
  return strdup(root);
}

static bool write_manifest(const char *root, const char *id, const char *mode,
                           int memory_mb) {
  char dir[1024];
  char path[2048];
  snprintf(dir, sizeof(dir), "%s/%s", root, id);
  if (!make_dir(dir)) {
    return false;
  }
  snprintf(path, sizeof(path), "%s/plugin.json", dir);
  char manifest[2048];
  snprintf(manifest, sizeof(manifest),
           "{"
           "\"schema\":1,\"id\":\"%s\",\"name\":\"Fixture %s\","
           "\"version\":\"0.1.0\",\"api\":1,\"runtime\":\"shell\","
           "\"entry\":[\"run.sh\"],"
           "\"capabilities\":[\"tile\",\"settings\",\"menu\"],"
           "\"permissions\":[\"network\"],"
           "\"menu\":[{\"id\":\"open\",\"label\":\"Open fixture\"}],"
           "\"tile\":{\"w\":1,\"h\":1,\"refresh_hz\":15},"
           "\"limits\":{\"memory_mb\":%d,\"nofile\":32},"
           "\"settings\":[{\"key\":\"show_meta\",\"type\":\"bool\","
           "\"label\":\"Show meta\",\"default\":true}]"
           "}",
           id, mode, memory_mb);
  if (!write_text(path, manifest)) {
    return false;
  }
  char script[2048];
  snprintf(script, sizeof(script),
           "#!/bin/sh\nexec \"%s\" %s \"$@\"\n", BRICKDESK_TEST_FIXTURE, mode);
  snprintf(path, sizeof(path), "%s/run.sh", dir);
  if (!write_text(path, script)) {
    return false;
  }
  chmod(path, 0755);
  return true;
}

static bool write_raw_manifest(const char *root, const char *dir_name,
                               const char *json) {
  char dir[1024];
  char path[2048];
  snprintf(dir, sizeof(dir), "%s/%s", root, dir_name);
  if (!make_dir(dir)) {
    return false;
  }
  snprintf(path, sizeof(path), "%s/plugin.json", dir);
  return write_text(path, json);
}

static bool write_settings_manifest(const char *root, const char *id,
                                    const char *default_value,
                                    const char *action, const char *value,
                                    const char *result_path) {
  char dir[1024];
  char path[2048];
  snprintf(dir, sizeof(dir), "%s/%s", root, id);
  if (!make_dir(dir)) {
    return false;
  }
  snprintf(path, sizeof(path), "%s/plugin.json", dir);
  char manifest[2048];
  snprintf(manifest, sizeof(manifest),
           "{\"schema\":1,\"id\":\"%s\",\"name\":\"Fixture %s\","
           "\"version\":\"0.1.0\",\"api\":1,\"runtime\":\"shell\","
           "\"entry\":[\"run.sh\"],\"capabilities\":[\"settings\"],"
           "\"settings\":[{\"key\":\"show_meta\",\"type\":\"bool\","
           "\"label\":\"Show meta\",\"default\":%s}]}",
           id, id, default_value);
  if (!write_text(path, manifest)) {
    return false;
  }
  char script[3072];
  snprintf(script, sizeof(script),
           "#!/bin/sh\n"
           "export BD_FIXTURE_SETTINGS_ACTION='%s'\n"
           "export BD_FIXTURE_SETTINGS_VALUE='%s'\n"
           "export BD_FIXTURE_SETTINGS_RESULT='%s'\n"
           "exec \"%s\" settings \"$@\"\n",
           action, value, result_path, BRICKDESK_TEST_FIXTURE);
  snprintf(path, sizeof(path), "%s/run.sh", dir);
  if (!write_text(path, script)) {
    return false;
  }
  chmod(path, 0755);
  return true;
}

static bool wait_for_result(bd_plugin_host *host, const char *path, char *out,
                            size_t out_size, int timeout_ms) {
  uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
  while (now_ms() < deadline) {
    bd_plugin_host_poll(host, 5);
    FILE *file = fopen(path, "rb");
    if (file != NULL) {
      bool ok = fgets(out, (int)out_size, file) != NULL;
      fclose(file);
      if (ok) {
        size_t length = strlen(out);
        while (length > 0 && (out[length - 1] == '\n' ||
                              out[length - 1] == '\r')) {
          out[--length] = '\0';
        }
        if (length > 0) {
          return true;
        }
      }
    }
  }
  return false;
}

static void settings_file_path(char *out, size_t out_size, const char *root,
                               const char *id) {
  snprintf(out, out_size, "%s/state/plugin-settings/%s/settings.json", root,
           id);
}

static bool make_settings_test_dir(const char *root, const char *id) {
  char path[2048];
  snprintf(path, sizeof(path), "%s/state", root);
  if (!make_dir(path)) {
    return false;
  }
  snprintf(path, sizeof(path), "%s/state/plugin-settings", root);
  if (!make_dir(path)) {
    return false;
  }
  snprintf(path, sizeof(path), "%s/state/plugin-settings/%s", root, id);
  return make_dir(path);
}

static bool read_file_text(const char *path, char *out, size_t out_size) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return false;
  }
  size_t length = fread(out, 1, out_size - 1, file);
  fclose(file);
  out[length] = '\0';
  return true;
}

static bd_plugin_host *make_host_paths(const char *plugin_dir,
                                      const char *root,
                                      bd_plugin_limits limits) {
  char runtime[1024];
  char state[1024];
  snprintf(runtime, sizeof(runtime), "%s/runtime", root);
  snprintf(state, sizeof(state), "%s/state", root);
  bd_plugin_host_config config;
  memset(&config, 0, sizeof(config));
  config.plugin_dir = plugin_dir;
  config.runtime_dir = runtime;
  config.state_dir = state;
  config.limits = &limits;
  config.display_width = 1024;
  config.display_height = 768;
  config.dark = true;
  config.accent = 0xe06c4a;
  config.locale = "en-US";
  return bd_plugin_host_create(&config);
}

static bd_plugin_host *make_host(const char *root, bd_plugin_limits limits) {
  return make_host_paths(root, root, limits);
}

static bool wait_for_state(bd_plugin_host *host, bd_plugin *plugin,
                           bd_plugin_state state, int timeout_ms) {
  uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
  while (now_ms() < deadline) {
    bd_plugin_host_poll(host, 5);
    if (bd_plugin_state_of(plugin) == state) {
      return true;
    }
  }
  return bd_plugin_state_of(plugin) == state;
}

static bool wait_for_ui(bd_plugin_host *host, bd_plugin *plugin,
                        int timeout_ms) {
  uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
  while (now_ms() < deadline) {
    bd_plugin_host_poll(host, 5);
    if (bd_plugin_ui_generation(plugin) > 0) {
      return true;
    }
  }
  return bd_plugin_ui_generation(plugin) > 0;
}

static void test_manifest_scan(void) {
  char *root = make_root();
  check(root != NULL, "manifest: temp dir");
  if (root == NULL) {
    return;
  }
  check(write_manifest(root, "com.test.good", "good", 32),
        "manifest: good plugin");
  check(write_raw_manifest(
            root, "com.test.badid",
            "{\"schema\":1,\"id\":\"com.test.other\",\"name\":\"x\","
            "\"version\":\"1\",\"api\":1,\"entry\":[\"run.sh\"]}"),
        "manifest: bad id manifest");
  check(write_raw_manifest(
            root, "com.test.missing",
            "{\"schema\":1,\"id\":\"com.test.missing\",\"name\":\"x\","
            "\"version\":\"1\",\"api\":1,\"entry\":[\"missing.sh\"]}"),
        "manifest: missing entry manifest");
  check(write_raw_manifest(
            root, "com.test.dupe",
            "{\"schema\":1,\"id\":\"com.test.dupe\",\"name\":\"x\","
            "\"version\":\"1\",\"api\":1,\"entry\":[\"run.sh\"],"
            "\"settings\":[{\"key\":\"a\",\"type\":\"bool\"},"
            "{\"key\":\"a\",\"type\":\"bool\"}]}"),
        "manifest: duplicate setting manifest");

  bd_plugin_limits limits = bd_plugin_default_limits();
  bd_plugin_host *host = make_host(root, limits);
  check(host != NULL, "manifest: host");
  if (host != NULL) {
    char error[256] = {0};
    int count = bd_plugin_host_scan(host, error, sizeof(error));
    check(count == 1, "manifest: expected 1 valid plugin, got %d (%s)", count,
          error);
    check(bd_plugin_host_diagnostic_count(host) >= 3,
          "manifest: expected >=3 diagnostics, got %zu",
          bd_plugin_host_diagnostic_count(host));
    bd_plugin *plugin = bd_plugin_host_find(host, "com.test.good");
    check(plugin != NULL, "manifest: find good plugin");
    if (plugin != NULL) {
      const bd_plugin_manifest *manifest = bd_plugin_manifest_of(plugin);
      check(manifest != NULL, "manifest: manifest pointer");
      if (manifest != NULL) {
        check(manifest->api == BD_PLUGIN_API_VERSION, "manifest: api");
        check(strcmp(manifest->runtime, "shell") == 0, "manifest: runtime");
        check((manifest->capabilities & BD_PLUGIN_CAP_TILE) != 0,
              "manifest: tile capability");
        check(manifest->tile.refresh_hz == 15, "manifest: refresh_hz");
        check(manifest->setting_count == 1, "manifest: setting count");
        check(manifest->menu_count == 1, "manifest: menu count");
        check(strcmp(manifest->menu[0].id, "open") == 0,
              "manifest: menu id");
        check(manifest->memory_limit_bytes == 32u * 1024u * 1024u,
              "manifest: memory limit");
      }
    }
    bd_plugin_host_destroy(host);
  }

  char missing_dir[1024];
  snprintf(missing_dir, sizeof(missing_dir), "%s/missing", root);
  bd_plugin_host *missing = make_host_paths(missing_dir, root, limits);
  if (missing != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(missing, error, sizeof(error)) == -1,
          "manifest: missing directory must fail");
    check(error[0] != '\0', "manifest: missing directory error text");
    bd_plugin_host_destroy(missing);
  }
  remove_tree(root);
  free(root);
}

static void test_handshake_and_ui(void) {
  char *root = make_root();
  check(root != NULL, "handshake: temp dir");
  if (root == NULL) {
    return;
  }
  check(write_manifest(root, "com.test.good", "good", 32), "handshake: plugin");
  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.handshake_timeout_ms = 1000;
  limits.idle_stop_ms = 0;
  bd_plugin_host *host = make_host(root, limits);
  check(host != NULL, "handshake: host");
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 1,
          "handshake: scan");
    bd_plugin *plugin = bd_plugin_host_find(host, "com.test.good");
    check(plugin != NULL, "handshake: find");
    if (plugin != NULL) {
      check(bd_plugin_set_tile_visible(plugin, true), "handshake: activate");
      check(wait_for_state(host, plugin, BD_PLUGIN_RUNNING, 2000),
            "handshake: expected running, got %s",
            bd_plugin_state_name(bd_plugin_state_of(plugin)));
      check(wait_for_ui(host, plugin, 1000), "handshake: expected UI");
      const bd_plugin_ui *tile = bd_plugin_ui_tile(plugin);
      const bd_plugin_ui *settings = bd_plugin_ui_settings(plugin);
      check(tile != NULL, "handshake: tile UI");
      check(settings != NULL, "handshake: settings UI");
      if (tile != NULL) {
        check(tile->root == 0, "handshake: tile root");
        check(tile->node_count >= 4, "handshake: tile node count %d",
              tile->node_count);
        check(tile->nodes[0].type == BD_PLUGIN_UI_COLUMN,
              "handshake: tile root type");
      }
      if (settings != NULL) {
        bool toggle = false;
        for (int i = 0; i < settings->node_count; ++i) {
          if (settings->nodes[i].type == BD_PLUGIN_UI_TOGGLE &&
              strcmp(settings->nodes[i].id, "show_meta") == 0) {
            toggle = true;
          }
        }
        check(toggle, "handshake: settings toggle");
      }
      bd_plugin_stats stats = bd_plugin_get_stats(plugin);
      check(stats.starts == 1, "handshake: starts=%d", stats.starts);
      check(stats.ui_generation >= 1, "handshake: generation=%d",
            stats.ui_generation);
      check(bd_plugin_tile_surface_path(plugin)[0] != '\0',
            "handshake: surface path");
      check(bd_plugin_tile_seq(plugin) == 10,
            "handshake: fixture frame seq=%u", bd_plugin_tile_seq(plugin));
      check(bd_plugin_get_setting(plugin, "show_meta") != NULL,
            "handshake: setting value");
      check(bd_plugin_set_setting(plugin, "show_meta", "false"),
            "handshake: set setting");
      check(strcmp(bd_plugin_get_setting(plugin, "show_meta"), "false") == 0,
            "handshake: setting updated");
      check(bd_plugin_activate_node(plugin, "refresh", "activate"),
            "handshake: ui.activate event");
    }
    bd_plugin_host_destroy(host);
  }
  remove_tree(root);
  free(root);
}

static void test_handshake_timeout(void) {
  char *root = make_root();
  check(root != NULL, "timeout: temp dir");
  if (root == NULL) {
    return;
  }
  check(write_manifest(root, "com.test.silent", "silent", 32),
        "timeout: plugin");
  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.handshake_timeout_ms = 100;
  limits.restart_limit = 0;
  limits.restart_base_ms = 10;
  limits.idle_stop_ms = 0;
  bd_plugin_host *host = make_host(root, limits);
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 1, "timeout: scan");
    bd_plugin *plugin = bd_plugin_host_find(host, "com.test.silent");
    if (plugin != NULL) {
      bd_plugin_set_tile_visible(plugin, true);
      check(wait_for_state(host, plugin, BD_PLUGIN_QUARANTINED, 1500),
            "timeout: expected quarantine, got %s",
            bd_plugin_state_name(bd_plugin_state_of(plugin)));
      bd_plugin_stats stats = bd_plugin_get_stats(plugin);
      check(stats.handshake_timeouts >= 1, "timeout: timeout count=%d",
            stats.handshake_timeouts);
      check(stats.starts == 1, "timeout: starts=%d", stats.starts);
      check(stats.crashes == 1, "timeout: crashes=%d", stats.crashes);
    }
    bd_plugin_host_destroy(host);
  }
  remove_tree(root);
  free(root);
}

static void test_crash_backoff(void) {
  char *root = make_root();
  check(root != NULL, "backoff: temp dir");
  if (root == NULL) {
    return;
  }
  check(write_manifest(root, "com.test.crash", "crash", 32), "backoff: plugin");
  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.handshake_timeout_ms = 1000;
  limits.restart_base_ms = 40;
  limits.restart_max_ms = 160;
  limits.restart_window_ms = 5000;
  limits.restart_limit = 3;
  limits.ping_interval_ms = 1000;
  limits.ping_timeout_ms = 3000;
  limits.idle_stop_ms = 0;
  bd_plugin_host *host = make_host(root, limits);
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 1, "backoff: scan");
    bd_plugin *plugin = bd_plugin_host_find(host, "com.test.crash");
    if (plugin != NULL) {
      uint64_t start = now_ms();
      bd_plugin_set_tile_visible(plugin, true);
      check(wait_for_state(host, plugin, BD_PLUGIN_QUARANTINED, 3000),
            "backoff: expected quarantine, got %s",
            bd_plugin_state_name(bd_plugin_state_of(plugin)));
      uint64_t elapsed = now_ms() - start;
      bd_plugin_stats stats = bd_plugin_get_stats(plugin);
      check(stats.starts == 4, "backoff: starts=%d (expected 4)", stats.starts);
      check(stats.restarts == 3, "backoff: restarts=%d (expected 3)",
            stats.restarts);
      check(stats.crashes == 4, "backoff: crashes=%d (expected 4)",
            stats.crashes);
      check(stats.last_exit_status == 3, "backoff: exit=%d",
            stats.last_exit_status);
      check(elapsed >= 200, "backoff: elapsed=%llums, expected >= 200",
            (unsigned long long)elapsed);
      int starts_before = stats.starts;
      uint64_t quiet_deadline = now_ms() + 300;
      while (now_ms() < quiet_deadline) {
        bd_plugin_host_poll(host, 10);
      }
      check(bd_plugin_get_stats(plugin).starts == starts_before,
            "backoff: quarantined plugin must not restart");
    }
    bd_plugin_host_destroy(host);
  }
  remove_tree(root);
  free(root);
}

static void test_frame_sequence(void) {
  char *root = make_root();
  check(root != NULL, "frames: temp dir");
  if (root == NULL) {
    return;
  }
  check(write_manifest(root, "com.test.good", "good", 32), "frames: plugin");
  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.handshake_timeout_ms = 1000;
  limits.idle_stop_ms = 0;
  bd_plugin_host *host = make_host(root, limits);
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 1, "frames: scan");
    bd_plugin *plugin = bd_plugin_host_find(host, "com.test.good");
    if (plugin != NULL) {
      bd_plugin_set_tile_visible(plugin, true);
      check(wait_for_state(host, plugin, BD_PLUGIN_RUNNING, 2000),
            "frames: running");
      uint64_t frame_deadline = now_ms() + 1000;
      while (now_ms() < frame_deadline && bd_plugin_tile_seq(plugin) != 10) {
        bd_plugin_host_poll(host, 5);
      }
      check(bd_plugin_tile_seq(plugin) == 10,
            "frames: fixture must finish its initial frame");
      const char *path = bd_plugin_tile_surface_path(plugin);
      check(path[0] != '\0', "frames: surface path");
      int fd = open(path, O_RDWR);
      check(fd >= 0, "frames: open surface");
      if (fd >= 0) {
        size_t bytes = sizeof(bd_plugin_frame_header) + 168u * 168u * 4u;
        bd_plugin_frame_header *header =
            (bd_plugin_frame_header *)mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                            MAP_SHARED, fd, 0);
        close(fd);
        check(header != MAP_FAILED, "frames: mmap");
        if (header != MAP_FAILED) {
          header->magic = BD_PLUGIN_FRAME_MAGIC;
          header->version = BD_PLUGIN_FRAME_VERSION;
          header->width = 168;
          header->height = 168;
          header->stride = 168 * 4;
          header->format = BD_PLUGIN_FRAME_FORMAT_RGBA8888;
          uint8_t *pixels = (uint8_t *)(header + 1);
          memset(pixels, 0x5a, 168u * 168u * 4u);
          __atomic_store_n(&header->seq, 100u, __ATOMIC_RELEASE);
          bd_plugin_frame frame;
          check(bd_plugin_tile_consume(plugin, &frame), "frames: first consume");
          check(frame.seq == 100 && frame.width == 168 && frame.rgba != NULL,
                "frames: first frame fields");
          check(bd_plugin_tile_consume(plugin, &frame) == false,
                "frames: unchanged seq must not redraw");
          sleep_ms(80);
          check(bd_plugin_tile_consume(plugin, &frame) == false,
                "frames: unchanged seq after rate limit must not redraw");
          __atomic_store_n(&header->seq, 101u, __ATOMIC_RELEASE);
          check(bd_plugin_tile_consume(plugin, &frame), "frames: second consume");
          check(frame.seq == 101, "frames: second seq");
          munmap(header, bytes);
        }
      }
    }
    bd_plugin_host_destroy(host);
  }
  remove_tree(root);
  free(root);
}

static bool parse_ui(const char *json) {
  bd_plugin_ui ui;
  char error[256] = {0};
  return bd_plugin_ui_parse(json, strlen(json), &ui, error, sizeof(error));
}

static void test_ui_validation(void) {
  const char *valid =
      "{\"type\":\"column\",\"padding\":10,\"children\":["
      "{\"type\":\"text\",\"text\":\"hello\"},"
      "{\"type\":\"progress\",\"value\":1,\"max\":2}]}";
  bd_plugin_ui ui;
  char error[256] = {0};
  check(bd_plugin_ui_parse(valid, strlen(valid), &ui, error, sizeof(error)),
        "ui: valid tree rejected: %s", error);
  check(ui.node_count == 3, "ui: node count=%d", ui.node_count);
  check(ui.nodes[0].type == BD_PLUGIN_UI_COLUMN && ui.nodes[0].child_count == 2,
        "ui: root children");

  const char *too_deep =
      "{\"type\":\"column\",\"children\":["
      "{\"type\":\"column\",\"children\":["
      "{\"type\":\"column\",\"children\":["
      "{\"type\":\"column\",\"children\":["
      "{\"type\":\"column\",\"children\":["
      "{\"type\":\"column\",\"children\":["
      "{\"type\":\"column\",\"children\":["
      "{\"type\":\"column\",\"children\":["
      "{\"type\":\"text\",\"text\":\"x\"}]}]}]}]}]}]}]}]}";
  check(!parse_ui(too_deep), "ui: depth 9 must be rejected");
  check(!parse_ui("{\"type\":\"unknown\"}"), "ui: unknown node must fail");
  check(!parse_ui("{\"type\":\"text\",\"text\":\"\"}"),
        "ui: empty text must fail");
  check(!parse_ui("{\"type\":\"progress\",\"value\":3,\"max\":2}"),
        "ui: value > max must fail");
  check(!parse_ui("{\"type\":\"toggle\",\"label\":\"x\",\"value\":true}"),
        "ui: toggle without id must fail");

  char big[16384];
  size_t used = 0;
  used += (size_t)snprintf(big + used, sizeof(big) - used,
                           "{\"type\":\"column\",\"children\":[");
  for (int row = 0; row < 24; ++row) {
    used += (size_t)snprintf(big + used, sizeof(big) - used,
                             "%s{\"type\":\"row\",\"children\":[",
                             row == 0 ? "" : ",");
    for (int column = 0; column < 4; ++column) {
      used += (size_t)snprintf(big + used, sizeof(big) - used,
                               "%s{\"type\":\"text\",\"text\":\"x\"}",
                               column == 0 ? "" : ",");
    }
    used += (size_t)snprintf(big + used, sizeof(big) - used, "]}");
  }
  snprintf(big + used, sizeof(big) - used, "]}");
  check(!parse_ui(big), "ui: more than 96 nodes must fail");

  char many_children[4096];
  used = 0;
  used += (size_t)snprintf(many_children + used, sizeof(many_children) - used,
                           "{\"type\":\"column\",\"children\":[");
  for (int i = 0; i < 25; ++i) {
    used += (size_t)snprintf(many_children + used,
                             sizeof(many_children) - used,
                             "%s{\"type\":\"text\",\"text\":\"x\"}",
                             i == 0 ? "" : ",");
  }
  snprintf(many_children + used, sizeof(many_children) - used, "]}");
  check(!parse_ui(many_children), "ui: more than 24 children must fail");

  char long_text[512];
  memset(long_text, 'a', 200);
  long_text[200] = '\0';
  char long_json[1024];
  snprintf(long_json, sizeof(long_json),
           "{\"type\":\"text\",\"text\":\"%s\"}", long_text);
  check(!parse_ui(long_json), "ui: oversize text must fail");
}

static void test_invalid_ui_message(void) {
  char *root = make_root();
  check(root != NULL, "badui: temp dir");
  if (root == NULL) {
    return;
  }
  check(write_manifest(root, "com.test.badui", "badui", 32), "badui: plugin");
  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.handshake_timeout_ms = 1000;
  limits.idle_stop_ms = 0;
  bd_plugin_host *host = make_host(root, limits);
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 1, "badui: scan");
    bd_plugin *plugin = bd_plugin_host_find(host, "com.test.badui");
    if (plugin != NULL) {
      bd_plugin_set_tile_visible(plugin, true);
      check(wait_for_state(host, plugin, BD_PLUGIN_RUNNING, 2000),
            "badui: running");
      uint64_t deadline = now_ms() + 500;
      while (now_ms() < deadline) {
        bd_plugin_host_poll(host, 10);
      }
      bd_plugin_stats stats = bd_plugin_get_stats(plugin);
      check(bd_plugin_ui_tile(plugin) == NULL, "badui: tile UI must stay empty");
      check(bd_plugin_ui_settings(plugin) == NULL,
            "badui: settings UI must stay empty");
      check(stats.protocol_errors >= 2, "badui: protocol errors=%d",
            stats.protocol_errors);
      check(bd_plugin_state_of(plugin) == BD_PLUGIN_RUNNING,
            "badui: host must keep running");
    }
    bd_plugin_host_destroy(host);
  }
  remove_tree(root);
  free(root);
}

static void test_idle_stop_and_restart(void) {
  char *root = make_root();
  check(root != NULL, "idle: temp dir");
  if (root == NULL) {
    return;
  }
  check(write_manifest(root, "com.test.good", "good", 32), "idle: plugin");
  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.handshake_timeout_ms = 1000;
  limits.idle_stop_ms = 150;
  limits.shutdown_timeout_ms = 500;
  bd_plugin_host *host = make_host(root, limits);
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 1, "idle: scan");
    bd_plugin *plugin = bd_plugin_host_find(host, "com.test.good");
    if (plugin != NULL) {
      bd_plugin_set_tile_visible(plugin, true);
      check(wait_for_state(host, plugin, BD_PLUGIN_RUNNING, 2000),
            "idle: running");
      check(bd_plugin_get_stats(plugin).starts == 1, "idle: initial start");
      bd_plugin_set_tile_visible(plugin, false);
      check(wait_for_state(host, plugin, BD_PLUGIN_STOPPED, 2000),
            "idle: expected stopped, got %s",
            bd_plugin_state_name(bd_plugin_state_of(plugin)));
      check(bd_plugin_get_stats(plugin).starts == 1,
            "idle: should not restart while hidden");
      check(bd_plugin_set_settings_open(plugin, true), "idle: open settings");
      check(wait_for_state(host, plugin, BD_PLUGIN_RUNNING, 2000),
            "idle: settings page must start the plugin");
      check(bd_plugin_get_stats(plugin).starts == 2,
            "idle: expected second start");
      bd_plugin_set_settings_open(plugin, false);
      check(wait_for_state(host, plugin, BD_PLUGIN_STOPPED, 2000),
            "idle: expected stopped after closing settings");
      check(bd_plugin_set_tile_visible(plugin, true), "idle: show tile again");
      check(wait_for_state(host, plugin, BD_PLUGIN_RUNNING, 2000),
            "idle: expected running after showing the tile");
      check(bd_plugin_get_stats(plugin).starts == 3,
            "idle: expected third start");
    }
    bd_plugin_host_destroy(host);
  }
  remove_tree(root);
  free(root);
}

static void test_memory_limit(void) {
  char *root = make_root();
  check(root != NULL, "memory: temp dir");
  if (root == NULL) {
    return;
  }
  check(write_manifest(root, "com.test.alloc", "alloc", 16), "memory: plugin");
  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.handshake_timeout_ms = 1000;
  limits.restart_limit = 0;
  limits.idle_stop_ms = 0;
  bd_plugin_host *host = make_host(root, limits);
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 1, "memory: scan");
    bd_plugin *plugin = bd_plugin_host_find(host, "com.test.alloc");
    if (plugin != NULL) {
      bd_plugin_set_tile_visible(plugin, true);
      check(wait_for_state(host, plugin, BD_PLUGIN_QUARANTINED, 3000),
            "memory: expected quarantine, got %s",
            bd_plugin_state_name(bd_plugin_state_of(plugin)));
      bd_plugin_stats stats = bd_plugin_get_stats(plugin);
      check(stats.last_exit_status == 7,
            "memory: fixture exit=%d (7 means the allocation was denied)",
            stats.last_exit_status);
    }
    bd_plugin_host_destroy(host);
  }
  remove_tree(root);
  free(root);
}

static void test_desktop_frames_with_busy_plugin(void) {
  char *root = make_root();
  check(root != NULL, "desktop: temp dir");
  if (root == NULL) {
    return;
  }
  check(write_manifest(root, "com.test.spin", "spin", 32), "desktop: plugin");
  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.handshake_timeout_ms = 1000;
  limits.ping_interval_ms = 100;
  limits.ping_timeout_ms = 1000;
  limits.restart_limit = 0;
  limits.idle_stop_ms = 0;
  bd_plugin_host *host = make_host(root, limits);
  if (host == NULL) {
    remove_tree(root);
    free(root);
    return;
  }
  char error[256] = {0};
  check(bd_plugin_host_scan(host, error, sizeof(error)) == 1, "desktop: scan");
  bd_plugin *plugin = bd_plugin_host_find(host, "com.test.spin");
  check(plugin != NULL, "desktop: find");
  if (plugin == NULL) {
    bd_plugin_host_destroy(host);
    remove_tree(root);
    free(root);
    return;
  }
  bd_plugin_set_tile_visible(plugin, true);
  check(wait_for_state(host, plugin, BD_PLUGIN_RUNNING, 2000),
        "desktop: spin plugin running");

  bd_platform_config platform_config;
  memset(&platform_config, 0, sizeof(platform_config));
  platform_config.title = "brickdesk plugin test";
  platform_config.width = 1024;
  platform_config.height = 768;
  platform_config.hidden = true;
  bd_platform *platform = bd_platform_create(&platform_config);
  check(platform != NULL, "desktop: platform");
  if (platform != NULL) {
    bd_gfx *gfx = bd_gfx_create(1024, 768);
    check(gfx != NULL, "desktop: gfx");
    if (gfx != NULL) {
      bd_desktop_theme theme;
      theme.ambient_baked = true;
      theme.accent = bd_color_rgb(224, 108, 74);
      bd_desktop *desktop = bd_desktop_create(gfx, &theme);
      check(desktop != NULL, "desktop: create");
      if (desktop != NULL) {
        bd_desktop_set_plugin_tile(desktop, plugin);
        uint64_t total_start = now_ms();
        uint64_t max_poll = 0;
        int presented = 0;
        for (int frame = 0; frame < 30; ++frame) {
          uint64_t poll_start = now_ms();
          bd_plugin_host_poll(host, 0);
          uint64_t poll_time = now_ms() - poll_start;
          if (poll_time > max_poll) {
            max_poll = poll_time;
          }
          bd_gfx_damage_all(gfx);
          bd_gfx_begin_frame(gfx);
          bd_desktop_draw(desktop, gfx);
          bd_gfx_end_frame(gfx);
          bd_gfx_stats stats = bd_gfx_get_stats(gfx);
          if (stats.presented) {
            bd_platform_present(platform);
            ++presented;
          }
        }
        uint64_t total = now_ms() - total_start;
        check(max_poll < 100,
              "desktop: plugin poll took %llums (must stay non-blocking)",
              (unsigned long long)max_poll);
        check(total < 4000,
              "desktop: 30 frames took %llums while a plugin spins",
              (unsigned long long)total);
        check(presented >= 25, "desktop: presented=%d of 30", presented);
        bd_desktop_destroy(desktop);
      }
      bd_gfx_destroy(gfx);
    }
    bd_platform_destroy(platform);
  }
  bd_plugin_host_destroy(host);
  remove_tree(root);
  free(root);
}

static void test_settings_persistence(void) {
  char *root = make_root();
  check(root != NULL, "settings: temp dir");
  if (root == NULL) {
    return;
  }
  const char *id = "com.test.persist";
  char result1[1024];
  char result2[1024];
  snprintf(result1, sizeof(result1), "%s/result-1.txt", root);
  snprintf(result2, sizeof(result2), "%s/result-2.txt", root);
  check(write_settings_manifest(root, id, "true", "set", "false", result1),
        "settings: manifest");

  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.handshake_timeout_ms = 2000;
  limits.idle_stop_ms = 0;
  bd_plugin_host *host = make_host(root, limits);
  check(host != NULL, "settings: host");
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 1,
          "settings: scan (%s)", error);
    bd_plugin *plugin = bd_plugin_host_find(host, id);
    check(plugin != NULL, "settings: find");
    if (plugin != NULL) {
      check(strcmp(bd_plugin_get_setting(plugin, "show_meta"), "true") == 0,
            "settings: manifest default");
      check(bd_plugin_set_settings_open(plugin, true),
            "settings: open starts the plugin");
      char result[256] = {0};
      check(wait_for_result(host, result1, result, sizeof(result), 3000),
            "settings: protocol set result");
      check(strcmp(result, "ok") == 0, "settings: protocol set: %s", result);
      check(strcmp(bd_plugin_get_setting(plugin, "show_meta"), "false") == 0,
            "settings: value after protocol set");
    }
    bd_plugin_host_destroy(host);
  }

  char state_file[2048];
  settings_file_path(state_file, sizeof(state_file), root, id);
  char contents[2048] = {0};
  check(access(state_file, F_OK) == 0, "settings: state file exists");
  check(read_file_text(state_file, contents, sizeof(contents)),
        "settings: read state file");
  check(strstr(contents, "\"version\":1") != NULL,
        "settings: state file version");
  check(strstr(contents, "\"show_meta\":\"false\"") != NULL,
        "settings: state file value");
  char package_file[2048];
  snprintf(package_file, sizeof(package_file), "%s/%s/settings.json", root, id);
  check(access(package_file, F_OK) != 0,
        "settings: must not write into the package directory");

  check(write_settings_manifest(root, id, "true", "get", "false", result2),
        "settings: get manifest");
  host = make_host(root, limits);
  check(host != NULL, "settings: restart host");
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 1,
          "settings: restart scan (%s)", error);
    bd_plugin *plugin = bd_plugin_host_find(host, id);
    check(plugin != NULL, "settings: restart find");
    if (plugin != NULL) {
      check(strcmp(bd_plugin_get_setting(plugin, "show_meta"), "false") == 0,
            "settings: value loaded before the plugin starts");
      check(bd_plugin_set_settings_open(plugin, true),
            "settings: restart open");
      char result[256] = {0};
      check(wait_for_result(host, result2, result, sizeof(result), 3000),
            "settings: protocol get result");
      check(strcmp(result, "value false") == 0,
            "settings: protocol get: %s", result);
      check(bd_plugin_set_setting(plugin, "show_meta", "true"),
            "settings: host API persists");
    }
    bd_plugin_host_destroy(host);
  }

  host = make_host(root, limits);
  check(host != NULL, "settings: third host");
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 1,
          "settings: third scan (%s)", error);
    bd_plugin *plugin = bd_plugin_host_find(host, id);
    check(plugin != NULL, "settings: third find");
    if (plugin != NULL) {
      check(strcmp(bd_plugin_get_setting(plugin, "show_meta"), "true") == 0,
            "settings: host API value loaded after restart");
    }
    bd_plugin_host_destroy(host);
  }
  remove_tree(root);
  free(root);
}

static void test_settings_isolation(void) {
  char *root = make_root();
  check(root != NULL, "isolation: temp dir");
  if (root == NULL) {
    return;
  }
  const char *alpha = "com.test.alpha";
  const char *beta = "com.test.beta";
  char alpha_result1[1024];
  char beta_result1[1024];
  char alpha_result2[1024];
  char beta_result2[1024];
  snprintf(alpha_result1, sizeof(alpha_result1), "%s/alpha-1.txt", root);
  snprintf(beta_result1, sizeof(beta_result1), "%s/beta-1.txt", root);
  snprintf(alpha_result2, sizeof(alpha_result2), "%s/alpha-2.txt", root);
  snprintf(beta_result2, sizeof(beta_result2), "%s/beta-2.txt", root);
  check(write_settings_manifest(root, alpha, "true", "set", "false",
                                alpha_result1),
        "isolation: alpha manifest");
  check(write_settings_manifest(root, beta, "false", "set", "true",
                                beta_result1),
        "isolation: beta manifest");

  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.handshake_timeout_ms = 2000;
  limits.idle_stop_ms = 0;
  bd_plugin_host *host = make_host(root, limits);
  check(host != NULL, "isolation: host");
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 2,
          "isolation: scan (%s)", error);
    bd_plugin *alpha_plugin = bd_plugin_host_find(host, alpha);
    bd_plugin *beta_plugin = bd_plugin_host_find(host, beta);
    check(alpha_plugin != NULL && beta_plugin != NULL, "isolation: find");
    if (alpha_plugin != NULL) {
      check(bd_plugin_set_settings_open(alpha_plugin, true),
            "isolation: start alpha");
      char result[256] = {0};
      check(wait_for_result(host, alpha_result1, result, sizeof(result), 3000),
            "isolation: alpha set result");
      check(strcmp(result, "ok") == 0, "isolation: alpha set: %s", result);
      check(strcmp(bd_plugin_get_setting(alpha_plugin, "show_meta"), "false") ==
                0,
            "isolation: alpha value");
    }
    if (beta_plugin != NULL) {
      check(bd_plugin_set_settings_open(beta_plugin, true),
            "isolation: start beta");
      char result[256] = {0};
      check(wait_for_result(host, beta_result1, result, sizeof(result), 3000),
            "isolation: beta set result");
      check(strcmp(result, "ok") == 0, "isolation: beta set: %s", result);
      check(strcmp(bd_plugin_get_setting(beta_plugin, "show_meta"), "true") ==
                0,
            "isolation: beta value");
    }
    bd_plugin_host_destroy(host);
  }

  char alpha_file[2048];
  char beta_file[2048];
  char contents[2048] = {0};
  settings_file_path(alpha_file, sizeof(alpha_file), root, alpha);
  settings_file_path(beta_file, sizeof(beta_file), root, beta);
  check(strcmp(alpha_file, beta_file) != 0, "isolation: distinct paths");
  check(access(alpha_file, F_OK) == 0, "isolation: alpha file");
  check(access(beta_file, F_OK) == 0, "isolation: beta file");
  check(read_file_text(alpha_file, contents, sizeof(contents)) &&
            strstr(contents, "\"show_meta\":\"false\"") != NULL,
        "isolation: alpha file value");
  check(read_file_text(beta_file, contents, sizeof(contents)) &&
            strstr(contents, "\"show_meta\":\"true\"") != NULL,
        "isolation: beta file value");

  check(write_settings_manifest(root, alpha, "true", "get", "true",
                                alpha_result2),
        "isolation: alpha get manifest");
  check(write_settings_manifest(root, beta, "false", "get", "false",
                                beta_result2),
        "isolation: beta get manifest");
  host = make_host(root, limits);
  check(host != NULL, "isolation: restart host");
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 2,
          "isolation: restart scan (%s)", error);
    bd_plugin *alpha_plugin = bd_plugin_host_find(host, alpha);
    bd_plugin *beta_plugin = bd_plugin_host_find(host, beta);
    check(alpha_plugin != NULL && beta_plugin != NULL,
          "isolation: restart find");
    if (alpha_plugin != NULL) {
      check(strcmp(bd_plugin_get_setting(alpha_plugin, "show_meta"), "false") ==
                0,
            "isolation: alpha loaded");
      check(bd_plugin_set_settings_open(alpha_plugin, true),
            "isolation: restart alpha");
      char result[256] = {0};
      check(wait_for_result(host, alpha_result2, result, sizeof(result), 3000),
            "isolation: alpha get result");
      check(strcmp(result, "value false") == 0,
            "isolation: alpha get: %s", result);
    }
    if (beta_plugin != NULL) {
      check(strcmp(bd_plugin_get_setting(beta_plugin, "show_meta"), "true") ==
                0,
            "isolation: beta loaded");
      check(bd_plugin_set_settings_open(beta_plugin, true),
            "isolation: restart beta");
      char result[256] = {0};
      check(wait_for_result(host, beta_result2, result, sizeof(result), 3000),
            "isolation: beta get result");
      check(strcmp(result, "value true") == 0,
            "isolation: beta get: %s", result);
    }
    bd_plugin_host_destroy(host);
  }
  remove_tree(root);
  free(root);
}

static void test_settings_corruption(void) {
  char *root = make_root();
  check(root != NULL, "corrupt: temp dir");
  if (root == NULL) {
    return;
  }
  const char *invalid_json = "com.test.corrupt";
  const char *invalid_value = "com.test.badvalue";
  char result[1024];
  snprintf(result, sizeof(result), "%s/corrupt-result.txt", root);
  check(write_settings_manifest(root, invalid_json, "true", "get", "true",
                                result),
        "corrupt: invalid JSON manifest");
  check(write_settings_manifest(root, invalid_value, "true", "get", "true",
                                result),
        "corrupt: invalid value manifest");
  check(make_settings_test_dir(root, invalid_json), "corrupt: state dir");
  check(make_settings_test_dir(root, invalid_value), "corrupt: value dir");
  char path[2048];
  settings_file_path(path, sizeof(path), root, invalid_json);
  check(write_text(path, "{not json"), "corrupt: write invalid JSON");
  settings_file_path(path, sizeof(path), root, invalid_value);
  check(write_text(path, "{\"version\":1,\"settings\":{\"show_meta\":\"maybe\"}}"),
        "corrupt: write invalid value");

  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.handshake_timeout_ms = 2000;
  limits.idle_stop_ms = 0;
  bd_plugin_host *host = make_host(root, limits);
  check(host != NULL, "corrupt: host");
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 2,
          "corrupt: scan (%s)", error);
    int setting_diagnostics = 0;
    for (size_t i = 0; i < bd_plugin_host_diagnostic_count(host); ++i) {
      char diagnostic[256];
      if (bd_plugin_host_diagnostic(host, i, diagnostic, sizeof(diagnostic)) &&
          strstr(diagnostic, "settings") != NULL) {
        ++setting_diagnostics;
      }
    }
    check(setting_diagnostics >= 2,
          "corrupt: expected >=2 settings diagnostics, got %d",
          setting_diagnostics);
    bd_plugin *plugin = bd_plugin_host_find(host, invalid_json);
    bd_plugin *bad_value = bd_plugin_host_find(host, invalid_value);
    check(plugin != NULL && bad_value != NULL, "corrupt: find");
    if (plugin != NULL) {
      check(strcmp(bd_plugin_get_setting(plugin, "show_meta"), "true") == 0,
            "corrupt: invalid JSON used the default");
      check(bd_plugin_set_settings_open(plugin, true),
            "corrupt: start plugin");
      char result_text[256] = {0};
      check(wait_for_result(host, result, result_text, sizeof(result_text),
                            3000),
            "corrupt: get result");
      check(strcmp(result_text, "value true") == 0,
            "corrupt: protocol get: %s", result_text);
    }
    if (bad_value != NULL) {
      check(strcmp(bd_plugin_get_setting(bad_value, "show_meta"), "true") == 0,
            "corrupt: invalid value used the default");
    }
    bd_plugin_host_destroy(host);
  }
  remove_tree(root);
  free(root);
}

static void test_settings_write_failure(void) {
  char *root = make_root();
  check(root != NULL, "write failure: temp dir");
  if (root == NULL) {
    return;
  }
  const char *id = "com.test.writefail";
  char result[1024];
  snprintf(result, sizeof(result), "%s/writefail-result.txt", root);
  check(write_settings_manifest(root, id, "true", "set", "false", result),
        "write failure: manifest");

  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.handshake_timeout_ms = 2000;
  limits.idle_stop_ms = 0;
  setenv("BRICKDESK_PLUGIN_SETTINGS_FAIL_WRITE", "0", 1);
  bd_plugin_host *host = make_host(root, limits);
  check(host != NULL, "write failure: host");
  char state_file[2048];
  settings_file_path(state_file, sizeof(state_file), root, id);
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 1,
          "write failure: scan (%s)", error);
    bd_plugin *plugin = bd_plugin_host_find(host, id);
    check(plugin != NULL, "write failure: find");
    if (plugin != NULL) {
      check(bd_plugin_set_settings_open(plugin, true),
            "write failure: start plugin");
      char result_text[256] = {0};
      check(wait_for_result(host, result, result_text, sizeof(result_text),
                            3000),
            "write failure: protocol result");
      check(strncmp(result_text, "error -32603", 12) == 0,
            "write failure: expected -32603, got %s", result_text);
      check(strstr(result_text, "injected write failure") != NULL,
            "write failure: error text: %s", result_text);
      check(strcmp(bd_plugin_get_setting(plugin, "show_meta"), "true") == 0,
            "write failure: value rolled back");
      check(access(state_file, F_OK) != 0,
            "write failure: failed write left no file");

      unsetenv("BRICKDESK_PLUGIN_SETTINGS_FAIL_WRITE");
      check(bd_plugin_set_setting(plugin, "show_meta", "false"),
            "write failure: host API succeeds after recovery");
      check(strcmp(bd_plugin_get_setting(plugin, "show_meta"), "false") == 0,
            "write failure: recovered value");

      setenv("BRICKDESK_PLUGIN_SETTINGS_FAIL_WRITE", "0", 1);
      check(!bd_plugin_set_setting(plugin, "show_meta", "true"),
            "write failure: host API reports the failure");
      check(strcmp(bd_plugin_get_setting(plugin, "show_meta"), "false") == 0,
            "write failure: host API rolled back");
      char contents[2048] = {0};
      check(read_file_text(state_file, contents, sizeof(contents)) &&
                strstr(contents, "\"show_meta\":\"false\"") != NULL,
            "write failure: previous file preserved");
      unsetenv("BRICKDESK_PLUGIN_SETTINGS_FAIL_WRITE");
    }
    bd_plugin_host_destroy(host);
  }
  unsetenv("BRICKDESK_PLUGIN_SETTINGS_FAIL_WRITE");

  host = make_host(root, limits);
  check(host != NULL, "write failure: reload host");
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 1,
          "write failure: reload scan (%s)", error);
    bd_plugin *plugin = bd_plugin_host_find(host, id);
    check(plugin != NULL, "write failure: reload find");
    if (plugin != NULL) {
      check(strcmp(bd_plugin_get_setting(plugin, "show_meta"), "false") == 0,
            "write failure: last successful value loaded");
    }
    bd_plugin_host_destroy(host);
  }
  remove_tree(root);
  free(root);
}

static void test_settings_directory_sync_failure(void) {
  char *root = make_root();
  check(root != NULL, "dir sync: temp dir");
  if (root == NULL) {
    return;
  }
  const char *id = "com.test.dirsync";
  char result[1024];
  snprintf(result, sizeof(result), "%s/dirsync-result.txt", root);
  check(write_settings_manifest(root, id, "true", "set", "false", result),
        "dir sync: manifest");

  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.handshake_timeout_ms = 2000;
  limits.idle_stop_ms = 0;
  setenv("BRICKDESK_PLUGIN_SETTINGS_FAIL_DIRSYNC", "1", 1);
  bd_plugin_host *host = make_host(root, limits);
  check(host != NULL, "dir sync: host");
  char state_file[2048];
  settings_file_path(state_file, sizeof(state_file), root, id);
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 1,
          "dir sync: scan (%s)", error);
    bd_plugin *plugin = bd_plugin_host_find(host, id);
    check(plugin != NULL, "dir sync: find");
    if (plugin != NULL) {
      check(bd_plugin_set_settings_open(plugin, true),
            "dir sync: start plugin");
      char result_text[256] = {0};
      check(wait_for_result(host, result, result_text, sizeof(result_text),
                            3000),
            "dir sync: protocol result");
      check(strcmp(result_text, "unsynced") == 0,
            "dir sync: expected durable:false, got %s", result_text);
      check(strcmp(bd_plugin_get_setting(plugin, "show_meta"), "false") == 0,
            "dir sync: memory keeps the new value");
      char contents[2048] = {0};
      check(read_file_text(state_file, contents, sizeof(contents)) &&
                strstr(contents, "\"show_meta\":\"false\"") != NULL,
            "dir sync: visible file keeps the new value");
      int not_durable = 0;
      for (size_t i = 0; i < bd_plugin_host_diagnostic_count(host); ++i) {
        char diagnostic[256];
        if (bd_plugin_host_diagnostic(host, i, diagnostic, sizeof(diagnostic)) &&
            strstr(diagnostic, "not durable") != NULL) {
          ++not_durable;
        }
      }
      check(not_durable >= 1, "dir sync: expected a not-durable diagnostic");

      size_t diagnostics_before = bd_plugin_host_diagnostic_count(host);
      check(bd_plugin_set_setting(plugin, "show_meta", "true"),
            "dir sync: host API reports the applied value");
      check(strcmp(bd_plugin_get_setting(plugin, "show_meta"), "true") == 0,
            "dir sync: host API keeps the new value");
      check(bd_plugin_host_diagnostic_count(host) > diagnostics_before,
            "dir sync: host API reports the sync failure");

      unsetenv("BRICKDESK_PLUGIN_SETTINGS_FAIL_DIRSYNC");
      check(bd_plugin_set_setting(plugin, "show_meta", "false"),
            "dir sync: recovery save succeeds");
      check(strcmp(bd_plugin_get_setting(plugin, "show_meta"), "false") == 0,
            "dir sync: recovery value");
    }
    bd_plugin_host_destroy(host);
  }
  unsetenv("BRICKDESK_PLUGIN_SETTINGS_FAIL_DIRSYNC");

  host = make_host(root, limits);
  check(host != NULL, "dir sync: reload host");
  if (host != NULL) {
    char error[256] = {0};
    check(bd_plugin_host_scan(host, error, sizeof(error)) == 1,
          "dir sync: reload scan (%s)", error);
    bd_plugin *plugin = bd_plugin_host_find(host, id);
    check(plugin != NULL, "dir sync: reload find");
    if (plugin != NULL) {
      check(strcmp(bd_plugin_get_setting(plugin, "show_meta"), "false") == 0,
            "dir sync: recovery value loaded");
    }
    bd_plugin_host_destroy(host);
  }
  remove_tree(root);
  free(root);
}

static void test_python_plugin(void) {
  bd_plugin_limits limits = bd_plugin_default_limits();
  limits.handshake_timeout_ms = 5000;
  limits.idle_stop_ms = 0;
  char runtime[] = "/tmp/bd-plugin-python-XXXXXX";
  char *runtime_dir = mkdtemp(runtime);
  check(runtime_dir != NULL, "python: runtime dir");
  if (runtime_dir == NULL) {
    return;
  }
  bd_plugin_host_config config;
  memset(&config, 0, sizeof(config));
  config.plugin_dir = BRICKDESK_TEST_PLUGIN_DIR;
  config.runtime_dir = runtime_dir;
  char state_dir[1024];
  snprintf(state_dir, sizeof(state_dir), "%s/state", runtime_dir);
  config.state_dir = state_dir;
  config.limits = &limits;
  config.display_width = 1024;
  config.display_height = 768;
  config.dark = true;
  config.locale = "en-US";
  bd_plugin_host *host = bd_plugin_host_create(&config);
  check(host != NULL, "python: host");
  if (host != NULL) {
    char error[256] = {0};
    int count = bd_plugin_host_scan(host, error, sizeof(error));
    check(count >= 1, "python: scan count=%d (%s)", count, error);
    for (size_t i = 0; i < bd_plugin_host_diagnostic_count(host); ++i) {
      char diagnostic[256];
      if (bd_plugin_host_diagnostic(host, i, diagnostic, sizeof(diagnostic))) {
        fprintf(stderr, "python diagnostic: %s\n", diagnostic);
      }
    }
    bd_plugin *plugin = bd_plugin_host_find(host, "com.brickdesk.hello-py");
    check(plugin != NULL, "python: hello-py plugin discovered");
    if (plugin != NULL) {
      bd_plugin_set_tile_visible(plugin, true);
      check(wait_for_state(host, plugin, BD_PLUGIN_RUNNING, 7000),
            "python: expected running, got %s",
            bd_plugin_state_name(bd_plugin_state_of(plugin)));
      check(wait_for_ui(host, plugin, 2000), "python: expected UI");
      const bd_plugin_ui *tile = bd_plugin_ui_tile(plugin);
      const bd_plugin_ui *settings = bd_plugin_ui_settings(plugin);
      check(tile != NULL, "python: tile UI");
      check(settings != NULL, "python: settings UI");
      if (settings != NULL) {
        bool slider = false;
        bool select = false;
        for (int i = 0; i < settings->node_count; ++i) {
          if (settings->nodes[i].type == BD_PLUGIN_UI_SLIDER) {
            slider = true;
          }
          if (settings->nodes[i].type == BD_PLUGIN_UI_SELECT) {
            select = true;
          }
        }
        check(slider && select, "python: slider and select settings");
      }
      int generation = bd_plugin_ui_generation(plugin);
      check(bd_plugin_set_setting(plugin, "show_meta", "false"),
            "python: set setting");
      uint64_t deadline = now_ms() + 3000;
      bool updated = false;
      while (now_ms() < deadline) {
        bd_plugin_host_poll(host, 10);
        if (bd_plugin_ui_generation(plugin) > generation) {
          updated = true;
          break;
        }
      }
      check(updated, "python: plugin did not update its UI after settings.changed");
    }
    bd_plugin_host_destroy(host);
  }
  remove_tree(runtime_dir);
}

int main(void) {
  test_manifest_scan();
  test_handshake_and_ui();
  test_handshake_timeout();
  test_crash_backoff();
  test_frame_sequence();
  test_ui_validation();
  test_invalid_ui_message();
  test_idle_stop_and_restart();
  test_settings_persistence();
  test_settings_isolation();
  test_settings_corruption();
  test_settings_write_failure();
  test_settings_directory_sync_failure();
  test_memory_limit();
  test_desktop_frames_with_busy_plugin();
  test_python_plugin();

  if (g_failures != 0) {
    fprintf(stderr, "plugin_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("plugin_test: ok\n");
  return 0;
}
