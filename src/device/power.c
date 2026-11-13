#define _POSIX_C_SOURCE 200809L

#include "brickdesk/device_power.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bd_power {
  bd_power_config config;
  bd_power_state state;
  bool cancel_pending;
  /* A hook may have changed state and still needs its matching restore. */
  bool input_touched;
  bool app_touched;
  bool light_resources_touched;
  bool deep_resources_touched;
  bool display_touched;
  bd_power_report last;
};

static void set_detail(bd_power_report *report, const char *format, ...) {
  if (report == NULL || report->detail[0] != '\0') {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(report->detail, sizeof(report->detail), format, args);
  va_end(args);
}

static void report_begin(bd_power *power, bd_power_report *report,
                         bd_power_action action, bd_power_sleep_mode mode) {
  memset(report, 0, sizeof(*report));
  report->action = action;
  report->mode = mode;
  report->result = BD_POWER_OK;
  report->state_before = power->state;
  report->state_after = power->state;
  report->failed_step = BD_POWER_STEP_NONE;
}

static void record_failure(bd_power_report *report, bd_power_step step,
                           int code, int error) {
  if (report->failed_step != BD_POWER_STEP_NONE) {
    return;
  }
  report->failed_step = step;
  report->step_code = code;
  report->step_errno = error;
  if (error != 0) {
    set_detail(report, "%s failed: %s (errno %d)",
               bd_power_step_name(step), strerror(error), error);
  } else if (code != 0) {
    set_detail(report, "%s failed: hook returned %d",
               bd_power_step_name(step), code);
  } else {
    set_detail(report, "%s failed", bd_power_step_name(step));
  }
}

static bd_power_result report_end(bd_power *power, bd_power_report *report,
                                  bd_power_report *out,
                                  bd_power_state state_after,
                                  bd_power_result result) {
  report->state_after = state_after;
  report->result = result;
  power->last = *report;
  if (out != NULL) {
    *out = *report;
  }
  return result;
}

static bd_power_result invalid_call(bd_power_report *report,
                                    bd_power_action action) {
  if (report != NULL) {
    memset(report, 0, sizeof(*report));
    report->action = action;
    report->result = BD_POWER_INVALID;
    report->state_before = BD_POWER_STATE_UNKNOWN;
    report->state_after = BD_POWER_STATE_UNKNOWN;
    report->failed_step = BD_POWER_STEP_NONE;
    set_detail(report, "invalid power handle");
  }
  return BD_POWER_INVALID;
}

static bool state_is_busy(bd_power_state state) {
  return state == BD_POWER_STATE_ENTERING_LIGHT ||
         state == BD_POWER_STATE_ENTERING_DEEP ||
         state == BD_POWER_STATE_WAKING ||
         state == BD_POWER_STATE_SHUTTING_DOWN ||
         state == BD_POWER_STATE_REBOOTING;
}

static int call_hook0(bd_power *power, bd_power_report *report,
                      bd_power_step step, int (*fn)(void *)) {
  if (fn == NULL) {
    return 0;
  }
  ++report->hook_calls;
  errno = 0;
  int code = fn(power->config.hooks.user);
  if (code != 0) {
    record_failure(report, step, code, 0);
  }
  return code;
}

static int call_hook_mode(bd_power *power, bd_power_report *report,
                          bd_power_step step,
                          int (*fn)(void *, bd_power_sleep_mode),
                          bd_power_sleep_mode mode) {
  if (fn == NULL) {
    return 0;
  }
  ++report->hook_calls;
  errno = 0;
  int code = fn(power->config.hooks.user, mode);
  if (code != 0) {
    record_failure(report, step, code, 0);
  }
  return code;
}

static bool enter_input(bd_power *power, bd_power_report *report) {
  power->input_touched = true;
  return call_hook0(power, report, BD_POWER_STEP_INPUT_RELEASE,
                    power->config.hooks.input_release) == 0;
}

static bool enter_app(bd_power *power, bd_power_report *report) {
  power->app_touched = true;
  return call_hook0(power, report, BD_POWER_STEP_APP_PAUSE,
                    power->config.hooks.app_pause) == 0;
}

static bool enter_resources(bd_power *power, bd_power_report *report,
                            bd_power_sleep_mode mode) {
  if (power->config.hooks.resource_prepare == NULL) {
    return true;
  }
  if (mode == BD_POWER_SLEEP_LIGHT) {
    power->light_resources_touched = true;
  } else {
    power->deep_resources_touched = true;
  }
  bd_power_step step = mode == BD_POWER_SLEEP_LIGHT
                           ? BD_POWER_STEP_RESOURCE_PREPARE_LIGHT
                           : BD_POWER_STEP_RESOURCE_PREPARE_DEEP;
  return call_hook_mode(power, report, step,
                        power->config.hooks.resource_prepare, mode) == 0;
}

static bool enter_display(bd_power *power, bd_power_report *report) {
  power->display_touched = true;
  return call_hook0(power, report, BD_POWER_STEP_DISPLAY_OFF,
                    power->config.hooks.display_off) == 0;
}

static bool restore_display(bd_power *power, bd_power_report *report) {
  if (!power->display_touched) {
    return true;
  }
  if (call_hook0(power, report, BD_POWER_STEP_DISPLAY_ON,
                 power->config.hooks.display_on) != 0) {
    ++report->restore_failures;
    return false;
  }
  power->display_touched = false;
  return true;
}

static bool restore_deep_resources(bd_power *power, bd_power_report *report) {
  if (!power->deep_resources_touched) {
    return true;
  }
  if (call_hook_mode(power, report, BD_POWER_STEP_RESOURCE_RESTORE_DEEP,
                     power->config.hooks.resource_restore,
                     BD_POWER_SLEEP_DEEP) != 0) {
    ++report->restore_failures;
    return false;
  }
  power->deep_resources_touched = false;
  return true;
}

static bool restore_light_resources(bd_power *power, bd_power_report *report) {
  if (!power->light_resources_touched) {
    return true;
  }
  if (call_hook_mode(power, report, BD_POWER_STEP_RESOURCE_RESTORE_LIGHT,
                     power->config.hooks.resource_restore,
                     BD_POWER_SLEEP_LIGHT) != 0) {
    ++report->restore_failures;
    return false;
  }
  power->light_resources_touched = false;
  return true;
}

static bool restore_app(bd_power *power, bd_power_report *report) {
  if (!power->app_touched) {
    return true;
  }
  if (call_hook0(power, report, BD_POWER_STEP_APP_RESUME,
                 power->config.hooks.app_resume) != 0) {
    ++report->restore_failures;
    return false;
  }
  power->app_touched = false;
  return true;
}

static bool restore_input(bd_power *power, bd_power_report *report) {
  if (!power->input_touched) {
    return true;
  }
  if (call_hook0(power, report, BD_POWER_STEP_INPUT_ACQUIRE,
                 power->config.hooks.input_acquire) != 0) {
    ++report->restore_failures;
    return false;
  }
  power->input_touched = false;
  return true;
}

/*
 * Reverse of the enter order: display, deep resources, light resources,
 * app, input.  Every step is attempted even when an earlier one fails, so a
 * single bad hook does not leave the remaining resources paused.
 */
static bool restore_all(bd_power *power, bd_power_report *report) {
  bool ok = true;
  if (!restore_display(power, report)) {
    ok = false;
  }
  if (!restore_deep_resources(power, report)) {
    ok = false;
  }
  if (!restore_light_resources(power, report)) {
    ok = false;
  }
  if (!restore_app(power, report)) {
    ok = false;
  }
  if (!restore_input(power, report)) {
    ok = false;
  }
  return ok;
}

static bool restore_deep_layer(bd_power *power, bd_power_report *report) {
  return restore_deep_resources(power, report);
}

static bool enter_light_layers(bd_power *power, bd_power_report *report) {
  if (!enter_input(power, report)) {
    return false;
  }
  if (!enter_app(power, report)) {
    return false;
  }
  if (!enter_resources(power, report, BD_POWER_SLEEP_LIGHT)) {
    return false;
  }
  if (!enter_display(power, report)) {
    return false;
  }
  return true;
}

static bool enter_deep_layers(bd_power *power, bd_power_report *report) {
  if (!enter_input(power, report)) {
    return false;
  }
  if (!enter_app(power, report)) {
    return false;
  }
  if (!enter_resources(power, report, BD_POWER_SLEEP_DEEP)) {
    return false;
  }
  if (!enter_display(power, report)) {
    return false;
  }
  return true;
}

static bd_power_backend_status call_backend_deep(bd_power *power,
                                                 bd_power_report *report) {
  report->backend_called = true;
  if (power->config.backend.deep_sleep == NULL) {
    record_failure(report, BD_POWER_STEP_DEEP_SLEEP, 0, ENOTSUP);
    return BD_POWER_BACKEND_UNSUPPORTED;
  }
  errno = 0;
  bd_power_backend_status status =
      power->config.backend.deep_sleep(power->config.backend.user);
  if (status == BD_POWER_BACKEND_ERROR) {
    record_failure(report, BD_POWER_STEP_DEEP_SLEEP, 0,
                   errno != 0 ? errno : EIO);
  } else if (status != BD_POWER_BACKEND_OK) {
    record_failure(report, BD_POWER_STEP_DEEP_SLEEP, 0, 0);
  }
  return status;
}

static bd_power_result map_backend_result(bd_power_backend_status status) {
  switch (status) {
    case BD_POWER_BACKEND_OK:
      return BD_POWER_OK;
    case BD_POWER_BACKEND_ERROR:
      return BD_POWER_BACKEND_FAILED;
    case BD_POWER_BACKEND_DISABLED:
      return BD_POWER_DISABLED;
    case BD_POWER_BACKEND_UNSUPPORTED:
      return BD_POWER_UNSUPPORTED;
    case BD_POWER_BACKEND_NO_EFFECT:
      return BD_POWER_NO_EFFECT;
  }
  return BD_POWER_BACKEND_FAILED;
}

static bd_power_result sleep_deep_from_idle(bd_power *power,
                                            bd_power_report *report) {
  if (!enter_deep_layers(power, report)) {
    report->rollback_attempted = true;
    bool recovered = restore_all(power, report);
    report->recovery_complete = recovered;
    power->state = recovered ? BD_POWER_STATE_IDLE : BD_POWER_STATE_DEGRADED;
    return report_end(power, report, NULL, power->state,
                      recovered ? BD_POWER_HOOK_FAILED : BD_POWER_DEGRADED);
  }

  bd_power_backend_status status = call_backend_deep(power, report);
  bool recovered = restore_all(power, report);
  if (status != BD_POWER_BACKEND_OK) {
    report->rollback_attempted = true;
    report->recovery_complete = recovered;
  }
  if (!recovered) {
    report->rollback_attempted = true;
    power->state = BD_POWER_STATE_DEGRADED;
    return report_end(power, report, NULL, power->state, BD_POWER_DEGRADED);
  }
  power->state = BD_POWER_STATE_IDLE;
  if (status == BD_POWER_BACKEND_OK) {
    set_detail(report,
               "deep sleep backend returned; the kernel path is unverified "
               "until the 20-cycle hardware test");
  }
  return report_end(power, report, NULL, power->state,
                    map_backend_result(status));
}

static bd_power_result sleep_deep_from_light(bd_power *power,
                                             bd_power_report *report) {
  if (!enter_resources(power, report, BD_POWER_SLEEP_DEEP)) {
    report->rollback_attempted = true;
    bool recovered = restore_deep_layer(power, report);
    report->recovery_complete = recovered;
    power->state =
        recovered ? BD_POWER_STATE_LIGHT_SLEEP : BD_POWER_STATE_DEGRADED;
    return report_end(power, report, NULL, power->state,
                      recovered ? BD_POWER_HOOK_FAILED : BD_POWER_DEGRADED);
  }

  bd_power_backend_status status = call_backend_deep(power, report);
  bool recovered = restore_deep_layer(power, report);
  if (status != BD_POWER_BACKEND_OK) {
    report->rollback_attempted = true;
    report->recovery_complete = recovered;
  }
  if (!recovered) {
    report->rollback_attempted = true;
    power->state = BD_POWER_STATE_DEGRADED;
    return report_end(power, report, NULL, power->state, BD_POWER_DEGRADED);
  }
  power->state = BD_POWER_STATE_LIGHT_SLEEP;
  if (status == BD_POWER_BACKEND_OK) {
    set_detail(report,
               "deep sleep backend returned; still in light sleep, call wake "
               "to restore input/app/display");
  }
  return report_end(power, report, NULL, power->state,
                    map_backend_result(status));
}

static bd_power_result shutdown_common(bd_power *power, bd_power_report *report,
                                       bool reboot, bool allowed) {
  if (power->state == BD_POWER_STATE_DEGRADED) {
    set_detail(report, "recovery is required before shutdown");
    return report_end(power, report, NULL, power->state,
                      BD_POWER_RECOVERY_REQUIRED);
  }
  if (state_is_busy(power->state)) {
    set_detail(report, "another power transition is in progress");
    return report_end(power, report, NULL, power->state, BD_POWER_BUSY);
  }
  if (power->state != BD_POWER_STATE_IDLE &&
      power->state != BD_POWER_STATE_LIGHT_SLEEP) {
    set_detail(report, "shutdown is only valid from idle or light sleep");
    return report_end(power, report, NULL, power->state, BD_POWER_INVALID);
  }
  if (!allowed) {
    set_detail(report, "action is disabled by configuration");
    return report_end(power, report, NULL, power->state, BD_POWER_DISABLED);
  }
  if (power->config.backend.shutdown == NULL) {
    set_detail(report, "no shutdown backend is configured");
    return report_end(power, report, NULL, power->state,
                      BD_POWER_UNSUPPORTED);
  }

  power->cancel_pending = false;
  bd_power_state origin = power->state;
  power->state = reboot ? BD_POWER_STATE_REBOOTING
                        : BD_POWER_STATE_SHUTTING_DOWN;

  bool entered =
      origin == BD_POWER_STATE_IDLE
          ? enter_deep_layers(power, report)
          : enter_resources(power, report, BD_POWER_SLEEP_DEEP);
  if (!entered) {
    report->rollback_attempted = true;
    bool recovered = origin == BD_POWER_STATE_IDLE
                         ? restore_all(power, report)
                         : restore_deep_layer(power, report);
    report->recovery_complete = recovered;
    power->state = recovered ? origin : BD_POWER_STATE_DEGRADED;
    return report_end(power, report, NULL, power->state,
                      recovered ? BD_POWER_HOOK_FAILED : BD_POWER_DEGRADED);
  }

  report->backend_called = true;
  errno = 0;
  bd_power_backend_status status =
      power->config.backend.shutdown(power->config.backend.user, reboot);
  if (status == BD_POWER_BACKEND_OK) {
    /* The backend contract says a successful shutdown never returns. */
    status = BD_POWER_BACKEND_NO_EFFECT;
    set_detail(report, "shutdown backend returned without powering off");
    record_failure(report,
                   reboot ? BD_POWER_STEP_REBOOT : BD_POWER_STEP_POWEROFF, 0,
                   0);
  } else if (status == BD_POWER_BACKEND_ERROR) {
    record_failure(report,
                   reboot ? BD_POWER_STEP_REBOOT : BD_POWER_STEP_POWEROFF, 0,
                   errno != 0 ? errno : EIO);
  } else {
    record_failure(report,
                   reboot ? BD_POWER_STEP_REBOOT : BD_POWER_STEP_POWEROFF, 0,
                   0);
  }

  report->rollback_attempted = true;
  bool recovered = origin == BD_POWER_STATE_IDLE
                       ? restore_all(power, report)
                       : restore_deep_layer(power, report);
  report->recovery_complete = recovered;
  power->state = recovered ? origin : BD_POWER_STATE_DEGRADED;
  return report_end(power, report, NULL, power->state,
                    recovered ? map_backend_result(status)
                              : BD_POWER_DEGRADED);
}

bd_power *bd_power_create(const bd_power_config *config, char *error,
                          size_t error_size) {
  if (config == NULL) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "missing power configuration");
    }
    return NULL;
  }

  bool any_action = config->allow_light_sleep || config->allow_deep_sleep ||
                    config->allow_poweroff || config->allow_reboot;
  if (any_action) {
    if (config->hooks.input_release == NULL ||
        config->hooks.input_acquire == NULL ||
        config->hooks.app_pause == NULL || config->hooks.app_resume == NULL ||
        config->hooks.display_off == NULL || config->hooks.display_on == NULL) {
      if (error != NULL && error_size > 0) {
        snprintf(error, error_size,
                 "input, app and display hooks are required for sleep and "
                 "shutdown");
      }
      return NULL;
    }
    if ((config->hooks.resource_prepare == NULL) !=
        (config->hooks.resource_restore == NULL)) {
      if (error != NULL && error_size > 0) {
        snprintf(error, error_size,
                 "resource_prepare and resource_restore must be supplied "
                 "together");
      }
      return NULL;
    }
  }
  if (config->allow_deep_sleep && config->backend.deep_sleep == NULL) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size,
               "deep sleep is enabled without a deep_sleep backend");
    }
    return NULL;
  }
  if ((config->allow_poweroff || config->allow_reboot) &&
      config->backend.shutdown == NULL) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size,
               "poweroff/reboot are enabled without a shutdown backend");
    }
    return NULL;
  }

  bd_power *power = calloc(1, sizeof(*power));
  if (power == NULL) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "out of memory");
    }
    return NULL;
  }
  power->config = *config;
  power->state = BD_POWER_STATE_IDLE;
  return power;
}

