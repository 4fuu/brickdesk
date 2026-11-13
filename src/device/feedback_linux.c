#define _POSIX_C_SOURCE 200809L

#include "device_feedback_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BD_FEEDBACK_BITS_PER_LONG (8 * sizeof(unsigned long))
#define BD_FEEDBACK_BITS_TO_LONGS(bits) \
  (((bits) + BD_FEEDBACK_BITS_PER_LONG - 1) / BD_FEEDBACK_BITS_PER_LONG)

static int real_evdev_open(void *user, const char *path, int flags) {
  (void)user;
  return open(path, flags | O_CLOEXEC);
}

static int real_evdev_close(void *user, int fd) {
  (void)user;
  return close(fd);
}

static int real_evdev_ioctl(void *user, int fd, unsigned long request,
                            void *arg) {
  (void)user;
  return ioctl(fd, request, arg);
}

static ssize_t real_evdev_write(void *user, int fd, const void *data,
                                size_t size) {
  (void)user;
  return write(fd, data, size);
}

const bd_feedback_evdev_io *bd_feedback_evdev_io_default(void) {
  static const bd_feedback_evdev_io io = {
      .user = NULL,
      .open = real_evdev_open,
      .close = real_evdev_close,
      .ioctl = real_evdev_ioctl,
      .write = real_evdev_write,
  };
  return &io;
}

static const char *rumble_root(const bd_feedback_rumble_config *config) {
  if (config == NULL || config->sysfs_root == NULL) {
    return "";
  }
  return config->sysfs_root;
}

static int rumble_max_percent(const bd_feedback_rumble_config *config) {
  if (config == NULL || config->max_percent == 0) {
    return 100;
  }
  return config->max_percent;
}

static int rumble_max_duration(const bd_feedback_rumble_config *config) {
  if (config == NULL || config->max_duration_ms == 0) {
    return 1000;
  }
  return config->max_duration_ms;
}

static bool test_bit(const unsigned long *bits, unsigned int bit) {
  return (bits[bit / BD_FEEDBACK_BITS_PER_LONG] >>
          (bit % BD_FEEDBACK_BITS_PER_LONG)) &
         1UL;
}

static void fill_vendor_motor(bd_feedback_rumble *rumble, const char *root) {
  char voltage[BD_FEEDBACK_PATH_MAX];
  char enable[BD_FEEDBACK_PATH_MAX];
  rumble->vendor_motor_voltage_present = false;
  rumble->vendor_motor_enable_present = false;
  rumble->vendor_blocker[0] = '\0';

  if (!bd_feedback_path_join(root, "/sys/class/motor/voltage", voltage,
                             sizeof(voltage)) ||
      !bd_feedback_path_join(root, "/sys/class/gpio/gpio227/value", enable,
                             sizeof(enable))) {
    return;
  }
  rumble->vendor_motor_voltage_present =
      bd_feedback_file_exists(voltage);
  rumble->vendor_motor_enable_present = bd_feedback_file_exists(enable);
  if (rumble->vendor_motor_voltage_present ||
      rumble->vendor_motor_enable_present) {
    bd_feedback_copy_string(rumble->vendor_blocker,
                            sizeof(rumble->vendor_blocker),
                            bd_feedback_vendor_motor_blocker());
  }
}

static void close_fd(const bd_feedback_evdev_io *io, int *fd) {
  if (*fd >= 0) {
    io->close(io->user, *fd);
    *fd = -1;
  }
}

static bd_feedback_result fail_evdev(
    bd_feedback_rumble *rumble, const bd_feedback_evdev_io *io,
    bd_feedback_result result, bd_feedback_error *error, const char *format,
    ...) {
  va_list args;
  va_start(args, format);
  if (error != NULL) {
    error->code = result;
    vsnprintf(error->message, sizeof(error->message), format, args);
  }
  va_end(args);
  close_fd(io, &rumble->internal.fd);
  rumble->internal.effect_id = -1;
  rumble->active = false;
  rumble->backend = BD_FEEDBACK_RUMBLE_NONE;
  return result;
}

