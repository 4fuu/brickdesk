#define _POSIX_C_SOURCE 200809L

#include "manifest.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json.h"

#define BD_PLUGIN_JSON_FILE_MAX (64 * 1024)

static void set_error(char *error, size_t error_size, const char *format,
                      const char *detail) {
  if (error == NULL || error_size == 0) {
    return;
  }
  if (detail != NULL) {
    snprintf(error, error_size, format, detail);
  } else {
    snprintf(error, error_size, "%s", format);
  }
}

static bool copy_string_field(const bd_plugin_json *json,
                              const jsmntok_t *object, const char *field,
                              char *out, size_t out_size, bool required,
                              char *error, size_t error_size) {
  const jsmntok_t *token = bd_plugin_json_member(json, object, field);
  if (token == NULL) {
    if (required) {
      set_error(error, error_size, "missing string field '%s'", field);
      return false;
    }
    return true;
  }
  if (!bd_plugin_json_string(json, token, out, out_size)) {
    set_error(error, error_size, "field '%s' is not a valid string or is too long",
              field);
    return false;
  }
  if (required && out[0] == '\0') {
    set_error(error, error_size, "field '%s' must not be empty", field);
    return false;
  }
  return true;
}

static bool valid_id(const char *id) {
  size_t length = strlen(id);
  if (length == 0 || length > BD_PLUGIN_MAX_ID) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    char c = id[i];
    bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '.' || c == '_' || c == '-';
    if (!allowed) {
      return false;
    }
  }
  return id[0] != '.' && id[0] != '-';
}

static bool safe_relative_path(const char *path) {
  if (path == NULL || path[0] == '\0' || path[0] == '/') {
    return false;
  }
  const char *cursor = path;
  while (*cursor != '\0') {
    const char *slash = strchr(cursor, '/');
    size_t length = slash != NULL ? (size_t)(slash - cursor) : strlen(cursor);
    if (length == 0) {
      return false;
    }
    if ((length == 1 && cursor[0] == '.') ||
        (length == 2 && cursor[0] == '.' && cursor[1] == '.')) {
      return false;
    }
    if (slash == NULL) {
      break;
    }
    cursor = slash + 1;
  }
  return true;
}

static bool join_path(char *out, size_t out_size, const char *dir,
                      const char *name) {
  int written = snprintf(out, out_size, "%s/%s", dir, name);
  return written > 0 && (size_t)written < out_size;
}

static bool parse_capabilities(const bd_plugin_json *json,
                               const jsmntok_t *root, unsigned *out,
                               char *error, size_t error_size) {
  const jsmntok_t *array = bd_plugin_json_member(json, root, "capabilities");
  if (array == NULL) {
    *out = 0;
    return true;
  }
  if (!bd_plugin_json_is_array(array)) {
    set_error(error, error_size, "field 'capabilities' must be an array", NULL);
    return false;
  }
  unsigned capabilities = 0;
  int index = (int)(array - json->tokens) + 1;
  for (int i = 0; i < array->size; ++i) {
    const jsmntok_t *token = &json->tokens[index];
    char name[24];
    if (!bd_plugin_json_string(json, token, name, sizeof(name))) {
      set_error(error, error_size, "capability must be a string", NULL);
      return false;
    }
    if (strcmp(name, "tile") == 0) {
      capabilities |= BD_PLUGIN_CAP_TILE;
    } else if (strcmp(name, "menu") == 0) {
      capabilities |= BD_PLUGIN_CAP_MENU;
    } else if (strcmp(name, "settings") == 0) {
      capabilities |= BD_PLUGIN_CAP_SETTINGS;
    } else if (strcmp(name, "hotkey") == 0) {
      capabilities |= BD_PLUGIN_CAP_HOTKEY;
    } else if (strcmp(name, "background") == 0) {
      capabilities |= BD_PLUGIN_CAP_BACKGROUND;
    } else {
      set_error(error, error_size, "unknown capability '%s'", name);
      return false;
    }
    const jsmntok_t *next = bd_plugin_json_skip(json, token);
    if (next == NULL) {
      return false;
    }
    index = (int)(next - json->tokens);
  }
  *out = capabilities;
  return true;
}

