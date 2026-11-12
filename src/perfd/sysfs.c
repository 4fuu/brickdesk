#define _POSIX_C_SOURCE 200809L

#include "brickdesk/perf.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BD_PERF_MAX_WRITE_STEPS 20

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
}

static bool join_path(const char *root, const char *path, char *out,
                      size_t out_size) {
  int written;
  if (root == NULL || *root == '\0') {
    written = snprintf(out, out_size, "%s", path);
  } else {
    written = snprintf(out, out_size, "%s%s", root, path);
  }
  return written > 0 && (size_t)written < out_size;
}

static bool format_path(char *out, size_t out_size, const char *format, ...) {
  va_list args;
  va_start(args, format);
  int written = vsnprintf(out, out_size, format, args);
  va_end(args);
  return written > 0 && (size_t)written < out_size;
}

static void copy_text(char *dst, size_t dst_size, const char *src) {
  if (dst_size == 0) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  size_t length = strlen(src);
  if (length >= dst_size) {
    length = dst_size - 1;
  }
  memcpy(dst, src, length);
  dst[length] = '\0';
}

static void trim_text(char *text) {
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

static bool read_text_file(const char *path, char *out, size_t out_size) {
  if (path == NULL || out == NULL || out_size == 0) {
    return false;
  }
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  size_t used = fread(out, 1, out_size - 1, file);
  bool ok = !ferror(file);
  out[used] = '\0';
  fclose(file);
  if (ok) {
    trim_text(out);
  }
  return ok;
}

static bool read_long_file(const char *path, long *out) {
  char text[64];
  if (!read_text_file(path, text, sizeof(text))) {
    return false;
  }
  errno = 0;
  char *end = NULL;
  long value = strtol(text, &end, 10);
  if (errno != 0 || end == text) {
    return false;
  }
  while (*end != '\0' && isspace((unsigned char)*end)) {
    ++end;
  }
  if (*end != '\0') {
    return false;
  }
  *out = value;
  return true;
}

static size_t read_word_list(const char *path,
                             char items[][BD_PERF_GOVERNOR_MAX + 1],
                             size_t max_items) {
  char text[1024];
  if (!read_text_file(path, text, sizeof(text))) {
    return 0;
  }
  size_t count = 0;
  char *save = NULL;
  for (char *token = strtok_r(text, " \t\r\n", &save); token != NULL;
       token = strtok_r(NULL, " \t\r\n", &save)) {
    if (count >= max_items) {
      break;
    }
    snprintf(items[count], BD_PERF_GOVERNOR_MAX + 1, "%s", token);
    ++count;
  }
  return count;
}

static size_t read_frequency_list(const char *path, int *items,
                                  size_t max_items) {
  char text[1024];
  if (!read_text_file(path, text, sizeof(text))) {
    return 0;
  }
  size_t count = 0;
  char *cursor = text;
  while (*cursor != '\0' && count < max_items) {
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(cursor, &end, 10);
    if (errno != 0 || end == cursor) {
      break;
    }
    if (value > 0) {
      items[count++] = (int)value;
    }
    cursor = end;
  }
  return count;
}

static int parse_cpu_list(const char *text) {
  int highest = -1;
  const char *cursor = text;
  while (*cursor != '\0') {
    while (*cursor != '\0' && !isdigit((unsigned char)*cursor)) {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }
    errno = 0;
    char *end = NULL;
    long first = strtol(cursor, &end, 10);
    if (errno != 0 || end == cursor) {
      break;
    }
    long last = first;
    cursor = end;
    if (*cursor == '-') {
      ++cursor;
      errno = 0;
      last = strtol(cursor, &end, 10);
      if (errno != 0 || end == cursor) {
        break;
      }
      cursor = end;
    }
    if (last > highest) {
      highest = (int)last;
    }
    while (*cursor != '\0' && *cursor != ',') {
      ++cursor;
    }
    if (*cursor == ',') {
      ++cursor;
    }
  }
  return highest + 1;
}

static void probe_cpufreq(const char *root, bd_perf_system *system) {
  char base[BD_PERF_PATH_MAX];
  if (!join_path(root, "/sys/devices/system/cpu/cpufreq", base,
                 sizeof(base))) {
    return;
  }
  DIR *dir = opendir(base);
  if (dir == NULL) {
    return;
  }

  char selected[BD_PERF_PATH_MAX] = "";
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "policy", 6) != 0) {
      continue;
    }
    ++system->policy_count;
    if (selected[0] == '\0' || strcmp(entry->d_name, selected) < 0) {
      snprintf(selected, sizeof(selected), "%s", entry->d_name);
    }
  }
  closedir(dir);
  if (selected[0] == '\0') {
    return;
  }

  char policy[BD_PERF_PATH_MAX];
  if (!join_path(root, "/sys/devices/system/cpu/cpufreq", base,
                 sizeof(base)) ||
      !format_path(policy, sizeof(policy), "%s/%s", base, selected)) {
    return;
  }

  char path[BD_PERF_PATH_MAX];
  if (!format_path(path, sizeof(path), "%s/scaling_available_governors",
                   policy)) {
    return;
  }
  system->governor_count =
      read_word_list(path, system->governors, BD_PERF_MAX_GOVERNORS);

  if (format_path(path, sizeof(path), "%s/scaling_available_frequencies",
                  policy)) {
    system->freq_count =
        read_frequency_list(path, system->freqs, BD_PERF_MAX_FREQS);
  }

  long value = 0;
  if (format_path(path, sizeof(path), "%s/cpuinfo_min_freq", policy) &&
      read_long_file(path, &value)) {
    system->cpuinfo_min_khz = (int)value;
  }
  if (format_path(path, sizeof(path), "%s/cpuinfo_max_freq", policy) &&
      read_long_file(path, &value)) {
    system->cpuinfo_max_khz = (int)value;
  }

  if (system->freq_count == 0) {
    if (system->cpuinfo_min_khz > 0) {
      system->freqs[system->freq_count++] = system->cpuinfo_min_khz;
    }
    if (system->cpuinfo_max_khz > 0 &&
        system->cpuinfo_max_khz !=
            (system->freq_count > 0 ? system->freqs[0] : -1)) {
      system->freqs[system->freq_count++] = system->cpuinfo_max_khz;
    }
  }
  if (system->cpuinfo_min_khz <= 0 && system->freq_count > 0) {
    system->cpuinfo_min_khz = system->freqs[0];
  }
  if (system->cpuinfo_max_khz <= 0 && system->freq_count > 0) {
    system->cpuinfo_max_khz = system->freqs[system->freq_count - 1];
  }

  if (format_path(path, sizeof(path), "%s/scaling_governor", policy)) {
    snprintf(system->governor_path, sizeof(system->governor_path), "%s", path);
    read_text_file(path, system->current_governor,
                   sizeof(system->current_governor));
  }
  if (format_path(path, sizeof(path), "%s/scaling_cur_freq", policy)) {
    snprintf(system->current_freq_path, sizeof(system->current_freq_path),
             "%s", path);
    if (read_long_file(path, &value)) {
      system->has_current_freq = true;
      system->current_freq_khz = (int)value;
    }
  }
  if (format_path(path, sizeof(path), "%s/scaling_min_freq", policy)) {
    snprintf(system->min_path, sizeof(system->min_path), "%s", path);
    if (read_long_file(path, &value)) {
      system->current_min_khz = (int)value;
    }
  }
  if (format_path(path, sizeof(path), "%s/scaling_max_freq", policy)) {
    snprintf(system->max_path, sizeof(system->max_path), "%s", path);
    if (read_long_file(path, &value)) {
      system->current_max_khz = (int)value;
    }
  }

  system->cpufreq = system->governor_count > 0 && system->freq_count > 0;
}