bd_feedback_result bd_feedback_rumble_probe_with_io(
    bd_feedback_rumble *rumble, const bd_feedback_rumble_config *config,
    const bd_feedback_evdev_io *evdev_io, bd_feedback_error *error) {
  bd_feedback_error_clear(error);
  if (rumble == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT, "rumble is NULL");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  memset(rumble, 0, sizeof(*rumble));
  rumble->backend = BD_FEEDBACK_RUMBLE_NONE;
  rumble->internal.fd = -1;
  rumble->internal.effect_id = -1;
  if (evdev_io == NULL) {
    evdev_io = bd_feedback_evdev_io_default();
  }
  const char *root = rumble_root(config);
  fill_vendor_motor(rumble, root);

  int max_percent = rumble_max_percent(config);
  int max_duration = rumble_max_duration(config);
  if (max_percent < 0 || max_percent > 100) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "rumble max_percent must be 0..100");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  if (max_duration < 1 || max_duration > 60000) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "rumble max_duration_ms must be 1..60000");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  bd_feedback_rumble_backend backend =
      config == NULL ? BD_FEEDBACK_RUMBLE_AUTO : config->backend;
  if (backend != BD_FEEDBACK_RUMBLE_AUTO &&
      backend != BD_FEEDBACK_RUMBLE_EVDEV &&
      backend != BD_FEEDBACK_RUMBLE_NONE) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "unknown rumble backend");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  if (backend == BD_FEEDBACK_RUMBLE_NONE) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                          "rumble disabled by configuration");
    return BD_FEEDBACK_ERR_UNSUPPORTED;
  }
  const char *event = config == NULL ? NULL : config->event;
  if (event == NULL || event[0] == '\0') {
    bd_feedback_set_error(
        error, BD_FEEDBACK_ERR_UNSUPPORTED,
        "no rumble event node selected; set event to the FF-capable "
        "/dev/input/eventN");
    return BD_FEEDBACK_ERR_UNSUPPORTED;
  }

  int fd = evdev_io->open(evdev_io->user, event, O_RDWR);
  int open_errno = errno;
  if (fd < 0) {
    if (open_errno == ENOENT || open_errno == ENODEV) {
      bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                            "rumble event node %s is not present", event);
      return BD_FEEDBACK_ERR_UNSUPPORTED;
    }
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO,
                          "cannot open rumble event node %s: %s", event,
                          strerror(open_errno));
    return BD_FEEDBACK_ERR_IO;
  }
  rumble->internal.fd = fd;

  unsigned long types[BD_FEEDBACK_BITS_TO_LONGS(EV_CNT)];
  memset(types, 0, sizeof(types));
  if (evdev_io->ioctl(evdev_io->user, fd, EVIOCGBIT(0, sizeof(types)),
                      types) < 0) {
    int ioctl_errno = errno;
    if (ioctl_errno == ENOTTY || ioctl_errno == EINVAL) {
      return fail_evdev(rumble, evdev_io, BD_FEEDBACK_ERR_UNSUPPORTED, error,
                        "%s is not an evdev node", event);
    }
    return fail_evdev(rumble, evdev_io, BD_FEEDBACK_ERR_IO, error,
                      "EVIOCGBIT(0) failed on %s: %s", event,
                      strerror(ioctl_errno));
  }
  if (!test_bit(types, EV_FF)) {
    return fail_evdev(rumble, evdev_io, BD_FEEDBACK_ERR_UNSUPPORTED, error,
                      "%s does not advertise EV_FF", event);
  }

  unsigned long ff_bits[BD_FEEDBACK_BITS_TO_LONGS(FF_CNT)];
  memset(ff_bits, 0, sizeof(ff_bits));
  if (evdev_io->ioctl(evdev_io->user, fd,
                      EVIOCGBIT(EV_FF, sizeof(ff_bits)), ff_bits) < 0) {
    int ioctl_errno = errno;
    return fail_evdev(rumble, evdev_io, BD_FEEDBACK_ERR_IO, error,
                      "EVIOCGBIT(EV_FF) failed on %s: %s", event,
                      strerror(ioctl_errno));
  }
  if (!test_bit(ff_bits, FF_RUMBLE)) {
    return fail_evdev(rumble, evdev_io, BD_FEEDBACK_ERR_UNSUPPORTED, error,
                      "%s does not advertise FF_RUMBLE", event);
  }

  int effects = 0;
  if (evdev_io->ioctl(evdev_io->user, fd, EVIOCGEFFECTS, &effects) < 0) {
    int ioctl_errno = errno;
    return fail_evdev(rumble, evdev_io, BD_FEEDBACK_ERR_IO, error,
                      "EVIOCGEFFECTS failed on %s: %s", event,
                      strerror(ioctl_errno));
  }
  if (effects <= 0) {
    return fail_evdev(rumble, evdev_io, BD_FEEDBACK_ERR_UNSUPPORTED, error,
                      "%s reports no free force-feedback effect slots", event);
  }

  char name[BD_FEEDBACK_NAME_MAX];
  memset(name, 0, sizeof(name));
  if (evdev_io->ioctl(evdev_io->user, fd, EVIOCGNAME(sizeof(name)), name) <
      0) {
    name[0] = '\0';
  } else {
    name[sizeof(name) - 1] = '\0';
    bd_feedback_trim(name);
  }

  if (!bd_feedback_copy_string(rumble->event, sizeof(rumble->event), event) ||
      !bd_feedback_copy_string(rumble->device_name,
                               sizeof(rumble->device_name), name)) {
    return fail_evdev(rumble, evdev_io, BD_FEEDBACK_ERR_ARGUMENT, error,
                      "rumble event path or name is too long");
  }
  rumble->backend = BD_FEEDBACK_RUMBLE_EVDEV;
  rumble->effects = effects;
  rumble->has_ff_rumble = true;
  rumble->max_percent = max_percent;
  rumble->max_duration_ms = max_duration;
  rumble->internal.evdev_io = evdev_io;
  rumble->internal.fd = fd;
  rumble->internal.effect_id = -1;
  rumble->active = false;
  return BD_FEEDBACK_OK;
}

