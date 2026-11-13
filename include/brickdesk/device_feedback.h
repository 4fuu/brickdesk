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
#include <stdint.h>

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
/* TG4040 vendor LED animation (/sys/class/led_anim)                       */
/* ---------------------------------------------------------------------- */

/*
 * Brick Pro exposes five addressable LED zones.  NextUI's tg5040 backend
 * (fixed commit b6ec1343) writes:
 *
 *   max_scale          zone m (top bar)
 *   max_scale_lr       zone lr (stick rings)
 *   max_scale_f1f2     zones f1 and f2 (shared brightness)
 *   max_scale_rear     zone rear (triggers)
 *   effect_<zone>      vendor effect id; written last, applies the state
 *   effect_rgb_hex_<zone>   RRGGBB
 *   effect_cycles_<zone>    -1 for unlimited
 *   effect_duration_<zone>  milliseconds
 *
 * Brightness is 0..100.  The vendor effect id space is shared across zones:
 * 1 linear, 2 breathe, 3 blink, 4 static, then the trimui animations.  This
 * module does not guess further ids; callers pass the id they want and the
 * probe only checks that the nodes exist.
 */
typedef enum bd_feedback_led_zone {
  BD_FEEDBACK_LED_ZONE_M = 0,
  BD_FEEDBACK_LED_ZONE_LR,
  BD_FEEDBACK_LED_ZONE_F1,
  BD_FEEDBACK_LED_ZONE_F2,
  BD_FEEDBACK_LED_ZONE_REAR,
  BD_FEEDBACK_LED_ZONE_COUNT
} bd_feedback_led_zone;

const char *bd_feedback_led_zone_name(bd_feedback_led_zone zone);
/* Node suffix: "m", "lr", "f1", "f2", "rear". */
const char *bd_feedback_led_zone_suffix(bd_feedback_led_zone zone);

typedef struct bd_feedback_led_anim_config {
  /* Prefix before /sys.  NULL or "" means the real /sys. */
  const char *sysfs_root;
  /* 0 means the default, 100. */
  int max_brightness;
} bd_feedback_led_anim_config;

typedef struct bd_feedback_led_anim_settings {
  int brightness;   /* 0..max_brightness */
  int effect;       /* vendor effect id, 0..255 */
  int cycles;       /* -1 unlimited, otherwise 0..65535 */
  int duration_ms;  /* 1..60000 */
  uint32_t rgb;     /* 0x00RRGGBB */
} bd_feedback_led_anim_settings;

typedef struct bd_feedback_led_anim {
  bool available;
  char root[BD_FEEDBACK_PATH_MAX];
  int max_brightness;
  bool zone_present[BD_FEEDBACK_LED_ZONE_COUNT];
  /* f1 and f2 share max_scale_f1f2; writing f1 covers both. */
  bool f1f2_shared;
} bd_feedback_led_anim;

/*
 * Probes /sys/class/led_anim without writing.  A zone is present when its
 * effect_<zone> node exists.  Returns UNSUPPORTED when the directory or every
 * zone is missing.
 */
bd_feedback_result bd_feedback_led_anim_probe(
    bd_feedback_led_anim *led, const bd_feedback_led_anim_config *config,
    bd_feedback_error *error);

bool bd_feedback_led_anim_has_zone(const bd_feedback_led_anim *led,
                                   bd_feedback_led_zone zone);

/*
 * Applies one zone in NextUI's order: brightness, cycles, duration, colour,
 * then effect (which applies the state).  Values outside the documented range
 * are rejected with RANGE and nothing is written.  A missing zone returns
 * UNSUPPORTED.  f2 skips the brightness write because f1/f2 share one node.
 */
bd_feedback_result bd_feedback_led_anim_apply(
    bd_feedback_led_anim *led, bd_feedback_led_zone zone,
    const bd_feedback_led_anim_settings *settings, bd_feedback_error *error);

/* Applies every present zone; settings must hold BD_FEEDBACK_LED_ZONE_COUNT. */
bd_feedback_result bd_feedback_led_anim_apply_all(
    bd_feedback_led_anim *led,
    const bd_feedback_led_anim_settings settings[BD_FEEDBACK_LED_ZONE_COUNT],
    bd_feedback_error *error);

/* Writes brightness 0 for every present zone; effects are left in place. */
bd_feedback_result bd_feedback_led_anim_off(bd_feedback_led_anim *led,
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

/* ---------------------------------------------------------------------- */
/* TG4040 vendor motor (/sys/class/motor/voltage + GPIO227)                 */
/* ---------------------------------------------------------------------- */

/*
 * NextUI's tg5040 backend maps strength 0..0xFFFF to
 *   voltage = 500000 + strength * (2500000 - 500000) / 0xFFFF
 * on Brick Pro (2.5 V ceiling; Brick/Smart Pro use 3.3 V), writes the voltage
 * to /sys/class/motor/voltage, then writes 1/0 to gpio227/value.  This module
 * exposes the same path with a 0..100 percent input.  The motor is a
 * short-pulse device: callers set it, then stop it from their own deadline.
 */
typedef struct bd_feedback_motor_config {
  const char *sysfs_root;   /* NULL or "" for the real /sys */
  int min_voltage_uv;       /* 0 => 500000 */
  int max_voltage_uv;       /* 0 => 2500000 (Brick Pro) */
  int max_percent;          /* 0 => 100 */
  int gpio;                 /* 0 => 227 */
} bd_feedback_motor_config;

typedef struct bd_feedback_motor {
  bool available;
  bool active;
  bool exported_by_us;
  char voltage[BD_FEEDBACK_PATH_MAX];
  char value[BD_FEEDBACK_PATH_MAX];
  char direction[BD_FEEDBACK_PATH_MAX];
  int min_voltage_uv;
  int max_voltage_uv;
  int max_percent;
  int gpio;
} bd_feedback_motor;

/*
 * Opens the vendor motor path.  If gpio227/value is absent but the GPIO export
 * node exists, the GPIO is exported and switched to output.  Returns
 * UNSUPPORTED when the voltage node or the GPIO path is unavailable.
 */
bd_feedback_result bd_feedback_motor_open(bd_feedback_motor *motor,
                                          const bd_feedback_motor_config *config,
                                          bd_feedback_error *error);

/*
 * Sets the motor to percent 0..100.  0 stops it.  Voltage is mapped linearly
 * between min and max; 100 uses the maximum.  The voltage is written before
 * the GPIO so the first pulse never runs at the previous level.
 */
bd_feedback_result bd_feedback_motor_set(bd_feedback_motor *motor, int percent,
                                         bd_feedback_error *error);

/* Writes GPIO227=0.  Idempotent while the motor is open. */
bd_feedback_result bd_feedback_motor_stop(bd_feedback_motor *motor,
                                          bd_feedback_error *error);

/* Stops and releases the handle.  Safe to call twice. */
bd_feedback_result bd_feedback_motor_close(bd_feedback_motor *motor,
                                           bd_feedback_error *error);

#ifdef __cplusplus
}
#endif

#endif /* BRICKDESK_DEVICE_FEEDBACK_H */
