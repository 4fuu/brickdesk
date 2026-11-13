#define _POSIX_C_SOURCE 200809L

#include "device_av_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BD_DISP_LCD_SET_BRIGHTNESS 0x102UL
#define BD_DISP_LCD_GET_BRIGHTNESS 0x103UL
#define BD_DISP_DEFAULT_RAW_MAX 255
#define BD_MAX_BACKLIGHTS 16
#define BD_DEFAULT_MODEL "TG4040"

static int real_disp_open(void *user, const char *path, int flags) {
  (void)user;
  return open(path, flags | O_CLOEXEC);
}

static long real_disp_ioctl(void *user, int fd, unsigned long request,
                            unsigned long *args) {
  (void)user;
  return ioctl(fd, request, args);
}

static int real_disp_close(void *user, int fd) {
  (void)user;
  return close(fd);
}

const bd_av_disp_io *bd_av_disp_io_default(void) {
  static const bd_av_disp_io io = {
      .user = NULL,
      .open = real_disp_open,
      .ioctl = real_disp_ioctl,
      .close = real_disp_close,
  };
  return &io;
}

static const char *config_root(const bd_av_brightness_config *config) {
  if (config == NULL || config->sysfs_root == NULL) {
    return "";
  }
  return config->sysfs_root;
}

static const char *config_disp_path(const bd_av_brightness_config *config) {
  if (config == NULL || config->disp_path == NULL) {
    return "/dev/disp";
  }
  return config->disp_path;
}

static const char *config_model_path(const bd_av_brightness_config *config) {
  if (config == NULL || config->model_path == NULL) {
    return "/etc/model";
  }
  return config->model_path;
}

static const char *config_allowed_models(
    const bd_av_brightness_config *config) {
  if (config == NULL || config->allowed_models == NULL) {
    return BD_DEFAULT_MODEL;
  }
  return config->allowed_models;
}

static int config_disp_raw_max(const bd_av_brightness_config *config) {
  if (config == NULL || config->disp_raw_max <= 0) {
    return BD_DISP_DEFAULT_RAW_MAX;
  }
  return config->disp_raw_max;
}

static bool model_is_allowed(const char *model, const char *allowed) {
  if (model == NULL || model[0] == '\0' || allowed == NULL) {
    return false;
  }
  size_t model_length = strlen(model);
  const char *cursor = allowed;
  while (*cursor != '\0') {
    while (*cursor == ',' || isspace((unsigned char)*cursor)) {
      ++cursor;
    }
    const char *start = cursor;
    while (*cursor != '\0' && *cursor != ',') {
      ++cursor;
    }
    const char *end = cursor;
    while (end > start && isspace((unsigned char)end[-1])) {
      --end;
    }
    if ((size_t)(end - start) == model_length &&
        memcmp(start, model, model_length) == 0) {
      return true;
    }
  }
  return false;
}

static bool read_int_file(const char *path, long *out) {
  char text[64];
  if (!bd_av_read_text(path, text, sizeof(text))) {
    return false;
  }
  return bd_av_parse_long(text, out);
}

static bool sysfs_backlight_valid(const char *directory, int *max_out,
                                  char *detail, size_t detail_size) {
  char max_path[BD_AV_PATH_MAX];
  char value_path[BD_AV_PATH_MAX];
  if (!bd_av_path_join(directory, "max_brightness", max_path,
                       sizeof(max_path)) ||
      !bd_av_path_join(directory, "brightness", value_path,
                       sizeof(value_path))) {
    snprintf(detail, detail_size, "path is too long");
    return false;
  }

  long max = 0;
  if (!read_int_file(max_path, &max) || max <= 0 || max > INT_MAX) {
    snprintf(detail, detail_size, "cannot read a positive max_brightness");
    return false;
  }
  long value = 0;
  if (!read_int_file(value_path, &value)) {
    snprintf(detail, detail_size, "cannot read brightness");
    return false;
  }
  if (value < 0 || value > max) {
    snprintf(detail, detail_size, "brightness %ld is outside 0..%ld", value,
             max);
    return false;
  }
  *max_out = (int)max;
  return true;
}

