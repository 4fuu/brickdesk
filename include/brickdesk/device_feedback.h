#ifndef BRICKDESK_DEVICE_FEEDBACK_H
#define BRICKDESK_DEVICE_FEEDBACK_H

/*
 * LED and rumble capability layer for brickdesk.
 *
 * This module implements two standard Linux interfaces only:
 *
 *   LED:    /sys/class/leds/<device>/brightness and max_brightness
 *   rumble: evdev force feedback using a FF_RUMBLE effect
 *           (EVIOCGBIT(EV_FF), EVIOCSFF, EV_FF, EVIOCRMFF)
 *
 * Both interfaces are documented kernel ABI.  The module does not include
 * vendor ioctls, PMIC sequences or guessed motor voltages.  The TG4040 vendor
 * nodes /sys/class/led_anim and /sys/class/motor/voltage + GPIO227 are probed
 * read-only and reported as hardware blockers; no write is attempted there.
 * See docs/device-feedback.md for the evidence and the real-device checklist.
 *
 * Every operation returns bd_feedback_result.  Missing hardware, a missing
 * kernel capability and an unsupported vendor path are explicit refusals,
 * never success.  Probing never writes.  Rumble close() stops and erases any
 * uploaded effect; LED has no automatic restore because the node may be
 * shared, so callers that own an LED must write 0 or their saved value before
 * exit.
 *
 * Units:
 *   LED brightness:  0..100 percent, capped by config.max_percent.
 *   rumble strength: 0..100 percent, capped by config.max_percent.
 *   rumble duration: milliseconds, capped by config.max_duration_ms.
 *   evdev magnitudes are 16-bit (0..65535); the mapping is internal.
 *
 * The API is synchronous and has no internal threads or timers.  The caller
 * must serialise calls on one object.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BD_FEEDBACK_PATH_MAX 256
#define BD_FEEDBACK_NAME_MAX 64
#define BD_FEEDBACK_MESSAGE_MAX 256

typedef enum bd_feedback_result {
  BD_FEEDBACK_OK = 0,
  BD_FEEDBACK_ERR_ARGUMENT,
  BD_FEEDBACK_ERR_UNSUPPORTED,
  BD_FEEDBACK_ERR_IO,
  BD_FEEDBACK_ERR_RANGE,
  BD_FEEDBACK_ERR_STATE
} bd_feedback_result;

typedef struct bd_feedback_error {
  bd_feedback_result code;
  char message[BD_FEEDBACK_MESSAGE_MAX];
} bd_feedback_error;

const char *bd_feedback_result_name(bd_feedback_result result);
void bd_feedback_error_clear(bd_feedback_error *error);

/* ---------------------------------------------------------------------- */
/* LED                                                                    */
/* ---------------------------------------------------------------------- */

typedef enum bd_feedback_led_backend {
  BD_FEEDBACK_LED_AUTO = 0,
  BD_FEEDBACK_LED_SYSFS,
  /* Explicitly disabled by configuration; probe returns UNSUPPORTED. */
  BD_FEEDBACK_LED_NONE
} bd_feedback_led_backend;

const char *bd_feedback_led_backend_name(bd_feedback_led_backend backend);

typedef struct bd_feedback_led_config {
  /*
   * Prefix before /sys.  NULL or "" means the real /sys.  Tests point this at
   * a fake tree such as "/tmp/bd-feedback/sysfs".
   */
  const char *sysfs_root;
  /*
   * Explicit LED directory.  An absolute path is resolved against
   * sysfs_root when sysfs_root is set; a relative path is joined to
   * sysfs_root.  NULL selects by `name` or auto-discovery.
   */
  const char *led;
  /*
   * Exact entry under <sysfs_root>/sys/class/leds.  It must be a plain name,
   * not a path.  An unknown name is UNSUPPORTED and never falls back to a
   * different LED.
   */
  const char *name;
  /*
   * When true and neither led nor name is set, scan
   * <sysfs_root>/sys/class/leds in sorted order and use the first LED with a
   * readable brightness/max_brightness and no active non-none trigger.
   * Default false: without an explicit selection the probe is UNSUPPORTED.
   */
  bool allow_auto_discover;
  /*
   * Hard ceiling for bd_feedback_led_set().  Requests above it are rejected
   * with BD_FEEDBACK_ERR_RANGE and no write.  0 means the default, 100.
   * A value of 1..100 is used as-is.
   */
  int max_percent;
  bd_feedback_led_backend backend;
} bd_feedback_led_config;

