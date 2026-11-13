#define _POSIX_C_SOURCE 200809L

#include "brickdesk/device_feedback.h"
#include "bd-feedback-play.h"
#include "device_feedback_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int g_failures;
static volatile sig_atomic_t g_alarm_stop;

static void test_alarm_handler(int signum) {
  (void)signum;
  g_alarm_stop = 1;
}

#define CHECK(condition, ...)                             \
  do {                                                    \
    if (!(condition)) {                                   \
      fprintf(stderr, "device_feedback_test:%d: ",        \
              __LINE__);                                  \
      fprintf(stderr, __VA_ARGS__);                       \
      fprintf(stderr, "\n");                              \
      ++g_failures;                                       \
    }                                                     \
  } while (0)

static void check_result(bd_feedback_result actual,
                         bd_feedback_result expected,
                         const char *expression, int line) {
  if (actual != expected) {
    fprintf(stderr,
            "device_feedback_test:%d: %s returned %s, expected %s\n", line,
            expression, bd_feedback_result_name(actual),
            bd_feedback_result_name(expected));
    ++g_failures;
  }
}

#define CHECK_RESULT(expression, expected) \
  check_result((expression), (expected), #expression, __LINE__)

static char *make_temp_dir(void) {
  char template[] = "/tmp/bd_feedback.XXXXXX";
  char *directory = mkdtemp(template);
  if (directory == NULL) {
    fprintf(stderr, "device_feedback_test: mkdtemp: %s\n", strerror(errno));
    exit(1);
  }
  char *copy = strdup(directory);
  if (copy == NULL) {
    exit(1);
  }
  return copy;
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

static bool join_path(char *out, size_t out_size, const char *base,
                      const char *relative) {
  int written = snprintf(out, out_size, "%s/%s", base, relative);
  return written > 0 && (size_t)written < out_size;
}

static bool mkdir_p(const char *path) {
  char buffer[512];
  int written = snprintf(buffer, sizeof(buffer), "%s", path);
  if (written <= 0 || (size_t)written >= sizeof(buffer)) {
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
  if (!join_path(path, sizeof(path), root, relative)) {
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

static bool read_file(const char *path, char *out, size_t out_size) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  size_t used = fread(out, 1, out_size - 1, file);
  out[used] = '\0';
  bool ok = !ferror(file);
  fclose(file);
  if (ok) {
    bd_feedback_trim(out);
  }
  return ok;
}

static bool ends_with(const char *text, const char *suffix) {
  size_t text_length = strlen(text);
  size_t suffix_length = strlen(suffix);
  return text_length >= suffix_length &&
         strcmp(text + text_length - suffix_length, suffix) == 0;
}

/* ---------------------------------------------------------------------- */
/* Fake file I/O for failure paths                                        */
/* ---------------------------------------------------------------------- */

typedef struct fake_files {
  char brightness[32];
  bool has_trigger;
  char trigger[64];
  bool writable;
  bool write_fail;
  bool read_fail;
  bool mismatch;
  int write_calls;
  int read_calls;
} fake_files;

static void fake_files_init(fake_files *files) {
  memset(files, 0, sizeof(*files));
  strcpy(files->brightness, "10");
  strcpy(files->trigger, "[none] timer");
  files->writable = true;
}

static bool fake_file_read(void *user, const char *path, char *out,
                           size_t out_size) {
  fake_files *files = (fake_files *)user;
  ++files->read_calls;
  if (files->read_fail) {
    return false;
  }
  const char *value = NULL;
  if (ends_with(path, "/max_brightness")) {
    value = "255";
  } else if (ends_with(path, "/brightness")) {
    value = files->brightness;
  } else if (ends_with(path, "/trigger") && files->has_trigger) {
    value = files->trigger;
  }
  if (value == NULL) {
    return false;
  }
  int written = snprintf(out, out_size, "%s", value);
  return written >= 0 && (size_t)written < out_size;
}

static bool fake_file_write(void *user, const char *path, const char *text) {
  fake_files *files = (fake_files *)user;
  ++files->write_calls;
  if (files->write_fail) {
    return false;
  }
  if (files->mismatch) {
    return true;
  }
  if (ends_with(path, "/brightness")) {
    snprintf(files->brightness, sizeof(files->brightness), "%s", text);
  }
  return true;
}

static bool fake_file_writable(void *user, const char *path) {
  fake_files *files = (fake_files *)user;
  (void)path;
  return files->writable;
}

/* ---------------------------------------------------------------------- */
/* Fake evdev force-feedback device                                       */
/* ---------------------------------------------------------------------- */

typedef enum fake_evdev_event {
  FAKE_EV_UPLOAD = 1,
  FAKE_EV_PLAY,
  FAKE_EV_STOP,
  FAKE_EV_ERASE,
  FAKE_EV_CLOSE
} fake_evdev_event;

typedef struct fake_evdev {
  int fd;
  int open_calls;
  int close_calls;
  int ioctl_calls;
  int upload_calls;
  int rmff_calls;
  int write_calls;
  bool open_fail;
  int open_errno;
  bool ioctl_fail;
  int ioctl_errno;
  bool no_ev_ff;
  bool no_ff_rumble;
  int effects;
  bool name_fail;
  bool upload_fail;
  int upload_errno;
  bool negative_id;
  bool write_fail;
  int write_errno;
  bool rmff_fail;
  int next_effect_id;
  int last_rmff;
  int events[64];
  int event_count;
  char name[BD_FEEDBACK_NAME_MAX];
  struct ff_effect last_effect;
  struct input_event last_write;
} fake_evdev;

static void fake_evdev_log(fake_evdev *fake, fake_evdev_event event) {
  if (fake->event_count < (int)(sizeof(fake->events) / sizeof(fake->events[0]))) {
    fake->events[fake->event_count] = (int)event;
  }
  ++fake->event_count;
}

static int fake_evdev_event_at(const fake_evdev *fake, int index) {
  if (index < 0 || index >= fake->event_count ||
      index >= (int)(sizeof(fake->events) / sizeof(fake->events[0]))) {
    return -1;
  }
  return fake->events[index];
}

static void fake_evdev_init(fake_evdev *fake) {
  memset(fake, 0, sizeof(*fake));
  fake->fd = 42;
  fake->effects = 16;
  fake->next_effect_id = 3;
  strcpy(fake->name, "Fake Rumble Pad");
}

static int fake_evdev_open(void *user, const char *path, int flags) {
  fake_evdev *fake = (fake_evdev *)user;
  (void)path;
  (void)flags;
  ++fake->open_calls;
  if (fake->open_fail) {
    errno = fake->open_errno;
    return -1;
  }
  return fake->fd;
}

static int fake_evdev_close(void *user, int fd) {
  fake_evdev *fake = (fake_evdev *)user;
  (void)fd;
  ++fake->close_calls;
  fake_evdev_log(fake, FAKE_EV_CLOSE);
  return 0;
}

static void fake_set_bit(void *arg, unsigned int bit) {
  unsigned long *bits = (unsigned long *)arg;
  size_t index = bit / (8 * sizeof(unsigned long));
  bits[index] |= 1UL << (bit % (8 * sizeof(unsigned long)));
}

static int fake_evdev_ioctl(void *user, int fd, unsigned long request,
                            void *arg) {
  fake_evdev *fake = (fake_evdev *)user;
  (void)fd;
  ++fake->ioctl_calls;
  if (fake->ioctl_fail) {
    errno = fake->ioctl_errno;
    return -1;
  }
  unsigned int nr = _IOC_NR(request);
  if (nr == 0x20u) {
    if (!fake->no_ev_ff) {
      fake_set_bit(arg, EV_FF);
    }
    return 0;
  }
  if (nr == 0x20u + EV_FF) {
    if (!fake->no_ff_rumble) {
      fake_set_bit(arg, FF_RUMBLE);
    }
    return 0;
  }
  if (nr == 0x84u) {
    *(int *)arg = fake->effects;
    return 0;
  }
  if (nr == 0x06u) {
    if (fake->name_fail) {
      errno = EIO;
      return -1;
    }
    int written = snprintf((char *)arg, BD_FEEDBACK_NAME_MAX, "%s",
                           fake->name);
    return written;
  }
  if (nr == 0x80u) {
    ++fake->upload_calls;
    if (fake->upload_fail) {
      errno = fake->upload_errno;
      return -1;
    }
    struct ff_effect *effect = (struct ff_effect *)arg;
    int id = fake->negative_id ? -1 : fake->next_effect_id++;
    effect->id = (short)id;
    memcpy(&fake->last_effect, effect, sizeof(*effect));
    fake_evdev_log(fake, FAKE_EV_UPLOAD);
    return 0;
  }
  if (nr == 0x81u) {
    if (fake->rmff_fail) {
      errno = EIO;
      return -1;
    }
    ++fake->rmff_calls;
    fake->last_rmff = (int)(intptr_t)arg;
    fake_evdev_log(fake, FAKE_EV_ERASE);
    return 0;
  }
  errno = ENOTTY;
  return -1;
}

static ssize_t fake_evdev_write(void *user, int fd, const void *data,
                                size_t size) {
  fake_evdev *fake = (fake_evdev *)user;
  (void)fd;
  ++fake->write_calls;
  if (fake->write_fail) {
    errno = fake->write_errno;
    return -1;
  }
  if (size == sizeof(fake->last_write)) {
    memcpy(&fake->last_write, data, size);
    if (fake->last_write.type == EV_FF) {
      fake_evdev_log(fake, fake->last_write.value == 0 ? FAKE_EV_STOP
                                                       : FAKE_EV_PLAY);
    }
  }
  return (ssize_t)size;
}

static bd_feedback_evdev_io make_fake_evdev_io(fake_evdev *fake) {
  bd_feedback_evdev_io io;
  memset(&io, 0, sizeof(io));
  io.user = fake;
  io.open = fake_evdev_open;
  io.close = fake_evdev_close;
  io.ioctl = fake_evdev_ioctl;
  io.write = fake_evdev_write;
  return io;
}

typedef struct fake_cli_wait {
  int calls;
  int requested_ms;
  bd_feedback_rumble *rumble;
  fake_evdev *evdev;
  bool fd_open_at_wait;
  bool active_at_wait;
  int close_calls_at_wait;
  bd_feedback_cli_wait_result result;
} fake_cli_wait;

static bd_feedback_cli_wait_result fake_cli_wait_fn(void *user,
                                                     int duration_ms) {
  fake_cli_wait *wait = (fake_cli_wait *)user;
  ++wait->calls;
  wait->requested_ms = duration_ms;
  wait->fd_open_at_wait = wait->rumble->internal.fd >= 0;
  wait->active_at_wait = wait->rumble->active;
  wait->close_calls_at_wait = wait->evdev->close_calls;
  return wait->result;
}

/* ---------------------------------------------------------------------- */
/* LED tests                                                              */
/* ---------------------------------------------------------------------- */

static void test_led_real_sysfs(void) {
  char *root = make_temp_dir();
  CHECK(write_fixture(root, "sys/class/leds/test-led/max_brightness", "255\n"),
        "write max_brightness");
  CHECK(write_fixture(root, "sys/class/leds/test-led/brightness", "10\n"),
        "write brightness");
  CHECK(write_fixture(root, "sys/class/leds/test-led/trigger",
                      "[none] timer heartbeat\n"),
        "write trigger");

  bd_feedback_led_config config;
  memset(&config, 0, sizeof(config));
  config.sysfs_root = root;
  config.led = "/sys/class/leds/test-led";
  bd_feedback_led led;
  bd_feedback_error error;
  bd_feedback_error_clear(&error);
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error), BD_FEEDBACK_OK);
  CHECK(led.backend == BD_FEEDBACK_LED_SYSFS, "LED backend is sysfs");
  CHECK(led.raw_min == 0 && led.raw_max == 255, "LED raw range 0..255");
  CHECK(led.max_percent == 100, "LED default max 100");
  CHECK(led.readable && led.writable, "LED readable and writable");
  CHECK(strcmp(led.name, "test-led") == 0, "LED name: %s", led.name);

  int percent = -1;
  CHECK_RESULT(bd_feedback_led_get(&led, &percent, &error), BD_FEEDBACK_OK);
  CHECK(percent == 4, "initial LED percent %d, expected 4", percent);

  CHECK_RESULT(bd_feedback_led_set(&led, 50, &error), BD_FEEDBACK_OK);
  char path[512];
  char text[64];
  CHECK(join_path(path, sizeof(path), root,
                  "sys/class/leds/test-led/brightness"),
        "brightness path");
  CHECK(read_file(path, text, sizeof(text)) && strcmp(text, "128") == 0,
        "50 percent wrote 128, got '%s'", text);
  CHECK_RESULT(bd_feedback_led_get(&led, &percent, &error), BD_FEEDBACK_OK);
  CHECK(percent == 50, "readback LED percent %d", percent);

  CHECK_RESULT(bd_feedback_led_set(&led, 100, &error), BD_FEEDBACK_OK);
  CHECK(read_file(path, text, sizeof(text)) && strcmp(text, "255") == 0,
        "100 percent wrote 255, got '%s'", text);
  CHECK_RESULT(bd_feedback_led_set(&led, 0, &error), BD_FEEDBACK_OK);
  CHECK(read_file(path, text, sizeof(text)) && strcmp(text, "0") == 0,
        "0 percent wrote 0, got '%s'", text);
  CHECK_RESULT(bd_feedback_led_set(&led, 101, &error),
               BD_FEEDBACK_ERR_RANGE);
  CHECK_RESULT(bd_feedback_led_set(&led, -1, &error),
               BD_FEEDBACK_ERR_RANGE);
  CHECK(read_file(path, text, sizeof(text)) && strcmp(text, "0") == 0,
        "range rejection did not write");

  CHECK(write_fixture(root, "sys/class/leds/test-led/brightness", "10\n"),
        "restore brightness");
  bd_feedback_led capped;
  config.max_percent = 40;
  CHECK_RESULT(bd_feedback_led_probe(&capped, &config, &error),
               BD_FEEDBACK_OK);
  CHECK(capped.max_percent == 40, "LED capped max 40");
  CHECK_RESULT(bd_feedback_led_set(&capped, 41, &error),
               BD_FEEDBACK_ERR_RANGE);
  CHECK(read_file(path, text, sizeof(text)) && strcmp(text, "10") == 0,
        "cap rejection did not write");
  CHECK_RESULT(bd_feedback_led_set(&capped, 40, &error), BD_FEEDBACK_OK);
  CHECK(read_file(path, text, sizeof(text)) && strcmp(text, "102") == 0,
        "40 percent wrote 102, got '%s'", text);
  CHECK_RESULT(bd_feedback_led_get(&capped, &percent, &error),
               BD_FEEDBACK_OK);
  CHECK(percent == 40, "capped readback %d", percent);

  char before[64];
  CHECK(read_file(path, before, sizeof(before)), "read pre-trigger brightness");
  CHECK(write_fixture(root, "sys/class/leds/test-led/trigger",
                      "[heartbeat] none timer\n"),
        "switch to active trigger after probe");
  CHECK_RESULT(bd_feedback_led_set(&led, 50, &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
  CHECK(strstr(error.message, "heartbeat") != NULL,
        "set recheck error mentions heartbeat: %s", error.message);
  char after[64];
  CHECK(read_file(path, after, sizeof(after)), "read post-trigger brightness");
  CHECK(strcmp(before, after) == 0,
        "set with active trigger changed brightness: '%s' -> '%s'", before,
        after);
  CHECK(write_fixture(root, "sys/class/leds/test-led/trigger",
                      "[none] timer heartbeat\n"),
        "restore trigger after set recheck");

  char trigger_path[512];
  CHECK(join_path(trigger_path, sizeof(trigger_path), root,
                  "sys/class/leds/test-led/trigger"),
        "trigger path");
  CHECK(unlink(trigger_path) == 0, "remove trigger file");
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error), BD_FEEDBACK_OK);
  CHECK_RESULT(bd_feedback_led_set(&led, 30, &error), BD_FEEDBACK_OK);
  CHECK(read_file(path, text, sizeof(text)) && strcmp(text, "77") == 0,
        "set without trigger wrote '%s'", text);
  CHECK(write_fixture(root, "sys/class/leds/test-led/trigger",
                      "[none] timer heartbeat\n"),
        "restore trigger after missing-trigger test");

  CHECK(write_fixture(root, "sys/class/leds/test-led/trigger",
                      "[heartbeat] none timer\n"),
        "active trigger");
  config.max_percent = 0;
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
  CHECK(strstr(error.message, "heartbeat") != NULL,
        "active trigger error mentions it: %s", error.message);
  CHECK(write_fixture(root, "sys/class/leds/test-led/trigger",
                      "[none] timer heartbeat\n"),
        "restore trigger");

  CHECK(unlink(path) == 0, "remove brightness file");
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
  CHECK(write_fixture(root, "sys/class/leds/test-led/brightness", "10\n"),
        "restore brightness file");

  char max_path[512];
  CHECK(join_path(max_path, sizeof(max_path), root,
                  "sys/class/leds/test-led/max_brightness"),
        "max path");
  CHECK(write_file(max_path, "0\n"), "write max 0");
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
  CHECK(write_file(max_path, "255\n"), "restore max");
  CHECK(write_fixture(root, "sys/class/leds/test-led/brightness", "abc\n"),
        "write bad brightness");
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
  CHECK(write_fixture(root, "sys/class/leds/test-led/brightness", "300\n"),
        "write out-of-range brightness");
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
  CHECK(write_fixture(root, "sys/class/leds/test-led/brightness", "10\n"),
        "restore brightness again");

  CHECK(write_fixture(root, "sys/class/leds/switch-led/max_brightness", "1\n"),
        "write switch max");
  CHECK(write_fixture(root, "sys/class/leds/switch-led/brightness", "0\n"),
        "write switch brightness");
  memset(&config, 0, sizeof(config));
  config.sysfs_root = root;
  config.name = "switch-led";
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error), BD_FEEDBACK_OK);
  CHECK_RESULT(bd_feedback_led_set(&led, 50, &error), BD_FEEDBACK_OK);
  char switch_path[512];
  CHECK(join_path(switch_path, sizeof(switch_path), root,
                  "sys/class/leds/switch-led/brightness"),
        "switch path");
  CHECK(read_file(switch_path, text, sizeof(text)) && strcmp(text, "1") == 0,
        "50 percent on max_brightness=1 wrote '%s'", text);
  CHECK_RESULT(bd_feedback_led_get(&led, &percent, &error), BD_FEEDBACK_OK);
  CHECK(percent == 100, "max_brightness=1 read %d", percent);
  CHECK_RESULT(bd_feedback_led_set(&led, 0, &error), BD_FEEDBACK_OK);
  CHECK(read_file(switch_path, text, sizeof(text)) && strcmp(text, "0") == 0,
        "0 percent on max_brightness=1 wrote '%s'", text);

  memset(&config, 0, sizeof(config));
  config.sysfs_root = root;
  config.name = "test-led";
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error), BD_FEEDBACK_OK);
  CHECK(strcmp(led.name, "test-led") == 0, "name selection");
  config.led = "/sys/class/leds/test-led";
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error),
               BD_FEEDBACK_ERR_ARGUMENT);
  config.led = NULL;
  config.name = "a/b";
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error),
               BD_FEEDBACK_ERR_ARGUMENT);
  config.name = "test-led";
  config.backend = BD_FEEDBACK_LED_NONE;
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
  CHECK(led.backend == BD_FEEDBACK_LED_NONE, "disabled LED backend");

  CHECK(write_fixture(root, "sys/class/leds/aaa-bad/max_brightness", "255\n"),
        "auto bad max");
  CHECK(write_fixture(root, "sys/class/leds/bbb-good/max_brightness", "255\n"),
        "auto good max");
  CHECK(write_fixture(root, "sys/class/leds/bbb-good/brightness", "0\n"),
        "auto good brightness");
  memset(&config, 0, sizeof(config));
  config.sysfs_root = root;
  config.allow_auto_discover = true;
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error), BD_FEEDBACK_OK);
  CHECK(strcmp(led.name, "bbb-good") == 0,
        "auto discovery selected %s", led.name);

  memset(&config, 0, sizeof(config));
  config.sysfs_root = root;
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
  config.led = "/sys/class/leds/missing";
  config.allow_auto_discover = true;
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
  CHECK(led.backend == BD_FEEDBACK_LED_NONE,
        "explicit missing LED did not fall back");

  char vendor[512];
  CHECK(join_path(vendor, sizeof(vendor), root, "sys/class/led_anim"),
        "vendor path");
  CHECK(mkdir_p(vendor), "create vendor LED dir");
  CHECK(write_fixture(root, "sys/class/led_anim/max_scale", "42\n"),
        "vendor max_scale");
  memset(&config, 0, sizeof(config));
  config.sysfs_root = root;
  config.name = "test-led";
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error), BD_FEEDBACK_OK);
  CHECK(led.vendor_led_anim_present, "vendor LED detected");
  CHECK(led.vendor_led_blocker[0] != '\0',
        "vendor LED blocker is filled");
  char vendor_value[512];
  CHECK(join_path(vendor_value, sizeof(vendor_value), root,
                  "sys/class/led_anim/max_scale"),
        "vendor value path");
  CHECK(read_file(vendor_value, text, sizeof(text)) && strcmp(text, "42") == 0,
        "vendor max_scale was not written: '%s'", text);

  config.name = NULL;
  CHECK_RESULT(bd_feedback_led_probe(&led, &config, &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
  CHECK(led.vendor_led_anim_present, "vendor LED detected on failure");
  CHECK(led.vendor_led_blocker[0] != '\0',
        "vendor LED blocker on failure");

  remove_temp_dir(root);
}

static void test_led_fake_io(void) {
  fake_files files;
  fake_files_init(&files);
  bd_feedback_file_io io;
  memset(&io, 0, sizeof(io));
  io.user = &files;
  io.read = fake_file_read;
  io.write = fake_file_write;
  io.writable = fake_file_writable;

  bd_feedback_led_config config;
  memset(&config, 0, sizeof(config));
  config.sysfs_root = "/nonexistent-bd-feedback";
  config.led = "/virtual/led";
  bd_feedback_led led;
  bd_feedback_error error;
  bd_feedback_error_clear(&error);
  CHECK_RESULT(bd_feedback_led_probe_with_io(&led, &config, &io, &error),
               BD_FEEDBACK_OK);
  CHECK(led.writable, "fake LED writable");
  CHECK_RESULT(bd_feedback_led_set(&led, 50, &error), BD_FEEDBACK_OK);
  CHECK(strcmp(files.brightness, "128") == 0,
        "fake LED wrote %s", files.brightness);
  int percent = 0;
  CHECK_RESULT(bd_feedback_led_get(&led, &percent, &error), BD_FEEDBACK_OK);
  CHECK(percent == 50, "fake LED read %d", percent);

  int writes_before_trigger = files.write_calls;
  files.has_trigger = true;
  strcpy(files.trigger, "[timer] none");
  CHECK_RESULT(bd_feedback_led_set(&led, 20, &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
  CHECK(files.write_calls == writes_before_trigger,
        "set recheck did not write a triggered LED");
  CHECK(strcmp(files.brightness, "128") == 0,
        "set recheck left brightness unchanged");
  CHECK(strstr(error.message, "timer") != NULL,
        "set recheck error mentions trigger: %s", error.message);
  files.has_trigger = false;

  files.write_fail = true;
  CHECK_RESULT(bd_feedback_led_set(&led, 20, &error), BD_FEEDBACK_ERR_IO);
  CHECK(strcmp(files.brightness, "128") == 0,
        "failed write left brightness unchanged");
  files.write_fail = false;
  files.mismatch = true;
  CHECK_RESULT(bd_feedback_led_set(&led, 30, &error), BD_FEEDBACK_ERR_IO);
  CHECK(strcmp(files.brightness, "128") == 0,
        "mismatched write left brightness unchanged");
  files.mismatch = false;
  CHECK_RESULT(bd_feedback_led_set(&led, 30, &error), BD_FEEDBACK_OK);
  CHECK(strcmp(files.brightness, "77") == 0,
        "30 percent wrote %s", files.brightness);
  files.read_fail = true;
  CHECK_RESULT(bd_feedback_led_get(&led, &percent, &error),
               BD_FEEDBACK_ERR_IO);
  files.read_fail = false;

  files.writable = false;
  bd_feedback_error_clear(&error);
  CHECK_RESULT(bd_feedback_led_probe_with_io(&led, &config, &io, &error),
               BD_FEEDBACK_OK);
  CHECK(!led.writable, "fake LED reports read-only");
  int writes = files.write_calls;
  CHECK_RESULT(bd_feedback_led_set(&led, 10, &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
  CHECK(files.write_calls == writes, "read-only LED was not written");

  files.writable = true;
  files.has_trigger = true;
  strcpy(files.trigger, "[timer] none");
  CHECK_RESULT(bd_feedback_led_probe_with_io(&led, &config, &io, &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
  CHECK(strstr(error.message, "timer") != NULL,
        "fake active trigger error: %s", error.message);
  files.has_trigger = false;

  config.max_percent = 40;
  CHECK_RESULT(bd_feedback_led_probe_with_io(&led, &config, &io, &error),
               BD_FEEDBACK_OK);
  CHECK_RESULT(bd_feedback_led_set(&led, 41, &error),
               BD_FEEDBACK_ERR_RANGE);
  CHECK_RESULT(bd_feedback_led_set(&led, 40, &error), BD_FEEDBACK_OK);
  CHECK(strcmp(files.brightness, "102") == 0,
        "capped fake LED wrote %s", files.brightness);

  config.max_percent = 101;
  CHECK_RESULT(bd_feedback_led_probe_with_io(&led, &config, &io, &error),
               BD_FEEDBACK_ERR_ARGUMENT);
  config.max_percent = 0;
  config.led = NULL;
  config.name = NULL;
  CHECK_RESULT(bd_feedback_led_probe_with_io(&led, &config, &io, &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
}

/* ---------------------------------------------------------------------- */
/* Rumble tests                                                           */
/* ---------------------------------------------------------------------- */

static bd_feedback_rumble_config rumble_config(const char *root) {
  bd_feedback_rumble_config config;
  memset(&config, 0, sizeof(config));
  config.event = "/dev/input/event7";
  config.sysfs_root = root;
  config.backend = BD_FEEDBACK_RUMBLE_EVDEV;
  return config;
}

static void test_rumble_probe_and_play(void) {
  fake_evdev fake;
  fake_evdev_init(&fake);
  bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
  bd_feedback_rumble_config config = rumble_config("/nonexistent-bd-feedback");
  bd_feedback_rumble rumble;
  bd_feedback_error error;
  bd_feedback_error_clear(&error);
  CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io, &error),
               BD_FEEDBACK_OK);
  CHECK(rumble.backend == BD_FEEDBACK_RUMBLE_EVDEV, "rumble backend evdev");
  CHECK(rumble.has_ff_rumble, "rumble has FF_RUMBLE");
  CHECK(rumble.effects == 16, "rumble effects %d", rumble.effects);
  CHECK(strcmp(rumble.device_name, "Fake Rumble Pad") == 0,
        "rumble device name: %s", rumble.device_name);
  CHECK(rumble.internal.fd == fake.fd, "rumble fd kept open");
  CHECK(fake.upload_calls == 0 && fake.write_calls == 0,
        "probe did not upload or play");

  CHECK_RESULT(bd_feedback_rumble_play(&rumble, 50, 200, &error),
               BD_FEEDBACK_OK);
  CHECK(fake.upload_calls == 1, "one effect uploaded");
  CHECK(fake.last_effect.type == FF_RUMBLE, "effect type FF_RUMBLE");
  CHECK(fake.last_effect.id == 3, "effect id %d", fake.last_effect.id);
  CHECK(fake.last_effect.u.rumble.strong_magnitude == 32768,
        "strong magnitude %u", fake.last_effect.u.rumble.strong_magnitude);
  CHECK(fake.last_effect.u.rumble.weak_magnitude == 32768,
        "weak magnitude %u", fake.last_effect.u.rumble.weak_magnitude);
  CHECK(fake.last_effect.replay.length == 200, "replay length %u",
        fake.last_effect.replay.length);
  CHECK(fake.last_effect.replay.delay == 0, "replay delay zero");
  CHECK(fake.last_effect.trigger.button == 0 &&
            fake.last_effect.trigger.interval == 0,
        "effect trigger is immediate");
  CHECK(fake.write_calls == 1, "one EV_FF write");
  CHECK(fake.last_write.type == EV_FF, "EV_FF write type");
  CHECK(fake.last_write.code == 3, "EV_FF write code %u",
        fake.last_write.code);
  CHECK(fake.last_write.value == 1, "EV_FF write value %d",
        fake.last_write.value);
  CHECK(rumble.active, "rumble active after play");

  CHECK_RESULT(bd_feedback_rumble_stop(&rumble, &error), BD_FEEDBACK_OK);
  CHECK(fake.write_calls == 2, "stop writes EV_FF");
  CHECK(fake.last_write.value == 0, "stop value zero");
  CHECK(fake.rmff_calls == 1, "stop erases the effect");
  CHECK(fake.last_rmff == 3, "erased effect id %d", fake.last_rmff);
  CHECK(!rumble.active, "rumble inactive after stop");
  CHECK(rumble.internal.effect_id == -1, "effect id cleared");
  CHECK_RESULT(bd_feedback_rumble_stop(&rumble, &error), BD_FEEDBACK_OK);
  CHECK(fake.write_calls == 2 && fake.rmff_calls == 1,
        "second stop is idempotent");

  CHECK_RESULT(bd_feedback_rumble_play(&rumble, 100, 50, &error),
               BD_FEEDBACK_OK);
  CHECK(fake.last_effect.u.rumble.strong_magnitude == 65535,
        "100 percent magnitude %u",
        fake.last_effect.u.rumble.strong_magnitude);
  CHECK_RESULT(bd_feedback_rumble_close(&rumble, &error), BD_FEEDBACK_OK);
  CHECK(fake.close_calls == 1, "close closes the fd");
  CHECK(fake.write_calls == 4 && fake.rmff_calls == 2,
        "close stopped and erased the active effect");
  CHECK(rumble.internal.fd == -1, "fd cleared after close");
  CHECK_RESULT(bd_feedback_rumble_close(&rumble, &error), BD_FEEDBACK_OK);
  CHECK(fake.close_calls == 1, "second close does nothing");
}

static void test_rumble_replay_and_ranges(void) {
  fake_evdev fake;
  fake_evdev_init(&fake);
  bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
  bd_feedback_rumble_config config = rumble_config("/nonexistent-bd-feedback");
  config.max_percent = 40;
  config.max_duration_ms = 300;
  bd_feedback_rumble rumble;
  bd_feedback_error error;
  bd_feedback_error_clear(&error);
  CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io, &error),
               BD_FEEDBACK_OK);

  CHECK_RESULT(bd_feedback_rumble_play(&rumble, 41, 100, &error),
               BD_FEEDBACK_ERR_RANGE);
  CHECK(fake.upload_calls == 0, "strength cap rejected before upload");
  CHECK_RESULT(bd_feedback_rumble_play(&rumble, 40, 301, &error),
               BD_FEEDBACK_ERR_RANGE);
  CHECK(fake.upload_calls == 0, "duration cap rejected before upload");
  CHECK_RESULT(bd_feedback_rumble_play(&rumble, 40, 0, &error),
               BD_FEEDBACK_ERR_RANGE);
  CHECK_RESULT(bd_feedback_rumble_play(&rumble, -1, 100, &error),
               BD_FEEDBACK_ERR_RANGE);
  CHECK_RESULT(bd_feedback_rumble_play(&rumble, 101, 100, &error),
               BD_FEEDBACK_ERR_RANGE);
  CHECK_RESULT(bd_feedback_rumble_play(&rumble, 40, 300, &error),
               BD_FEEDBACK_OK);
  CHECK(fake.upload_calls == 1, "capped play uploaded once");

  int writes = fake.write_calls;
  int erases = fake.rmff_calls;
  CHECK_RESULT(bd_feedback_rumble_play(&rumble, 0, 100, &error),
               BD_FEEDBACK_OK);
  CHECK(fake.upload_calls == 1, "zero strength does not upload");
  CHECK(fake.write_calls == writes + 1, "zero strength stops");
  CHECK(fake.rmff_calls == erases + 1, "zero strength erases");
  CHECK_RESULT(bd_feedback_rumble_play(&rumble, 0, 100, &error),
               BD_FEEDBACK_OK);
  CHECK(fake.upload_calls == 1, "zero strength without effect is a no-op");

  CHECK_RESULT(bd_feedback_rumble_play(&rumble, 20, 100, &error),
               BD_FEEDBACK_OK);
  int uploads = fake.upload_calls;
  int writes_now = fake.write_calls;
  int erases_now = fake.rmff_calls;
  CHECK_RESULT(bd_feedback_rumble_play(&rumble, 30, 120, &error),
               BD_FEEDBACK_OK);
  CHECK(fake.upload_calls == uploads + 1, "replay uploads a new effect");
  CHECK(fake.rmff_calls == erases_now + 1, "replay erases the old effect");
  CHECK(fake.write_calls == writes_now + 2, "replay stops then plays");

  CHECK_RESULT(bd_feedback_rumble_close(&rumble, &error), BD_FEEDBACK_OK);
  CHECK(fake.close_calls == 1, "replay close");
}

static void test_rumble_capabilities(void) {
  bd_feedback_rumble_config config = rumble_config("/nonexistent-bd-feedback");
  bd_feedback_rumble rumble;
  bd_feedback_error error;

  {
    fake_evdev fake;
    fake_evdev_init(&fake);
    fake.no_ev_ff = true;
    bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
    bd_feedback_error_clear(&error);
    CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io,
                                                  &error),
                 BD_FEEDBACK_ERR_UNSUPPORTED);
    CHECK(strstr(error.message, "EV_FF") != NULL, "EV_FF error: %s",
          error.message);
    CHECK(fake.close_calls == 1, "failed probe closes the fd");
  }
  {
    fake_evdev fake;
    fake_evdev_init(&fake);
    fake.no_ff_rumble = true;
    bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
    bd_feedback_error_clear(&error);
    CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io,
                                                  &error),
                 BD_FEEDBACK_ERR_UNSUPPORTED);
    CHECK(strstr(error.message, "FF_RUMBLE") != NULL, "FF_RUMBLE error: %s",
          error.message);
  }
  {
    fake_evdev fake;
    fake_evdev_init(&fake);
    fake.effects = 0;
    bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
    bd_feedback_error_clear(&error);
    CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io,
                                                  &error),
                 BD_FEEDBACK_ERR_UNSUPPORTED);
    CHECK(strstr(error.message, "effect slots") != NULL,
          "effect slot error: %s", error.message);
  }
  {
    fake_evdev fake;
    fake_evdev_init(&fake);
    fake.ioctl_fail = true;
    fake.ioctl_errno = EIO;
    bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
    bd_feedback_error_clear(&error);
    CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io,
                                                  &error),
                 BD_FEEDBACK_ERR_IO);
    CHECK(fake.close_calls == 1, "I/O failure closes the fd");
  }
  {
    fake_evdev fake;
    fake_evdev_init(&fake);
    fake.open_fail = true;
    fake.open_errno = ENOENT;
    bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
    bd_feedback_error_clear(&error);
    CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io,
                                                  &error),
                 BD_FEEDBACK_ERR_UNSUPPORTED);
    CHECK(fake.close_calls == 0, "missing node was not closed");
  }
  {
    fake_evdev fake;
    fake_evdev_init(&fake);
    fake.open_fail = true;
    fake.open_errno = EACCES;
    bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
    bd_feedback_error_clear(&error);
    CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io,
                                                  &error),
                 BD_FEEDBACK_ERR_IO);
  }
  {
    fake_evdev fake;
    fake_evdev_init(&fake);
    fake.name_fail = true;
    bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
    bd_feedback_error_clear(&error);
    CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io,
                                                  &error),
                 BD_FEEDBACK_OK);
    CHECK(rumble.device_name[0] == '\0', "name failure is non-fatal");
    CHECK_RESULT(bd_feedback_rumble_close(&rumble, &error), BD_FEEDBACK_OK);
  }

  bd_feedback_error_clear(&error);
  config.event = NULL;
  CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, NULL,
                                                &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
  config = rumble_config("/nonexistent-bd-feedback");
  config.max_percent = 101;
  CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, NULL,
                                                &error),
               BD_FEEDBACK_ERR_ARGUMENT);
  config = rumble_config("/nonexistent-bd-feedback");
  config.max_duration_ms = 60001;
  CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, NULL,
                                                &error),
               BD_FEEDBACK_ERR_ARGUMENT);
  config = rumble_config("/nonexistent-bd-feedback");
  config.backend = (bd_feedback_rumble_backend)99;
  CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, NULL,
                                                &error),
               BD_FEEDBACK_ERR_ARGUMENT);
  config = rumble_config("/nonexistent-bd-feedback");
  config.backend = BD_FEEDBACK_RUMBLE_NONE;
  CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, NULL,
                                                &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);

  config = rumble_config("/nonexistent-bd-feedback");
  config.event = "/nonexistent-bd-feedback/event";
  bd_feedback_error_clear(&error);
  CHECK_RESULT(bd_feedback_rumble_probe(&rumble, &config, &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
  CHECK(strstr(error.message, "not present") != NULL,
        "missing real event error: %s", error.message);
}

static void test_rumble_failures(void) {
  bd_feedback_rumble_config config = rumble_config("/nonexistent-bd-feedback");
  bd_feedback_rumble rumble;
  bd_feedback_error error;

  {
    fake_evdev fake;
    fake_evdev_init(&fake);
    bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
    bd_feedback_error_clear(&error);
    CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io,
                                                  &error),
                 BD_FEEDBACK_OK);
    fake.upload_fail = true;
    fake.upload_errno = EINVAL;
    CHECK_RESULT(bd_feedback_rumble_play(&rumble, 50, 200, &error),
                 BD_FEEDBACK_ERR_IO);
    CHECK(fake.upload_calls == 1, "upload attempted once");
    CHECK(fake.write_calls == 0, "failed upload did not play");
    CHECK(rumble.internal.effect_id == -1, "failed upload has no effect id");
    CHECK(!rumble.active, "failed upload is inactive");
    CHECK_RESULT(bd_feedback_rumble_close(&rumble, &error), BD_FEEDBACK_OK);
    CHECK(fake.close_calls == 1, "upload failure close");
  }
  {
    fake_evdev fake;
    fake_evdev_init(&fake);
    bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
    bd_feedback_error_clear(&error);
    CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io,
                                                  &error),
                 BD_FEEDBACK_OK);
    fake.write_fail = true;
    fake.write_errno = EIO;
    CHECK_RESULT(bd_feedback_rumble_play(&rumble, 50, 200, &error),
                 BD_FEEDBACK_ERR_IO);
    CHECK(fake.upload_calls == 1, "play write failure uploaded");
    CHECK(fake.write_calls == 2, "play failure attempted stop");
    CHECK(fake.rmff_calls == 1, "play failure erased the effect");
    CHECK(rumble.internal.effect_id == -1,
          "play failure cleanup cleared effect id");
    CHECK(!rumble.active, "play failure is inactive");
    CHECK(strstr(error.message, "EV_FF play") != NULL,
          "play failure message: %s", error.message);
    CHECK_RESULT(bd_feedback_rumble_close(&rumble, &error), BD_FEEDBACK_OK);
  }
  {
    fake_evdev fake;
    fake_evdev_init(&fake);
    bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
    bd_feedback_error_clear(&error);
    CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io,
                                                  &error),
                 BD_FEEDBACK_OK);
    fake.write_fail = true;
    fake.write_errno = EIO;
    fake.rmff_fail = true;
    CHECK_RESULT(bd_feedback_rumble_play(&rumble, 50, 200, &error),
                 BD_FEEDBACK_ERR_IO);
    CHECK(rumble.internal.effect_id == 3,
          "un-erased effect id retained: %d", rumble.internal.effect_id);
    CHECK(!rumble.active, "failed play never became active");
    CHECK_RESULT(bd_feedback_rumble_close(&rumble, &error),
                 BD_FEEDBACK_ERR_IO);
    CHECK(fake.close_calls == 1, "cleanup failure still closes");
    CHECK(rumble.internal.fd == -1, "cleanup failure clears fd");
    CHECK(rumble.internal.effect_id == -1, "cleanup failure clears effect id");
  }

  memset(&rumble, 0, sizeof(rumble));
  rumble.internal.fd = -1;
  rumble.backend = BD_FEEDBACK_RUMBLE_NONE;
  bd_feedback_error_clear(&error);
  CHECK_RESULT(bd_feedback_rumble_play(&rumble, 10, 10, &error),
               BD_FEEDBACK_ERR_STATE);
  CHECK_RESULT(bd_feedback_rumble_stop(&rumble, &error),
               BD_FEEDBACK_ERR_STATE);
  CHECK_RESULT(bd_feedback_rumble_close(&rumble, &error), BD_FEEDBACK_OK);
}

