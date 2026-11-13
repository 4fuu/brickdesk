#define _POSIX_C_SOURCE 200809L

#include "brickdesk/device_av.h"
#include "device_av_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition, ...)                         \
  do {                                                \
    if (!(condition)) {                               \
      fprintf(stderr, "device_av_test:%d: ", __LINE__); \
      fprintf(stderr, __VA_ARGS__);                   \
      fprintf(stderr, "\n");                          \
      ++g_failures;                                   \
    }                                                 \
  } while (0)

static int g_failures;

static void check_result(bd_av_result actual, bd_av_result expected,
                         const char *expression, int line) {
  if (actual != expected) {
    fprintf(stderr,
            "device_av_test:%d: %s returned %s, expected %s\n", line,
            expression, bd_av_result_name(actual),
            bd_av_result_name(expected));
    ++g_failures;
  }
}

#define CHECK_RESULT(expression, expected) \
  check_result((expression), (expected), #expression, __LINE__)

static char *make_temp_dir(void) {
  char template[] = "/tmp/bd_device_av.XXXXXX";
  char *directory = mkdtemp(template);
  if (directory == NULL) {
    fprintf(stderr, "device_av_test: mkdtemp: %s\n", strerror(errno));
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
  return ok;
}

/* ---------------------------------------------------------------------- */
/* Fake /dev/disp                                                         */
/* ---------------------------------------------------------------------- */

typedef struct fake_disp {
  int fd;
  int get_value;
  bool get_error;
  bool set_error;
  bool mismatch;
  bool open_rw_fail;
  bool open_ro_fail;
  int open_calls;
  int get_calls;
  int set_calls;
  int close_calls;
  unsigned long last_set[4];
} fake_disp;

static int fake_disp_open(void *user, const char *path, int flags) {
  (void)path;
  fake_disp *fake = (fake_disp *)user;
  ++fake->open_calls;
  if ((flags & O_ACCMODE) == O_RDWR) {
    if (fake->open_rw_fail) {
      errno = EACCES;
      return -1;
    }
  } else if (fake->open_ro_fail) {
    errno = EACCES;
    return -1;
  }
  return fake->fd;
}

static long fake_disp_ioctl(void *user, int fd, unsigned long request,
                            unsigned long *args) {
  (void)fd;
  fake_disp *fake = (fake_disp *)user;
  if (request == 0x103UL) {
    ++fake->get_calls;
    if (fake->get_error) {
      errno = EIO;
      return -1;
    }
    return fake->get_value;
  }
  if (request == 0x102UL) {
    ++fake->set_calls;
    memcpy(fake->last_set, args, 4 * sizeof(args[0]));
    if (fake->set_error) {
      errno = EIO;
      return -1;
    }
    if (!fake->mismatch) {
      fake->get_value = (int)args[1];
    }
    return 0;
  }
  errno = ENOTTY;
  return -1;
}

static int fake_disp_close(void *user, int fd) {
  (void)fd;
  fake_disp *fake = (fake_disp *)user;
  ++fake->close_calls;
  return 0;
}

static bd_av_disp_io make_disp_io(fake_disp *fake) {
  bd_av_disp_io io = {
      .user = fake,
      .open = fake_disp_open,
      .ioctl = fake_disp_ioctl,
      .close = fake_disp_close,
  };
  return io;
}

/* ---------------------------------------------------------------------- */
/* Fake ALSA mixer                                                        */
/* ---------------------------------------------------------------------- */

#define FAKE_MAX_CONTROLS 12

typedef struct fake_control {
  char name[BD_AV_NAME_MAX];
  unsigned int numid;
  unsigned int type;
  unsigned int access;
  unsigned int count;
  unsigned int index;
  long min;
  long max;
  long step;
  long values[4];
  bool read_fail;
  bool write_fail;
  bool corrupt_write;
} fake_control;

typedef struct fake_mixer {
  bool found;
  bool open_fail;
  bool list_fail;
  bool read_all_fail;
  bool write_all_fail;
  bool truncated;
  int card_index;
  char card_id[BD_AV_NAME_MAX];
  int open_calls;
  int list_calls;
  int read_calls;
  int write_calls;
  int close_calls;
  fake_control controls[FAKE_MAX_CONTROLS];
  size_t count;
} fake_mixer;

static fake_control *add_control(fake_mixer *fake, const char *name,
                                 unsigned int type, unsigned int count,
                                 long min, long max, long step,
                                 unsigned int access) {
  if (fake->count >= FAKE_MAX_CONTROLS) {
    return NULL;
  }
  fake_control *control = &fake->controls[fake->count];
  memset(control, 0, sizeof(*control));
  snprintf(control->name, sizeof(control->name), "%s", name);
  control->numid = (unsigned int)fake->count + 1;
  control->type = type;
  control->count = count;
  control->index = 0;
  control->min = min;
  control->max = max;
  control->step = step;
  control->access = access;
  for (unsigned int i = 0; i < count && i < 4; ++i) {
    control->values[i] = type == 1u ? 1 : min;
  }
  ++fake->count;
  return control;
}

static fake_control *find_control(fake_mixer *fake, const char *name) {
  for (size_t i = 0; i < fake->count; ++i) {
    if (strcmp(fake->controls[i].name, name) == 0) {
      return &fake->controls[i];
    }
  }
  return NULL;
}

static bool fake_find_card(void *user, const char *proc_cards,
                           const char *card_name, bool by_index,
                           int requested_index, int *card_index,
                           char *card_id, size_t card_id_size,
                           bd_av_error *error) {
  (void)proc_cards;
  (void)card_name;
  (void)by_index;
  (void)requested_index;
  fake_mixer *fake = (fake_mixer *)user;
  if (!fake->found) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED, "fake card is absent");
    return false;
  }
  *card_index = fake->card_index;
  bd_av_copy_string(card_id, card_id_size, fake->card_id);
  return true;
}

