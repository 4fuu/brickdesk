#define _POSIX_C_SOURCE 200809L

#include "battery_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define BD_BATTERY_INTERVAL_MIN_MS 1000u
#define BD_BATTERY_INTERVAL_MAX_MS (24u * 60u * 60u * 1000u)

const char *bd_battery_result_name(bd_battery_result result) {
  switch (result) {
    case BD_BATTERY_OK:
      return "ok";
    case BD_BATTERY_ERR_ARGUMENT:
      return "invalid-argument";
    case BD_BATTERY_ERR_UNSUPPORTED:
      return "unsupported";
    case BD_BATTERY_ERR_STALE:
      return "stale";
    case BD_BATTERY_ERR_IO:
      return "io-error";
  }
  return "unknown";
}

void bd_battery_error_clear(bd_battery_error *error) {
  if (error == NULL) {
    return;
  }
  error->code = BD_BATTERY_OK;
  error->message[0] = '\0';
}

void bd_battery_set_error(bd_battery_error *error, bd_battery_result code,
                          const char *format, ...) {
  if (error == NULL) {
    return;
  }
  error->code = code;
  if (format == NULL) {
    error->message[0] = '\0';
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(error->message, sizeof(error->message), format, args);
  va_end(args);
}

bool bd_battery_copy_string(char *dst, size_t dst_size, const char *src) {
  if (dst == NULL || dst_size == 0 || src == NULL) {
    if (dst != NULL && dst_size > 0) {
      dst[0] = '\0';
    }
    return false;
  }
  size_t length = strlen(src);
  if (length >= dst_size) {
    memcpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
    return false;
  }
  memcpy(dst, src, length + 1);
  return true;
}

const char *bd_battery_supply_type_name(bd_battery_supply_type type) {
  switch (type) {
    case BD_BATTERY_SUPPLY_UNKNOWN:
      return "unknown";
    case BD_BATTERY_SUPPLY_BATTERY:
      return "battery";
    case BD_BATTERY_SUPPLY_USB:
      return "usb";
    case BD_BATTERY_SUPPLY_MAINS:
      return "mains";
    case BD_BATTERY_SUPPLY_UPS:
      return "ups";
    case BD_BATTERY_SUPPLY_OTHER:
      return "other";
  }
  return "unknown";
}

const char *bd_battery_charge_status_name(bd_battery_charge_status status) {
  switch (status) {
    case BD_BATTERY_CHARGE_UNKNOWN:
      return "unknown";
    case BD_BATTERY_CHARGE_CHARGING:
      return "charging";
    case BD_BATTERY_CHARGE_DISCHARGING:
      return "discharging";
    case BD_BATTERY_CHARGE_NOT_CHARGING:
      return "not-charging";
    case BD_BATTERY_CHARGE_FULL:
      return "full";
  }
  return "unknown";
}

const char *bd_battery_health_name(bd_battery_health health) {
  switch (health) {
    case BD_BATTERY_HEALTH_UNKNOWN:
      return "unknown";
    case BD_BATTERY_HEALTH_GOOD:
      return "good";
    case BD_BATTERY_HEALTH_OVERHEAT:
      return "overheat";
    case BD_BATTERY_HEALTH_DEAD:
      return "dead";
    case BD_BATTERY_HEALTH_OVER_VOLTAGE:
      return "over-voltage";
    case BD_BATTERY_HEALTH_FAILURE:
      return "failure";
    case BD_BATTERY_HEALTH_COLD:
      return "cold";
    case BD_BATTERY_HEALTH_OVER_CURRENT:
      return "over-current";
    case BD_BATTERY_HEALTH_OTHER:
      return "other";
  }
  return "unknown";
}

const char *bd_battery_policy_level_name(bd_battery_policy_level level) {
  switch (level) {
    case BD_BATTERY_LEVEL_UNKNOWN:
      return "unknown";
    case BD_BATTERY_LEVEL_NORMAL:
      return "normal";
    case BD_BATTERY_LEVEL_REMINDER:
      return "reminder";
    case BD_BATTERY_LEVEL_CRITICAL:
      return "critical";
  }
  return "unknown";
}

const char *bd_battery_recommendation_name(
    bd_battery_recommendation recommendation) {
  switch (recommendation) {
    case BD_BATTERY_RECOMMEND_NONE:
      return "none";
    case BD_BATTERY_RECOMMEND_SHOW_REMINDER:
      return "show-reminder";
    case BD_BATTERY_RECOMMEND_SHOW_CRITICAL:
      return "show-critical";
    case BD_BATTERY_RECOMMEND_PREPARE_SHUTDOWN:
      return "prepare-shutdown";
  }
  return "none";
}

const char *bd_battery_event_name(uint32_t event) {
  switch (event) {
    case BD_BATTERY_EVENT_LOW:
      return "low";
    case BD_BATTERY_EVENT_CRITICAL:
      return "critical";
    case BD_BATTERY_EVENT_RECOVERED:
      return "recovered";
    case BD_BATTERY_EVENT_PLUGGED:
      return "plugged";
    case BD_BATTERY_EVENT_UNPLUGGED:
      return "unplugged";
    case BD_BATTERY_EVENT_UNKNOWN:
      return "unknown";
    default:
      return "none";
  }
}

const char *bd_battery_policy_result_name(bd_battery_policy_result result) {
  switch (result) {
    case BD_BATTERY_POLICY_OK:
      return "ok";
    case BD_BATTERY_POLICY_DISABLED:
      return "disabled";
    case BD_BATTERY_POLICY_INVALID:
      return "invalid";
  }
  return "invalid";
}

void bd_battery_policy_config_default(bd_battery_policy_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->enabled = true;
  config->reminder_percent = 20;
  config->critical_percent = 5;
  config->suppress_while_plugged = true;
  config->recommend_shutdown = true;
  config->reminder_hysteresis_percent = 3;
  config->critical_hysteresis_percent = 2;
  config->confirm_samples = 2;
  config->reminder_repeat_ms = 10u * 60u * 1000u;
  config->critical_repeat_ms = 2u * 60u * 1000u;
  config->sample_normal_ms = 60u * 1000u;
  config->sample_reminder_ms = 30u * 1000u;
  config->sample_critical_ms = 15u * 1000u;
  config->sample_charging_ms = 60u * 1000u;
  config->sample_confirm_ms = 5u * 1000u;
  config->unknown_initial_ms = 5u * 1000u;
  config->unknown_max_ms = 120u * 1000u;
}

static bool validate_interval(char *error, size_t error_size,
                              const char *name, uint32_t value,
                              bool allow_zero) {
  if (allow_zero && value == 0) {
    return true;
  }
  if (value < BD_BATTERY_INTERVAL_MIN_MS ||
      value > BD_BATTERY_INTERVAL_MAX_MS) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size,
               "%s must be 0 or between %u and %u ms, got %u", name,
               BD_BATTERY_INTERVAL_MIN_MS, BD_BATTERY_INTERVAL_MAX_MS, value);
    }
    return false;
  }
  return true;
}

