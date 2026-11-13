#ifndef BRICKDESK_TOOLS_BD_FEEDBACK_PLAY_H
#define BRICKDESK_TOOLS_BD_FEEDBACK_PLAY_H

/*
 * CLI-only playback lifecycle helper.
 *
 * The device_feedback library deliberately has no timers or threads.  The
 * command-line tool still has to keep the event fd open for the requested
 * playback time, then stop/erase and close it.  This helper owns that sequence
 * so the CLI and a host regression test execute the exact same code.
 */

#include "brickdesk/device_feedback.h"

#include <stdbool.h>
#include <signal.h>

typedef enum bd_feedback_cli_wait_result {
  BD_FEEDBACK_CLI_WAIT_COMPLETE = 0,
  BD_FEEDBACK_CLI_WAIT_INTERRUPTED,
  BD_FEEDBACK_CLI_WAIT_ERROR
} bd_feedback_cli_wait_result;

typedef enum bd_feedback_cli_play_status {
  BD_FEEDBACK_CLI_PLAY_OK = 0,
  BD_FEEDBACK_CLI_PLAY_FAILED,
  BD_FEEDBACK_CLI_PLAY_INTERRUPTED
} bd_feedback_cli_play_status;

typedef bd_feedback_cli_wait_result (*bd_feedback_cli_wait_fn)(
    void *user, int duration_ms);

typedef struct bd_feedback_cli_play_report {
  bd_feedback_result play_result;
  bd_feedback_result close_result;
  bd_feedback_cli_wait_result wait_result;
  bool waited;
  int requested_ms;
  bd_feedback_error play_error;
  bd_feedback_error close_error;
} bd_feedback_cli_play_report;

/*
 * Plays strength for duration_ms, waits while the effect is active, then
 * stops/erases and closes the handle.  The wait is skipped when playback did
 * not succeed, including strength 0 (which the library treats as stop).
 *
 * The handle is closed exactly once on every path after probe success,
 * including wait interruption and playback failure.  The report may be NULL.
 * The return value is a CLI status, not a library result.
 */
bd_feedback_cli_play_status bd_feedback_cli_play_and_wait(
    bd_feedback_rumble *rumble, int strength, int duration_ms,
    bd_feedback_cli_wait_fn wait, void *wait_user,
    bd_feedback_cli_play_report *report);

/*
 * Production wait used by bd-feedback.  user points to a volatile
 * sig_atomic_t stop flag.  It sleeps on CLOCK_MONOTONIC for the full duration,
 * returns INTERRUPTED when the flag becomes non-zero, and does not touch the
 * rumble handle.  A duration of 0 returns COMPLETE immediately.
 */
bd_feedback_cli_wait_result bd_feedback_cli_wait_ms(void *user,
                                                    int duration_ms);

#endif /* BRICKDESK_TOOLS_BD_FEEDBACK_PLAY_H */
