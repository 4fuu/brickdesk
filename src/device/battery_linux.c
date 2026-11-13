#define _POSIX_C_SOURCE 200809L

#include "battery_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define BD_POWER_SUPPLY_REL "sys/class/power_supply"
#define BD_ATTR_TEXT_MAX 64
#define BD_AXP2202_BATTERY_NAME "axp2202-battery"

typedef enum attr_read {
  ATTR_MISSING = 0,
  ATTR_OK,
  ATTR_INVALID
} attr_read;

typedef struct name_list {
  char **items;
  size_t count;
  size_t capacity;
} name_list;

static bool path_join(const char *base, const char *name, const char *attr,
                      char *out, size_t out_size) {
  int written;
  if (name == NULL || name[0] == '\0') {
    if (attr == NULL) {
      return false;
    }
    written = snprintf(out, out_size, "%s/%s", base, attr);
  } else if (attr == NULL || attr[0] == '\0') {
    written = snprintf(out, out_size, "%s/%s", base, name);
  } else {
    written = snprintf(out, out_size, "%s/%s/%s", base, name, attr);
  }
  return written > 0 && (size_t)written < out_size;
}

static bool root_join(const char *root, const char *relative, char *out,
                      size_t out_size) {
  const char *base = root == NULL ? "" : root;
  int written;
  if (base[0] == '\0' || (base[0] == '/' && base[1] == '\0')) {
    written = snprintf(out, out_size, "/%s", relative);
  } else {
    size_t length = strlen(base);
    written = snprintf(out, out_size, "%s%s%s", base,
                       base[length - 1] == '/' ? "" : "/", relative);
  }
  return written > 0 && (size_t)written < out_size;
}

static bool valid_supply_name(const char *name) {
  if (name == NULL || name[0] == '\0') {
    return false;
  }
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    return false;
  }
  return strchr(name, '/') == NULL;
}

static void trim(char *text) {
  if (text == NULL) {
    return;
  }
  size_t length = strlen(text);
  while (length > 0 && isspace((unsigned char)text[length - 1])) {
    text[--length] = '\0';
  }
  char *start = text;
  while (*start != '\0' && isspace((unsigned char)*start)) {
    ++start;
  }
  if (start != text) {
    memmove(text, start, strlen(start) + 1);
  }
}

static attr_read read_text_attr(const char *path, char *out,
                                size_t out_size) {
  if (path == NULL || out == NULL || out_size < 2) {
    return ATTR_INVALID;
  }
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return errno == ENOENT ? ATTR_MISSING : ATTR_INVALID;
  }
  size_t used = fread(out, 1, out_size - 1, file);
  bool failed = ferror(file) != 0;
  bool truncated = false;
  if (!failed && used == out_size - 1) {
    int extra = fgetc(file);
    if (extra != EOF) {
      truncated = true;
    }
    if (ferror(file)) {
      failed = true;
    }
  }
  if (fclose(file) != 0) {
    failed = true;
  }
  out[used] = '\0';
  if (failed || truncated) {
    return ATTR_INVALID;
  }
  trim(out);
  if (out[0] == '\0') {
    return ATTR_INVALID;
  }
  return ATTR_OK;
}

static bool parse_int64(const char *text, int64_t *out) {
  if (text == NULL || out == NULL || text[0] == '\0') {
    return false;
  }
  errno = 0;
  char *end = NULL;
  long long value = strtoll(text, &end, 10);
  if (errno != 0 || end == text) {
    return false;
  }
  while (*end != '\0' && isspace((unsigned char)*end)) {
    ++end;
  }
  if (*end != '\0') {
    return false;
  }
  *out = (int64_t)value;
  return true;
}

static attr_read read_int_attr(const char *path, int64_t *out) {
  char text[BD_ATTR_TEXT_MAX];
  attr_read read = read_text_attr(path, text, sizeof(text));
  if (read != ATTR_OK) {
    return read;
  }
  if (!parse_int64(text, out)) {
    return ATTR_INVALID;
  }
  return ATTR_OK;
}

static attr_read read_bool_attr(const char *path, bool *out) {
  int64_t value = 0;
  attr_read read = read_int_attr(path, &value);
  if (read != ATTR_OK) {
    return read;
  }
  if (value != 0 && value != 1) {
    return ATTR_INVALID;
  }
  *out = value == 1;
  return ATTR_OK;
}

