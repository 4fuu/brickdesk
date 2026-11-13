#define _POSIX_C_SOURCE 200809L

#include "brickdesk/device_battery.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *yes_no_unknown(bool known, bool value) {
  if (!known) {
    return "unknown";
  }
  return value ? "yes" : "no";
}

static void print_events(uint32_t events) {
  static const struct {
    uint32_t bit;
    const char *name;
  } names[] = {
      {BD_BATTERY_EVENT_LOW, "low"},
      {BD_BATTERY_EVENT_CRITICAL, "critical"},
      {BD_BATTERY_EVENT_RECOVERED, "recovered"},
      {BD_BATTERY_EVENT_PLUGGED, "plugged"},
      {BD_BATTERY_EVENT_UNPLUGGED, "unplugged"},
      {BD_BATTERY_EVENT_UNKNOWN, "unknown"},
  };
  if (events == 0) {
    printf("none");
    return;
  }
  bool first = true;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    if ((events & names[i].bit) == 0) {
      continue;
    }
    if (!first) {
      printf(",");
    }
    printf("%s", names[i].name);
    first = false;
  }
}

static void print_usage(FILE *stream) {
  fprintf(stream,
          "usage: bd-battery [options]\n"
          "\n"
          "Read-only power_supply probe.  It never writes sysfs, PMIC or the\n"
          "power state machine.  Units: capacity %%, voltage uV, current uA,\n"
          "temperature tenths of Celsius, time seconds.\n"
          "\n"
          "  --root DIR        sysfs root (default /; use a fixture for tests)\n"
          "  --battery NAME    exact power_supply name (default: discover)\n"
          "  --no-policy       skip the low-battery policy diagnostic\n"
          "  -h, --help        show this help\n");
}

static void print_status(const bd_battery *battery,
                         const bd_battery_status *status) {
  printf("battery.available=%s\n", status->battery_available ? "yes" : "no");
  if (status->battery_name[0] != '\0') {
    printf("battery.name=%s\n", status->battery_name);
  }
  if (bd_battery_selected_path(battery) != NULL) {
    printf("battery.path=%s\n", bd_battery_selected_path(battery));
  }
  printf("battery.present=%s\n",
         yes_no_unknown(status->present_known, status->present));
  if (status->capacity_known) {
    printf("battery.capacity=%d\n", status->capacity_percent);
  } else {
    printf("battery.capacity=unknown\n");
  }
  if (status->status_known) {
    printf("battery.status=%s\n",
           bd_battery_charge_status_name(status->status));
  } else {
    printf("battery.status=unknown\n");
  }
  printf("battery.external_power=%s\n",
         yes_no_unknown(status->external_power_known,
                        status->external_power));
  if (status->voltage_known) {
    printf("battery.voltage_uv=%" PRId64 "\n", status->voltage_uv);
  } else {
    printf("battery.voltage_uv=unknown\n");
  }
  if (status->current_known) {
    printf("battery.current_ua=%" PRId64 "\n", status->current_ua);
  } else {
    printf("battery.current_ua=unknown\n");
  }
  if (status->temp_known) {
    printf("battery.temp_deci_c=%" PRId32 "\n", status->temp_deci_c);
  } else {
    printf("battery.temp_deci_c=unknown\n");
  }
  if (status->time_to_empty_known) {
    printf("battery.time_to_empty_s=%" PRId64 "\n",
           status->time_to_empty_s);
  } else {
    printf("battery.time_to_empty_s=unknown\n");
  }
  if (status->time_to_full_known) {
    printf("battery.time_to_full_s=%" PRId64 "\n", status->time_to_full_s);
  } else {
    printf("battery.time_to_full_s=unknown\n");
  }
  if (status->health_known) {
    printf("battery.health=%s\n", bd_battery_health_name(status->health));
  } else {
    printf("battery.health=unknown\n");
  }
  printf("battery.read_errors=%u\n", status->read_errors);
  printf("supply.count=%u\n", status->supply_count);
  printf("supply.truncated=%s\n", status->supplies_truncated ? "yes" : "no");
  for (unsigned i = 0; i < status->supply_count &&
                       i < BD_BATTERY_MAX_SUPPLIES;
       ++i) {
    const bd_battery_supply *supply = &status->supplies[i];
    printf("supply.%u.name=%s\n", i, supply->name);
    printf("supply.%u.selected=%s\n", i, supply->selected ? "yes" : "no");
    printf("supply.%u.type=%s\n", i,
           bd_battery_supply_type_name(supply->type));
    if (supply->type_text[0] != '\0') {
      printf("supply.%u.type_text=%s\n", i, supply->type_text);
    }
    printf("supply.%u.present=%s\n", i,
           yes_no_unknown(supply->present_known, supply->present));
    printf("supply.%u.online=%s\n", i,
           yes_no_unknown(supply->online_known, supply->online));
    if (supply->status_known) {
      printf("supply.%u.status=%s\n", i,
             bd_battery_charge_status_name(supply->status));
    } else {
      printf("supply.%u.status=unknown\n", i);
    }
  }
}