static void probe_cpus(const char *root, bd_perf_system *system) {
  char path[BD_PERF_PATH_MAX];
  int present_count = 0;
  if (join_path(root, "/sys/devices/system/cpu/present", path,
                sizeof(path))) {
    char text[128];
    if (read_text_file(path, text, sizeof(text))) {
      present_count = parse_cpu_list(text);
    }
  }

  int highest = 1;
  char base[BD_PERF_PATH_MAX];
  if (join_path(root, "/sys/devices/system/cpu", base, sizeof(base))) {
    for (int cpu = 1; cpu < BD_PERF_MAX_CPUS; ++cpu) {
      char online[BD_PERF_PATH_MAX];
      if (!format_path(online, sizeof(online), "%s/cpu%d/online", base, cpu)) {
        continue;
      }
      long value = 0;
      if (read_long_file(online, &value)) {
        snprintf(system->online_path[cpu], sizeof(system->online_path[cpu]),
                 "%s", online);
        if (cpu + 1 > highest) {
          highest = cpu + 1;
        }
      }
    }
  }

  if (present_count > highest) {
    highest = present_count;
  }
  if (highest > BD_PERF_MAX_CPUS) {
    highest = BD_PERF_MAX_CPUS;
  }
  system->cpu_count = highest;
  system->online_count = 1; /* cpu0 is always online */
  for (int cpu = 1; cpu < system->cpu_count; ++cpu) {
    long value = 1;
    if (system->online_path[cpu][0] != '\0') {
      read_long_file(system->online_path[cpu], &value);
    }
    if (value != 0) {
      ++system->online_count;
    }
  }
}