static bool parse_permissions(const bd_plugin_json *json,
                              const jsmntok_t *root, bd_plugin_manifest *out,
                              char *error, size_t error_size) {
  const jsmntok_t *array = bd_plugin_json_member(json, root, "permissions");
  if (array == NULL) {
    return true;
  }
  if (!bd_plugin_json_is_array(array) ||
      array->size > BD_PLUGIN_MAX_PERMISSIONS) {
    set_error(error, error_size,
              "field 'permissions' must be an array of at most 8 strings",
              NULL);
    return false;
  }
  int index = (int)(array - json->tokens) + 1;
  for (int i = 0; i < array->size; ++i) {
    const jsmntok_t *token = &json->tokens[index];
    if (!bd_plugin_json_string(json, token, out->permissions[i],
                               sizeof(out->permissions[i]))) {
      set_error(error, error_size, "permission must be a short string", NULL);
      return false;
    }
    ++out->permission_count;
    const jsmntok_t *next = bd_plugin_json_skip(json, token);
    if (next == NULL) {
      return false;
    }
    index = (int)(next - json->tokens);
  }
  return true;
}

static bool parse_entry(const bd_plugin_json *json, const jsmntok_t *root,
                        bd_plugin_manifest *out, char *error,
                        size_t error_size) {
  const jsmntok_t *array = bd_plugin_json_member(json, root, "entry");
  if (!bd_plugin_json_is_array(array) || array->size < 1 ||
      array->size > BD_PLUGIN_MAX_ENTRY_ARGS) {
    set_error(error, error_size,
              "field 'entry' must be an array of 1 to 8 strings", NULL);
    return false;
  }
  int index = (int)(array - json->tokens) + 1;
  for (int i = 0; i < array->size; ++i) {
    const jsmntok_t *token = &json->tokens[index];
    if (!bd_plugin_json_string(json, token, out->entry[i],
                               sizeof(out->entry[i]))) {
      set_error(error, error_size,
                "entry arguments must be strings shorter than 512 bytes", NULL);
      return false;
    }
    ++out->entry_count;
    const jsmntok_t *next = bd_plugin_json_skip(json, token);
    if (next == NULL) {
      return false;
    }
    index = (int)(next - json->tokens);
  }
  if (!safe_relative_path(out->entry[0])) {
    set_error(error, error_size,
              "entry[0] must be a relative path without '..' components", NULL);
    return false;
  }
  char full[BD_PLUGIN_MAX_PATH + 64];
  if (!join_path(full, sizeof(full), out->dir, out->entry[0]) ||
      access(full, X_OK) != 0) {
    set_error(error, error_size, "entry executable '%s' is missing or not executable",
              out->entry[0]);
    return false;
  }
  return true;
}

static bool parse_tile(const bd_plugin_json *json, const jsmntok_t *root,
                       bd_plugin_manifest *out, char *error,
                       size_t error_size) {
  const jsmntok_t *tile = bd_plugin_json_member(json, root, "tile");
  if ((out->capabilities & BD_PLUGIN_CAP_TILE) == 0) {
    out->tile.w = 1;
    out->tile.h = 1;
    out->tile.refresh_hz = BD_PLUGIN_TILE_MAX_FPS;
    return true;
  }
  if (!bd_plugin_json_is_object(tile)) {
    set_error(error, error_size,
              "capability 'tile' requires a 'tile' object", NULL);
    return false;
  }
  int w = 0;
  int h = 0;
  int refresh = 0;
  const jsmntok_t *value = bd_plugin_json_member(json, tile, "w");
  if (!bd_plugin_json_int(json, value, &w) || w < 1 || w > 2) {
    set_error(error, error_size, "tile.w must be 1 or 2", NULL);
    return false;
  }
  value = bd_plugin_json_member(json, tile, "h");
  if (!bd_plugin_json_int(json, value, &h) || h < 1 || h > 2) {
    set_error(error, error_size, "tile.h must be 1 or 2", NULL);
    return false;
  }
  value = bd_plugin_json_member(json, tile, "refresh_hz");
  if (!bd_plugin_json_int(json, value, &refresh) || refresh < 1 ||
      refresh > BD_PLUGIN_TILE_MAX_FPS) {
    set_error(error, error_size, "tile.refresh_hz must be 1..15", NULL);
    return false;
  }
  out->tile.w = w;
  out->tile.h = h;
  out->tile.refresh_hz = refresh;
  return true;
}