void bd_power_destroy(bd_power *power) {
  free(power);
}

bd_power_result bd_power_sleep_light(bd_power *power,
                                     bd_power_report *out) {
  if (power == NULL) {
    return invalid_call(out, BD_POWER_ACTION_LIGHT_SLEEP);
  }
  bd_power_report report;
  report_begin(power, &report, BD_POWER_ACTION_LIGHT_SLEEP,
               BD_POWER_SLEEP_LIGHT);

  if (power->state == BD_POWER_STATE_LIGHT_SLEEP) {
    set_detail(&report, "light sleep is already active");
    return report_end(power, &report, out, power->state, BD_POWER_ALREADY);
  }
  if (power->state == BD_POWER_STATE_DEGRADED) {
    set_detail(&report, "recovery is required before sleeping again");
    return report_end(power, &report, out, power->state,
                      BD_POWER_RECOVERY_REQUIRED);
  }
  if (state_is_busy(power->state) ||
      power->state != BD_POWER_STATE_IDLE) {
    set_detail(&report, "another power transition is in progress");
    return report_end(power, &report, out, power->state, BD_POWER_BUSY);
  }
  if (!power->config.allow_light_sleep) {
    set_detail(&report, "light sleep is disabled by configuration");
    return report_end(power, &report, out, power->state, BD_POWER_DISABLED);
  }
  if (power->cancel_pending) {
    power->cancel_pending = false;
    set_detail(&report, "queued sleep request was cancelled");
    return report_end(power, &report, out, power->state, BD_POWER_CANCELLED);
  }

  power->state = BD_POWER_STATE_ENTERING_LIGHT;
  if (enter_light_layers(power, &report)) {
    power->state = BD_POWER_STATE_LIGHT_SLEEP;
    set_detail(&report,
               "light sleep entered; display_off is not suspend-to-RAM");
    return report_end(power, &report, out, power->state, BD_POWER_OK);
  }

  report.rollback_attempted = true;
  bool recovered = restore_all(power, &report);
  report.recovery_complete = recovered;
  power->state =
      recovered ? BD_POWER_STATE_IDLE : BD_POWER_STATE_DEGRADED;
  return report_end(power, &report, out, power->state,
                    recovered ? BD_POWER_HOOK_FAILED : BD_POWER_DEGRADED);
}