bd_feedback_result bd_feedback_rumble_probe(
    bd_feedback_rumble *rumble, const bd_feedback_rumble_config *config,
    bd_feedback_error *error) {
  return bd_feedback_rumble_probe_with_io(rumble, config,
                                         bd_feedback_evdev_io_default(), error);
}

static ssize_t write_event(const bd_feedback_evdev_io *io,
                           bd_feedback_rumble *rumble,
                           const struct input_event *event) {
  ssize_t written;
  do {
    written = io->write(io->user, rumble->internal.fd, event, sizeof(*event));
  } while (written < 0 && errno == EINTR);
  return written;
}

static bd_feedback_result erase_effect(bd_feedback_rumble *rumble,
                                       bd_feedback_error *error,
                                       const char *context) {
  const bd_feedback_evdev_io *io =
      (const bd_feedback_evdev_io *)rumble->internal.evdev_io;
  if (io == NULL || rumble->internal.fd < 0) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_STATE,
                          "%s: rumble is not open", context);
    return BD_FEEDBACK_ERR_STATE;
  }
  if (rumble->internal.effect_id < 0) {
    rumble->active = false;
    return BD_FEEDBACK_OK;
  }
  int effect_id = rumble->internal.effect_id;
  if (io->ioctl(io->user, rumble->internal.fd, EVIOCRMFF,
                (void *)(intptr_t)effect_id) < 0) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO,
                          "%s: EVIOCRMFF failed for effect %d: %s", context,
                          effect_id, strerror(errno));
    return BD_FEEDBACK_ERR_IO;
  }
  rumble->internal.effect_id = -1;
  rumble->active = false;
  return BD_FEEDBACK_OK;
}

static bd_feedback_result stop_effect(bd_feedback_rumble *rumble,
                                      bd_feedback_error *error,
                                      const char *context) {
  const bd_feedback_evdev_io *io =
      (const bd_feedback_evdev_io *)rumble->internal.evdev_io;
  if (io == NULL || rumble->internal.fd < 0) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_STATE,
                          "%s: rumble is not open", context);
    return BD_FEEDBACK_ERR_STATE;
  }
  if (rumble->internal.effect_id < 0) {
    rumble->active = false;
    return BD_FEEDBACK_OK;
  }

  struct input_event stop;
  memset(&stop, 0, sizeof(stop));
  stop.type = EV_FF;
  stop.code = (unsigned short)rumble->internal.effect_id;
  stop.value = 0;
  ssize_t stop_written = write_event(io, rumble, &stop);
  bool write_ok = stop_written == (ssize_t)sizeof(stop);
  int write_errno = stop_written < 0 ? errno : EIO;
  bd_feedback_error erase_error;
  bd_feedback_error_clear(&erase_error);
  bd_feedback_result erase_result =
      erase_effect(rumble, &erase_error, context);
  if (!write_ok || erase_result != BD_FEEDBACK_OK) {
    bd_feedback_set_error(
        error, BD_FEEDBACK_ERR_IO, "%s: stop%s%s%s%s", context,
        write_ok ? "" : ": EV_FF stop failed: ",
        write_ok ? "" : strerror(write_errno),
        erase_result == BD_FEEDBACK_OK ? "" : "; ",
        erase_result == BD_FEEDBACK_OK
            ? ""
            : (erase_error.message[0] != '\0' ? erase_error.message
                                              : "erase failed"));
    return BD_FEEDBACK_ERR_IO;
  }
  return BD_FEEDBACK_OK;
}

