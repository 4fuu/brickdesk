#define _POSIX_C_SOURCE 200809L

#include "brickdesk/config.h"

#include <errno.h>
#include <fcntl.h>
#include <ini.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "config_internal.h"

#define BD_PATH_MAX 1024
#define BD_MAX_DIAGNOSTICS 256

typedef struct bd_record {
  char *section;
  char *key;
  char *value;
  char *file;
  int line;
  bd_config_layer layer;
  bool known;   /* a schema entry exists for this section/key */
  bool matched; /* the value was valid and applied */
} bd_record;

typedef struct bd_record_list {
  bd_record *items;
  size_t count;
  size_t capacity;
} bd_record_list;

typedef struct bd_entry {
  char *key; /* dotted path */
  char *section;
  char *name;
  const bd_config_spec *spec;
  char *text; /* canonical value for string-like types */
  bool boolean;
  long integer;
  bool valid;
  bool seen;     /* a file supplied this key, even if invalid */
  bool user_set; /* local.ini or bd_config_set owns the effective value */
  bd_config_layer layer;
  char *source_file;
  int source_line;
} bd_entry;

struct bd_config {
  char *dir;
  char *local_path;
  bool read_only;
  bool oom;
  bd_entry *entries;
  size_t entry_count;
  size_t entry_capacity;
  bd_record *records;
  size_t record_count;
  size_t record_capacity;
  bd_config_diagnostic *diagnostics;
  size_t diagnostic_count;
  size_t diagnostic_capacity;
  bd_config_change_fn change_fn;
  void *change_user;
};

typedef struct bd_parse_ctx {
  bd_config *config;
  bd_record_list *list;
  const char *file;
  bd_config_layer layer;
} bd_parse_ctx;

typedef struct bd_strbuf {
  char *data;
  size_t length;
  size_t capacity;
} bd_strbuf;

static bool run_migrations(bd_record_list *list, int *version, char *error,
                           size_t error_size);
static bool build_migrated_content(bd_strbuf *content,
                                   const bd_record_list *list, int version,
                                   const char *backup_path, char *error,
                                   size_t error_size);
static bool write_atomic(const char *path, const char *backup_path,
                         const char *data, size_t length, char *error,
                         size_t error_size);
static void strbuf_free(bd_strbuf *buffer);

static char *dup_string(const char *text) {
  if (text == NULL) {
    return NULL;
  }
  size_t length = strlen(text);
  char *copy = malloc(length + 1);
  if (copy != NULL) {
    memcpy(copy, text, length + 1);
  }
  return copy;
}

static void set_error(char *error, size_t error_size, const char *fmt, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  vsnprintf(error, error_size, fmt, args);
  va_end(args);
}

static bool make_path(const char *dir, const char *name, char *out,
                      size_t out_size) {
  size_t dir_length = strlen(dir);
  const char *separator =
      (dir_length > 0 && dir[dir_length - 1] == '/') ? "" : "/";
  int written = snprintf(out, out_size, "%s%s%s", dir, separator, name);
  return written > 0 && (size_t)written < out_size;
}

static bool path_dirname(const char *path, char *out, size_t out_size) {
  const char *slash = strrchr(path, '/');
  if (slash == NULL) {
    return snprintf(out, out_size, ".") < (int)out_size;
  }
  size_t length = (size_t)(slash - path);
  if (length == 0) {
    length = 1;
  }
  if (length + 1 > out_size) {
    return false;
  }
  memcpy(out, path, length);
  out[length] = '\0';
  return true;
}

static void diag_add(bd_config *config, bd_config_severity severity,
                     const char *file, int line, const char *key,
                     const char *fmt, ...) {
  if (config->diagnostic_count >= BD_MAX_DIAGNOSTICS) {
    return;
  }
  if (config->diagnostic_count == config->diagnostic_capacity) {
    size_t capacity =
        config->diagnostic_capacity == 0 ? 8 : config->diagnostic_capacity * 2;
    bd_config_diagnostic *list =
        realloc(config->diagnostics, capacity * sizeof(*list));
    if (list == NULL) {
      config->oom = true;
      return;
    }
    config->diagnostics = list;
    config->diagnostic_capacity = capacity;
  }
  char message[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  bd_config_diagnostic *diag =
      &config->diagnostics[config->diagnostic_count];
  memset(diag, 0, sizeof(*diag));
  diag->severity = severity;
  diag->line = line;
  if (file != NULL) {
    diag->file = dup_string(file);
    if (diag->file == NULL) {
      config->oom = true;
      return;
    }
  }
  if (key != NULL) {
    diag->key = dup_string(key);
    if (diag->key == NULL) {
      free((void *)diag->file);
      diag->file = NULL;
      config->oom = true;
      return;
    }
  }
  diag->message = dup_string(message);
  if (diag->message == NULL) {
    free((void *)diag->file);
    free((void *)diag->key);
    diag->file = NULL;
    diag->key = NULL;
    config->oom = true;
    return;
  }
  ++config->diagnostic_count;
}

static void record_list_free(bd_record_list *list) {
  for (size_t i = 0; i < list->count; ++i) {
    free(list->items[i].section);
    free(list->items[i].key);
    free(list->items[i].value);
    free(list->items[i].file);
  }
  free(list->items);
  memset(list, 0, sizeof(*list));
}

static bool record_list_add(bd_config *config, bd_record_list *list,
                            const char *section, const char *key,
                            const char *value, const char *file, int line,
                            bd_config_layer layer) {
  if (list->count == list->capacity) {
    size_t capacity = list->capacity == 0 ? 16 : list->capacity * 2;
    bd_record *items = realloc(list->items, capacity * sizeof(*items));
    if (items == NULL) {
      config->oom = true;
      return false;
    }
    list->items = items;
    list->capacity = capacity;
  }
  bd_record *record = &list->items[list->count];
  memset(record, 0, sizeof(*record));
  record->section = dup_string(section != NULL ? section : "");
  record->key = dup_string(key != NULL ? key : "");
  record->value = dup_string(value != NULL ? value : "");
  record->file = dup_string(file != NULL ? file : "");
  if (record->section == NULL || record->key == NULL ||
      record->value == NULL || record->file == NULL) {
    free(record->section);
    free(record->key);
    free(record->value);
    free(record->file);
    config->oom = true;
    return false;
  }
  record->line = line;
  record->layer = layer;
  ++list->count;
  return true;
}

static bool record_list_append_move(bd_config *config, bd_record_list *list) {
  if (list->count == 0) {
    return true;
  }
  if (config->record_count + list->count > config->record_capacity) {
    size_t capacity = config->record_capacity == 0 ? 16 : config->record_capacity;
    while (capacity < config->record_count + list->count) {
      capacity *= 2;
    }
    bd_record *items =
        realloc(config->records, capacity * sizeof(*items));
    if (items == NULL) {
      config->oom = true;
      return false;
    }
    config->records = items;
    config->record_capacity = capacity;
  }
  memcpy(&config->records[config->record_count], list->items,
         list->count * sizeof(*list->items));
  config->record_count += list->count;
  free(list->items);
  memset(list, 0, sizeof(*list));
  return true;
}

static int config_ini_handler(void *user, const char *section,
                              const char *name, const char *value,
                              int lineno) {
  bd_parse_ctx *ctx = user;
  if (ctx->config->oom) {
    return 0;
  }
  if (!record_list_add(ctx->config, ctx->list, section, name, value, ctx->file,
                       lineno, ctx->layer)) {
    return 0;
  }
  return 1;
}

static bool parse_long(const char *text, long *out) {
  if (text == NULL || *text == '\0') {
    return false;
  }
  errno = 0;
  char *end = NULL;
  long value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }
  *out = value;
  return true;
}

