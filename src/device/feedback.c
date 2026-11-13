#define _POSIX_C_SOURCE 200809L

#include "device_feedback_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BD_FEEDBACK_MAX_LEDS 64

static const char vendor_led_blocker[] =
    "vendor /sys/class/led_anim is probe-only: max_scale range (0..255 vs "
    "0..100), zone mapping (m/lr/f1f2/rear), effect ids and frame_hex "
    "semantics are not verified on TG4040; no write attempted";

static const char vendor_motor_blocker[] =
    "vendor /sys/class/motor/voltage + GPIO227 is probe-only: safe voltage "
    "range, polarity, export state and thermal limits are not verified on "
    "TG4040; no motor write attempted";

const char *bd_feedback_vendor_led_blocker(void) {
  return vendor_led_blocker;
}

const char *bd_feedback_vendor_motor_blocker(void) {
  return vendor_motor_blocker;
}

const char *bd_feedback_result_name(bd_feedback_result result) {
  switch (result) {
    case BD_FEEDBACK_OK:
      return "ok";
    case BD_FEEDBACK_ERR_ARGUMENT:
      return "invalid-argument";
    case BD_FEEDBACK_ERR_UNSUPPORTED:
      return "unsupported";
    case BD_FEEDBACK_ERR_IO:
      return "io-error";
    case BD_FEEDBACK_ERR_RANGE:
      return "out-of-range";
    case BD_FEEDBACK_ERR_STATE:
      return "invalid-state";
    default:
      return "unknown";
  }
}

const char *bd_feedback_led_backend_name(bd_feedback_led_backend backend) {
  switch (backend) {
    case BD_FEEDBACK_LED_AUTO:
      return "auto";
    case BD_FEEDBACK_LED_SYSFS:
      return "sysfs";
    case BD_FEEDBACK_LED_NONE:
      return "none";
    default:
      return "unknown";
  }
}

const char *bd_feedback_rumble_backend_name(
    bd_feedback_rumble_backend backend) {
  switch (backend) {
    case BD_FEEDBACK_RUMBLE_AUTO:
      return "auto";
    case BD_FEEDBACK_RUMBLE_EVDEV:
      return "evdev";
    case BD_FEEDBACK_RUMBLE_NONE:
      return "none";
    default:
      return "unknown";
  }
}

void bd_feedback_error_clear(bd_feedback_error *error) {
  if (error == NULL) {
    return;
  }
  error->code = BD_FEEDBACK_OK;
  error->message[0] = '\0';
}

