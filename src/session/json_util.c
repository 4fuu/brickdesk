#define JSMN_STATIC
#define JSMN_STRICT
#include "jsmn.h"

#include "json_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
}

static const jsmntok_t *token_at(const bd_json *doc, int index) {
  if (doc == NULL || doc->tokens == NULL || index < 0 ||
      index >= doc->token_count) {
    return NULL;
  }
  return &((const jsmntok_t *)doc->tokens)[index];
}

bool bd_json_parse(bd_json *doc, const char *text, size_t length, char *error,
                   size_t error_size) {
  if (doc == NULL || text == NULL) {
    set_error(error, error_size, "invalid JSON buffer");
    return false;
  }
  memset(doc, 0, sizeof(*doc));
  if (length == 0 || length > 16u * 1024u * 1024u) {
    set_error(error, error_size, "invalid JSON length");
    return false;
  }

  /* One token per byte is the worst case for jsmn. */
  size_t capacity = length + 1;
  jsmntok_t *tokens = (jsmntok_t *)calloc(capacity, sizeof(*tokens));
  if (tokens == NULL) {
    set_error(error, error_size, "out of memory parsing JSON");
    return false;
  }

  jsmn_parser parser;
  jsmn_init(&parser);
  int result = jsmn_parse(&parser, text, length, tokens,
                          (unsigned int)capacity);
  if (result < 0) {
    if (result == JSMN_ERROR_NOMEM) {
      set_error(error, error_size, "out of memory parsing JSON");
    } else if (result == JSMN_ERROR_PART) {
      set_error(error, error_size, "incomplete JSON");
    } else {
      set_error(error, error_size, "invalid JSON");
    }
    free(tokens);
    return false;
  }
  if (result == 0 || tokens[0].type != JSMN_OBJECT) {
    set_error(error, error_size, "JSON root must be an object");
    free(tokens);
    return false;
  }

  doc->text = text;
  doc->length = length;
  doc->tokens = tokens;
  doc->token_count = result;
  return true;
}

void bd_json_free(bd_json *doc) {
  if (doc == NULL) {
    return;
  }
  free(doc->tokens);
  memset(doc, 0, sizeof(*doc));
}

int bd_json_root(const bd_json *doc) {
  return token_at(doc, 0) != NULL ? 0 : -1;
}

bool bd_json_is_object(const bd_json *doc, int token) {
  const jsmntok_t *t = token_at(doc, token);
  return t != NULL && t->type == JSMN_OBJECT;
}

bool bd_json_is_array(const bd_json *doc, int token) {
  const jsmntok_t *t = token_at(doc, token);
  return t != NULL && t->type == JSMN_ARRAY;
}

bool bd_json_is_string(const bd_json *doc, int token) {
  const jsmntok_t *t = token_at(doc, token);
  return t != NULL && t->type == JSMN_STRING;
}

bool bd_json_is_primitive(const bd_json *doc, int token) {
  const jsmntok_t *t = token_at(doc, token);
  return t != NULL && t->type == JSMN_PRIMITIVE;
}

int bd_json_skip(const bd_json *doc, int token) {
  const jsmntok_t *t = token_at(doc, token);
  if (t == NULL) {
    return -1;
  }
  int end = token + 1;
  if (t->type == JSMN_OBJECT) {
    for (int pair = 0; pair < t->size; ++pair) {
      end = bd_json_skip(doc, end); /* key */
      if (end < 0) {
        return -1;
      }
      end = bd_json_skip(doc, end); /* value */
      if (end < 0) {
        return -1;
      }
    }
  } else if (t->type == JSMN_ARRAY) {
    for (int element = 0; element < t->size; ++element) {
      end = bd_json_skip(doc, end);
      if (end < 0) {
        return -1;
      }
    }
  }
  return end;
}

static bool token_equals(const bd_json *doc, int token, const char *text) {
  const jsmntok_t *t = token_at(doc, token);
  if (t == NULL || t->type != JSMN_STRING) {
    return false;
  }
  size_t length = (size_t)(t->end - t->start);
  return strlen(text) == length &&
         memcmp(doc->text + t->start, text, length) == 0;
}

int bd_json_object_get(const bd_json *doc, int object, const char *key) {
  const jsmntok_t *obj = token_at(doc, object);
  if (obj == NULL || obj->type != JSMN_OBJECT) {
    return -1;
  }
  int child = object + 1;
  for (int pair = 0; pair < obj->size; ++pair) {
    int value = bd_json_skip(doc, child);
    if (value < 0) {
      return -1;
    }
    if (token_equals(doc, child, key)) {
      return value;
    }
    child = bd_json_skip(doc, value);
    if (child < 0) {
      return -1;
    }
  }
  return -1;
}

int bd_json_array_size(const bd_json *doc, int array) {
  const jsmntok_t *t = token_at(doc, array);
  return t != NULL && t->type == JSMN_ARRAY ? t->size : -1;
}