static int fake_open_card(void *user, const char *snd_root, int card_index,
                          const char *card_name, bool by_index,
                          char *card_id, size_t card_id_size,
                          bd_av_error *error) {
  (void)snd_root;
  (void)card_name;
  (void)by_index;
  fake_mixer *fake = (fake_mixer *)user;
  ++fake->open_calls;
  if (fake->open_fail) {
    bd_av_set_error(error, BD_AV_ERR_IO, "fake mixer open failure");
    return -1;
  }
  if (card_index != fake->card_index) {
    bd_av_set_error(error, BD_AV_ERR_IO, "fake card index mismatch");
    return -1;
  }
  bd_av_copy_string(card_id, card_id_size, fake->card_id);
  return 42;
}

static bool fake_list_controls(void *user, int fd,
                               bd_av_mixer_control *controls,
                               size_t capacity, size_t *count,
                               bool *truncated, bd_av_error *error) {
  (void)fd;
  fake_mixer *fake = (fake_mixer *)user;
  ++fake->list_calls;
  if (fake->list_fail) {
    bd_av_set_error(error, BD_AV_ERR_IO, "fake mixer list failure");
    return false;
  }
  size_t used = fake->count < capacity ? fake->count : capacity;
  for (size_t i = 0; i < used; ++i) {
    bd_av_mixer_control *out = &controls[i];
    const fake_control *in = &fake->controls[i];
    memset(out, 0, sizeof(*out));
    out->numid = in->numid;
    out->iface = 2;
    out->index = in->index;
    out->type = in->type;
    out->access = in->access;
    out->count = in->count;
    out->min = in->min;
    out->max = in->max;
    out->step = in->step;
    bd_av_copy_string(out->name, sizeof(out->name), in->name);
  }
  *count = used;
  *truncated = fake->truncated || fake->count > capacity;
  return true;
}

static bool fake_read_control(void *user, int fd,
                              const bd_av_mixer_control *control,
                              long *values, size_t count,
                              bd_av_error *error) {
  (void)fd;
  fake_mixer *fake = (fake_mixer *)user;
  ++fake->read_calls;
  if (fake->read_all_fail) {
    bd_av_set_error(error, BD_AV_ERR_IO, "fake mixer read failure");
    return false;
  }
  fake_control *stored = find_control(fake, control->name);
  if (stored == NULL) {
    bd_av_set_error(error, BD_AV_ERR_IO, "fake control '%s' is absent",
                    control->name);
    return false;
  }
  if (stored->read_fail) {
    bd_av_set_error(error, BD_AV_ERR_IO, "fake control '%s' read failure",
                    control->name);
    return false;
  }
  size_t used = count < stored->count ? count : stored->count;
  for (size_t i = 0; i < used; ++i) {
    values[i] = stored->values[i];
  }
  return true;
}

static bool fake_write_control(void *user, int fd,
                               const bd_av_mixer_control *control,
                               const long *values, size_t count,
                               bd_av_error *error) {
  (void)fd;
  fake_mixer *fake = (fake_mixer *)user;
  ++fake->write_calls;
  if (fake->write_all_fail) {
    bd_av_set_error(error, BD_AV_ERR_IO, "fake mixer write failure");
    return false;
  }
  fake_control *stored = find_control(fake, control->name);
  if (stored == NULL) {
    bd_av_set_error(error, BD_AV_ERR_IO, "fake control '%s' is absent",
                    control->name);
    return false;
  }
  if (stored->write_fail) {
    bd_av_set_error(error, BD_AV_ERR_IO, "fake control '%s' write failure",
                    control->name);
    return false;
  }
  size_t used = count < stored->count ? count : stored->count;
  for (size_t i = 0; i < used; ++i) {
    stored->values[i] = values[i];
  }
  if (stored->corrupt_write && stored->count > 0) {
    stored->values[0] += 1;
  }
  return true;
}

static void fake_close_card(void *user, int fd) {
  (void)fd;
  fake_mixer *fake = (fake_mixer *)user;
  ++fake->close_calls;
}

static bd_av_mixer_io make_mixer_io(fake_mixer *fake) {
  bd_av_mixer_io io = {
      .user = fake,
      .find_card = fake_find_card,
      .open_card = fake_open_card,
      .list_controls = fake_list_controls,
      .read_control = fake_read_control,
      .write_control = fake_write_control,
      .close_card = fake_close_card,
  };
  return io;
}

static void setup_default_mixer(fake_mixer *fake) {
  memset(fake, 0, sizeof(*fake));
  fake->found = true;
  fake->card_index = 0;
  snprintf(fake->card_id, sizeof(fake->card_id), "audiocodec");
  add_control(fake, "digital volume", BD_AV_ALSA_TYPE_INTEGER, 1, 0, 63, 1,
              BD_AV_ALSA_ACCESS_READ | BD_AV_ALSA_ACCESS_WRITE);
  fake_control *speaker =
      add_control(fake, "HpSpeaker Switch", BD_AV_ALSA_TYPE_BOOLEAN, 1, 0, 1,
                  1, BD_AV_ALSA_ACCESS_READ | BD_AV_ALSA_ACCESS_WRITE);
  fake_control *headphone =
      add_control(fake, "Headphone Switch", BD_AV_ALSA_TYPE_BOOLEAN, 1, 0, 1,
                  1, BD_AV_ALSA_ACCESS_READ | BD_AV_ALSA_ACCESS_WRITE);
  add_control(fake, "Headphone Volume", BD_AV_ALSA_TYPE_INTEGER, 1, 0, 7, 1,
              BD_AV_ALSA_ACCESS_READ | BD_AV_ALSA_ACCESS_WRITE);
  if (speaker != NULL) {
    speaker->values[0] = 1;
  }
  if (headphone != NULL) {
    headphone->values[0] = 1;
  }
}

/* ---------------------------------------------------------------------- */
/* Brightness tests                                                       */
/* ---------------------------------------------------------------------- */