bd_power_result bd_power_sleep_deep(bd_power *power, bd_power_report *out) {
  if (power == NULL) {
    return invalid_call(out, BD_POWER_ACTION_DEEP_SLEEP);
  }
  bd_power_report report;
  report_begin(power, &report, BD_POWER_ACTION_DEEP_SLEEP,
               BD_POWER_SLEEP_DEEP);

  if (power->state == BD_POWER_STATE_DEGRADED) {
    set_detail(&report, "recovery is required before sleeping again");
    return report_end(power, &report, out, power->state,
                      BD_POWER_RECOVERY_REQUIRED);
  }
  if (state_is_busy(power->state)) {
    set_detail(&report, "another power transition is in progress");
    return report_end(power, &report, out, power->state, BD_POWER_BUSY);
  }
  if (power->state != BD_POWER_STATE_IDLE &&
      power->state != BD_POWER_STATE_LIGHT_SLEEP) {
    set_detail(&report, "deep sleep is only valid from idle or light sleep");
    return report_end(power, &report, out, power->state, BD_POWER_INVALID);
  }
  if (!power->config.allow_deep_sleep) {
    set_detail(&report, "deep sleep is disabled by configuration");
    return report_end(power, &report, out, power->state, BD_POWER_DISABLED);
  }
  if (power->state == BD_POWER_STATE_IDLE && power->cancel_pending) {
    power->cancel_pending = false;
    set_detail(&report, "queued sleep request was cancelled");
    return report_end(power, &report, out, power->state, BD_POWER_CANCELLED);
  }

  bool from_light = power->state == BD_POWER_STATE_LIGHT_SLEEP;
  power->state = BD_POWER_STATE_ENTERING_DEEP;
  bd_power_result result =
      from_light ? sleep_deep_from_light(power, &report)
                 : sleep_deep_from_idle(power, &report);
  power->last = report;
  if (out != NULL) {
    *out = report;
  }
  return result;
}

