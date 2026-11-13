#define _POSIX_C_SOURCE 200809L

#include "brickdesk/device_battery.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef BRICKDESK_BATTERY_FIXTURE_DIR
#define BRICKDESK_BATTERY_FIXTURE_DIR "tests/fixtures/battery"
#endif

#define FIXTURE(name) BRICKDESK_BATTERY_FIXTURE_DIR "/" name

static int g_failures;

#define CHECK(condition, ...)                         \
  do {                                                \
    if (!(condition)) {                               \
      fprintf(stderr, "device_battery_test:%d: ", __LINE__); \
      fprintf(stderr, __VA_ARGS__);                   \
      fprintf(stderr, "\n");                          \
      ++g_failures;                                   \
    }                                                 \
  } while (0)

static void check_result(bd_battery_result actual, bd_battery_result expected,
                         const char *expression, int line) {
  if (actual != expected) {
    fprintf(stderr,
            "device_battery_test:%d: %s returned %s, expected %s (%s)\n", line,
            expression, bd_battery_result_name(actual),
            bd_battery_result_name(expected),
            actual == BD_BATTERY_ERR_ARGUMENT ? "argument" : "");
    ++g_failures;
  }
}

static void check_policy_result(bd_battery_policy_result actual,
                                bd_battery_policy_result expected,
                                const char *expression, int line) {
  if (actual != expected) {
    fprintf(stderr,
            "device_battery_test:%d: %s returned policy %s, expected %s\n",
            line, expression, bd_battery_policy_result_name(actual),
            bd_battery_policy_result_name(expected));
    ++g_failures;
  }
}

#define CHECK_RESULT(expression, expected) \
  check_result((expression), (expected), #expression, __LINE__)

#define CHECK_POLICY_RESULT(expression, expected) \
  check_policy_result((expression), (expected), #expression, __LINE__)

static char *make_temp_dir(void) {
  char template[] = "/tmp/bd_device_battery.XXXXXX";
  char *directory = mkdtemp(template);
  if (directory == NULL) {
    fprintf(stderr, "device_battery_test: mkdtemp: %s\n", strerror(errno));
    exit(1);
  }
  char *copy = strdup(directory);
  if (copy == NULL) {
    exit(1);
  }
  return copy;
}

static void remove_tree(char *directory) {
  if (directory == NULL) {
    return;
  }
  char command[512];
  int written = snprintf(command, sizeof(command), "rm -rf '%s'", directory);
  if (written > 0 && (size_t)written < sizeof(command)) {
    int result = system(command);
    (void)result;
  }
  free(directory);
}

static bool mkdir_p(const char *path) {
  char buffer[512];
  int written = snprintf(buffer, sizeof(buffer), "%s", path);
  if (written <= 0 || (size_t)written >= sizeof(buffer)) {
    return false;
  }
  for (char *cursor = buffer + 1; *cursor != '\0'; ++cursor) {
    if (*cursor == '/') {
      *cursor = '\0';
      if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
        return false;
      }
      *cursor = '/';
    }
  }
  return mkdir(buffer, 0755) == 0 || errno == EEXIST;
}

static bool write_file(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs(text, file) >= 0;
  if (fclose(file) != 0) {
    ok = false;
  }
  return ok;
}

static bool write_under(const char *root, const char *relative,
                        const char *text) {
  char path[512];
  int written = snprintf(path, sizeof(path), "%s/%s", root, relative);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    return false;
  }
  char parent[512];
  snprintf(parent, sizeof(parent), "%s", path);
  char *slash = strrchr(parent, '/');
  if (slash == NULL) {
    return false;
  }
  *slash = '\0';
  if (!mkdir_p(parent)) {
    return false;
  }
  return write_file(path, text);
}

static bool set_supply(const char *root, const char *name, const char *attr,
                       const char *text) {
  char relative[512];
  int written = snprintf(relative, sizeof(relative),
                         "sys/class/power_supply/%s/%s", name, attr);
  if (written <= 0 || (size_t)written >= sizeof(relative)) {
    return false;
  }
  return write_under(root, relative, text);
}

static void remove_supply(char *root, const char *name) {
  char command[512];
  int written = snprintf(command, sizeof(command),
                         "rm -rf '%s/sys/class/power_supply/%s'", root, name);
  if (written > 0 && (size_t)written < sizeof(command)) {
    int result = system(command);
    (void)result;
  }
}

static char *make_tree(void) {
  char *root = make_temp_dir();
  if (!set_supply(root, "axp2202-battery", "type", "Battery\n") ||
      !set_supply(root, "axp2202-battery", "present", "1\n") ||
      !set_supply(root, "axp2202-usb", "type", "USB\n") ||
      !set_supply(root, "axp2202-usb", "online", "0\n")) {
    fprintf(stderr, "device_battery_test: cannot create temp tree\n");
    exit(1);
  }
  return root;
}

static bd_battery_supply *find_supply(bd_battery_status *status,
                                      const char *name) {
  for (unsigned i = 0; i < status->supply_count &&
                       i < BD_BATTERY_MAX_SUPPLIES;
       ++i) {
    if (strcmp(status->supplies[i].name, name) == 0) {
      return &status->supplies[i];
    }
  }
  return NULL;
}

