#ifndef BRICKDESK_LED_H
#define BRICKDESK_LED_H

/*
 * LED policy for the Brick Pro vendor LED zones.
 *
 * The device layer (brickdesk/device_feedback.h) writes
 * /sys/class/led_anim; this module turns the [led] configuration and the
 * system state into one of the profiles below.  It never writes a standard
 * Linux LED class node and it never blocks on a timer: the shell owns the
 * sleep-pulse deadline and calls bd_led_off() when it expires.
 *
 * Profiles mirror NextUI's tg5040 backend (fixed commit b6ec1343):
 *   default          user zone brightness/colour/effect
 *   off              brightness 0, effects left in place
 *   low battery      interval breathe 0xFF3300, unlimited
 *   critical battery interval breathe 0xFF0000, unlimited
 *   charging         breathe 0x00FF00, unlimited
 *   sleep            breathe, user colour, 5 cycles
 *   ambient          static, caller colour
 *
 * Brightness is 0..100.  The f1/f2 keys share max_scale_f1f2, so f2 uses the
 * f1 brightness value.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "brickdesk/config.h"

typedef enum bd_led_profile {
  BD_LED_PROFILE_DEFAULT = 0,
  BD_LED_PROFILE_OFF,
  BD_LED_PROFILE_LOW_BATTERY,
  BD_LED_PROFILE_CRITICAL_BATTERY,
  BD_LED_PROFILE_CHARGING,
  BD_LED_PROFILE_SLEEP,
  BD_LED_PROFILE_AMBIENT
} bd_led_profile;

typedef struct bd_led bd_led;

/*
 * Probes the vendor LED path and keeps a borrowed config pointer.  Returns a
 * handle even when the hardware is absent; bd_led_available() is false and
 * bd_led_unavailable_reason() explains why.  Returns NULL only for invalid
 * arguments or allocation failure.
 */
bd_led *bd_led_create(bd_config *config, const char *sysfs_root, char *error,
                      size_t error_size);
void bd_led_destroy(bd_led *led);

bool bd_led_available(const bd_led *led);
const char *bd_led_unavailable_reason(const bd_led *led);

/* Re-reads the [led] keys; does not write the hardware. */
void bd_led_reload(bd_led *led);

/* Applies the configured per-zone brightness/colour/effect. */
bool bd_led_apply_default(bd_led *led, char *error, size_t error_size);

bool bd_led_apply_profile(bd_led *led, bd_led_profile profile, char *error,
                          size_t error_size);

/* Ambient colour; requires BD_LED_PROFILE_AMBIENT semantics. */
bool bd_led_apply_ambient(bd_led *led, uint32_t rgb, char *error,
                          size_t error_size);

/* Brightness 0 on every present zone. */
bool bd_led_off(bd_led *led, char *error, size_t error_size);

/* Length of the five-cycle sleep breathe, for the shell's off deadline. */
uint64_t bd_led_sleep_pulse_ms(const bd_led *led);

#endif /* BRICKDESK_LED_H */