static bd_av_result probe_sysfs_directory(
    const char *directory, const bd_av_brightness_config *config,
    bd_av_brightness *brightness, bd_av_error *error) {
  int max = 0;
  char detail[BD_AV_MESSAGE_MAX];
  if (!sysfs_backlight_valid(directory, &max, detail, sizeof(detail))) {
    return BD_AV_ERR_UNSUPPORTED;
  }
  if (!bd_av_copy_string(brightness->device, sizeof(brightness->device),
                         directory)) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT,
                    "backlight path is too long: %s", directory);
    return BD_AV_ERR_ARGUMENT;
  }
  brightness->backend = BD_AV_BRIGHTNESS_SYSFS;
  brightness->raw_min = 0;
  brightness->raw_max = max;
  brightness->readable = true;
  char value_path[BD_AV_PATH_MAX];
  if (bd_av_path_join(directory, "brightness", value_path,
                      sizeof(value_path))) {
    brightness->writable = bd_av_writable(value_path);
  }
  brightness->internal.disp_io = NULL;

  char model[BD_AV_NAME_MAX];
  if (bd_av_read_text(config_model_path(config), model, sizeof(model))) {
    bd_av_copy_string(brightness->model, sizeof(brightness->model), model);
  }
  return BD_AV_OK;
}

static int compare_names(const void *left, const void *right) {
  const char *const *a = (const char *const *)left;
  const char *const *b = (const char *const *)right;
  return strcmp(*a, *b);
}

static bd_av_result probe_sysfs(const bd_av_brightness_config *config,
                                bd_av_brightness *brightness,
                                bd_av_error *error) {
  const char *root = config_root(config);
  const char *explicit_path = config == NULL ? NULL : config->backlight;
  if (explicit_path != NULL && explicit_path[0] != '\0') {
    char directory[BD_AV_PATH_MAX];
    if (!bd_av_path_join(root, explicit_path, directory, sizeof(directory))) {
      bd_av_set_error(error, BD_AV_ERR_ARGUMENT,
                      "backlight path is too long: %s", explicit_path);
      return BD_AV_ERR_ARGUMENT;
    }
    char detail[BD_AV_MESSAGE_MAX];
    int max = 0;
    if (!sysfs_backlight_valid(directory, &max, detail, sizeof(detail))) {
      bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED, "backlight %s: %s",
                      directory, detail);
      return BD_AV_ERR_UNSUPPORTED;
    }
    return probe_sysfs_directory(directory, config, brightness, error);
  }

  char base[BD_AV_PATH_MAX];
  if (!bd_av_path_join(root, "/sys/class/backlight", base, sizeof(base))) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "sysfs root path is too long");
    return BD_AV_ERR_ARGUMENT;
  }
  DIR *dir = opendir(base);
  if (dir == NULL) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "no readable backlight class at %s", base);
    return BD_AV_ERR_UNSUPPORTED;
  }

  char names[BD_MAX_BACKLIGHTS][NAME_MAX + 1];
  char *sorted[BD_MAX_BACKLIGHTS];
  size_t count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL && count < BD_MAX_BACKLIGHTS) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    if (!bd_av_copy_string(names[count], sizeof(names[count]),
                           entry->d_name)) {
      continue;
    }
    sorted[count] = names[count];
    ++count;
  }
  closedir(dir);
  if (count == 0) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "no backlight entries under %s", base);
    return BD_AV_ERR_UNSUPPORTED;
  }
  qsort(sorted, count, sizeof(sorted[0]), compare_names);

  char last_detail[BD_AV_MESSAGE_MAX] = "no usable backlight";
  for (size_t i = 0; i < count; ++i) {
    char directory[BD_AV_PATH_MAX];
    if (!bd_av_path_join(base, sorted[i], directory, sizeof(directory))) {
      continue;
    }
    int max = 0;
    char detail[BD_AV_MESSAGE_MAX];
    if (!sysfs_backlight_valid(directory, &max, detail, sizeof(detail))) {
      snprintf(last_detail, sizeof(last_detail), "%s", detail);
      continue;
    }
    return probe_sysfs_directory(directory, config, brightness, error);
  }
  bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED, "%s", last_detail);
  return BD_AV_ERR_UNSUPPORTED;
}