static void probe_swappiness(const char *root, bd_perf_system *system) {
  char path[BD_PERF_PATH_MAX];
  long value = 0;
  if (join_path(root, "/proc/sys/vm/swappiness", path, sizeof(path)) &&
      read_long_file(path, &value)) {
    snprintf(system->swappiness_path, sizeof(system->swappiness_path), "%s",
             path);
    system->has_swappiness = true;
    system->swappiness = (int)value;
  }
}

static void probe_thermal(const char *root, bd_perf_system *system) {
  char base[BD_PERF_PATH_MAX];
  if (!join_path(root, "/sys/class/thermal", base, sizeof(base))) {
    return;
  }
  DIR *dir = opendir(base);
  if (dir == NULL) {
    return;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL &&
         system->thermal_count < BD_PERF_MAX_THERMAL) {
    if (strncmp(entry->d_name, "thermal_zone", 12) != 0) {
      continue;
    }
    char zone_dir[BD_PERF_PATH_MAX];
    if (!format_path(zone_dir, sizeof(zone_dir), "%s/%s", base,
                     entry->d_name)) {
      continue;
    }
    char path[BD_PERF_PATH_MAX];
    char type[BD_PERF_NAME_MAX + 1] = "";
    long temp = 0;
    if (format_path(path, sizeof(path), "%s/type", zone_dir)) {
      read_text_file(path, type, sizeof(type));
    }
    if (format_path(path, sizeof(path), "%s/temp", zone_dir) &&
        read_long_file(path, &temp)) {
      bd_perf_thermal *thermal = &system->thermals[system->thermal_count++];
      memset(thermal, 0, sizeof(*thermal));
      copy_text(thermal->zone, sizeof(thermal->zone), entry->d_name);
      copy_text(thermal->type, sizeof(thermal->type), type);
      thermal->temp_mc = temp;
    }
  }
  closedir(dir);
}

static void probe_devfreq(const char *root, bd_perf_system *system) {
  char base[BD_PERF_PATH_MAX];
  if (!join_path(root, "/sys/class/devfreq", base, sizeof(base))) {
    return;
  }
  DIR *dir = opendir(base);
  if (dir == NULL) {
    return;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL &&
         system->devfreq_count < BD_PERF_MAX_DEVFREQ) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    char device[BD_PERF_PATH_MAX];
    if (!format_path(device, sizeof(device), "%s/%s", base, entry->d_name)) {
      continue;
    }
    char path[BD_PERF_PATH_MAX];
    long current = 0;
    if (!format_path(path, sizeof(path), "%s/cur_freq", device) ||
        !read_long_file(path, &current)) {
      continue;
    }
    bd_perf_devfreq *devfreq = &system->devfreqs[system->devfreq_count++];
    memset(devfreq, 0, sizeof(*devfreq));
    copy_text(devfreq->name, sizeof(devfreq->name), entry->d_name);
    devfreq->cur_freq = current;
    if (format_path(path, sizeof(path), "%s/available_frequencies", device)) {
      char text[1024];
      if (read_text_file(path, text, sizeof(text))) {
        char *cursor = text;
        while (*cursor != '\0' &&
               devfreq->available_count < BD_PERF_MAX_FREQS) {
          while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
            ++cursor;
          }
          if (*cursor == '\0') {
            break;
          }
          errno = 0;
          char *end = NULL;
          long value = strtol(cursor, &end, 10);
          if (errno != 0 || end == cursor) {
            break;
          }
          if (value > 0) {
            devfreq->available[devfreq->available_count++] = value;
          }
          cursor = end;
        }
        devfreq->has_available = devfreq->available_count > 0;
      }
    }
  }
  closedir(dir);
}

bool bd_perf_system_probe(const char *root, bd_perf_system *system,
                          char *error, size_t error_size) {
  if (system == NULL) {
    set_error(error, error_size, "system is NULL");
    return false;
  }
  memset(system, 0, sizeof(*system));
  if (root == NULL) {
    root = "";
  }
  if (strlen(root) >= sizeof(system->root)) {
    set_error(error, error_size, "sysfs root path is too long");
    return false;
  }
  snprintf(system->root, sizeof(system->root), "%s", root);

  probe_cpufreq(root, system);
  probe_cpus(root, system);
  probe_swappiness(root, system);
  probe_thermal(root, system);
  probe_devfreq(root, system);
  return true;
}