void bd_feedback_set_error(bd_feedback_error *error, bd_feedback_result code,
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

bool bd_feedback_copy_string(char *dst, size_t dst_size, const char *src) {
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

void bd_feedback_trim(char *text) {
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

bool bd_feedback_parse_int(const char *text, int *out) {
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
  if (*end != '\0' || value < INT_MIN || value > INT_MAX) {
    return false;
  }
  *out = (int)value;
  return true;
}

bool bd_feedback_read_text(const char *path, char *out, size_t out_size) {
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
    bd_feedback_trim(out);
  }
  return ok;
}

bool bd_feedback_write_text(const char *path, const char *text) {
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

bool bd_feedback_path_join(const char *root, const char *path, char *out,
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

bool bd_feedback_file_exists(const char *path) {
  return path != NULL && access(path, F_OK) == 0;
}

bool bd_feedback_dir_exists(const char *path) {
  if (path == NULL) {
    return false;
  }
  struct stat info;
  return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool real_file_read(void *user, const char *path, char *out,
                           size_t out_size) {
  (void)user;
  return bd_feedback_read_text(path, out, out_size);
}

static bool real_file_write(void *user, const char *path, const char *text) {
  (void)user;
  return bd_feedback_write_text(path, text);
}

static bool real_file_writable(void *user, const char *path) {
  (void)user;
  return path != NULL && access(path, W_OK) == 0;
}

const bd_feedback_file_io *bd_feedback_file_io_default(void) {
  static const bd_feedback_file_io io = {
      .user = NULL,
      .read = real_file_read,
      .write = real_file_write,
      .writable = real_file_writable,
  };
  return &io;
}

static const char *config_root(const bd_feedback_led_config *config) {
  if (config == NULL || config->sysfs_root == NULL) {
    return "";
  }
  return config->sysfs_root;
}

static int config_max_percent(const bd_feedback_led_config *config) {
  if (config == NULL || config->max_percent == 0) {
    return 100;
  }
  return config->max_percent;
}

static void fill_vendor_led(bd_feedback_led *led, const char *root) {
  char directory[BD_FEEDBACK_PATH_MAX];
  led->vendor_led_anim_present = false;
  led->vendor_led_blocker[0] = '\0';
  if (!bd_feedback_path_join(root, "/sys/class/led_anim", directory,
                             sizeof(directory))) {
    return;
  }
  if (!bd_feedback_dir_exists(directory)) {
    return;
  }
  led->vendor_led_anim_present = true;
  bd_feedback_copy_string(led->vendor_led_blocker,
                          sizeof(led->vendor_led_blocker),
                          vendor_led_blocker);
}

static int raw_from_percent(int percent, int raw_max) {
  return (int)(((long long)percent * raw_max + 50) / 100);
}

static int percent_from_raw(int raw, int raw_max) {
  if (raw_max <= 0) {
    return 0;
  }
  return (int)(((long long)raw * 100 + raw_max / 2) / raw_max);
}

static bool active_trigger(const char *text, char *out, size_t out_size) {
  const char *open = strchr(text, '[');
  if (open != NULL) {
    const char *close = strchr(open + 1, ']');
    if (close == NULL || close == open + 1) {
      return false;
    }
    size_t length = (size_t)(close - open - 1);
    if (length >= out_size) {
      length = out_size - 1;
    }
    memcpy(out, open + 1, length);
    out[length] = '\0';
    bd_feedback_trim(out);
    return true;
  }
  if (!bd_feedback_copy_string(out, out_size, text)) {
    return false;
  }
  bd_feedback_trim(out);
  return out[0] != '\0';
}

static bd_feedback_result probe_led_directory(
    bd_feedback_led *led, const char *directory, int max_percent,
    const bd_feedback_file_io *file_io, bd_feedback_error *error) {
  char max_path[BD_FEEDBACK_PATH_MAX];
  char value_path[BD_FEEDBACK_PATH_MAX];
  char trigger_path[BD_FEEDBACK_PATH_MAX];
  if (!bd_feedback_path_join(directory, "max_brightness", max_path,
                             sizeof(max_path)) ||
      !bd_feedback_path_join(directory, "brightness", value_path,
                             sizeof(value_path)) ||
      !bd_feedback_path_join(directory, "trigger", trigger_path,
                             sizeof(trigger_path))) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "LED path is too long: %s", directory);
    return BD_FEEDBACK_ERR_ARGUMENT;
  }

  char text[BD_FEEDBACK_MESSAGE_MAX];
  int raw_max = 0;
  if (!file_io->read(file_io->user, max_path, text, sizeof(text)) ||
      !bd_feedback_parse_int(text, &raw_max) || raw_max <= 0) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                          "cannot read a positive max_brightness under %s",
                          directory);
    return BD_FEEDBACK_ERR_UNSUPPORTED;
  }

  int raw = 0;
  if (!file_io->read(file_io->user, value_path, text, sizeof(text)) ||
      !bd_feedback_parse_int(text, &raw)) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                          "cannot read brightness under %s", directory);
    return BD_FEEDBACK_ERR_UNSUPPORTED;
  }
  if (raw < 0 || raw > raw_max) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                          "brightness %d is outside 0..%d under %s", raw,
                          raw_max, directory);
    return BD_FEEDBACK_ERR_UNSUPPORTED;
  }

  if (file_io->read(file_io->user, trigger_path, text, sizeof(text))) {
    char trigger[BD_FEEDBACK_NAME_MAX];
    if (!active_trigger(text, trigger, sizeof(trigger))) {
      bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                            "cannot parse the LED trigger under %s",
                            directory);
      return BD_FEEDBACK_ERR_UNSUPPORTED;
    }
    if (strcmp(trigger, "none") != 0) {
      bd_feedback_set_error(
          error, BD_FEEDBACK_ERR_UNSUPPORTED,
          "LED %s has active trigger '%s'; set trigger to none first",
          directory, trigger);
      return BD_FEEDBACK_ERR_UNSUPPORTED;
    }
  } else if (bd_feedback_file_exists(trigger_path)) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                          "cannot read the LED trigger under %s", directory);
    return BD_FEEDBACK_ERR_UNSUPPORTED;
  }

  const char *base = strrchr(directory, '/');
  base = base == NULL ? directory : base + 1;
  if (!bd_feedback_copy_string(led->device, sizeof(led->device), directory) ||
      !bd_feedback_copy_string(led->name, sizeof(led->name), base)) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "LED path is too long: %s", directory);
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  led->backend = BD_FEEDBACK_LED_SYSFS;
  led->raw_min = 0;
  led->raw_max = raw_max;
  led->max_percent = max_percent;
  led->readable = true;
  led->writable = max_percent > 0 &&
                  file_io->writable(file_io->user, value_path);
  led->internal.file_io = file_io;
  return BD_FEEDBACK_OK;
}

