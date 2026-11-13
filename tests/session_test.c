#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "brickdesk/app.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_PATH 1024
#define TEST_EVENTS 256
#define TEST_APP_JSON 8192

typedef struct logged_event {
  bd_session_event_kind kind;
  bd_session_state previous;
  bd_session_state state;
  char app_id[BD_APP_ID_MAX];
  char hook[32];
  bool hook_ok;
  int exit_code;
  int term_signal;
  char suggested_app_id[BD_APP_ID_MAX];
} logged_event;

typedef struct event_log {
  logged_event items[TEST_EVENTS];
  size_t count;
} event_log;

static int g_failures;
static char g_root[128];
static char g_child_path[PATH_MAX];
static char g_demo_path[PATH_MAX];

static void check(bool condition, const char *format, ...) {
  if (condition) {
    return;
  }
  ++g_failures;
  fprintf(stderr, "session_test: ");
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
}

static void sleep_ms(int milliseconds) {
  struct timespec delay;
  delay.tv_sec = milliseconds / 1000;
  delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
  nanosleep(&delay, NULL);
}

static bool __attribute__((noinline)) join_path(const char *dir,
                                                 const char *name, char *out,
                                                 size_t out_size) {
  int written = snprintf(out, out_size, "%s/%s", dir, name);
  return written > 0 && (size_t)written < out_size;
}

static bool make_dir(const char *path) {
  return mkdir(path, 0755) == 0 || errno == EEXIST;
}

static bool write_file(const char *path, const char *content) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs(content, file) >= 0;
  if (fclose(file) != 0) {
    ok = false;
  }
  return ok;
}

static char *read_file(const char *path) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  char *content = (char *)malloc((size_t)size + 1);
  if (content == NULL) {
    fclose(file);
    return NULL;
  }
  size_t got = fread(content, 1, (size_t)size, file);
  fclose(file);
  if (got != (size_t)size) {
    free(content);
    return NULL;
  }
  content[got] = '\0';
  return content;
}

static size_t count_occurrences(const char *path, const char *needle) {
  char *content = read_file(path);
  if (content == NULL) {
    return 0;
  }
  size_t count = 0;
  size_t length = strlen(needle);
  const char *cursor = content;
  while ((cursor = strstr(cursor, needle)) != NULL) {
    ++count;
    cursor += length;
  }
  free(content);
  return count;
}

static void on_event(void *user, const bd_session_event *event) {
  event_log *log = (event_log *)user;
  if (log->count >= TEST_EVENTS) {
    return;
  }
  logged_event *entry = &log->items[log->count++];
  memset(entry, 0, sizeof(*entry));
  entry->kind = event->kind;
  entry->previous = event->previous;
  entry->state = event->state;
  entry->hook_ok = event->hook_ok;
  entry->exit_code = event->exit_code;
  entry->term_signal = event->term_signal;
  if (event->app_id != NULL) {
    snprintf(entry->app_id, sizeof(entry->app_id), "%s", event->app_id);
  }
  if (event->hook != NULL) {
    snprintf(entry->hook, sizeof(entry->hook), "%s", event->hook);
  }
  if (event->suggested_app_id[0] != '\0') {
    snprintf(entry->suggested_app_id, sizeof(entry->suggested_app_id), "%s",
             event->suggested_app_id);
  }
}

static size_t event_count(const event_log *log, bd_session_event_kind kind,
                          const char *app_id) {
  size_t count = 0;
  for (size_t i = 0; i < log->count; ++i) {
    if (log->items[i].kind == kind &&
        (app_id == NULL || strcmp(log->items[i].app_id, app_id) == 0)) {
      ++count;
    }
  }
  return count;
}

static bool find_hook_event(const event_log *log, const char *app_id,
                            const char *hook, bool *ok) {
  for (size_t i = 0; i < log->count; ++i) {
    const logged_event *event = &log->items[i];
    if (event->kind == BD_SESSION_EVENT_HOOK &&
        strcmp(event->app_id, app_id) == 0 && strcmp(event->hook, hook) == 0) {
      if (ok != NULL) {
        *ok = event->hook_ok;
      }
      return true;
    }
  }
  return false;
}

static bd_session *create_session(const char *name, int max_background,
                                  event_log *log, bd_session_timeouts timeouts,
                                  bd_session_perf_ops perf) {
  char socket_dir[TEST_PATH];
  char frame_dir[TEST_PATH];
  char state_root[TEST_PATH];
  snprintf(socket_dir, sizeof(socket_dir), "%s/socket-%s", g_root, name);
  snprintf(frame_dir, sizeof(frame_dir), "%s/frame-%s", g_root, name);
  snprintf(state_root, sizeof(state_root), "%s/state-%s", g_root, name);

  bd_session_config config;
  memset(&config, 0, sizeof(config));
  config.socket_dir = socket_dir;
  config.frame_dir = frame_dir;
  config.state_root = state_root;
  config.session_id = name;
  config.max_background = max_background;
  config.timeouts = timeouts;
  config.perf = perf;
  config.on_event = on_event;
  config.event_user = log;
  char error[256] = {0};
  bd_session *session = bd_session_create(&config, error, sizeof(error));
  if (session == NULL) {
    check(false, "create session %s: %s", name, error);
  }
  return session;
}

static bool make_app(const char *directory, const char *id, const char *name,
                     const char *exec_path, const char *mode, const char *hooks,
                     const char *capabilities, const char *state_path,
                     const char *art_fields, bd_app *app) {
  char dir[TEST_PATH];
  snprintf(dir, sizeof(dir), "%s/apps/%s", g_root, directory);
  make_dir(dir);
  char log_path[TEST_PATH];
  join_path(dir, "child.log", log_path, sizeof(log_path));
  char manifest[TEST_PATH];
  join_path(dir, "app.json", manifest, sizeof(manifest));

  char exec_json[TEST_PATH * 2];
  if (mode != NULL) {
    snprintf(exec_json, sizeof(exec_json), "\"%s\",\"%s\"", exec_path, mode);
  } else {
    snprintf(exec_json, sizeof(exec_json), "\"%s\"", exec_path);
  }
  char state_json[TEST_PATH + 128];
  if (state_path != NULL) {
    snprintf(state_json, sizeof(state_json),
             ",\"state\":{\"path\":\"%s\",\"format\":\"text\",\"max_slots\":1}",
             state_path);
  } else {
    state_json[0] = '\0';
  }
  char art_json[TEST_PATH];
  if (art_fields != NULL) {
    snprintf(art_json, sizeof(art_json), ",\"art\":{%s}", art_fields);
  } else {
    art_json[0] = '\0';
  }
  char content[TEST_APP_JSON];
  snprintf(content, sizeof(content),
           "{\"schema\":1,\"id\":\"%s\",\"name\":\"%s\",\"version\":\"1.0\","
           "\"type\":\"app\",\"category\":\"app\",\"exec\":[%s],\"cwd\":\".\","
           "\"env\":{\"TEST_CHILD_LOG\":\"%s\"},"
           "\"capabilities\":{%s},\"hooks\":{%s},"
           "\"menu\":{\"mode\":\"overlay\"},\"perf\":{\"profile\":"
           "\"performance\"},\"accent\":\"#336699\"%s%s}",
           id, name, exec_json, log_path, capabilities != NULL ? capabilities : "",
           hooks != NULL ? hooks : "", state_json, art_json);
  if (!write_file(manifest, content)) {
    check(false, "write manifest %s failed", manifest);
    return false;
  }
  char error[512] = {0};
  if (!bd_app_parse_file(manifest, app, error, sizeof(error))) {
    check(false, "parse %s: %s", manifest, error);
    return false;
  }
  return true;
}

