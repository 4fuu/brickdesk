#ifndef BRICKDESK_HAPTICS_H
#define BRICKDESK_HAPTICS_H

/*
 * Short haptic pulses on the Brick Pro vendor motor.
 *
 * The device layer owns /sys/class/motor/voltage + GPIO227.  This module owns
 * the pulse timeline: the shell calls bd_haptics_update() from its event loop
 * and wakes at bd_haptics_next_deadline().  There is no thread and no timer
 * inside the module.
 *
 * A pattern is a sequence of (strength, duration) steps.  Strength 0 is a gap.
 * The first step starts immediately; each following step starts when the
 * previous deadline expires.  This covers the menu tick, the FN double pulse,
 * boot and sleep pulses with one implementation.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct bd_haptics bd_haptics;

#define BD_HAPTICS_MAX_STEPS 8

bd_haptics *bd_haptics_create(const char *sysfs_root, char *error,
                              size_t error_size);
void bd_haptics_destroy(bd_haptics *haptics);

bool bd_haptics_available(const bd_haptics *haptics);
const char *bd_haptics_unavailable_reason(const bd_haptics *haptics);

void bd_haptics_set_enabled(bd_haptics *haptics, bool enabled);
void bd_haptics_set_strength(bd_haptics *haptics, int percent);
int bd_haptics_strength(const bd_haptics *haptics);

/*
 * Starts a pattern at now_ms.  Strengths are 0..100, durations 1..60000 ms.
 * Returns false when haptics are disabled/unavailable or the pattern is
 * invalid; no hardware write happens then.
 */
bool bd_haptics_play_pattern(bd_haptics *haptics, const int *strengths,
                             const int *durations_ms, int count,
                             uint64_t now_ms);

/* Single pulse convenience; strength 0 or duration 0 is a no-op. */
bool bd_haptics_play(bd_haptics *haptics, int strength, int duration_ms,
                     uint64_t now_ms);

/* Advances the pattern; stops the motor when the last step ends. */
void bd_haptics_update(bd_haptics *haptics, uint64_t now_ms);

/* Earliest pattern deadline, or false when no pulse is scheduled. */
bool bd_haptics_next_deadline(const bd_haptics *haptics, uint64_t *until_ms);

/* Cancels the pattern and writes GPIO227=0. */
void bd_haptics_stop(bd_haptics *haptics);

#endif /* BRICKDESK_HAPTICS_H */