bool bd_battery_policy_config_validate(const bd_battery_policy_config *config,
                                       char *error, size_t error_size) {
  if (error != NULL && error_size > 0) {
    error[0] = '\0';
  }
  if (config == NULL) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "configuration is null");
    }
    return false;
  }
  if (!config->enabled) {
    return true;
  }
  if (config->reminder_percent < 1 || config->reminder_percent > 100) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size,
               "reminder_percent must be 1..100, got %d",
               config->reminder_percent);
    }
    return false;
  }
  if (config->critical_percent < 0 ||
      config->critical_percent >= config->reminder_percent) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size,
               "critical_percent must be 0..%d and below reminder_percent, "
               "got %d",
               config->reminder_percent - 1, config->critical_percent);
    }
    return false;
  }
  if (config->reminder_hysteresis_percent < 0 ||
      config->reminder_hysteresis_percent > 100 ||
      config->critical_hysteresis_percent < 0 ||
      config->critical_hysteresis_percent > 100) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "hysteresis must be 0..100");
    }
    return false;
  }
  if (config->reminder_percent + config->reminder_hysteresis_percent > 100 ||
      config->critical_percent + config->critical_hysteresis_percent > 100) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size,
               "threshold plus hysteresis must not exceed 100");
    }
    return false;
  }
  if (config->confirm_samples < 1 || config->confirm_samples > 100) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size,
               "confirm_samples must be 1..100, got %u",
               config->confirm_samples);
    }
    return false;
  }
  if (!validate_interval(error, error_size, "reminder_repeat_ms",
                         config->reminder_repeat_ms, true) ||
      !validate_interval(error, error_size, "critical_repeat_ms",
                         config->critical_repeat_ms, true) ||
      !validate_interval(error, error_size, "sample_normal_ms",
                         config->sample_normal_ms, false) ||
      !validate_interval(error, error_size, "sample_reminder_ms",
                         config->sample_reminder_ms, false) ||
      !validate_interval(error, error_size, "sample_critical_ms",
                         config->sample_critical_ms, false) ||
      !validate_interval(error, error_size, "sample_charging_ms",
                         config->sample_charging_ms, false) ||
      !validate_interval(error, error_size, "sample_confirm_ms",
                         config->sample_confirm_ms, false) ||
      !validate_interval(error, error_size, "unknown_initial_ms",
                         config->unknown_initial_ms, false) ||
      !validate_interval(error, error_size, "unknown_max_ms",
                         config->unknown_max_ms, false)) {
    return false;
  }
  if (config->unknown_initial_ms > config->unknown_max_ms) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size,
               "unknown_initial_ms must not exceed unknown_max_ms");
    }
    return false;
  }
  return true;
}

