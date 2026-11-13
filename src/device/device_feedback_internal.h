#ifndef BRICKDESK_DEVICE_FEEDBACK_INTERNAL_H
#define BRICKDESK_DEVICE_FEEDBACK_INTERNAL_H

/*
 * Test seams for the LED/rumble capability layer.  This header is private to
 * src/device and the unit tests; it is not installed or shipped.
 *
 * Production callers use brickdesk/device_feedback.h.  The *_with_io entry
 * points let host tests exercise range, readback, failure, capability and
 * cleanup behaviour without real LEDs or a force-feedback event node.
 */

#include "brickdesk/device_feedback.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct bd_feedback_file_io {
  void *user;
  bool (*read)(void *user, const char *path, char *out, size_t out_size);
  bool (*write)(void *user, const char *path, const char *text);
  bool (*writable)(void *user, const char *path);
} bd_feedback_file_io;

typedef struct bd_feedback_evdev_io {
  void *user;
  int (*open)(void *user, const char *path, int flags);
  int (*close)(void *user, int fd);
  /*
   * The request is one of EVIOCGBIT/EVIOCGEFFECTS/EVIOCGNAME/EVIOCSFF/
   * EVIOCRMFF.  EVIOCRMFF passes the effect id as a pointer-sized value,
   * matching the kernel's int argument.
   */
  int (*ioctl)(void *user, int fd, unsigned long request, void *arg);
  ssize_t (*write)(void *user, int fd, const void *data, size_t size);
} bd_feedback_evdev_io;

const bd_feedback_file_io *bd_feedback_file_io_default(void);
const bd_feedback_evdev_io *bd_feedback_evdev_io_default(void);

bd_feedback_result bd_feedback_led_probe_with_io(
    bd_feedback_led *led, const bd_feedback_led_config *config,
    const bd_feedback_file_io *file_io, bd_feedback_error *error);

bd_feedback_result bd_feedback_rumble_probe_with_io(
    bd_feedback_rumble *rumble, const bd_feedback_rumble_config *config,
    const bd_feedback_evdev_io *evdev_io, bd_feedback_error *error);

/* Shared helpers, implemented in feedback.c. */
void bd_feedback_set_error(bd_feedback_error *error, bd_feedback_result code,
                           const char *format, ...);
bool bd_feedback_copy_string(char *dst, size_t dst_size, const char *src);
void bd_feedback_trim(char *text);
bool bd_feedback_parse_int(const char *text, int *out);
bool bd_feedback_read_text(const char *path, char *out, size_t out_size);
bool bd_feedback_write_text(const char *path, const char *text);
bool bd_feedback_path_join(const char *root, const char *path, char *out,
                           size_t out_size);
bool bd_feedback_file_exists(const char *path);
bool bd_feedback_dir_exists(const char *path);

/* Constant vendor blockers, also used by the diagnostics CLI. */
const char *bd_feedback_vendor_led_blocker(void);
const char *bd_feedback_vendor_motor_blocker(void);

#endif /* BRICKDESK_DEVICE_FEEDBACK_INTERNAL_H */