bd_feedback_result bd_feedback_rumble_play(bd_feedback_rumble *rumble,
                                           int strength, int duration_ms,
                                           bd_feedback_error *error) {
  bd_feedback_error_clear(error);
  if (rumble == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT, "rumble is NULL");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  const bd_feedback_evdev_io *io =
      (const bd_feedback_evdev_io *)rumble->internal.evdev_io;
  if (io == NULL || rumble->internal.fd < 0 ||
      rumble->backend != BD_FEEDBACK_RUMBLE_EVDEV) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_STATE,
                          "rumble is not open");
    return BD_FEEDBACK_ERR_STATE;
  }
  if (strength < 0 || strength > 100) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_RANGE,
                          "rumble strength %d is outside 0..100", strength);
    return BD_FEEDBACK_ERR_RANGE;
  }
  if (strength > rumble->max_percent) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_RANGE,
                          "rumble strength %d exceeds configured maximum %d",
                          strength, rumble->max_percent);
    return BD_FEEDBACK_ERR_RANGE;
  }
  if (duration_ms < 1 || duration_ms > rumble->max_duration_ms) {
    bd_feedback_set_error(
        error, BD_FEEDBACK_ERR_RANGE,
        "rumble duration %d ms is outside 1..%d (configured maximum)",
        duration_ms, rumble->max_duration_ms);
    return BD_FEEDBACK_ERR_RANGE;
  }
  if (strength == 0) {
    return bd_feedback_rumble_stop(rumble, error);
  }

  if (rumble->internal.effect_id >= 0) {
    bd_feedback_result stopped = bd_feedback_rumble_stop(rumble, error);
    if (stopped != BD_FEEDBACK_OK) {
      return stopped;
    }
  }

  struct ff_effect effect;
  memset(&effect, 0, sizeof(effect));
  effect.type = FF_RUMBLE;
  effect.id = -1;
  effect.direction = 0;
  effect.trigger.button = 0;
  effect.trigger.interval = 0;
  effect.replay.length = (unsigned short)duration_ms;
  effect.replay.delay = 0;
  unsigned int magnitude =
      (unsigned int)(((long long)strength * 65535 + 50) / 100);
  effect.u.rumble.strong_magnitude = (unsigned short)magnitude;
  effect.u.rumble.weak_magnitude = (unsigned short)magnitude;

  if (io->ioctl(io->user, rumble->internal.fd, EVIOCSFF, &effect) < 0) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO,
                          "EVIOCSFF failed on %s: %s", rumble->event,
                          strerror(errno));
    return BD_FEEDBACK_ERR_IO;
  }
  if (effect.id < 0) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO,
                          "EVIOCSFF returned an invalid effect id %d",
                          effect.id);
    return BD_FEEDBACK_ERR_IO;
  }
  rumble->internal.effect_id = effect.id;

  struct input_event play;
  memset(&play, 0, sizeof(play));
  play.type = EV_FF;
  play.code = (unsigned short)effect.id;
  play.value = 1;
  ssize_t play_written = write_event(io, rumble, &play);
  if (play_written != (ssize_t)sizeof(play)) {
    int write_errno = play_written < 0 ? errno : EIO;
    bd_feedback_error cleanup_error;
    bd_feedback_error_clear(&cleanup_error);
    bd_feedback_result cleanup =
        bd_feedback_rumble_stop(rumble, &cleanup_error);
    bd_feedback_set_error(
        error, BD_FEEDBACK_ERR_IO, "EV_FF play failed on %s: %s%s%s",
        rumble->event, strerror(write_errno),
        cleanup == BD_FEEDBACK_OK ? "" : "; cleanup: ",
        cleanup == BD_FEEDBACK_OK
            ? ""
            : (cleanup_error.message[0] != '\0' ? cleanup_error.message
                                                : "effect erase failed"));
    return BD_FEEDBACK_ERR_IO;
  }
  rumble->active = true;
  return BD_FEEDBACK_OK;
}

bd_feedback_result bd_feedback_rumble_stop(bd_feedback_rumble *rumble,
                                           bd_feedback_error *error) {
  bd_feedback_error_clear(error);
  if (rumble == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT, "rumble is NULL");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  if (rumble->internal.fd < 0) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_STATE,
                          "rumble is not open");
    return BD_FEEDBACK_ERR_STATE;
  }
  return stop_effect(rumble, error, "rumble stop");
}

bd_feedback_result bd_feedback_rumble_close(bd_feedback_rumble *rumble,
                                            bd_feedback_error *error) {
  bd_feedback_error_clear(error);
  if (rumble == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT, "rumble is NULL");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  const bd_feedback_evdev_io *io =
      (const bd_feedback_evdev_io *)rumble->internal.evdev_io;
  if (io == NULL || rumble->internal.fd < 0) {
    rumble->active = false;
    rumble->internal.effect_id = -1;
    rumble->backend = BD_FEEDBACK_RUMBLE_NONE;
    return BD_FEEDBACK_OK;
  }

  bd_feedback_error stop_error;
  bd_feedback_error_clear(&stop_error);
  bd_feedback_result stop_result = stop_effect(rumble, &stop_error, "close");
  int close_result = io->close(io->user, rumble->internal.fd);
  int close_errno = errno;
  rumble->internal.fd = -1;
  rumble->internal.effect_id = -1;
  rumble->active = false;
  rumble->backend = BD_FEEDBACK_RUMBLE_NONE;
  if (stop_result != BD_FEEDBACK_OK) {
    bd_feedback_set_error(
        error, BD_FEEDBACK_ERR_IO, "%s%s%s",
        stop_error.message[0] != '\0' ? stop_error.message
                                      : "rumble stop failed",
        close_result == 0 ? "" : "; close failed: ",
        close_result == 0 ? "" : strerror(close_errno));
    return BD_FEEDBACK_ERR_IO;
  }
  if (close_result != 0) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO,
                          "cannot close rumble event node: %s",
                          strerror(close_errno));
    return BD_FEEDBACK_ERR_IO;
  }
  return BD_FEEDBACK_OK;
}

