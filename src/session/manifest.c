#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "brickdesk/app.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json_util.h"

#ifndef BRICKDESK_APPS_DIR
#define BRICKDESK_APPS_DIR "apps"
#endif

typedef struct bd_app_list {
  bd_app *items;
  size_t count;
  size_t capacity;
} bd_app_list;

static const char *const k_span_names[BD_APP_SPAN_COUNT] = {
    "1x1", "2x1", "1x2", "2x2", "3x2"};

static const char *const k_hook_names[BD_APP_HOOK_COUNT] = {
    "pause", "resume", "save", "load", "quit", "screenshot"};

static const char *const k_hook_methods[BD_APP_HOOK_COUNT] = {
    "app.pause", "app.resume", "state.save",
    "state.load", "app.quit", "app.screenshot"};

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
}

static void manifest_error(const char *path, const bd_json *doc, int token,
                           char *error, size_t error_size,
                           const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  char message[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  int line = doc != NULL ? bd_json_line(doc, token) : 0;
  if (line > 0) {
    snprintf(error, error_size, "%s:%d: %s", path != NULL ? path : "<memory>",
             line, message);
  } else {
    snprintf(error, error_size, "%s: %s", path != NULL ? path : "<memory>",
             message);
  }
}

static char *duplicate_string(const char *text) {
  if (text == NULL) {
    return NULL;
  }
  size_t length = strlen(text);
  char *copy = (char *)malloc(length + 1);
  if (copy != NULL) {
    memcpy(copy, text, length + 1);
  }
  return copy;
}

static bool copy_string_token(const bd_json *doc, int token, char *out,
                              size_t out_size, bool *truncated) {
  if (out == NULL || out_size == 0) {
    return false;
  }
  out[0] = '\0';
  return bd_json_string(doc, token, out, out_size, truncated);
}

static bool valid_id(const char *id) {
  size_t length = strlen(id);
  if (length == 0 || length >= BD_APP_ID_MAX) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    char c = id[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

static bool valid_relative_path(const char *path) {
  if (path == NULL || path[0] == '\0' || path[0] == '/') {
    return false;
  }
  const char *part = path;
  while (*part != '\0') {
    const char *slash = strchr(part, '/');
    size_t length = slash != NULL ? (size_t)(slash - part) : strlen(part);
    if (length == 0 || (length == 1 && part[0] == '.') ||
        (length == 2 && part[0] == '.' && part[1] == '.')) {
      return false;
    }
    if (slash == NULL) {
      break;
    }
    part = slash + 1;
  }
  return true;
}

static bool join_path(const char *dir, const char *rel, char *out,
                      size_t out_size) {
  int written;
  if (rel[0] == '/') {
    written = snprintf(out, out_size, "%s", rel);
  } else {
    written = snprintf(out, out_size, "%s/%s", dir, rel);
  }
  return written > 0 && (size_t)written < out_size;
}

static bool path_is_regular(const char *path) {
  struct stat status;
  return lstat(path, &status) == 0 && S_ISREG(status.st_mode);
}

static bool normalize_color(const char *input, char *out, size_t out_size) {
  if (input == NULL || strlen(input) != 7 || input[0] != '#') {
    return false;
  }
  for (int i = 1; i < 7; ++i) {
    char c = input[i];
    bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    if (!hex) {
      return false;
    }
  }
  if (out_size < 8) {
    return false;
  }
  out[0] = '#';
  for (int i = 1; i < 7; ++i) {
    char c = input[i];
    if (c >= 'a' && c <= 'f') {
      c = (char)(c - 'a' + 'A');
    }
    out[i] = c;
  }
  out[7] = '\0';
  return true;
}

static bool parse_type(const char *value, bd_app_type *out) {
  if (strcmp(value, "app") == 0) {
    *out = BD_APP_TYPE_APP;
    return true;
  }
  if (strcmp(value, "game") == 0) {
    *out = BD_APP_TYPE_GAME;
    return true;
  }
  return false;
}

static bool parse_category(const char *value, bd_app_category *out) {
  if (strcmp(value, "app") == 0) {
    *out = BD_APP_CATEGORY_APP;
  } else if (strcmp(value, "game") == 0) {
    *out = BD_APP_CATEGORY_GAME;
  } else if (strcmp(value, "emulator") == 0) {
    *out = BD_APP_CATEGORY_EMULATOR;
  } else if (strcmp(value, "tool") == 0) {
    *out = BD_APP_CATEGORY_TOOL;
  } else if (strcmp(value, "service") == 0) {
    *out = BD_APP_CATEGORY_SERVICE;
  } else {
    return false;
  }
  return true;
}

static bool parse_menu_mode(const char *value, bd_app_menu_mode *out) {
  if (strcmp(value, "overlay") == 0) {
    *out = BD_APP_MENU_OVERLAY;
  } else if (strcmp(value, "embedded") == 0) {
    *out = BD_APP_MENU_EMBEDDED;
  } else if (strcmp(value, "handoff") == 0) {
    *out = BD_APP_MENU_HANDOFF;
  } else if (strcmp(value, "none") == 0) {
    *out = BD_APP_MENU_NONE;
  } else {
    return false;
  }
  return true;
}

static bool parse_hook_type(const char *value, bd_app_hook_type *out) {
  if (strcmp(value, "ipc") == 0) {
    *out = BD_APP_HOOK_IPC;
    return true;
  }
  if (strcmp(value, "command") == 0) {
    *out = BD_APP_HOOK_COMMAND;
    return true;
  }
  return false;
}

static bool parse_live_provider(const char *value, bd_app_live_provider *out) {
  if (strcmp(value, "none") == 0) {
    *out = BD_APP_LIVE_NONE;
    return true;
  }
  if (strcmp(value, "builtin.now-playing") == 0) {
    *out = BD_APP_LIVE_NOW_PLAYING;
    return true;
  }
  if (strcmp(value, "file") == 0) {
    *out = BD_APP_LIVE_FILE;
    return true;
  }
  return false;
}

static bool string_field(const bd_json *doc, int object, const char *key,
                         char *out, size_t out_size, bool *truncated) {
  int token = bd_json_object_get(doc, object, key);
  if (token < 0) {
    out[0] = '\0';
    if (truncated != NULL) {
      *truncated = false;
    }
    return false;
  }
  return copy_string_token(doc, token, out, out_size, truncated);
}

static bool parse_exec_array(const bd_json *doc, int token, const char *path,
                             bd_app *app, char *error, size_t error_size) {
  int count = bd_json_array_size(doc, token);
  if (count <= 0) {
    manifest_error(path, doc, token, error, error_size,
                   "'exec' must be a non-empty array");
    return false;
  }
  if (count > BD_APP_MAX_EXEC) {
    manifest_error(path, doc, token, error, error_size,
                   "'exec' has more than %d entries", BD_APP_MAX_EXEC);
    return false;
  }
  app->exec = (char **)calloc((size_t)count, sizeof(char *));
  if (app->exec == NULL) {
    manifest_error(path, doc, token, error, error_size, "out of memory");
    return false;
  }
  app->exec_count = (size_t)count;
  for (int i = 0; i < count; ++i) {
    int value = bd_json_array_at(doc, token, i);
    char buffer[BD_APP_PATH_MAX];
    bool truncated = false;
    if (value < 0 ||
        !copy_string_token(doc, value, buffer, sizeof(buffer), &truncated)) {
      manifest_error(path, doc, value < 0 ? token : value, error, error_size,
                     "'exec[%d]' must be a string", i);
      return false;
    }
    if (buffer[0] == '\0') {
      manifest_error(path, doc, value, error, error_size,
                     "'exec[%d]' must not be empty", i);
      return false;
    }
    if (truncated) {
      manifest_error(path, doc, value, error, error_size,
                     "'exec[%d]' is too long", i);
      return false;
    }
    app->exec[i] = duplicate_string(buffer);
    if (app->exec[i] == NULL) {
      manifest_error(path, doc, value, error, error_size, "out of memory");
      return false;
    }
  }
  return true;
}

static bool parse_env_object(const bd_json *doc, int token, const char *path,
                             bd_app *app, char *error, size_t error_size) {
  if (!bd_json_is_object(doc, token)) {
    manifest_error(path, doc, token, error, error_size,
                   "'env' must be an object");
    return false;
  }
  /* jsmn object size is the number of key/value pairs. */
  int size = 0;
  {
    int end = bd_json_skip(doc, token);
    if (end < 0) {
      manifest_error(path, doc, token, error, error_size, "invalid 'env'");
      return false;
    }
    int child = token + 1;
    while (child < end) {
      ++size;
      child = bd_json_skip(doc, child);
      if (child < 0) {
        break;
      }
      child = bd_json_skip(doc, child);
      if (child < 0) {
        break;
      }
    }
  }
  if (size > BD_APP_MAX_ENV) {
    manifest_error(path, doc, token, error, error_size,
                   "'env' has more than %d entries", BD_APP_MAX_ENV);
    return false;
  }
  if (size == 0) {
    return true;
  }
  app->env_keys = (char **)calloc((size_t)size, sizeof(char *));
  app->env_values = (char **)calloc((size_t)size, sizeof(char *));
  if (app->env_keys == NULL || app->env_values == NULL) {
    manifest_error(path, doc, token, error, error_size, "out of memory");
    return false;
  }

  int child = token + 1;
  for (int i = 0; i < size; ++i) {
    int value = bd_json_skip(doc, child);
    if (value < 0) {
      manifest_error(path, doc, token, error, error_size, "invalid 'env'");
      return false;
    }
    char key[64];
    char content[BD_APP_PATH_MAX];
    bool key_truncated = false;
    bool value_truncated = false;
    if (!bd_json_is_string(doc, child) ||
        !copy_string_token(doc, child, key, sizeof(key), &key_truncated)) {
      manifest_error(path, doc, child, error, error_size,
                     "'env' keys must be strings");
      return false;
    }
    if (key[0] == '\0' || strchr(key, '=') != NULL) {
      manifest_error(path, doc, child, error, error_size,
                     "'env' key '%s' is invalid", key);
      return false;
    }
    if (key_truncated) {
      manifest_error(path, doc, child, error, error_size,
                     "'env' key is too long");
      return false;
    }
    if (!bd_json_is_string(doc, value) ||
        !copy_string_token(doc, value, content, sizeof(content),
                           &value_truncated) ||
        value_truncated) {
      manifest_error(path, doc, value < 0 ? child : value, error, error_size,
                     "'env.%s' must be a string shorter than %d bytes", key,
                     BD_APP_PATH_MAX);
      return false;
    }
    app->env_keys[i] = duplicate_string(key);
    app->env_values[i] = duplicate_string(content);
    if (app->env_keys[i] == NULL || app->env_values[i] == NULL) {
      manifest_error(path, doc, token, error, error_size, "out of memory");
      return false;
    }
    app->env_count = (size_t)i + 1;
    child = bd_json_skip(doc, value);
    if (child < 0) {
      manifest_error(path, doc, token, error, error_size, "invalid 'env'");
      return false;
    }
  }
  return true;
}

static bool parse_hook(const bd_json *doc, int object, bd_app_hook_kind kind,
                       const char *path, bd_app_hook *hook, char *error,
                       size_t error_size) {
  if (!bd_json_is_object(doc, object)) {
    manifest_error(path, doc, object, error, error_size,
                   "'hooks.%s' must be an object", k_hook_names[kind]);
    return false;
  }
  hook->type = BD_APP_HOOK_NONE;
  hook->timeout_ms = 0;

  char type[16] = {0};
  bool type_found = string_field(doc, object, "type", type, sizeof(type), NULL);
  int method_token = bd_json_object_get(doc, object, "method");
  int exec_token = bd_json_object_get(doc, object, "exec");

  if (type_found) {
    if (!parse_hook_type(type, &hook->type)) {
      manifest_error(path, doc, object, error, error_size,
                     "'hooks.%s.type' must be 'ipc' or 'command'",
                     k_hook_names[kind]);
      return false;
    }
  } else if (method_token >= 0) {
    hook->type = BD_APP_HOOK_IPC;
  } else if (exec_token >= 0) {
    hook->type = BD_APP_HOOK_COMMAND;
  } else {
    manifest_error(path, doc, object, error, error_size,
                   "'hooks.%s' needs a type, method or exec",
                   k_hook_names[kind]);
    return false;
  }

  if (hook->type == BD_APP_HOOK_IPC) {
    bool truncated = false;
    if (method_token >= 0) {
      if (!copy_string_token(doc, method_token, hook->method,
                             sizeof(hook->method), &truncated) ||
          truncated || hook->method[0] == '\0') {
        manifest_error(path, doc, method_token, error, error_size,
                       "'hooks.%s.method' is invalid", k_hook_names[kind]);
        return false;
      }
    } else {
      snprintf(hook->method, sizeof(hook->method), "%s",
               k_hook_methods[kind]);
    }
  } else {
    if (exec_token < 0) {
      manifest_error(path, doc, object, error, error_size,
                     "'hooks.%s.exec' is required for command hooks",
                     k_hook_names[kind]);
      return false;
    }
    /* Command argv is parsed with a temporary app so the generic exec
     * parser can be reused. */
    bd_app temp;
    memset(&temp, 0, sizeof(temp));
    if (!parse_exec_array(doc, exec_token, path, &temp, error, error_size)) {
      bd_app_destroy(&temp);
      return false;
    }
    hook->exec = temp.exec;
    hook->exec_count = temp.exec_count;
    temp.exec = NULL;
    temp.exec_count = 0;
    bd_app_destroy(&temp);
    if (hook->exec_count == 0) {
      manifest_error(path, doc, object, error, error_size,
                     "'hooks.%s.exec' is required for command hooks",
                     k_hook_names[kind]);
      return false;
    }
  }

  int timeout_token = bd_json_object_get(doc, object, "timeout_ms");
  if (timeout_token >= 0) {
    long timeout = 0;
    if (!bd_json_int(doc, timeout_token, &timeout) || timeout < 0 ||
        timeout > 600000) {
      manifest_error(path, doc, timeout_token, error, error_size,
                     "'hooks.%s.timeout_ms' must be 0..600000",
                     k_hook_names[kind]);
      return false;
    }
    hook->timeout_ms = (int)timeout;
  }
  return true;
}

static bool parse_hooks(const bd_json *doc, int hooks, const char *path,
                        bd_app *app, char *error, size_t error_size) {
  if (hooks < 0) {
    return true;
  }
  if (!bd_json_is_object(doc, hooks)) {
    manifest_error(path, doc, hooks, error, error_size,
                   "'hooks' must be an object");
    return false;
  }
  for (int kind = 0; kind < BD_APP_HOOK_COUNT; ++kind) {
    int object = bd_json_object_get(doc, hooks, k_hook_names[kind]);
    if (object < 0) {
      continue;
    }
    if (!parse_hook(doc, object, (bd_app_hook_kind)kind, path,
                    &app->hooks[kind], error, error_size)) {
      return false;
    }
  }
  return true;
}

static bool parse_capabilities(const bd_json *doc, int token, const char *path,
                               bd_app *app, char *error, size_t error_size) {
  if (!bd_json_is_object(doc, token)) {
    manifest_error(path, doc, token, error, error_size,
                   "'capabilities' must be an object");
    return false;
  }
  /* LED/rumble notifications predate capability declarations; keep them on
   * unless an app explicitly opts out. */
  app->caps.rumble = true;
  app->caps.led_ambient = true;
  struct {
    const char *name;
    bool *field;
  } fields[] = {
      {"pause", &app->caps.pause},
      {"resume", &app->caps.resume},
      {"save_state", &app->caps.save_state},
      {"load_state", &app->caps.load_state},
      {"screenshot", &app->caps.screenshot},
      {"background", &app->caps.background},
      {"display_release", &app->caps.display_release},
      {"rumble", &app->caps.rumble},
      {"led_ambient", &app->caps.led_ambient},
  };
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
    int value = bd_json_object_get(doc, token, fields[i].name);
    if (value < 0) {
      continue;
    }
    bool flag = false;
    if (!bd_json_bool(doc, value, &flag)) {
      manifest_error(path, doc, value, error, error_size,
                     "'capabilities.%s' must be a boolean", fields[i].name);
      return false;
    }
    *fields[i].field = flag;
  }
  return true;
}

static bool parse_menu(const bd_json *doc, int menu, const char *path,
                       bd_app *app, char *error, size_t error_size) {
  app->menu.mode = BD_APP_MENU_OVERLAY;
  if (menu < 0) {
    return true;
  }
  if (!bd_json_is_object(doc, menu)) {
    manifest_error(path, doc, menu, error, error_size,
                   "'menu' must be an object");
    return false;
  }
  char mode[16] = {0};
  if (string_field(doc, menu, "mode", mode, sizeof(mode), NULL)) {
    if (!parse_menu_mode(mode, &app->menu.mode)) {
      manifest_error(path, doc, menu, error, error_size,
                     "'menu.mode' must be overlay, embedded, handoff or none");
      return false;
    }
  }

  int overlay = bd_json_object_get(doc, menu, "overlay");
  if (overlay < 0) {
    overlay = menu;
  }
  int safe = bd_json_object_get(doc, overlay, "safe");
  if (safe >= 0) {
    bool flag = false;
    if (!bd_json_bool(doc, safe, &flag)) {
      manifest_error(path, doc, safe, error, error_size,
                     "'menu.overlay.safe' must be a boolean");
      return false;
    }
    app->menu.overlay_safe = flag;
  }
  int exclude = bd_json_object_get(doc, overlay, "exclude_keys");
  if (exclude >= 0) {
    int count = bd_json_array_size(doc, exclude);
    if (count < 0 || count > BD_APP_MAX_EXCLUDE_KEYS) {
      manifest_error(path, doc, exclude, error, error_size,
                     "'menu.overlay.exclude_keys' must have at most %d items",
                     BD_APP_MAX_EXCLUDE_KEYS);
      return false;
    }
    for (int i = 0; i < count; ++i) {
      int value = bd_json_array_at(doc, exclude, i);
      bool truncated = false;
      char key[32];
      if (value < 0 ||
          !copy_string_token(doc, value, key, sizeof(key), &truncated) ||
          truncated || key[0] == '\0') {
        manifest_error(path, doc, value < 0 ? exclude : value, error,
                       error_size,
                       "'menu.overlay.exclude_keys[%d]' must be a short string",
                       i);
        return false;
      }
      snprintf(app->menu.exclude_keys[app->menu.exclude_key_count],
               sizeof(app->menu.exclude_keys[0]), "%s", key);
      ++app->menu.exclude_key_count;
    }
  }
  return true;
}

static bool parse_perf(const bd_json *doc, int perf, const char *path,
                       bd_app *app, char *error, size_t error_size) {
  snprintf(app->perf.profile, sizeof(app->perf.profile), "%s", "balanced");
  if (perf < 0) {
    return true;
  }
  if (!bd_json_is_object(doc, perf)) {
    manifest_error(path, doc, perf, error, error_size,
                   "'perf' must be an object");
    return false;
  }
  char profile[32];
  if (string_field(doc, perf, "profile", profile, sizeof(profile), NULL)) {
    if (profile[0] == '\0') {
      manifest_error(path, doc, perf, error, error_size,
                     "'perf.profile' must not be empty");
      return false;
    }
    snprintf(app->perf.profile, sizeof(app->perf.profile), "%s", profile);
  }
  char core[64];
  if (string_field(doc, perf, "core", core, sizeof(core), NULL)) {
    snprintf(app->perf.core, sizeof(app->perf.core), "%s", core);
  }
  return true;
}

static bool parse_state(const bd_json *doc, int state, const char *path,
                        bd_app *app, char *error, size_t error_size) {
  app->state.max_slots = 1;
  if (state < 0) {
    return true;
  }
  if (!bd_json_is_object(doc, state)) {
    manifest_error(path, doc, state, error, error_size,
                   "'state' must be an object");
    return false;
  }
  char relative[BD_APP_PATH_MAX];
  bool truncated = false;
  if (string_field(doc, state, "path", relative, sizeof(relative),
                   &truncated)) {
    if (truncated || !valid_relative_path(relative)) {
      manifest_error(path, doc, state, error, error_size,
                     "'state.path' must be a relative path without '..'");
      return false;
    }
    snprintf(app->state.path, sizeof(app->state.path), "%s", relative);
  }
  char format[32];
  if (string_field(doc, state, "format", format, sizeof(format), NULL)) {
    snprintf(app->state.format, sizeof(app->state.format), "%s", format);
  }
  int slots = bd_json_object_get(doc, state, "max_slots");
  if (slots >= 0) {
    long value = 0;
    if (!bd_json_int(doc, slots, &value) || value < 0 || value > 32) {
      manifest_error(path, doc, slots, error, error_size,
                     "'state.max_slots' must be 0..32");
      return false;
    }
    app->state.max_slots = (int)value;
  }
  return true;
}

static bool parse_live(const bd_json *doc, int live, const char *path,
                       bd_app *app, char *error, size_t error_size) {
  app->live.refresh_ms = 1000;
  app->live.provider = BD_APP_LIVE_NONE;
  if (live < 0) {
    return true;
  }
  if (!bd_json_is_object(doc, live)) {
    manifest_error(path, doc, live, error, error_size,
                   "'live' must be an object");
    return false;
  }
  char provider[32];
  if (string_field(doc, live, "provider", provider, sizeof(provider), NULL)) {
    if (!parse_live_provider(provider, &app->live.provider)) {
      manifest_error(path, doc, live, error, error_size,
                     "'live.provider' is unknown");
      return false;
    }
  }
  char relative[BD_APP_PATH_MAX];
  bool truncated = false;
  if (string_field(doc, live, "state", relative, sizeof(relative),
                   &truncated)) {
    if (truncated || !valid_relative_path(relative)) {
      manifest_error(path, doc, live, error, error_size,
                     "'live.state' must be a relative path without '..'");
      return false;
    }
    snprintf(app->live.state_path, sizeof(app->live.state_path), "%s",
             relative);
    if (!string_field(doc, live, "provider", provider, sizeof(provider),
                      NULL)) {
      app->live.provider = BD_APP_LIVE_FILE;
    }
  }
  int refresh = bd_json_object_get(doc, live, "refreshMs");
  if (refresh >= 0) {
    long value = 0;
    if (!bd_json_int(doc, refresh, &value) || value < 500 || value > 5000) {
      manifest_error(path, doc, refresh, error, error_size,
                     "'live.refreshMs' must be 500..5000");
      return false;
    }
    app->live.refresh_ms = (int)value;
  }
  int fields = bd_json_object_get(doc, live, "fields");
  if (fields >= 0) {
    int count = bd_json_array_size(doc, fields);
    if (count < 0 || count > BD_APP_MAX_LIVE_FIELDS) {
      manifest_error(path, doc, fields, error, error_size,
                     "'live.fields' must have at most %d items",
                     BD_APP_MAX_LIVE_FIELDS);
      return false;
    }
    for (int i = 0; i < count; ++i) {
      int value = bd_json_array_at(doc, fields, i);
      char name[32];
      bool cut = false;
      if (value < 0 ||
          !copy_string_token(doc, value, name, sizeof(name), &cut) || cut) {
        manifest_error(path, doc, value < 0 ? fields : value, error,
                       error_size, "'live.fields[%d]' must be a string", i);
        return false;
      }
      if (strcmp(name, "title") == 0) {
        app->live.fields |= BD_APP_LIVE_FIELD_TITLE;
      } else if (strcmp(name, "subtitle") == 0) {
        app->live.fields |= BD_APP_LIVE_FIELD_SUBTITLE;
      } else if (strcmp(name, "progress") == 0) {
        app->live.fields |= BD_APP_LIVE_FIELD_PROGRESS;
      } else if (strcmp(name, "artwork") == 0) {
        app->live.fields |= BD_APP_LIVE_FIELD_ARTWORK;
      } else {
        manifest_error(path, doc, value, error, error_size,
                       "'live.fields[%d]' is not a known field", i);
        return false;
      }
    }
  }
  return true;
}

static bool assign_art_path(const bd_json *doc, int token, const char *path,
                            const char *field, char *out, size_t out_size,
                            char *error, size_t error_size) {
  char relative[BD_APP_PATH_MAX];
  bool truncated = false;
  if (!copy_string_token(doc, token, relative, sizeof(relative), &truncated) ||
      truncated || !valid_relative_path(relative)) {
    manifest_error(path, doc, token, error, error_size,
                   "'%s' must be a relative path without '..'", field);
    return false;
  }
  snprintf(out, out_size, "%s", relative);
  return true;
}

static bool parse_art(const bd_json *doc, int root, const char *path,
                      bd_app *app, char *error, size_t error_size) {
  int art = bd_json_object_get(doc, root, "art");
  if (art >= 0 && !bd_json_is_object(doc, art)) {
    manifest_error(path, doc, art, error, error_size,
                   "'art' must be an object");
    return false;
  }

  int icon = art >= 0 ? bd_json_object_get(doc, art, "icon") : -1;
  if (icon < 0) {
    icon = bd_json_object_get(doc, root, "icon");
  }
  if (icon >= 0 &&
      !assign_art_path(doc, icon, path, "art.icon", app->icon,
                       sizeof(app->icon), error, error_size)) {
    return false;
  }

  int covers = art >= 0 ? bd_json_object_get(doc, art, "covers") : -1;
  if (covers < 0) {
    covers = bd_json_object_get(doc, root, "covers");
  }
  for (int span = 0; span < BD_APP_SPAN_COUNT; ++span) {
    int image = covers >= 0
                    ? bd_json_object_get(doc, covers, k_span_names[span])
                    : -1;
    if (image < 0 && span == BD_APP_SPAN_1X1) {
      image = bd_json_object_get(doc, root, "cover");
    }
    if (image >= 0) {
      char field[32];
      snprintf(field, sizeof(field), "art.covers.%s", k_span_names[span]);
      if (!assign_art_path(doc, image, path, field,
                           app->covers[span],
                           sizeof(app->covers[span]), error, error_size)) {
        return false;
      }
    }
  }

  int backdrop = art >= 0 ? bd_json_object_get(doc, art, "backdrop") : -1;
  if (backdrop < 0) {
    backdrop = bd_json_object_get(doc, root, "backdrop");
  }
  if (backdrop >= 0 &&
      !assign_art_path(doc, backdrop, path, "art.backdrop", app->backdrop,
                       sizeof(app->backdrop), error, error_size)) {
    return false;
  }

  int fallback = art >= 0
                     ? bd_json_object_get(doc, art, "fallbackColor")
                     : -1;
  if (fallback < 0) {
    fallback = bd_json_object_get(doc, root, "fallbackColor");
  }
  if (fallback >= 0) {
    char color[16];
    bool truncated = false;
    if (!copy_string_token(doc, fallback, color, sizeof(color), &truncated) ||
        truncated || !normalize_color(color, app->fallback_color,
                                      sizeof(app->fallback_color))) {
      manifest_error(path, doc, fallback, error, error_size,
                     "'art.fallbackColor' must be #RRGGBB");
      return false;
    }
  }
  return true;
}

static char *parent_directory(const char *path) {
  char *copy = duplicate_string(path);
  if (copy == NULL) {
    return NULL;
  }
  char *slash = strrchr(copy, '/');
  if (slash == NULL) {
    snprintf(copy, strlen(copy) + 1, ".");
  } else if (slash == copy) {
    slash[1] = '\0';
  } else {
    *slash = '\0';
  }
  return copy;
}

static bool resolve_dir(const char *dir, char *out, size_t out_size,
                        char *error, size_t error_size) {
  char resolved[PATH_MAX];
  if (realpath(dir, resolved) == NULL) {
    set_error(error, error_size, "cannot resolve app directory '%s': %s", dir,
              strerror(errno));
    return false;
  }
  if (strlen(resolved) >= out_size) {
    set_error(error, error_size, "app directory path is too long");
    return false;
  }
  snprintf(out, out_size, "%s", resolved);
  return true;
}

bool bd_app_parse_json(const char *json, size_t length, const char *dir,
                       const char *manifest_path, bd_app *out, char *error,
                       size_t error_size) {
  if (out == NULL || json == NULL || dir == NULL) {
    set_error(error, error_size, "invalid manifest arguments");
    return false;
  }
  memset(out, 0, sizeof(*out));
  /* LED/rumble notifications predate capability declarations; keep them on
   * unless an app explicitly opts out. */
  out->caps.rumble = true;
  out->caps.led_ambient = true;
  const char *path = manifest_path != NULL ? manifest_path : "<memory>";

  if (!resolve_dir(dir, out->dir, sizeof(out->dir), error, error_size)) {
    return false;
  }
  snprintf(out->manifest_path, sizeof(out->manifest_path), "%s", path);
  snprintf(out->cwd, sizeof(out->cwd), "%s", out->dir);
  out->schema = 1;
  out->menu.mode = BD_APP_MENU_OVERLAY;
  out->state.max_slots = 1;
  out->live.refresh_ms = 1000;
  snprintf(out->perf.profile, sizeof(out->perf.profile), "%s", "balanced");

  bd_json doc;
  char json_error[128] = {0};
  if (!bd_json_parse(&doc, json, length, json_error, sizeof(json_error))) {
    set_error(error, error_size, "%s: %s", path, json_error);
    return false;
  }

  bool ok = false;
  int root = bd_json_root(&doc);
  if (!bd_json_is_object(&doc, root)) {
    manifest_error(path, &doc, root, error, error_size,
                   "manifest root must be an object");
    goto done;
  }

  int schema = bd_json_object_get(&doc, root, "schema");
  if (schema < 0) {
    manifest_error(path, &doc, root, error, error_size,
                   "'schema' is required");
    goto done;
  }
  long schema_value = 0;
  if (!bd_json_int(&doc, schema, &schema_value) || schema_value != 1) {
    manifest_error(path, &doc, schema, error, error_size,
                   "unsupported manifest schema (expected 1)");
    goto done;
  }
  out->schema = 1;

  int id = bd_json_object_get(&doc, root, "id");
  char id_buffer[BD_APP_ID_MAX];
  bool id_truncated = false;
  if (id < 0 || !copy_string_token(&doc, id, id_buffer, sizeof(id_buffer),
                                   &id_truncated) ||
      id_truncated || !valid_id(id_buffer)) {
    manifest_error(path, &doc, id < 0 ? root : id, error, error_size,
                   "'id' must match [a-z0-9._-]{1,64}");
    goto done;
  }
  snprintf(out->id, sizeof(out->id), "%s", id_buffer);

  int name = bd_json_object_get(&doc, root, "name");
  bool name_truncated = false;
  if (name < 0 ||
      !copy_string_token(&doc, name, out->name, sizeof(out->name),
                         &name_truncated) ||
      out->name[0] == '\0') {
    manifest_error(path, &doc, name < 0 ? root : name, error, error_size,
                   "'name' is required and must not be empty");
    goto done;
  }
  (void)name_truncated;

  int subtitle = bd_json_object_get(&doc, root, "subtitle");
  if (subtitle >= 0) {
    copy_string_token(&doc, subtitle, out->subtitle, sizeof(out->subtitle),
                      NULL);
  }
  int version = bd_json_object_get(&doc, root, "version");
  if (version >= 0) {
    copy_string_token(&doc, version, out->version, sizeof(out->version),
                      NULL);
  }

  int type = bd_json_object_get(&doc, root, "type");
  char type_name[16] = {0};
  bool has_type = false;
  if (type >= 0) {
    if (!copy_string_token(&doc, type, type_name, sizeof(type_name), NULL) ||
        !parse_type(type_name, &out->type)) {
      manifest_error(path, &doc, type, error, error_size,
                     "'type' must be 'app' or 'game'");
      goto done;
    }
    has_type = true;
  }

  int category = bd_json_object_get(&doc, root, "category");
  if (category >= 0) {
    char category_name[16] = {0};
    if (!copy_string_token(&doc, category, category_name,
                           sizeof(category_name), NULL) ||
        !parse_category(category_name, &out->category)) {
      manifest_error(path, &doc, category, error, error_size,
                     "'category' must be game, app, emulator, tool or service");
      goto done;
    }
  } else if (has_type) {
    out->category = out->type == BD_APP_TYPE_GAME ? BD_APP_CATEGORY_GAME
                                                  : BD_APP_CATEGORY_APP;
  } else {
    manifest_error(path, &doc, root, error, error_size,
                   "'type' or 'category' is required");
    goto done;
  }
  if (!has_type) {
    out->type = out->category == BD_APP_CATEGORY_GAME ? BD_APP_TYPE_GAME
                                                      : BD_APP_TYPE_APP;
  }

  int exec = bd_json_object_get(&doc, root, "exec");
  if (exec < 0 || !parse_exec_array(&doc, exec, path, out, error,
                                     error_size)) {
    if (exec < 0) {
      manifest_error(path, &doc, root, error, error_size,
                     "'exec' is required");
    }
    goto done;
  }

  char cwd[BD_APP_PATH_MAX];
  if (string_field(&doc, root, "cwd", cwd, sizeof(cwd), NULL) &&
      cwd[0] != '\0') {
    if (strcmp(cwd, ".") == 0) {
      /* Default working directory is the app directory. */
    } else if (cwd[0] == '/') {
      snprintf(out->cwd, sizeof(out->cwd), "%s", cwd);
    } else if (!valid_relative_path(cwd)) {
      manifest_error(path, &doc, root, error, error_size,
                     "'cwd' must be absolute or a relative path without '..'");
      goto done;
    } else if (!join_path(out->dir, cwd, out->cwd, sizeof(out->cwd))) {
      manifest_error(path, &doc, root, error, error_size,
                     "'cwd' path is too long");
      goto done;
    }
  }

  int env = bd_json_object_get(&doc, root, "env");
  if (env >= 0 && !parse_env_object(&doc, env, path, out, error,
                                     error_size)) {
    goto done;
  }

  int accent = bd_json_object_get(&doc, root, "accent");
  if (accent >= 0) {
    char color[16];
    bool truncated = false;
    if (!copy_string_token(&doc, accent, color, sizeof(color), &truncated) ||
        truncated ||
        !normalize_color(color, out->accent, sizeof(out->accent))) {
      manifest_error(path, &doc, accent, error, error_size,
                     "'accent' must be #RRGGBB");
      goto done;
    }
  }

  if (!parse_art(&doc, root, path, out, error, error_size)) {
    goto done;
  }

  int caps = bd_json_object_get(&doc, root, "capabilities");
  if (caps >= 0 &&
      !parse_capabilities(&doc, caps, path, out, error, error_size)) {
    goto done;
  }

  int hooks = bd_json_object_get(&doc, root, "hooks");
  if (hooks >= 0 &&
      !parse_hooks(&doc, hooks, path, out, error, error_size)) {
    goto done;
  }

  int menu = bd_json_object_get(&doc, root, "menu");
  if (menu >= 0 && !parse_menu(&doc, menu, path, out, error, error_size)) {
    goto done;
  }

  int perf = bd_json_object_get(&doc, root, "perf");
  if (perf >= 0 && !parse_perf(&doc, perf, path, out, error, error_size)) {
    goto done;
  }

  int state = bd_json_object_get(&doc, root, "state");
  if (state >= 0 && !parse_state(&doc, state, path, out, error, error_size)) {
    goto done;
  }

  int live = bd_json_object_get(&doc, root, "live");
  if (live >= 0 && !parse_live(&doc, live, path, out, error, error_size)) {
    goto done;
  }

  ok = true;

done:
  bd_json_free(&doc);
  if (!ok) {
    bd_app_destroy(out);
  }
  return ok;
}

bool bd_app_parse_file(const char *manifest_path, bd_app *out, char *error,
                       size_t error_size) {
  if (manifest_path == NULL || out == NULL) {
    set_error(error, error_size, "invalid manifest path");
    return false;
  }
  memset(out, 0, sizeof(*out));

  int fd = open(manifest_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    set_error(error, error_size, "%s: %s", manifest_path, strerror(errno));
    return false;
  }
  struct stat status;
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
    set_error(error, error_size, "%s: not a regular file", manifest_path);
    close(fd);
    return false;
  }
  if (status.st_size <= 0 ||
      (uint64_t)status.st_size > BD_APP_MANIFEST_MAX_BYTES) {
    set_error(error, error_size, "%s: manifest size %lld is invalid", manifest_path,
              (long long)status.st_size);
    close(fd);
    return false;
  }

  size_t size = (size_t)status.st_size;
  char *buffer = (char *)malloc(size + 1);
  if (buffer == NULL) {
    set_error(error, error_size, "%s: out of memory", manifest_path);
    close(fd);
    return false;
  }
  size_t got = 0;
  while (got < size) {
    ssize_t n = read(fd, buffer + got, size - got);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      set_error(error, error_size, "%s: %s", manifest_path, strerror(errno));
      free(buffer);
      close(fd);
      return false;
    }
    if (n == 0) {
      break;
    }
    got += (size_t)n;
  }
  close(fd);
  if (got != size) {
    set_error(error, error_size, "%s: short read", manifest_path);
    free(buffer);
    return false;
  }
  buffer[size] = '\0';

  char *dir = parent_directory(manifest_path);
  if (dir == NULL) {
    set_error(error, error_size, "out of memory");
    free(buffer);
    return false;
  }
  bool ok = bd_app_parse_json(buffer, size, dir, manifest_path, out, error,
                              error_size);
  free(dir);
  free(buffer);
  return ok;
}