static int compare_names(const void *left, const void *right) {
  const char *const *a = (const char *const *)left;
  const char *const *b = (const char *const *)right;
  return strcmp(*a, *b);
}

static bd_feedback_result probe_led_auto(
    bd_feedback_led *led, const char *root, int max_percent,
    const bd_feedback_file_io *file_io, bd_feedback_error *error) {
  char base[BD_FEEDBACK_PATH_MAX];
  if (!bd_feedback_path_join(root, "/sys/class/leds", base, sizeof(base))) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "sysfs root path is too long");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  DIR *dir = opendir(base);
  if (dir == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                          "no readable LED class at %s", base);
    return BD_FEEDBACK_ERR_UNSUPPORTED;
  }

  char names[BD_FEEDBACK_MAX_LEDS][NAME_MAX + 1];
  char *sorted[BD_FEEDBACK_MAX_LEDS];
  size_t count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL && count < BD_FEEDBACK_MAX_LEDS) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    if (!bd_feedback_copy_string(names[count], sizeof(names[count]),
                                 entry->d_name)) {
      continue;
    }
    sorted[count] = names[count];
    ++count;
  }
  closedir(dir);
  if (count == 0) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                          "no LED entries under %s", base);
    return BD_FEEDBACK_ERR_UNSUPPORTED;
  }
  qsort(sorted, count, sizeof(sorted[0]), compare_names);

  char last_detail[BD_FEEDBACK_MESSAGE_MAX] = "no usable LED";
  for (size_t i = 0; i < count; ++i) {
    char directory[BD_FEEDBACK_PATH_MAX];
    if (!bd_feedback_path_join(base, sorted[i], directory,
                               sizeof(directory))) {
      continue;
    }
    bd_feedback_led candidate;
    memset(&candidate, 0, sizeof(candidate));
    bd_feedback_error candidate_error;
    bd_feedback_error_clear(&candidate_error);
    bd_feedback_result result =
        probe_led_directory(&candidate, directory, max_percent, file_io,
                            &candidate_error);
    if (result == BD_FEEDBACK_OK) {
      *led = candidate;
      return BD_FEEDBACK_OK;
    }
    if (candidate_error.message[0] != '\0') {
      bd_feedback_copy_string(last_detail, sizeof(last_detail),
                              candidate_error.message);
    }
  }
  bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                        "no usable LED under %s (%s)", base, last_detail);
  return BD_FEEDBACK_ERR_UNSUPPORTED;
}