/* ---------------------------------------------------------------------- */
/* TG4040 vendor LED animation                                             */
/* ---------------------------------------------------------------------- */

const char *bd_feedback_led_zone_name(bd_feedback_led_zone zone) {
  switch (zone) {
    case BD_FEEDBACK_LED_ZONE_M:
      return "top bar";
    case BD_FEEDBACK_LED_ZONE_LR:
      return "stick rings";
    case BD_FEEDBACK_LED_ZONE_F1:
      return "F1";
    case BD_FEEDBACK_LED_ZONE_F2:
      return "F2";
    case BD_FEEDBACK_LED_ZONE_REAR:
      return "triggers";
    case BD_FEEDBACK_LED_ZONE_COUNT:
      break;
  }
  return "unknown";
}

const char *bd_feedback_led_zone_suffix(bd_feedback_led_zone zone) {
  switch (zone) {
    case BD_FEEDBACK_LED_ZONE_M:
      return "m";
    case BD_FEEDBACK_LED_ZONE_LR:
      return "lr";
    case BD_FEEDBACK_LED_ZONE_F1:
      return "f1";
    case BD_FEEDBACK_LED_ZONE_F2:
      return "f2";
    case BD_FEEDBACK_LED_ZONE_REAR:
      return "rear";
    case BD_FEEDBACK_LED_ZONE_COUNT:
      break;
  }
  return "";
}

static bool led_anim_node(const bd_feedback_led_anim *led, const char *name,
                          char *out, size_t out_size) {
  char relative[BD_FEEDBACK_PATH_MAX];
  int written = snprintf(relative, sizeof(relative),
                         "/sys/class/led_anim/%s", name);
  if (written <= 0 || (size_t)written >= sizeof(relative)) {
    return false;
  }
  return bd_feedback_path_join(led->root, relative, out, out_size);
}

static bool write_led_anim_node(const bd_feedback_led_anim *led,
                                const char *name, const char *text,
                                bd_feedback_error *error,
                                bd_feedback_result failure) {
  char path[BD_FEEDBACK_PATH_MAX];
  if (!led_anim_node(led, name, path, sizeof(path))) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "led_anim node name is too long: %s", name);
    return false;
  }
  if (!bd_feedback_write_text(path, text)) {
    bd_feedback_set_error(error, failure, "cannot write %s", path);
    return false;
  }
  return true;
}

bd_feedback_result bd_feedback_led_anim_probe(
    bd_feedback_led_anim *led, const bd_feedback_led_anim_config *config,
    bd_feedback_error *error) {
  bd_feedback_error_clear(error);
  if (led == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT, "led is NULL");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  memset(led, 0, sizeof(*led));
  const char *root = config != NULL && config->sysfs_root != NULL
                         ? config->sysfs_root
                         : "";
  int max_brightness =
      config != NULL && config->max_brightness != 0 ? config->max_brightness
                                                    : 100;
  if (max_brightness < 1 || max_brightness > 100) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "led_anim max_brightness must be 1..100");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  if (!bd_feedback_copy_string(led->root, sizeof(led->root), root)) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "led_anim sysfs root is too long");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  led->max_brightness = max_brightness;

  char directory[BD_FEEDBACK_PATH_MAX];
  if (!bd_feedback_path_join(root, "/sys/class/led_anim", directory,
                             sizeof(directory)) ||
      !bd_feedback_dir_exists(directory)) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                          "%s is not present", directory);
    return BD_FEEDBACK_ERR_UNSUPPORTED;
  }

  for (int i = 0; i < BD_FEEDBACK_LED_ZONE_COUNT; ++i) {
    char name[32];
    snprintf(name, sizeof(name), "effect_%s",
             bd_feedback_led_zone_suffix((bd_feedback_led_zone)i));
    char path[BD_FEEDBACK_PATH_MAX];
    if (led_anim_node(led, name, path, sizeof(path)) &&
        bd_feedback_file_exists(path)) {
      led->zone_present[i] = true;
    }
  }
  char shared[BD_FEEDBACK_PATH_MAX];
  led->f1f2_shared =
      led_anim_node(led, "max_scale_f1f2", shared, sizeof(shared)) &&
      bd_feedback_file_exists(shared);

  led->available = false;
  for (int i = 0; i < BD_FEEDBACK_LED_ZONE_COUNT; ++i) {
    if (led->zone_present[i]) {
      led->available = true;
      break;
    }
  }
  if (!led->available) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                          "no led_anim zone nodes under %s", directory);
    return BD_FEEDBACK_ERR_UNSUPPORTED;
  }
  return BD_FEEDBACK_OK;
}