static void output_zero(bd_battery_policy_output *output) {
  memset(output, 0, sizeof(*output));
  output->level = BD_BATTERY_LEVEL_UNKNOWN;
  output->recommendation = BD_BATTERY_RECOMMEND_NONE;
}

bd_battery_policy_result bd_battery_policy_init(
    bd_battery_policy *policy, const bd_battery_policy_config *config,
    uint64_t now_ms, bd_battery_policy_output *output,
    bd_battery_error *error) {
  if (policy == NULL || output == NULL) {
    bd_battery_set_error(error, BD_BATTERY_ERR_ARGUMENT,
                         "policy and output are required");
    return BD_BATTERY_POLICY_INVALID;
  }
  memset(policy, 0, sizeof(*policy));
  if (config == NULL) {
    bd_battery_policy_config_default(&policy->config);
  } else {
    policy->config = *config;
  }
  output_zero(output);

  if (!policy->config.enabled) {
    policy->initialized = true;
    policy->level = BD_BATTERY_LEVEL_UNKNOWN;
    policy->candidate = BD_BATTERY_LEVEL_UNKNOWN;
    policy->next_sample_ms = 0;
    output->level = BD_BATTERY_LEVEL_UNKNOWN;
    bd_battery_error_clear(error);
    return BD_BATTERY_POLICY_DISABLED;
  }

  char detail[BD_BATTERY_MESSAGE_MAX];
  if (!bd_battery_policy_config_validate(&policy->config, detail,
                                         sizeof(detail))) {
    policy->initialized = false;
    bd_battery_set_error(error, BD_BATTERY_ERR_ARGUMENT, "%s", detail);
    return BD_BATTERY_POLICY_INVALID;
  }

  policy->initialized = true;
  policy->level = BD_BATTERY_LEVEL_UNKNOWN;
  policy->candidate = BD_BATTERY_LEVEL_UNKNOWN;
  policy->next_sample_ms = now_ms;
  output->level = BD_BATTERY_LEVEL_UNKNOWN;
  output->next_sample_ms = now_ms;
  output->next_sample_after_ms = 0;
  bd_battery_error_clear(error);
  return BD_BATTERY_POLICY_OK;
}