typedef struct bd_feedback_led {
  bd_feedback_led_backend backend;
  /* LED directory. */
  char device[BD_FEEDBACK_PATH_MAX];
  /* Directory basename, for diagnostics. */
  char name[BD_FEEDBACK_NAME_MAX];
  int raw_min;
  int raw_max;
  int max_percent;
  bool readable;
  bool writable;
  /*
   * Read-only observation of the TG4040 vendor LED class.  This module never
   * writes it; the blocker explains which facts are missing.
   */
  bool vendor_led_anim_present;
  char vendor_led_blocker[BD_FEEDBACK_MESSAGE_MAX];
  struct {
    const void *file_io; /* internal: bd_feedback_file_io */
  } internal;
} bd_feedback_led;

/*
 * Probes one standard LED class node without writing.  On success backend is
 * SYSFS, raw_max comes from max_brightness, and the initial brightness must be
 * inside 0..raw_max.  An active non-none trigger makes the LED UNSUPPORTED:
 * the kernel trigger owns the output and a brightness write would not be a
 * reliable success.  Auto-discovery is opt-in.
 *
 * A failed probe leaves backend NONE and fills error when supplied.  The
 * vendor LED blocker is filled even on failure.
 */
bd_feedback_result bd_feedback_led_probe(bd_feedback_led *led,
                                         const bd_feedback_led_config *config,
                                         bd_feedback_error *error);

/* Reads the current LED brightness as 0..100. */
bd_feedback_result bd_feedback_led_get(const bd_feedback_led *led,
                                       int *percent, bd_feedback_error *error);

/*
 * Sets brightness from 0..100, capped by the configured max_percent.  Values
 * outside the public range or above the configured cap are rejected with
 * BD_FEEDBACK_ERR_RANGE and no write.  The raw value is rounded and read back;
 * a backend that does not match the request is an I/O error.  A read-only LED
 * returns UNSUPPORTED.  Writing 0 turns the LED off.
 *
 * The trigger attribute is read again immediately before the write.  If it is
 * now an active non-none trigger, the write is rejected with UNSUPPORTED and
 * no brightness write occurs.  A missing trigger attribute is still supported:
 * it means the LED has no trigger control.
 */
bd_feedback_result bd_feedback_led_set(bd_feedback_led *led, int percent,
                                       bd_feedback_error *error);

/* ---------------------------------------------------------------------- */
/* Rumble                                                                 */
/* ---------------------------------------------------------------------- */

typedef enum bd_feedback_rumble_backend {
  BD_FEEDBACK_RUMBLE_AUTO = 0,
  BD_FEEDBACK_RUMBLE_EVDEV,
  /* Explicitly disabled by configuration; probe returns UNSUPPORTED. */
  BD_FEEDBACK_RUMBLE_NONE
} bd_feedback_rumble_backend;

const char *bd_feedback_rumble_backend_name(bd_feedback_rumble_backend backend);