static bool parse_menu(const bd_plugin_json *json, const jsmntok_t *root,
                       bd_plugin_manifest *out, char *error,
                       size_t error_size) {
  const jsmntok_t *array = bd_plugin_json_member(json, root, "menu");
  if (array == NULL) {
    return true;
  }
  if (!bd_plugin_json_is_array(array) ||
      array->size > BD_PLUGIN_MAX_MENU_ITEMS) {
    set_error(error, error_size,
              "field 'menu' must be an array of at most 8 objects", NULL);
    return false;
  }
  int index = (int)(array - json->tokens) + 1;
  for (int i = 0; i < array->size; ++i) {
    const jsmntok_t *item = &json->tokens[index];
    if (!bd_plugin_json_is_object(item)) {
      set_error(error, error_size, "each menu entry must be an object", NULL);
      return false;
    }
    bd_plugin_menu_item *entry = &out->menu[out->menu_count];
    memset(entry, 0, sizeof(*entry));
    if (!copy_string_field(json, item, "id", entry->id, sizeof(entry->id), true,
                           error, error_size) ||
        !copy_string_field(json, item, "label", entry->label,
                           sizeof(entry->label), true, error, error_size)) {
      return false;
    }
    for (int existing = 0; existing < out->menu_count; ++existing) {
      if (strcmp(out->menu[existing].id, entry->id) == 0) {
        set_error(error, error_size, "duplicate menu id '%s'", entry->id);
        return false;
      }
    }
    ++out->menu_count;
    const jsmntok_t *next = bd_plugin_json_skip(json, item);
    if (next == NULL) {
      return false;
    }
    index = (int)(next - json->tokens);
  }
  return true;
}

static bool parse_settings(const bd_plugin_json *json, const jsmntok_t *root,
                           bd_plugin_manifest *out, char *error,
                           size_t error_size) {
  const jsmntok_t *array = bd_plugin_json_member(json, root, "settings");
  if (array == NULL) {
    return true;
  }
  if (!bd_plugin_json_is_array(array) ||
      array->size > BD_PLUGIN_MAX_SETTINGS) {
    set_error(error, error_size,
              "field 'settings' must be an array of at most 8 objects", NULL);
    return false;
  }
  int index = (int)(array - json->tokens) + 1;
  for (int i = 0; i < array->size; ++i) {
    const jsmntok_t *setting = &json->tokens[index];
    if (!bd_plugin_json_is_object(setting)) {
      set_error(error, error_size, "each settings entry must be an object",
                NULL);
      return false;
    }
    bd_plugin_setting_spec *spec = &out->settings[out->setting_count];
    memset(spec, 0, sizeof(*spec));
    if (!copy_string_field(json, setting, "key", spec->key, sizeof(spec->key),
                           true, error, error_size) ||
        !copy_string_field(json, setting, "label", spec->label,
                           sizeof(spec->label), false, error, error_size)) {
      return false;
    }
    if (spec->label[0] == '\0') {
      size_t key_length = strlen(spec->key);
      if (key_length >= sizeof(spec->label)) {
        key_length = sizeof(spec->label) - 1;
      }
      memmove(spec->label, spec->key, key_length);
      spec->label[key_length] = '\0';
    }
    char type[16] = "string";
    if (!copy_string_field(json, setting, "type", type, sizeof(type), false,
                           error, error_size)) {
      return false;
    }
    if (strcmp(type, "bool") == 0) {
      spec->type = BD_PLUGIN_SETTING_BOOL;
    } else if (strcmp(type, "int") == 0) {
      spec->type = BD_PLUGIN_SETTING_INT;
    } else if (strcmp(type, "float") == 0) {
      spec->type = BD_PLUGIN_SETTING_FLOAT;
    } else if (strcmp(type, "string") == 0) {
      spec->type = BD_PLUGIN_SETTING_STRING;
    } else {
      set_error(error, error_size,
                "setting type must be bool, int, float or string", NULL);
      return false;
    }
    const jsmntok_t *fallback = bd_plugin_json_member(json, setting, "default");
    if (fallback != NULL && spec->type == BD_PLUGIN_SETTING_BOOL) {
      bool value = false;
      if (!bd_plugin_json_bool(json, fallback, &value)) {
        set_error(error, error_size, "setting 'default' must be a boolean", NULL);
        return false;
      }
      snprintf(spec->default_value, sizeof(spec->default_value), "%s",
               value ? "true" : "false");
    } else if (fallback != NULL && spec->type == BD_PLUGIN_SETTING_INT) {
      int value = 0;
      if (!bd_plugin_json_int(json, fallback, &value)) {
        set_error(error, error_size, "setting 'default' must be an integer",
                  NULL);
        return false;
      }
      snprintf(spec->default_value, sizeof(spec->default_value), "%d", value);
    } else if (fallback != NULL && spec->type == BD_PLUGIN_SETTING_FLOAT) {
      float value = 0.0f;
      if (!bd_plugin_json_float(json, fallback, &value)) {
        set_error(error, error_size, "setting 'default' must be a number", NULL);
        return false;
      }
      snprintf(spec->default_value, sizeof(spec->default_value), "%.6g",
               (double)value);
    } else if (fallback != NULL) {
      if (!bd_plugin_json_string(json, fallback, spec->default_value,
                                 sizeof(spec->default_value))) {
        set_error(error, error_size, "setting 'default' must be a string", NULL);
        return false;
      }
    } else if (spec->type == BD_PLUGIN_SETTING_BOOL) {
      snprintf(spec->default_value, sizeof(spec->default_value), "false");
    }
    for (int existing = 0; existing < out->setting_count; ++existing) {
      if (strcmp(out->settings[existing].key, spec->key) == 0) {
        set_error(error, error_size, "duplicate setting key '%s'", spec->key);
        return false;
      }
    }
    ++out->setting_count;
    const jsmntok_t *next = bd_plugin_json_skip(json, setting);
    if (next == NULL) {
      return false;
    }
    index = (int)(next - json->tokens);
  }
  return true;
}