bool bd_feedback_led_anim_has_zone(const bd_feedback_led_anim *led,
                                   bd_feedback_led_zone zone) {
  if (led == NULL || !led->available || zone < 0 ||
      zone >= BD_FEEDBACK_LED_ZONE_COUNT) {
    return false;
  }
  return led->zone_present[zone];
}

static bool led_anim_brightness_node(const bd_feedback_led_anim *led,
                                     bd_feedback_led_zone zone, char *out,
                                     size_t out_size) {
  switch (zone) {
    case BD_FEEDBACK_LED_ZONE_M:
      return led_anim_node(led, "max_scale", out, out_size);
    case BD_FEEDBACK_LED_ZONE_F1:
      return led_anim_node(led, "max_scale_f1f2", out, out_size);
    case BD_FEEDBACK_LED_ZONE_F2:
      return false; /* shared with f1; f1 owns the brightness write */
    case BD_FEEDBACK_LED_ZONE_LR:
    case BD_FEEDBACK_LED_ZONE_REAR: {
      char name[32];
      snprintf(name, sizeof(name), "max_scale_%s",
               bd_feedback_led_zone_suffix(zone));
      return led_anim_node(led, name, out, out_size);
    }
    case BD_FEEDBACK_LED_ZONE_COUNT:
      break;
  }
  return false;
}

static bd_feedback_result apply_led_anim_zone(
    bd_feedback_led_anim *led, bd_feedback_led_zone zone,
    const bd_feedback_led_anim_settings *settings, bd_feedback_error *error) {
  const char *suffix = bd_feedback_led_zone_suffix(zone);
  char text[64];

  if (settings->brightness < 0 ||
      settings->brightness > led->max_brightness) {
    bd_feedback_set_error(
        error, BD_FEEDBACK_ERR_RANGE,
        "led_anim brightness %d is outside 0..%d", settings->brightness,
        led->max_brightness);
    return BD_FEEDBACK_ERR_RANGE;
  }
  if (settings->effect < 0 || settings->effect > 255) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_RANGE,
                          "led_anim effect %d is outside 0..255",
                          settings->effect);
    return BD_FEEDBACK_ERR_RANGE;
  }
  if (settings->cycles < -1 || settings->cycles > 65535) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_RANGE,
                          "led_anim cycles %d is outside -1..65535",
                          settings->cycles);
    return BD_FEEDBACK_ERR_RANGE;
  }
  if (settings->duration_ms < 1 || settings->duration_ms > 60000) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_RANGE,
                          "led_anim duration %d ms is outside 1..60000",
                          settings->duration_ms);
    return BD_FEEDBACK_ERR_RANGE;
  }
  char brightness_path[BD_FEEDBACK_PATH_MAX];
  if (led_anim_brightness_node(led, zone, brightness_path,
                               sizeof(brightness_path))) {
    snprintf(text, sizeof(text), "%d\n", settings->brightness);
    if (!bd_feedback_write_text(brightness_path, text)) {
      bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO,
                            "cannot write %s", brightness_path);
      return BD_FEEDBACK_ERR_IO;
    }
  } else if (zone != BD_FEEDBACK_LED_ZONE_F2) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                          "led_anim zone %s has no brightness node",
                          bd_feedback_led_zone_name(zone));
    return BD_FEEDBACK_ERR_UNSUPPORTED;
  }

  char name[32];
  snprintf(name, sizeof(name), "effect_cycles_%s", suffix);
  snprintf(text, sizeof(text), "%d\n", settings->cycles);
  if (!write_led_anim_node(led, name, text, error, BD_FEEDBACK_ERR_IO)) {
    return BD_FEEDBACK_ERR_IO;
  }
  snprintf(name, sizeof(name), "effect_duration_%s", suffix);
  snprintf(text, sizeof(text), "%d\n", settings->duration_ms);
  if (!write_led_anim_node(led, name, text, error, BD_FEEDBACK_ERR_IO)) {
    return BD_FEEDBACK_ERR_IO;
  }
  snprintf(name, sizeof(name), "effect_rgb_hex_%s", suffix);
  snprintf(text, sizeof(text), "%06X\n", (unsigned)(settings->rgb & 0xFFFFFFu));
  if (!write_led_anim_node(led, name, text, error, BD_FEEDBACK_ERR_IO)) {
    return BD_FEEDBACK_ERR_IO;
  }
  snprintf(name, sizeof(name), "effect_%s", suffix);
  snprintf(text, sizeof(text), "%d\n", settings->effect);
  if (!write_led_anim_node(led, name, text, error, BD_FEEDBACK_ERR_IO)) {
    return BD_FEEDBACK_ERR_IO;
  }
  return BD_FEEDBACK_OK;
}