static void test_full_fixture(void) {
  bd_battery battery;
  bd_battery_error error;
  bd_battery_status status;
  bd_battery_config config = {.sysfs_root = FIXTURE("full")};

  CHECK_RESULT(bd_battery_open(&battery, &config, &error), BD_BATTERY_OK);
  CHECK(bd_battery_selected(&battery), "full fixture must select a battery");
  CHECK(strcmp(bd_battery_selected_name(&battery), "axp2202-battery") == 0,
        "selected name is %s", bd_battery_selected_name(&battery));
  CHECK(bd_battery_selected_path(&battery) != NULL,
        "selected path is missing");

  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(status.battery_available, "battery must be available");
  CHECK(strcmp(status.battery_name, "axp2202-battery") == 0,
        "status battery name is %s", status.battery_name);
  CHECK(status.present_known && status.present, "present must be known true");
  CHECK(status.capacity_known && status.capacity_percent == 73,
        "capacity %d known=%d", status.capacity_percent,
        status.capacity_known ? 1 : 0);
  CHECK(status.status_known &&
            status.status == BD_BATTERY_CHARGE_DISCHARGING,
        "status %s", bd_battery_charge_status_name(status.status));
  CHECK(status.external_power_known && status.external_power,
        "external power must be known true");
  CHECK(status.voltage_known && status.voltage_uv == 3950000,
        "voltage %lld", (long long)status.voltage_uv);
  CHECK(status.current_known && status.current_ua == -420000,
        "current %lld", (long long)status.current_ua);
  CHECK(status.temp_known && status.temp_deci_c == 305, "temp %d",
        status.temp_deci_c);
  CHECK(status.time_to_empty_known && status.time_to_empty_s == 7200,
        "time to empty %lld", (long long)status.time_to_empty_s);
  CHECK(!status.time_to_full_known, "time to full must be unknown");
  CHECK(status.health_known && status.health == BD_BATTERY_HEALTH_GOOD,
        "health %s", bd_battery_health_name(status.health));
  CHECK(status.read_errors == 0, "read errors %u", status.read_errors);
  CHECK(status.supply_count == 4, "supply count %u", status.supply_count);
  CHECK(!status.supplies_truncated, "supplies must not be truncated");

  bd_battery_supply *battery_supply =
      find_supply(&status, "axp2202-battery");
  bd_battery_supply *usb = find_supply(&status, "axp2202-usb");
  bd_battery_supply *ac = find_supply(&status, "axp2202-ac");
  bd_battery_supply *spare = find_supply(&status, "spare-battery");
  CHECK(battery_supply != NULL && battery_supply->selected,
        "selected flag missing");
  CHECK(battery_supply != NULL &&
            battery_supply->type == BD_BATTERY_SUPPLY_BATTERY,
        "battery type");
  CHECK(usb != NULL && usb->online_known && usb->online,
        "usb online must be known true");
  CHECK(ac != NULL && ac->online_known && !ac->online,
        "ac online must be known false");
  CHECK(spare != NULL && spare->present_known && !spare->present,
        "spare battery must be known absent");
}

static void test_limited_fixture(void) {
  bd_battery battery;
  bd_battery_error error;
  bd_battery_status status;
  bd_battery_config config = {.sysfs_root = FIXTURE("limited")};

  CHECK_RESULT(bd_battery_open(&battery, &config, &error), BD_BATTERY_OK);
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(status.battery_available, "battery must be available");
  CHECK(status.capacity_known && status.capacity_percent == 42,
        "capacity %d", status.capacity_percent);
  CHECK(!status.present_known, "present must be unknown");
  CHECK(!status.status_known, "status must be unknown");
  CHECK(!status.external_power_known, "external power must be unknown");
  CHECK(!status.voltage_known, "voltage must be unknown");
  CHECK(!status.current_known, "current must be unknown");
  CHECK(!status.temp_known, "temperature must be unknown");
  CHECK(!status.time_to_empty_known, "time to empty must be unknown");
  CHECK(!status.time_to_full_known, "time to full must be unknown");
  CHECK(!status.health_known, "health must be unknown");
  CHECK(status.read_errors == 0, "missing optional fields are not errors");
}

static void test_no_battery_and_late_acquire(void) {
  char *root = make_temp_dir();
  if (!set_supply(root, "usb0", "type", "USB\n") ||
      !set_supply(root, "usb0", "online", "0\n")) {
    exit(1);
  }
  bd_battery battery;
  bd_battery_error error;
  bd_battery_status status;
  bd_battery_config config = {.sysfs_root = root};

  CHECK_RESULT(bd_battery_open(&battery, &config, &error),
               BD_BATTERY_ERR_UNSUPPORTED);
  CHECK(!bd_battery_selected(&battery), "nothing must be selected");
  CHECK_RESULT(bd_battery_read(&battery, &status, &error),
               BD_BATTERY_ERR_UNSUPPORTED);
  CHECK(!status.battery_available, "no battery must not be available");
  CHECK(!status.capacity_known, "missing battery must not be 0%%");
  CHECK(status.external_power_known && !status.external_power,
        "usb0 online=0 must be known false");

  if (!set_supply(root, "axp2202-battery", "type", "Battery\n") ||
      !set_supply(root, "axp2202-battery", "present", "1\n") ||
      !set_supply(root, "axp2202-battery", "capacity", "64\n")) {
    exit(1);
  }
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(status.battery_available && status.capacity_known &&
            status.capacity_percent == 64,
        "late battery must be discovered");

  remove_supply(root, "axp2202-battery");
  CHECK_RESULT(bd_battery_read(&battery, &status, &error),
               BD_BATTERY_ERR_UNSUPPORTED);
  CHECK(!status.capacity_known, "removed late battery must be unknown");
  remove_tree(root);
}

