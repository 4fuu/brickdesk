#ifndef BRICKDESK_CONFIG_H
#define BRICKDESK_CONFIG_H

/*
 * Layered INI configuration.
 *
 * Values come from compiled-in defaults, then the shipped INI files in the
 * configuration directory, then local.ini.  Every value is validated against
 * the schema in src/config/schema.c.  Reads are by dotted key:
 *
 *   ui.language
 *   input.long_press_ms
 *   hotkey.menu.hold_ms
 *   device.TrimUI Gamepad.A
 *
 * A key is the full dotted path of section and field.  The whole string is
 * matched exactly, so section names may contain dots and spaces.
 */

#include <stdbool.h>
#include <stddef.h>

/* Version understood by this build; bump when the schema changes. */
#define BD_CONFIG_SCHEMA_VERSION 1

typedef struct bd_config bd_config;

typedef enum bd_config_type {
  BD_CONFIG_BOOL = 0,
  BD_CONFIG_INT,
  BD_CONFIG_ENUM,
  BD_CONFIG_STRING,
  BD_CONFIG_ACCENT,
  BD_CONFIG_DURATION,
  BD_CONFIG_COMBO
} bd_config_type;

typedef enum bd_config_layer {
  BD_CONFIG_LAYER_DEFAULT = 0, /* compiled-in schema default */
  BD_CONFIG_LAYER_SYSTEM,      /* brickdesk.ini, perf.ini, hotkeys.ini, input.ini */
  BD_CONFIG_LAYER_USER         /* local.ini */
} bd_config_layer;

typedef enum bd_config_severity {
  BD_CONFIG_WARNING = 0,
  BD_CONFIG_ERROR
} bd_config_severity;

typedef struct bd_config_diagnostic {
  bd_config_severity severity;
  const char *file; /* NULL for compiled-in defaults */
  int line;         /* 0 when unknown */
  const char *key;  /* dotted key, or NULL for a file-level problem */
  const char *message;
} bd_config_diagnostic;

/*
 * A typed value.  For string-like types (enum, string, accent, combo) `string`
 * points into the config and stays valid until the next set or free.  For
 * durations `integer` is milliseconds.  `string` is NULL for bool/int/duration.
 */
typedef struct bd_config_value {
  bd_config_type type;
  bool boolean;
  long integer;
  const char *string;
} bd_config_value;

typedef struct bd_config_options {
  /* NULL uses BRICKDESK_CONFIG_DIR, then the build-time default. */
  const char *config_dir;
  /* Run schema migrations and write migrated files back. */
  bool migrate;
} bd_config_options;

typedef void (*bd_config_change_fn)(void *user, const char *key);

/*
 * Schema metadata for one key.  The settings UI derives its widgets from this
 * instead of restating each row: bools become toggles, ints and durations get
 * the min/max range, enums get their value list, accents become swatches.
 * `enum_values` points into the compiled-in schema and is NULL-terminated.
 */
typedef struct bd_config_schema_info {
  bd_config_type type;
  long min_value;
  long max_value;
  const char *const *enum_values;
  size_t enum_count;
  const char *default_value; /* NULL when the key is optional and unset */
  size_t max_length;
  int duration_unit_ms;
  bool required;
} bd_config_schema_info;

/*
 * Looks up the spec for a dotted key, including dynamic [hotkey.*],
 * [profile.*], [app.*], [core.*] and [device.*] keys.  Works for keys that
 * have no loaded value yet.  Returns false when the key is not in the schema.
 */
bool bd_config_describe(const bd_config *config, const char *key,
                        bd_config_schema_info *out);

/*
 * Loads all configuration.  Never returns NULL for bad file contents: the
 * failing key is recorded as a diagnostic and the lower layer stays in effect.
 * Returns NULL only on allocation failure, with `error` filled in.
 */
bd_config *bd_config_load(const bd_config_options *options, char *error,
                          size_t error_size);
void bd_config_free(bd_config *config);

/* BRICKDESK_CONFIG_DIR, or the build-time default (repo config/ on host). */
const char *bd_config_default_dir(void);

const char *bd_config_dir(const bd_config *config);
/* True when a file is from a newer schema; save() refuses to write. */
bool bd_config_read_only(const bd_config *config);

size_t bd_config_diagnostic_count(const bd_config *config);
bool bd_config_get_diagnostic(const bd_config *config, size_t index,
                              bd_config_diagnostic *out);

/* Effective keys in schema order (dynamic sections follow the static keys). */
size_t bd_config_key_count(const bd_config *config);
const char *bd_config_key(const bd_config *config, size_t index);

bool bd_config_get(const bd_config *config, const char *key,
                   bd_config_value *out);
const char *bd_config_get_string(const bd_config *config, const char *key,
                                 const char *fallback);
long bd_config_get_int(const bd_config *config, const char *key, long fallback);
bool bd_config_get_bool(const bd_config *config, const char *key, bool fallback);

bd_config_layer bd_config_source_layer(const bd_config *config,
                                       const char *key);
/* Returns NULL for compiled defaults.  `line` may be NULL. */
const char *bd_config_source_file(const bd_config *config, const char *key,
                                  int *line);

/*
 * Validates and applies a user override in memory.  It does not touch disk
 * until bd_config_save().  The change callback runs when the effective value
 * changes.
 */
bool bd_config_set(bd_config *config, const char *key, const char *value,
                   char *error, size_t error_size);

/* Atomically rewrites local.ini with only user overrides. */
bool bd_config_save(bd_config *config, char *error, size_t error_size);

void bd_config_set_change_callback(bd_config *config, bd_config_change_fn fn,
                                   void *user);

/*
 * Resolves an accent preset ("persimmon", "coral", "teal", "blue", "green",
 * "violet") to #RRGGBB for the dark or light palette.  A custom #RRGGBB value
 * is returned unchanged.  Returns NULL for an unknown value.
 */
const char *bd_config_accent_hex(const char *accent, bool dark);

#endif /* BRICKDESK_CONFIG_H */