/* Returns the schema version of a parsed file, or -1 when [meta] is invalid. */
static int file_version(bd_config *config, const bd_record_list *list,
                        const char *file) {
  int version = 0;
  for (size_t i = 0; i < list->count; ++i) {
    const bd_record *record = &list->items[i];
    if (strcmp(record->section, "meta") != 0) {
      continue;
    }
    if (strcmp(record->key, "schema_version") == 0) {
      long parsed = 0;
      if (!parse_long(record->value, &parsed) || parsed < 0 ||
          parsed > INT_MAX) {
        diag_add(config, BD_CONFIG_ERROR, file, record->line,
                 "meta.schema_version", "expected a non-negative integer");
        return -1;
      }
      version = (int)parsed;
    } else {
      char meta_key[128];
      snprintf(meta_key, sizeof(meta_key), "meta.%s", record->key);
      diag_add(config, BD_CONFIG_WARNING, file, record->line, meta_key,
               "unknown key in [meta] ignored");
    }
  }
  return version;
}

static bool section_matches(const char *pattern, const char *section) {
  size_t length = strlen(pattern);
  if (length >= 2 && pattern[length - 1] == '*' &&
      pattern[length - 2] == '.') {
    size_t prefix = length - 2;
    return strncmp(pattern, section, prefix) == 0 && section[prefix] != '\0';
  }
  return strcmp(pattern, section) == 0;
}

static const bd_config_spec *find_spec(const char *section, const char *name) {
  for (size_t i = 0; i < bd_config_spec_count; ++i) {
    const bd_config_spec *spec = &bd_config_specs[i];
    if (!section_matches(spec->section, section)) {
      continue;
    }
    if (strcmp(spec->key, "*") == 0 || strcmp(spec->key, name) == 0) {
      return spec;
    }
  }
  return NULL;
}

static const bd_config_spec *find_section_spec(const char *section) {
  for (size_t i = 0; i < bd_config_spec_count; ++i) {
    if (section_matches(bd_config_specs[i].section, section)) {
      return &bd_config_specs[i];
    }
  }
  return NULL;
}

static bd_entry *find_entry(const bd_config *config, const char *section,
                            const char *name) {
  for (size_t i = 0; i < config->entry_count; ++i) {
    bd_entry *entry = &config->entries[i];
    if (strcmp(entry->section, section) == 0 &&
        strcmp(entry->name, name) == 0) {
      return entry;
    }
  }
  return NULL;
}

static bd_entry *find_entry_by_key(const bd_config *config, const char *key) {
  for (size_t i = 0; i < config->entry_count; ++i) {
    if (strcmp(config->entries[i].key, key) == 0) {
      return &config->entries[i];
    }
  }
  return NULL;
}

static char *make_entry_key(const char *section, const char *name) {
  size_t section_length = strlen(section);
  size_t name_length = strlen(name);
  size_t length = section_length + name_length + 2;
  char *key = malloc(length);
  if (key == NULL) {
    return NULL;
  }
  if (section_length == 0) {
    snprintf(key, length, "%s", name);
  } else {
    snprintf(key, length, "%s.%s", section, name);
  }
  return key;
}

static bool entry_apply(bd_config *config, bd_entry *entry,
                        const char *normalized, bd_config_layer layer,
                        const char *file, int line) {
  char *text = NULL;
  char *source_file = NULL;
  bool boolean = false;
  long integer = 0;

  switch (entry->spec->type) {
    case BD_CONFIG_BOOL:
      boolean = strcmp(normalized, "true") == 0;
      integer = boolean ? 1 : 0;
      break;
    case BD_CONFIG_INT:
    case BD_CONFIG_DURATION:
      if (!parse_long(normalized, &integer)) {
        config->oom = true;
        return false;
      }
      break;
    case BD_CONFIG_ENUM:
    case BD_CONFIG_STRING:
    case BD_CONFIG_ACCENT:
    case BD_CONFIG_COMBO:
      text = dup_string(normalized);
      if (text == NULL) {
        config->oom = true;
        return false;
      }
      break;
    default:
      config->oom = true;
      return false;
  }

  if (file != NULL) {
    source_file = dup_string(file);
    if (source_file == NULL) {
      free(text);
      config->oom = true;
      return false;
    }
  }

  free(entry->text);
  entry->text = text;
  free(entry->source_file);
  entry->source_file = source_file;
  entry->boolean = boolean;
  entry->integer = integer;
  entry->valid = true;
  entry->layer = layer;
  entry->source_line = line;
  if (layer == BD_CONFIG_LAYER_USER) {
    entry->user_set = true;
  }
  return true;
}

