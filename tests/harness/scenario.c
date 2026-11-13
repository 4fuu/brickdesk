#define JSMN_STATIC
#include "jsmn.h"

#include "scenario.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BD_SCENARIO_MAX_JSON_TOKENS 4096
#define BD_SCENARIO_MAX_FILE_BYTES (1024 * 1024)

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
}

static bool token_equals(const char *json, const jsmntok_t *token, const char *text) {
  size_t length = (size_t)(token->end - token->start);
  return token->type == JSMN_STRING && strlen(text) == length &&
         memcmp(json + token->start, text, length) == 0;
}

static bool token_to_int(const char *json, const jsmntok_t *token, int *out) {
  if (token->type != JSMN_PRIMITIVE) {
    return false;
  }
  char buffer[32];
  size_t length = (size_t)(token->end - token->start);
  if (length == 0 || length >= sizeof(buffer)) {
    return false;
  }
  memcpy(buffer, json + token->start, length);
  buffer[length] = '\0';

  char *end = NULL;
  long value = strtol(buffer, &end, 10);
  if (end == buffer || *end != '\0') {
    return false;
  }
  if (value < -2147483647L || value > 2147483647L) {
    return false;
  }
  *out = (int)value;
  return true;
}

static bool token_to_string(const char *json, const jsmntok_t *token, char *out,
                            size_t out_size) {
  if (token->type != JSMN_STRING) {
    return false;
  }
  size_t length = (size_t)(token->end - token->start);
  if (length == 0 || length >= out_size || memchr(json + token->start, '\\', length) != NULL) {
    return false;
  }
  memcpy(out, json + token->start, length);
  out[length] = '\0';
  return true;
}

/* Returns the token index just past the value at index. */
static int skip_token(const jsmntok_t *tokens, int index) {
  int end = index + 1;
  if (tokens[index].type == JSMN_OBJECT) {
    /* jsmn counts object size in key/value pairs, not child tokens. */
    for (int pair = 0; pair < tokens[index].size; ++pair) {
      end = skip_token(tokens, end);       /* key */
      end = skip_token(tokens, end);       /* value */
    }
  } else if (tokens[index].type == JSMN_ARRAY) {
    for (int element = 0; element < tokens[index].size; ++element) {
      end = skip_token(tokens, end);
    }
  }
  return end;
}

static bool safe_filename(const char *name) {
  if (name == NULL || name[0] == '\0') {
    return false;
  }
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    return false;
  }
  for (const char *p = name; *p != '\0'; ++p) {
    const char c = *p;
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (!allowed) {
      return false;
    }
  }
  return true;
}

static char *read_file(const char *path, size_t *length, char *error,
                       size_t error_size) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    set_error(error, error_size, "cannot open scenario file '%s'", path);
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    set_error(error, error_size, "cannot seek scenario file '%s'", path);
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size <= 0 || size > BD_SCENARIO_MAX_FILE_BYTES) {
    set_error(error, error_size, "scenario file size %ld is invalid (max %d bytes)",
              size, BD_SCENARIO_MAX_FILE_BYTES);
    fclose(file);
    return NULL;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    set_error(error, error_size, "cannot rewind scenario file '%s'", path);
    fclose(file);
    return NULL;
  }

  char *buffer = (char *)malloc((size_t)size + 1u);
  if (buffer == NULL) {
    set_error(error, error_size, "out of memory reading scenario");
    fclose(file);
    return NULL;
  }
  size_t read = fread(buffer, 1, (size_t)size, file);
  fclose(file);
  if (read != (size_t)size) {
    set_error(error, error_size, "short read on scenario file '%s'", path);
    free(buffer);
    return NULL;
  }
  buffer[size] = '\0';
  *length = (size_t)size;
  return buffer;
}