static void test_brightness_sysfs(void) {
  char *root = make_temp_dir();
  CHECK(write_fixture(root, "sys/class/backlight/zzz/max_brightness", "255\n"),
        "write zzz max");
  CHECK(write_fixture(root, "sys/class/backlight/zzz/brightness", "128\n"),
        "write zzz value");
  CHECK(write_fixture(root, "sys/class/backlight/aaa/max_brightness", "1000\n"),
        "write aaa max");
  CHECK(write_fixture(root, "sys/class/backlight/aaa/brightness", "250\n"),
        "write aaa value");

  bd_av_brightness_config config;
  memset(&config, 0, sizeof(config));
  config.sysfs_root = root;
  config.disp_path = "/tmp/bd-device-av-no-disp";
  bd_av_brightness brightness;
  bd_av_error error;
  CHECK_RESULT(bd_av_brightness_probe(&brightness, &config, &error),
               BD_AV_OK);
  CHECK(brightness.backend == BD_AV_BRIGHTNESS_SYSFS,
        "expected the sysfs backend, got %s",
        bd_av_brightness_backend_name(brightness.backend));
  CHECK(strstr(brightness.device, "/aaa") != NULL,
        "expected the first sorted backlight, got %s", brightness.device);
  CHECK(brightness.raw_min == 0 && brightness.raw_max == 1000,
        "unexpected range %d..%d", brightness.raw_min, brightness.raw_max);
  CHECK(brightness.readable && brightness.writable,
        "sysfs brightness should be readable/writable");

  int raw = -1;
  CHECK_RESULT(bd_av_brightness_get_raw(&brightness, &raw, &error), BD_AV_OK);
  CHECK(raw == 250, "expected raw 250, got %d", raw);
  int percent = -1;
  CHECK_RESULT(bd_av_brightness_get(&brightness, &percent, &error), BD_AV_OK);
  CHECK(percent == 25, "expected 25%%, got %d%%", percent);

  CHECK_RESULT(bd_av_brightness_set(&brightness, 50, &error), BD_AV_OK);
  char path[512];
  CHECK(join_path(path, sizeof(path), root,
                  "sys/class/backlight/aaa/brightness"),
        "join brightness path");
  char text[32];
  CHECK(read_file(path, text, sizeof(text)), "read brightness after set");
  CHECK(strcmp(text, "500") == 0, "expected raw 500 after 50%%, got '%s'",
        text);
  CHECK_RESULT(bd_av_brightness_get(&brightness, &percent, &error), BD_AV_OK);
  CHECK(percent == 50, "expected 50%%, got %d%%", percent);

  CHECK_RESULT(bd_av_brightness_set_raw(&brightness, 1000, &error), BD_AV_OK);
  CHECK_RESULT(bd_av_brightness_get(&brightness, &percent, &error), BD_AV_OK);
  CHECK(percent == 100, "expected 100%%, got %d%%", percent);

  CHECK_RESULT(bd_av_brightness_set(&brightness, 101, &error),
               BD_AV_ERR_RANGE);
  CHECK(error.code == BD_AV_ERR_RANGE, "range error should be reported");
  CHECK(read_file(path, text, sizeof(text)) && strcmp(text, "1000") == 0,
        "out-of-range percent must not write");
  CHECK_RESULT(bd_av_brightness_set_raw(&brightness, 1001, &error),
               BD_AV_ERR_RANGE);
  CHECK_RESULT(bd_av_brightness_set_raw(&brightness, -1, &error),
               BD_AV_ERR_RANGE);

  /* A directory in place of the brightness file is a write failure. */
  unlink(path);
  CHECK(mkdir(path, 0755) == 0, "replace brightness with a directory");
  CHECK_RESULT(bd_av_brightness_set_raw(&brightness, 10, &error),
               BD_AV_ERR_IO);
  rmdir(path);
  /* A symlink to /dev/null accepts the write but cannot be read back. */
  CHECK(symlink("/dev/null", path) == 0, "create /dev/null symlink");
  CHECK_RESULT(bd_av_brightness_set_raw(&brightness, 10, &error),
               BD_AV_ERR_IO);

  bd_av_brightness_config explicit_config = config;
  explicit_config.backlight = "sys/class/backlight/missing";
  CHECK_RESULT(bd_av_brightness_probe(&brightness, &explicit_config, &error),
               BD_AV_ERR_UNSUPPORTED);
  CHECK(brightness.backend == BD_AV_BRIGHTNESS_NONE,
        "failed probe must leave backend none");

  char *bad_root = make_temp_dir();
  CHECK(write_fixture(bad_root, "sys/class/backlight/bad/max_brightness", "0\n"),
        "write bad max");
  CHECK(write_fixture(bad_root, "sys/class/backlight/bad/brightness", "0\n"),
        "write bad value");
  bd_av_brightness_config bad_config = config;
  bad_config.sysfs_root = bad_root;
  CHECK_RESULT(bd_av_brightness_probe(&brightness, &bad_config, &error),
               BD_AV_ERR_UNSUPPORTED);
  remove_temp_dir(bad_root);

  char *empty_root = make_temp_dir();
  bd_av_brightness_config empty_config = config;
  empty_config.sysfs_root = empty_root;
  CHECK_RESULT(bd_av_brightness_probe(&brightness, &empty_config, &error),
               BD_AV_ERR_UNSUPPORTED);
  CHECK(error.code == BD_AV_ERR_UNSUPPORTED,
        "missing device should report unsupported");
  CHECK_RESULT(bd_av_brightness_set(&brightness, 50, &error),
               BD_AV_ERR_UNSUPPORTED);
  remove_temp_dir(empty_root);
  remove_temp_dir(root);
}