static bool governor_available(const bd_perf_system *system,
                               const char *governor) {
  for (size_t i = 0; i < system->governor_count; ++i) {
    if (strcmp(system->governors[i], governor) == 0) {
      return true;
    }
  }
  return false;
}

static int clamp_khz(const bd_perf_system *system, int khz) {
  if (system->cpuinfo_min_khz > 0 && khz < system->cpuinfo_min_khz) {
    khz = system->cpuinfo_min_khz;
  }
  if (system->cpuinfo_max_khz > 0 && khz > system->cpuinfo_max_khz) {
    khz = system->cpuinfo_max_khz;
  }
  return khz;
}

static bool snap_khz(const bd_perf_system *system, int khz, int *out) {
  bool found = false;
  int best = 0;
  for (size_t i = 0; i < system->freq_count; ++i) {
    if (system->freqs[i] <= khz &&
        (!found || system->freqs[i] > best)) {
      best = system->freqs[i];
      found = true;
    }
  }
  if (found) {
    *out = best;
  }
  return found;
}

bool bd_perf_plan_build(const bd_perf_system *system,
                        const bd_perf_limits *limits, bd_perf_plan *plan,
                        char *error, size_t error_size) {
  if (system == NULL || limits == NULL || plan == NULL) {
    set_error(error, error_size, "invalid plan arguments");
    return false;
  }
  memset(plan, 0, sizeof(*plan));
  snprintf(plan->profile, sizeof(plan->profile), "%s", limits->profile);
  snprintf(plan->app, sizeof(plan->app), "%s", limits->app);
  snprintf(plan->core, sizeof(plan->core), "%s", limits->core);
  snprintf(plan->current_governor, sizeof(plan->current_governor), "%s",
           system->current_governor);
  plan->current_min_khz = system->current_min_khz;
  plan->current_max_khz = system->current_max_khz;
  plan->current_online_cores = system->online_count;
  plan->current_swappiness = system->swappiness;

  if (!system->cpufreq) {
    set_error(error, error_size,
              "cpufreq policy not found under the sysfs root");
    return false;
  }
  if (system->current_governor[0] == '\0') {
    set_error(error, error_size, "cannot read scaling_governor");
    return false;
  }

  if (limits->has_governor) {
    const char *requested = limits->governor;
    if (requested == NULL || requested[0] == '\0') {
      set_error(error, error_size, "governor is empty");
      return false;
    }
    snprintf(plan->governor, sizeof(plan->governor), "%s", requested);
    if (!governor_available(system, plan->governor)) {
      bool fell_back = false;
      if (strcmp(requested, "schedutil") == 0) {
        static const char *const fallback[] = {"ondemand", "performance"};
        for (size_t i = 0; i < sizeof(fallback) / sizeof(fallback[0]); ++i) {
          if (governor_available(system, fallback[i])) {
            snprintf(plan->governor, sizeof(plan->governor), "%s",
                     fallback[i]);
            fell_back = true;
            break;
          }
        }
      }
      if (!fell_back) {
        char available[256] = "";
        size_t used = 0;
        for (size_t i = 0; i < system->governor_count; ++i) {
          int written =
              snprintf(available + used, sizeof(available) - used, "%s%s",
                       i == 0 ? "" : ", ", system->governors[i]);
          if (written <= 0 || (size_t)written >= sizeof(available) - used) {
            break;
          }
          used += (size_t)written;
        }
        set_error(error, error_size,
                  "governor '%s' is not available (available: %s)", requested,
                  available[0] != '\0' ? available : "none");
        return false;
      }
      plan->governor_fallback = true;
    }
    plan->governor_change =
        strcmp(plan->governor, system->current_governor) != 0;
  } else {
    snprintf(plan->governor, sizeof(plan->governor), "%s",
             system->current_governor);
    plan->governor_change = false;
  }

  bool min_requested = limits->has_min_khz && limits->min_khz > 0;
  bool max_requested = limits->has_max_khz && limits->max_khz > 0;
  if ((limits->has_min_khz && limits->min_khz < 0) ||
      (limits->has_max_khz && limits->max_khz < 0)) {
    set_error(error, error_size, "frequency must not be negative");
    return false;
  }
  int target_min = min_requested ? limits->min_khz : system->current_min_khz;
  int target_max = max_requested ? limits->max_khz : system->current_max_khz;

  if (!min_requested && !max_requested) {
    /* Governor-only profiles must not rewrite frequency limits just because
     * the running values are not present in the advertised table. */
    plan->min_khz = target_min;
    plan->max_khz = target_max;
    plan->min_change = false;
    plan->max_change = false;
  } else {
    if (target_min <= 0 || target_max <= 0) {
      set_error(error, error_size, "cannot determine current frequency limits");
      return false;
    }
    if (target_min > target_max) {
      set_error(error, error_size,
                "min_khz %d is greater than max_khz %d", target_min,
                target_max);
      return false;
    }
    int clamped_min = min_requested ? clamp_khz(system, target_min) : target_min;
    int clamped_max = max_requested ? clamp_khz(system, target_max) : target_max;
    if (clamped_min > clamped_max) {
      set_error(error, error_size,
                "clamped min_khz %d is greater than clamped max_khz %d",
                clamped_min, clamped_max);
      return false;
    }
    if (min_requested) {
      if (!snap_khz(system, clamped_min, &plan->min_khz)) {
        set_error(error, error_size,
                  "no available frequency at or below min_khz %d",
                  clamped_min);
        return false;
      }
    } else {
      plan->min_khz = target_min;
    }
    if (max_requested) {
      if (!snap_khz(system, clamped_max, &plan->max_khz)) {
        set_error(error, error_size,
                  "no available frequency at or below max_khz %d",
                  clamped_max);
        return false;
      }
    } else {
      plan->max_khz = target_max;
    }
    if (plan->min_khz > plan->max_khz) {
      set_error(error, error_size,
                "min_khz %d is greater than max_khz %d after frequency lookup",
                plan->min_khz, plan->max_khz);
      return false;
    }
    plan->min_change = plan->min_khz != system->current_min_khz;
    plan->max_change = plan->max_khz != system->current_max_khz;
  }
  if (plan->governor_change && system->governor_path[0] == '\0') {
    set_error(error, error_size, "scaling_governor path is not available");
    return false;
  }
  if (plan->min_change && system->min_path[0] == '\0') {
    set_error(error, error_size, "scaling_min_freq path is not available");
    return false;
  }
  if (plan->max_change && system->max_path[0] == '\0') {
    set_error(error, error_size, "scaling_max_freq path is not available");
    return false;
  }

  if (limits->has_online_cores) {
    int target_online = limits->online_cores;
    if (target_online < 2 || target_online > 4) {
      set_error(error, error_size,
                "online_cores %d is outside the supported range 2..4",
                target_online);
      return false;
    }
    if (target_online > system->cpu_count) {
      set_error(error, error_size,
                "online_cores %d exceeds the %d detected CPUs", target_online,
                system->cpu_count);
      return false;
    }
    if (target_online != system->online_count) {
      for (int cpu = 1; cpu < target_online; ++cpu) {
        if (system->online_path[cpu][0] == '\0') {
          set_error(error, error_size,
                    "cpu%d has no online control file", cpu);
          return false;
        }
      }
      for (int cpu = target_online; cpu < system->cpu_count; ++cpu) {
        if (system->online_path[cpu][0] == '\0') {
          set_error(error, error_size,
                    "cpu%d has no online control file", cpu);
          return false;
        }
      }
    }
    plan->online_cores = target_online;
    plan->online_change = target_online != system->online_count;
  } else {
    plan->online_cores = system->online_count;
    plan->online_change = false;
  }

  if (limits->has_swappiness) {
    if (limits->swappiness < 0 || limits->swappiness > 100) {
      set_error(error, error_size, "swappiness %d is outside 0..100",
                limits->swappiness);
      return false;
    }
    if (system->has_swappiness) {
      plan->swappiness = limits->swappiness;
      plan->swappiness_change =
          plan->swappiness != system->swappiness;
    } else {
      plan->swappiness_unsupported = true;
    }
  }
  return true;
}