static bool parse_limits(const bd_plugin_json *json, const jsmntok_t *root,
                         bd_plugin_manifest *out, char *error,
                         size_t error_size) {
  out->memory_limit_bytes = 64u * 1024u * 1024u;
  out->nofile_limit = 64;
  out->nice_value = 10;
  const jsmntok_t *limits = bd_plugin_json_member(json, root, "limits");
  if (limits == NULL) {
    return true;
  }
  if (!bd_plugin_json_is_object(limits)) {
    set_error(error, error_size, "field 'limits' must be an object", NULL);
    return false;
  }
  const jsmntok_t *value = bd_plugin_json_member(json, limits, "memory_mb");
  if (value != NULL) {
    int memory_mb = 0;
    if (!bd_plugin_json_int(json, value, &memory_mb) || memory_mb < 1 ||
        memory_mb > 256) {
      set_error(error, error_size, "limits.memory_mb must be 1..256", NULL);
      return false;
    }
    out->memory_limit_bytes = (size_t)memory_mb * 1024u * 1024u;
  }
  value = bd_plugin_json_member(json, limits, "nofile");
  if (value != NULL) {
    int nofile = 0;
    if (!bd_plugin_json_int(json, value, &nofile) || nofile < 8 ||
        nofile > 256) {
      set_error(error, error_size, "limits.nofile must be 8..256", NULL);
      return false;
    }
    out->nofile_limit = nofile;
  }
  value = bd_plugin_json_member(json, limits, "nice");
  if (value != NULL) {
    int nice_value = 0;
    if (!bd_plugin_json_int(json, value, &nice_value) || nice_value < 0 ||
        nice_value > 19) {
      set_error(error, error_size, "limits.nice must be 0..19", NULL);
      return false;
    }
    out->nice_value = nice_value;
  }
  return true;
}

static bool read_file(const char *path, char **out, size_t *out_length,
                      char *error, size_t error_size) {
  struct stat info;
  if (lstat(path, &info) != 0) {
    set_error(error, error_size, "cannot stat '%s'", path);
    return false;
  }
  if (!S_ISREG(info.st_mode) || info.st_size <= 0 ||
      info.st_size > BD_PLUGIN_JSON_FILE_MAX) {
    set_error(error, error_size, "plugin.json is missing, not a file, or larger than 64 KB",
              NULL);
    return false;
  }
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    set_error(error, error_size, "cannot open plugin.json", NULL);
    return false;
  }
  size_t size = (size_t)info.st_size;
  char *buffer = (char *)malloc(size + 1);
  if (buffer == NULL) {
    fclose(file);
    set_error(error, error_size, "out of memory reading plugin.json", NULL);
    return false;
  }
  size_t read = fread(buffer, 1, size, file);
  fclose(file);
  if (read != size) {
    free(buffer);
    set_error(error, error_size, "short read on plugin.json", NULL);
    return false;
  }
  buffer[size] = '\0';
  *out = buffer;
  *out_length = size;
  return true;
}