static bool wait_for_state(bd_session *session, const char *app_id,
                           bd_session_state state, int timeout_ms) {
  int waited = 0;
  while (waited <= timeout_ms) {
    bd_session_poll(session);
    bd_session_app_info info;
    if (bd_session_find(session, app_id, &info) && info.state == state) {
      return true;
    }
    sleep_ms(5);
    waited += 5;
  }
  bd_session_app_info info;
  if (bd_session_find(session, app_id, &info)) {
    fprintf(stderr, "session_test: %s state %s, wanted %s\n", app_id,
            bd_session_state_name(info.state), bd_session_state_name(state));
  }
  return false;
}

static char proc_state(int pid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/stat", pid);
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return '?';
  }
  char buffer[1024];
  char *line = fgets(buffer, sizeof(buffer), file);
  fclose(file);
  if (line == NULL) {
    return '?';
  }
  char *close = strrchr(buffer, ')');
  if (close == NULL || close[1] == '\0' || close[2] == '\0') {
    return '?';
  }
  return close[2];
}

static bool wait_proc_state(int pid, char wanted, int timeout_ms) {
  int waited = 0;
  while (waited <= timeout_ms) {
    if (proc_state(pid) == wanted) {
      return true;
    }
    sleep_ms(5);
    waited += 5;
  }
  return false;
}

static bool wait_proc_not_stopped(int pid, int timeout_ms) {
  int waited = 0;
  while (waited <= timeout_ms) {
    char state = proc_state(pid);
    if (state != 'T' && state != '?') {
      return true;
    }
    sleep_ms(5);
    waited += 5;
  }
  return false;
}

static void test_manifest_parse(void) {
  char dir[TEST_PATH];
  snprintf(dir, sizeof(dir), "%s/parse-app", g_root);
  make_dir(dir);
  char path[TEST_PATH];
  join_path(dir, "app.json", path, sizeof(path));
  const char *json =
      "{\"schema\":1,\"id\":\"dev.example.game\",\"name\":\"Example\","
      "\"subtitle\":\"Sub\",\"version\":\"2.1\",\"type\":\"game\","
      "\"category\":\"emulator\",\"exec\":[\"/bin/true\",\"--flag\"],"
      "\"cwd\":\".\",\"env\":{\"HOME\":\"/tmp\",\"LANG\":\"C\"},"
      "\"icon\":\"art/icon.png\",\"accent\":\"#a1b2c3\","
      "\"art\":{\"covers\":{\"1x1\":\"art/1.png\",\"3x2\":\"art/3x2.jpg\"},"
      "\"backdrop\":\"art/backdrop.jpg\",\"fallbackColor\":\"#112233\"},"
      "\"capabilities\":{\"pause\":true,\"resume\":true,\"save_state\":true,"
      "\"load_state\":true,\"screenshot\":true,\"background\":false,"
      "\"display_release\":true},"
      "\"hooks\":{\"pause\":{\"type\":\"ipc\",\"method\":\"app.pause\","
      "\"timeout_ms\":300},\"quit\":{\"type\":\"command\","
      "\"exec\":[\"/bin/true\",\"{state_dir}\"],\"timeout_ms\":2000}},"
      "\"menu\":{\"mode\":\"handoff\",\"overlay\":{\"safe\":true,"
      "\"exclude_keys\":[\"Menu\",\"X\"]}},"
      "\"perf\":{\"profile\":\"performance\",\"core\":\"snes9x\"},"
      "\"state\":{\"path\":\"state/slot.bin\",\"format\":\"retroarch\","
      "\"max_slots\":4},"
      "\"live\":{\"provider\":\"file\",\"state\":\"state/tile.json\","
      "\"refreshMs\":750,\"fields\":[\"title\",\"progress\",\"artwork\"]}}";
  write_file(path, json);
  bd_app app;
  char error[512] = {0};
  bool ok = bd_app_parse_file(path, &app, error, sizeof(error));
  check(ok, "full manifest should parse: %s", error);
  if (ok) {
    check(strcmp(app.id, "dev.example.game") == 0, "id");
    check(strcmp(app.name, "Example") == 0, "name");
    check(app.type == BD_APP_TYPE_GAME, "type");
    check(app.category == BD_APP_CATEGORY_EMULATOR, "category");
    check(app.exec_count == 2 && strcmp(app.exec[1], "--flag") == 0, "exec");
    check(app.env_count == 2, "env count");
    check(strcmp(app.accent, "#A1B2C3") == 0, "accent");
    check(strcmp(app.covers[BD_APP_SPAN_1X1], "art/1.png") == 0, "1x1");
    check(strcmp(app.covers[BD_APP_SPAN_3X2], "art/3x2.jpg") == 0, "3x2");
    check(strcmp(app.fallback_color, "#112233") == 0, "fallback colour");
    check(app.caps.pause && app.caps.display_release, "capabilities");
    check(app.hooks[BD_APP_HOOK_PAUSE].type == BD_APP_HOOK_IPC &&
              app.hooks[BD_APP_HOOK_PAUSE].timeout_ms == 300,
          "pause hook");
    check(app.hooks[BD_APP_HOOK_QUIT].type == BD_APP_HOOK_COMMAND &&
              app.hooks[BD_APP_HOOK_QUIT].exec_count == 2,
          "command quit hook");
    check(app.menu.mode == BD_APP_MENU_HANDOFF && app.menu.overlay_safe,
          "menu");
    check(app.menu.exclude_key_count == 2, "exclude keys");
    check(strcmp(app.perf.profile, "performance") == 0 &&
              strcmp(app.perf.core, "snes9x") == 0,
          "perf");
    check(strcmp(app.state.path, "state/slot.bin") == 0 &&
              app.state.max_slots == 4,
          "state");
    check(app.live.provider == BD_APP_LIVE_FILE && app.live.refresh_ms == 750,
          "live");
    check((app.live.fields & BD_APP_LIVE_FIELD_TITLE) != 0 &&
              (app.live.fields & BD_APP_LIVE_FIELD_PROGRESS) != 0,
          "live fields");
    bd_app copy;
    check(bd_app_copy(&copy, &app), "app copy");
    check(strcmp(copy.id, app.id) == 0 && copy.exec_count == app.exec_count,
          "copied app");
    bd_app_destroy(&copy);
    bd_app_destroy(&app);
  }

  const char *invalid[] = {
      "{\"schema\":1,\"name\":\"x\",\"type\":\"app\",\"exec\":[\"/bin/true\"]}",
      "{\"schema\":2,\"id\":\"a.b\",\"name\":\"x\",\"type\":\"app\","
      "\"exec\":[\"/bin/true\"]}",
      "{\"schema\":1,\"id\":\"Bad\",\"name\":\"x\",\"type\":\"app\","
      "\"exec\":[\"/bin/true\"]}",
      "{\"schema\":1,\"id\":\"a.b\",\"name\":\"x\",\"exec\":[\"/bin/true\"]}",
      "{\"schema\":1,\"id\":\"a.b\",\"name\":\"x\",\"type\":\"app\","
      "\"exec\":[]}",
      "{\"schema\":1,\"id\":\"a.b\",\"name\":\"x\",\"type\":\"app\","
      "\"exec\":[\"/bin/true\"],\"accent\":\"red\"}",
      "{\"schema\":1,\"id\":\"a.b\",\"name\":\"x\",\"type\":\"app\","
      "\"exec\":[\"/bin/true\"],\"art\":{\"covers\":{\"1x1\":\"../x.png\"}}}",
      "{\"schema\":1,\"id\":\"a.b\",\"name\":\"x\",\"type\":\"app\","
      "\"exec\":[\"/bin/true\"],\"menu\":{\"mode\":\"floating\"}}",
      "{\"schema\":1,\"id\":\"a.b\",\"name\":\"x\",\"type\":\"app\","
      "\"exec\":[\"/bin/true\"],\"hooks\":{\"pause\":{\"type\":\"bad\"}}}",
      "{\"schema\":1,\"id\":\"a.b\",\"name\":\"x\",\"type\":\"app\","
      "\"exec\":[\"/bin/true\"],\"state\":{\"path\":\"../x\"}}",
      "{not json}",
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    bd_app bad;
    char message[512] = {0};
    bool parsed = bd_app_parse_json(invalid[i], strlen(invalid[i]), dir,
                                    "<test>", &bad, message, sizeof(message));
    check(!parsed && message[0] != '\0', "invalid manifest %zu accepted", i);
    bd_app_destroy(&bad);
  }
}