static void test_selection(void) {
  char *root = make_temp_dir();
  if (!set_supply(root, "a-battery", "type", "Battery\n") ||
      !set_supply(root, "a-battery", "present", "0\n") ||
      !set_supply(root, "a-battery", "capacity", "10\n") ||
      !set_supply(root, "axp2202-battery", "type", "Battery\n") ||
      !set_supply(root, "axp2202-battery", "present", "1\n") ||
      !set_supply(root, "axp2202-battery", "capacity", "20\n") ||
      !set_supply(root, "z-battery", "type", "Battery\n") ||
      !set_supply(root, "z-battery", "present", "1\n") ||
      !set_supply(root, "z-battery", "capacity", "30\n") ||
      !set_supply(root, "axp2202-usb", "type", "USB\n") ||
      !set_supply(root, "axp2202-usb", "online", "0\n")) {
    exit(1);
  }

  bd_battery battery;
  bd_battery_error error;
  bd_battery_status status;
  bd_battery_config config = {.sysfs_root = root};
  CHECK_RESULT(bd_battery_open(&battery, &config, &error), BD_BATTERY_OK);
  CHECK(strcmp(bd_battery_selected_name(&battery), "axp2202-battery") == 0,
        "present axp2202-battery must win over absent a-battery");

  bd_battery_config explicit_config = {.sysfs_root = root,
                                       .battery_name = "z-battery"};
  CHECK_RESULT(bd_battery_open(&battery, &explicit_config, &error),
               BD_BATTERY_OK);
  CHECK(strcmp(bd_battery_selected_name(&battery), "z-battery") == 0,
        "explicit selection");
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(status.capacity_known && status.capacity_percent == 30,
        "explicit capacity");

  bd_battery_config usb_config = {.sysfs_root = root,
                                  .battery_name = "axp2202-usb"};
  CHECK_RESULT(bd_battery_open(&battery, &usb_config, &error),
               BD_BATTERY_ERR_UNSUPPORTED);
  CHECK(!bd_battery_selected(&battery), "USB must not become a battery");

  bd_battery_config missing_config = {.sysfs_root = root,
                                      .battery_name = "missing"};
  CHECK_RESULT(bd_battery_open(&battery, &missing_config, &error),
               BD_BATTERY_ERR_UNSUPPORTED);

  if (!set_supply(root, "axp2202-battery", "present", "0\n")) {
    exit(1);
  }
  CHECK_RESULT(bd_battery_open(&battery, &config, &error), BD_BATTERY_OK);
  CHECK(strcmp(bd_battery_selected_name(&battery), "z-battery") == 0,
        "present z-battery must win over absent axp2202-battery");
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(status.present_known && status.present && status.capacity_known &&
            status.capacity_percent == 30,
        "z-battery must be usable");

  if (!set_supply(root, "z-battery", "present", "0\n")) {
    exit(1);
  }
  CHECK_RESULT(bd_battery_open(&battery, &config, &error), BD_BATTERY_OK);
  CHECK(bd_battery_selected_name(&battery) != NULL,
        "all-absent batteries still select one for diagnostics");
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(status.present_known && !status.present,
        "all-absent selection must report present=no");
  CHECK(!status.capacity_known, "absent battery capacity must be unknown");

  remove_tree(root);
}

static void test_present_false(void) {
  char *root = make_tree();
  if (!set_supply(root, "axp2202-battery", "present", "0\n") ||
      !set_supply(root, "axp2202-battery", "capacity", "0\n") ||
      !set_supply(root, "axp2202-battery", "status", "Full\n") ||
      !set_supply(root, "axp2202-battery", "voltage_now", "0\n") ||
      !set_supply(root, "axp2202-battery", "current_now", "0\n") ||
      !set_supply(root, "axp2202-battery", "temp", "0\n") ||
      !set_supply(root, "axp2202-battery", "health", "Good\n") ||
      !set_supply(root, "axp2202-usb", "online", "1\n")) {
    exit(1);
  }
  bd_battery battery;
  bd_battery_error error;
  bd_battery_status status;
  bd_battery_config config = {.sysfs_root = root};
  CHECK_RESULT(bd_battery_open(&battery, &config, &error), BD_BATTERY_OK);
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(status.present_known && !status.present, "present must be false");
  CHECK(!status.capacity_known, "detached battery must not report 0%%");
  CHECK(!status.status_known, "detached battery status must be unknown");
  CHECK(!status.voltage_known, "detached voltage must be unknown");
  CHECK(!status.current_known, "detached current must be unknown");
  CHECK(!status.temp_known, "detached temperature must be unknown");
  CHECK(!status.health_known, "detached health must be unknown");
  CHECK(status.external_power_known && status.external_power,
        "external power must still be read");
  CHECK(status.read_errors == 0, "present=false is not a read error");
  remove_tree(root);
}

static void test_bad_values(void) {
  char *root = make_tree();
  if (!set_supply(root, "axp2202-battery", "present", "2\n") ||
      !set_supply(root, "axp2202-battery", "capacity", "not-a-number\n") ||
      !set_supply(root, "axp2202-battery", "status", "Bogus\n") ||
      !set_supply(root, "axp2202-battery", "voltage_now", "-1\n") ||
      !set_supply(root, "axp2202-battery", "current_now", "nope\n") ||
      !set_supply(root, "axp2202-battery", "temp", "nope\n") ||
      !set_supply(root, "axp2202-battery", "time_to_empty_now", "-5\n") ||
      !set_supply(root, "axp2202-battery", "time_to_full_now", "nope\n") ||
      !set_supply(root, "axp2202-battery", "health", "Mystery\n")) {
    exit(1);
  }
  bd_battery battery;
  bd_battery_error error;
  bd_battery_status status;
  bd_battery_config config = {.sysfs_root = root};
  CHECK_RESULT(bd_battery_open(&battery, &config, &error), BD_BATTERY_OK);
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(!status.present_known, "invalid present must be unknown");
  CHECK(!status.capacity_known, "invalid capacity must be unknown");
  CHECK(!status.status_known, "invalid status must be unknown");
  CHECK(!status.voltage_known, "negative voltage must be unknown");
  CHECK(!status.current_known, "invalid current must be unknown");
  CHECK(!status.temp_known, "invalid temperature must be unknown");
  CHECK(!status.time_to_empty_known, "negative time must be unknown");
  CHECK(!status.time_to_full_known, "invalid time must be unknown");
  CHECK(status.health_known && status.health == BD_BATTERY_HEALTH_OTHER,
        "unrecognised health is known-other");
  CHECK(status.read_errors >= 8, "read errors %u", status.read_errors);
  remove_tree(root);
}

