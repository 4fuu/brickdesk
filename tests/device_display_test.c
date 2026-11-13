#define _POSIX_C_SOURCE 200809L

#include "brickdesk/device_display.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

#define CHECK(condition, ...)                          \
  do {                                                 \
    if (!(condition)) {                                \
      fprintf(stderr, "device_display_test:%d: ",      \
              __LINE__);                               \
      fprintf(stderr, __VA_ARGS__);                    \
      fprintf(stderr, "\n");                           \
      ++g_failures;                                    \
    }                                                  \
  } while (0)

static char *make_temp_dir(void) {
  char template[] = "/tmp/bd_display.XXXXXX";
  char *directory = mkdtemp(template);
  if (directory == NULL) {
    fprintf(stderr, "device_display_test: mkdtemp: %s\n", strerror(errno));
    exit(1);
  }
  return strdup(directory);
}

static void remove_temp_dir(char *directory) {
  if (directory == NULL) {
    return;
  }
  char command[512];
  snprintf(command, sizeof(command), "rm -rf '%s'", directory);
  int result = system(command);
  (void)result;
  free(directory);
}

static bool mkdir_p(const char *path) {
  char buffer[512];
  if (snprintf(buffer, sizeof(buffer), "%s", path) >= (int)sizeof(buffer)) {
    return false;
  }
  for (char *cursor = buffer + 1; *cursor != '\0'; ++cursor) {
    if (*cursor == '/') {
      *cursor = '\0';
      if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
        return false;
      }
      *cursor = '/';
    }
  }
  return mkdir(buffer, 0755) == 0 || errno == EEXIST;
}

static bool write_file(const char *path, const char *text) {
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

static bool write_fixture(const char *root, const char *relative,
                          const char *text) {
  char path[512];
  if (snprintf(path, sizeof(path), "%s/%s", root, relative) >=
      (int)sizeof(path)) {
    return false;
  }
  char parent[512];
  snprintf(parent, sizeof(parent), "%s", path);
  char *slash = strrchr(parent, '/');
  if (slash == NULL) {
    return false;
  }
  *slash = '\0';
  if (!mkdir_p(parent)) {
    return false;
  }
  return write_file(path, text);
}

static bool read_file(const char *root, const char *relative, char *out,
                      size_t out_size) {
  char path[512];
  if (snprintf(path, sizeof(path), "%s/%s", root, relative) >=
      (int)sizeof(path)) {
    return false;
  }
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  size_t used = fread(out, 1, out_size - 1, file);
  out[used] = '\0';
  bool ok = !ferror(file);
  fclose(file);
  while (used > 0 && (out[used - 1] == '\n' || out[used - 1] == '\r')) {
    out[--used] = '\0';
  }
  return ok;
}

static void test_display_attrs(void) {
  char *root = make_temp_dir();
  CHECK(write_fixture(root, "sys/class/disp/disp/attr/color_temperature", "0\n"),
        "create color_temperature");
  CHECK(write_fixture(root, "sys/class/disp/disp/attr/enhance_contrast", "50\n"),
        "create enhance_contrast");
  CHECK(write_fixture(root, "sys/class/disp/disp/attr/enhance_saturation",
                      "50\n"),
        "create enhance_saturation");
  CHECK(write_fixture(root, "sys/class/disp/disp/attr/enhance_bright", "50\n"),
        "create enhance_bright");

  bd_display display;
  bd_display_config config;
  memset(&config, 0, sizeof(config));
  config.sysfs_root = root;
  config.disp_path = "/nonexistent/disp";
  bd_display_error error;
  bd_display_error_clear(&error);
  CHECK(bd_display_probe(&display, &config, &error) == BD_DISPLAY_OK,
        "probe display: %s", error.message);
  CHECK(display.available, "display available");
  CHECK(!display.white_point_supported, "white point unsupported without disp");

  bd_display_settings settings;
  memset(&settings, 0, sizeof(settings));
  settings.color_temperature = -120;
  settings.contrast = 80;
  settings.saturation = 20;
  settings.exposure = 70;
  settings.white_point = true;
  settings.red_gain = 100;
  settings.green_gain = 94;
  settings.blue_gain = 54;
  CHECK(bd_display_apply(&display, &settings, &error) == BD_DISPLAY_OK,
        "apply display: %s", error.message);

  char text[32];
  CHECK(read_file(root, "sys/class/disp/disp/attr/color_temperature", text,
                  sizeof(text)) &&
            strcmp(text, "-120") == 0,
        "color_temperature=%s", text);
  CHECK(read_file(root, "sys/class/disp/disp/attr/enhance_contrast", text,
                  sizeof(text)) &&
            strcmp(text, "80") == 0,
        "contrast=%s", text);
  CHECK(read_file(root, "sys/class/disp/disp/attr/enhance_saturation", text,
                  sizeof(text)) &&
            strcmp(text, "20") == 0,
        "saturation=%s", text);
  CHECK(read_file(root, "sys/class/disp/disp/attr/enhance_bright", text,
                  sizeof(text)) &&
            strcmp(text, "70") == 0,
        "exposure=%s", text);

  settings.contrast = 9;
  CHECK(bd_display_apply(&display, &settings, &error) ==
            BD_DISPLAY_ERR_RANGE,
        "contrast range");
  remove_temp_dir(root);
}

static void test_dp_detection(void) {
  char *root = make_temp_dir();
  bd_display display;
  memset(&display, 0, sizeof(display));
  snprintf(display.root, sizeof(display.root), "%s", root);
  CHECK(write_fixture(root, "sys/class/drm/card0-DP-1/status", "connected\n"),
        "create DP status");
  bool connected = false;
  char connector[64];
  CHECK(bd_display_dp_connected(&display, &connected, connector,
                                sizeof(connector)),
        "find DP connector");
  CHECK(connected, "DP connected");
  CHECK(strcmp(connector, "card0-DP-1") == 0, "connector=%s", connector);

  CHECK(write_fixture(root, "sys/class/drm/card0-DP-1/status", "disconnected\n"),
        "update DP status");
  CHECK(bd_display_dp_connected(&display, &connected, connector,
                                sizeof(connector)),
        "find disconnected DP connector");
  CHECK(!connected, "DP disconnected");
  remove_temp_dir(root);
}

int main(void) {
  test_display_attrs();
  test_dp_detection();
  if (g_failures != 0) {
    fprintf(stderr, "device_display_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("device_display_test: all checks passed\n");
  return 0;
}