static void test_brightness_disp(void) {
  char *root = make_temp_dir();
  CHECK(write_fixture(root, "etc/model", "TG4040\n"), "write model");
  char model_path[512];
  CHECK(join_path(model_path, sizeof(model_path), root, "etc/model"),
        "join model path");

  fake_disp fake;
  memset(&fake, 0, sizeof(fake));
  fake.fd = 9;
  fake.get_value = 128;
  bd_av_disp_io io = make_disp_io(&fake);

  bd_av_brightness_config config;
  memset(&config, 0, sizeof(config));
  config.backend = BD_AV_BRIGHTNESS_DISP;
  config.model_path = model_path;
  config.disp_path = "/dev/disp-test";
  bd_av_brightness brightness;
  bd_av_error error;
  CHECK_RESULT(bd_av_brightness_probe_with_io(&brightness, &config, &io,
                                              &error),
               BD_AV_OK);
  CHECK(brightness.backend == BD_AV_BRIGHTNESS_DISP,
        "expected disp backend, got %s",
        bd_av_brightness_backend_name(brightness.backend));
  CHECK(strcmp(brightness.model, "TG4040") == 0, "model should be TG4040");
  CHECK(brightness.raw_max == 255, "expected raw max 255, got %d",
        brightness.raw_max);
  CHECK(fake.get_calls == 1 && fake.set_calls == 0,
        "probe must only call GET (get=%d set=%d)", fake.get_calls,
        fake.set_calls);

  int raw = -1;
  CHECK_RESULT(bd_av_brightness_get_raw(&brightness, &raw, &error), BD_AV_OK);
  CHECK(raw == 128, "expected raw 128, got %d", raw);
  int percent = -1;
  CHECK_RESULT(bd_av_brightness_get(&brightness, &percent, &error), BD_AV_OK);
  CHECK(percent == 50, "expected 50%%, got %d%%", percent);

  CHECK_RESULT(bd_av_brightness_set(&brightness, 50, &error), BD_AV_OK);
  CHECK(fake.set_calls == 1, "expected one SET call, got %d",
        fake.set_calls);
  CHECK(fake.last_set[0] == 0 && fake.last_set[1] == 128,
        "SET args should be screen 0 raw 128, got %lu/%lu", fake.last_set[0],
        fake.last_set[1]);
  int after = -1;
  CHECK_RESULT(bd_av_brightness_get(&brightness, &after, &error), BD_AV_OK);
  CHECK(after == 50, "expected 50%% after readback, got %d%%", after);

  int set_before = fake.set_calls;
  CHECK_RESULT(bd_av_brightness_set(&brightness, 101, &error),
               BD_AV_ERR_RANGE);
  CHECK_RESULT(bd_av_brightness_set_raw(&brightness, 256, &error),
               BD_AV_ERR_RANGE);
  CHECK(fake.set_calls == set_before,
        "range errors must not reach /dev/disp");

  fake.get_error = true;
  CHECK_RESULT(bd_av_brightness_probe_with_io(&brightness, &config, &io,
                                              &error),
               BD_AV_ERR_UNSUPPORTED);
  CHECK(fake.set_calls == set_before,
        "a failed GET probe must not write");
  fake.get_error = false;

  fake.open_rw_fail = true;
  fake.open_ro_fail = false;
  fake.get_value = 128;
  CHECK_RESULT(bd_av_brightness_probe_with_io(&brightness, &config, &io,
                                              &error),
               BD_AV_OK);
  CHECK(!brightness.writable, "read-only /dev/disp should not be writable");
  CHECK_RESULT(bd_av_brightness_set(&brightness, 50, &error),
               BD_AV_ERR_UNSUPPORTED);
  CHECK(fake.set_calls == set_before, "read-only disp must not write");
  fake.open_rw_fail = false;

  fake.set_error = true;
  CHECK_RESULT(bd_av_brightness_probe_with_io(&brightness, &config, &io,
                                              &error),
               BD_AV_OK);
  CHECK_RESULT(bd_av_brightness_set(&brightness, 25, &error), BD_AV_ERR_IO);
  fake.set_error = false;

  fake.mismatch = true;
  fake.get_value = 128;
  CHECK_RESULT(bd_av_brightness_probe_with_io(&brightness, &config, &io,
                                              &error),
               BD_AV_OK);
  CHECK_RESULT(bd_av_brightness_set(&brightness, 25, &error), BD_AV_ERR_IO);
  fake.mismatch = false;

  char *unknown_root = make_temp_dir();
  CHECK(write_fixture(unknown_root, "etc/model", "TG9999\n"),
        "write unknown model");
  char unknown_path[512];
  CHECK(join_path(unknown_path, sizeof(unknown_path), unknown_root,
                  "etc/model"),
        "join unknown model");
  bd_av_brightness_config unknown = config;
  unknown.model_path = unknown_path;
  int opens_before = fake.open_calls;
  int sets_before = fake.set_calls;
  CHECK_RESULT(bd_av_brightness_probe_with_io(&brightness, &unknown, &io,
                                              &error),
               BD_AV_ERR_UNSUPPORTED);
  CHECK(fake.open_calls == opens_before && fake.set_calls == sets_before,
        "unknown model must not open or write /dev/disp");
  remove_temp_dir(unknown_root);

  bd_av_brightness_config missing = config;
  missing.model_path = "/tmp/bd-device-av-missing-model";
  CHECK_RESULT(bd_av_brightness_probe_with_io(&brightness, &missing, &io,
                                              &error),
               BD_AV_ERR_UNSUPPORTED);

  bd_av_brightness_config wrong_max = config;
  wrong_max.disp_raw_max = 100;
  fake.get_value = 128;
  CHECK_RESULT(bd_av_brightness_probe_with_io(&brightness, &wrong_max, &io,
                                              &error),
               BD_AV_ERR_UNSUPPORTED);

  bd_av_brightness_config allowed = config;
  allowed.allowed_models = "TG9999,TG4040";
  fake.get_value = 128;
  fake.open_rw_fail = false;
  CHECK_RESULT(bd_av_brightness_probe_with_io(&brightness, &allowed, &io,
                                              &error),
               BD_AV_OK);

  remove_temp_dir(root);
}

/* ---------------------------------------------------------------------- */
/* Audio tests                                                            */
/* ---------------------------------------------------------------------- */

static void open_default_audio(bd_av_audio *audio, fake_mixer *fake,
                               bd_av_mixer_io *io,
                               const char *speaker_mute,
                               bd_av_audio_config *config) {
  memset(config, 0, sizeof(*config));
  config->speaker_mute = speaker_mute;
  setup_default_mixer(fake);
  *io = make_mixer_io(fake);
  bd_av_error error;
  CHECK_RESULT(bd_av_audio_open_with_io(audio, config, io, &error), BD_AV_OK);
}