static void test_rumble_vendor_motor(void) {
  char *root = make_temp_dir();
  CHECK(write_fixture(root, "sys/class/motor/voltage", "900000\n"),
        "write vendor voltage");
  CHECK(write_fixture(root, "sys/class/gpio/gpio227/value", "0\n"),
        "write vendor gpio");

  fake_evdev fake;
  fake_evdev_init(&fake);
  bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
  bd_feedback_rumble_config config = rumble_config(root);
  bd_feedback_rumble rumble;
  bd_feedback_error error;
  bd_feedback_error_clear(&error);
  CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io, &error),
               BD_FEEDBACK_OK);
  CHECK(rumble.vendor_motor_voltage_present, "vendor voltage detected");
  CHECK(rumble.vendor_motor_enable_present, "vendor gpio detected");
  CHECK(rumble.vendor_blocker[0] != '\0', "vendor blocker is filled");
  CHECK(strstr(rumble.vendor_blocker, "probe-only") != NULL,
        "vendor blocker text: %s", rumble.vendor_blocker);
  CHECK_RESULT(bd_feedback_rumble_close(&rumble, &error), BD_FEEDBACK_OK);

  char path[512];
  char text[64];
  CHECK(join_path(path, sizeof(path), root, "sys/class/motor/voltage"),
        "vendor voltage path");
  CHECK(read_file(path, text, sizeof(text)) && strcmp(text, "900000") == 0,
        "vendor voltage was not written: '%s'", text);
  CHECK(join_path(path, sizeof(path), root,
                  "sys/class/gpio/gpio227/value"),
        "vendor gpio path");
  CHECK(read_file(path, text, sizeof(text)) && strcmp(text, "0") == 0,
        "vendor gpio was not written: '%s'", text);

  fake_evdev_init(&fake);
  fake.open_fail = true;
  fake.open_errno = ENOENT;
  io = make_fake_evdev_io(&fake);
  bd_feedback_error_clear(&error);
  CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io, &error),
               BD_FEEDBACK_ERR_UNSUPPORTED);
  CHECK(rumble.vendor_motor_voltage_present,
        "vendor voltage detected on failure");
  CHECK(rumble.vendor_motor_enable_present,
        "vendor gpio detected on failure");
  CHECK(rumble.vendor_blocker[0] != '\0',
        "vendor blocker on failure");

  remove_temp_dir(root);
}