int bd_json_array_at(const bd_json *doc, int array, int index) {
  const jsmntok_t *t = token_at(doc, array);
  if (t == NULL || t->type != JSMN_ARRAY || index < 0 || index >= t->size) {
    return -1;
  }
  int child = array + 1;
  for (int i = 0; i < index; ++i) {
    child = bd_json_skip(doc, child);
    if (child < 0) {
      return -1;
    }
  }
  return child;
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
  size_t count;
  if (codepoint <= 0x7f) {
    bytes[0] = (unsigned char)codepoint;
    count = 1;
  } else if (codepoint <= 0x7ff) {
    bytes[0] = (unsigned char)(0xc0 | (codepoint >> 6));
    bytes[1] = (unsigned char)(0x80 | (codepoint & 0x3f));
    count = 2;
  } else if (codepoint <= 0xffff) {
    bytes[0] = (unsigned char)(0xe0 | (codepoint >> 12));
    bytes[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
    bytes[2] = (unsigned char)(0x80 | (codepoint & 0x3f));
    count = 3;
  } else {
    bytes[0] = (unsigned char)(0xf0 | (codepoint >> 18));
    bytes[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3f));
    bytes[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
    bytes[3] = (unsigned char)(0x80 | (codepoint & 0x3f));
    count = 4;
  }
  for (size_t i = 0; i < count; ++i) {
    if (*used + 1 < out_size) {
      out[(*used)++] = (char)bytes[i];
    }
  }
  return true;
}

static bool copy_string(const bd_json *doc, const jsmntok_t *t, char *out,
                        size_t out_size, bool *truncated, bool raw) {
  if (out == NULL || out_size == 0) {
    return false;
  }
  size_t used = 0;
  bool cut = false;
  const char *start = doc->text + t->start;
  const char *end = doc->text + t->end;
  if (raw) {
    size_t length = (size_t)(end - start);
    if (length >= out_size) {
      length = out_size - 1;
      cut = true;
    }
    memcpy(out, start, length);
    out[length] = '\0';
    if (truncated != NULL) {
      *truncated = cut;
    }
    return true;
  }

  for (const char *p = start; p < end; ++p) {
    unsigned char c = (unsigned char)*p;
    if (c == '\\') {
      if (++p >= end) {
        return false;
      }
      switch (*p) {
        case '"': c = '"'; break;
        case '\\': c = '\\'; break;
        case '/': c = '/'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        case 'u': {
          if (end - p < 5) {
            return false;
          }
          int digits[4];
          for (int i = 0; i < 4; ++i) {
            digits[i] = hex_value(p[1 + i]);
            if (digits[i] < 0) {
              return false;
            }
          }
          unsigned int codepoint =
              ((unsigned int)digits[0] << 12) | ((unsigned int)digits[1] << 8) |
              ((unsigned int)digits[2] << 4) | (unsigned int)digits[3];
          p += 4;
          if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
            /* High surrogate; jsmn keeps the raw escape, so decode a pair. */
            if (end - p < 7 || p[1] != '\\' || p[2] != 'u') {
              codepoint = 0xfffd;
            } else {
              int low_digits[4];
              bool valid = true;
              for (int i = 0; i < 4; ++i) {
                low_digits[i] = hex_value(p[3 + i]);
                if (low_digits[i] < 0) {
                  valid = false;
                  break;
                }
              }
              if (!valid) {
                return false;
              }
              unsigned int low =
                  ((unsigned int)low_digits[0] << 12) |
                  ((unsigned int)low_digits[1] << 8) |
                  ((unsigned int)low_digits[2] << 4) |
                  (unsigned int)low_digits[3];
              if (low < 0xdc00 || low > 0xdfff) {
                codepoint = 0xfffd;
              } else {
                codepoint = 0x10000 + ((codepoint - 0xd800) << 10) +
                            (low - 0xdc00);
                p += 6;
              }
            }
          }
          append_utf8(out, out_size, &used, codepoint);
          continue;
        }
        default:
          return false;
      }
    }
    if (used + 1 < out_size) {
      out[used++] = (char)c;
    } else {
      cut = true;
    }
  }
  out[used] = '\0';
  if (truncated != NULL) {
    *truncated = cut;
  }
  return true;
}

bool bd_json_string(const bd_json *doc, int token, char *out, size_t out_size,
                    bool *truncated) {
  const jsmntok_t *t = token_at(doc, token);
  if (t == NULL || t->type != JSMN_STRING) {
    return false;
  }
  return copy_string(doc, t, out, out_size, truncated, false);
}

bool bd_json_raw(const bd_json *doc, int token, char *out, size_t out_size,
                 bool *truncated) {
  const jsmntok_t *t = token_at(doc, token);
  if (t == NULL) {
    return false;
  }
  return copy_string(doc, t, out, out_size, truncated, true);
}

bool bd_json_int(const bd_json *doc, int token, long *out) {
  const jsmntok_t *t = token_at(doc, token);
  if (t == NULL || t->type != JSMN_PRIMITIVE || out == NULL) {
    return false;
  }
  char buffer[32];
  size_t length = (size_t)(t->end - t->start);
  if (length == 0 || length >= sizeof(buffer)) {
    return false;
  }
  memcpy(buffer, doc->text + t->start, length);
  buffer[length] = '\0';
  char *end = NULL;
  long value = strtol(buffer, &end, 10);
  if (end == buffer || *end != '\0') {
    return false;
  }
  *out = value;
  return true;
}

bool bd_json_bool(const bd_json *doc, int token, bool *out) {
  const jsmntok_t *t = token_at(doc, token);
  if (t == NULL || t->type != JSMN_PRIMITIVE || out == NULL) {
    return false;
  }
  size_t length = (size_t)(t->end - t->start);
  if (length == 4 && memcmp(doc->text + t->start, "true", 4) == 0) {
    *out = true;
    return true;
  }
  if (length == 5 && memcmp(doc->text + t->start, "false", 5) == 0) {
    *out = false;
    return true;
  }
  return false;
}

int bd_json_line(const bd_json *doc, int token) {
  const jsmntok_t *t = token_at(doc, token);
  if (t == NULL || t->start < 0) {
    return 0;
  }
  int line = 1;
  for (int i = 0; i < t->start; ++i) {
    if (doc->text[i] == '\n') {
      ++line;
    }
  }
  return line;
}