static void test_capacity_boundaries(void) {
  char *root = make_tree();
  bd_battery battery;
  bd_battery_error error;
  bd_battery_status status;
  bd_battery_config config = {.sysfs_root = root};

  const char *values[] = {" 73 \n", "0\n", "100\n", "101\n", "-1\n", ""};
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    if (!set_supply(root, "axp2202-battery", "capacity", values[i])) {
      exit(1);
    }
    CHECK_RESULT(bd_battery_open(&battery, &config, &error), BD_BATTERY_OK);
    CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
    if (i == 0) {
      CHECK(status.capacity_known && status.capacity_percent == 73,
            "trimmed 73 must be known");
    } else if (i == 1) {
      CHECK(status.capacity_known && status.capacity_percent == 0,
            "0 is a valid known value");
    } else if (i == 2) {
      CHECK(status.capacity_known && status.capacity_percent == 100,
            "100 is a valid known value");
    } else {
      CHECK(!status.capacity_known, "capacity value %zu must be unknown", i);
    }
  }
  remove_tree(root);
}

static void test_external_power_aggregation(void) {
  char *root = make_tree();
  bd_battery battery;
  bd_battery_error error;
  bd_battery_status status;
  bd_battery_config config = {.sysfs_root = root};

  CHECK_RESULT(bd_battery_open(&battery, &config, &error), BD_BATTERY_OK);
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(status.external_power_known && !status.external_power,
        "one USB online=0 is known false");

  if (!set_supply(root, "axp2202-ac", "type", "Mains\n") ||
      !set_supply(root, "axp2202-ac", "online", "0\n")) {
    exit(1);
  }
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(status.external_power_known && !status.external_power,
        "two known-off externals are known false");

  if (!set_supply(root, "axp2202-usb", "online", "1\n")) {
    exit(1);
  }
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(status.external_power_known && status.external_power,
        "one online external is known true");

  if (!set_supply(root, "axp2202-ac", "online", "bad\n")) {
    exit(1);
  }
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(status.external_power_known && status.external_power,
        "one online external wins over an unknown one");

  if (!set_supply(root, "axp2202-usb", "online", "0\n")) {
    exit(1);
  }
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(!status.external_power_known,
        "known-off plus unknown external must stay unknown");

  if (!set_supply(root, "axp2202-ac", "online", "0\n") ||
      !set_supply(root, "axp2202-usb", "online", "bad\n")) {
    exit(1);
  }
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(!status.external_power_known,
        "bad online must not become known false");

  remove_supply(root, "axp2202-ac");
  remove_supply(root, "axp2202-usb");
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(!status.external_power_known,
        "no external node means external power is unknown");
  CHECK(!status.external_power, "unknown external must not be true");

  if (!set_supply(root, "usb-present", "type", "USB\n") ||
      !set_supply(root, "usb-present", "present", "1\n")) {
    exit(1);
  }
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(!status.external_power_known,
        "present must not be used as online");
  remove_tree(root);
}

static void test_stale_and_type_change(void) {
  char *root = make_tree();
  if (!set_supply(root, "axp2202-battery", "capacity", "55\n")) {
    exit(1);
  }
  bd_battery battery;
  bd_battery_error error;
  bd_battery_status status;
  bd_battery_config config = {.sysfs_root = root};
  CHECK_RESULT(bd_battery_open(&battery, &config, &error), BD_BATTERY_OK);
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(status.capacity_percent == 55, "initial capacity");

  remove_supply(root, "axp2202-battery");
  CHECK_RESULT(bd_battery_read(&battery, &status, &error),
               BD_BATTERY_ERR_STALE);
  CHECK(!status.battery_available, "stale battery must not be available");
  CHECK(!status.capacity_known, "stale battery must not report capacity");
  CHECK(!status.present_known, "stale battery must not report presence");
  CHECK(status.external_power_known && !status.external_power,
        "stale battery must still leave external power readable");

  if (!set_supply(root, "axp2202-battery", "type", "USB\n") ||
      !set_supply(root, "axp2202-battery", "present", "1\n") ||
      !set_supply(root, "axp2202-battery", "online", "1\n")) {
    exit(1);
  }
  CHECK_RESULT(bd_battery_read(&battery, &status, &error),
               BD_BATTERY_ERR_STALE);
  CHECK(!status.capacity_known, "type change must not keep capacity");

  char command[512];
  int written = snprintf(command, sizeof(command),
                         "rm -rf '%s/sys/class/power_supply'", root);
  if (written > 0 && (size_t)written < sizeof(command)) {
    int result = system(command);
    (void)result;
  }
  CHECK_RESULT(bd_battery_read(&battery, &status, &error),
               BD_BATTERY_ERR_UNSUPPORTED);
  CHECK(!status.battery_available, "missing class is unsupported");
  remove_tree(root);
}

static void test_supply_table_truncation(void) {
  char *root = make_temp_dir();
  if (!set_supply(root, "z-battery", "type", "Battery\n") ||
      !set_supply(root, "z-battery", "present", "1\n") ||
      !set_supply(root, "z-battery", "capacity", "33\n")) {
    exit(1);
  }
  char name[32];
  for (int i = 0; i < 20; ++i) {
    snprintf(name, sizeof(name), "usb%02d", i);
    if (!set_supply(root, name, "type", "USB\n") ||
        !set_supply(root, name, "online", "0\n")) {
      exit(1);
    }
  }
  bd_battery battery;
  bd_battery_error error;
  bd_battery_status status;
  bd_battery_config config = {.sysfs_root = root};
  CHECK_RESULT(bd_battery_open(&battery, &config, &error), BD_BATTERY_OK);
  CHECK(strcmp(bd_battery_selected_name(&battery), "z-battery") == 0,
        "selected battery");
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(status.supply_count == 21, "supply count %u", status.supply_count);
  CHECK(status.supplies_truncated, "table must be truncated");
  CHECK(status.battery_available && status.capacity_known &&
            status.capacity_percent == 33,
        "selected battery beyond the table must still be read");
  remove_tree(root);
}