static bd_entry *entry_add(bd_config *config, const char *section,
                           const char *name, const bd_config_spec *spec) {
  if (config->entry_count == config->entry_capacity) {
    size_t capacity = config->entry_capacity == 0 ? 32 : config->entry_capacity * 2;
    bd_entry *entries = realloc(config->entries, capacity * sizeof(*entries));
    if (entries == NULL) {
      config->oom = true;
      return NULL;
    }
    config->entries = entries;
    config->entry_capacity = capacity;
  }
  bd_entry *entry = &config->entries[config->entry_count];
  memset(entry, 0, sizeof(*entry));
  entry->key = make_entry_key(section, name);
  entry->section = dup_string(section);
  entry->name = dup_string(name);
  if (entry->key == NULL || entry->section == NULL || entry->name == NULL) {
    free(entry->key);
    free(entry->section);
    free(entry->name);
    config->oom = true;
    return NULL;
  }
  entry->spec = spec;
  entry->layer = BD_CONFIG_LAYER_DEFAULT;
  ++config->entry_count;

  if (spec->default_value != NULL) {
    char normalized[BD_CONFIG_VALUE_MAX];
    char error[128];
    if (!bd_config_normalize(spec, spec->default_value, normalized,
                             sizeof(normalized), error, sizeof(error))) {
      diag_add(config, BD_CONFIG_ERROR, NULL, 0, entry->key,
               "bad schema default: %s", error);
      return entry;
    }
    if (!entry_apply(config, entry, normalized, BD_CONFIG_LAYER_DEFAULT, NULL,
                     0)) {
      return NULL;
    }
  }
  return entry;
}

static bool section_has_entries(const bd_config *config,
                                const char *section) {
  for (size_t i = 0; i < config->entry_count; ++i) {
    if (strcmp(config->entries[i].section, section) == 0) {
      return true;
    }
  }
  return false;
}

static bool section_location(const bd_config *config, const char *section,
                             const char **file, int *line) {
  for (size_t i = 0; i < config->record_count; ++i) {
    if (strcmp(config->records[i].section, section) == 0) {
      if (file != NULL) {
        *file = config->records[i].file;
      }
      if (line != NULL) {
        *line = config->records[i].line;
      }
      return true;
    }
  }
  if (file != NULL) {
    *file = NULL;
  }
  if (line != NULL) {
    *line = 0;
  }
  return false;
}

static void discover_dynamic_sections(bd_config *config) {
  for (size_t i = 0; i < config->record_count; ++i) {
    const bd_record *record = &config->records[i];
    if (strcmp(record->section, "meta") == 0) {
      continue;
    }
    if (section_has_entries(config, record->section)) {
      continue;
    }
    const bd_config_spec *pattern = find_section_spec(record->section);
    if (pattern == NULL) {
      continue;
    }
    for (size_t j = 0; j < bd_config_spec_count; ++j) {
      const bd_config_spec *spec = &bd_config_specs[j];
      if (strcmp(spec->section, pattern->section) != 0 ||
          strcmp(spec->key, "*") == 0) {
        continue;
      }
      if (entry_add(config, record->section, spec->key, spec) == NULL &&
          config->oom) {
        return;
      }
    }
  }
}

static void apply_records(bd_config *config) {
  for (size_t i = 0; i < config->record_count; ++i) {
    bd_record *record = &config->records[i];
    if (strcmp(record->section, "meta") == 0) {
      continue;
    }
    const bd_config_spec *spec = find_spec(record->section, record->key);
    char *dotted = NULL;
    if (spec == NULL) {
      dotted = make_entry_key(record->section, record->key);
      diag_add(config, BD_CONFIG_WARNING, record->file, record->line, dotted,
               "unknown key ignored%s",
               record->layer == BD_CONFIG_LAYER_USER
                   ? " (kept in local.ini on save)"
                   : "");
      free(dotted);
      continue;
    }
    record->known = true;

    bd_entry *entry = find_entry(config, record->section, record->key);
    if (entry == NULL) {
      entry = entry_add(config, record->section, record->key, spec);
      if (entry == NULL) {
        return;
      }
    }
    entry->seen = true;
    if (record->layer < entry->layer) {
      continue;
    }
    char normalized[BD_CONFIG_VALUE_MAX];
    char error[128];
    if (!bd_config_normalize(spec, record->value, normalized,
                             sizeof(normalized), error, sizeof(error))) {
      diag_add(config, BD_CONFIG_ERROR, record->file, record->line, entry->key,
               "%s", error);
      continue;
    }
    if (!entry_apply(config, entry, normalized, record->layer, record->file,
                     record->line)) {
      return;
    }
    record->matched = true;
  }
}

static void check_required(bd_config *config) {
  for (size_t i = 0; i < config->entry_count; ++i) {
    const bd_entry *entry = &config->entries[i];
    if ((entry->spec->flags & BD_CONFIG_SPEC_REQUIRED) == 0) {
      continue;
    }
    if (!entry->seen) {
      const char *file = NULL;
      int line = 0;
      section_location(config, entry->section, &file, &line);
      diag_add(config, BD_CONFIG_ERROR, file, line, entry->key,
               "missing required key");
    }
  }
}