static bool write_text_file(const char *path, const char *text, char *error,
                            size_t error_size) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    set_error(error, error_size, "%s: %s", path, strerror(errno));
    return false;
  }
  bool ok = fputs(text, file) >= 0;
  if (ok) {
    ok = fputc('\n', file) != EOF;
  }
  if (fclose(file) != 0) {
    ok = false;
  }
  if (!ok) {
    set_error(error, error_size, "%s: write failed", path);
  }
  return ok;
}

static bool write_string_verified(const char *path, const char *value,
                                  char *error, size_t error_size) {
  if (!write_text_file(path, value, error, error_size)) {
    return false;
  }
  char actual[BD_PERF_GOVERNOR_MAX + 1];
  if (!read_text_file(path, actual, sizeof(actual)) ||
      strcmp(actual, value) != 0) {
    set_error(error, error_size, "%s: value did not stick", path);
    return false;
  }
  return true;
}

static bool write_long_verified(const char *path, int value, char *error,
                                size_t error_size) {
  char text[32];
  snprintf(text, sizeof(text), "%d", value);
  if (!write_text_file(path, text, error, error_size)) {
    return false;
  }
  long actual = 0;
  if (!read_long_file(path, &actual) || actual != value) {
    set_error(error, error_size, "%s: value did not stick", path);
    return false;
  }
  return true;
}