static void test_fixture_none(void) {
  bd_battery battery;
  bd_battery_error error;
  bd_battery_status status;
  bd_battery_config config = {.sysfs_root = FIXTURE("none")};
  CHECK_RESULT(bd_battery_open(&battery, &config, &error),
               BD_BATTERY_ERR_UNSUPPORTED);
  CHECK_RESULT(bd_battery_read(&battery, &status, &error),
               BD_BATTERY_ERR_UNSUPPORTED);
  CHECK(!status.capacity_known, "none fixture must not be 0%%");
  CHECK(status.external_power_known && !status.external_power,
        "usb0 online=0");
}

static bd_battery_status normal_status(int capacity) {
  bd_battery_status status;
  memset(&status, 0, sizeof(status));
  status.battery_available = true;
  status.present_known = true;
  status.present = true;
  status.capacity_known = true;
  status.capacity_percent = capacity;
  status.status_known = true;
  status.status = BD_BATTERY_CHARGE_DISCHARGING;
  status.external_power_known = true;
  status.external_power = false;
  return status;
}

static bd_battery_status charging_status(int capacity) {
  bd_battery_status status = normal_status(capacity);
  status.status = BD_BATTERY_CHARGE_CHARGING;
  status.external_power_known = true;
  status.external_power = true;
  return status;
}

static bd_battery_status unknown_status(void) {
  bd_battery_status status;
  memset(&status, 0, sizeof(status));
  return status;
}

static bd_battery_policy init_policy(bd_battery_policy_config *config) {
  bd_battery_policy policy;
  bd_battery_policy_output output;
  bd_battery_error error;
  bd_battery_policy_config defaults;
  bd_battery_policy_config_default(&defaults);
  if (config != NULL) {
    defaults = *config;
  }
  CHECK_POLICY_RESULT(bd_battery_policy_init(&policy, &defaults, 1000, &output,
                                      &error),
               BD_BATTERY_POLICY_OK);
  CHECK(output.next_sample_ms == 1000, "init must sample immediately");
  CHECK(output.level == BD_BATTERY_LEVEL_UNKNOWN, "init level");
  return policy;
}

static void test_external_power_beyond_public_table(void) {
  char *root = make_temp_dir();
  if (!set_supply(root, "axp2202-battery", "type", "Battery\n") ||
      !set_supply(root, "axp2202-battery", "present", "1\n") ||
      !set_supply(root, "axp2202-battery", "capacity", "3\n") ||
      !set_supply(root, "axp2202-battery", "status", "Discharging\n")) {
    exit(1);
  }
  char name[32];
  for (int i = 0; i < 20; ++i) {
    snprintf(name, sizeof(name), "usb%02d", i);
    if (!set_supply(root, name, "type", "USB\n") ||
        !set_supply(root, name, "online", "0\n")) {
      exit(1);
    }
  }

  bd_battery battery;
  bd_battery_error error;
  bd_battery_status status;
  bd_battery_config config = {.sysfs_root = root};
  CHECK_RESULT(bd_battery_open(&battery, &config, &error), BD_BATTERY_OK);

  /* The only online supply is the last sorted node, beyond supplies[16]. */
  if (!set_supply(root, "usb19", "online", "1\n")) {
    exit(1);
  }
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(status.supply_count == 21, "supply count %u", status.supply_count);
  CHECK(status.supplies_truncated, "table must be truncated");
  CHECK(status.external_power_known && status.external_power,
        "online node beyond the public table must be aggregated");
  CHECK(status.capacity_known && status.capacity_percent == 3,
        "battery capacity");

  bd_battery_policy_output output;
  bd_battery_policy_config policy_config;
  bd_battery_policy_config_default(&policy_config);
  bd_battery_policy policy = init_policy(&policy_config);
  bd_battery_policy_update(&policy, &status, 1000, &output);
  bd_battery_policy_update(&policy, &status, 6000, &output);
  CHECK(output.level == BD_BATTERY_LEVEL_CRITICAL, "critical after 3%%");
  CHECK(output.suppressed, "online beyond table must suppress");
  CHECK(!(output.events & BD_BATTERY_EVENT_CRITICAL),
        "plugged critical must not alert");
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_NONE,
        "plugged critical must not recommend shutdown");

  /* An unknown online beyond the table must make the aggregate unknown. */
  if (!set_supply(root, "usb19", "online", "bad\n")) {
    exit(1);
  }
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(!status.external_power_known,
        "unknown online beyond the table must not become known false");
  CHECK(!status.external_power, "unknown external must not be true");
  CHECK(status.read_errors >= 1, "bad online beyond table must be read");

  policy = init_policy(&policy_config);
  bd_battery_policy_update(&policy, &status, 1000, &output);
  bd_battery_policy_update(&policy, &status, 6000, &output);
  CHECK(output.level == BD_BATTERY_LEVEL_CRITICAL, "critical with unknown ext");
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_SHOW_CRITICAL,
        "unknown external must not prepare shutdown");

  /* An unreadable type must not hide a potentially connected supply. */
  if (!set_supply(root, "usb19", "type", "\n") ||
      !set_supply(root, "usb19", "online", "1\n")) {
    exit(1);
  }
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(!status.external_power_known,
        "unclassified supply must not become known unplugged");
  policy = init_policy(&policy_config);
  bd_battery_policy_update(&policy, &status, 1000, &output);
  bd_battery_policy_update(&policy, &status, 6000, &output);
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_SHOW_CRITICAL,
        "unclassified supply must not prepare shutdown");

  /* All external nodes known offline: only now is shutdown advice allowed. */
  if (!set_supply(root, "usb19", "type", "USB\n") ||
      !set_supply(root, "usb19", "online", "0\n")) {
    exit(1);
  }
  CHECK_RESULT(bd_battery_read(&battery, &status, &error), BD_BATTERY_OK);
  CHECK(status.external_power_known && !status.external_power,
        "all nodes offline must be known false");

  policy = init_policy(&policy_config);
  bd_battery_policy_update(&policy, &status, 1000, &output);
  bd_battery_policy_update(&policy, &status, 6000, &output);
  CHECK(output.level == BD_BATTERY_LEVEL_CRITICAL, "critical when offline");
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_PREPARE_SHUTDOWN,
        "known offline critical may prepare shutdown");

  remove_tree(root);
}