bd_power_result bd_power_wake(bd_power *power, bd_power_report *out) {
  if (power == NULL) {
    return invalid_call(out, BD_POWER_ACTION_WAKE);
  }
  bd_power_report report;
  report_begin(power, &report, BD_POWER_ACTION_WAKE, BD_POWER_SLEEP_LIGHT);

  if (power->state == BD_POWER_STATE_IDLE) {
    set_detail(&report, "already awake");
    return report_end(power, &report, out, power->state,
                      BD_POWER_NOT_SLEEPING);
  }
  if (state_is_busy(power->state)) {
    set_detail(&report, "another power transition is in progress");
    return report_end(power, &report, out, power->state, BD_POWER_BUSY);
  }
  if (power->state != BD_POWER_STATE_LIGHT_SLEEP &&
      power->state != BD_POWER_STATE_DEGRADED) {
    set_detail(&report, "wake is only valid from light sleep or degraded");
    return report_end(power, &report, out, power->state, BD_POWER_INVALID);
  }

  power->state = BD_POWER_STATE_WAKING;
  bool recovered = restore_all(power, &report);
  if (!recovered) {
    report.recovery_complete = false;
    power->state = BD_POWER_STATE_DEGRADED;
    return report_end(power, &report, out, power->state, BD_POWER_DEGRADED);
  }
  report.recovery_complete = true;
  power->state = BD_POWER_STATE_IDLE;
  set_detail(&report, "woke and restored all power hooks");
  return report_end(power, &report, out, power->state, BD_POWER_OK);
}