void bd_app_destroy(bd_app *app) {
  if (app == NULL) {
    return;
  }
  for (size_t i = 0; i < app->exec_count; ++i) {
    free(app->exec != NULL ? app->exec[i] : NULL);
  }
  free(app->exec);
  for (size_t i = 0; i < app->env_count; ++i) {
    free(app->env_keys != NULL ? app->env_keys[i] : NULL);
    free(app->env_values != NULL ? app->env_values[i] : NULL);
  }
  free(app->env_keys);
  free(app->env_values);
  for (int kind = 0; kind < BD_APP_HOOK_COUNT; ++kind) {
    for (size_t i = 0; i < app->hooks[kind].exec_count; ++i) {
      free(app->hooks[kind].exec != NULL ? app->hooks[kind].exec[i] : NULL);
    }
    free(app->hooks[kind].exec);
  }
  memset(app, 0, sizeof(*app));
}

bool bd_app_copy(bd_app *dst, const bd_app *src) {
  if (dst == NULL || src == NULL) {
    return false;
  }
  memset(dst, 0, sizeof(*dst));
  *dst = *src;
  dst->exec = NULL;
  dst->env_keys = NULL;
  dst->env_values = NULL;
  for (int kind = 0; kind < BD_APP_HOOK_COUNT; ++kind) {
    dst->hooks[kind].exec = NULL;
  }

  if (src->exec_count > 0) {
    dst->exec = (char **)calloc(src->exec_count, sizeof(char *));
    if (dst->exec == NULL) {
      bd_app_destroy(dst);
      return false;
    }
    dst->exec_count = src->exec_count;
    for (size_t i = 0; i < src->exec_count; ++i) {
      dst->exec[i] = duplicate_string(src->exec[i]);
      if (dst->exec[i] == NULL) {
        bd_app_destroy(dst);
        return false;
      }
    }
  }
  if (src->env_count > 0) {
    dst->env_keys = (char **)calloc(src->env_count, sizeof(char *));
    dst->env_values = (char **)calloc(src->env_count, sizeof(char *));
    if (dst->env_keys == NULL || dst->env_values == NULL) {
      bd_app_destroy(dst);
      return false;
    }
    dst->env_count = src->env_count;
    for (size_t i = 0; i < src->env_count; ++i) {
      dst->env_keys[i] = duplicate_string(src->env_keys[i]);
      dst->env_values[i] = duplicate_string(src->env_values[i]);
      if (dst->env_keys[i] == NULL || dst->env_values[i] == NULL) {
        bd_app_destroy(dst);
        return false;
      }
    }
  }
  for (int kind = 0; kind < BD_APP_HOOK_COUNT; ++kind) {
    const bd_app_hook *source = &src->hooks[kind];
    bd_app_hook *target = &dst->hooks[kind];
    if (source->exec_count > 0) {
      target->exec = (char **)calloc(source->exec_count, sizeof(char *));
      if (target->exec == NULL) {
        bd_app_destroy(dst);
        return false;
      }
      target->exec_count = source->exec_count;
      for (size_t i = 0; i < source->exec_count; ++i) {
        target->exec[i] = duplicate_string(source->exec[i]);
        if (target->exec[i] == NULL) {
          bd_app_destroy(dst);
          return false;
        }
      }
    }
  }
  return true;
}