static void test_audio_volume(void) {
  char *root = make_temp_dir();
  CHECK(write_fixture(root, "speaker/mute", "0\n"), "write speaker mute");
  char mute_path[512];
  CHECK(join_path(mute_path, sizeof(mute_path), root, "speaker/mute"),
        "join mute path");

  fake_mixer fake;
  bd_av_mixer_io io;
  bd_av_audio_config config;
  bd_av_audio audio;
  open_default_audio(&audio, &fake, &io, mute_path, &config);

  CHECK(audio.card_index == 0, "expected card 0, got %d", audio.card_index);
  CHECK(strcmp(audio.card_id, "audiocodec") == 0,
        "expected audiocodec, got '%s'", audio.card_id);
  CHECK(audio.has_volume && audio.volume_inverted, "volume should be bound");
  CHECK(audio.volume_min == 0 && audio.volume_max == 63,
        "unexpected volume range %ld..%ld", audio.volume_min,
        audio.volume_max);
  CHECK(audio.has_mute && !audio.internal.mute_is_alsa,
        "sysfs speaker mute should be bound");
  CHECK(audio.has_route, "both path switches should be bound");
  CHECK(audio.has_headphone_volume, "headphone volume should be enumerated");
  CHECK(bd_av_audio_control_count(&audio) == fake.count,
        "control count mismatch");

  int percent = -1;
  CHECK_RESULT(bd_av_audio_get_volume(&audio, &percent, NULL), BD_AV_OK);
  CHECK(percent == 100, "inverted raw 0 should read 100%%, got %d%%", percent);

  CHECK_RESULT(bd_av_audio_set_volume(&audio, 100, NULL), BD_AV_OK);
  fake_control *volume = find_control(&fake, "digital volume");
  CHECK(volume != NULL && volume->values[0] == 0,
        "100%% should write raw 0");

  CHECK_RESULT(bd_av_audio_set_volume(&audio, 50, NULL), BD_AV_OK);
  CHECK(volume != NULL && volume->values[0] == 31,
        "50%% should write inverted raw 31, got %ld",
        volume == NULL ? -1 : volume->values[0]);
  CHECK_RESULT(bd_av_audio_get_volume(&audio, &percent, NULL), BD_AV_OK);
  CHECK(percent >= 50 && percent <= 51,
        "50%% readback should be 50-51%%, got %d%%", percent);

  CHECK_RESULT(bd_av_audio_set_volume(&audio, 0, NULL), BD_AV_OK);
  CHECK(volume != NULL && volume->values[0] == 63,
        "0%% should write raw 63");
  char text[32];
  CHECK(read_file(mute_path, text, sizeof(text)) && strcmp(text, "1") == 0,
        "0%% should engage speaker mute, got '%s'", text);
  bool muted = false;
  CHECK_RESULT(bd_av_audio_get_muted(&audio, &muted, NULL), BD_AV_OK);
  CHECK(muted, "0%% should read back muted");

  CHECK_RESULT(bd_av_audio_set_volume(&audio, 50, NULL), BD_AV_OK);
  CHECK(read_file(mute_path, text, sizeof(text)) && strcmp(text, "0") == 0,
        "raising volume should clear the automatic mute, got '%s'", text);
  CHECK_RESULT(bd_av_audio_get_muted(&audio, &muted, NULL), BD_AV_OK);
  CHECK(!muted, "50%% should be unmuted");

  CHECK_RESULT(bd_av_audio_set_muted(&audio, true, NULL), BD_AV_OK);
  CHECK(audio.muted, "explicit mute state should be recorded");
  CHECK(read_file(mute_path, text, sizeof(text)) && strcmp(text, "1") == 0,
        "explicit mute should write 1");
  CHECK_RESULT(bd_av_audio_set_volume(&audio, 70, NULL), BD_AV_OK);
  CHECK(read_file(mute_path, text, sizeof(text)) && strcmp(text, "1") == 0,
        "volume change must not clear an explicit mute");
  CHECK_RESULT(bd_av_audio_set_muted(&audio, false, NULL), BD_AV_OK);
  CHECK(read_file(mute_path, text, sizeof(text)) && strcmp(text, "0") == 0,
        "explicit unmute should write 0");

  int writes_before = fake.write_calls;
  CHECK_RESULT(bd_av_audio_set_volume(&audio, 101, NULL), BD_AV_ERR_RANGE);
  CHECK_RESULT(bd_av_audio_set_volume(&audio, -1, NULL), BD_AV_ERR_RANGE);
  CHECK(fake.write_calls == writes_before,
        "out-of-range volume must not write");

  fake.write_all_fail = true;
  long before_failed_write = volume->values[0];
  CHECK_RESULT(bd_av_audio_set_volume(&audio, 40, NULL), BD_AV_ERR_IO);
  CHECK(volume != NULL && volume->values[0] == before_failed_write,
        "failed write must not change the control");
  fake.write_all_fail = false;

  fake.read_all_fail = true;
  CHECK_RESULT(bd_av_audio_get_volume(&audio, &percent, NULL), BD_AV_ERR_IO);
  fake.read_all_fail = false;

  volume->corrupt_write = true;
  CHECK_RESULT(bd_av_audio_set_volume(&audio, 40, NULL), BD_AV_ERR_IO);
  volume->corrupt_write = false;

  bd_av_audio_close(&audio);
  CHECK(fake.close_calls == 1, "close should be called once, got %d",
        fake.close_calls);

  /* No volume control: the card opens but the role is unsupported. */
  memset(&fake, 0, sizeof(fake));
  fake.found = true;
  fake.card_index = 0;
  snprintf(fake.card_id, sizeof(fake.card_id), "audiocodec");
  add_control(&fake, "HpSpeaker Switch", BD_AV_ALSA_TYPE_BOOLEAN, 1, 0, 1, 1,
              BD_AV_ALSA_ACCESS_READ | BD_AV_ALSA_ACCESS_WRITE);
  add_control(&fake, "Headphone Switch", BD_AV_ALSA_TYPE_BOOLEAN, 1, 0, 1, 1,
              BD_AV_ALSA_ACCESS_READ | BD_AV_ALSA_ACCESS_WRITE);
  io = make_mixer_io(&fake);
  memset(&config, 0, sizeof(config));
  config.speaker_mute = mute_path;
  bd_av_error error;
  CHECK_RESULT(bd_av_audio_open_with_io(&audio, &config, &io, &error),
               BD_AV_OK);
  CHECK(!audio.has_volume, "missing volume control must not be invented");
  int no_volume_writes = fake.write_calls;
  CHECK_RESULT(bd_av_audio_set_volume(&audio, 50, &error),
               BD_AV_ERR_UNSUPPORTED);
  CHECK(fake.write_calls == no_volume_writes,
        "unsupported volume must not write");
  bd_av_audio_close(&audio);

  memset(&fake, 0, sizeof(fake));
  fake.found = false;
  io = make_mixer_io(&fake);
  CHECK_RESULT(bd_av_audio_open_with_io(&audio, &config, &io, &error),
               BD_AV_ERR_UNSUPPORTED);
  CHECK(fake.open_calls == 0, "missing card must not open a control device");

  memset(&fake, 0, sizeof(fake));
  fake.found = true;
  fake.card_index = 0;
  fake.open_fail = true;
  snprintf(fake.card_id, sizeof(fake.card_id), "audiocodec");
  io = make_mixer_io(&fake);
  CHECK_RESULT(bd_av_audio_open_with_io(&audio, &config, &io, &error),
               BD_AV_ERR_IO);

  memset(&fake, 0, sizeof(fake));
  fake.found = true;
  fake.card_index = 0;
  fake.list_fail = true;
  snprintf(fake.card_id, sizeof(fake.card_id), "audiocodec");
  io = make_mixer_io(&fake);
  CHECK_RESULT(bd_av_audio_open_with_io(&audio, &config, &io, &error),
               BD_AV_ERR_IO);
  CHECK(fake.close_calls == 1, "list failure should close the mixer");

  remove_temp_dir(root);
}