static void print_policy(const bd_battery_status *status) {
  bd_battery_policy_config config;
  bd_battery_policy_config_default(&config);
  bd_battery_policy policy;
  bd_battery_policy_output output;
  bd_battery_error error;
  bd_battery_policy_result result =
      bd_battery_policy_init(&policy, &config, 0, &output, &error);
  if (result != BD_BATTERY_POLICY_OK) {
    printf("policy.enabled=no\n");
    printf("policy.error=%s\n", error.message);
    return;
  }
  result = bd_battery_policy_update(&policy, status, 0, &output);
  printf("policy.enabled=yes\n");
  printf("policy.reminder_percent=%d\n", config.reminder_percent);
  printf("policy.critical_percent=%d\n", config.critical_percent);
  printf("policy.level=%s\n",
         bd_battery_policy_level_name(output.level));
  printf("policy.capacity_known=%s\n",
         output.capacity_known ? "yes" : "no");
  printf("policy.plugged_in=%s\n",
         yes_no_unknown(output.plugged_in_known, output.plugged_in));
  printf("policy.suppressed=%s\n", output.suppressed ? "yes" : "no");
  printf("policy.events=");
  print_events(output.events);
  printf("\n");
  printf("policy.recommendation=%s\n",
         bd_battery_recommendation_name(output.recommendation));
  printf("policy.next_sample_ms=%" PRIu64 "\n", output.next_sample_ms);
  printf("policy.next_sample_after_ms=%" PRIu32 "\n",
         output.next_sample_after_ms);
  if (result != BD_BATTERY_POLICY_OK) {
    printf("policy.error=%s\n", bd_battery_policy_result_name(result));
  }
}

int main(int argc, char **argv) {
  const char *root = NULL;
  const char *battery_name = NULL;
  bool policy = true;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (strcmp(arg, "--root") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "bd-battery: --root needs a directory\n");
        return 2;
      }
      root = argv[++i];
      continue;
    }
    if (strcmp(arg, "--battery") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "bd-battery: --battery needs a name\n");
        return 2;
      }
      battery_name = argv[++i];
      continue;
    }
    if (strcmp(arg, "--no-policy") == 0) {
      policy = false;
      continue;
    }
    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      print_usage(stdout);
      return 0;
    }
    fprintf(stderr, "bd-battery: unknown option: %s\n", arg);
    print_usage(stderr);
    return 2;
  }

  bd_battery_config config = {.sysfs_root = root, .battery_name = battery_name};
  bd_battery battery;
  bd_battery_error error;
  bd_battery_status status;
  memset(&status, 0, sizeof(status));
  printf("probe.read_only=yes\n");
  bd_battery_result open_result = bd_battery_open(&battery, &config, &error);
  if (open_result != BD_BATTERY_OK) {
    printf("probe.open=%s\n", bd_battery_result_name(open_result));
    if (error.message[0] != '\0') {
      printf("probe.open_message=%s\n", error.message);
    }
  } else {
    printf("probe.open=ok\n");
  }
  if (open_result == BD_BATTERY_ERR_ARGUMENT ||
      open_result == BD_BATTERY_ERR_IO) {
    print_status(&battery, &status);
    printf("probe.read=not-attempted\n");
    return 1;
  }

  bd_battery_result read_result = bd_battery_read(&battery, &status, &error);
  print_status(&battery, &status);
  if (read_result != BD_BATTERY_OK) {
    printf("probe.read=%s\n", bd_battery_result_name(read_result));
    if (error.message[0] != '\0') {
      printf("probe.read_message=%s\n", error.message);
    }
    return 1;
  }
  if (policy) {
    print_policy(&status);
  }
  return 0;
}