bd_power_result bd_power_cancel(bd_power *power, bd_power_report *out) {
  if (power == NULL) {
    return invalid_call(out, BD_POWER_ACTION_CANCEL);
  }
  bd_power_report report;
  report_begin(power, &report, BD_POWER_ACTION_CANCEL,
               BD_POWER_SLEEP_LIGHT);

  if (power->state == BD_POWER_STATE_IDLE) {
    if (power->cancel_pending) {
      set_detail(&report, "a sleep cancel is already queued");
      return report_end(power, &report, out, power->state, BD_POWER_ALREADY);
    }
    power->cancel_pending = true;
    set_detail(&report, "next sleep request will be cancelled");
    return report_end(power, &report, out, power->state, BD_POWER_OK);
  }
  if (power->state == BD_POWER_STATE_DEGRADED) {
    set_detail(&report, "call wake to retry recovery");
    return report_end(power, &report, out, power->state,
                      BD_POWER_RECOVERY_REQUIRED);
  }
  if (state_is_busy(power->state)) {
    set_detail(&report, "another power transition is in progress");
    return report_end(power, &report, out, power->state, BD_POWER_BUSY);
  }
  if (power->state != BD_POWER_STATE_LIGHT_SLEEP) {
    set_detail(&report, "cancel is only valid while idle or light sleeping");
    return report_end(power, &report, out, power->state, BD_POWER_INVALID);
  }

  power->state = BD_POWER_STATE_WAKING;
  bool recovered = restore_all(power, &report);
  if (!recovered) {
    report.recovery_complete = false;
    power->state = BD_POWER_STATE_DEGRADED;
    return report_end(power, &report, out, power->state, BD_POWER_DEGRADED);
  }
  report.recovery_complete = true;
  power->state = BD_POWER_STATE_IDLE;
  set_detail(&report, "cancelled light sleep and restored all power hooks");
  return report_end(power, &report, out, power->state, BD_POWER_OK);
}