static void test_audio_mute(void) {
  char *root = make_temp_dir();
  char missing[512];
  CHECK(join_path(missing, sizeof(missing), root, "speaker/missing"),
        "join missing mute path");

  fake_mixer fake;
  bd_av_mixer_io io;
  bd_av_audio_config config;
  bd_av_audio audio;
  open_default_audio(&audio, &fake, &io, missing, &config);
  CHECK(!audio.has_mute, "missing mute path must not be bound");
  bool muted = false;
  bd_av_error error;
  CHECK_RESULT(bd_av_audio_get_muted(&audio, &muted, &error),
               BD_AV_ERR_UNSUPPORTED);
  CHECK_RESULT(bd_av_audio_set_muted(&audio, true, &error),
               BD_AV_ERR_UNSUPPORTED);
  bd_av_audio_close(&audio);

  char mute_path[512];
  CHECK(join_path(mute_path, sizeof(mute_path), root, "speaker/mute"),
        "join mute path");
  CHECK(write_fixture(root, "speaker/mute", "2\n"), "write malformed mute");
  open_default_audio(&audio, &fake, &io, mute_path, &config);
  CHECK(!audio.has_mute, "malformed mute value must not be bound");
  bd_av_audio_close(&audio);

  CHECK(write_fixture(root, "speaker/mute", "0\n"), "write valid mute");
  open_default_audio(&audio, &fake, &io, mute_path, &config);
  CHECK(audio.has_mute, "valid speaker mute should be bound");
  unlink(mute_path);
  CHECK(mkdir(mute_path, 0755) == 0, "replace mute with a directory");
  CHECK_RESULT(bd_av_audio_set_muted(&audio, true, &error), BD_AV_ERR_IO);
  CHECK(!audio.muted, "failed mute write must not update explicit state");
  bd_av_audio_close(&audio);
  rmdir(mute_path);

  /* ALSA mute switch instead of the sysfs path. */
  memset(&fake, 0, sizeof(fake));
  fake.found = true;
  fake.card_index = 0;
  snprintf(fake.card_id, sizeof(fake.card_id), "audiocodec");
  add_control(&fake, "digital volume", BD_AV_ALSA_TYPE_INTEGER, 1, 0, 63, 1,
              BD_AV_ALSA_ACCESS_READ | BD_AV_ALSA_ACCESS_WRITE);
  fake_control *mute_switch =
      add_control(&fake, "Master Mute", BD_AV_ALSA_TYPE_BOOLEAN, 1, 0, 1, 1,
                  BD_AV_ALSA_ACCESS_READ | BD_AV_ALSA_ACCESS_WRITE);
  add_control(&fake, "HpSpeaker Switch", BD_AV_ALSA_TYPE_BOOLEAN, 1, 0, 1, 1,
              BD_AV_ALSA_ACCESS_READ | BD_AV_ALSA_ACCESS_WRITE);
  add_control(&fake, "Headphone Switch", BD_AV_ALSA_TYPE_BOOLEAN, 1, 0, 1, 1,
              BD_AV_ALSA_ACCESS_READ | BD_AV_ALSA_ACCESS_WRITE);
  io = make_mixer_io(&fake);
  memset(&config, 0, sizeof(config));
  config.speaker_mute = missing;
  config.mute_control = "Master Mute";
  CHECK_RESULT(bd_av_audio_open_with_io(&audio, &config, &io, &error),
               BD_AV_OK);
  CHECK(audio.has_mute && audio.internal.mute_is_alsa,
        "configured ALSA mute should be bound");
  CHECK_RESULT(bd_av_audio_set_muted(&audio, true, &error), BD_AV_OK);
  CHECK(mute_switch != NULL && mute_switch->values[0] == 0,
        "muted should write switch 0");
  CHECK_RESULT(bd_av_audio_get_muted(&audio, &muted, &error), BD_AV_OK);
  CHECK(muted, "switch 0 should read as muted");
  CHECK_RESULT(bd_av_audio_set_muted(&audio, false, &error), BD_AV_OK);
  CHECK(mute_switch != NULL && mute_switch->values[0] == 1,
        "unmuted should write switch 1");
  bd_av_audio_close(&audio);

  remove_temp_dir(root);
}