static void test_policy_validation(void) {
  bd_battery_policy_config config;
  bd_battery_policy_config_default(&config);
  char error[BD_BATTERY_MESSAGE_MAX];
  CHECK(bd_battery_policy_config_validate(&config, error, sizeof(error)),
        "defaults must validate: %s", error);

  config.reminder_percent = 0;
  CHECK(!bd_battery_policy_config_validate(&config, error, sizeof(error)) &&
            error[0] != '\0',
        "reminder=0 must fail");
  bd_battery_policy_config_default(&config);
  config.critical_percent = config.reminder_percent;
  CHECK(!bd_battery_policy_config_validate(&config, error, sizeof(error)),
        "critical must be below reminder");
  bd_battery_policy_config_default(&config);
  config.reminder_hysteresis_percent = 90;
  CHECK(!bd_battery_policy_config_validate(&config, error, sizeof(error)),
        "threshold plus hysteresis over 100 must fail");
  bd_battery_policy_config_default(&config);
  config.confirm_samples = 0;
  CHECK(!bd_battery_policy_config_validate(&config, error, sizeof(error)),
        "confirm=0 must fail");
  bd_battery_policy_config_default(&config);
  config.sample_normal_ms = 0;
  CHECK(!bd_battery_policy_config_validate(&config, error, sizeof(error)),
        "zero sample interval must fail");
  bd_battery_policy_config_default(&config);
  config.unknown_initial_ms = 5000;
  config.unknown_max_ms = 1000;
  CHECK(!bd_battery_policy_config_validate(&config, error, sizeof(error)),
        "unknown initial over max must fail");
  bd_battery_policy_config_default(&config);
  config.enabled = false;
  config.reminder_percent = -5;
  CHECK(bd_battery_policy_config_validate(&config, error, sizeof(error)),
        "disabled config is accepted");

  bd_battery_policy policy;
  bd_battery_policy_output output;
  bd_battery_error init_error;
  bd_battery_policy_config_default(&config);
  config.critical_percent = -1;
  CHECK_POLICY_RESULT(bd_battery_policy_init(&policy, &config, 10, &output,
                                      &init_error),
               BD_BATTERY_POLICY_INVALID);
  CHECK(init_error.code == BD_BATTERY_ERR_ARGUMENT, "init error code");
}

static void test_policy_levels_and_hysteresis(void) {
  bd_battery_policy_config config;
  bd_battery_policy_config_default(&config);
  bd_battery_policy policy = init_policy(&config);
  bd_battery_policy_output output;
  bd_battery_status status = normal_status(50);
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 1000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.level == BD_BATTERY_LEVEL_NORMAL, "50%% is normal");
  CHECK(output.events == 0, "normal has no event");
  CHECK(output.next_sample_after_ms == 60000, "normal deadline");
  CHECK(output.next_sample_ms == 61000, "absolute normal deadline");

  status = normal_status(20);
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 61000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.level == BD_BATTERY_LEVEL_NORMAL, "first low sample waits");
  CHECK(output.events == 0, "first low sample must not alert");
  CHECK(output.next_sample_after_ms == 5000, "confirmation deadline");

  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 66000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.level == BD_BATTERY_LEVEL_REMINDER, "20%% is reminder");
  CHECK(output.events & BD_BATTERY_EVENT_LOW, "reminder event");
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_SHOW_REMINDER,
        "reminder recommendation");

  status = normal_status(5);
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 96000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.level == BD_BATTERY_LEVEL_REMINDER, "critical waits");
  CHECK(output.events == 0, "critical first sample must not alert");
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 101000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.level == BD_BATTERY_LEVEL_CRITICAL, "5%% is critical");
  CHECK(output.events & BD_BATTERY_EVENT_CRITICAL, "critical event");
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_PREPARE_SHUTDOWN,
        "critical unplugged recommendation");

  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 104000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.events == 0, "critical must not repeat early");
  CHECK(output.next_sample_after_ms == 15000, "critical deadline");

  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 221000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.events & BD_BATTERY_EVENT_CRITICAL,
        "critical repeat after interval");

  status = normal_status(6);
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 236000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.level == BD_BATTERY_LEVEL_CRITICAL,
        "6%% stays critical inside hysteresis");
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 241000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.level == BD_BATTERY_LEVEL_CRITICAL,
        "6%% still critical after confirmation");

  status = normal_status(7);
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 246000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.level == BD_BATTERY_LEVEL_CRITICAL, "7%% waits");
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 251000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.level == BD_BATTERY_LEVEL_REMINDER, "7%% leaves critical");
  CHECK(output.events & BD_BATTERY_EVENT_LOW, "downgrade emits reminder");

  status = normal_status(22);
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 281000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.level == BD_BATTERY_LEVEL_REMINDER,
        "22%% stays inside reminder hysteresis");
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 286000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.level == BD_BATTERY_LEVEL_REMINDER,
        "22%% still reminder after confirmation");

  status = normal_status(23);
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 316000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.level == BD_BATTERY_LEVEL_REMINDER, "23%% waits");
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 321000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.level == BD_BATTERY_LEVEL_NORMAL, "23%% recovers");
  CHECK(output.events & BD_BATTERY_EVENT_RECOVERED, "recovery event");
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_NONE,
        "normal recommendation");
}