static bool load_file(bd_config *config, const char *name,
                      bd_config_layer layer, bool migrate) {
  char path[BD_PATH_MAX];
  if (!make_path(config->dir, name, path, sizeof(path))) {
    diag_add(config, BD_CONFIG_ERROR, config->dir, 0, NULL,
             "configuration path is too long");
    return false;
  }

  struct stat status;
  if (stat(path, &status) != 0) {
    if (errno == ENOENT) {
      return true;
    }
    diag_add(config, BD_CONFIG_ERROR, path, 0, NULL, "cannot stat: %s",
             strerror(errno));
    return false;
  }
  if (!S_ISREG(status.st_mode)) {
    diag_add(config, BD_CONFIG_ERROR, path, 0, NULL, "not a regular file");
    return false;
  }

  bd_record_list list;
  memset(&list, 0, sizeof(list));
  bd_parse_ctx ctx = {config, &list, path, layer};
  int result = ini_parse(path, config_ini_handler, &ctx);
  if (result < 0) {
    if (result == -2) {
      config->oom = true;
    } else {
      diag_add(config, BD_CONFIG_ERROR, path, 0, NULL, "cannot read file");
    }
    record_list_free(&list);
    return false;
  }
  if (result > 0) {
    diag_add(config, BD_CONFIG_ERROR, path, result, NULL, "syntax error");
  }

  if (list.count == 0) {
    record_list_free(&list);
    return true;
  }

  int version = file_version(config, &list, path);
  if (version < 0) {
    config->read_only = true;
  } else if (version > BD_CONFIG_SCHEMA_VERSION) {
    diag_add(config, BD_CONFIG_ERROR, path, 0, NULL,
             "schema version %d is newer than supported version %d", version,
             BD_CONFIG_SCHEMA_VERSION);
    config->read_only = true;
  } else if (version < BD_CONFIG_SCHEMA_VERSION) {
    if (!migrate) {
      diag_add(config, BD_CONFIG_WARNING, path, 0, NULL,
               "schema version %d is older than %d; migration needed", version,
               BD_CONFIG_SCHEMA_VERSION);
    } else {
      int new_version = version;
      char error[128];
      if (!run_migrations(&list, &new_version, error, sizeof(error))) {
        diag_add(config, BD_CONFIG_ERROR, path, 0, NULL,
                 "migration failed: %s", error);
        config->read_only = true;
      } else {
        char backup[BD_PATH_MAX];
        int written = snprintf(backup, sizeof(backup), "%s.bak.v%d", path,
                               version);
        bd_strbuf content;
        memset(&content, 0, sizeof(content));
        if (written <= 0 || (size_t)written >= sizeof(backup) ||
            !build_migrated_content(&content, &list, new_version, backup,
                                    error, sizeof(error))) {
          diag_add(config, BD_CONFIG_ERROR, path, 0, NULL,
                   "migration failed: %s",
                   error[0] != '\0' ? error : "path is too long");
          config->read_only = true;
        } else if (!write_atomic(path, backup, content.data, content.length,
                                 error, sizeof(error))) {
          diag_add(config, BD_CONFIG_ERROR, path, 0, NULL,
                   "migration write failed: %s", error);
          config->read_only = true;
        }
        strbuf_free(&content);
      }
    }
  }

  bool ok = record_list_append_move(config, &list);
  record_list_free(&list);
  return ok;
}

/*
 * Migration hooks.  A hook may rewrite records and must advance *version.
 * The current chain only adds the [meta] header, which is written by the
 * migration writer; future hooks can rename or restructure keys here.
 */
static bool migrate_0_to_1(bd_record_list *list, int *version, char *error,
                           size_t error_size) {
  (void)list;
  (void)error;
  (void)error_size;
  *version = 1;
  return true;
}

static bool run_migrations(bd_record_list *list, int *version, char *error,
                           size_t error_size) {
  static const struct {
    int from;
    int to;
    bool (*fn)(bd_record_list *, int *, char *, size_t);
  } migrations[] = {
      {0, 1, migrate_0_to_1},
  };
  while (*version < BD_CONFIG_SCHEMA_VERSION) {
    bool applied = false;
    for (size_t i = 0; i < sizeof(migrations) / sizeof(migrations[0]); ++i) {
      if (migrations[i].from == *version) {
        int next = *version;
        if (!migrations[i].fn(list, &next, error, error_size) ||
            next <= *version) {
          if (error[0] == '\0') {
            set_error(error, error_size, "migration did not advance version");
          }
          return false;
        }
        *version = next;
        applied = true;
        break;
      }
    }
    if (!applied) {
      set_error(error, error_size, "no migration from version %d", *version);
      return false;
    }
  }
  return true;
}

static bool strbuf_reserve(bd_strbuf *buffer, size_t extra) {
  if (buffer->length + extra + 1 <= buffer->capacity) {
    return true;
  }
  size_t capacity = buffer->capacity == 0 ? 512 : buffer->capacity;
  while (capacity < buffer->length + extra + 1) {
    capacity *= 2;
  }
  char *data = realloc(buffer->data, capacity);
  if (data == NULL) {
    return false;
  }
  buffer->data = data;
  buffer->capacity = capacity;
  return true;
}

