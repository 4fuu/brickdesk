#ifndef BRICKDESK_SESSION_JSON_UTIL_H
#define BRICKDESK_SESSION_JSON_UTIL_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Minimal jsmn wrapper shared by the manifest parser and the app IPC host.
 * Tokens are addressed by integer index; callers never see jsmntok_t.
 */
typedef struct bd_json {
  const char *text;
  size_t length;
  void *tokens;
  int token_count;
} bd_json;

bool bd_json_parse(bd_json *doc, const char *text, size_t length, char *error,
                   size_t error_size);
void bd_json_free(bd_json *doc);

int bd_json_root(const bd_json *doc);
bool bd_json_is_object(const bd_json *doc, int token);
bool bd_json_is_array(const bd_json *doc, int token);
bool bd_json_is_string(const bd_json *doc, int token);
bool bd_json_is_primitive(const bd_json *doc, int token);

/* Returns the value token index, or -1 when the key is absent/not an object. */
int bd_json_object_get(const bd_json *doc, int object, const char *key);
int bd_json_array_size(const bd_json *doc, int array);
/* Returns the value token at `index` inside `array`, or -1. */
int bd_json_array_at(const bd_json *doc, int array, int index);
/* Index just past the value at `token`, for manual object iteration. */
int bd_json_skip(const bd_json *doc, int token);

/* Unescapes a JSON string.  `truncated` may be NULL. */
bool bd_json_string(const bd_json *doc, int token, char *out, size_t out_size,
                    bool *truncated);
/* Copies a value exactly as it appears (valid JSON), truncating if needed. */
bool bd_json_raw(const bd_json *doc, int token, char *out, size_t out_size,
                 bool *truncated);
bool bd_json_int(const bd_json *doc, int token, long *out);
bool bd_json_bool(const bd_json *doc, int token, bool *out);

/* 1-based line of a token in the source text, for diagnostics. */
int bd_json_line(const bd_json *doc, int token);

#endif /* BRICKDESK_SESSION_JSON_UTIL_H */
