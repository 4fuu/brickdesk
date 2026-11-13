#ifndef BRICKDESK_DEVICE_PROCESS_H
#define BRICKDESK_DEVICE_PROCESS_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Small argv-only process helpers shared by the network/Bluetooth backends.
 * No shell is involved, so SSIDs, passwords and device names cannot inject
 * syntax.  bd_process_run captures combined stdout/stderr and enforces a
 * timeout; bd_process_spawn detaches a daemon-like child.
 */

bool bd_process_run(char *const argv[], char *output, size_t output_size,
                    int *exit_code, char *error, size_t error_size);
bool bd_process_spawn(char *const argv[], char *error, size_t error_size);

#endif /* BRICKDESK_DEVICE_PROCESS_H */
