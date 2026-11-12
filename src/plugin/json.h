#ifndef BRICKDESK_PLUGIN_JSON_H
#define BRICKDESK_PLUGIN_JSON_H

#include <stdbool.h>
#include <stddef.h>

#ifndef JSMN_HEADER
#define JSMN_HEADER
#endif
#include <jsmn.h>

#define BD_PLUGIN_JSON_MAX_TOKENS 4096

typedef struct bd_plugin_json {
  const char *text;
  size_t length;
  jsmntok_t *tokens;
  int count;
} bd_plugin_json;

/*
 * Parses exactly one JSON value covering the whole input. Returns false and
 * fills error on syntax errors or when more than max_tokens tokens are
 * needed. The caller owns the bd_plugin_json and calls
 * bd_plugin_json_destroy.
 */
bool bd_plugin_json_parse(const char *text, size_t length,
                          bd_plugin_json *out, int max_tokens, char *error,
                          size_t error_size);
void bd_plugin_json_destroy(bd_plugin_json *json);

/* Next token after the subtree rooted at token, or NULL at the end. */
const jsmntok_t *bd_plugin_json_skip(const bd_plugin_json *json,
                                     const jsmntok_t *token);
/* Object member value for key, or NULL. */
const jsmntok_t *bd_plugin_json_member(const bd_plugin_json *json,
                                       const jsmntok_t *object,
                                       const char *key);
bool bd_plugin_json_is_string(const jsmntok_t *token);
bool bd_plugin_json_is_object(const jsmntok_t *token);
bool bd_plugin_json_is_array(const jsmntok_t *token);
bool bd_plugin_json_is_number(const jsmntok_t *token);
/* Copies and unescapes a string token. Rejects embedded NUL and overflow. */
bool bd_plugin_json_string(const bd_plugin_json *json,
                           const jsmntok_t *token, char *out, size_t out_size);
bool bd_plugin_json_int(const bd_plugin_json *json, const jsmntok_t *token,
                        int *out);
bool bd_plugin_json_long(const bd_plugin_json *json, const jsmntok_t *token,
                         long *out);
bool bd_plugin_json_float(const bd_plugin_json *json, const jsmntok_t *token,
                          float *out);
bool bd_plugin_json_bool(const bd_plugin_json *json, const jsmntok_t *token,
                         bool *out);

#endif /* BRICKDESK_PLUGIN_JSON_H */