static bool strbuf_append(bd_strbuf *buffer, const char *text, size_t length) {
  if (!strbuf_reserve(buffer, length)) {
    return false;
  }
  memcpy(buffer->data + buffer->length, text, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
  return true;
}

static bool strbuf_printf(bd_strbuf *buffer, const char *fmt, ...) {
  va_list args;
  va_list copy;
  va_start(args, fmt);
  va_copy(copy, args);
  int needed = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if (needed < 0) {
    va_end(copy);
    return false;
  }
  if (!strbuf_reserve(buffer, (size_t)needed)) {
    va_end(copy);
    return false;
  }
  vsnprintf(buffer->data + buffer->length, (size_t)needed + 1, fmt, copy);
  va_end(copy);
  buffer->length += (size_t)needed;
  return true;
}

static void strbuf_free(bd_strbuf *buffer) {
  free(buffer->data);
  memset(buffer, 0, sizeof(*buffer));
}

static bool build_migrated_content(bd_strbuf *content,
                                   const bd_record_list *list, int version,
                                   const char *backup_path, char *error,
                                   size_t error_size) {
  if (!strbuf_printf(content,
                     "# Migrated by brickdesk-config to schema version %d.\n"
                     "# The original file is saved as %s.\n\n",
                     version, backup_path) ||
      !strbuf_printf(content, "[meta]\nschema_version = %d\n", version)) {
    set_error(error, error_size, "out of memory");
    return false;
  }
  for (size_t i = 0; i < list->count; ++i) {
    const bd_record *record = &list->items[i];
    if (strcmp(record->section, "meta") != 0 ||
        strcmp(record->key, "schema_version") == 0) {
      continue;
    }
    if (!strbuf_printf(content, "%s = %s\n", record->key, record->value)) {
      set_error(error, error_size, "out of memory");
      return false;
    }
  }
  if (!strbuf_append(content, "\n", 1)) {
    set_error(error, error_size, "out of memory");
    return false;
  }
  for (size_t i = 0; i < list->count; ++i) {
    const bd_record *record = &list->items[i];
    if (strcmp(record->section, "meta") == 0) {
      continue;
    }
    bool already = false;
    for (size_t j = 0; j < i; ++j) {
      if (strcmp(list->items[j].section, record->section) == 0 &&
          strcmp(list->items[j].section, "meta") != 0) {
        already = true;
        break;
      }
    }
    if (already) {
      continue;
    }
    if (!strbuf_printf(content, "[%s]\n", record->section)) {
      set_error(error, error_size, "out of memory");
      return false;
    }
    for (size_t j = i; j < list->count; ++j) {
      const bd_record *item = &list->items[j];
      if (strcmp(item->section, record->section) != 0) {
        continue;
      }
      if (!strbuf_printf(content, "%s = %s\n", item->key, item->value)) {
        set_error(error, error_size, "out of memory");
        return false;
      }
    }
    if (!strbuf_append(content, "\n", 1)) {
      set_error(error, error_size, "out of memory");
      return false;
    }
  }
  return true;
}

static bool copy_file(const char *source, const char *destination, char *error,
                      size_t error_size) {
  int input = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (input < 0) {
    set_error(error, error_size, "%s: %s", source, strerror(errno));
    return false;
  }
  int output = open(destination,
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                    0644);
  if (output < 0) {
    set_error(error, error_size, "%s: %s", destination, strerror(errno));
    close(input);
    return false;
  }
  (void)fchmod(output, 0644);
  char buffer[4096];
  bool ok = true;
  for (;;) {
    ssize_t got = read(input, buffer, sizeof(buffer));
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      set_error(error, error_size, "%s: %s", source, strerror(errno));
      ok = false;
      break;
    }
    if (got == 0) {
      break;
    }
    size_t offset = 0;
    while (offset < (size_t)got) {
      ssize_t written =
          write(output, buffer + offset, (size_t)got - offset);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        set_error(error, error_size, "%s: %s", destination, strerror(errno));
        ok = false;
        break;
      }
      if (written == 0) {
        set_error(error, error_size, "%s: short write", destination);
        ok = false;
        break;
      }
      offset += (size_t)written;
    }
    if (!ok) {
      break;
    }
  }
  if (ok && fsync(output) != 0) {
    set_error(error, error_size, "%s: %s", destination, strerror(errno));
    ok = false;
  }
  if (close(output) != 0 && ok) {
    set_error(error, error_size, "%s: %s", destination, strerror(errno));
    ok = false;
  }
  close(input);
  if (!ok) {
    unlink(destination);
  }
  return ok;
}

static void sync_directory(const char *path) {
  char directory[BD_PATH_MAX];
  if (!path_dirname(path, directory, sizeof(directory))) {
    return;
  }
  int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return;
  }
  (void)fsync(fd);
  close(fd);
}

static bool write_atomic(const char *path, const char *backup_path,
                         const char *data, size_t length, char *error,
                         size_t error_size) {
  struct stat status;
  if (stat(path, &status) == 0) {
    if (backup_path != NULL &&
        !copy_file(path, backup_path, error, error_size)) {
      return false;
    }
  } else if (errno != ENOENT) {
    set_error(error, error_size, "%s: %s", path, strerror(errno));
    return false;
  }

  char temporary[BD_PATH_MAX];
  int written = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
                         (long)getpid());
  if (written <= 0 || (size_t)written >= sizeof(temporary)) {
    set_error(error, error_size, "configuration path is too long");
    return false;
  }

  int fd = open(temporary,
                O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (fd < 0) {
    set_error(error, error_size, "%s: %s", temporary, strerror(errno));
    return false;
  }
  (void)fchmod(fd, 0644);

  long fail_after = -1;
  const char *failpoint = getenv("BRICKDESK_CONFIG_FAIL_WRITE");
  if (failpoint != NULL && *failpoint != '\0') {
    char *end = NULL;
    errno = 0;
    long parsed = strtol(failpoint, &end, 10);
    if (errno == 0 && end != failpoint && *end == '\0' && parsed >= 0) {
      fail_after = parsed;
    }
  }

  size_t offset = 0;
  bool ok = true;
  while (offset < length) {
    size_t chunk = length - offset;
    if (fail_after >= 0) {
      if (offset >= (size_t)fail_after) {
        set_error(error, error_size, "injected write failure");
        ok = false;
        break;
      }
      size_t remaining = (size_t)fail_after - offset;
      if (remaining < chunk) {
        chunk = remaining;
      }
    }
    ssize_t result = write(fd, data + offset, chunk);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      set_error(error, error_size, "%s: %s", temporary, strerror(errno));
      ok = false;
      break;
    }
    if (result == 0) {
      set_error(error, error_size, "%s: short write", temporary);
      ok = false;
      break;
    }
    offset += (size_t)result;
  }
  if (ok && fsync(fd) != 0) {
    set_error(error, error_size, "%s: %s", temporary, strerror(errno));
    ok = false;
  }
  if (close(fd) != 0 && ok) {
    set_error(error, error_size, "%s: %s", temporary, strerror(errno));
    ok = false;
  }
  if (!ok) {
    unlink(temporary);
    return false;
  }
  if (rename(temporary, path) != 0) {
    set_error(error, error_size, "%s: %s", path, strerror(errno));
    unlink(temporary);
    return false;
  }
  sync_directory(path);
  return true;
}