static bool parse_status(const char *text, bd_battery_charge_status *out) {
  if (strcasecmp(text, "Charging") == 0) {
    *out = BD_BATTERY_CHARGE_CHARGING;
    return true;
  }
  if (strcasecmp(text, "Discharging") == 0) {
    *out = BD_BATTERY_CHARGE_DISCHARGING;
    return true;
  }
  if (strcasecmp(text, "Not charging") == 0) {
    *out = BD_BATTERY_CHARGE_NOT_CHARGING;
    return true;
  }
  if (strcasecmp(text, "Full") == 0) {
    *out = BD_BATTERY_CHARGE_FULL;
    return true;
  }
  if (strcasecmp(text, "Unknown") == 0) {
    *out = BD_BATTERY_CHARGE_UNKNOWN;
    return true;
  }
  return false;
}

static bd_battery_supply_type parse_type(const char *text) {
  if (strcasecmp(text, "Battery") == 0) {
    return BD_BATTERY_SUPPLY_BATTERY;
  }
  if (strcasecmp(text, "Mains") == 0) {
    return BD_BATTERY_SUPPLY_MAINS;
  }
  if (strcasecmp(text, "UPS") == 0) {
    return BD_BATTERY_SUPPLY_UPS;
  }
  if (strcasecmp(text, "Unknown") == 0) {
    return BD_BATTERY_SUPPLY_UNKNOWN;
  }
  if (strcasecmp(text, "USB") == 0 || strcasecmp(text, "USB_DCP") == 0 ||
      strcasecmp(text, "USB_CDP") == 0 || strcasecmp(text, "USB_ACA") == 0 ||
      strcasecmp(text, "USB_C") == 0 ||
      strcasecmp(text, "USB_TYPE_C") == 0 ||
      strcasecmp(text, "USB_PD") == 0 ||
      strcasecmp(text, "USB_PD_DRP") == 0) {
    return BD_BATTERY_SUPPLY_USB;
  }
  return BD_BATTERY_SUPPLY_OTHER;
}

static bd_battery_health parse_health(const char *text) {
  if (strcasecmp(text, "Good") == 0) {
    return BD_BATTERY_HEALTH_GOOD;
  }
  if (strcasecmp(text, "Overheat") == 0) {
    return BD_BATTERY_HEALTH_OVERHEAT;
  }
  if (strcasecmp(text, "Dead") == 0) {
    return BD_BATTERY_HEALTH_DEAD;
  }
  if (strcasecmp(text, "Over voltage") == 0) {
    return BD_BATTERY_HEALTH_OVER_VOLTAGE;
  }
  if (strcasecmp(text, "Unspecified failure") == 0) {
    return BD_BATTERY_HEALTH_FAILURE;
  }
  if (strcasecmp(text, "Cold") == 0) {
    return BD_BATTERY_HEALTH_COLD;
  }
  if (strcasecmp(text, "Over current") == 0) {
    return BD_BATTERY_HEALTH_OVER_CURRENT;
  }
  if (strcasecmp(text, "Unknown") == 0) {
    return BD_BATTERY_HEALTH_UNKNOWN;
  }
  return BD_BATTERY_HEALTH_OTHER;
}

static bool read_supply(const char *class_dir, const char *name,
                        bd_battery_supply *supply, unsigned *read_errors) {
  memset(supply, 0, sizeof(*supply));
  if (!bd_battery_copy_string(supply->name, sizeof(supply->name), name)) {
    return false;
  }

  char path[BD_BATTERY_PATH_MAX];
  char text[BD_ATTR_TEXT_MAX];
  if (!path_join(class_dir, name, "type", path, sizeof(path))) {
    return false;
  }
  attr_read read = read_text_attr(path, text, sizeof(text));
  if (read == ATTR_OK) {
    bd_battery_copy_string(supply->type_text, sizeof(supply->type_text), text);
    supply->type = parse_type(text);
  } else if (read == ATTR_INVALID) {
    ++*read_errors;
  }

  if (path_join(class_dir, name, "present", path, sizeof(path))) {
    read = read_bool_attr(path, &supply->present);
    if (read == ATTR_OK) {
      supply->present_known = true;
    } else if (read == ATTR_INVALID) {
      ++*read_errors;
    }
  }

  if (path_join(class_dir, name, "online", path, sizeof(path))) {
    read = read_bool_attr(path, &supply->online);
    if (read == ATTR_OK) {
      supply->online_known = true;
    } else if (read == ATTR_INVALID) {
      ++*read_errors;
    }
  }

  if (path_join(class_dir, name, "status", path, sizeof(path))) {
    read = read_text_attr(path, text, sizeof(text));
    if (read == ATTR_OK) {
      if (parse_status(text, &supply->status)) {
        supply->status_known = true;
      } else {
        ++*read_errors;
      }
    } else if (read == ATTR_INVALID) {
      ++*read_errors;
    }
  }
  return true;
}