static void test_audio_route(void) {
  char *root = make_temp_dir();
  char mute_path[512];
  CHECK(join_path(mute_path, sizeof(mute_path), root, "speaker/mute"),
        "join mute path");
  CHECK(write_fixture(root, "speaker/mute", "0\n"), "write speaker mute");

  fake_mixer fake;
  bd_av_mixer_io io;
  bd_av_audio_config config;
  bd_av_audio audio;
  open_default_audio(&audio, &fake, &io, mute_path, &config);
  bd_av_error error;

  bd_av_route route = BD_AV_ROUTE_UNKNOWN;
  CHECK_RESULT(bd_av_audio_get_route(&audio, &route, &error), BD_AV_OK);
  CHECK(route == BD_AV_ROUTE_BOTH,
        "both switches start on; expected both, got %s",
        bd_av_route_name(route));

  CHECK_RESULT(bd_av_audio_set_route(&audio, BD_AV_ROUTE_SPEAKER, &error),
               BD_AV_OK);
  fake_control *speaker = find_control(&fake, "HpSpeaker Switch");
  fake_control *headphone = find_control(&fake, "Headphone Switch");
  CHECK(speaker != NULL && speaker->values[0] == 1,
        "speaker route should enable HpSpeaker");
  CHECK(headphone != NULL && headphone->values[0] == 0,
        "speaker route should disable Headphone");
  CHECK_RESULT(bd_av_audio_get_route(&audio, &route, &error), BD_AV_OK);
  CHECK(route == BD_AV_ROUTE_SPEAKER, "expected speaker, got %s",
        bd_av_route_name(route));

  CHECK_RESULT(bd_av_audio_set_route(&audio, BD_AV_ROUTE_HEADPHONE, &error),
               BD_AV_OK);
  CHECK(speaker != NULL && speaker->values[0] == 0,
        "headphone route should disable HpSpeaker");
  CHECK(headphone != NULL && headphone->values[0] == 1,
        "headphone route should enable Headphone");
  CHECK_RESULT(bd_av_audio_get_route(&audio, &route, &error), BD_AV_OK);
  CHECK(route == BD_AV_ROUTE_HEADPHONE, "expected headphone, got %s",
        bd_av_route_name(route));

  int writes_before = fake.write_calls;
  CHECK_RESULT(bd_av_audio_set_route(&audio, BD_AV_ROUTE_AUTO, &error),
               BD_AV_ERR_STATE);
  CHECK(fake.write_calls == writes_before,
        "AUTO without a jack edge must not write");

  CHECK_RESULT(bd_av_audio_set_jack(&audio, true, &error), BD_AV_OK);
  CHECK(audio.jack_known && audio.headphone_plugged,
        "jack state should be recorded");
  CHECK_RESULT(bd_av_audio_get_route(&audio, &route, &error), BD_AV_OK);
  CHECK(route == BD_AV_ROUTE_HEADPHONE,
        "plugging the jack should select headphone, got %s",
        bd_av_route_name(route));
  CHECK_RESULT(bd_av_audio_set_jack(&audio, false, &error), BD_AV_OK);
  CHECK_RESULT(bd_av_audio_get_route(&audio, &route, &error), BD_AV_OK);
  CHECK(route == BD_AV_ROUTE_SPEAKER,
        "unplugging the jack should select speaker, got %s",
        bd_av_route_name(route));

  speaker->write_fail = true;
  CHECK_RESULT(bd_av_audio_set_route(&audio, BD_AV_ROUTE_SPEAKER, &error),
               BD_AV_ERR_IO);
  speaker->write_fail = false;
  headphone->corrupt_write = true;
  CHECK_RESULT(bd_av_audio_set_route(&audio, BD_AV_ROUTE_HEADPHONE, &error),
               BD_AV_ERR_IO);
  headphone->corrupt_write = false;
  bd_av_audio_close(&audio);

  /* Manual routing records the jack but does not move the switches. */
  open_default_audio(&audio, &fake, &io, mute_path, &config);
  config.manual_route = true;
  bd_av_audio_close(&audio);
  setup_default_mixer(&fake);
  io = make_mixer_io(&fake);
  CHECK_RESULT(bd_av_audio_open_with_io(&audio, &config, &io, &error),
               BD_AV_OK);
  CHECK(audio.manual_route, "manual route should be recorded");
  CHECK_RESULT(bd_av_audio_set_jack(&audio, true, &error), BD_AV_OK);
  CHECK_RESULT(bd_av_audio_get_route(&audio, &route, &error), BD_AV_OK);
  CHECK(route == BD_AV_ROUTE_BOTH,
        "manual mode should leave the path unchanged, got %s",
        bd_av_route_name(route));
  bd_av_audio_close(&audio);

  /* One missing switch means route control is unsupported. */
  memset(&fake, 0, sizeof(fake));
  fake.found = true;
  fake.card_index = 0;
  snprintf(fake.card_id, sizeof(fake.card_id), "audiocodec");
  add_control(&fake, "digital volume", BD_AV_ALSA_TYPE_INTEGER, 1, 0, 63, 1,
              BD_AV_ALSA_ACCESS_READ | BD_AV_ALSA_ACCESS_WRITE);
  add_control(&fake, "HpSpeaker Switch", BD_AV_ALSA_TYPE_BOOLEAN, 1, 0, 1, 1,
              BD_AV_ALSA_ACCESS_READ | BD_AV_ALSA_ACCESS_WRITE);
  io = make_mixer_io(&fake);
  memset(&config, 0, sizeof(config));
  config.speaker_mute = mute_path;
  CHECK_RESULT(bd_av_audio_open_with_io(&audio, &config, &io, &error),
               BD_AV_OK);
  CHECK(!audio.has_route, "one switch is not a route capability");
  CHECK_RESULT(bd_av_audio_set_route(&audio, BD_AV_ROUTE_SPEAKER, &error),
               BD_AV_ERR_UNSUPPORTED);
  CHECK_RESULT(bd_av_audio_set_jack(&audio, true, &error),
               BD_AV_ERR_UNSUPPORTED);
  CHECK(audio.jack_known && audio.headphone_plugged,
        "jack state should be recorded even without route hardware");
  bd_av_audio_close(&audio);

  remove_temp_dir(root);
}