static bd_av_result probe_disp(const bd_av_brightness_config *config,
                               const bd_av_disp_io *io,
                               bd_av_brightness *brightness,
                               bd_av_error *error) {
  const char *model_path = config_model_path(config);
  char model[BD_AV_NAME_MAX];
  if (!bd_av_read_text(model_path, model, sizeof(model))) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "cannot identify device: cannot read %s", model_path);
    return BD_AV_ERR_UNSUPPORTED;
  }
  const char *allowed = config_allowed_models(config);
  if (!model_is_allowed(model, allowed)) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "device model '%s' is not in the /dev/disp allowlist '%s'",
                    model, allowed);
    return BD_AV_ERR_UNSUPPORTED;
  }

  const char *disp_path = config_disp_path(config);
  int fd = io->open(io->user, disp_path, O_RDWR);
  bool writable = fd >= 0;
  if (fd < 0) {
    fd = io->open(io->user, disp_path, O_RDONLY);
  }
  if (fd < 0) {
    if (bd_av_file_exists(disp_path)) {
      bd_av_set_error(error, BD_AV_ERR_IO, "cannot open %s: %s", disp_path,
                      strerror(errno));
      return BD_AV_ERR_IO;
    }
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED, "%s is not present",
                    disp_path);
    return BD_AV_ERR_UNSUPPORTED;
  }

  int raw_max = config_disp_raw_max(config);
  unsigned long args[4] = {0, 0, 0, 0};
  long value = io->ioctl(io->user, fd, BD_DISP_LCD_GET_BRIGHTNESS, args);
  int ioctl_errno = errno;
  io->close(io->user, fd);
  if (value < 0) {
    bd_av_set_error(
        error, BD_AV_ERR_UNSUPPORTED,
        "/dev/disp brightness get (0x103) failed: %s; refusing to write",
        strerror(ioctl_errno));
    return BD_AV_ERR_UNSUPPORTED;
  }
  if (value > raw_max) {
    bd_av_set_error(
        error, BD_AV_ERR_UNSUPPORTED,
        "/dev/disp brightness get returned %ld outside 0..%d; refusing to write",
        value, raw_max);
    return BD_AV_ERR_UNSUPPORTED;
  }

  if (!bd_av_copy_string(brightness->device, sizeof(brightness->device),
                         disp_path) ||
      !bd_av_copy_string(brightness->model, sizeof(brightness->model),
                         model)) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT,
                    "device model or path is too long");
    return BD_AV_ERR_ARGUMENT;
  }
  brightness->backend = BD_AV_BRIGHTNESS_DISP;
  brightness->raw_min = 0;
  brightness->raw_max = raw_max;
  brightness->readable = true;
  brightness->writable = writable;
  brightness->internal.disp_io = io;
  return BD_AV_OK;
}

bd_av_result bd_av_brightness_probe_with_io(
    bd_av_brightness *brightness, const bd_av_brightness_config *config,
    const bd_av_disp_io *disp_io, bd_av_error *error) {
  bd_av_error_clear(error);
  if (brightness == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "brightness is NULL");
    return BD_AV_ERR_ARGUMENT;
  }
  memset(brightness, 0, sizeof(*brightness));
  brightness->backend = BD_AV_BRIGHTNESS_NONE;
  if (disp_io == NULL) {
    disp_io = bd_av_disp_io_default();
  }
  bd_av_brightness_backend requested =
      config == NULL ? BD_AV_BRIGHTNESS_AUTO : config->backend;
  if (requested != BD_AV_BRIGHTNESS_AUTO &&
      requested != BD_AV_BRIGHTNESS_SYSFS &&
      requested != BD_AV_BRIGHTNESS_DISP) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "unknown brightness backend");
    return BD_AV_ERR_ARGUMENT;
  }

  if (requested == BD_AV_BRIGHTNESS_AUTO ||
      requested == BD_AV_BRIGHTNESS_SYSFS) {
    bd_av_brightness candidate;
    bd_av_error sysfs_error;
    bd_av_error_clear(&sysfs_error);
    bd_av_result result =
        probe_sysfs(config, &candidate, &sysfs_error);
    if (result == BD_AV_OK) {
      *brightness = candidate;
      return BD_AV_OK;
    }
    if (requested == BD_AV_BRIGHTNESS_SYSFS) {
      bd_av_set_error(error, result, "%s",
                      sysfs_error.message[0] != '\0' ? sysfs_error.message
                                                     : "no sysfs backlight");
      return result;
    }

    bd_av_brightness disp_candidate;
    bd_av_error disp_error;
    bd_av_error_clear(&disp_error);
    bd_av_result disp_result =
        probe_disp(config, disp_io, &disp_candidate, &disp_error);
    if (disp_result == BD_AV_OK) {
      *brightness = disp_candidate;
      return BD_AV_OK;
    }
    bd_av_set_error(error, disp_result,
                    "no brightness backend (sysfs: %s; disp: %s)",
                    sysfs_error.message[0] != '\0' ? sysfs_error.message
                                                   : "unavailable",
                    disp_error.message[0] != '\0' ? disp_error.message
                                                  : "unavailable");
    return disp_result;
  }

  return probe_disp(config, disp_io, brightness, error);
}

