#define _POSIX_C_SOURCE 200809L

#include "json.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *error, size_t error_size, const char *message) {
  if (error != NULL && error_size > 0) {
    snprintf(error, error_size, "%s", message);
  }
}

bool bd_plugin_json_parse(const char *text, size_t length,
                          bd_plugin_json *out, int max_tokens, char *error,
                          size_t error_size) {
  if (out == NULL) {
    set_error(error, error_size, "missing JSON output");
    return false;
  }
  memset(out, 0, sizeof(*out));
  if (text == NULL || length == 0) {
    set_error(error, error_size, "empty JSON document");
    return false;
  }
  if (max_tokens <= 0 || max_tokens > BD_PLUGIN_JSON_MAX_TOKENS) {
    max_tokens = BD_PLUGIN_JSON_MAX_TOKENS;
  }
  jsmntok_t *tokens =
      (jsmntok_t *)calloc((size_t)max_tokens, sizeof(jsmntok_t));
  if (tokens == NULL) {
    set_error(error, error_size, "out of memory for JSON tokens");
    return false;
  }
  jsmn_parser parser;
  jsmn_init(&parser);
  int count = jsmn_parse(&parser, text, length, tokens, (unsigned int)max_tokens);
  if (count < 0) {
    free(tokens);
    if (count == JSMN_ERROR_NOMEM) {
      set_error(error, error_size, "JSON document has too many tokens");
    } else if (count == JSMN_ERROR_INVAL) {
      set_error(error, error_size, "invalid JSON");
    } else if (count == JSMN_ERROR_PART) {
      set_error(error, error_size, "truncated JSON");
    } else {
      set_error(error, error_size, "JSON parse failed");
    }
    return false;
  }
  if (count == 0 || tokens[0].type != JSMN_OBJECT) {
    free(tokens);
    set_error(error, error_size, "JSON root must be an object");
    return false;
  }
  out->text = text;
  out->length = length;
  out->tokens = tokens;
  out->count = count;
  return true;
}

void bd_plugin_json_destroy(bd_plugin_json *json) {
  if (json == NULL) {
    return;
  }
  free(json->tokens);
  memset(json, 0, sizeof(*json));
}

const jsmntok_t *bd_plugin_json_skip(const bd_plugin_json *json,
                                     const jsmntok_t *token) {
  if (json == NULL || token == NULL) {
    return NULL;
  }
  int index = (int)(token - json->tokens);
  int end = index + 1;
  if (token->type == JSMN_OBJECT) {
    for (int pair = 0; pair < token->size; ++pair) {
      if (end >= json->count) {
        return NULL;
      }
      end = (int)(bd_plugin_json_skip(json, &json->tokens[end]) - json->tokens);
      if (end >= json->count) {
        return NULL;
      }
      end = (int)(bd_plugin_json_skip(json, &json->tokens[end]) - json->tokens);
      if (end > json->count) {
        return NULL;
      }
    }
  } else if (token->type == JSMN_ARRAY) {
    for (int element = 0; element < token->size; ++element) {
      if (end >= json->count) {
        return NULL;
      }
      end = (int)(bd_plugin_json_skip(json, &json->tokens[end]) - json->tokens);
      if (end > json->count) {
        return NULL;
      }
    }
  }
  if (end > json->count) {
    return NULL;
  }
  return &json->tokens[end];
}

static bool token_equals(const bd_plugin_json *json, const jsmntok_t *token,
                         const char *key) {
  if (token == NULL || token->type != JSMN_STRING) {
    return false;
  }
  size_t length = (size_t)(token->end - token->start);
  return strlen(key) == length &&
         memcmp(json->text + token->start, key, length) == 0;
}

const jsmntok_t *bd_plugin_json_member(const bd_plugin_json *json,
                                       const jsmntok_t *object,
                                       const char *key) {
  if (json == NULL || object == NULL || object->type != JSMN_OBJECT ||
      key == NULL) {
    return NULL;
  }
  int index = (int)(object - json->tokens) + 1;
  for (int pair = 0; pair < object->size; ++pair) {
    if (index + 1 >= json->count) {
      return NULL;
    }
    const jsmntok_t *name = &json->tokens[index];
    const jsmntok_t *value = &json->tokens[index + 1];
    if (token_equals(json, name, key)) {
      return value;
    }
    const jsmntok_t *next = bd_plugin_json_skip(json, value);
    if (next == NULL) {
      return NULL;
    }
    index = (int)(next - json->tokens);
  }
  return NULL;
}

bool bd_plugin_json_is_string(const jsmntok_t *token) {
  return token != NULL && token->type == JSMN_STRING;
}

bool bd_plugin_json_is_object(const jsmntok_t *token) {
  return token != NULL && token->type == JSMN_OBJECT;
}

bool bd_plugin_json_is_array(const jsmntok_t *token) {
  return token != NULL && token->type == JSMN_ARRAY;
}