typedef struct diagnostic_state {
  int count;
} diagnostic_state;

static void scan_diagnostic(void *user, const char *path, const char *message) {
  (void)path;
  (void)message;
  diagnostic_state *state = (diagnostic_state *)user;
  ++state->count;
}

static void test_scan_and_assets(void) {
  char root[TEST_PATH];
  snprintf(root, sizeof(root), "%s/scan", g_root);
  make_dir(root);
  const char *names[] = {"alpha", "beta", "gamma", "broken"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    char child[TEST_PATH];
    join_path(root, names[i], child, sizeof(child));
    make_dir(child);
    char art[TEST_PATH];
    join_path(child, "art", art, sizeof(art));
    make_dir(art);
  }

  char path[TEST_PATH];
  join_path(root, "alpha/app.json", path, sizeof(path));
  write_file(path,
             "{\"schema\":1,\"id\":\"dev.alpha\",\"name\":\"Alpha\","
             "\"type\":\"app\",\"exec\":[\"/bin/true\"],"
             "\"art\":{\"icon\":\"art/icon.png\","
             "\"covers\":{\"1x1\":\"art/cover-1x1.png\","
             "\"2x1\":\"art/escape.png\"}}}");
  join_path(root, "alpha/art/cover-1x1.png", path, sizeof(path));
  write_file(path, "");
  join_path(root, "alpha/art/icon.png", path, sizeof(path));
  write_file(path, "");
  join_path(root, "alpha/art/escape.png", path, sizeof(path));
  char outside[TEST_PATH];
  join_path(g_root, "outside.png", outside, sizeof(outside));
  write_file(outside, "outside");
  unlink(path);
  check(symlink(outside, path) == 0, "create symlink fixture: %s",
        strerror(errno));

  join_path(root, "beta/manifest.json", path, sizeof(path));
  write_file(path,
             "{\"schema\":1,\"id\":\"dev.beta\",\"name\":\"Beta\","
             "\"type\":\"app\",\"exec\":[\"/bin/true\"]}");

  join_path(root, "gamma/app.json", path, sizeof(path));
  write_file(path,
             "{\"schema\":1,\"id\":\"dev.gamma\",\"name\":\"Gamma\","
             "\"type\":\"app\",\"exec\":[\"/bin/true\"],"
             "\"art\":{\"icon\":\"art/icon.png\"}}");
  join_path(root, "gamma/art/icon.png", path, sizeof(path));
  write_file(path, "");

  join_path(root, "broken/app.json", path, sizeof(path));
  write_file(path, "{broken}");

  bd_app_list *list = bd_app_list_create();
  diagnostic_state diagnostics = {0};
  char error[256] = {0};
  bool ok = bd_app_list_scan(list, root, scan_diagnostic, &diagnostics, error,
                             sizeof(error));
  check(ok, "scan failed: %s", error);
  check(bd_app_list_count(list) == 3, "scan count %zu", bd_app_list_count(list));
  check(diagnostics.count == 1, "scan diagnostics %d", diagnostics.count);
  check(bd_app_list_at(list, 0) != NULL &&
            strcmp(bd_app_list_at(list, 0)->id, "dev.alpha") == 0,
        "scan order");

  const bd_app *alpha = bd_app_list_find(list, "dev.alpha");
  const bd_app *beta = bd_app_list_find(list, "dev.beta");
  const bd_app *gamma = bd_app_list_find(list, "dev.gamma");
  check(alpha != NULL && beta != NULL && gamma != NULL, "find apps");

  bd_app_asset asset;
  check(bd_app_asset_cover(alpha, BD_APP_SPAN_3X2, &asset) &&
            asset.kind == BD_APP_ASSET_COVER,
        "3x2 falls back to a cover");
  check(strstr(asset.path, "cover-1x1.png") != NULL,
        "3x2 fallback path %s", asset.path);
  check(bd_app_asset_cover(alpha, BD_APP_SPAN_2X1, &asset) &&
            strstr(asset.path, "cover-1x1.png") != NULL,
        "symlink escape falls back to 1x1, got %s", asset.path);
  check(bd_app_asset_cover(gamma, BD_APP_SPAN_1X1, &asset) &&
            asset.kind == BD_APP_ASSET_ICON,
        "icon fallback");
  check(bd_app_asset_cover(beta, BD_APP_SPAN_1X1, &asset) &&
            asset.kind == BD_APP_ASSET_MATERIAL &&
            strcmp(asset.color, "#A8674F") == 0 &&
            strcmp(asset.initial, "B") == 0,
        "material fallback %s %s", asset.color, asset.initial);
  check(bd_app_asset_backdrop(beta, &asset) &&
            asset.kind == BD_APP_ASSET_MATERIAL,
        "backdrop material fallback");
  bd_app_list_destroy(list);
}

static bool write_frame_file(const char *path, uint32_t width, uint32_t height,
                             uint32_t format, uint64_t seq, uint32_t magic,
                             uint16_t version, uint32_t stride,
                             bool fill_pixels) {
  uint32_t bpp = format == BD_APP_PIXEL_RGBA8888 ? 4u : 2u;
  size_t pixel_size = (size_t)stride * height;
  size_t total = sizeof(bd_app_last_frame_header) + pixel_size;
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return false;
  }
  if (ftruncate(fd, (off_t)total) != 0) {
    close(fd);
    return false;
  }
  uint8_t *map = (uint8_t *)malloc(total);
  if (map == NULL) {
    close(fd);
    return false;
  }
  memset(map, 0, total);
  bd_app_last_frame_header header;
  memset(&header, 0, sizeof(header));
  header.magic = magic;
  header.version = version;
  header.header_size = (uint16_t)sizeof(header);
  header.width = width;
  header.height = height;
  header.format = format;
  header.stride = stride;
  header.frame_seq = seq;
  header.data_size = pixel_size;
  memcpy(map, &header, sizeof(header));
  if (fill_pixels) {
    for (size_t y = 0; y < height; ++y) {
      for (size_t x = 0; x < width; ++x) {
        size_t offset = sizeof(header) + y * stride + x * bpp;
        map[offset + 0] = (uint8_t)(x + 1);
        map[offset + 1] = (uint8_t)(y + 2);
        map[offset + 2] = 0x40;
        if (bpp == 4) {
          map[offset + 3] = 0xff;
        }
      }
    }
  }
  bool ok = write(fd, map, total) == (ssize_t)total;
  free(map);
  close(fd);
  return ok;
}

