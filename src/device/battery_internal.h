#ifndef BRICKDESK_BATTERY_INTERNAL_H
#define BRICKDESK_BATTERY_INTERNAL_H

/*
 * Shared helpers for the battery reader and policy.  This header is private
 * to src/device; production callers use brickdesk/device_battery.h.
 */

#include "brickdesk/device_battery.h"

#include <stdbool.h>
#include <stddef.h>

void bd_battery_set_error(bd_battery_error *error, bd_battery_result code,
                          const char *format, ...);
bool bd_battery_copy_string(char *dst, size_t dst_size, const char *src);

#endif /* BRICKDESK_BATTERY_INTERNAL_H */
