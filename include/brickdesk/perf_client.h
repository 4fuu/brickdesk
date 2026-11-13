#ifndef BRICKDESK_PERF_CLIENT_H
#define BRICKDESK_PERF_CLIENT_H

/*
 * Minimal client for brickdesk-perfd's line-delimited JSON socket.
 *
 * The desktop only needs one operation: ask the daemon to change the global
 * profile and wait for the daemon's confirmation.  The daemon remains the only
 * hardware writer.  Connection setup and every send/receive use non-blocking
 * I/O with a caller-supplied absolute timeout, so a missing or wedged daemon
 * returns an error instead of stalling the desktop forever.
 */

#include <stdbool.h>
#include <stddef.h>

#include "brickdesk/perf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum bd_perf_client_status {
  BD_PERF_CLIENT_OK = 0,
  /* The socket does not exist or the daemon refused the connection. */
  BD_PERF_CLIENT_UNAVAILABLE,
  /* The daemon answered with a JSON-RPC error. */
  BD_PERF_CLIENT_REJECTED,
  BD_PERF_CLIENT_TIMEOUT,
  BD_PERF_CLIENT_PROTOCOL
} bd_perf_client_status;

const char *bd_perf_client_status_name(bd_perf_client_status status);

typedef struct bd_perf_client_result {
  char profile[BD_PERF_NAME_MAX + 1];
  char governor[BD_PERF_GOVERNOR_MAX + 1];
  int min_khz;
  int max_khz;
  int online_cores;
} bd_perf_client_result;

/* $BRICKDESK_PERF_SOCKET, then /run/brickdesk/perfd.sock, then /tmp. */
const char *bd_perf_client_default_socket(void);

/*
 * Applies `profile` as the daemon's global profile.  `timeout_ms` bounds the
 * whole non-blocking round trip (250 ms is enough on the device; tests may
 * raise it).  On BD_PERF_CLIENT_OK the result contains the daemon-confirmed
 * profile and plan values.  On error, `error` receives a readable reason.
 */
bd_perf_client_status bd_perf_client_set_global(
    const char *socket_path, const char *profile, int timeout_ms,
    bd_perf_client_result *result, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif /* BRICKDESK_PERF_CLIENT_H */