bd_power_result bd_power_poweroff(bd_power *power, bd_power_poweroff_mode mode,
                                  bd_power_report *out) {
  if (power == NULL) {
    return invalid_call(out, BD_POWER_ACTION_POWEROFF);
  }
  bd_power_report report;
  report_begin(power, &report, BD_POWER_ACTION_POWEROFF,
               BD_POWER_SLEEP_DEEP);

  if (mode == BD_POWER_POWEROFF_PMIC_SAFE) {
    set_detail(&report, "%s", bd_power_linux_pmic_blocker());
    return report_end(power, &report, out, power->state,
                      BD_POWER_UNSUPPORTED);
  }
  if (mode != BD_POWER_POWEROFF_STANDARD) {
    set_detail(&report, "unknown poweroff mode");
    return report_end(power, &report, out, power->state, BD_POWER_INVALID);
  }
  bd_power_result result =
      shutdown_common(power, &report, false, power->config.allow_poweroff);
  if (out != NULL) {
    *out = report;
  }
  return result;
}

bd_power_result bd_power_reboot(bd_power *power, bd_power_report *out) {
  if (power == NULL) {
    return invalid_call(out, BD_POWER_ACTION_REBOOT);
  }
  bd_power_report report;
  report_begin(power, &report, BD_POWER_ACTION_REBOOT,
               BD_POWER_SLEEP_DEEP);
  bd_power_result result =
      shutdown_common(power, &report, true, power->config.allow_reboot);
  if (out != NULL) {
    *out = report;
  }
  return result;
}