static void test_audio_enumeration(void) {
  fake_mixer fake;
  memset(&fake, 0, sizeof(fake));
  fake.found = true;
  fake.card_index = 0;
  fake.truncated = true;
  snprintf(fake.card_id, sizeof(fake.card_id), "audiocodec");
  add_control(&fake, "digital volume", BD_AV_ALSA_TYPE_INTEGER, 1, 0, 63, 1,
              BD_AV_ALSA_ACCESS_READ | BD_AV_ALSA_ACCESS_WRITE);
  add_control(&fake, "Inactive", BD_AV_ALSA_TYPE_INTEGER, 1, 0, 10, 1,
              BD_AV_ALSA_ACCESS_INACTIVE);
  add_control(&fake, "Enum", BD_AV_ALSA_TYPE_ENUMERATED, 1, 0, 2, 1,
              BD_AV_ALSA_ACCESS_READ | BD_AV_ALSA_ACCESS_WRITE);
  add_control(&fake, "Big", BD_AV_ALSA_TYPE_INTEGER64, 2, 0, 1000, 1,
              BD_AV_ALSA_ACCESS_READ | BD_AV_ALSA_ACCESS_WRITE);
  bd_av_mixer_io io = make_mixer_io(&fake);
  bd_av_audio_config config;
  memset(&config, 0, sizeof(config));
  config.speaker_mute = "";
  bd_av_audio audio;
  bd_av_error error;
  CHECK_RESULT(bd_av_audio_open_with_io(&audio, &config, &io, &error),
               BD_AV_OK);
  CHECK(audio.controls_truncated, "truncation flag should be reported");
  CHECK(bd_av_audio_control_count(&audio) == fake.count,
        "expected %zu controls, got %zu", fake.count,
        bd_av_audio_control_count(&audio));
  const bd_av_control_info *inactive = bd_av_audio_control_at(&audio, 1);
  CHECK(inactive != NULL && strcmp(inactive->name, "Inactive") == 0,
        "control 1 should be Inactive");
  CHECK(inactive != NULL && !inactive->readable && !inactive->writable,
        "inactive control must not look usable");
  const bd_av_control_info *enumerated = bd_av_audio_control_at(&audio, 2);
  CHECK(enumerated != NULL && enumerated->type == BD_AV_CONTROL_ENUMERATED,
        "control 2 should be enumerated");
  const bd_av_control_info *big = bd_av_audio_control_at(&audio, 3);
  CHECK(big != NULL && big->type == BD_AV_CONTROL_INTEGER64 &&
            big->count == 2 && big->max == 1000,
        "integer64 control metadata");
  CHECK(bd_av_audio_control_at(&audio, 99) == NULL,
        "out-of-range control should be NULL");
  CHECK(bd_av_audio_control_count(NULL) == 0,
        "NULL audio should have no controls");
  CHECK(bd_av_audio_control_at(NULL, 0) == NULL,
        "NULL audio control lookup");
  bd_av_audio_close(&audio);
}

static void test_card_parse(void) {
  char *root = make_temp_dir();
  char cards[512];
  CHECK(join_path(cards, sizeof(cards), root, "cards"), "join cards path");
  CHECK(write_file(cards,
                   " 0 [audiocodec     ]: sunxi-codec - audiocodec\n"
                   "                      sunxi-codec\n"
                   " 1 [other          ]: other - card\n"),
        "write cards file");
  int index = -1;
  char id[BD_AV_NAME_MAX];
  bd_av_error error;
  CHECK(bd_av_alsa_find_card(cards, "audiocodec", false, -1, &index, id,
                             sizeof(id), &error),
        "find audiocodec: %s", error.message);
  CHECK(index == 0 && strcmp(id, "audiocodec") == 0,
        "expected card 0 audiocodec, got %d '%s'", index, id);
  CHECK(bd_av_alsa_find_card(cards, "unused", true, 1, &index, id,
                             sizeof(id), &error),
        "find by index");
  CHECK(index == 1 && strcmp(id, "other") == 0,
        "expected card 1 other, got %d '%s'", index, id);
  CHECK(!bd_av_alsa_find_card(cards, "missing", false, -1, &index, id,
                              sizeof(id), &error),
        "missing card should fail");
  CHECK(error.code == BD_AV_ERR_UNSUPPORTED,
        "missing card should be unsupported");
  CHECK(!bd_av_alsa_find_card(cards, "unused", true, 9, &index, id,
                              sizeof(id), &error),
        "missing index should fail");
  CHECK(!bd_av_alsa_find_card("/tmp/bd-device-av-missing-cards", "audiocodec",
                              false, -1, &index, id, sizeof(id), &error),
        "missing cards file should fail");
  CHECK(error.code == BD_AV_ERR_IO, "missing cards file should be I/O");
  remove_temp_dir(root);
}

static void test_api_guards(void) {
  bd_av_error error;
  bd_av_error_clear(&error);
  CHECK_RESULT(bd_av_brightness_probe(NULL, NULL, &error),
               BD_AV_ERR_ARGUMENT);
  int percent = 0;
  CHECK_RESULT(bd_av_brightness_get(NULL, &percent, &error),
               BD_AV_ERR_ARGUMENT);
  CHECK_RESULT(bd_av_brightness_set(NULL, 50, &error), BD_AV_ERR_ARGUMENT);
  CHECK_RESULT(bd_av_audio_open(NULL, NULL, &error), BD_AV_ERR_ARGUMENT);
  CHECK_RESULT(bd_av_audio_get_volume(NULL, &percent, &error),
               BD_AV_ERR_ARGUMENT);
  CHECK_RESULT(bd_av_audio_set_volume(NULL, 50, &error), BD_AV_ERR_ARGUMENT);
  bd_av_audio_close(NULL);

  CHECK(strcmp(bd_av_result_name(BD_AV_OK), "ok") == 0,
        "result name for OK");
  CHECK(strcmp(bd_av_route_name(BD_AV_ROUTE_AUTO), "auto") == 0,
        "route name for auto");
  CHECK(strcmp(bd_av_brightness_backend_name(BD_AV_BRIGHTNESS_DISP),
               "disp") == 0,
        "backend name for disp");
  CHECK(strcmp(bd_av_control_type_name(BD_AV_CONTROL_INTEGER64),
               "integer64") == 0,
        "control type name");

  char small[4];
  CHECK(!bd_av_copy_string(small, sizeof(small), "toolong"),
        "truncation should be reported");
  CHECK(strcmp(small, "too") == 0, "truncated string should be bounded");
}

int main(void) {
  test_api_guards();
  test_brightness_sysfs();
  test_brightness_disp();
  test_audio_volume();
  test_audio_mute();
  test_audio_route();
  test_audio_enumeration();
  test_card_parse();

  if (g_failures != 0) {
    fprintf(stderr, "device_av_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("device_av_test: all checks passed\n");
  return 0;
}
