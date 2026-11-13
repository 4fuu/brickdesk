#ifndef BRICKDESK_DEVICE_TIME_H
#define BRICKDESK_DEVICE_TIME_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/*
 * Clock, timezone, NTP and RTC helpers for the stock tg5040 image.
 *
 * Timezone/NTP prefer uci (system.@system[0].zonename, system.ntp.enable) and
 * fall back to /etc/localtime and /etc/timezone.  RTC writes use
 * `hwclock -u -w`; the AXP2202 RTC is exposed by the kernel as /dev/rtc0.
 */

typedef struct bd_time_status {
  time_t now;
  char timezone[64];
  bool ntp;
  bool rtc;
} bd_time_status;

bool bd_time_get_status(bd_time_status *status);
bool bd_time_set_epoch(time_t epoch, char *error, size_t error_size);
bool bd_time_set_timezone(const char *timezone, char *error,
                          size_t error_size);
bool bd_time_set_ntp(bool enabled, char *error, size_t error_size);
bool bd_time_sync_now(char *error, size_t error_size);
bool bd_time_write_rtc(char *error, size_t error_size);

#endif /* BRICKDESK_DEVICE_TIME_H */
