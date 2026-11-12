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

static bd_plugin_host *make_host_paths(const char *plugin_dir,
                                      const char *root,
                                      bd_plugin_limits limits) {
  char runtime[1024];
  snprintf(runtime, sizeof(runtime), "%s/runtime", root);
  bd_plugin_host_config config;
  memset(&config, 0, sizeof(config));
  config.plugin_dir = plugin_dir;
  config.runtime_dir = runtime;
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
      theme.dark = true;
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