bd_power_state bd_power_current_state(const bd_power *power) {
  return power != NULL ? power->state : BD_POWER_STATE_UNKNOWN;
}

bool bd_power_pending_cancel(const bd_power *power) {
  return power != NULL && power->cancel_pending;
}

const bd_power_report *bd_power_last_report(const bd_power *power) {
  return power != NULL ? &power->last : NULL;
}

const char *bd_power_state_name(bd_power_state state) {
  switch (state) {
    case BD_POWER_STATE_UNKNOWN:
      return "unknown";
    case BD_POWER_STATE_IDLE:
      return "idle";
    case BD_POWER_STATE_ENTERING_LIGHT:
      return "entering-light";
    case BD_POWER_STATE_LIGHT_SLEEP:
      return "light-sleep";
    case BD_POWER_STATE_ENTERING_DEEP:
      return "entering-deep";
    case BD_POWER_STATE_WAKING:
      return "waking";
    case BD_POWER_STATE_SHUTTING_DOWN:
      return "shutting-down";
    case BD_POWER_STATE_REBOOTING:
      return "rebooting";
    case BD_POWER_STATE_DEGRADED:
      return "degraded";
  }
  return "unknown";
}

const char *bd_power_result_name(bd_power_result result) {
  switch (result) {
    case BD_POWER_OK:
      return "ok";
    case BD_POWER_ALREADY:
      return "already";
    case BD_POWER_BUSY:
      return "busy";
    case BD_POWER_CANCELLED:
      return "cancelled";
    case BD_POWER_NOT_SLEEPING:
      return "not-sleeping";
    case BD_POWER_DISABLED:
      return "disabled";
    case BD_POWER_UNSUPPORTED:
      return "unsupported";
    case BD_POWER_RECOVERY_REQUIRED:
      return "recovery-required";
    case BD_POWER_HOOK_FAILED:
      return "hook-failed";
    case BD_POWER_BACKEND_FAILED:
      return "backend-failed";
    case BD_POWER_NO_EFFECT:
      return "no-effect";
    case BD_POWER_DEGRADED:
      return "degraded";
    case BD_POWER_INVALID:
      return "invalid";
  }
  return "invalid";
}