static void test_last_frame(void) {
  char path[TEST_PATH];
  join_path(g_root, "frame-valid.bin", path, sizeof(path));
  check(write_frame_file(path, 2, 2, BD_APP_PIXEL_RGBA8888, 7,
                         BD_APP_LAST_FRAME_MAGIC, BD_APP_LAST_FRAME_VERSION, 8,
                         true),
        "write valid frame");
  bd_app_last_frame frame;
  char error[256] = {0};
  check(bd_app_last_frame_open(path, &frame, error, sizeof(error)),
        "open valid frame: %s", error);
  check(frame.header.width == 2 && frame.header.height == 2 &&
            frame.header.frame_seq == 7,
        "frame header");
  uint8_t pixels[16];
  bd_app_last_frame_header copied;
  check(bd_app_last_frame_copy(&frame, pixels, sizeof(pixels), &copied, error,
                               sizeof(error)),
        "copy frame: %s", error);
  check(pixels[0] == 1 && pixels[1] == 2 && pixels[4] == 2 &&
            pixels[5] == 2 && pixels[8] == 1 && pixels[9] == 3,
        "frame pixels");
  bd_app_last_frame_close(&frame);

  join_path(g_root, "frame-bad-magic.bin", path, sizeof(path));
  write_frame_file(path, 2, 2, BD_APP_PIXEL_RGBA8888, 1, 0,
                   BD_APP_LAST_FRAME_VERSION, 8, false);
  check(!bd_app_last_frame_open(path, &frame, error, sizeof(error)),
        "bad magic accepted");
  join_path(g_root, "frame-bad-version.bin", path, sizeof(path));
  write_frame_file(path, 2, 2, BD_APP_PIXEL_RGBA8888, 1,
                   BD_APP_LAST_FRAME_MAGIC, 9, 8, false);
  check(!bd_app_last_frame_open(path, &frame, error, sizeof(error)),
        "bad version accepted");
  join_path(g_root, "frame-bad-stride.bin", path, sizeof(path));
  write_frame_file(path, 2, 2, BD_APP_PIXEL_RGBA8888, 1,
                   BD_APP_LAST_FRAME_MAGIC, BD_APP_LAST_FRAME_VERSION, 2, false);
  check(!bd_app_last_frame_open(path, &frame, error, sizeof(error)),
        "small stride accepted");
  check(bd_app_last_frame_pixel_size(BD_APP_PIXEL_RGBA8888, 3, 4) == 48,
        "pixel size RGBA");
  check(bd_app_last_frame_pixel_size(BD_APP_PIXEL_RGB565, 3, 4) == 24,
        "pixel size RGB565");
}

static void test_session_sigstop(void) {
  event_log log;
  memset(&log, 0, sizeof(log));
  bd_session *session = create_session("sigstop", 2, &log,
                                       (bd_session_timeouts){0},
                                       (bd_session_perf_ops){0});
  check(session != NULL, "session");
  if (session == NULL) {
    return;
  }
  bd_app app;
  check(make_app("sigstop", "dev.sigstop", "Sigstop", g_child_path, "idle",
                 "", "", NULL, NULL, &app),
        "make sigstop app");
  bd_session_result result = bd_session_launch(session, &app, NULL, 0);
  check(result == BD_SESSION_OK, "launch %s", bd_session_result_name(result));
  check(wait_for_state(session, "dev.sigstop", BD_SESSION_FOREGROUND, 2000),
        "foreground");
  bd_session_app_info info;
  check(bd_session_find(session, "dev.sigstop", &info), "info");
  int pid = info.pid;
  check(pid > 0, "pid");
  check(bd_session_running_count(session) == 1, "running count");
  bd_session_app_info listed;
  check(bd_session_running_at(session, 0, &listed) &&
            strcmp(listed.app_id, "dev.sigstop") == 0,
        "running list");
  check(bd_session_background_count(session) == 0, "background count start");

  check(bd_session_background(session, "dev.sigstop") == BD_SESSION_OK,
        "background request");
  check(wait_for_state(session, "dev.sigstop", BD_SESSION_BACKGROUND, 2000),
        "background state");
  check(wait_proc_state(pid, 'T', 1000), "process did not stop: %c",
        proc_state(pid));

  check(bd_session_resume(session, "dev.sigstop") == BD_SESSION_OK, "resume");
  check(wait_for_state(session, "dev.sigstop", BD_SESSION_FOREGROUND, 2000),
        "foreground after resume");
  check(wait_proc_not_stopped(pid, 1000), "process stayed stopped");

  check(bd_session_background(session, "dev.sigstop") == BD_SESSION_OK,
        "background again");
  check(wait_for_state(session, "dev.sigstop", BD_SESSION_BACKGROUND, 2000),
        "background again state");
  check(wait_proc_state(pid, 'T', 1000), "process did not stop again");
  check(bd_session_terminate(session, "dev.sigstop") == BD_SESSION_OK,
        "terminate");
  check(wait_for_state(session, "dev.sigstop", BD_SESSION_EXITED, 3000),
        "exited after terminate");
  check(bd_session_find(session, "dev.sigstop", &info), "info after exit");
  check(info.term_signal == SIGTERM, "terminate signal %d", info.term_signal);
  check(bd_session_running_count(session) == 0, "running count after exit");
  check(!bd_session_running_at(session, 0, &listed), "running list after exit");
  check(bd_session_clear_terminated(session) == 1, "clear terminated");
  check(!bd_session_find(session, "dev.sigstop", &info),
        "terminated entry was not cleared");
  bd_app_destroy(&app);
  bd_session_destroy(session);
}

static void test_session_demo(void) {
  event_log log;
  memset(&log, 0, sizeof(log));
  bd_session *session = create_session("demo", 2, &log,
                                       (bd_session_timeouts){0},
                                       (bd_session_perf_ops){0});
  check(session != NULL, "demo session");
  if (session == NULL) {
    return;
  }
  bd_app app;
  check(make_app(
            "demo", "dev.brickdesk.demo", "Demo", g_demo_path, NULL,
            "\"pause\":{\"type\":\"ipc\",\"method\":\"app.pause\","
            "\"timeout_ms\":500},"
            "\"resume\":{\"type\":\"ipc\",\"method\":\"app.resume\","
            "\"timeout_ms\":2000},"
            "\"save\":{\"type\":\"ipc\",\"method\":\"state.save\","
            "\"timeout_ms\":2000},"
            "\"load\":{\"type\":\"ipc\",\"method\":\"state.load\","
            "\"timeout_ms\":2000},"
            "\"quit\":{\"type\":\"ipc\",\"method\":\"app.quit\","
            "\"timeout_ms\":1000}",
            "\"pause\":true,\"resume\":true,\"save_state\":true,"
            "\"load_state\":true,\"screenshot\":true,\"display_release\":true",
            "state.txt", NULL, &app),
        "make demo app");
  bd_session_result result = bd_session_launch(session, &app, NULL, 0);
  check(result == BD_SESSION_OK, "demo launch %s",
        bd_session_result_name(result));
  check(wait_for_state(session, "dev.brickdesk.demo", BD_SESSION_FOREGROUND,
                       5000),
        "demo foreground");
  bd_session_app_info info;
  check(bd_session_find(session, "dev.brickdesk.demo", &info), "demo info");
  check(info.ipc_connected, "demo did not connect IPC");

  check(bd_session_background(session, "dev.brickdesk.demo") == BD_SESSION_OK,
        "demo background");
  check(wait_for_state(session, "dev.brickdesk.demo", BD_SESSION_BACKGROUND,
                       3000),
        "demo background state");
  check(bd_session_find(session, "dev.brickdesk.demo", &info), "demo info bg");
  check(info.display_released, "demo did not release display");

  char frame_dir[TEST_PATH];
  snprintf(frame_dir, sizeof(frame_dir), "%s/frame-demo", g_root);
  char frame_path[TEST_PATH];
  check(bd_app_last_frame_path(frame_dir, "demo-00", frame_path,
                               sizeof(frame_path)),
        "demo frame path");
  int waited = 0;
  while (access(frame_path, R_OK) != 0 && waited < 2000) {
    bd_session_poll(session);
    sleep_ms(5);
    waited += 5;
  }
  bd_app_last_frame frame;
  char error[256] = {0};
  check(bd_app_last_frame_open(frame_path, &frame, error, sizeof(error)),
        "open demo frame: %s", error);
  if (frame.map != NULL) {
    check(frame.header.width == 512 && frame.header.height == 384,
          "demo frame %ux%u", frame.header.width, frame.header.height);
    size_t size = bd_app_last_frame_pixel_size(frame.header.format,
                                               frame.header.width,
                                               frame.header.height);
    uint8_t *pixels = (uint8_t *)malloc(size);
    check(pixels != NULL, "allocate frame pixels");
    if (pixels != NULL) {
      check(bd_app_last_frame_copy(&frame, pixels, size, NULL, error,
                                   sizeof(error)),
            "copy demo frame: %s", error);
      bool nonzero = false;
      for (size_t i = 0; i < size; i += 97) {
        if (pixels[i] != 0) {
          nonzero = true;
          break;
        }
      }
      check(nonzero, "demo frame is blank");
      free(pixels);
    }
    bd_app_last_frame_close(&frame);
  }

  check(bd_session_resume(session, "dev.brickdesk.demo") == BD_SESSION_OK,
        "demo resume");
  check(wait_for_state(session, "dev.brickdesk.demo", BD_SESSION_FOREGROUND,
                       3000),
        "demo foreground after resume");
  check(bd_session_find(session, "dev.brickdesk.demo", &info), "demo info fg");
  check(info.ipc_connected, "demo lost IPC after resume");

  check(bd_session_save_state(session, "dev.brickdesk.demo", 0) ==
            BD_SESSION_OK,
        "save state request");
  int save_waited = 0;
  while (!find_hook_event(&log, "dev.brickdesk.demo", "save", NULL) &&
         save_waited < 3000) {
    bd_session_poll(session);
    sleep_ms(5);
    save_waited += 5;
  }
  check(find_hook_event(&log, "dev.brickdesk.demo", "save", NULL),
        "save hook event");

  check(bd_session_terminate(session, "dev.brickdesk.demo") == BD_SESSION_OK,
        "demo terminate");
  check(wait_for_state(session, "dev.brickdesk.demo", BD_SESSION_EXITED, 5000),
        "demo exited");
  check(bd_session_find(session, "dev.brickdesk.demo", &info), "demo info end");
  check(info.exit_code == 0, "demo exit code %d", info.exit_code);

  char state_path[TEST_PATH];
  snprintf(state_path, sizeof(state_path), "%s/state-demo/dev.brickdesk.demo/state.txt",
           g_root);
  char *state = read_file(state_path);
  check(state != NULL && strstr(state, "slot=0") != NULL,
        "demo state file missing or wrong");
  free(state);
  bd_app_destroy(&app);
  bd_session_destroy(session);
}