bd_feedback_result bd_feedback_led_anim_apply(
    bd_feedback_led_anim *led, bd_feedback_led_zone zone,
    const bd_feedback_led_anim_settings *settings, bd_feedback_error *error) {
  bd_feedback_error_clear(error);
  if (led == NULL || settings == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "led_anim handle or settings is NULL");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  if (!led->available) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_STATE,
                          "led_anim was not probed or is unavailable");
    return BD_FEEDBACK_ERR_STATE;
  }
  if (zone < 0 || zone >= BD_FEEDBACK_LED_ZONE_COUNT ||
      !led->zone_present[zone]) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                          "led_anim zone %d is not present", (int)zone);
    return BD_FEEDBACK_ERR_UNSUPPORTED;
  }
  return apply_led_anim_zone(led, zone, settings, error);
}

bd_feedback_result bd_feedback_led_anim_apply_all(
    bd_feedback_led_anim *led,
    const bd_feedback_led_anim_settings settings[BD_FEEDBACK_LED_ZONE_COUNT],
    bd_feedback_error *error) {
  bd_feedback_error_clear(error);
  if (led == NULL || settings == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "led_anim handle or settings is NULL");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  if (!led->available) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_STATE,
                          "led_anim was not probed or is unavailable");
    return BD_FEEDBACK_ERR_STATE;
  }
  for (int i = 0; i < BD_FEEDBACK_LED_ZONE_COUNT; ++i) {
    if (!led->zone_present[i]) {
      continue;
    }
    bd_feedback_result result =
        apply_led_anim_zone(led, (bd_feedback_led_zone)i, &settings[i], error);
    if (result != BD_FEEDBACK_OK) {
      return result;
    }
  }
  return BD_FEEDBACK_OK;
}

bd_feedback_result bd_feedback_led_anim_off(bd_feedback_led_anim *led,
                                            bd_feedback_error *error) {
  bd_feedback_error_clear(error);
  if (led == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT, "led is NULL");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  if (!led->available) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_STATE,
                          "led_anim was not probed or is unavailable");
    return BD_FEEDBACK_ERR_STATE;
  }
  for (int i = 0; i < BD_FEEDBACK_LED_ZONE_COUNT; ++i) {
    if (!led->zone_present[i]) {
      continue;
    }
    char path[BD_FEEDBACK_PATH_MAX];
    if (!led_anim_brightness_node(led, (bd_feedback_led_zone)i, path,
                                  sizeof(path))) {
      continue;
    }
    if (!bd_feedback_write_text(path, "0\n")) {
      bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO, "cannot write %s",
                            path);
      return BD_FEEDBACK_ERR_IO;
    }
  }
  return BD_FEEDBACK_OK;
}

/* ---------------------------------------------------------------------- */
/* TG4040 vendor motor                                                     */
/* ---------------------------------------------------------------------- */

static int motor_voltage_for_percent(const bd_feedback_motor *motor,
                                     int percent) {
  if (percent <= 0 || percent >= 100) {
    return motor->max_voltage_uv;
  }
  long long span = (long long)motor->max_voltage_uv - motor->min_voltage_uv;
  return motor->min_voltage_uv +
         (int)((long long)percent * span / 100);
}