static bool parse_step(const char *json, const jsmntok_t *tokens, int object_index,
                       int step_index, bd_scenario *scenario, char *error,
                       size_t error_size) {
  bd_step step;
  memset(&step, 0, sizeof(step));

  char type[32] = {0};
  bool has_type = false;
  bool has_frames = false;
  bool has_ms = false;
  bool has_key = false;
  bool has_action = false;
  bool has_name = false;
  int frames = 0;
  int milliseconds = 0;
  char key_name[32] = {0};
  char action[16] = {0};

  int child = object_index + 1;
  for (int pair = 0; pair < tokens[object_index].size; ++pair) {
    const jsmntok_t *key = &tokens[child];
    const jsmntok_t *value = &tokens[child + 1];

    if (token_equals(json, key, "type")) {
      if (!token_to_string(json, value, type, sizeof(type))) {
        set_error(error, error_size, "step %d: 'type' must be a string", step_index);
        return false;
      }
      has_type = true;
    } else if (token_equals(json, key, "frames")) {
      if (!token_to_int(json, value, &frames) || frames < 0) {
        set_error(error, error_size, "step %d: 'frames' must be a non-negative integer",
                  step_index);
        return false;
      }
      has_frames = true;
    } else if (token_equals(json, key, "ms")) {
      if (!token_to_int(json, value, &milliseconds) || milliseconds < 0) {
        set_error(error, error_size, "step %d: 'ms' must be a non-negative integer",
                  step_index);
        return false;
      }
      has_ms = true;
    } else if (token_equals(json, key, "key")) {
      if (!token_to_string(json, value, key_name, sizeof(key_name))) {
        set_error(error, error_size, "step %d: 'key' must be a string", step_index);
        return false;
      }
      has_key = true;
    } else if (token_equals(json, key, "action")) {
      if (!token_to_string(json, value, action, sizeof(action))) {
        set_error(error, error_size, "step %d: 'action' must be a string", step_index);
        return false;
      }
      has_action = true;
    } else if (token_equals(json, key, "name")) {
      if (!token_to_string(json, value, step.screenshot, sizeof(step.screenshot))) {
        set_error(error, error_size, "step %d: 'name' must be a string", step_index);
        return false;
      }
      has_name = true;
    }

    child = skip_token(tokens, child + 1);
  }

  if (!has_type) {
    set_error(error, error_size, "step %d: missing 'type'", step_index);
    return false;
  }

  if (strcmp(type, "wait") == 0) {
    if (has_frames && has_ms) {
      set_error(error, error_size, "step %d: use either 'frames' or 'ms', not both",
                step_index);
      return false;
    }
    if (!has_frames && !has_ms) {
      set_error(error, error_size, "step %d: wait needs 'frames' or 'ms'", step_index);
      return false;
    }
    step.type = BD_STEP_WAIT;
    if (has_ms) {
      if (milliseconds > 1000000) {
        set_error(error, error_size, "step %d: 'ms' is too large", step_index);
        return false;
      }
      step.frames = (milliseconds + scenario->fixed_dt_ms - 1) / scenario->fixed_dt_ms;
    } else {
      if (frames > 1000000) {
        set_error(error, error_size, "step %d: 'frames' is too large", step_index);
        return false;
      }
      step.frames = frames;
    }
  } else if (strcmp(type, "key") == 0) {
    if (!has_key || !has_action) {
      set_error(error, error_size, "step %d: key needs 'key' and 'action'", step_index);
      return false;
    }
    step.key = bd_key_from_name(key_name);
    if (step.key == BD_KEY_UNKNOWN) {
      set_error(error, error_size, "step %d: unknown key '%s'", step_index, key_name);
      return false;
    }
    if (strcmp(action, "down") == 0) {
      step.pressed = true;
    } else if (strcmp(action, "up") == 0) {
      step.pressed = false;
    } else {
      set_error(error, error_size, "step %d: action must be 'down' or 'up'", step_index);
      return false;
    }
    step.type = BD_STEP_KEY;
  } else if (strcmp(type, "run") == 0) {
    if (!has_ms) {
      set_error(error, error_size, "step %d: run needs 'ms'", step_index);
      return false;
    }
    if (milliseconds <= 0 || milliseconds > 600000) {
      set_error(error, error_size,
                "step %d: run 'ms' must be between 1 and 600000", step_index);
      return false;
    }
    step.type = BD_STEP_RUN;
    step.ms = milliseconds;
  } else if (strcmp(type, "screenshot") == 0) {
    if (!has_name) {
      set_error(error, error_size, "step %d: screenshot needs 'name'", step_index);
      return false;
    }
    if (!safe_filename(step.screenshot)) {
      set_error(error, error_size,
                "step %d: screenshot name '%s' is not a safe file name", step_index,
                step.screenshot);
      return false;
    }
    for (int i = 0; i < scenario->step_count; ++i) {
      if (scenario->steps[i].type == BD_STEP_SCREENSHOT &&
          strcmp(scenario->steps[i].screenshot, step.screenshot) == 0) {
        set_error(error, error_size, "step %d: duplicate screenshot name '%s'", step_index,
                  step.screenshot);
        return false;
      }
    }
    step.type = BD_STEP_SCREENSHOT;
  } else {
    set_error(error, error_size, "step %d: unknown type '%s'", step_index, type);
    return false;
  }

  if (scenario->step_count >= BD_SCENARIO_MAX_STEPS) {
    set_error(error, error_size, "too many steps (max %d)", BD_SCENARIO_MAX_STEPS);
    return false;
  }
  scenario->steps[scenario->step_count++] = step;
  return true;
}