static void test_crash(void) {
  event_log log;
  memset(&log, 0, sizeof(log));
  bd_session *session = create_session("crash", 2, &log,
                                       (bd_session_timeouts){0},
                                       (bd_session_perf_ops){0});
  check(session != NULL, "crash session");
  if (session == NULL) {
    return;
  }
  bd_app app;
  check(make_app("crash", "dev.crash", "Crash", g_child_path, "crash", "", "",
                 NULL, NULL, &app),
        "make crash app");
  check(bd_session_launch(session, &app, NULL, 0) == BD_SESSION_OK,
        "launch crash");
  check(wait_for_state(session, "dev.crash", BD_SESSION_CRASHED, 3000),
        "crash not detected");
  bd_session_app_info info;
  check(bd_session_find(session, "dev.crash", &info), "crash info");
  check(info.exit_code == 3, "crash exit code %d", info.exit_code);
  check(event_count(&log, BD_SESSION_EVENT_CRASH, "dev.crash") == 1,
        "crash event");
  bd_app_destroy(&app);
  bd_session_destroy(session);
}

static void test_background_limit(void) {
  event_log log;
  memset(&log, 0, sizeof(log));
  bd_session *session = create_session("limit", 2, &log,
                                       (bd_session_timeouts){0},
                                       (bd_session_perf_ops){0});
  check(session != NULL, "limit session");
  if (session == NULL) {
    return;
  }
  bd_app apps[3];
  memset(apps, 0, sizeof(apps));
  const char *ids[] = {"dev.a", "dev.b", "dev.c"};
  for (int i = 0; i < 3; ++i) {
    char dir[16];
    snprintf(dir, sizeof(dir), "limit%d", i);
    check(make_app(dir, ids[i], ids[i], g_child_path, "idle", "", "", NULL,
                   NULL, &apps[i]),
          "make limit app %d", i);
  }
  check(bd_session_launch(session, &apps[0], NULL, 0) == BD_SESSION_OK,
        "launch a");
  check(wait_for_state(session, ids[0], BD_SESSION_FOREGROUND, 2000),
        "a foreground");
  check(bd_session_background(session, ids[0]) == BD_SESSION_OK, "bg a");
  check(wait_for_state(session, ids[0], BD_SESSION_BACKGROUND, 2000),
        "a background");
  sleep_ms(30);
  check(bd_session_launch(session, &apps[1], NULL, 0) == BD_SESSION_OK,
        "launch b");
  check(wait_for_state(session, ids[1], BD_SESSION_FOREGROUND, 2000),
        "b foreground");
  check(bd_session_background(session, ids[1]) == BD_SESSION_OK, "bg b");
  check(wait_for_state(session, ids[1], BD_SESSION_BACKGROUND, 2000),
        "b background");
  check(bd_session_launch(session, &apps[2], NULL, 0) == BD_SESSION_OK,
        "launch c");
  check(wait_for_state(session, ids[2], BD_SESSION_FOREGROUND, 2000),
        "c foreground");
  check(bd_session_background(session, ids[2]) == BD_SESSION_NEEDS_CONFIRM,
        "background limit did not ask for confirmation");
  check(wait_for_state(session, ids[2], BD_SESSION_FOREGROUND, 100),
        "c should remain foreground");
  bd_session_eviction eviction;
  check(bd_session_suggest_eviction(session, &eviction), "eviction");
  check(strcmp(eviction.app_id, ids[0]) == 0, "eviction oldest %s",
        eviction.app_id);
  check(event_count(&log, BD_SESSION_EVENT_NEEDS_ATTENTION, ids[2]) == 1,
        "attention event");
  bd_session_app_info info;
  check(bd_session_find(session, ids[0], &info) &&
            info.state == BD_SESSION_BACKGROUND,
        "limit killed or changed the oldest app");
  for (int i = 0; i < 3; ++i) {
    bd_app_destroy(&apps[i]);
  }
  bd_session_destroy(session);
}