bd_av_result bd_av_brightness_probe(bd_av_brightness *brightness,
                                    const bd_av_brightness_config *config,
                                    bd_av_error *error) {
  return bd_av_brightness_probe_with_io(brightness, config,
                                        bd_av_disp_io_default(), error);
}

static bd_av_result brightness_read_raw_sysfs(
    const bd_av_brightness *brightness, int *raw, bd_av_error *error) {
  char path[BD_AV_PATH_MAX];
  if (!bd_av_path_join(brightness->device, "brightness", path,
                       sizeof(path))) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "backlight path is too long");
    return BD_AV_ERR_ARGUMENT;
  }
  long value = 0;
  if (!read_int_file(path, &value)) {
    bd_av_set_error(error, BD_AV_ERR_IO, "cannot read %s", path);
    return BD_AV_ERR_IO;
  }
  if (value < brightness->raw_min || value > brightness->raw_max) {
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "%s returned %ld outside %d..%d", path, value,
                    brightness->raw_min, brightness->raw_max);
    return BD_AV_ERR_IO;
  }
  *raw = (int)value;
  return BD_AV_OK;
}

static bd_av_result brightness_set_raw_sysfs(
    bd_av_brightness *brightness, int raw, bd_av_error *error) {
  char path[BD_AV_PATH_MAX];
  if (!bd_av_path_join(brightness->device, "brightness", path,
                       sizeof(path))) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "backlight path is too long");
    return BD_AV_ERR_ARGUMENT;
  }
  char text[32];
  snprintf(text, sizeof(text), "%d", raw);
  if (!bd_av_write_text(path, text)) {
    bd_av_set_error(error, BD_AV_ERR_IO, "cannot write %s", path);
    return BD_AV_ERR_IO;
  }
  int actual = 0;
  bd_av_result result =
      brightness_read_raw_sysfs(brightness, &actual, error);
  if (result != BD_AV_OK) {
    return result;
  }
  if (actual != raw) {
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "brightness readback is %d after writing %d", actual, raw);
    return BD_AV_ERR_IO;
  }
  return BD_AV_OK;
}

static bd_av_result brightness_read_raw_disp(
    const bd_av_brightness *brightness, int *raw, bd_av_error *error) {
  const bd_av_disp_io *io =
      (const bd_av_disp_io *)brightness->internal.disp_io;
  if (io == NULL) {
    bd_av_set_error(error, BD_AV_ERR_STATE, "disp backend is not initialised");
    return BD_AV_ERR_STATE;
  }
  int fd = io->open(io->user, brightness->device, O_RDWR);
  if (fd < 0) {
    fd = io->open(io->user, brightness->device, O_RDONLY);
  }
  if (fd < 0) {
    bd_av_set_error(error, BD_AV_ERR_IO, "cannot open %s: %s",
                    brightness->device, strerror(errno));
    return BD_AV_ERR_IO;
  }
  unsigned long args[4] = {0, 0, 0, 0};
  long value = io->ioctl(io->user, fd, BD_DISP_LCD_GET_BRIGHTNESS, args);
  int ioctl_errno = errno;
  io->close(io->user, fd);
  if (value < 0) {
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "/dev/disp brightness get (0x103) failed: %s",
                    strerror(ioctl_errno));
    return BD_AV_ERR_IO;
  }
  if (value < brightness->raw_min || value > brightness->raw_max) {
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "/dev/disp brightness get returned %ld outside %d..%d",
                    value, brightness->raw_min, brightness->raw_max);
    return BD_AV_ERR_IO;
  }
  *raw = (int)value;
  return BD_AV_OK;
}

static bd_av_result brightness_set_raw_disp(bd_av_brightness *brightness,
                                            int raw, bd_av_error *error) {
  const bd_av_disp_io *io =
      (const bd_av_disp_io *)brightness->internal.disp_io;
  if (io == NULL) {
    bd_av_set_error(error, BD_AV_ERR_STATE, "disp backend is not initialised");
    return BD_AV_ERR_STATE;
  }
  int fd = io->open(io->user, brightness->device, O_RDWR);
  if (fd < 0) {
    bd_av_set_error(error, BD_AV_ERR_IO, "cannot open %s for writing: %s",
                    brightness->device, strerror(errno));
    return BD_AV_ERR_IO;
  }
  unsigned long args[4] = {0, (unsigned long)raw, 0, 0};
  long result = io->ioctl(io->user, fd, BD_DISP_LCD_SET_BRIGHTNESS, args);
  int ioctl_errno = errno;
  io->close(io->user, fd);
  if (result < 0) {
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "/dev/disp brightness set (0x102) failed: %s",
                    strerror(ioctl_errno));
    return BD_AV_ERR_IO;
  }
  int actual = 0;
  bd_av_result read_result =
      brightness_read_raw_disp(brightness, &actual, error);
  if (read_result != BD_AV_OK) {
    return read_result;
  }
  if (actual != raw) {
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "brightness readback is %d after writing %d", actual, raw);
    return BD_AV_ERR_IO;
  }
  return BD_AV_OK;
}