const char *bd_app_category_name(bd_app_category category) {
  switch (category) {
    case BD_APP_CATEGORY_APP: return "app";
    case BD_APP_CATEGORY_GAME: return "game";
    case BD_APP_CATEGORY_EMULATOR: return "emulator";
    case BD_APP_CATEGORY_TOOL: return "tool";
    case BD_APP_CATEGORY_SERVICE: return "service";
  }
  return "app";
}

const char *bd_app_menu_mode_name(bd_app_menu_mode mode) {
  switch (mode) {
    case BD_APP_MENU_OVERLAY: return "overlay";
    case BD_APP_MENU_EMBEDDED: return "embedded";
    case BD_APP_MENU_HANDOFF: return "handoff";
    case BD_APP_MENU_NONE: return "none";
  }
  return "overlay";
}

const char *bd_app_hook_name(bd_app_hook_kind kind) {
  if (kind < 0 || kind >= BD_APP_HOOK_COUNT) {
    return "unknown";
  }
  return k_hook_names[kind];
}

const char *bd_app_span_name(bd_app_cover_span span) {
  if (span < 0 || span >= BD_APP_SPAN_COUNT) {
    return "1x1";
  }
  return k_span_names[span];
}

bool bd_app_span_from_name(const char *name, bd_app_cover_span *out) {
  if (name == NULL || out == NULL) {
    return false;
  }
  for (int i = 0; i < BD_APP_SPAN_COUNT; ++i) {
    if (strcmp(name, k_span_names[i]) == 0) {
      *out = (bd_app_cover_span)i;
      return true;
    }
  }
  return false;
}