static void test_terminate_escalation(void) {
  event_log log;
  memset(&log, 0, sizeof(log));
  bd_session_timeouts timeouts;
  memset(&timeouts, 0, sizeof(timeouts));
  timeouts.hello_ms = 1000;
  timeouts.save_ms = 100;
  timeouts.quit_ms = 100;
  timeouts.term_ms = 120;
  timeouts.kill_ms = 300;
  bd_session *session = create_session("term", 2, &log, timeouts,
                                       (bd_session_perf_ops){0});
  check(session != NULL, "terminate session");
  if (session == NULL) {
    return;
  }
  bd_app app;
  check(make_app(
            "term", "dev.term", "Term", g_child_path, "ignore_quit",
            "\"save\":{\"type\":\"ipc\",\"method\":\"state.save\","
            "\"timeout_ms\":100},"
            "\"quit\":{\"type\":\"ipc\",\"method\":\"app.quit\","
            "\"timeout_ms\":100}",
            "", NULL, NULL, &app),
        "make terminate app");
  check(bd_session_launch(session, &app, NULL, 0) == BD_SESSION_OK,
        "launch terminate");
  check(wait_for_state(session, "dev.term", BD_SESSION_FOREGROUND, 2000),
        "terminate foreground");
  bd_session_app_info before;
  check(bd_session_find(session, "dev.term", &before), "terminate info bg");
  check(bd_session_background(session, "dev.term") == BD_SESSION_OK,
        "terminate background before stopping");
  check(wait_for_state(session, "dev.term", BD_SESSION_BACKGROUND, 2000),
        "terminate background state");
  check(wait_proc_state(before.pid, 'T', 1000),
        "terminate helper did not stop");
  uint64_t start = 0;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  start = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
  check(bd_session_terminate(session, "dev.term") == BD_SESSION_OK,
        "terminate request");
  check(wait_for_state(session, "dev.term", BD_SESSION_EXITED, 3000),
        "terminate did not finish");
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t elapsed = (uint64_t)now.tv_sec * 1000u +
                     (uint64_t)now.tv_nsec / 1000000u - start;
  bd_session_app_info info;
  check(bd_session_find(session, "dev.term", &info), "terminate info");
  check(info.term_signal == SIGKILL, "escalation signal %d", info.term_signal);
  check(elapsed >= 180, "escalation too fast: %llu ms",
        (unsigned long long)elapsed);
  bool save_ok = false;
  check(find_hook_event(&log, "dev.term", "save", &save_ok) && save_ok,
        "save hook before quit");
  bool quit_ok = true;
  check(find_hook_event(&log, "dev.term", "quit", &quit_ok) && !quit_ok,
        "quit hook timeout");

  char log_path[TEST_PATH];
  snprintf(log_path, sizeof(log_path), "%s/apps/term/child.log", g_root);
  char *child_log = read_file(log_path);
  check(child_log != NULL, "child log missing");
  if (child_log != NULL) {
    char *save = strstr(child_log, "method state.save");
    char *quit = strstr(child_log, "method app.quit");
    check(save != NULL && quit != NULL && save < quit,
          "hook order was save=%p quit=%p", (void *)save, (void *)quit);
    check(strstr(child_log, "quit ignored") != NULL, "quit was not ignored");
    free(child_log);
  }
  bd_app_destroy(&app);
  bd_session_destroy(session);
}

typedef struct fake_perf {
  int apply_count;
  int restore_count;
  char last_profile[32];
} fake_perf;

static bool fake_apply(void *user, const char *profile, const char *core) {
  fake_perf *perf = (fake_perf *)user;
  ++perf->apply_count;
  snprintf(perf->last_profile, sizeof(perf->last_profile), "%s", profile);
  (void)core;
  return true;
}

static bool fake_restore(void *user) {
  fake_perf *perf = (fake_perf *)user;
  ++perf->restore_count;
  return true;
}

static void test_perf_calls(void) {
  event_log log;
  memset(&log, 0, sizeof(log));
  fake_perf perf;
  memset(&perf, 0, sizeof(perf));
  bd_session_perf_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.user = &perf;
  ops.apply = fake_apply;
  ops.restore = fake_restore;
  bd_session *session = create_session("perf", 2, &log,
                                       (bd_session_timeouts){0}, ops);
  check(session != NULL, "perf session");
  if (session == NULL) {
    return;
  }
  bd_app app;
  check(make_app("perf", "dev.perf", "Perf", g_child_path, "idle", "", "",
                 NULL, NULL, &app),
        "make perf app");
  check(bd_session_launch(session, &app, NULL, 0) == BD_SESSION_OK,
        "launch perf");
  check(wait_for_state(session, "dev.perf", BD_SESSION_FOREGROUND, 2000),
        "perf foreground");
  check(perf.apply_count == 1 && strcmp(perf.last_profile, "performance") == 0,
        "apply count %d profile %s", perf.apply_count, perf.last_profile);
  check(bd_session_background(session, "dev.perf") == BD_SESSION_OK, "perf bg");
  check(wait_for_state(session, "dev.perf", BD_SESSION_BACKGROUND, 2000),
        "perf background");
  check(perf.restore_count == 1, "restore after background %d",
        perf.restore_count);
  check(bd_session_resume(session, "dev.perf") == BD_SESSION_OK, "perf resume");
  check(wait_for_state(session, "dev.perf", BD_SESSION_FOREGROUND, 2000),
        "perf foreground again");
  check(perf.apply_count == 2, "apply after resume %d", perf.apply_count);
  check(bd_session_terminate(session, "dev.perf") == BD_SESSION_OK,
        "perf terminate");
  check(wait_for_state(session, "dev.perf", BD_SESSION_EXITED, 3000),
        "perf exited");
  check(perf.restore_count == 2, "restore after exit %d", perf.restore_count);
  bd_app_destroy(&app);
  bd_session_destroy(session);
}

