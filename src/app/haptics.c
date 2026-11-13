#define _POSIX_C_SOURCE 200809L

#include "brickdesk/haptics.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brickdesk/device_feedback.h"

#define BD_HAPTICS_REASON_MAX 256

struct bd_haptics {
  bd_feedback_motor motor;
  bool available;
  bool enabled;
  int strength;
  char reason[BD_HAPTICS_REASON_MAX];
  int strengths[BD_HAPTICS_MAX_STEPS];
  int durations[BD_HAPTICS_MAX_STEPS];
  int step_count;
  int step;
  uint64_t deadline_ms;
};

static bool set_error(char *error, size_t error_size, const char *format,
                      ...) {
  if (error != NULL && error_size > 0) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
  }
  return false;
}

bd_haptics *bd_haptics_create(const char *sysfs_root, char *error,
                              size_t error_size) {
  bd_haptics *haptics = calloc(1, sizeof(*haptics));
  if (haptics == NULL) {
    set_error(error, error_size, "out of memory");
    return NULL;
  }
  haptics->enabled = true;
  haptics->strength = 60;
  bd_feedback_motor_config config;
  memset(&config, 0, sizeof(config));
  config.sysfs_root = sysfs_root;
  config.max_voltage_uv = 2500000; /* Brick Pro ceiling from NextUI */
  config.min_voltage_uv = 500000;
  config.max_percent = 100;
  config.gpio = 227;
  bd_feedback_error feedback;
  bd_feedback_error_clear(&feedback);
  bd_feedback_result result =
      bd_feedback_motor_open(&haptics->motor, &config, &feedback);
  if (result == BD_FEEDBACK_OK) {
    haptics->available = true;
  } else {
    haptics->available = false;
    snprintf(haptics->reason, sizeof(haptics->reason), "%s",
             feedback.message[0] != '\0'
                 ? feedback.message
                 : bd_feedback_result_name(result));
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "%s", haptics->reason);
    }
  }
  return haptics;
}

void bd_haptics_destroy(bd_haptics *haptics) {
  if (haptics == NULL) {
    return;
  }
  bd_haptics_stop(haptics);
  bd_feedback_error feedback;
  bd_feedback_error_clear(&feedback);
  bd_feedback_motor_close(&haptics->motor, &feedback);
  free(haptics);
}

bool bd_haptics_available(const bd_haptics *haptics) {
  return haptics != NULL && haptics->available;
}

const char *bd_haptics_unavailable_reason(const bd_haptics *haptics) {
  return haptics != NULL ? haptics->reason : "missing haptics handle";
}

void bd_haptics_set_enabled(bd_haptics *haptics, bool enabled) {
  if (haptics == NULL) {
    return;
  }
  haptics->enabled = enabled;
  if (!enabled) {
    bd_haptics_stop(haptics);
  }
}

void bd_haptics_set_strength(bd_haptics *haptics, int percent) {
  if (haptics == NULL) {
    return;
  }
  if (percent < 0) {
    percent = 0;
  }
  if (percent > 100) {
    percent = 100;
  }
  haptics->strength = percent;
}

int bd_haptics_strength(const bd_haptics *haptics) {
  return haptics != NULL ? haptics->strength : 0;
}

static bool motor_set(bd_haptics *haptics, int strength) {
  bd_feedback_error feedback;
  bd_feedback_error_clear(&feedback);
  return bd_feedback_motor_set(&haptics->motor, strength, &feedback) ==
         BD_FEEDBACK_OK;
}

static bool start_step(bd_haptics *haptics, uint64_t now_ms) {
  if (haptics->step >= haptics->step_count) {
    haptics->step_count = 0;
    haptics->step = 0;
    haptics->deadline_ms = 0;
    motor_set(haptics, 0);
    return false;
  }
  int strength = haptics->strengths[haptics->step];
  int duration = haptics->durations[haptics->step];
  if (!motor_set(haptics, strength)) {
    haptics->step_count = 0;
    haptics->step = 0;
    haptics->deadline_ms = 0;
    return false;
  }
  haptics->deadline_ms = now_ms + (uint64_t)duration;
  return true;
}

bool bd_haptics_play_pattern(bd_haptics *haptics, const int *strengths,
                             const int *durations_ms, int count,
                             uint64_t now_ms) {
  if (haptics == NULL || !haptics->available || !haptics->enabled ||
      strengths == NULL || durations_ms == NULL || count < 1 ||
      count > BD_HAPTICS_MAX_STEPS) {
    return false;
  }
  for (int i = 0; i < count; ++i) {
    if (strengths[i] < 0 || strengths[i] > 100 || durations_ms[i] < 1 ||
        durations_ms[i] > 60000) {
      return false;
    }
    haptics->strengths[i] = strengths[i];
    haptics->durations[i] = durations_ms[i];
  }
  haptics->step_count = count;
  haptics->step = 0;
  haptics->deadline_ms = 0;
  return start_step(haptics, now_ms);
}

bool bd_haptics_play(bd_haptics *haptics, int strength, int duration_ms,
                     uint64_t now_ms) {
  if (strength <= 0 || duration_ms <= 0) {
    return false;
  }
  return bd_haptics_play_pattern(haptics, &strength, &duration_ms, 1, now_ms);
}

void bd_haptics_update(bd_haptics *haptics, uint64_t now_ms) {
  if (haptics == NULL || haptics->step_count == 0) {
    return;
  }
  while (haptics->step_count > 0 && now_ms >= haptics->deadline_ms) {
    uint64_t deadline = haptics->deadline_ms;
    ++haptics->step;
    if (haptics->step >= haptics->step_count) {
      haptics->step_count = 0;
      haptics->step = 0;
      haptics->deadline_ms = 0;
      motor_set(haptics, 0);
      return;
    }
    /* A late loop starts the next step at the missed deadline, not now, so
     * the whole pattern keeps its original length. */
    if (!start_step(haptics, deadline)) {
      return;
    }
  }
}

bool bd_haptics_next_deadline(const bd_haptics *haptics, uint64_t *until_ms) {
  if (haptics == NULL || until_ms == NULL || haptics->step_count == 0) {
    return false;
  }
  *until_ms = haptics->deadline_ms;
  return true;
}

void bd_haptics_stop(bd_haptics *haptics) {
  if (haptics == NULL) {
    return;
  }
  haptics->step_count = 0;
  haptics->step = 0;
  haptics->deadline_ms = 0;
  if (haptics->available) {
    bd_feedback_error feedback;
    bd_feedback_error_clear(&feedback);
    bd_feedback_motor_stop(&haptics->motor, &feedback);
  }
}