static void name_list_free(name_list *list) {
  if (list == NULL) {
    return;
  }
  for (size_t i = 0; i < list->count; ++i) {
    free(list->items[i]);
  }
  free(list->items);
  memset(list, 0, sizeof(*list));
}

static bool name_list_add(name_list *list, const char *name) {
  if (list->count == list->capacity) {
    size_t capacity = list->capacity == 0 ? 8 : list->capacity * 2;
    char **items = realloc(list->items, capacity * sizeof(*items));
    if (items == NULL) {
      return false;
    }
    list->items = items;
    list->capacity = capacity;
  }
  size_t length = strlen(name);
  char *copy = malloc(length + 1);
  if (copy == NULL) {
    return false;
  }
  memcpy(copy, name, length + 1);
  list->items[list->count++] = copy;
  return true;
}

static int compare_names(const void *left, const void *right) {
  const char *const *a = (const char *const *)left;
  const char *const *b = (const char *const *)right;
  return strcmp(*a, *b);
}

static bd_battery_result list_supplies(const char *class_dir, name_list *list,
                                       bd_battery_error *error) {
  memset(list, 0, sizeof(*list));
  DIR *directory = opendir(class_dir);
  if (directory == NULL) {
    if (errno == ENOENT) {
      bd_battery_set_error(error, BD_BATTERY_ERR_UNSUPPORTED,
                           "no power_supply class at %s", class_dir);
      return BD_BATTERY_ERR_UNSUPPORTED;
    }
    bd_battery_set_error(error, BD_BATTERY_ERR_IO, "cannot open %s: %s",
                         class_dir, strerror(errno));
    return BD_BATTERY_ERR_IO;
  }

  bd_battery_result result = BD_BATTERY_OK;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(directory);
    if (entry == NULL) {
      if (errno != 0) {
        bd_battery_set_error(error, BD_BATTERY_ERR_IO,
                             "cannot read %s: %s", class_dir,
                             strerror(errno));
        result = BD_BATTERY_ERR_IO;
      }
      break;
    }
    if (!valid_supply_name(entry->d_name)) {
      continue;
    }
    if (strlen(entry->d_name) >= BD_BATTERY_NAME_MAX) {
      bd_battery_set_error(error, BD_BATTERY_ERR_ARGUMENT,
                           "power supply name is too long: %s",
                           entry->d_name);
      result = BD_BATTERY_ERR_ARGUMENT;
      break;
    }
    if (!name_list_add(list, entry->d_name)) {
      bd_battery_set_error(error, BD_BATTERY_ERR_IO,
                           "out of memory while listing %s", class_dir);
      result = BD_BATTERY_ERR_IO;
      break;
    }
  }
  if (closedir(directory) != 0 && result == BD_BATTERY_OK) {
    bd_battery_set_error(error, BD_BATTERY_ERR_IO,
                         "cannot close %s: %s", class_dir, strerror(errno));
    result = BD_BATTERY_ERR_IO;
  }
  if (result != BD_BATTERY_OK) {
    name_list_free(list);
    return result;
  }
  if (list->count > 1) {
    qsort(list->items, list->count, sizeof(*list->items), compare_names);
  }
  return BD_BATTERY_OK;
}

static bool supply_is_battery(const char *class_dir, const char *name,
                              bd_battery_supply *info,
                              unsigned *read_errors) {
  if (!read_supply(class_dir, name, info, read_errors)) {
    return false;
  }
  return info->type == BD_BATTERY_SUPPLY_BATTERY;
}

