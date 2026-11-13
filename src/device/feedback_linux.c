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