static bool format_entry_value(const bd_entry *entry, char *buffer,
                               size_t buffer_size) {
  int written = -1;
  switch (entry->spec->type) {
    case BD_CONFIG_BOOL:
      written = snprintf(buffer, buffer_size, "%s",
                         entry->boolean ? "true" : "false");
      break;
    case BD_CONFIG_INT:
      written = snprintf(buffer, buffer_size, "%ld", entry->integer);
      break;
    case BD_CONFIG_DURATION:
      if (entry->integer > 0 && entry->integer % 1000 == 0) {
        written = snprintf(buffer, buffer_size, "%lds", entry->integer / 1000);
      } else {
        written = snprintf(buffer, buffer_size, "%ldms", entry->integer);
      }
      break;
    case BD_CONFIG_ENUM:
    case BD_CONFIG_STRING:
    case BD_CONFIG_ACCENT:
    case BD_CONFIG_COMBO:
      written = snprintf(buffer, buffer_size, "%s",
                         entry->text != NULL ? entry->text : "");
      break;
    default:
      return false;
  }
  return written >= 0 && (size_t)written < buffer_size;
}

static bool section_in_list(const char *const *sections, size_t count,
                            const char *section) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(sections[i], section) == 0) {
      return true;
    }
  }
  return false;
}

static bool write_known_section(bd_strbuf *content, const bd_config *config,
                                const char *section) {
  for (size_t i = 0; i < config->entry_count; ++i) {
    const bd_entry *entry = &config->entries[i];
    if (strcmp(entry->section, section) != 0 || !entry->user_set ||
        !entry->valid) {
      continue;
    }
    char value[BD_CONFIG_VALUE_MAX];
    if (!format_entry_value(entry, value, sizeof(value)) ||
        !strbuf_printf(content, "%s = %s\n", entry->name, value)) {
      return false;
    }
  }
  return true;
}

static bool write_unknown_section(bd_strbuf *content, const bd_config *config,
                                  const char *section) {
  for (size_t i = 0; i < config->record_count; ++i) {
    const bd_record *record = &config->records[i];
    if (record->layer != BD_CONFIG_LAYER_USER || record->known ||
        record->matched || strcmp(record->section, "meta") == 0 ||
        strcmp(record->section, section) != 0) {
      continue;
    }
    if (!strbuf_printf(content, "%s = %s\n", record->key, record->value)) {
      return false;
    }
  }
  return true;
}

static bool build_local_content(bd_config *config, bd_strbuf *content,
                                char *error, size_t error_size) {
  if (!strbuf_printf(content,
                     "# Written by brickdesk-config; only user overrides are "
                     "stored here.\n\n") ||
      !strbuf_printf(content, "[meta]\nschema_version = %d\n",
                     BD_CONFIG_SCHEMA_VERSION)) {
    set_error(error, error_size, "out of memory");
    return false;
  }
  for (size_t i = 0; i < config->record_count; ++i) {
    const bd_record *record = &config->records[i];
    if (record->layer != BD_CONFIG_LAYER_USER || record->known ||
        record->matched || strcmp(record->section, "meta") != 0 ||
        strcmp(record->key, "schema_version") == 0) {
      continue;
    }
    if (!strbuf_printf(content, "%s = %s\n", record->key, record->value)) {
      set_error(error, error_size, "out of memory");
      return false;
    }
  }
  if (!strbuf_append(content, "\n", 1)) {
    set_error(error, error_size, "out of memory");
    return false;
  }

  size_t capacity = config->entry_count + config->record_count + 1;
  const char **sections = calloc(capacity, sizeof(*sections));
  if (sections == NULL) {
    set_error(error, error_size, "out of memory");
    return false;
  }
  size_t section_count = 0;

  for (size_t i = 0; i < config->entry_count; ++i) {
    const bd_entry *entry = &config->entries[i];
    if (!entry->user_set || !entry->valid ||
        section_in_list(sections, section_count, entry->section)) {
      continue;
    }
    if (!strbuf_printf(content, "[%s]\n", entry->section) ||
        !write_known_section(content, config, entry->section) ||
        !write_unknown_section(content, config, entry->section) ||
        !strbuf_append(content, "\n", 1)) {
      set_error(error, error_size, "out of memory");
      free(sections);
      return false;
    }
    sections[section_count++] = entry->section;
  }

  for (size_t i = 0; i < config->record_count; ++i) {
    const bd_record *record = &config->records[i];
    if (record->layer != BD_CONFIG_LAYER_USER || record->known ||
        record->matched || strcmp(record->section, "meta") == 0 ||
        section_in_list(sections, section_count, record->section)) {
      continue;
    }
    if (!strbuf_printf(content, "[%s]\n", record->section) ||
        !write_unknown_section(content, config, record->section) ||
        !strbuf_append(content, "\n", 1)) {
      set_error(error, error_size, "out of memory");
      free(sections);
      return false;
    }
    sections[section_count++] = record->section;
  }

  free(sections);
  return true;
}

static bool load_entries(bd_config *config) {
  for (size_t i = 0; i < bd_config_spec_count; ++i) {
    const bd_config_spec *spec = &bd_config_specs[i];
    if (strchr(spec->section, '*') != NULL) {
      continue;
    }
    if (entry_add(config, spec->section, spec->key, spec) == NULL &&
        config->oom) {
      return false;
    }
  }
  discover_dynamic_sections(config);
  if (config->oom) {
    return false;
  }
  apply_records(config);
  if (config->oom) {
    return false;
  }
  check_required(config);
  return !config->oom;
}

const char *bd_config_default_dir(void) {
  const char *env = getenv("BRICKDESK_CONFIG_DIR");
  if (env != NULL && *env != '\0') {
    return env;
  }
#ifdef BRICKDESK_BUILTIN_CONFIG_DIR
  return BRICKDESK_BUILTIN_CONFIG_DIR;
#else
  return "config";
#endif
}