bd_feedback_result bd_feedback_led_probe_with_io(
    bd_feedback_led *led, const bd_feedback_led_config *config,
    const bd_feedback_file_io *file_io, bd_feedback_error *error) {
  bd_feedback_error_clear(error);
  if (led == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT, "led is NULL");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  memset(led, 0, sizeof(*led));
  led->backend = BD_FEEDBACK_LED_NONE;
  if (file_io == NULL) {
    file_io = bd_feedback_file_io_default();
  }
  const char *root = config_root(config);
  fill_vendor_led(led, root);

  int max_percent = config_max_percent(config);
  if (max_percent < 0 || max_percent > 100) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "LED max_percent must be 0..100");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  bd_feedback_led_backend backend =
      config == NULL ? BD_FEEDBACK_LED_AUTO : config->backend;
  if (backend != BD_FEEDBACK_LED_AUTO && backend != BD_FEEDBACK_LED_SYSFS &&
      backend != BD_FEEDBACK_LED_NONE) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "unknown LED backend");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  if (backend == BD_FEEDBACK_LED_NONE) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                          "LED disabled by configuration");
    return BD_FEEDBACK_ERR_UNSUPPORTED;
  }

  const char *explicit_led = config == NULL ? NULL : config->led;
  const char *name = config == NULL ? NULL : config->name;
  if (explicit_led != NULL && explicit_led[0] == '\0') {
    explicit_led = NULL;
  }
  if (name != NULL && name[0] == '\0') {
    name = NULL;
  }
  if (explicit_led != NULL && name != NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "configure LED path or name, not both");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  if (name != NULL &&
      (strchr(name, '/') != NULL || strcmp(name, ".") == 0 ||
       strcmp(name, "..") == 0)) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "LED name must be a plain class entry: %s", name);
    return BD_FEEDBACK_ERR_ARGUMENT;
  }

  if (explicit_led != NULL) {
    char directory[BD_FEEDBACK_PATH_MAX];
    if (!bd_feedback_path_join(root, explicit_led, directory,
                               sizeof(directory))) {
      bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                            "LED path is too long: %s", explicit_led);
      return BD_FEEDBACK_ERR_ARGUMENT;
    }
    bd_feedback_result result = probe_led_directory(
        led, directory, max_percent, file_io, error);
    if (result != BD_FEEDBACK_OK) {
      fill_vendor_led(led, root);
    }
    return result;
  }

  if (name != NULL) {
    char relative[BD_FEEDBACK_PATH_MAX];
    int written = snprintf(relative, sizeof(relative), "/sys/class/leds/%s",
                           name);
    if (written < 0 || (size_t)written >= sizeof(relative)) {
      bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                            "LED name is too long: %s", name);
      return BD_FEEDBACK_ERR_ARGUMENT;
    }
    char directory[BD_FEEDBACK_PATH_MAX];
    if (!bd_feedback_path_join(root, relative, directory, sizeof(directory))) {
      bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                            "LED path is too long: %s", name);
      return BD_FEEDBACK_ERR_ARGUMENT;
    }
    bd_feedback_result result = probe_led_directory(
        led, directory, max_percent, file_io, error);
    if (result != BD_FEEDBACK_OK) {
      fill_vendor_led(led, root);
    }
    return result;
  }

  if (config != NULL && config->allow_auto_discover) {
    bd_feedback_result result =
        probe_led_auto(led, root, max_percent, file_io, error);
    fill_vendor_led(led, root);
    return result;
  }

  bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                        "no LED selected; set led or name (auto-discovery "
                        "is opt-in)");
  return BD_FEEDBACK_ERR_UNSUPPORTED;
}

bd_feedback_result bd_feedback_led_probe(bd_feedback_led *led,
                                         const bd_feedback_led_config *config,
                                         bd_feedback_error *error) {
  return bd_feedback_led_probe_with_io(led, config,
                                       bd_feedback_file_io_default(), error);
}

