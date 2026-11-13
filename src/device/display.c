#define _POSIX_C_SOURCE 200809L

#include "brickdesk/device_display.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BD_DISPLAY_GAMMA_ENTRIES 256
#define BD_DISPLAY_LCD_SET_GAMMA_TABLE 0x10b
#define BD_DISPLAY_LCD_GAMMA_ENABLE 0x10c
#define BD_DISPLAY_LCD_GAMMA_DISABLE 0x10d
#define BD_DISPLAY_GAMMA_SCREEN 0

static void set_error(bd_display_error *error, bd_display_result code,
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

const char *bd_display_result_name(bd_display_result result) {
  switch (result) {
    case BD_DISPLAY_OK:
      return "ok";
    case BD_DISPLAY_ERR_ARGUMENT:
      return "invalid-argument";
    case BD_DISPLAY_ERR_UNSUPPORTED:
      return "unsupported";
    case BD_DISPLAY_ERR_IO:
      return "io-error";
    case BD_DISPLAY_ERR_RANGE:
      return "out-of-range";
    case BD_DISPLAY_ERR_STATE:
      return "invalid-state";
  }
  return "unknown";
}

void bd_display_error_clear(bd_display_error *error) {
  if (error != NULL) {
    error->code = BD_DISPLAY_OK;
    error->message[0] = '\0';
  }
}

static bool path_join(const char *root, const char *path, char *out,
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

static bool write_text(const char *path, const char *text) {
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

static bool read_text(const char *path, char *out, size_t out_size) {
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
  fclose(file);
  while (used > 0 && (out[used - 1] == '\n' || out[used - 1] == '\r' ||
                      out[used - 1] == ' ' || out[used - 1] == '\t')) {
    out[--used] = '\0';
  }
  return ok;
}

static bool write_attr(const bd_display *display, const char *name, int value,
                       bd_display_error *error) {
  char path[BD_DISPLAY_PATH_MAX];
  if (!path_join(display->attr_dir, name, path, sizeof(path))) {
    set_error(error, BD_DISPLAY_ERR_ARGUMENT, "display attribute is too long");
    return false;
  }
  char text[32];
  snprintf(text, sizeof(text), "%d\n", value);
  if (!write_text(path, text)) {
    set_error(error, BD_DISPLAY_ERR_IO, "cannot write %s", path);
    return false;
  }
  return true;
}

static uint8_t clamp_u8(double value) {
  if (value < 0.0) {
    return 0;
  }
  if (value > 255.0) {
    return 255;
  }
  return (uint8_t)(value + 0.5);
}

static double srgb_to_linear(double value) {
  if (value <= 0.04045) {
    return value / 12.92;
  }
  return pow((value + 0.055) / 1.055, 2.4);
}

static double linear_to_srgb(double value) {
  if (value <= 0.0) {
    return 0.0;
  }
  if (value <= 0.0031308) {
    return value * 12.92;
  }
  return 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}

static void fill_gamma_table(uint32_t table[BD_DISPLAY_GAMMA_ENTRIES],
                             int red_gain, int green_gain, int blue_gain) {
  double r = (double)red_gain / 100.0;
  double g = (double)green_gain / 100.0;
  double b = (double)blue_gain / 100.0;
  for (int i = 0; i < BD_DISPLAY_GAMMA_ENTRIES; ++i) {
    double linear = srgb_to_linear((double)i / 255.0);
    uint32_t rr = clamp_u8(linear_to_srgb(linear * r) * 255.0);
    uint32_t gg = clamp_u8(linear_to_srgb(linear * g) * 255.0);
    uint32_t bb = clamp_u8(linear_to_srgb(linear * b) * 255.0);
    table[i] = (rr << 16) | (gg << 8) | bb;
  }
}

static int open_disp(const bd_display *display) {
  return open(display->disp_path, O_RDWR | O_CLOEXEC);
}

static bool apply_gamma(const bd_display *display, bool enabled,
                        int red_gain, int green_gain, int blue_gain,
                        char *error, size_t error_size) {
  int fd = open_disp(display);
  if (fd < 0) {
    snprintf(error, error_size, "cannot open %.150s: %.50s",
             display->disp_path, strerror(errno));
    return false;
  }
  uint32_t table[BD_DISPLAY_GAMMA_ENTRIES];
  fill_gamma_table(table, red_gain, green_gain, blue_gain);
  unsigned long set_param[4] = {
      (unsigned long)BD_DISPLAY_GAMMA_SCREEN,
      (unsigned long)(uintptr_t)table,
      (unsigned long)(BD_DISPLAY_GAMMA_ENTRIES * sizeof(uint32_t)),
      0,
  };
  if (ioctl(fd, BD_DISPLAY_LCD_SET_GAMMA_TABLE, set_param) < 0) {
    snprintf(error, error_size, "DISP_LCD_SET_GAMMA_TABLE failed: %s",
             strerror(errno));
    close(fd);
    return false;
  }
  unsigned long mode_param[4] = {
      (unsigned long)BD_DISPLAY_GAMMA_SCREEN, 0, 0, 0};
  int request =
      enabled ? BD_DISPLAY_LCD_GAMMA_ENABLE : BD_DISPLAY_LCD_GAMMA_DISABLE;
  if (ioctl(fd, request, mode_param) < 0) {
    snprintf(error, error_size, "display gamma %s failed: %s",
             enabled ? "enable" : "disable", strerror(errno));
    close(fd);
    return false;
  }
  close(fd);
  return true;
}

bd_display_result bd_display_probe(bd_display *display,
                                   const bd_display_config *config,
                                   bd_display_error *error) {
  bd_display_error_clear(error);
  if (display == NULL) {
    set_error(error, BD_DISPLAY_ERR_ARGUMENT, "display is NULL");
    return BD_DISPLAY_ERR_ARGUMENT;
  }
  memset(display, 0, sizeof(*display));
  const char *root = config != NULL && config->sysfs_root != NULL
                         ? config->sysfs_root
                         : "";
  const char *disp_path = config != NULL && config->disp_path != NULL
                              ? config->disp_path
                              : "/dev/disp";
  snprintf(display->root, sizeof(display->root), "%s", root);
  snprintf(display->disp_path, sizeof(display->disp_path), "%s", disp_path);
  if (!path_join(root, "/sys/class/disp/disp/attr", display->attr_dir,
                 sizeof(display->attr_dir)) ||
      access(display->attr_dir, F_OK) != 0) {
    set_error(error, BD_DISPLAY_ERR_UNSUPPORTED, "%s is not present",
              display->attr_dir);
    return BD_DISPLAY_ERR_UNSUPPORTED;
  }

  static const char *const REQUIRED[] = {
      "color_temperature", "enhance_contrast", "enhance_saturation",
      "enhance_bright"};
  for (size_t i = 0; i < sizeof(REQUIRED) / sizeof(REQUIRED[0]); ++i) {
    char path[BD_DISPLAY_PATH_MAX];
    if (!path_join(display->attr_dir, REQUIRED[i], path, sizeof(path)) ||
        access(path, W_OK) != 0) {
      set_error(error, BD_DISPLAY_ERR_UNSUPPORTED, "%s is not writable", path);
      return BD_DISPLAY_ERR_UNSUPPORTED;
    }
  }
  display->white_point_supported = access(disp_path, R_OK | W_OK) == 0;
  display->available = true;
  return BD_DISPLAY_OK;
}

bd_display_result bd_display_apply(bd_display *display,
                                   const bd_display_settings *settings,
                                   bd_display_error *error) {
  bd_display_error_clear(error);
  if (display == NULL || settings == NULL) {
    set_error(error, BD_DISPLAY_ERR_ARGUMENT,
              "display handle or settings is NULL");
    return BD_DISPLAY_ERR_ARGUMENT;
  }
  if (!display->available) {
    set_error(error, BD_DISPLAY_ERR_STATE,
              "display was not probed or is unavailable");
    return BD_DISPLAY_ERR_STATE;
  }
  if (settings->color_temperature < -200 ||
      settings->color_temperature > 200) {
    set_error(error, BD_DISPLAY_ERR_RANGE,
              "color temperature %d is outside -200..200",
              settings->color_temperature);
    return BD_DISPLAY_ERR_RANGE;
  }
  if (settings->contrast < 10 || settings->contrast > 100) {
    set_error(error, BD_DISPLAY_ERR_RANGE,
              "contrast %d is outside 10..100", settings->contrast);
    return BD_DISPLAY_ERR_RANGE;
  }
  if (settings->saturation < 0 || settings->saturation > 100) {
    set_error(error, BD_DISPLAY_ERR_RANGE,
              "saturation %d is outside 0..100", settings->saturation);
    return BD_DISPLAY_ERR_RANGE;
  }
  if (settings->exposure < 10 || settings->exposure > 100) {
    set_error(error, BD_DISPLAY_ERR_RANGE,
              "exposure %d is outside 10..100", settings->exposure);
    return BD_DISPLAY_ERR_RANGE;
  }
  if (settings->red_gain < 0 || settings->red_gain > 200 ||
      settings->green_gain < 0 || settings->green_gain > 200 ||
      settings->blue_gain < 0 || settings->blue_gain > 200) {
    set_error(error, BD_DISPLAY_ERR_RANGE,
              "white point gains must be 0..200");
    return BD_DISPLAY_ERR_RANGE;
  }

  if (!write_attr(display, "color_temperature", settings->color_temperature,
                  error) ||
      !write_attr(display, "enhance_contrast", settings->contrast, error) ||
      !write_attr(display, "enhance_saturation", settings->saturation, error) ||
      !write_attr(display, "enhance_bright", settings->exposure, error)) {
    return BD_DISPLAY_ERR_IO;
  }

  display->white_point_error[0] = '\0';
  if (!display->white_point_supported) {
    snprintf(display->white_point_error,
             sizeof(display->white_point_error),
             "%.200s is not accessible; white point skipped",
             display->disp_path);
    return BD_DISPLAY_OK;
  }
  if (!apply_gamma(display, settings->white_point, settings->red_gain,
                   settings->green_gain, settings->blue_gain,
                   display->white_point_error,
                   sizeof(display->white_point_error))) {
    /* The four vendor attributes succeeded; white point is an enhancement. */
    return BD_DISPLAY_OK;
  }
  return BD_DISPLAY_OK;
}

bool bd_display_dp_connected(const bd_display *display, bool *connected,
                             char *connector, size_t connector_size) {
  if (display == NULL || connected == NULL) {
    return false;
  }
  *connected = false;
  if (connector != NULL && connector_size > 0) {
    connector[0] = '\0';
  }
  char drm[BD_DISPLAY_PATH_MAX];
  if (!path_join(display->root, "/sys/class/drm", drm, sizeof(drm))) {
    return false;
  }
  DIR *dir = opendir(drm);
  if (dir == NULL) {
    return false;
  }
  bool found = false;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strstr(entry->d_name, "-DP-") == NULL) {
      continue;
    }
    char status_path[BD_DISPLAY_PATH_MAX];
    int written = snprintf(status_path, sizeof(status_path), "%s/%s/status",
                           drm, entry->d_name);
    if (written <= 0 || (size_t)written >= sizeof(status_path)) {
      continue;
    }
    char status[32];
    if (!read_text(status_path, status, sizeof(status))) {
      continue;
    }
    if (strcmp(status, "connected") == 0) {
      *connected = true;
      if (connector != NULL && connector_size > 0) {
        snprintf(connector, connector_size, "%s", entry->d_name);
      }
      found = true;
      break;
    }
    found = true;
  }
  closedir(dir);
  return found;
}