static int compare_apps(const void *left, const void *right) {
  const bd_app *a = (const bd_app *)left;
  const bd_app *b = (const bd_app *)right;
  return strcmp(a->id, b->id);
}

bd_app_list *bd_app_list_create(void) {
  return (bd_app_list *)calloc(1, sizeof(bd_app_list));
}

void bd_app_list_destroy(bd_app_list *list) {
  if (list == NULL) {
    return;
  }
  for (size_t i = 0; i < list->count; ++i) {
    bd_app_destroy(&list->items[i]);
  }
  free(list->items);
  free(list);
}

static bool list_append(bd_app_list *list, const bd_app *app) {
  if (list->count == list->capacity) {
    size_t capacity = list->capacity == 0 ? 16 : list->capacity * 2;
    bd_app *items =
        (bd_app *)realloc(list->items, capacity * sizeof(bd_app));
    if (items == NULL) {
      return false;
    }
    list->items = items;
    list->capacity = capacity;
  }
  list->items[list->count] = *app;
  ++list->count;
  return true;
}

static bool manifest_for_dir(const char *dir, char *out, size_t out_size) {
  static const char *const names[] = {"app.json", "manifest.json"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    if (!join_path(dir, names[i], out, out_size)) {
      continue;
    }
    if (path_is_regular(out)) {
      return true;
    }
  }
  return false;
}