static bd_feedback_result led_value_path(const bd_feedback_led *led,
                                         char *out, size_t out_size,
                                         bd_feedback_error *error) {
  if (!bd_feedback_path_join(led->device, "brightness", out, out_size)) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "LED path is too long");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  return BD_FEEDBACK_OK;
}

bd_feedback_result bd_feedback_led_get(const bd_feedback_led *led,
                                       int *percent, bd_feedback_error *error) {
  bd_feedback_error_clear(error);
  if (led == NULL || percent == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "led or percent is NULL");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  if (led->backend != BD_FEEDBACK_LED_SYSFS) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_STATE,
                          "LED backend is not initialised");
    return BD_FEEDBACK_ERR_STATE;
  }
  const bd_feedback_file_io *file_io =
      (const bd_feedback_file_io *)led->internal.file_io;
  if (file_io == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_STATE,
                          "LED file I/O is not initialised");
    return BD_FEEDBACK_ERR_STATE;
  }
  char path[BD_FEEDBACK_PATH_MAX];
  bd_feedback_result result = led_value_path(led, path, sizeof(path), error);
  if (result != BD_FEEDBACK_OK) {
    return result;
  }
  char text[BD_FEEDBACK_MESSAGE_MAX];
  int raw = 0;
  if (!file_io->read(file_io->user, path, text, sizeof(text)) ||
      !bd_feedback_parse_int(text, &raw)) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO, "cannot read %s", path);
    return BD_FEEDBACK_ERR_IO;
  }
  if (raw < led->raw_min || raw > led->raw_max) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO,
                          "%s returned %d outside %d..%d", path, raw,
                          led->raw_min, led->raw_max);
    return BD_FEEDBACK_ERR_IO;
  }
  *percent = percent_from_raw(raw, led->raw_max);
  return BD_FEEDBACK_OK;
}

bd_feedback_result bd_feedback_led_set(bd_feedback_led *led, int percent,
                                       bd_feedback_error *error) {
  bd_feedback_error_clear(error);
  if (led == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT, "led is NULL");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  if (led->backend != BD_FEEDBACK_LED_SYSFS) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_STATE,
                          "LED backend is not initialised");
    return BD_FEEDBACK_ERR_STATE;
  }
  if (percent < 0 || percent > 100) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_RANGE,
                          "LED percent %d is outside 0..100", percent);
    return BD_FEEDBACK_ERR_RANGE;
  }
  if (percent > led->max_percent) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_RANGE,
                          "LED percent %d exceeds configured maximum %d",
                          percent, led->max_percent);
    return BD_FEEDBACK_ERR_RANGE;
  }
  if (!led->writable) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                          "LED %s is read-only or disabled", led->device);
    return BD_FEEDBACK_ERR_UNSUPPORTED;
  }
  const bd_feedback_file_io *file_io =
      (const bd_feedback_file_io *)led->internal.file_io;
  if (file_io == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_STATE,
                          "LED file I/O is not initialised");
    return BD_FEEDBACK_ERR_STATE;
  }
  char path[BD_FEEDBACK_PATH_MAX];
  bd_feedback_result result = led_value_path(led, path, sizeof(path), error);
  if (result != BD_FEEDBACK_OK) {
    return result;
  }
  int raw = raw_from_percent(percent, led->raw_max);
  char text[32];
  snprintf(text, sizeof(text), "%d", raw);
  if (!file_io->write(file_io->user, path, text)) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO, "cannot write %s", path);
    return BD_FEEDBACK_ERR_IO;
  }
  char readback[BD_FEEDBACK_MESSAGE_MAX];
  int actual = 0;
  if (!file_io->read(file_io->user, path, readback, sizeof(readback)) ||
      !bd_feedback_parse_int(readback, &actual)) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO,
                          "cannot read back %s after writing %d", path, raw);
    return BD_FEEDBACK_ERR_IO;
  }
  if (actual != raw) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO,
                          "LED readback is %d after writing %d", actual, raw);
    return BD_FEEDBACK_ERR_IO;
  }
  return BD_FEEDBACK_OK;
}