typedef enum write_kind {
  WRITE_GOVERNOR = 0,
  WRITE_MIN,
  WRITE_MAX,
  WRITE_ONLINE,
  WRITE_SWAPPINESS
} write_kind;

typedef struct write_step {
  write_kind kind;
  int cpu;
  int old_number;
  char old_text[BD_PERF_GOVERNOR_MAX + 1];
} write_step;

typedef struct write_log {
  write_step steps[BD_PERF_MAX_WRITE_STEPS];
  size_t count;
} write_log;

static bool log_step(write_log *log, write_kind kind, int cpu,
                     const char *old_text, int old_number) {
  if (log->count >= BD_PERF_MAX_WRITE_STEPS) {
    return false;
  }
  write_step *step = &log->steps[log->count++];
  memset(step, 0, sizeof(*step));
  step->kind = kind;
  step->cpu = cpu;
  step->old_number = old_number;
  if (old_text != NULL) {
    snprintf(step->old_text, sizeof(step->old_text), "%s", old_text);
  }
  return true;
}

static bool rollback_log(const bd_perf_system *system, const write_log *log,
                         char *error, size_t error_size) {
  bool ok = true;
  char first_error[256] = "";
  for (size_t i = log->count; i > 0; --i) {
    const write_step *step = &log->steps[i - 1];
    char local_error[256] = "";
    bool restored = false;
    switch (step->kind) {
      case WRITE_GOVERNOR:
        restored = write_string_verified(system->governor_path,
                                          step->old_text, local_error,
                                          sizeof(local_error));
        break;
      case WRITE_MIN:
        restored = write_long_verified(system->min_path, step->old_number,
                                        local_error, sizeof(local_error));
        break;
      case WRITE_MAX:
        restored = write_long_verified(system->max_path, step->old_number,
                                        local_error, sizeof(local_error));
        break;
      case WRITE_ONLINE:
        if (step->cpu > 0 && step->cpu < BD_PERF_MAX_CPUS &&
            system->online_path[step->cpu][0] != '\0') {
          restored = write_long_verified(system->online_path[step->cpu],
                                          step->old_number, local_error,
                                          sizeof(local_error));
        }
        break;
      case WRITE_SWAPPINESS:
        if (system->has_swappiness) {
          restored = write_long_verified(system->swappiness_path,
                                          step->old_number, local_error,
                                          sizeof(local_error));
        } else {
          restored = true;
        }
        break;
    }
    if (!restored && ok) {
      ok = false;
      snprintf(first_error, sizeof(first_error), "%s", local_error);
    }
  }
  if (!ok) {
    set_error(error, error_size, "rollback failed: %s", first_error);
  }
  return ok;
}

static bool read_online(const bd_perf_system *system, int cpu, int *online) {
  if (cpu <= 0 || cpu >= BD_PERF_MAX_CPUS ||
      system->online_path[cpu][0] == '\0') {
    return false;
  }
  long value = 0;
  if (!read_long_file(system->online_path[cpu], &value)) {
    return false;
  }
  *online = value != 0;
  return true;
}

static bool set_online(const bd_perf_system *system, int cpu, int wanted,
                       write_log *log, char *error, size_t error_size) {
  int current = 0;
  if (!read_online(system, cpu, &current)) {
    set_error(error, error_size, "cannot read cpu%d online state", cpu);
    return false;
  }
  if (current == wanted) {
    return true;
  }
  if (!log_step(log, WRITE_ONLINE, cpu, NULL, current)) {
    set_error(error, error_size, "transaction log is full");
    return false;
  }
  return write_long_verified(system->online_path[cpu], wanted, error,
                             error_size);
}

