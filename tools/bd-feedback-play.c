#define _POSIX_C_SOURCE 200809L

#include "bd-feedback-play.h"

#include <errno.h>
#include <string.h>
#include <time.h>

bd_feedback_cli_wait_result bd_feedback_cli_wait_ms(void *user,
                                                    int duration_ms) {
  if (user == NULL || duration_ms < 0) {
    return BD_FEEDBACK_CLI_WAIT_ERROR;
  }
  volatile sig_atomic_t *stop = (volatile sig_atomic_t *)user;
  if (*stop != 0) {
    return BD_FEEDBACK_CLI_WAIT_INTERRUPTED;
  }
  if (duration_ms == 0) {
    return BD_FEEDBACK_CLI_WAIT_COMPLETE;
  }

  struct timespec start;
  if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
    return BD_FEEDBACK_CLI_WAIT_ERROR;
  }

  for (;;) {
    if (*stop != 0) {
      return BD_FEEDBACK_CLI_WAIT_INTERRUPTED;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
      return BD_FEEDBACK_CLI_WAIT_ERROR;
    }
    long long elapsed_ms =
        (long long)(now.tv_sec - start.tv_sec) * 1000 +
        (long long)(now.tv_nsec - start.tv_nsec) / 1000000;
    if (elapsed_ms >= duration_ms) {
      return *stop != 0 ? BD_FEEDBACK_CLI_WAIT_INTERRUPTED
                        : BD_FEEDBACK_CLI_WAIT_COMPLETE;
    }

    long long remaining_ms = (long long)duration_ms - elapsed_ms;
    struct timespec request;
    request.tv_sec = (time_t)(remaining_ms / 1000);
    request.tv_nsec = (long)((remaining_ms % 1000) * 1000000);
    if (nanosleep(&request, NULL) != 0) {
      if (errno == EINTR) {
        continue;
      }
      return BD_FEEDBACK_CLI_WAIT_ERROR;
    }
  }
}

bd_feedback_cli_play_status bd_feedback_cli_play_and_wait(
    bd_feedback_rumble *rumble, int strength, int duration_ms,
    bd_feedback_cli_wait_fn wait, void *wait_user,
    bd_feedback_cli_play_report *report) {
  bd_feedback_cli_play_report local;
  if (report == NULL) {
    report = &local;
  }
  memset(report, 0, sizeof(*report));
  report->play_result = BD_FEEDBACK_ERR_ARGUMENT;
  report->close_result = BD_FEEDBACK_ERR_ARGUMENT;
  report->wait_result = BD_FEEDBACK_CLI_WAIT_COMPLETE;
  bd_feedback_error_clear(&report->play_error);
  bd_feedback_error_clear(&report->close_error);
  if (rumble == NULL) {
    return BD_FEEDBACK_CLI_PLAY_FAILED;
  }

  report->play_result =
      bd_feedback_rumble_play(rumble, strength, duration_ms,
                              &report->play_error);
  if (report->play_result == BD_FEEDBACK_OK && strength > 0) {
    if (wait == NULL) {
      report->wait_result = BD_FEEDBACK_CLI_WAIT_ERROR;
    } else {
      report->waited = true;
      report->requested_ms = duration_ms;
      report->wait_result = wait(wait_user, duration_ms);
    }
  }

  report->close_result = bd_feedback_rumble_close(rumble, &report->close_error);
  if (report->close_result != BD_FEEDBACK_OK ||
      report->play_result != BD_FEEDBACK_OK ||
      report->wait_result == BD_FEEDBACK_CLI_WAIT_ERROR) {
    return BD_FEEDBACK_CLI_PLAY_FAILED;
  }
  if (report->wait_result == BD_FEEDBACK_CLI_WAIT_INTERRUPTED) {
    return BD_FEEDBACK_CLI_PLAY_INTERRUPTED;
  }
  return BD_FEEDBACK_CLI_PLAY_OK;
}