bool bd_app_list_scan(bd_app_list *list, const char *dir,
                      bd_app_scan_diagnostic_fn diagnostic, void *user,
                      char *error, size_t error_size) {
  if (list == NULL || dir == NULL) {
    set_error(error, error_size, "invalid app directory");
    return false;
  }
  DIR *handle = opendir(dir);
  if (handle == NULL) {
    set_error(error, error_size, "cannot open app directory '%s': %s", dir,
              strerror(errno));
    return false;
  }

  bool ok = true;
  struct dirent *entry = NULL;
  while ((entry = readdir(handle)) != NULL) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    char child[BD_APP_PATH_MAX];
    if (!join_path(dir, entry->d_name, child, sizeof(child))) {
      continue;
    }
    struct stat status;
    if (lstat(child, &status) != 0 || !S_ISDIR(status.st_mode)) {
      continue;
    }
    char manifest[BD_APP_PATH_MAX];
    if (!manifest_for_dir(child, manifest, sizeof(manifest))) {
      continue;
    }
    bd_app app;
    char parse_error[512] = {0};
    if (!bd_app_parse_file(manifest, &app, parse_error,
                           sizeof(parse_error))) {
      if (diagnostic != NULL) {
        diagnostic(user, manifest, parse_error);
      }
      continue;
    }
    bool duplicate = false;
    for (size_t i = 0; i < list->count; ++i) {
      if (strcmp(list->items[i].id, app.id) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      if (diagnostic != NULL) {
        diagnostic(user, manifest, "duplicate app id");
      }
      bd_app_destroy(&app);
      continue;
    }
    if (!list_append(list, &app)) {
      bd_app_destroy(&app);
      set_error(error, error_size, "out of memory scanning app directory");
      ok = false;
      break;
    }
  }
  closedir(handle);

  if (ok && list->count > 1) {
    qsort(list->items, list->count, sizeof(bd_app), compare_apps);
  }
  return ok;
}