bool bd_perf_apply(const bd_perf_system *system, const bd_perf_plan *plan,
                   bd_perf_apply_report *report, char *error,
                   size_t error_size) {
  if (report != NULL) {
    memset(report, 0, sizeof(*report));
  }
  if (system == NULL || plan == NULL) {
    set_error(error, error_size, "invalid apply arguments");
    return false;
  }
  if (!system->cpufreq) {
    set_error(error, error_size, "cpufreq policy not available");
    return false;
  }

  write_log log;
  memset(&log, 0, sizeof(log));
  bool ok = true;

  if (plan->governor_change) {
    if (!log_step(&log, WRITE_GOVERNOR, 0, plan->current_governor, 0)) {
      set_error(error, error_size, "transaction log is full");
      return false;
    }
    ok = write_string_verified(system->governor_path, plan->governor, error,
                               error_size);
  }

  if (ok && (plan->min_change || plan->max_change)) {
    bool write_min_first =
        plan->max_change && plan->min_change &&
        plan->max_khz < plan->current_min_khz;
    if (write_min_first) {
      if (!log_step(&log, WRITE_MIN, 0, NULL, plan->current_min_khz)) {
        ok = false;
        set_error(error, error_size, "transaction log is full");
      } else {
        ok = write_long_verified(system->min_path, plan->min_khz, error,
                                 error_size);
      }
      if (ok && plan->max_change) {
        if (!log_step(&log, WRITE_MAX, 0, NULL, plan->current_max_khz)) {
          ok = false;
          set_error(error, error_size, "transaction log is full");
        } else {
          ok = write_long_verified(system->max_path, plan->max_khz, error,
                                   error_size);
        }
      }
    } else {
      if (plan->max_change) {
        if (!log_step(&log, WRITE_MAX, 0, NULL, plan->current_max_khz)) {
          ok = false;
          set_error(error, error_size, "transaction log is full");
        } else {
          ok = write_long_verified(system->max_path, plan->max_khz, error,
                                   error_size);
        }
      }
      if (ok && plan->min_change) {
        if (!log_step(&log, WRITE_MIN, 0, NULL, plan->current_min_khz)) {
          ok = false;
          set_error(error, error_size, "transaction log is full");
        } else {
          ok = write_long_verified(system->min_path, plan->min_khz, error,
                                   error_size);
        }
      }
    }
  }

  if (ok && plan->online_change) {
    for (int cpu = system->cpu_count - 1; cpu >= plan->online_cores; --cpu) {
      if (!set_online(system, cpu, 0, &log, error, error_size)) {
        ok = false;
        break;
      }
    }
    if (ok) {
      for (int cpu = 1; cpu < plan->online_cores; ++cpu) {
        if (!set_online(system, cpu, 1, &log, error, error_size)) {
          ok = false;
          break;
        }
      }
    }
  }

  if (ok && plan->swappiness_change) {
    if (!system->has_swappiness) {
      ok = false;
      set_error(error, error_size, "swappiness is not available");
    } else if (!log_step(&log, WRITE_SWAPPINESS, 0, NULL,
                         plan->current_swappiness)) {
      ok = false;
      set_error(error, error_size, "transaction log is full");
    } else {
      ok = write_long_verified(system->swappiness_path, plan->swappiness,
                               error, error_size);
    }
  }

  if (!ok) {
    char apply_error[256];
    snprintf(apply_error, sizeof(apply_error), "%s",
             error != NULL ? error : "write failed");
    if (report != NULL) {
      report->writes = (int)log.count;
      report->rolled_back = log.count > 0;
    }
    if (log.count > 0) {
      char rollback_error[256] = "";
      if (!rollback_log(system, &log, rollback_error,
                        sizeof(rollback_error))) {
        set_error(error, error_size, "%s (rollback failed: %s)", apply_error,
                  rollback_error);
      } else {
        set_error(error, error_size, "%s", apply_error);
      }
    } else {
      set_error(error, error_size, "%s", apply_error);
    }
    return false;
  }

  if (report != NULL) {
    report->changed = plan->governor_change || plan->min_change ||
                      plan->max_change || plan->online_change ||
                      plan->swappiness_change;
    report->writes = (int)log.count;
  }
  return true;
}