bd_feedback_result bd_feedback_motor_open(bd_feedback_motor *motor,
                                          const bd_feedback_motor_config *config,
                                          bd_feedback_error *error) {
  bd_feedback_error_clear(error);
  if (motor == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT, "motor is NULL");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  memset(motor, 0, sizeof(*motor));
  const char *root = config != NULL && config->sysfs_root != NULL
                         ? config->sysfs_root
                         : "";
  motor->min_voltage_uv =
      config != NULL && config->min_voltage_uv != 0 ? config->min_voltage_uv
                                                     : 500000;
  motor->max_voltage_uv =
      config != NULL && config->max_voltage_uv != 0 ? config->max_voltage_uv
                                                     : 2500000;
  motor->max_percent =
      config != NULL && config->max_percent != 0 ? config->max_percent : 100;
  motor->gpio = config != NULL && config->gpio != 0 ? config->gpio : 227;
  if (motor->min_voltage_uv <= 0 ||
      motor->max_voltage_uv <= motor->min_voltage_uv ||
      motor->max_percent < 1 || motor->max_percent > 100 || motor->gpio < 0) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "invalid motor voltage/percent/gpio configuration");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }

  char relative[BD_FEEDBACK_PATH_MAX];
  snprintf(relative, sizeof(relative), "/sys/class/motor/voltage");
  if (!bd_feedback_path_join(root, relative, motor->voltage,
                             sizeof(motor->voltage)) ||
      !bd_feedback_file_exists(motor->voltage)) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                          "%s is not present", motor->voltage);
    return BD_FEEDBACK_ERR_UNSUPPORTED;
  }
  snprintf(relative, sizeof(relative), "/sys/class/gpio/gpio%d/value",
           motor->gpio);
  if (!bd_feedback_path_join(root, relative, motor->value,
                             sizeof(motor->value))) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "motor GPIO path is too long");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  snprintf(relative, sizeof(relative), "/sys/class/gpio/gpio%d/direction",
           motor->gpio);
  if (!bd_feedback_path_join(root, relative, motor->direction,
                             sizeof(motor->direction))) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT,
                          "motor GPIO direction path is too long");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }

  if (!bd_feedback_file_exists(motor->value)) {
    char export_path[BD_FEEDBACK_PATH_MAX];
    snprintf(relative, sizeof(relative), "/sys/class/gpio/export");
    if (!bd_feedback_path_join(root, relative, export_path,
                               sizeof(export_path)) ||
        !bd_feedback_file_exists(export_path)) {
      bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                            "%s is not exported and %s is missing",
                            motor->value, export_path);
      return BD_FEEDBACK_ERR_UNSUPPORTED;
    }
    char number[16];
    snprintf(number, sizeof(number), "%d\n", motor->gpio);
    if (!bd_feedback_write_text(export_path, number) &&
        !bd_feedback_file_exists(motor->value)) {
      bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO,
                            "cannot export GPIO %d through %s", motor->gpio,
                            export_path);
      return BD_FEEDBACK_ERR_IO;
    }
    motor->exported_by_us = true;
    if (bd_feedback_file_exists(motor->direction) &&
        !bd_feedback_write_text(motor->direction, "out\n")) {
      bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO,
                            "cannot set %s to output", motor->direction);
      return BD_FEEDBACK_ERR_IO;
    }
  }
  if (access(motor->value, W_OK) != 0) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_UNSUPPORTED,
                          "%s is not writable", motor->value);
    return BD_FEEDBACK_ERR_UNSUPPORTED;
  }
  motor->available = true;
  motor->active = false;
  return BD_FEEDBACK_OK;
}

bd_feedback_result bd_feedback_motor_set(bd_feedback_motor *motor, int percent,
                                         bd_feedback_error *error) {
  bd_feedback_error_clear(error);
  if (motor == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT, "motor is NULL");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  if (!motor->available) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_STATE,
                          "motor was not opened or is unavailable");
    return BD_FEEDBACK_ERR_STATE;
  }
  if (percent < 0 || percent > 100 || percent > motor->max_percent) {
    bd_feedback_set_error(
        error, BD_FEEDBACK_ERR_RANGE,
        "motor strength %d is outside 0..%d", percent, motor->max_percent);
    return BD_FEEDBACK_ERR_RANGE;
  }
  int voltage = motor_voltage_for_percent(motor, percent);
  char text[32];
  snprintf(text, sizeof(text), "%d\n", voltage);
  if (!bd_feedback_write_text(motor->voltage, text)) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO, "cannot write %s",
                          motor->voltage);
    return BD_FEEDBACK_ERR_IO;
  }
  snprintf(text, sizeof(text), "%d\n", percent > 0 ? 1 : 0);
  if (!bd_feedback_write_text(motor->value, text)) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO, "cannot write %s",
                          motor->value);
    return BD_FEEDBACK_ERR_IO;
  }
  motor->active = percent > 0;
  return BD_FEEDBACK_OK;
}

bd_feedback_result bd_feedback_motor_stop(bd_feedback_motor *motor,
                                          bd_feedback_error *error) {
  bd_feedback_error_clear(error);
  if (motor == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT, "motor is NULL");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  if (!motor->available) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_STATE,
                          "motor was not opened or is unavailable");
    return BD_FEEDBACK_ERR_STATE;
  }
  if (!bd_feedback_write_text(motor->value, "0\n")) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_IO, "cannot write %s",
                          motor->value);
    return BD_FEEDBACK_ERR_IO;
  }
  motor->active = false;
  return BD_FEEDBACK_OK;
}

bd_feedback_result bd_feedback_motor_close(bd_feedback_motor *motor,
                                           bd_feedback_error *error) {
  bd_feedback_error_clear(error);
  if (motor == NULL) {
    bd_feedback_set_error(error, BD_FEEDBACK_ERR_ARGUMENT, "motor is NULL");
    return BD_FEEDBACK_ERR_ARGUMENT;
  }
  if (!motor->available) {
    return BD_FEEDBACK_OK;
  }
  bd_feedback_result result = bd_feedback_motor_stop(motor, error);
  motor->available = false;
  motor->active = false;
  return result;
}