static uint64_t elapsed_ms(uint64_t now, uint64_t then) {
  return now >= then ? now - then : 0;
}

static uint64_t add_delay(uint64_t now, uint32_t delay) {
  if (UINT64_MAX - now < (uint64_t)delay) {
    return UINT64_MAX;
  }
  return now + (uint64_t)delay;
}

static void policy_set_level(bd_battery_policy *policy,
                             bd_battery_policy_output *output, uint64_t now,
                             bd_battery_policy_level level) {
  bd_battery_policy_level old = policy->level;
  if (level == old) {
    return;
  }
  policy->level = level;
  policy->candidate = level;
  policy->candidate_count = 0;

  switch (level) {
    case BD_BATTERY_LEVEL_UNKNOWN:
      output->events |= BD_BATTERY_EVENT_UNKNOWN;
      policy->low_alerted = false;
      policy->critical_alerted = false;
      break;
    case BD_BATTERY_LEVEL_NORMAL:
      if (old == BD_BATTERY_LEVEL_REMINDER ||
          old == BD_BATTERY_LEVEL_CRITICAL) {
        output->events |= BD_BATTERY_EVENT_RECOVERED;
      }
      policy->low_alerted = false;
      policy->critical_alerted = false;
      break;
    case BD_BATTERY_LEVEL_REMINDER:
      policy->critical_alerted = false;
      policy->low_alerted = false;
      if (!policy->suppressed) {
        output->events |= BD_BATTERY_EVENT_LOW;
        policy->low_alerted = true;
        policy->last_low_ms = now;
      }
      break;
    case BD_BATTERY_LEVEL_CRITICAL:
      policy->low_alerted = false;
      policy->critical_alerted = false;
      if (!policy->suppressed) {
        output->events |= BD_BATTERY_EVENT_CRITICAL;
        policy->critical_alerted = true;
        policy->last_critical_ms = now;
      }
      break;
  }
}

static void policy_repeat_alert(bd_battery_policy *policy,
                                bd_battery_policy_output *output,
                                uint64_t now) {
  if (policy->suppressed) {
    return;
  }
  if (policy->level == BD_BATTERY_LEVEL_REMINDER) {
    bool due = !policy->low_alerted;
    if (!due && policy->config.reminder_repeat_ms != 0) {
      due = elapsed_ms(now, policy->last_low_ms) >=
            policy->config.reminder_repeat_ms;
    }
    if (due) {
      output->events |= BD_BATTERY_EVENT_LOW;
      policy->low_alerted = true;
      policy->last_low_ms = now;
    }
  } else if (policy->level == BD_BATTERY_LEVEL_CRITICAL) {
    bool due = !policy->critical_alerted;
    if (!due && policy->config.critical_repeat_ms != 0) {
      due = elapsed_ms(now, policy->last_critical_ms) >=
            policy->config.critical_repeat_ms;
    }
    if (due) {
      output->events |= BD_BATTERY_EVENT_CRITICAL;
      policy->critical_alerted = true;
      policy->last_critical_ms = now;
    }
  }
}