size_t bd_app_list_count(const bd_app_list *list) {
  return list != NULL ? list->count : 0;
}

const bd_app *bd_app_list_at(const bd_app_list *list, size_t index) {
  if (list == NULL || index >= list->count) {
    return NULL;
  }
  return &list->items[index];
}

const bd_app *bd_app_list_find(const bd_app_list *list, const char *id) {
  if (list == NULL || id == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < list->count; ++i) {
    if (strcmp(list->items[i].id, id) == 0) {
      return &list->items[i];
    }
  }
  return NULL;
}

const char *bd_app_default_dir(void) {
  const char *environment = getenv("BRICKDESK_APPS_DIR");
  if (environment != NULL && environment[0] != '\0') {
    return environment;
  }
  return BRICKDESK_APPS_DIR;
}

static bool file_inside_app(const bd_app *app, const char *relative,
                            char *out, size_t out_size) {
  if (app == NULL || relative == NULL || relative[0] == '\0' ||
      !valid_relative_path(relative)) {
    return false;
  }
  char joined[BD_APP_PATH_MAX];
  if (!join_path(app->dir, relative, joined, sizeof(joined))) {
    return false;
  }
  char resolved[PATH_MAX];
  if (realpath(joined, resolved) == NULL) {
    return false;
  }
  size_t dir_length = strlen(app->dir);
  if (strncmp(resolved, app->dir, dir_length) != 0 ||
      resolved[dir_length] != '/') {
    return false;
  }
  struct stat status;
  if (stat(resolved, &status) != 0 || !S_ISREG(status.st_mode)) {
    return false;
  }
  if (strlen(resolved) >= out_size) {
    return false;
  }
  snprintf(out, out_size, "%s", resolved);
  return true;
}