static bd_battery_result discover_battery(const char *class_dir,
                                          const name_list *list,
                                          const char *explicit_name,
                                          char *out, size_t out_size,
                                          bd_battery_error *error) {
  bd_battery_supply info;
  unsigned ignored_errors = 0;

  if (explicit_name != NULL && explicit_name[0] != '\0') {
    bool found = false;
    for (size_t i = 0; i < list->count; ++i) {
      if (strcmp(list->items[i], explicit_name) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      bd_battery_set_error(error, BD_BATTERY_ERR_UNSUPPORTED,
                           "no power supply named %s", explicit_name);
      return BD_BATTERY_ERR_UNSUPPORTED;
    }
    if (!read_supply(class_dir, explicit_name, &info, &ignored_errors)) {
      bd_battery_set_error(error, BD_BATTERY_ERR_ARGUMENT,
                           "power supply name is too long: %s",
                           explicit_name);
      return BD_BATTERY_ERR_ARGUMENT;
    }
    if (info.type != BD_BATTERY_SUPPLY_BATTERY) {
      bd_battery_set_error(error, BD_BATTERY_ERR_UNSUPPORTED,
                           "%s is not a Battery power supply (type=%s)",
                           explicit_name,
                           info.type_text[0] ? info.type_text : "missing");
      return BD_BATTERY_ERR_UNSUPPORTED;
    }
    if (!bd_battery_copy_string(out, out_size, explicit_name)) {
      bd_battery_set_error(error, BD_BATTERY_ERR_ARGUMENT,
                           "battery name is too long: %s", explicit_name);
      return BD_BATTERY_ERR_ARGUMENT;
    }
    return BD_BATTERY_OK;
  }

  bool have_present = false;
  bool have_unknown = false;
  for (size_t i = 0; i < list->count; ++i) {
    if (!supply_is_battery(class_dir, list->items[i], &info,
                           &ignored_errors)) {
      continue;
    }
    if (info.present_known) {
      if (info.present) {
        have_present = true;
      }
    } else {
      have_unknown = true;
    }
  }

  /*
   * Prefer a present battery.  If none is known present, prefer one whose
   * presence is unknown (rather than one that is known absent).  Only when
   * every battery node is known absent do we still select one, so diagnostics
   * can report present=no instead of pretending the device has no battery
   * hardware at all.
   */
  int wanted = have_present ? 1 : (have_unknown ? -1 : 0);
  const char *first = NULL;
  const char *preferred = NULL;
  for (size_t i = 0; i < list->count; ++i) {
    if (!supply_is_battery(class_dir, list->items[i], &info,
                           &ignored_errors)) {
      continue;
    }
    int present = info.present_known ? (info.present ? 1 : 0) : -1;
    if (wanted == 1 && present != 1) {
      continue;
    }
    if (wanted == -1 && present != -1) {
      continue;
    }
    if (first == NULL) {
      first = list->items[i];
    }
    if (strcmp(list->items[i], BD_AXP2202_BATTERY_NAME) == 0) {
      preferred = list->items[i];
      break;
    }
  }

  const char *selected = preferred != NULL ? preferred : first;
  if (selected == NULL) {
    bd_battery_set_error(error, BD_BATTERY_ERR_UNSUPPORTED,
                         "no Battery power supply in %s", class_dir);
    return BD_BATTERY_ERR_UNSUPPORTED;
  }
  if (!bd_battery_copy_string(out, out_size, selected)) {
    bd_battery_set_error(error, BD_BATTERY_ERR_ARGUMENT,
                         "battery name is too long: %s", selected);
    return BD_BATTERY_ERR_ARGUMENT;
  }
  return BD_BATTERY_OK;
}

bd_battery_result bd_battery_open(bd_battery *battery,
                                  const bd_battery_config *config,
                                  bd_battery_error *error) {
  if (battery == NULL) {
    bd_battery_set_error(error, BD_BATTERY_ERR_ARGUMENT,
                         "battery handle is required");
    return BD_BATTERY_ERR_ARGUMENT;
  }
  memset(battery, 0, sizeof(*battery));
  bd_battery_error_clear(error);

  const char *root = config == NULL ? NULL : config->sysfs_root;
  const char *explicit_name =
      config == NULL ? NULL : config->battery_name;
  if (root != NULL && root[0] != '\0' &&
      !bd_battery_copy_string(battery->root, sizeof(battery->root), root)) {
    bd_battery_set_error(error, BD_BATTERY_ERR_ARGUMENT,
                         "sysfs root is too long");
    return BD_BATTERY_ERR_ARGUMENT;
  }
  if (!root_join(battery->root, BD_POWER_SUPPLY_REL, battery->class_dir,
                 sizeof(battery->class_dir))) {
    bd_battery_set_error(error, BD_BATTERY_ERR_ARGUMENT,
                         "power_supply path is too long");
    return BD_BATTERY_ERR_ARGUMENT;
  }

  if (explicit_name != NULL && explicit_name[0] != '\0') {
    if (!valid_supply_name(explicit_name) ||
        !bd_battery_copy_string(battery->battery_name,
                                sizeof(battery->battery_name),
                                explicit_name)) {
      bd_battery_set_error(error, BD_BATTERY_ERR_ARGUMENT,
                           "invalid battery name: %s", explicit_name);
      return BD_BATTERY_ERR_ARGUMENT;
    }
    battery->explicit_battery = true;
  }

  name_list list;
  bd_battery_result result = list_supplies(battery->class_dir, &list, error);
  if (result != BD_BATTERY_OK) {
    return result;
  }

  char selected[BD_BATTERY_NAME_MAX];
  selected[0] = '\0';
  result = discover_battery(battery->class_dir, &list, explicit_name, selected,
                            sizeof(selected), error);
  if (result == BD_BATTERY_OK) {
    if (!bd_battery_copy_string(battery->battery_name,
                                sizeof(battery->battery_name), selected) ||
        !path_join(battery->class_dir, selected, "", battery->battery_path,
                   sizeof(battery->battery_path))) {
      bd_battery_set_error(error, BD_BATTERY_ERR_ARGUMENT,
                           "battery path is too long: %s", selected);
      result = BD_BATTERY_ERR_ARGUMENT;
    } else {
      battery->selected = true;
      bd_battery_error_clear(error);
    }
  }
  name_list_free(&list);
  return result;
}

static void clear_measurements(bd_battery_status *status) {
  status->battery_available = false;
  status->present_known = false;
  status->present = false;
  status->capacity_known = false;
  status->capacity_percent = 0;
  status->status_known = false;
  status->status = BD_BATTERY_CHARGE_UNKNOWN;
  status->voltage_known = false;
  status->voltage_uv = 0;
  status->current_known = false;
  status->current_ua = 0;
  status->temp_known = false;
  status->temp_deci_c = 0;
  status->time_to_empty_known = false;
  status->time_to_empty_s = 0;
  status->time_to_full_known = false;
  status->time_to_full_s = 0;
  status->health_known = false;
  status->health = BD_BATTERY_HEALTH_UNKNOWN;
}

static void read_measurements(const char *directory,
                              bd_battery_status *status) {
  char path[BD_BATTERY_PATH_MAX];
  char text[BD_ATTR_TEXT_MAX];
  int64_t value = 0;
  attr_read read;

  if (path_join(directory, "", "capacity", path, sizeof(path))) {
    read = read_int_attr(path, &value);
    if (read == ATTR_OK && value >= 0 && value <= 100) {
      status->capacity_known = true;
      status->capacity_percent = (int)value;
    } else if (read == ATTR_INVALID ||
               (read == ATTR_OK && (value < 0 || value > 100))) {
      ++status->read_errors;
    }
  }

  if (path_join(directory, "", "status", path, sizeof(path))) {
    read = read_text_attr(path, text, sizeof(text));
    if (read == ATTR_OK) {
      if (parse_status(text, &status->status)) {
        status->status_known = true;
      } else {
        ++status->read_errors;
      }
    } else if (read == ATTR_INVALID) {
      ++status->read_errors;
    }
  }

  if (path_join(directory, "", "voltage_now", path, sizeof(path))) {
    read = read_int_attr(path, &value);
    if (read == ATTR_OK && value >= 0) {
      status->voltage_known = true;
      status->voltage_uv = value;
    } else if (read == ATTR_INVALID || (read == ATTR_OK && value < 0)) {
      ++status->read_errors;
    }
  }

  if (path_join(directory, "", "current_now", path, sizeof(path))) {
    read = read_int_attr(path, &value);
    if (read == ATTR_OK) {
      status->current_known = true;
      status->current_ua = value;
    } else if (read == ATTR_INVALID) {
      ++status->read_errors;
    }
  }

  if (path_join(directory, "", "temp", path, sizeof(path))) {
    read = read_int_attr(path, &value);
    if (read == ATTR_OK && value >= INT32_MIN && value <= INT32_MAX) {
      status->temp_known = true;
      status->temp_deci_c = (int32_t)value;
    } else if (read == ATTR_INVALID ||
               (read == ATTR_OK &&
                (value < INT32_MIN || value > INT32_MAX))) {
      ++status->read_errors;
    }
  }

  if (path_join(directory, "", "time_to_empty_now", path, sizeof(path))) {
    read = read_int_attr(path, &value);
    if (read == ATTR_OK && value >= 0) {
      status->time_to_empty_known = true;
      status->time_to_empty_s = value;
    } else if (read == ATTR_INVALID || (read == ATTR_OK && value < 0)) {
      ++status->read_errors;
    }
  }

  if (path_join(directory, "", "time_to_full_now", path, sizeof(path))) {
    read = read_int_attr(path, &value);
    if (read == ATTR_OK && value >= 0) {
      status->time_to_full_known = true;
      status->time_to_full_s = value;
    } else if (read == ATTR_INVALID || (read == ATTR_OK && value < 0)) {
      ++status->read_errors;
    }
  }

  if (path_join(directory, "", "health", path, sizeof(path))) {
    read = read_text_attr(path, text, sizeof(text));
    if (read == ATTR_OK) {
      status->health_known = true;
      status->health = parse_health(text);
    } else if (read == ATTR_INVALID) {
      ++status->read_errors;
    }
  }
}

typedef struct external_power_scan {
  bool any_external;
  bool any_online;
  bool all_known;
} external_power_scan;

static void accumulate_external_power(const bd_battery_supply *supply,
                                      external_power_scan *scan) {
  if (supply->type == BD_BATTERY_SUPPLY_UNKNOWN) {
    /* A failed type read cannot prove that this is not external power. */
    scan->any_external = true;
    scan->all_known = false;
    return;
  }
  if (supply->type != BD_BATTERY_SUPPLY_USB &&
      supply->type != BD_BATTERY_SUPPLY_MAINS) {
    return;
  }
  scan->any_external = true;
  if (!supply->online_known) {
    scan->all_known = false;
    return;
  }
  if (supply->online) {
    scan->any_online = true;
  }
}

static void finish_external_power(bd_battery_status *status,
                                  const external_power_scan *scan) {
  status->external_power = scan->any_online;
  status->external_power_known =
      scan->any_external && (scan->any_online || scan->all_known);
}

bd_battery_result bd_battery_read(const bd_battery *battery,
                                  bd_battery_status *status,
                                  bd_battery_error *error) {
  if (battery == NULL || status == NULL) {
    bd_battery_set_error(error, BD_BATTERY_ERR_ARGUMENT,
                         "battery handle and status are required");
    return BD_BATTERY_ERR_ARGUMENT;
  }
  memset(status, 0, sizeof(*status));
  bd_battery_error_clear(error);

  name_list list;
  bd_battery_result result = list_supplies(battery->class_dir, &list, error);
  if (result != BD_BATTERY_OK) {
    return result;
  }

  status->supply_count = (unsigned)list.count;
  status->supplies_truncated = list.count > BD_BATTERY_MAX_SUPPLIES;
  unsigned copied = status->supply_count < BD_BATTERY_MAX_SUPPLIES
                        ? status->supply_count
                        : BD_BATTERY_MAX_SUPPLIES;
  external_power_scan power = {.any_external = false,
                               .any_online = false,
                               .all_known = true};
  for (size_t i = 0; i < list.count; ++i) {
    bd_battery_supply extra;
    bd_battery_supply *entry =
        i < copied ? &status->supplies[i] : &extra;
    if (!read_supply(battery->class_dir, list.items[i], entry,
                     &status->read_errors)) {
      if (i < copied) {
        bd_battery_set_error(error, BD_BATTERY_ERR_IO,
                             "power supply name is too long: %s",
                             list.items[i]);
        name_list_free(&list);
        return BD_BATTERY_ERR_IO;
      }
      /*
       * A node beyond the public table could not be classified.  Treat it as
       * potentially external and unknown instead of reporting a false
       * "unplugged" from the truncated table.
       */
      power.any_external = true;
      power.all_known = false;
      continue;
    }
    accumulate_external_power(entry, &power);
  }
  finish_external_power(status, &power);

  char selected[BD_BATTERY_NAME_MAX];
  selected[0] = '\0';
  if (battery->battery_name[0] != '\0') {
    bd_battery_copy_string(selected, sizeof(selected), battery->battery_name);
  } else {
    result = discover_battery(battery->class_dir, &list, NULL, selected,
                              sizeof(selected), error);
    if (result != BD_BATTERY_OK) {
      name_list_free(&list);
      return result;
    }
  }

  bd_battery_supply selected_info;
  bd_battery_supply *selected_supply = NULL;
  bool selected_in_list = false;
  size_t selected_index = 0;
  for (size_t i = 0; i < list.count; ++i) {
    if (strcmp(list.items[i], selected) == 0) {
      selected_in_list = true;
      selected_index = i;
      break;
    }
  }
  if (!selected_in_list) {
    bd_battery_result missing =
        battery->selected ? BD_BATTERY_ERR_STALE : BD_BATTERY_ERR_UNSUPPORTED;
    bd_battery_set_error(error, missing, "no power supply named %s", selected);
    clear_measurements(status);
    name_list_free(&list);
    return missing;
  }
  if (selected_index < copied) {
    selected_supply = &status->supplies[selected_index];
  } else {
    /*
     * More supplies than the public table holds.  The selected battery is
     * still read directly so truncation cannot turn into a false STALE.
     */
    if (!read_supply(battery->class_dir, selected, &selected_info,
                     &status->read_errors)) {
      bd_battery_set_error(error, BD_BATTERY_ERR_IO,
                           "cannot read selected battery %s", selected);
      clear_measurements(status);
      name_list_free(&list);
      return BD_BATTERY_ERR_IO;
    }
    selected_supply = &selected_info;
  }
  if (selected_supply->type != BD_BATTERY_SUPPLY_BATTERY) {
    bd_battery_set_error(error, BD_BATTERY_ERR_STALE,
                         "selected battery %s is no longer a Battery supply",
                         selected);
    clear_measurements(status);
    name_list_free(&list);
    return BD_BATTERY_ERR_STALE;
  }
  selected_supply->selected = true;

  char directory[BD_BATTERY_PATH_MAX];
  if (!path_join(battery->class_dir, selected, "", directory,
                 sizeof(directory))) {
    bd_battery_set_error(error, BD_BATTERY_ERR_IO,
                         "battery path is too long: %s", selected);
    clear_measurements(status);
    name_list_free(&list);
    return BD_BATTERY_ERR_IO;
  }

  status->battery_available = true;
  bd_battery_copy_string(status->battery_name, sizeof(status->battery_name),
                         selected);

  char path[BD_BATTERY_PATH_MAX];
  attr_read read = ATTR_MISSING;
  if (path_join(directory, "", "present", path, sizeof(path))) {
    read = read_bool_attr(path, &status->present);
  }
  if (read == ATTR_OK) {
    status->present_known = true;
  } else if (read == ATTR_INVALID) {
    ++status->read_errors;
  }

  /*
   * A detached battery has no meaningful capacity/status/voltage.  Do not let
   * a driver's stale zero turn into a critical-battery event.
   */
  if (!status->present_known || status->present) {
    read_measurements(directory, status);
  }

  if (access(directory, F_OK) != 0) {
    bd_battery_set_error(error, BD_BATTERY_ERR_STALE,
                         "selected battery %s disappeared during read",
                         selected);
    clear_measurements(status);
    name_list_free(&list);
    return BD_BATTERY_ERR_STALE;
  }

  name_list_free(&list);
  return BD_BATTERY_OK;
}

const char *bd_battery_selected_name(const bd_battery *battery) {
  if (battery == NULL || battery->battery_name[0] == '\0') {
    return NULL;
  }
  return battery->battery_name;
}

const char *bd_battery_selected_path(const bd_battery *battery) {
  if (battery == NULL || battery->battery_path[0] == '\0') {
    return NULL;
  }
  return battery->battery_path;
}

bool bd_battery_selected(const bd_battery *battery) {
  return battery != NULL && battery->selected;
}