static void test_policy_debounce(void) {
  bd_battery_policy_config config;
  bd_battery_policy_config_default(&config);
  bd_battery_policy policy = init_policy(&config);
  bd_battery_policy_output output;
  bd_battery_status status = normal_status(50);
  bd_battery_policy_update(&policy, &status, 1000, &output);

  status = normal_status(20);
  bd_battery_policy_update(&policy, &status, 61000, &output);
  CHECK(output.events == 0, "first low");
  status = normal_status(50);
  bd_battery_policy_update(&policy, &status, 66000, &output);
  CHECK(output.events == 0, "recovery before confirmation");
  CHECK(output.level == BD_BATTERY_LEVEL_NORMAL, "jitter must not warn");
  status = normal_status(20);
  bd_battery_policy_update(&policy, &status, 71000, &output);
  CHECK(output.events == 0, "new first low");
  bd_battery_policy_update(&policy, &status, 76000, &output);
  CHECK(output.events & BD_BATTERY_EVENT_LOW,
        "two consecutive lows must warn");

  bd_battery_status critical = normal_status(5);
  bd_battery_policy_update(&policy, &critical, 91000, &output);
  CHECK(output.events == 0, "critical cannot fire from one sample");
  bd_battery_status normal = normal_status(50);
  bd_battery_policy_update(&policy, &normal, 96000, &output);
  CHECK(output.level == BD_BATTERY_LEVEL_REMINDER,
        "return to 50%% leaves reminder through normal hysteresis");
}

static void test_policy_plug_suppression(void) {
  bd_battery_policy_config config;
  bd_battery_policy_config_default(&config);
  bd_battery_policy policy = init_policy(&config);
  bd_battery_policy_output output;
  bd_battery_status status = normal_status(50);
  bd_battery_policy_update(&policy, &status, 1000, &output);

  status = normal_status(20);
  bd_battery_policy_update(&policy, &status, 61000, &output);
  bd_battery_policy_update(&policy, &status, 66000, &output);
  CHECK(output.events & BD_BATTERY_EVENT_LOW, "low before plug");

  status = charging_status(20);
  bd_battery_policy_update(&policy, &status, 96000, &output);
  CHECK(output.events & BD_BATTERY_EVENT_PLUGGED, "plug event");
  CHECK(output.suppressed, "plugged must suppress");
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_NONE,
        "plugged reminder is suppressed");
  CHECK(output.next_sample_after_ms == 60000, "charging deadline");

  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 156000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(!(output.events & BD_BATTERY_EVENT_LOW),
        "plugged level must not re-alert");

  status = normal_status(20);
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 216000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.events & BD_BATTERY_EVENT_UNPLUGGED, "unplug event");
  CHECK(output.events & BD_BATTERY_EVENT_LOW, "unplug resumes reminder");
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_SHOW_REMINDER,
        "unplugged recommendation");
  CHECK(!output.suppressed, "unplug must clear suppression");
}

static void test_policy_plug_before_confirmation(void) {
  bd_battery_policy_config config;
  bd_battery_policy_config_default(&config);
  bd_battery_policy policy = init_policy(&config);
  bd_battery_policy_output output;
  bd_battery_status status = normal_status(50);
  bd_battery_policy_update(&policy, &status, 1000, &output);
  status = normal_status(20);
  bd_battery_policy_update(&policy, &status, 61000, &output);
  status = charging_status(20);
  bd_battery_policy_update(&policy, &status, 66000, &output);
  CHECK(output.events & BD_BATTERY_EVENT_PLUGGED, "plug before confirm");
  CHECK(!(output.events & BD_BATTERY_EVENT_LOW),
        "plugged sample must not alert");

  status = normal_status(20);
  bd_battery_policy_update(&policy, &status, 72000, &output);
  CHECK(output.level == BD_BATTERY_LEVEL_REMINDER,
        "level may be confirmed while plugged");
  CHECK(output.events & BD_BATTERY_EVENT_UNPLUGGED, "unplug");
  CHECK(output.events & BD_BATTERY_EVENT_LOW, "unplug alert");
}

static void test_policy_charging_inference(void) {
  bd_battery_policy_config config;
  bd_battery_policy_config_default(&config);
  bd_battery_policy policy = init_policy(&config);
  bd_battery_policy_output output;
  bd_battery_status status = normal_status(50);
  bd_battery_policy_update(&policy, &status, 1000, &output);

  bd_battery_status charging = normal_status(20);
  charging.external_power_known = false;
  charging.status = BD_BATTERY_CHARGE_CHARGING;
  bd_battery_policy_update(&policy, &charging, 61000, &output);
  CHECK(output.plugged_in_known && output.plugged_in,
        "charging status infers plugged");
  CHECK(output.suppressed, "charging status suppresses");
  bd_battery_policy_update(&policy, &charging, 66000, &output);
  CHECK(output.level == BD_BATTERY_LEVEL_REMINDER,
        "level still tracked while suppressed");
  CHECK(!(output.events & BD_BATTERY_EVENT_LOW),
        "no low event while charging");

  bd_battery_status discharging = normal_status(20);
  discharging.external_power_known = false;
  discharging.status = BD_BATTERY_CHARGE_DISCHARGING;
  bd_battery_policy_update(&policy, &discharging, 71000, &output);
  CHECK(output.events & BD_BATTERY_EVENT_UNPLUGGED,
        "discharging clears inferred plug");
  CHECK(output.events & BD_BATTERY_EVENT_LOW, "low resumes");
}