bd_config *bd_config_load(const bd_config_options *options, char *error,
                          size_t error_size) {
  bd_config_options defaults = {NULL, true};
  if (options == NULL) {
    options = &defaults;
  }
  const char *dir = options->config_dir;
  if (dir == NULL || *dir == '\0') {
    dir = bd_config_default_dir();
  }

  bd_config *config = calloc(1, sizeof(*config));
  if (config == NULL) {
    set_error(error, error_size, "out of memory");
    return NULL;
  }
  config->dir = dup_string(dir);
  if (config->dir == NULL) {
    set_error(error, error_size, "out of memory");
    bd_config_free(config);
    return NULL;
  }
  char local[BD_PATH_MAX];
  if (!make_path(dir, "local.ini", local, sizeof(local))) {
    diag_add(config, BD_CONFIG_ERROR, dir, 0, NULL,
             "configuration path is too long");
    config->read_only = true;
  } else {
    config->local_path = dup_string(local);
    if (config->local_path == NULL) {
      set_error(error, error_size, "out of memory");
      bd_config_free(config);
      return NULL;
    }
  }

  struct stat status;
  bool dir_ok = stat(dir, &status) == 0 && S_ISDIR(status.st_mode);
  if (!dir_ok) {
    diag_add(config, BD_CONFIG_ERROR, dir, 0, NULL,
             "configuration directory not found");
  } else {
    static const char *const system_files[] = {
        "brickdesk.ini", "perf.ini", "hotkeys.ini", "input.ini"};
    for (size_t i = 0; i < sizeof(system_files) / sizeof(system_files[0]); ++i) {
      load_file(config, system_files[i], BD_CONFIG_LAYER_SYSTEM,
                options->migrate);
      if (config->oom) {
        break;
      }
    }
    if (!config->oom) {
      load_file(config, "local.ini", BD_CONFIG_LAYER_USER, options->migrate);
    }
  }

  if (config->oom || !load_entries(config)) {
    set_error(error, error_size, "out of memory");
    bd_config_free(config);
    return NULL;
  }
  return config;
}

void bd_config_free(bd_config *config) {
  if (config == NULL) {
    return;
  }
  for (size_t i = 0; i < config->entry_count; ++i) {
    free(config->entries[i].key);
    free(config->entries[i].section);
    free(config->entries[i].name);
    free(config->entries[i].text);
    free(config->entries[i].source_file);
  }
  free(config->entries);
  for (size_t i = 0; i < config->record_count; ++i) {
    free(config->records[i].section);
    free(config->records[i].key);
    free(config->records[i].value);
    free(config->records[i].file);
  }
  free(config->records);
  for (size_t i = 0; i < config->diagnostic_count; ++i) {
    free((void *)config->diagnostics[i].file);
    free((void *)config->diagnostics[i].key);
    free((void *)config->diagnostics[i].message);
  }
  free(config->diagnostics);
  free(config->dir);
  free(config->local_path);
  free(config);
}

const char *bd_config_dir(const bd_config *config) {
  return config != NULL ? config->dir : NULL;
}

bool bd_config_read_only(const bd_config *config) {
  return config != NULL && config->read_only;
}

size_t bd_config_diagnostic_count(const bd_config *config) {
  return config != NULL ? config->diagnostic_count : 0;
}

bool bd_config_get_diagnostic(const bd_config *config, size_t index,
                              bd_config_diagnostic *out) {
  if (config == NULL || out == NULL || index >= config->diagnostic_count) {
    return false;
  }
  *out = config->diagnostics[index];
  return true;
}

size_t bd_config_key_count(const bd_config *config) {
  size_t count = 0;
  if (config == NULL) {
    return 0;
  }
  for (size_t i = 0; i < config->entry_count; ++i) {
    if (config->entries[i].valid) {
      ++count;
    }
  }
  return count;
}

const char *bd_config_key(const bd_config *config, size_t index) {
  if (config == NULL) {
    return NULL;
  }
  size_t seen = 0;
  for (size_t i = 0; i < config->entry_count; ++i) {
    if (!config->entries[i].valid) {
      continue;
    }
    if (seen == index) {
      return config->entries[i].key;
    }
    ++seen;
  }
  return NULL;
}

bool bd_config_get(const bd_config *config, const char *key,
                   bd_config_value *out) {
  if (config == NULL || key == NULL || out == NULL) {
    return false;
  }
  const bd_entry *entry = find_entry_by_key(config, key);
  if (entry == NULL || !entry->valid) {
    return false;
  }
  out->type = entry->spec->type;
  out->boolean = entry->boolean;
  out->integer = entry->integer;
  out->string = entry->text;
  return true;
}

static const bd_config_spec *describe_spec(const bd_config *config,
                                           const char *key) {
  if (config != NULL) {
    const bd_entry *entry = find_entry_by_key(config, key);
    if (entry != NULL) {
      return entry->spec;
    }
  }
  size_t key_length = strlen(key);
  for (size_t i = 0; i < bd_config_spec_count; ++i) {
    const bd_config_spec *spec = &bd_config_specs[i];
    size_t section_length = strlen(spec->section);
    if (section_length >= 2 && spec->section[section_length - 1] == '*' &&
        spec->section[section_length - 2] == '.') {
      size_t prefix = section_length - 2;
      if (key_length <= prefix + 1 || strncmp(key, spec->section, prefix) != 0 ||
          key[prefix] != '.') {
        continue;
      }
      if (strcmp(spec->key, "*") == 0) {
        return spec;
      }
      size_t field_length = strlen(spec->key);
      if (key_length <= prefix + 1 + field_length + 1) {
        continue;
      }
      const char *field = key + key_length - field_length;
      if (field[-1] == '.' && strcmp(field, spec->key) == 0) {
        return spec;
      }
      continue;
    }
    size_t expected = section_length + 1 + strlen(spec->key);
    if (key_length == expected && strncmp(key, spec->section, section_length) == 0 &&
        key[section_length] == '.' &&
        strcmp(key + section_length + 1, spec->key) == 0) {
      return spec;
    }
  }
  return NULL;
}