static bd_battery_policy_level target_level(
    const bd_battery_policy *policy, int capacity) {
  const bd_battery_policy_config *config = &policy->config;
  if (capacity <= config->critical_percent) {
    return BD_BATTERY_LEVEL_CRITICAL;
  }
  if (policy->level == BD_BATTERY_LEVEL_CRITICAL) {
    if (capacity < config->critical_percent +
                       config->critical_hysteresis_percent) {
      return BD_BATTERY_LEVEL_CRITICAL;
    }
    if (capacity < config->reminder_percent +
                       config->reminder_hysteresis_percent) {
      return BD_BATTERY_LEVEL_REMINDER;
    }
    return BD_BATTERY_LEVEL_NORMAL;
  }
  if (policy->level == BD_BATTERY_LEVEL_REMINDER) {
    if (capacity <= config->reminder_percent) {
      return BD_BATTERY_LEVEL_REMINDER;
    }
    if (capacity < config->reminder_percent +
                       config->reminder_hysteresis_percent) {
      return BD_BATTERY_LEVEL_REMINDER;
    }
    return BD_BATTERY_LEVEL_NORMAL;
  }
  if (capacity <= config->reminder_percent) {
    return BD_BATTERY_LEVEL_REMINDER;
  }
  return BD_BATTERY_LEVEL_NORMAL;
}

static bool capacity_is_usable(const bd_battery_status *status) {
  if (status == NULL || !status->battery_available ||
      !status->capacity_known) {
    return false;
  }
  if (status->capacity_percent < 0 || status->capacity_percent > 100) {
    return false;
  }
  if (status->present_known && !status->present) {
    return false;
  }
  return true;
}

static void policy_update_plug_state(bd_battery_policy *policy,
                                     bd_battery_policy_output *output,
                                     const bd_battery_status *status) {
  bool plugged_known = false;
  bool plugged = false;
  if (status != NULL && status->external_power_known) {
    plugged_known = true;
    plugged = status->external_power;
  } else if (status != NULL && status->status_known &&
             (status->status == BD_BATTERY_CHARGE_CHARGING ||
              status->status == BD_BATTERY_CHARGE_FULL)) {
    /*
     * External power is unknown, but a charging/full battery is enough to
     * suppress low-battery alerts.  "Not charging" is deliberately not
     * treated as plugged; discharging is handled below as the symmetric
     * recovery signal.
     */
    plugged_known = true;
    plugged = true;
  } else if (status != NULL && status->status_known &&
             status->status == BD_BATTERY_CHARGE_DISCHARGING) {
    /*
     * Symmetric inference for recovery.  The battery is supplying the load,
     * so low-battery alerts must resume even if no external-supply node
     * exposes online.  Shutdown recommendations still require
     * external_power_known below.
     */
    plugged_known = true;
    plugged = false;
  }

  bool was_known = policy->plugged_known;
  bool was_plugged = policy->plugged;
  bool first = !policy->plug_state_seen;

  if (!first) {
    if (plugged_known && plugged && !(was_known && was_plugged)) {
      output->events |= BD_BATTERY_EVENT_PLUGGED;
      if (policy->config.suppress_while_plugged) {
        policy->low_alerted = false;
        policy->critical_alerted = false;
      }
    } else if (plugged_known && !plugged && !(was_known && !was_plugged)) {
      output->events |= BD_BATTERY_EVENT_UNPLUGGED;
    }
  } else {
    policy->plug_state_seen = true;
    if (policy->config.suppress_while_plugged && plugged_known && plugged) {
      policy->low_alerted = false;
      policy->critical_alerted = false;
    }
  }

  policy->plugged_known = plugged_known;
  policy->plugged = plugged;
  policy->suppressed =
      policy->config.suppress_while_plugged && plugged_known && plugged;

  output->plugged_in_known = plugged_known;
  output->plugged_in = plugged_known && plugged;
  output->suppressed = policy->suppressed;
}

static uint32_t policy_interval(const bd_battery_policy *policy,
                                bool capacity_known) {
  const bd_battery_policy_config *config = &policy->config;
  uint32_t interval;
  if (!capacity_known) {
    unsigned shift =
        policy->unknown_streak > 0 ? policy->unknown_streak - 1 : 0;
    if (shift > 8) {
      shift = 8;
    }
    uint64_t value = (uint64_t)config->unknown_initial_ms << shift;
    if (value > config->unknown_max_ms) {
      value = config->unknown_max_ms;
    }
    interval = (uint32_t)value;
  } else if (policy->suppressed) {
    interval = config->sample_charging_ms;
  } else if (policy->level == BD_BATTERY_LEVEL_CRITICAL) {
    interval = config->sample_critical_ms;
  } else if (policy->level == BD_BATTERY_LEVEL_REMINDER) {
    interval = config->sample_reminder_ms;
  } else {
    interval = config->sample_normal_ms;
  }

  if (capacity_known && policy->candidate_count > 0 &&
      policy->candidate != policy->level &&
      config->sample_confirm_ms < interval) {
    interval = config->sample_confirm_ms;
  }
  return interval;
}