typedef struct bd_feedback_rumble_config {
  /*
   * Explicit /dev/input/eventN path.  There is no implicit autodetection:
   * selecting a controller or the wrong event node would be a guess.  The
   * node is opened O_RDWR and must advertise FF_RUMBLE.
   */
  const char *event;
  /*
   * Prefix before /sys, used only for the read-only vendor motor probe.
   * NULL or "" means the real /sys.  No vendor node is written.
   */
  const char *sysfs_root;
  /*
   * Hard ceiling for bd_feedback_rumble_play().  Requests above it are
   * rejected with BD_FEEDBACK_ERR_RANGE and nothing is uploaded.  0 means the
   * default, 100; 1..100 is used as-is.
   */
  int max_percent;
  /*
   * Hard ceiling for the effect replay length in milliseconds.  Requests above
   * it are rejected with BD_FEEDBACK_ERR_RANGE and nothing is uploaded.
   * 0 means the default, 1000.  The accepted range is 1..60000.
   */
  int max_duration_ms;
  bd_feedback_rumble_backend backend;
} bd_feedback_rumble_config;

typedef struct bd_feedback_rumble {
  bd_feedback_rumble_backend backend;
  char event[BD_FEEDBACK_PATH_MAX];
  char device_name[BD_FEEDBACK_NAME_MAX];
  int effects;
  bool has_ff_rumble;
  int max_percent;
  int max_duration_ms;
  bool active;
  /*
   * Read-only observation of the TG4040 vendor motor path.  This module never
   * writes /sys/class/motor/voltage or the gpio227 value.
   */
  bool vendor_motor_voltage_present;
  bool vendor_motor_enable_present;
  char vendor_blocker[BD_FEEDBACK_MESSAGE_MAX];
  struct {
    const void *evdev_io; /* internal: bd_feedback_evdev_io */
    int fd;
    int effect_id; /* -1 when no effect is uploaded */
  } internal;
} bd_feedback_rumble;

/*
 * Opens the configured event node and verifies the standard force-feedback
 * capability without playing anything.  It requires the EV_FF event type,
 * the FF_RUMBLE effect bit and at least one effect slot.  An absent node is
 * UNSUPPORTED; an open failure with any other errno is I/O; a node without
 * FF_RUMBLE is UNSUPPORTED.  The vendor motor paths are probed read-only and
 * fill the blocker even when the evdev probe fails.
 *
 * On success the fd stays open until bd_feedback_rumble_close().
 */
bd_feedback_result bd_feedback_rumble_probe(
    bd_feedback_rumble *rumble, const bd_feedback_rumble_config *config,
    bd_feedback_error *error);

/*
 * Plays a rumble effect for duration_ms.  strength is 0..100 and is mapped
 * linearly to both strong and weak 16-bit magnitudes.  strength 0 is
 * equivalent to stop() and does not upload an effect.
 *
 * Any previously uploaded effect is stopped and erased first.  The effect is
 * uploaded with EVIOCSFF and started with one EV_FF write.  The kernel stops a
 * replay of the configured length; this module does not arm a timer.  A failed
 * upload or play leaves no active effect when cleanup succeeds, and returns
 * I/O on failure.
 */
bd_feedback_result bd_feedback_rumble_play(bd_feedback_rumble *rumble,
                                           int strength, int duration_ms,
                                           bd_feedback_error *error);

/*
 * Stops and erases the current effect.  Idempotent while the handle is open
 * and no effect is active.  Returns I/O if the stop write or erase failed; the
 * effect id is retained for a later close()/stop() retry when erase failed.
 * An unopened or closed handle returns BD_FEEDBACK_ERR_STATE.
 */
bd_feedback_result bd_feedback_rumble_stop(bd_feedback_rumble *rumble,
                                           bd_feedback_error *error);

/*
 * Stops/erases any effect, closes the event fd, and clears the handle.  Safe
 * to call twice.  Returns the cleanup result: BD_FEEDBACK_OK, or I/O when the
 * stop/erase/close failed.  The fd is closed after cleanup is attempted, even
 * when stop/erase fails, so an effect that could not be erased cannot be
 * retried through this handle; the caller must treat that as a hardware/output
 * risk.
 */
bd_feedback_result bd_feedback_rumble_close(bd_feedback_rumble *rumble,
                                            bd_feedback_error *error);

#ifdef __cplusplus
}
#endif

#endif /* BRICKDESK_DEVICE_FEEDBACK_H */
