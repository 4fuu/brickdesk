#ifndef BRICKDESK_CONFIG_INTERNAL_H
#define BRICKDESK_CONFIG_INTERNAL_H

#include <stddef.h>

#include "brickdesk/config.h"

#define BD_CONFIG_VALUE_MAX 256
#define BD_CONFIG_SPEC_REQUIRED 0x1u

typedef struct bd_config_spec {
  const char *section; /* "ui", or a prefix pattern such as "hotkey.*" */
  const char *key;     /* field name, or "*" to accept any name in the section */
  bd_config_type type;
  /* String form of the default; NULL means the key is optional and unset. */
  const char *default_value;
  long min_value;
  long max_value;
  const char *const *enum_values; /* NULL-terminated, for BD_CONFIG_ENUM */
  size_t max_length;              /* for string-like types; 0 means default */
  int duration_unit_ms;           /* unit for a duration without a suffix */
  unsigned flags;                 /* BD_CONFIG_SPEC_REQUIRED */
} bd_config_spec;

extern const bd_config_spec bd_config_specs[];
extern const size_t bd_config_spec_count;

/*
 * Canonicalizes `value` for `spec`.  Returns false and fills `error` when the
 * value is invalid.  `out` must hold at least `out_size` bytes.  For ACCENT,
 * both preset names and #RRGGBB are accepted.
 */
bool bd_config_normalize(const bd_config_spec *spec, const char *value,
                         char *out, size_t out_size, char *error,
                         size_t error_size);

#endif /* BRICKDESK_CONFIG_INTERNAL_H */
