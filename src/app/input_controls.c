#define _POSIX_C_SOURCE 200809L

#include "brickdesk/input_controls.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BD_INPUTD_CONTROL_DIR "/tmp/trimui_inputd"
#define BD_INPUTD_PATH_MAX 512

static const char *control_dir(void) {
  const char *env = getenv("BRICKDESK_INPUTD_CONTROL_DIR");
  return env != NULL && env[0] != '\0' ? env : BD_INPUTD_CONTROL_DIR;
}

static bool ensure_dir(void) {
  const char *dir = control_dir();
  if (mkdir(dir, 0755) == 0 || errno == EEXIST) {
    return true;
  }
  return false;
}

static bool set_flag(const char *name, bool enabled) {
  char path[BD_INPUTD_PATH_MAX];
  int written = snprintf(path, sizeof(path), "%s/%s", control_dir(), name);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    return false;
  }
  if (!enabled) {
    if (unlink(path) == 0 || errno == ENOENT) {
      return true;
    }
    return false;
  }
  if (!ensure_dir()) {
    return false;
  }
  int fd = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
  if (fd < 0) {
    return false;
  }
  close(fd);
  return true;
}

static bool get_flag(const char *name) {
  char path[BD_INPUTD_PATH_MAX];
  int written = snprintf(path, sizeof(path), "%s/%s", control_dir(), name);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    return false;
  }
  return access(path, F_OK) == 0;
}

bool bd_inputd_set_dpad_to_joystick(bool enabled) {
  return set_flag("input_dpad_to_joystick", enabled);
}

bool bd_inputd_set_no_dpad(bool enabled) {
  return set_flag("input_no_dpad", enabled);
}

bool bd_inputd_set_turbo(bool enabled) {
  static const char *const FILES[] = {
      "turbo_a", "turbo_b", "turbo_x",  "turbo_y",
      "turbo_l", "turbo_l2", "turbo_r", "turbo_r2",
  };
  bool ok = true;
  for (size_t i = 0; i < sizeof(FILES) / sizeof(FILES[0]); ++i) {
    if (!set_flag(FILES[i], enabled)) {
      ok = false;
    }
  }
  return ok;
}

bool bd_inputd_turbo_enabled(void) {
  return get_flag("turbo_a");
}