const char *bd_power_action_name(bd_power_action action) {
  switch (action) {
    case BD_POWER_ACTION_NONE:
      return "none";
    case BD_POWER_ACTION_LIGHT_SLEEP:
      return "light-sleep";
    case BD_POWER_ACTION_DEEP_SLEEP:
      return "deep-sleep";
    case BD_POWER_ACTION_WAKE:
      return "wake";
    case BD_POWER_ACTION_CANCEL:
      return "cancel";
    case BD_POWER_ACTION_POWEROFF:
      return "poweroff";
    case BD_POWER_ACTION_REBOOT:
      return "reboot";
  }
  return "none";
}

const char *bd_power_step_name(bd_power_step step) {
  switch (step) {
    case BD_POWER_STEP_NONE:
      return "none";
    case BD_POWER_STEP_INPUT_RELEASE:
      return "input-release";
    case BD_POWER_STEP_APP_PAUSE:
      return "app-pause";
    case BD_POWER_STEP_RESOURCE_PREPARE_LIGHT:
      return "resource-prepare-light";
    case BD_POWER_STEP_RESOURCE_PREPARE_DEEP:
      return "resource-prepare-deep";
    case BD_POWER_STEP_DISPLAY_OFF:
      return "display-off";
    case BD_POWER_STEP_DEEP_SLEEP:
      return "deep-sleep";
    case BD_POWER_STEP_DISPLAY_ON:
      return "display-on";
    case BD_POWER_STEP_RESOURCE_RESTORE_DEEP:
      return "resource-restore-deep";
    case BD_POWER_STEP_RESOURCE_RESTORE_LIGHT:
      return "resource-restore-light";
    case BD_POWER_STEP_APP_RESUME:
      return "app-resume";
    case BD_POWER_STEP_INPUT_ACQUIRE:
      return "input-acquire";
    case BD_POWER_STEP_POWEROFF:
      return "poweroff";
    case BD_POWER_STEP_REBOOT:
      return "reboot";
  }
  return "none";
}