bool bd_perf_snapshot_take(const bd_perf_system *system,
                           bd_perf_snapshot *snapshot) {
  if (system == NULL || snapshot == NULL) {
    return false;
  }
  memset(snapshot, 0, sizeof(*snapshot));
  for (size_t i = 0; i < BD_PERF_MAX_CPUS; ++i) {
    snapshot->online[i] = -1;
  }
  if (system->cpufreq) {
    if (system->current_governor[0] != '\0') {
      snapshot->has_governor = true;
      snprintf(snapshot->governor, sizeof(snapshot->governor), "%s",
               system->current_governor);
    }
    if (system->current_min_khz > 0) {
      snapshot->has_min = true;
      snapshot->min_khz = system->current_min_khz;
    }
    if (system->current_max_khz > 0) {
      snapshot->has_max = true;
      snapshot->max_khz = system->current_max_khz;
    }
  }
  for (int cpu = 1; cpu < system->cpu_count; ++cpu) {
    int online = 0;
    if (read_online(system, cpu, &online)) {
      snapshot->online[cpu] = online;
    }
  }
  if (system->has_swappiness) {
    snapshot->has_swappiness = true;
    snapshot->swappiness = system->swappiness;
  }
  return true;
}

static bool restore_limits(const bd_perf_system *system, int old_min,
                           int old_max, char *error, size_t error_size) {
  long current_min = 0;
  long current_max = 0;
  if (!read_long_file(system->min_path, &current_min) ||
      !read_long_file(system->max_path, &current_max)) {
    current_min = old_min;
    current_max = old_max;
  }
  if (old_max < current_min) {
    if (!write_long_verified(system->min_path, old_min, error, error_size)) {
      return false;
    }
    return write_long_verified(system->max_path, old_max, error, error_size);
  }
  if (old_min > current_max) {
    if (!write_long_verified(system->max_path, old_max, error, error_size)) {
      return false;
    }
    return write_long_verified(system->min_path, old_min, error, error_size);
  }
  if (!write_long_verified(system->max_path, old_max, error, error_size)) {
    return false;
  }
  return write_long_verified(system->min_path, old_min, error, error_size);
}

bool bd_perf_snapshot_restore(const bd_perf_system *system,
                              const bd_perf_snapshot *snapshot, char *error,
                              size_t error_size) {
  if (system == NULL || snapshot == NULL) {
    set_error(error, error_size, "invalid snapshot");
    return false;
  }
  bool ok = true;
  char first_error[256] = "";
  char local_error[256];

  if (snapshot->has_governor && system->governor_path[0] != '\0') {
    if (!write_string_verified(system->governor_path, snapshot->governor,
                               local_error, sizeof(local_error))) {
      ok = false;
      snprintf(first_error, sizeof(first_error), "%s", local_error);
    }
  }

  if (snapshot->has_min && snapshot->has_max &&
      system->min_path[0] != '\0' && system->max_path[0] != '\0') {
    if (!restore_limits(system, snapshot->min_khz, snapshot->max_khz,
                        local_error, sizeof(local_error))) {
      if (ok) {
        ok = false;
        snprintf(first_error, sizeof(first_error), "%s", local_error);
      }
    }
  }

  for (int cpu = system->cpu_count - 1; cpu >= 1; --cpu) {
    if (snapshot->online[cpu] != 0) {
      continue;
    }
    if (system->online_path[cpu][0] == '\0' ||
        !write_long_verified(system->online_path[cpu], 0, local_error,
                             sizeof(local_error))) {
      if (ok) {
        ok = false;
        snprintf(first_error, sizeof(first_error), "%s",
                 system->online_path[cpu][0] == '\0'
                     ? "missing cpu online path"
                     : local_error);
      }
    }
  }
  for (int cpu = 1; cpu < system->cpu_count; ++cpu) {
    if (snapshot->online[cpu] != 1) {
      continue;
    }
    if (system->online_path[cpu][0] == '\0' ||
        !write_long_verified(system->online_path[cpu], 1, local_error,
                             sizeof(local_error))) {
      if (ok) {
        ok = false;
        snprintf(first_error, sizeof(first_error), "%s",
                 system->online_path[cpu][0] == '\0'
                     ? "missing cpu online path"
                     : local_error);
      }
    }
  }

  if (snapshot->has_swappiness && system->has_swappiness) {
    if (!write_long_verified(system->swappiness_path, snapshot->swappiness,
                             local_error, sizeof(local_error))) {
      if (ok) {
        ok = false;
        snprintf(first_error, sizeof(first_error), "%s", local_error);
      }
    }
  }

  if (!ok) {
    set_error(error, error_size, "%s", first_error);
  }
  return ok;
}