bool bd_config_describe(const bd_config *config, const char *key,
                        bd_config_schema_info *out) {
  if (key == NULL || out == NULL) {
    return false;
  }
  const bd_config_spec *spec = describe_spec(config, key);
  if (spec == NULL) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  out->type = spec->type;
  out->min_value = spec->min_value;
  out->max_value = spec->max_value;
  out->enum_values = spec->enum_values;
  out->default_value = spec->default_value;
  out->max_length = spec->max_length;
  out->duration_unit_ms = spec->duration_unit_ms;
  out->required = (spec->flags & BD_CONFIG_SPEC_REQUIRED) != 0;
  if (spec->enum_values != NULL) {
    while (spec->enum_values[out->enum_count] != NULL) {
      ++out->enum_count;
    }
  }
  return true;
}

const char *bd_config_get_string(const bd_config *config, const char *key,
                                 const char *fallback) {
  bd_config_value value;
  if (!bd_config_get(config, key, &value)) {
    return fallback;
  }
  switch (value.type) {
    case BD_CONFIG_ENUM:
    case BD_CONFIG_STRING:
    case BD_CONFIG_ACCENT:
    case BD_CONFIG_COMBO:
      return value.string != NULL ? value.string : fallback;
    default:
      return fallback;
  }
}

long bd_config_get_int(const bd_config *config, const char *key,
                       long fallback) {
  bd_config_value value;
  if (!bd_config_get(config, key, &value)) {
    return fallback;
  }
  if (value.type == BD_CONFIG_INT || value.type == BD_CONFIG_DURATION) {
    return value.integer;
  }
  return fallback;
}

bool bd_config_get_bool(const bd_config *config, const char *key,
                        bool fallback) {
  bd_config_value value;
  if (!bd_config_get(config, key, &value)) {
    return fallback;
  }
  if (value.type == BD_CONFIG_BOOL) {
    return value.boolean;
  }
  return fallback;
}

bd_config_layer bd_config_source_layer(const bd_config *config,
                                       const char *key) {
  if (config == NULL || key == NULL) {
    return BD_CONFIG_LAYER_DEFAULT;
  }
  const bd_entry *entry = find_entry_by_key(config, key);
  return entry != NULL && entry->valid ? entry->layer : BD_CONFIG_LAYER_DEFAULT;
}

const char *bd_config_source_file(const bd_config *config, const char *key,
                                  int *line) {
  if (line != NULL) {
    *line = 0;
  }
  if (config == NULL || key == NULL) {
    return NULL;
  }
  const bd_entry *entry = find_entry_by_key(config, key);
  if (entry == NULL || !entry->valid) {
    return NULL;
  }
  if (line != NULL) {
    *line = entry->source_line;
  }
  return entry->source_file;
}

static bool entry_matches(const bd_entry *entry, const char *normalized) {
  if (!entry->valid) {
    return false;
  }
  switch (entry->spec->type) {
    case BD_CONFIG_BOOL:
      return entry->boolean == (strcmp(normalized, "true") == 0);
    case BD_CONFIG_INT:
    case BD_CONFIG_DURATION: {
      long value = 0;
      return parse_long(normalized, &value) && value == entry->integer;
    }
    case BD_CONFIG_ENUM:
    case BD_CONFIG_STRING:
    case BD_CONFIG_ACCENT:
    case BD_CONFIG_COMBO:
      return entry->text != NULL && strcmp(entry->text, normalized) == 0;
    default:
      return false;
  }
}

bool bd_config_set(bd_config *config, const char *key, const char *value,
                   char *error, size_t error_size) {
  if (config == NULL || key == NULL || value == NULL) {
    set_error(error, error_size, "invalid argument");
    return false;
  }
  if (config->read_only) {
    set_error(error, error_size,
              "configuration is read-only (written by a newer version)");
    return false;
  }
  bd_entry *entry = find_entry_by_key(config, key);
  if (entry == NULL) {
    set_error(error, error_size, "unknown key '%s'", key);
    return false;
  }
  char normalized[BD_CONFIG_VALUE_MAX];
  char normalize_error[128];
  if (!bd_config_normalize(entry->spec, value, normalized,
                           sizeof(normalized), normalize_error,
                           sizeof(normalize_error))) {
    set_error(error, error_size, "%s: %s", key, normalize_error);
    return false;
  }
  bool changed = !entry_matches(entry, normalized);
  if (!entry_apply(config, entry, normalized, BD_CONFIG_LAYER_USER,
                   config->local_path, 0)) {
    set_error(error, error_size, "out of memory");
    return false;
  }
  entry->user_set = true;
  if (changed && config->change_fn != NULL) {
    config->change_fn(config->change_user, entry->key);
  }
  return true;
}

bool bd_config_save(bd_config *config, char *error, size_t error_size) {
  if (config == NULL) {
    set_error(error, error_size, "invalid argument");
    return false;
  }
  if (config->read_only) {
    set_error(error, error_size,
              "configuration is read-only (written by a newer version)");
    return false;
  }
  if (config->local_path == NULL) {
    set_error(error, error_size, "configuration directory is not usable");
    return false;
  }
  bd_strbuf content;
  memset(&content, 0, sizeof(content));
  if (!build_local_content(config, &content, error, error_size)) {
    strbuf_free(&content);
    return false;
  }
  char backup[BD_PATH_MAX];
  int written = snprintf(backup, sizeof(backup), "%s.bak", config->local_path);
  bool ok = written > 0 && (size_t)written < sizeof(backup) &&
            write_atomic(config->local_path, backup, content.data,
                         content.length, error, error_size);
  if (written <= 0 || (size_t)written >= sizeof(backup)) {
    set_error(error, error_size, "configuration path is too long");
  }
  strbuf_free(&content);
  return ok;
}

void bd_config_set_change_callback(bd_config *config, bd_config_change_fn fn,
                                   void *user) {
  if (config == NULL) {
    return;
  }
  config->change_fn = fn;
  config->change_user = user;
}