static void test_session_freeze(void) {
  event_log log;
  memset(&log, 0, sizeof(log));
  fake_perf perf;
  memset(&perf, 0, sizeof(perf));
  bd_session_perf_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.user = &perf;
  ops.apply = fake_apply;
  ops.restore = fake_restore;
  bd_session *session = create_session("freeze", 1, &log,
                                       (bd_session_timeouts){0}, ops);
  check(session != NULL, "freeze session");
  if (session == NULL) {
    return;
  }

  bd_app background_app;
  bd_app frozen_app;
  check(make_app("freeze-bg", "dev.freeze.bg", "FreezeBg", g_child_path,
                 "idle", "", "", NULL, NULL, &background_app),
        "make freeze background app");
  check(make_app("freeze-fg", "dev.freeze.fg", "FreezeFg", g_child_path,
                 "tick", "", "", NULL, NULL, &frozen_app),
        "make freeze foreground app");

  check(bd_session_launch(session, &background_app, NULL, 0) == BD_SESSION_OK,
        "freeze launch background app");
  check(wait_for_state(session, "dev.freeze.bg", BD_SESSION_FOREGROUND, 2000),
        "freeze background app foreground");
  check(bd_session_background(session, "dev.freeze.bg") == BD_SESSION_OK,
        "freeze background app request");
  check(wait_for_state(session, "dev.freeze.bg", BD_SESSION_BACKGROUND, 2000),
        "freeze background app state");
  check(bd_session_background_count(session) == 1 &&
            bd_session_max_background(session) == 1,
        "freeze quota was not full (%d/%d)",
        bd_session_background_count(session),
        bd_session_max_background(session));
  bd_session_app_info background_info;
  check(bd_session_find(session, "dev.freeze.bg", &background_info),
        "freeze background info");
  int background_pid = background_info.pid;
  check(background_pid > 0 && proc_state(background_pid) == 'T',
        "background app was not stopped");

  check(bd_session_launch(session, &frozen_app, NULL, 0) == BD_SESSION_OK,
        "freeze launch foreground app");
  check(wait_for_state(session, "dev.freeze.fg", BD_SESSION_FOREGROUND, 2000),
        "freeze foreground app state");
  bd_session_app_info info;
  check(bd_session_find(session, "dev.freeze.fg", &info), "freeze info");
  int pid = info.pid;
  check(pid > 0, "freeze pid %d", pid);
  check(info.state == BD_SESSION_FOREGROUND && !info.frozen &&
            !info.background && !info.stopped_by_session,
        "freeze initial info state=%s frozen=%d background=%d stopped=%d",
        bd_session_state_name(info.state), info.frozen, info.background,
        info.stopped_by_session);

  char tick_path[TEST_PATH];
  snprintf(tick_path, sizeof(tick_path), "%s/apps/freeze-fg/child.log", g_root);
  int waited = 0;
  while (count_occurrences(tick_path, "tick") == 0 && waited < 2000) {
    bd_session_poll(session);
    sleep_ms(5);
    waited += 5;
  }
  size_t ticks_before = count_occurrences(tick_path, "tick");
  check(ticks_before > 0, "tick child never ran");
  int apply_before = perf.apply_count;
  int restore_before = perf.restore_count;
  size_t events_before = log.count;

  bd_session_result result = bd_session_freeze(session, "dev.freeze.fg");
  check(result == BD_SESSION_OK, "freeze returned %s",
        bd_session_result_name(result));
  check(bd_session_find(session, "dev.freeze.fg", &info), "frozen info");
  check(strcmp(info.app_id, "dev.freeze.fg") == 0 &&
            info.state == BD_SESSION_FOREGROUND && info.frozen &&
            !info.background && !info.stopped_by_session,
        "freeze identity/state app=%s state=%s frozen=%d background=%d "
        "stopped=%d",
        info.app_id, bd_session_state_name(info.state), info.frozen,
        info.background, info.stopped_by_session);
  check(proc_state(pid) == 'T', "frozen child state %c", proc_state(pid));
  check(bd_session_background_count(session) == 1 &&
            bd_session_max_background(session) == 1,
        "freeze changed quota %d/%d", bd_session_background_count(session),
        bd_session_max_background(session));
  check(bd_session_running_count(session) == 2,
        "freeze running count %zu", bd_session_running_count(session));
  check(perf.apply_count == apply_before &&
            perf.restore_count == restore_before,
        "freeze changed perf leases apply=%d restore=%d", perf.apply_count,
        perf.restore_count);

  size_t ticks_at_stop = count_occurrences(tick_path, "tick");
  for (int i = 0; i < 20; ++i) {
    int next = bd_session_poll(session);
    check(next >= 0, "poll while frozen returned %d", next);
    sleep_ms(5);
  }
  check(count_occurrences(tick_path, "tick") == ticks_at_stop,
        "child executed while frozen (%zu -> %zu)", ticks_at_stop,
        count_occurrences(tick_path, "tick"));
  check(bd_session_find(session, "dev.freeze.fg", &info) && info.frozen &&
            info.state == BD_SESSION_FOREGROUND,
        "poll changed frozen state");
  check(log.count == events_before,
        "freeze/poll emitted %zu event(s)", log.count - events_before);

  check(bd_session_freeze(session, "dev.freeze.fg") == BD_SESSION_OK,
        "duplicate freeze");
  sleep_ms(30);
  check(count_occurrences(tick_path, "tick") == ticks_at_stop,
        "duplicate freeze resumed execution");
  check(proc_state(pid) == 'T', "duplicate freeze child state %c",
        proc_state(pid));

  check(bd_session_thaw(session, "dev.freeze.fg") == BD_SESSION_OK, "thaw");
  check(bd_session_find(session, "dev.freeze.fg", &info), "thaw info");
  check(info.state == BD_SESSION_FOREGROUND && !info.frozen &&
            !info.background,
        "thaw state=%s frozen=%d background=%d",
        bd_session_state_name(info.state), info.frozen, info.background);
  check(wait_proc_not_stopped(pid, 1000), "child stayed stopped after thaw");
  size_t ticks_after_thaw = ticks_at_stop;
  waited = 0;
  while (waited < 2000) {
    bd_session_poll(session);
    ticks_after_thaw = count_occurrences(tick_path, "tick");
    if (ticks_after_thaw > ticks_at_stop) {
      break;
    }
    sleep_ms(5);
    waited += 5;
  }
  check(ticks_after_thaw > ticks_at_stop,
        "child did not resume ticking (%zu -> %zu)", ticks_at_stop,
        ticks_after_thaw);
  check(bd_session_thaw(session, "dev.freeze.fg") == BD_SESSION_OK,
        "duplicate thaw");
  check(bd_session_find(session, "dev.freeze.fg", &info) && !info.frozen &&
            info.state == BD_SESSION_FOREGROUND,
        "duplicate thaw changed state");
  check(bd_session_background_count(session) == 1 &&
            bd_session_running_count(session) == 2,
        "thaw changed quota or foreground");
  check(proc_state(background_pid) == 'T',
        "thaw resumed the background app");
  check(perf.apply_count == apply_before &&
            perf.restore_count == restore_before,
        "thaw changed perf leases apply=%d restore=%d", perf.apply_count,
        perf.restore_count);
  check(log.count == events_before,
        "freeze/thaw emitted %zu event(s)", log.count - events_before);

  check(bd_session_freeze(session, "dev.freeze.fg") == BD_SESSION_OK,
        "freeze before terminate");
  check(proc_state(pid) == 'T', "terminate target not frozen");
  check(bd_session_terminate(session, "dev.freeze.fg") == BD_SESSION_OK,
        "terminate frozen foreground");
  check(wait_for_state(session, "dev.freeze.fg", BD_SESSION_EXITED, 3000),
        "frozen app did not exit on terminate");
  check(bd_session_find(session, "dev.freeze.fg", &info), "terminated info");
  check(info.exited && !info.frozen && !info.background &&
            info.term_signal == SIGTERM,
        "terminated frozen info exited=%d frozen=%d signal=%d", info.exited,
        info.frozen, info.term_signal);
  check(perf.restore_count == restore_before + 1,
        "terminate restore count %d", perf.restore_count);
  check(bd_session_background_count(session) == 1,
        "foreground exit changed background quota");

  check(bd_session_freeze(session, "dev.freeze.bg") ==
            BD_SESSION_INVALID_STATE,
        "freeze background app accepted");
  check(bd_session_thaw(session, "dev.freeze.bg") == BD_SESSION_INVALID_STATE,
        "thaw background app accepted");
  check(bd_session_freeze(session, "dev.freeze.fg") ==
            BD_SESSION_INVALID_STATE,
        "freeze exited app accepted");
  check(bd_session_thaw(session, "dev.freeze.fg") == BD_SESSION_INVALID_STATE,
        "thaw exited app accepted");
  check(bd_session_freeze(session, "dev.missing") == BD_SESSION_NOT_FOUND,
        "freeze missing app");
  check(bd_session_thaw(session, "dev.missing") == BD_SESSION_NOT_FOUND,
        "thaw missing app");

  bd_app_destroy(&background_app);
  bd_app_destroy(&frozen_app);
  bd_session_destroy(session);
}