static void test_cli_play_lifecycle(void) {
  bd_feedback_rumble_config config = rumble_config("/nonexistent-bd-feedback");
  bd_feedback_error error;

  {
    fake_evdev fake;
    fake_evdev_init(&fake);
    bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
    bd_feedback_rumble rumble;
    bd_feedback_error_clear(&error);
    CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io,
                                                  &error),
                 BD_FEEDBACK_OK);
    fake_cli_wait wait;
    memset(&wait, 0, sizeof(wait));
    wait.rumble = &rumble;
    wait.evdev = &fake;
    wait.result = BD_FEEDBACK_CLI_WAIT_COMPLETE;
    bd_feedback_cli_play_report report;
    bd_feedback_cli_play_status status = bd_feedback_cli_play_and_wait(
        &rumble, 50, 200, fake_cli_wait_fn, &wait, &report);
    CHECK(status == BD_FEEDBACK_CLI_PLAY_OK, "CLI play status %d", status);
    CHECK(wait.calls == 1, "CLI waited once, got %d", wait.calls);
    CHECK(wait.requested_ms == 200, "CLI waited %d ms, expected 200",
          wait.requested_ms);
    CHECK(wait.fd_open_at_wait, "CLI closed the fd before the wait");
    CHECK(wait.active_at_wait, "CLI effect was inactive during the wait");
    CHECK(wait.close_calls_at_wait == 0,
          "CLI closed the event node before the wait");
    CHECK(fake.close_calls == 1, "CLI close count %d", fake.close_calls);
    CHECK(report.waited, "CLI report says it did not wait");
    CHECK(report.play_result == BD_FEEDBACK_OK &&
              report.close_result == BD_FEEDBACK_OK &&
              report.wait_result == BD_FEEDBACK_CLI_WAIT_COMPLETE,
          "CLI report results play=%s close=%s wait=%d",
          bd_feedback_result_name(report.play_result),
          bd_feedback_result_name(report.close_result), report.wait_result);
    CHECK(fake.event_count == 5, "CLI event count %d", fake.event_count);
    CHECK(fake_evdev_event_at(&fake, 0) == FAKE_EV_UPLOAD,
          "CLI event 0 %d", fake_evdev_event_at(&fake, 0));
    CHECK(fake_evdev_event_at(&fake, 1) == FAKE_EV_PLAY,
          "CLI event 1 %d", fake_evdev_event_at(&fake, 1));
    CHECK(fake_evdev_event_at(&fake, 2) == FAKE_EV_STOP,
          "CLI event 2 %d", fake_evdev_event_at(&fake, 2));
    CHECK(fake_evdev_event_at(&fake, 3) == FAKE_EV_ERASE,
          "CLI event 3 %d", fake_evdev_event_at(&fake, 3));
    CHECK(fake_evdev_event_at(&fake, 4) == FAKE_EV_CLOSE,
          "CLI event 4 %d", fake_evdev_event_at(&fake, 4));
  }

  {
    volatile sig_atomic_t stop = 0;
    struct timespec start;
    struct timespec end;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0, "clock start");
    bd_feedback_cli_wait_result wait_result =
        bd_feedback_cli_wait_ms((void *)&stop, 40);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0, "clock end");
    long long elapsed_ms =
        (long long)(end.tv_sec - start.tv_sec) * 1000 +
        (long long)(end.tv_nsec - start.tv_nsec) / 1000000;
    CHECK(wait_result == BD_FEEDBACK_CLI_WAIT_COMPLETE,
          "real wait result %d", wait_result);
    CHECK(elapsed_ms >= 30 && elapsed_ms < 1000,
          "real wait elapsed %lld ms", elapsed_ms);
    stop = 1;
    wait_result = bd_feedback_cli_wait_ms((void *)&stop, 200);
    CHECK(wait_result == BD_FEEDBACK_CLI_WAIT_INTERRUPTED,
          "interrupted wait result %d", wait_result);
  }

  {
    struct sigaction old_action;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = test_alarm_handler;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGALRM, &action, &old_action) == 0,
          "install SIGALRM handler");
    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));
    timer.it_value.tv_usec = 20000;
    CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0, "arm SIGALRM");
    struct timespec start;
    struct timespec end;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0, "signal clock start");
    g_alarm_stop = 0;
    bd_feedback_cli_wait_result wait_result =
        bd_feedback_cli_wait_ms((void *)&g_alarm_stop, 200);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0, "signal clock end");
    long long elapsed_ms =
        (long long)(end.tv_sec - start.tv_sec) * 1000 +
        (long long)(end.tv_nsec - start.tv_nsec) / 1000000;
    memset(&timer, 0, sizeof(timer));
    setitimer(ITIMER_REAL, &timer, NULL);
    sigaction(SIGALRM, &old_action, NULL);
    CHECK(wait_result == BD_FEEDBACK_CLI_WAIT_INTERRUPTED,
          "signal wait result %d", wait_result);
    CHECK(elapsed_ms < 180, "signal wait elapsed %lld ms", elapsed_ms);
  }

  {
    fake_evdev fake;
    fake_evdev_init(&fake);
    bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
    bd_feedback_rumble rumble;
    bd_feedback_error_clear(&error);
    CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io,
                                                  &error),
                 BD_FEEDBACK_OK);
    volatile sig_atomic_t stop = 0;
    struct timespec start;
    struct timespec end;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0, "play clock start");
    bd_feedback_cli_play_report report;
    bd_feedback_cli_play_status status = bd_feedback_cli_play_and_wait(
        &rumble, 50, 40, bd_feedback_cli_wait_ms, (void *)&stop, &report);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0, "play clock end");
    long long elapsed_ms =
        (long long)(end.tv_sec - start.tv_sec) * 1000 +
        (long long)(end.tv_nsec - start.tv_nsec) / 1000000;
    CHECK(status == BD_FEEDBACK_CLI_PLAY_OK, "real-wait CLI status %d",
          status);
    CHECK(elapsed_ms >= 30 && elapsed_ms < 1000,
          "real-wait CLI elapsed %lld ms", elapsed_ms);
    CHECK(report.waited, "real-wait report waited");
    CHECK(fake.close_calls == 1, "real-wait close %d", fake.close_calls);
    CHECK(fake_evdev_event_at(&fake, 2) == FAKE_EV_STOP,
          "real-wait cleanup stop");
    CHECK(fake_evdev_event_at(&fake, 3) == FAKE_EV_ERASE,
          "real-wait cleanup erase");
    CHECK(fake_evdev_event_at(&fake, 4) == FAKE_EV_CLOSE,
          "real-wait cleanup close");
  }

  {
    fake_evdev fake;
    fake_evdev_init(&fake);
    bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
    bd_feedback_rumble rumble;
    bd_feedback_error_clear(&error);
    CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io,
                                                  &error),
                 BD_FEEDBACK_OK);
    fake_cli_wait wait;
    memset(&wait, 0, sizeof(wait));
    wait.rumble = &rumble;
    wait.evdev = &fake;
    wait.result = BD_FEEDBACK_CLI_WAIT_INTERRUPTED;
    bd_feedback_cli_play_report report;
    bd_feedback_cli_play_status status = bd_feedback_cli_play_and_wait(
        &rumble, 50, 200, fake_cli_wait_fn, &wait, &report);
    CHECK(status == BD_FEEDBACK_CLI_PLAY_INTERRUPTED,
          "interrupted CLI status %d", status);
    CHECK(wait.calls == 1, "interrupted CLI wait calls %d", wait.calls);
    CHECK(fake.close_calls == 1, "interrupted CLI close %d",
          fake.close_calls);
    CHECK(report.wait_result == BD_FEEDBACK_CLI_WAIT_INTERRUPTED,
          "interrupted report wait %d", report.wait_result);
    CHECK(fake_evdev_event_at(&fake, 2) == FAKE_EV_STOP,
          "interrupted CLI cleanup stopped");
    CHECK(fake_evdev_event_at(&fake, 3) == FAKE_EV_ERASE,
          "interrupted CLI cleanup erased");
    CHECK(fake_evdev_event_at(&fake, 4) == FAKE_EV_CLOSE,
          "interrupted CLI cleanup closed");
  }

  {
    fake_evdev fake;
    fake_evdev_init(&fake);
    fake.upload_fail = true;
    fake.upload_errno = EINVAL;
    bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
    bd_feedback_rumble rumble;
    bd_feedback_error_clear(&error);
    CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io,
                                                  &error),
                 BD_FEEDBACK_OK);
    fake_cli_wait wait;
    memset(&wait, 0, sizeof(wait));
    wait.rumble = &rumble;
    wait.evdev = &fake;
    bd_feedback_cli_play_report report;
    bd_feedback_cli_play_status status = bd_feedback_cli_play_and_wait(
        &rumble, 50, 200, fake_cli_wait_fn, &wait, &report);
    CHECK(status == BD_FEEDBACK_CLI_PLAY_FAILED,
          "failed play CLI status %d", status);
    CHECK(wait.calls == 0, "failed play waited %d times", wait.calls);
    CHECK(!report.waited, "failed play report waited");
    CHECK(report.play_result == BD_FEEDBACK_ERR_IO,
          "failed play result %s",
          bd_feedback_result_name(report.play_result));
    CHECK(fake.close_calls == 1, "failed play close %d", fake.close_calls);
  }

  {
    fake_evdev fake;
    fake_evdev_init(&fake);
    bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
    bd_feedback_rumble rumble;
    bd_feedback_error_clear(&error);
    CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io,
                                                  &error),
                 BD_FEEDBACK_OK);
    fake_cli_wait wait;
    memset(&wait, 0, sizeof(wait));
    wait.rumble = &rumble;
    wait.evdev = &fake;
    bd_feedback_cli_play_report report;
    bd_feedback_cli_play_status status = bd_feedback_cli_play_and_wait(
        &rumble, 0, 200, fake_cli_wait_fn, &wait, &report);
    CHECK(status == BD_FEEDBACK_CLI_PLAY_OK,
          "zero-strength CLI status %d", status);
    CHECK(wait.calls == 0, "zero strength waited %d times", wait.calls);
    CHECK(!report.waited, "zero strength report waited");
    CHECK(fake.upload_calls == 0, "zero strength uploaded");
    CHECK(fake.close_calls == 1, "zero strength close %d", fake.close_calls);
  }

  {
    fake_evdev fake;
    fake_evdev_init(&fake);
    bd_feedback_evdev_io io = make_fake_evdev_io(&fake);
    bd_feedback_rumble rumble;
    bd_feedback_error_clear(&error);
    CHECK_RESULT(bd_feedback_rumble_probe_with_io(&rumble, &config, &io,
                                                  &error),
                 BD_FEEDBACK_OK);
    fake.write_fail = true;
    fake.write_errno = EIO;
    fake.rmff_fail = true;
    fake_cli_wait wait;
    memset(&wait, 0, sizeof(wait));
    wait.rumble = &rumble;
    wait.evdev = &fake;
    bd_feedback_cli_play_report report;
    bd_feedback_cli_play_status status = bd_feedback_cli_play_and_wait(
        &rumble, 50, 200, fake_cli_wait_fn, &wait, &report);
    CHECK(status == BD_FEEDBACK_CLI_PLAY_FAILED,
          "cleanup failure CLI status %d", status);
    CHECK(wait.calls == 0, "cleanup failure waited %d times", wait.calls);
    CHECK(report.close_result == BD_FEEDBACK_ERR_IO,
          "cleanup failure close result %s",
          bd_feedback_result_name(report.close_result));
    CHECK(fake.close_calls == 1, "cleanup failure close %d",
          fake.close_calls);
  }

  bd_feedback_cli_play_report null_report;
  CHECK(bd_feedback_cli_play_and_wait(NULL, 10, 10, fake_cli_wait_fn, NULL,
                                      &null_report) ==
            BD_FEEDBACK_CLI_PLAY_FAILED,
        "NULL rumble helper status");
}

int main(void) {
  test_led_real_sysfs();
  test_led_fake_io();
  test_rumble_probe_and_play();
  test_rumble_replay_and_ranges();
  test_rumble_capabilities();
  test_rumble_failures();
  test_rumble_vendor_motor();
  test_cli_play_lifecycle();

  if (g_failures != 0) {
    fprintf(stderr, "device_feedback_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("device_feedback_test: all checks passed\n");
  return 0;
}