static void initial_of(const char *name, char *out, size_t out_size) {
  if (out_size == 0) {
    return;
  }
  out[0] = '\0';
  if (name == NULL || name[0] == '\0') {
    return;
  }
  const unsigned char *text = (const unsigned char *)name;
  size_t length = 1;
  if (text[0] >= 0xf0 && text[0] <= 0xf4) {
    length = 4;
  } else if (text[0] >= 0xe0 && text[0] <= 0xef) {
    length = 3;
  } else if (text[0] >= 0xc2 && text[0] <= 0xdf) {
    length = 2;
  }
  if (length >= out_size) {
    length = out_size - 1;
  }
  for (size_t i = 1; i < length; ++i) {
    if ((text[i] & 0xc0) != 0x80) {
      length = i;
      break;
    }
  }
  memcpy(out, text, length);
  out[length] = '\0';
}

static void asset_color(const bd_app *app, char *out, size_t out_size) {
  if (app->accent[0] != '\0') {
    snprintf(out, out_size, "%s", app->accent);
  } else if (app->fallback_color[0] != '\0') {
    snprintf(out, out_size, "%s", app->fallback_color);
  } else {
    snprintf(out, out_size, "%s", "#A8674F");
  }
}

static bool asset_from_cover(const bd_app *app, bd_app_cover_span span,
                             bd_app_asset *out) {
  if (app->covers[span][0] == '\0') {
    return false;
  }
  if (!file_inside_app(app, app->covers[span], out->path,
                       sizeof(out->path))) {
    return false;
  }
  out->kind = BD_APP_ASSET_COVER;
  asset_color(app, out->color, sizeof(out->color));
  initial_of(app->name, out->initial, sizeof(out->initial));
  return true;
}