static void test_policy_unknown_and_shutdown_gate(void) {
  bd_battery_policy_config config;
  bd_battery_policy_config_default(&config);
  bd_battery_policy policy = init_policy(&config);
  bd_battery_policy_output output;
  bd_battery_status status = normal_status(50);
  bd_battery_policy_update(&policy, &status, 1000, &output);
  status = normal_status(20);
  bd_battery_policy_update(&policy, &status, 61000, &output);
  bd_battery_policy_update(&policy, &status, 66000, &output);
  CHECK(output.events & BD_BATTERY_EVENT_LOW, "reminder");

  bd_battery_status unknown = unknown_status();
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &unknown, 96000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.level == BD_BATTERY_LEVEL_UNKNOWN, "unknown level");
  CHECK(output.events & BD_BATTERY_EVENT_UNKNOWN, "unknown event");
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_NONE,
        "unknown must not recommend");
  CHECK(output.next_sample_after_ms == 5000, "first unknown interval");
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &unknown, 101000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.events == 0, "unknown transition only once");
  CHECK(output.next_sample_after_ms == 10000, "unknown backoff doubles");
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &unknown, 111000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.next_sample_after_ms == 20000, "unknown backoff grows");

  status = normal_status(5);
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 131000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.level == BD_BATTERY_LEVEL_UNKNOWN,
        "critical still needs confirmation after unknown");
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_NONE,
        "no shutdown before confirmation");
  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 136000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(output.level == BD_BATTERY_LEVEL_CRITICAL, "critical after unknown");
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_PREPARE_SHUTDOWN,
        "known unplugged critical may prepare shutdown");

  bd_battery_status external_unknown = normal_status(5);
  external_unknown.external_power_known = false;
  bd_battery_policy_update(&policy, &external_unknown, 166000, &output);
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_SHOW_CRITICAL,
        "unknown external must not recommend shutdown");

  bd_battery_status present_unknown = normal_status(5);
  present_unknown.present_known = false;
  bd_battery_policy_update(&policy, &present_unknown, 196000, &output);
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_SHOW_CRITICAL,
        "unknown presence must not recommend shutdown");

  bd_battery_status detached = normal_status(5);
  detached.present = false;
  bd_battery_policy_update(&policy, &detached, 226000, &output);
  CHECK(output.level == BD_BATTERY_LEVEL_UNKNOWN,
        "detached battery is unknown");
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_NONE,
        "detached battery must not alert");

  bd_battery_policy_config no_shutdown;
  bd_battery_policy_config_default(&no_shutdown);
  no_shutdown.recommend_shutdown = false;
  bd_battery_policy second = init_policy(&no_shutdown);
  status = normal_status(5);
  bd_battery_policy_update(&second, &status, 1000, &output);
  bd_battery_policy_update(&second, &status, 6000, &output);
  CHECK(output.level == BD_BATTERY_LEVEL_CRITICAL,
        "critical with shutdown disabled");
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_SHOW_CRITICAL,
        "shutdown disabled only shows critical");
}

static void test_policy_deadlines_and_disabled(void) {
  bd_battery_policy_config config;
  bd_battery_policy_config_default(&config);
  bd_battery_policy policy = init_policy(&config);
  bd_battery_policy_output output;
  bd_battery_status status = normal_status(50);
  bd_battery_policy_update(&policy, &status, 1000, &output);
  CHECK(output.next_sample_after_ms == 60000, "normal sampling");
  CHECK(output.next_sample_ms == 61000, "normal deadline");

  status = normal_status(20);
  bd_battery_policy_update(&policy, &status, 61000, &output);
  CHECK(output.next_sample_after_ms == 5000, "confirmation sampling");
  bd_battery_policy_update(&policy, &status, 66000, &output);
  CHECK(output.next_sample_after_ms == 30000, "reminder sampling");
  CHECK(bd_battery_policy_next_sample_ms(&policy) == 96000,
        "stored deadline");

  status = normal_status(5);
  bd_battery_policy_update(&policy, &status, 96000, &output);
  bd_battery_policy_update(&policy, &status, 101000, &output);
  CHECK(output.next_sample_after_ms == 15000, "critical sampling");

  status = normal_status(50);
  bd_battery_policy_update(&policy, &status, UINT64_MAX - 10, &output);
  CHECK(output.next_sample_ms == UINT64_MAX, "deadline saturates");

  bd_battery_policy_config disabled;
  bd_battery_policy_config_default(&disabled);
  disabled.enabled = false;
  bd_battery_policy disabled_policy;
  bd_battery_error error;
  CHECK_POLICY_RESULT(bd_battery_policy_init(&disabled_policy, &disabled, 100, &output,
                                      &error),
               BD_BATTERY_POLICY_DISABLED);
  CHECK(output.next_sample_ms == 0, "disabled has no deadline");
  CHECK_POLICY_RESULT(bd_battery_policy_update(&disabled_policy, &status, 100,
                                        &output),
               BD_BATTERY_POLICY_DISABLED);
  CHECK(output.events == 0, "disabled has no events");
  CHECK(output.recommendation == BD_BATTERY_RECOMMEND_NONE,
        "disabled has no recommendation");
}

static void test_policy_time_going_backwards(void) {
  bd_battery_policy_config config;
  bd_battery_policy_config_default(&config);
  bd_battery_policy policy = init_policy(&config);
  bd_battery_policy_output output;
  bd_battery_status status = normal_status(50);
  bd_battery_policy_update(&policy, &status, 1000, &output);
  status = normal_status(5);
  bd_battery_policy_update(&policy, &status, 61000, &output);
  bd_battery_policy_update(&policy, &status, 66000, &output);
  CHECK(output.events & BD_BATTERY_EVENT_CRITICAL, "critical event");

  CHECK_POLICY_RESULT(bd_battery_policy_update(&policy, &status, 5000, &output),
               BD_BATTERY_POLICY_OK);
  CHECK(!(output.events & BD_BATTERY_EVENT_CRITICAL),
        "backwards clock must not look like a repeat");
  CHECK(output.next_sample_ms == 20000, "deadline uses current now");
}

int main(void) {
  test_full_fixture();
  test_limited_fixture();
  test_no_battery_and_late_acquire();
  test_selection();
  test_present_false();
  test_bad_values();
  test_capacity_boundaries();
  test_external_power_aggregation();
  test_stale_and_type_change();
  test_supply_table_truncation();
  test_external_power_beyond_public_table();
  test_fixture_none();
  test_policy_validation();
  test_policy_levels_and_hysteresis();
  test_policy_debounce();
  test_policy_plug_suppression();
  test_policy_plug_before_confirmation();
  test_policy_charging_inference();
  test_policy_unknown_and_shutdown_gate();
  test_policy_deadlines_and_disabled();
  test_policy_time_going_backwards();

  if (g_failures != 0) {
    fprintf(stderr, "device_battery_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("device_battery_test: OK\n");
  return 0;
}