bool bd_plugin_json_is_number(const jsmntok_t *token) {
  return token != NULL && token->type == JSMN_PRIMITIVE;
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static bool append_utf8(char *out, size_t out_size, size_t *used,
                        unsigned int codepoint) {
  unsigned char bytes[4];
  int count = 0;
  if (codepoint <= 0x7fu) {
    bytes[count++] = (unsigned char)codepoint;
  } else if (codepoint <= 0x7ffu) {
    bytes[count++] = (unsigned char)(0xc0u | (codepoint >> 6));
    bytes[count++] = (unsigned char)(0x80u | (codepoint & 0x3fu));
  } else if (codepoint <= 0xffffu) {
    bytes[count++] = (unsigned char)(0xe0u | (codepoint >> 12));
    bytes[count++] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3fu));
    bytes[count++] = (unsigned char)(0x80u | (codepoint & 0x3fu));
  } else if (codepoint <= 0x10ffffu) {
    bytes[count++] = (unsigned char)(0xf0u | (codepoint >> 18));
    bytes[count++] = (unsigned char)(0x80u | ((codepoint >> 12) & 0x3fu));
    bytes[count++] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3fu));
    bytes[count++] = (unsigned char)(0x80u | (codepoint & 0x3fu));
  } else {
    return false;
  }
  if (*used + (size_t)count + 1 > out_size) {
    return false;
  }
  memcpy(out + *used, bytes, (size_t)count);
  *used += (size_t)count;
  return true;
}

bool bd_plugin_json_string(const bd_plugin_json *json,
                           const jsmntok_t *token, char *out,
                           size_t out_size) {
  if (json == NULL || token == NULL || token->type != JSMN_STRING ||
      out == NULL || out_size == 0) {
    return false;
  }
  size_t used = 0;
  const char *cursor = json->text + token->start;
  const char *end = json->text + token->end;
  while (cursor < end) {
    unsigned char c = (unsigned char)*cursor++;
    if (c == '\\') {
      if (cursor >= end) {
        return false;
      }
      char escape = *cursor++;
      switch (escape) {
        case '"':
        case '\\':
        case '/':
          c = (unsigned char)escape;
          break;
        case 'b':
          c = '\b';
          break;
        case 'f':
          c = '\f';
          break;
        case 'n':
          c = '\n';
          break;
        case 'r':
          c = '\r';
          break;
        case 't':
          c = '\t';
          break;
        case 'u': {
          if (end - cursor < 4) {
            return false;
          }
          unsigned int codepoint = 0;
          for (int i = 0; i < 4; ++i) {
            int value = hex_value(cursor[i]);
            if (value < 0) {
              return false;
            }
            codepoint = (codepoint << 4) | (unsigned int)value;
          }
          cursor += 4;
          if (codepoint == 0) {
            return false;
          }
          if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
            if (end - cursor < 6 || cursor[0] != '\\' || cursor[1] != 'u') {
              return false;
            }
            cursor += 2;
            unsigned int low = 0;
            for (int i = 0; i < 4; ++i) {
              int value = hex_value(cursor[i]);
              if (value < 0) {
                return false;
              }
              low = (low << 4) | (unsigned int)value;
            }
            cursor += 4;
            if (low < 0xdc00u || low > 0xdfffu) {
              return false;
            }
            codepoint =
                0x10000u + ((codepoint - 0xd800u) << 10) + (low - 0xdc00u);
          } else if (codepoint >= 0xdc00u && codepoint <= 0xdfffu) {
            return false;
          }
          if (!append_utf8(out, out_size, &used, codepoint)) {
            return false;
          }
          continue;
        }
        default:
          return false;
      }
      if (c == 0) {
        return false;
      }
    }
    if (used + 1 >= out_size) {
      return false;
    }
    out[used++] = (char)c;
  }
  out[used] = '\0';
  return true;
}

static bool token_text(const bd_plugin_json *json, const jsmntok_t *token,
                       char *buffer, size_t buffer_size) {
  if (json == NULL || token == NULL) {
    return false;
  }
  size_t length = (size_t)(token->end - token->start);
  if (length == 0 || length >= buffer_size) {
    return false;
  }
  memcpy(buffer, json->text + token->start, length);
  buffer[length] = '\0';
  return true;
}

bool bd_plugin_json_int(const bd_plugin_json *json, const jsmntok_t *token,
                        int *out) {
  long value = 0;
  if (!bd_plugin_json_long(json, token, &value)) {
    return false;
  }
  if (value < -2147483647L || value > 2147483647L) {
    return false;
  }
  *out = (int)value;
  return true;
}

bool bd_plugin_json_long(const bd_plugin_json *json, const jsmntok_t *token,
                         long *out) {
  char buffer[64];
  if (out == NULL || !token_text(json, token, buffer, sizeof(buffer))) {
    return false;
  }
  errno = 0;
  char *end = NULL;
  long value = strtol(buffer, &end, 10);
  if (errno != 0 || end == buffer || *end != '\0') {
    return false;
  }
  *out = value;
  return true;
}

bool bd_plugin_json_float(const bd_plugin_json *json, const jsmntok_t *token,
                          float *out) {
  char buffer[64];
  if (out == NULL || !token_text(json, token, buffer, sizeof(buffer))) {
    return false;
  }
  errno = 0;
  char *end = NULL;
  double value = strtod(buffer, &end);
  if (errno != 0 || end == buffer || *end != '\0' || !isfinite(value)) {
    return false;
  }
  if (value < -1.0e9 || value > 1.0e9) {
    return false;
  }
  *out = (float)value;
  return true;
}

bool bd_plugin_json_bool(const bd_plugin_json *json, const jsmntok_t *token,
                         bool *out) {
  if (json == NULL || token == NULL || token->type != JSMN_PRIMITIVE ||
      out == NULL) {
    return false;
  }
  size_t length = (size_t)(token->end - token->start);
  if (length == 4 && memcmp(json->text + token->start, "true", 4) == 0) {
    *out = true;
    return true;
  }
  if (length == 5 && memcmp(json->text + token->start, "false", 5) == 0) {
    *out = false;
    return true;
  }
  return false;
}