bool bd_scenario_load(const char *path, bd_scenario *out, char *error,
                      size_t error_size) {
  if (path == NULL || out == NULL) {
    set_error(error, error_size, "invalid scenario arguments");
    return false;
  }

  memset(out, 0, sizeof(*out));
  out->width = 1024;
  out->height = 768;
  out->fixed_dt_ms = 16;

  size_t length = 0;
  char *json = read_file(path, &length, error, error_size);
  if (json == NULL) {
    return false;
  }

  static jsmntok_t tokens[BD_SCENARIO_MAX_JSON_TOKENS];
  jsmn_parser parser;
  jsmn_init(&parser);
  int token_count = jsmn_parse(&parser, json, length, tokens, BD_SCENARIO_MAX_JSON_TOKENS);
  if (token_count < 0) {
    set_error(error, error_size, "JSON parse failed (code %d) near byte %d", token_count,
              (int)parser.pos);
    free(json);
    return false;
  }
  if (token_count == 0 || tokens[0].type != JSMN_OBJECT) {
    set_error(error, error_size, "scenario root must be a JSON object");
    free(json);
    return false;
  }

  /* Pass 1: global properties, so field order does not matter. */
  int child = 1;
  for (int pair = 0; pair < tokens[0].size; ++pair) {
    const jsmntok_t *key = &tokens[child];
    const jsmntok_t *value = &tokens[child + 1];

    if (token_equals(json, key, "name")) {
      if (!token_to_string(json, value, out->name, sizeof(out->name))) {
        set_error(error, error_size, "'name' must be a string");
        free(json);
        return false;
      }
    } else if (token_equals(json, key, "size")) {
      if (value->type != JSMN_ARRAY || value->size != 2) {
        set_error(error, error_size, "'size' must be [width, height]");
        free(json);
        return false;
      }
      int width = 0;
      int height = 0;
      if (!token_to_int(json, &tokens[child + 2], &width) ||
          !token_to_int(json, &tokens[child + 3], &height) || width <= 0 || height <= 0 ||
          width > 8192 || height > 8192) {
        set_error(error, error_size, "'size' must contain two positive integers");
        free(json);
        return false;
      }
      out->width = width;
      out->height = height;
    } else if (token_equals(json, key, "fixed_dt_ms")) {
      if (!token_to_int(json, value, &out->fixed_dt_ms) || out->fixed_dt_ms <= 0 ||
          out->fixed_dt_ms > 1000) {
        set_error(error, error_size, "'fixed_dt_ms' must be between 1 and 1000");
        free(json);
        return false;
      }
    }
    child = skip_token(tokens, child + 1);
  }

  if (out->name[0] == '\0') {
    set_error(error, error_size, "missing scenario 'name'");
    free(json);
    return false;
  }
  if (!safe_filename(out->name)) {
    set_error(error, error_size, "scenario name '%s' is not a safe file name", out->name);
    free(json);
    return false;
  }

  /* Pass 2: steps, now with the final fixed_dt_ms. */
  child = 1;
  for (int pair = 0; pair < tokens[0].size; ++pair) {
    const jsmntok_t *key = &tokens[child];
    const jsmntok_t *value = &tokens[child + 1];

    if (token_equals(json, key, "steps")) {
      if (value->type != JSMN_ARRAY) {
        set_error(error, error_size, "'steps' must be an array");
        free(json);
        return false;
      }
      int step_token = child + 2;
      for (int step_index = 0; step_index < value->size; ++step_index) {
        if (tokens[step_token].type != JSMN_OBJECT) {
          set_error(error, error_size, "step %d must be an object", step_index);
          free(json);
          return false;
        }
        if (!parse_step(json, tokens, step_token, step_index, out, error, error_size)) {
          free(json);
          return false;
        }
        step_token = skip_token(tokens, step_token);
      }
    }
    child = skip_token(tokens, child + 1);
  }

  if (out->step_count == 0) {
    set_error(error, error_size, "scenario has no steps");
    free(json);
    return false;
  }

  free(json);
  return true;
}