static void test_session_freeze_busy(void) {
  event_log log;
  memset(&log, 0, sizeof(log));
  bd_session *session = create_session("freeze-busy", 2, &log,
                                       (bd_session_timeouts){0},
                                       (bd_session_perf_ops){0});
  check(session != NULL, "freeze busy session");
  if (session == NULL) {
    return;
  }
  bd_app app;
  check(make_app("freeze-busy", "dev.freeze.busy", "FreezeBusy", g_child_path,
                 "ipc",
                 "\"save\":{\"type\":\"ipc\",\"method\":\"state.save\","
                 "\"timeout_ms\":500}",
                 "\"save_state\":true", NULL, NULL, &app),
        "make freeze busy app");
  check(bd_session_launch(session, &app, NULL, 0) == BD_SESSION_OK,
        "freeze busy launch");
  check(wait_for_state(session, "dev.freeze.busy", BD_SESSION_FOREGROUND,
                       3000),
        "freeze busy foreground");
  bd_session_app_info info;
  check(bd_session_find(session, "dev.freeze.busy", &info) &&
            info.ipc_connected,
        "freeze busy IPC");

  check(bd_session_save_state(session, "dev.freeze.busy", 0) == BD_SESSION_OK,
        "freeze busy save request");
  check(bd_session_freeze(session, "dev.freeze.busy") == BD_SESSION_BUSY,
        "freeze during pending hook was not busy");
  check(bd_session_find(session, "dev.freeze.busy", &info) && !info.frozen &&
            info.state == BD_SESSION_FOREGROUND,
        "busy freeze changed state");

  bool save_ok = false;
  int waited = 0;
  while (!find_hook_event(&log, "dev.freeze.busy", "save", NULL) &&
         waited < 3000) {
    bd_session_poll(session);
    sleep_ms(5);
    waited += 5;
  }
  check(find_hook_event(&log, "dev.freeze.busy", "save", &save_ok) && save_ok,
        "freeze busy save hook");

  check(bd_session_freeze(session, "dev.freeze.busy") == BD_SESSION_OK,
        "freeze after pending hook");
  check(bd_session_find(session, "dev.freeze.busy", &info) && info.frozen,
        "freeze after pending hook not applied");
  check(proc_state(info.pid) == 'T', "freeze busy child state %c",
        proc_state(info.pid));
  check(bd_session_thaw(session, "dev.freeze.busy") == BD_SESSION_OK,
        "freeze busy thaw");
  check(bd_session_freeze(session, "dev.freeze.busy") == BD_SESSION_OK,
        "freeze before background");
  check(bd_session_background(session, "dev.freeze.busy") == BD_SESSION_OK,
        "background while frozen");
  check(wait_for_state(session, "dev.freeze.busy", BD_SESSION_BACKGROUND,
                       3000),
        "background while frozen state");
  check(bd_session_find(session, "dev.freeze.busy", &info) && !info.frozen &&
            info.background,
        "background while frozen did not clear freeze");
  check(wait_proc_state(info.pid, 'T', 1000),
        "background while frozen did not stop the child");
  check(bd_session_resume(session, "dev.freeze.busy") == BD_SESSION_OK,
        "resume after frozen background");
  check(wait_for_state(session, "dev.freeze.busy", BD_SESSION_FOREGROUND,
                       3000),
        "foreground after frozen background");
  check(bd_session_find(session, "dev.freeze.busy", &info) && !info.frozen &&
            info.state == BD_SESSION_FOREGROUND,
        "resume after frozen background state");
  check(bd_session_terminate(session, "dev.freeze.busy") == BD_SESSION_OK,
        "freeze busy terminate");
  check(bd_session_freeze(session, "dev.freeze.busy") == BD_SESSION_BUSY,
        "freeze during termination was not busy");
  check(wait_for_state(session, "dev.freeze.busy", BD_SESSION_EXITED, 3000),
        "freeze busy did not exit");
  check(bd_session_find(session, "dev.freeze.busy", &info) && info.exited &&
            !info.frozen,
        "freeze busy final info");

  bd_app_destroy(&app);
  bd_session_destroy(session);
}

static void test_session_freeze_external(void) {
  event_log log;
  memset(&log, 0, sizeof(log));
  bd_session *session = create_session("freeze-ext", 2, &log,
                                       (bd_session_timeouts){0},
                                       (bd_session_perf_ops){0});
  check(session != NULL, "freeze external session");
  if (session == NULL) {
    return;
  }
  bd_app app;
  check(make_app("freeze-ext", "dev.freeze.ext", "FreezeExt", g_child_path,
                 "idle", "", "", NULL, NULL, &app),
        "make freeze external app");
  check(bd_session_launch(session, &app, NULL, 0) == BD_SESSION_OK,
        "freeze external launch");
  check(wait_for_state(session, "dev.freeze.ext", BD_SESSION_FOREGROUND, 2000),
        "freeze external foreground");
  bd_session_app_info info;
  check(bd_session_find(session, "dev.freeze.ext", &info), "freeze external info");
  int pid = info.pid;
  check(kill(pid, SIGSTOP) == 0, "external SIGSTOP failed: %s",
        strerror(errno));
  int waited = 0;
  while (waited < 2000) {
    bd_session_poll(session);
    bd_session_find(session, "dev.freeze.ext", &info);
    if (info.stopped_by_session) {
      break;
    }
    sleep_ms(5);
    waited += 5;
  }
  check(info.stopped_by_session, "session did not observe the external stop");
  check(proc_state(pid) == 'T', "external stop state %c", proc_state(pid));
  check(bd_session_thaw(session, "dev.freeze.ext") == BD_SESSION_INVALID_STATE,
        "thaw accepted an externally stopped app");
  check(proc_state(pid) == 'T', "thaw resumed an externally stopped app");
  check(bd_session_freeze(session, "dev.freeze.ext") ==
            BD_SESSION_INVALID_STATE,
        "freeze claimed an externally stopped app");
  check(bd_session_terminate(session, "dev.freeze.ext") == BD_SESSION_OK,
        "terminate external stop");
  check(wait_for_state(session, "dev.freeze.ext", BD_SESSION_EXITED, 3000),
        "external stop did not terminate");

  bd_app exit_app;
  check(make_app("freeze-exit", "dev.freeze.exit", "FreezeExit", g_child_path,
                 "exit0", "", "", NULL, NULL, &exit_app),
        "make freeze exit app");
  check(bd_session_launch(session, &exit_app, NULL, 0) == BD_SESSION_OK,
        "freeze exit launch");
  sleep_ms(100);
  check(bd_session_freeze(session, "dev.freeze.exit") ==
            BD_SESSION_INVALID_STATE,
        "freeze accepted an already-exited child");
  check(bd_session_find(session, "dev.freeze.exit", &info), "freeze exit info");
  check(info.exited && !info.frozen && info.exit_code == 0,
        "freeze exit info exited=%d frozen=%d code=%d", info.exited,
        info.frozen, info.exit_code);
  check(bd_session_running_count(session) == 0,
        "exited child is still running");

  bd_app_destroy(&app);
  bd_app_destroy(&exit_app);
  bd_session_destroy(session);
}

static void test_session_freeze_destroy(void) {
  event_log log;
  memset(&log, 0, sizeof(log));
  bd_session *session = create_session("freeze-destroy", 1, &log,
                                       (bd_session_timeouts){0},
                                       (bd_session_perf_ops){0});
  check(session != NULL, "freeze destroy session");
  if (session == NULL) {
    return;
  }
  bd_app app;
  check(make_app("freeze-destroy", "dev.freeze.destroy", "FreezeDestroy",
                 g_child_path, "idle", "", "", NULL, NULL, &app),
        "make freeze destroy app");
  check(bd_session_launch(session, &app, NULL, 0) == BD_SESSION_OK,
        "freeze destroy launch");
  check(wait_for_state(session, "dev.freeze.destroy", BD_SESSION_FOREGROUND,
                       2000),
        "freeze destroy foreground");
  bd_session_app_info info;
  check(bd_session_find(session, "dev.freeze.destroy", &info),
        "freeze destroy info");
  int pid = info.pid;
  check(bd_session_freeze(session, "dev.freeze.destroy") == BD_SESSION_OK,
        "freeze before destroy");
  check(proc_state(pid) == 'T', "destroy target not frozen");
  bd_session_destroy(session);
  errno = 0;
  int alive = kill(pid, 0);
  check(alive != 0 && errno == ESRCH,
        "destroy left frozen pid %d alive=%d errno=%d", pid, alive, errno);
  bd_app_destroy(&app);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <demo> <child>\n", argv[0]);
    return 2;
  }
  if (realpath(argv[1], g_demo_path) == NULL ||
      realpath(argv[2], g_child_path) == NULL) {
    fprintf(stderr, "realpath failed: %s\n", strerror(errno));
    return 2;
  }
  char template[] = "/tmp/bd_session_test.XXXXXX";
  if (mkdtemp(template) == NULL) {
    fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
    return 2;
  }
  snprintf(g_root, sizeof(g_root), "%s", template);
  char apps[TEST_PATH];
  snprintf(apps, sizeof(apps), "%s/apps", g_root);
  make_dir(apps);

  test_manifest_parse();
  test_scan_and_assets();
  test_last_frame();
  test_session_sigstop();
  test_session_demo();
  test_crash();
  test_background_limit();
  test_terminate_escalation();
  test_perf_calls();
  test_session_freeze();
  test_session_freeze_busy();
  test_session_freeze_external();
  test_session_freeze_destroy();

  if (g_failures != 0) {
    fprintf(stderr, "session_test: %d failure(s), artifacts in %s\n", g_failures,
            g_root);
    return 1;
  }
  printf("session_test: all checks passed\n");
  return 0;
}