static bd_battery_recommendation policy_recommendation(
    const bd_battery_policy *policy, const bd_battery_status *status,
    bool capacity_known) {
  if (!capacity_known || policy->level == BD_BATTERY_LEVEL_UNKNOWN ||
      policy->suppressed) {
    return BD_BATTERY_RECOMMEND_NONE;
  }
  if (policy->level == BD_BATTERY_LEVEL_CRITICAL) {
    if (policy->config.recommend_shutdown && status != NULL &&
        status->present_known && status->present &&
        status->external_power_known && !status->external_power) {
      return BD_BATTERY_RECOMMEND_PREPARE_SHUTDOWN;
    }
    return BD_BATTERY_RECOMMEND_SHOW_CRITICAL;
  }
  if (policy->level == BD_BATTERY_LEVEL_REMINDER) {
    return BD_BATTERY_RECOMMEND_SHOW_REMINDER;
  }
  return BD_BATTERY_RECOMMEND_NONE;
}

bd_battery_policy_result bd_battery_policy_update(
    bd_battery_policy *policy, const bd_battery_status *status,
    uint64_t now_ms, bd_battery_policy_output *output) {
  if (policy == NULL || output == NULL || !policy->initialized) {
    if (output != NULL) {
      output_zero(output);
    }
    return BD_BATTERY_POLICY_INVALID;
  }
  output_zero(output);
  if (!policy->config.enabled) {
    output->level = policy->level;
    policy->next_sample_ms = 0;
    return BD_BATTERY_POLICY_DISABLED;
  }

  policy_update_plug_state(policy, output, status);
  bool capacity_known = capacity_is_usable(status);
  output->capacity_known = capacity_known;

  if (!capacity_known) {
    policy->candidate = BD_BATTERY_LEVEL_UNKNOWN;
    policy->candidate_count = 0;
    ++policy->unknown_streak;
    policy_set_level(policy, output, now_ms, BD_BATTERY_LEVEL_UNKNOWN);
  } else {
    policy->unknown_streak = 0;
    int capacity = status->capacity_percent;
    bd_battery_policy_level target = target_level(policy, capacity);

    if (policy->level == BD_BATTERY_LEVEL_UNKNOWN &&
        target == BD_BATTERY_LEVEL_NORMAL) {
      /* Never delay the safe state behind a confirmation window. */
      policy_set_level(policy, output, now_ms, target);
    } else if (target == policy->level) {
      policy->candidate = target;
      policy->candidate_count = 0;
    } else {
      if (policy->candidate == target) {
        ++policy->candidate_count;
      } else {
        policy->candidate = target;
        policy->candidate_count = 1;
      }
      if (policy->candidate_count >= policy->config.confirm_samples) {
        policy_set_level(policy, output, now_ms, target);
      }
    }

    policy_repeat_alert(policy, output, now_ms);
  }

  output->level = policy->level;
  output->recommendation =
      policy_recommendation(policy, status, capacity_known);
  uint32_t interval = policy_interval(policy, capacity_known);
  output->next_sample_after_ms = interval;
  output->next_sample_ms = add_delay(now_ms, interval);
  policy->next_sample_ms = output->next_sample_ms;
  return BD_BATTERY_POLICY_OK;
}

uint64_t bd_battery_policy_next_sample_ms(const bd_battery_policy *policy) {
  return policy == NULL ? 0 : policy->next_sample_ms;
}