bd_av_result bd_av_brightness_get_raw(const bd_av_brightness *brightness,
                                      int *raw, bd_av_error *error) {
  bd_av_error_clear(error);
  if (brightness == NULL || raw == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "brightness or raw is NULL");
    return BD_AV_ERR_ARGUMENT;
  }
  switch (brightness->backend) {
    case BD_AV_BRIGHTNESS_SYSFS:
      return brightness_read_raw_sysfs(brightness, raw, error);
    case BD_AV_BRIGHTNESS_DISP:
      return brightness_read_raw_disp(brightness, raw, error);
    default:
      bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                      "no brightness backend is available");
      return BD_AV_ERR_UNSUPPORTED;
  }
}

bd_av_result bd_av_brightness_set_raw(bd_av_brightness *brightness, int raw,
                                      bd_av_error *error) {
  bd_av_error_clear(error);
  if (brightness == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "brightness is NULL");
    return BD_AV_ERR_ARGUMENT;
  }
  if (brightness->backend == BD_AV_BRIGHTNESS_NONE) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "no brightness backend is available");
    return BD_AV_ERR_UNSUPPORTED;
  }
  if (brightness->backend != BD_AV_BRIGHTNESS_SYSFS &&
      brightness->backend != BD_AV_BRIGHTNESS_DISP) {
    bd_av_set_error(error, BD_AV_ERR_STATE,
                    "brightness backend is not initialised");
    return BD_AV_ERR_STATE;
  }
  if (raw < brightness->raw_min || raw > brightness->raw_max) {
    bd_av_set_error(error, BD_AV_ERR_RANGE, "brightness %d is outside %d..%d",
                    raw, brightness->raw_min, brightness->raw_max);
    return BD_AV_ERR_RANGE;
  }
  if (!brightness->writable) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "brightness backend is read-only");
    return BD_AV_ERR_UNSUPPORTED;
  }
  if (brightness->backend == BD_AV_BRIGHTNESS_SYSFS) {
    return brightness_set_raw_sysfs(brightness, raw, error);
  }
  return brightness_set_raw_disp(brightness, raw, error);
}

static int percent_to_raw(const bd_av_brightness *brightness, int percent) {
  long long span = (long long)brightness->raw_max - brightness->raw_min;
  long long offset = ((long long)percent * span + 50) / 100;
  return (int)(brightness->raw_min + offset);
}

static int raw_to_percent(const bd_av_brightness *brightness, int raw) {
  long long span = (long long)brightness->raw_max - brightness->raw_min;
  if (span <= 0) {
    return 0;
  }
  long long percent =
      ((long long)(raw - brightness->raw_min) * 100 + span / 2) / span;
  if (percent < 0) {
    percent = 0;
  }
  if (percent > 100) {
    percent = 100;
  }
  return (int)percent;
}

bd_av_result bd_av_brightness_get(const bd_av_brightness *brightness,
                                  int *percent, bd_av_error *error) {
  bd_av_error_clear(error);
  if (percent == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "percent is NULL");
    return BD_AV_ERR_ARGUMENT;
  }
  int raw = 0;
  bd_av_result result = bd_av_brightness_get_raw(brightness, &raw, error);
  if (result != BD_AV_OK) {
    return result;
  }
  *percent = raw_to_percent(brightness, raw);
  return BD_AV_OK;
}

bd_av_result bd_av_brightness_set(bd_av_brightness *brightness, int percent,
                                  bd_av_error *error) {
  bd_av_error_clear(error);
  if (brightness == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "brightness is NULL");
    return BD_AV_ERR_ARGUMENT;
  }
  if (percent < 0 || percent > 100) {
    bd_av_set_error(error, BD_AV_ERR_RANGE,
                    "brightness percent %d is outside 0..100", percent);
    return BD_AV_ERR_RANGE;
  }
  if (brightness->backend == BD_AV_BRIGHTNESS_NONE) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "no brightness backend is available");
    return BD_AV_ERR_UNSUPPORTED;
  }
  return bd_av_brightness_set_raw(brightness,
                                  percent_to_raw(brightness, percent), error);
}
