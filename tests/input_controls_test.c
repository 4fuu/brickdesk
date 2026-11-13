#define _POSIX_C_SOURCE 200809L

#include "brickdesk/input_controls.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

#define CHECK(condition, ...)                        \
  do {                                               \
    if (!(condition)) {                              \
      fprintf(stderr, "input_controls_test:%d: ",    \
              __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                  \
      fprintf(stderr, "\n");                         \
      ++g_failures;                                  \
    }                                                \
  } while (0)

static bool exists(const char *dir, const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  return access(path, F_OK) == 0;
}

int main(void) {
  char template[] = "/tmp/bd_input_controls.XXXXXX";
  char *dir = mkdtemp(template);
  if (dir == NULL) {
    fprintf(stderr, "input_controls_test: mkdtemp: %s\n", strerror(errno));
    return 1;
  }
  setenv("BRICKDESK_INPUTD_CONTROL_DIR", dir, 1);

  CHECK(bd_inputd_set_turbo(true), "enable turbo");
  CHECK(exists(dir, "turbo_a"), "turbo_a");
  CHECK(exists(dir, "turbo_b"), "turbo_b");
  CHECK(exists(dir, "turbo_x"), "turbo_x");
  CHECK(exists(dir, "turbo_y"), "turbo_y");
  CHECK(exists(dir, "turbo_l"), "turbo_l");
  CHECK(exists(dir, "turbo_l2"), "turbo_l2");
  CHECK(exists(dir, "turbo_r"), "turbo_r");
  CHECK(exists(dir, "turbo_r2"), "turbo_r2");
  CHECK(bd_inputd_turbo_enabled(), "turbo enabled");
  CHECK(bd_inputd_set_turbo(false), "disable turbo");
  CHECK(!exists(dir, "turbo_a"), "turbo_a removed");
  CHECK(!bd_inputd_turbo_enabled(), "turbo disabled");

  CHECK(bd_inputd_set_dpad_to_joystick(true), "enable dpad joystick");
  CHECK(exists(dir, "input_dpad_to_joystick"), "dpad joystick file");
  CHECK(bd_inputd_set_dpad_to_joystick(false), "disable dpad joystick");
  CHECK(!exists(dir, "input_dpad_to_joystick"), "dpad joystick removed");

  CHECK(bd_inputd_set_no_dpad(true), "enable no-dpad");
  CHECK(exists(dir, "input_no_dpad"), "no-dpad file");
  CHECK(bd_inputd_set_no_dpad(false), "disable no-dpad");
  CHECK(!exists(dir, "input_no_dpad"), "no-dpad removed");

  char command[512];
  snprintf(command, sizeof(command), "rm -rf '%s'", dir);
  int result = system(command);
  (void)result;
  if (g_failures != 0) {
    fprintf(stderr, "input_controls_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("input_controls_test: all checks passed\n");
  return 0;
}
