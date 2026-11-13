#define _POSIX_C_SOURCE 200809L

#include "device_av_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *bd_av_result_name(bd_av_result result) {
  switch (result) {
    case BD_AV_OK:
      return "ok";
    case BD_AV_ERR_ARGUMENT:
      return "invalid-argument";
    case BD_AV_ERR_UNSUPPORTED:
      return "unsupported";
    case BD_AV_ERR_IO:
      return "io-error";
    case BD_AV_ERR_RANGE:
      return "out-of-range";
    case BD_AV_ERR_STATE:
      return "invalid-state";
    default:
      return "unknown";
  }
}

const char *bd_av_brightness_backend_name(bd_av_brightness_backend backend) {
  switch (backend) {
    case BD_AV_BRIGHTNESS_AUTO:
      return "auto";
    case BD_AV_BRIGHTNESS_SYSFS:
      return "sysfs";
    case BD_AV_BRIGHTNESS_DISP:
      return "disp";
    case BD_AV_BRIGHTNESS_NONE:
      return "none";
    default:
      return "unknown";
  }
}

const char *bd_av_route_name(bd_av_route route) {
  switch (route) {
    case BD_AV_ROUTE_UNKNOWN:
      return "unknown";
    case BD_AV_ROUTE_NONE:
      return "none";
    case BD_AV_ROUTE_SPEAKER:
      return "speaker";
    case BD_AV_ROUTE_HEADPHONE:
      return "headphone";
    case BD_AV_ROUTE_BOTH:
      return "both";
    case BD_AV_ROUTE_AUTO:
      return "auto";
    default:
      return "unknown";
  }
}

const char *bd_av_control_type_name(bd_av_control_type type) {
  switch (type) {
    case BD_AV_CONTROL_BOOLEAN:
      return "boolean";
    case BD_AV_CONTROL_INTEGER:
      return "integer";
    case BD_AV_CONTROL_INTEGER64:
      return "integer64";
    case BD_AV_CONTROL_ENUMERATED:
      return "enumerated";
    case BD_AV_CONTROL_OTHER:
    default:
      return "other";
  }
}

void bd_av_error_clear(bd_av_error *error) {
  if (error == NULL) {
    return;
  }
  error->code = BD_AV_OK;
  error->message[0] = '\0';
}

void bd_av_set_error(bd_av_error *error, bd_av_result code,
                     const char *format, ...) {
  if (error == NULL) {
    return;
  }
  error->code = code;
  if (format == NULL) {
    error->message[0] = '\0';
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(error->message, sizeof(error->message), format, args);
  va_end(args);
}

bool bd_av_copy_string(char *dst, size_t dst_size, const char *src) {
  if (dst == NULL || dst_size == 0 || src == NULL) {
    if (dst != NULL && dst_size > 0) {
      dst[0] = '\0';
    }
    return false;
  }
  size_t length = strlen(src);
  if (length >= dst_size) {
    memcpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
    return false;
  }
  memcpy(dst, src, length + 1);
  return true;
}

void bd_av_trim(char *text) {
  if (text == NULL) {
    return;
  }
  size_t length = strlen(text);
  while (length > 0 && isspace((unsigned char)text[length - 1])) {
    text[--length] = '\0';
  }
  char *start = text;
  while (*start != '\0' && isspace((unsigned char)*start)) {
    ++start;
  }
  if (start != text) {
    memmove(text, start, strlen(start) + 1);
  }
}

bool bd_av_parse_long(const char *text, long *out) {
  if (text == NULL || out == NULL || text[0] == '\0') {
    return false;
  }
  errno = 0;
  char *end = NULL;
  long value = strtol(text, &end, 10);
  if (errno != 0 || end == text) {
    return false;
  }
  while (*end != '\0' && isspace((unsigned char)*end)) {
    ++end;
  }
  if (*end != '\0') {
    return false;
  }
  *out = value;
  return true;
}

bool bd_av_read_text(const char *path, char *out, size_t out_size) {
  if (path == NULL || out == NULL || out_size == 0) {
    return false;
  }
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  size_t used = fread(out, 1, out_size - 1, file);
  bool ok = !ferror(file);
  out[used] = '\0';
  if (fclose(file) != 0) {
    ok = false;
  }
  if (ok) {
    bd_av_trim(out);
  }
  return ok;
}

bool bd_av_write_text(const char *path, const char *text) {
  if (path == NULL || text == NULL) {
    return false;
  }
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs(text, file) >= 0;
  if (fclose(file) != 0) {
    ok = false;
  }
  return ok;
}

bool bd_av_path_join(const char *root, const char *path, char *out,
                     size_t out_size) {
  if (path == NULL || out == NULL || out_size == 0) {
    return false;
  }
  if (root == NULL || root[0] == '\0') {
    int written = snprintf(out, out_size, "%s", path);
    return written > 0 && (size_t)written < out_size;
  }
  size_t root_length = strlen(root);
  bool root_slash = root_length > 0 && root[root_length - 1] == '/';
  const char *suffix = path;
  int written;
  if (path[0] == '/') {
    if (root_slash) {
      ++suffix;
    }
    written = snprintf(out, out_size, "%s%s", root, suffix);
  } else {
    written = snprintf(out, out_size, root_slash ? "%s%s" : "%s/%s", root,
                       path);
  }
  return written > 0 && (size_t)written < out_size;
}

bool bd_av_file_exists(const char *path) {
  return path != NULL && access(path, F_OK) == 0;
}

bool bd_av_writable(const char *path) {
  return path != NULL && access(path, W_OK) == 0;
}