bool bd_app_asset_cover(const bd_app *app, bd_app_cover_span span,
                        bd_app_asset *out) {
  if (app == NULL || out == NULL || span < 0 || span >= BD_APP_SPAN_COUNT) {
    return false;
  }
  memset(out, 0, sizeof(*out));

  bd_app_cover_span chain[3];
  size_t count = 0;
  chain[count++] = span;
  if (span == BD_APP_SPAN_3X2) {
    chain[count++] = BD_APP_SPAN_2X1;
  }
  if (span != BD_APP_SPAN_1X1) {
    chain[count++] = BD_APP_SPAN_1X1;
  }
  for (size_t i = 0; i < count; ++i) {
    if (asset_from_cover(app, chain[i], out)) {
      return true;
    }
  }

  if (app->icon[0] != '\0' &&
      file_inside_app(app, app->icon, out->path, sizeof(out->path))) {
    out->kind = BD_APP_ASSET_ICON;
    asset_color(app, out->color, sizeof(out->color));
    initial_of(app->name, out->initial, sizeof(out->initial));
    return true;
  }

  initial_of(app->name, out->initial, sizeof(out->initial));
  asset_color(app, out->color, sizeof(out->color));
  out->kind = out->initial[0] != '\0' ? BD_APP_ASSET_MATERIAL
                                      : BD_APP_ASSET_NEUTRAL;
  return true;
}

bool bd_app_asset_backdrop(const bd_app *app, bd_app_asset *out) {
  if (app == NULL || out == NULL) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  if (app->backdrop[0] != '\0' &&
      file_inside_app(app, app->backdrop, out->path, sizeof(out->path))) {
    out->kind = BD_APP_ASSET_BACKDROP;
    asset_color(app, out->color, sizeof(out->color));
    initial_of(app->name, out->initial, sizeof(out->initial));
    return true;
  }

  static const bd_app_cover_span order[] = {
      BD_APP_SPAN_3X2, BD_APP_SPAN_2X2, BD_APP_SPAN_2X1,
      BD_APP_SPAN_1X2, BD_APP_SPAN_1X1};
  for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
    if (asset_from_cover(app, order[i], out)) {
      return true;
    }
  }

  initial_of(app->name, out->initial, sizeof(out->initial));
  asset_color(app, out->color, sizeof(out->color));
  out->kind = BD_APP_ASSET_MATERIAL;
  return true;
}
