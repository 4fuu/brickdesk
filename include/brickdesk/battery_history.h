#ifndef BRICKDESK_BATTERY_HISTORY_H
#define BRICKDESK_BATTERY_HISTORY_H

/*
 * Persistent battery history and a conservative remaining-time estimate.
 *
 * The module is storage-only: it never reads power_supply, never draws and
 * never decides policy.  The shell records samples from bd_battery_status and
 * the settings page reads the series for its chart.  The file is a small CSV
 * under the user configuration directory and is capped at 24 hours of
 * five-minute samples, so it cannot grow without bound.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BD_BATTERY_HISTORY_MAX 288
#define BD_BATTERY_HISTORY_INTERVAL_S 300

typedef struct bd_battery_point {
  int64_t epoch_s;
  int percent; /* 0..100 */
  bool charging;
} bd_battery_point;

typedef struct bd_battery_history bd_battery_history;

bd_battery_history *bd_battery_history_create(void);
void bd_battery_history_destroy(bd_battery_history *history);

/* Missing or malformed files leave an empty history and return false. */
bool bd_battery_history_load(bd_battery_history *history, const char *path);
/* Writes a temp file next to path and renames it; returns false on IO error. */
bool bd_battery_history_save(const bd_battery_history *history,
                             const char *path);

/*
 * Appends one sample.  A sample is kept when the percentage changed or the
 * interval elapsed; the caller may call this every battery poll.  Oldest
 * samples are dropped once the ring is full.
 */
bool bd_battery_history_record(bd_battery_history *history, int64_t epoch_s,
                               int percent, bool charging);

size_t bd_battery_history_count(const bd_battery_history *history);
/* index 0 is the oldest point; NULL when out of range. */
const bd_battery_point *bd_battery_history_at(const bd_battery_history *history,
                                              size_t index);
/* Nearest known percentage at or before epoch_s, or -1. */
int bd_battery_history_percent_at(const bd_battery_history *history,
                                  int64_t epoch_s);

/*
 * Remaining minutes from the discharge slope over the last hour.  Returns -1
 * when charging, when the battery is steady/rising, or when fewer than 15
 * minutes of discharging history exist.  Never extrapolates from one bad
 * point: at least three samples are required.
 */
int bd_battery_history_remaining_minutes(const bd_battery_history *history,
                                         int64_t now_s);

#endif /* BRICKDESK_BATTERY_HISTORY_H */