bool bd_plugin_manifest_load(const char *dir, bd_plugin_manifest *out,
                             char *error, size_t error_size) {
  if (dir == NULL || out == NULL) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  if (strlen(dir) >= sizeof(out->dir)) {
    set_error(error, error_size, "plugin directory path is too long", NULL);
    return false;
  }
  snprintf(out->dir, sizeof(out->dir), "%s", dir);

  char json_path[BD_PLUGIN_MAX_PATH + 32];
  if (!join_path(json_path, sizeof(json_path), dir, "plugin.json")) {
    set_error(error, error_size, "plugin directory path is too long", NULL);
    return false;
  }
  char *text = NULL;
  size_t length = 0;
  if (!read_file(json_path, &text, &length, error, error_size)) {
    return false;
  }

  bd_plugin_json json;
  bool ok = false;
  char parse_error[192] = {0};
  if (!bd_plugin_json_parse(text, length, &json, 1024, parse_error,
                            sizeof(parse_error))) {
    set_error(error, error_size, "plugin.json: %s", parse_error);
    free(text);
    return false;
  }

  const jsmntok_t *root = &json.tokens[0];
  int schema = 0;
  const jsmntok_t *token = bd_plugin_json_member(&json, root, "schema");
  if (!bd_plugin_json_int(&json, token, &schema) || schema != 1) {
    set_error(error, error_size, "schema must be 1", NULL);
    goto done;
  }
  token = bd_plugin_json_member(&json, root, "api");
  if (!bd_plugin_json_int(&json, token, &out->api) ||
      out->api < 1 || out->api > BD_PLUGIN_API_VERSION) {
    set_error(error, error_size, "api must be 1", NULL);
    goto done;
  }
  if (!copy_string_field(&json, root, "id", out->id, sizeof(out->id), true,
                         error, error_size) ||
      !copy_string_field(&json, root, "name", out->name, sizeof(out->name),
                         true, error, error_size) ||
      !copy_string_field(&json, root, "version", out->version,
                         sizeof(out->version), true, error, error_size)) {
    goto done;
  }
  if (!valid_id(out->id)) {
    set_error(error, error_size,
              "id must match [a-z0-9._-]{1,64} and must not start with '.' or '-'",
              NULL);
    goto done;
  }
  /* The install directory is a package name; the manifest id is the stable
   * identity. The id is validated above, so it is safe in socket/shm paths. */
  snprintf(out->runtime, sizeof(out->runtime), "%s", "native");
  if (!copy_string_field(&json, root, "runtime", out->runtime,
                         sizeof(out->runtime), false, error, error_size)) {
    goto done;
  }
  if (strcmp(out->runtime, "native") != 0 && strcmp(out->runtime, "python") != 0 &&
      strcmp(out->runtime, "shell") != 0 && strcmp(out->runtime, "lua") != 0) {
    set_error(error, error_size,
              "runtime must be native, python, shell or lua", NULL);
    goto done;
  }
  token = bd_plugin_json_member(&json, root, "autostart");
  if (token != NULL && !bd_plugin_json_bool(&json, token, &out->autostart)) {
    set_error(error, error_size, "autostart must be a boolean", NULL);
    goto done;
  }
  if (!parse_capabilities(&json, root, &out->capabilities, error,
                          error_size) ||
      !parse_permissions(&json, root, out, error, error_size) ||
      !parse_entry(&json, root, out, error, error_size) ||
      !parse_tile(&json, root, out, error, error_size) ||
      !parse_menu(&json, root, out, error, error_size) ||
      !parse_settings(&json, root, out, error, error_size) ||
      !parse_limits(&json, root, out, error, error_size)) {
    goto done;
  }
  ok = true;

done:
  bd_plugin_json_destroy(&json);
  free(text);
  return ok;
}
