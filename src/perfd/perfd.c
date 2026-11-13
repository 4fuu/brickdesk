#define _POSIX_C_SOURCE 200809L

#include "brickdesk/perf.h"

#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define BD_PERFD_MAX_CLIENTS 8
#define BD_PERFD_LINE_MAX 4096
#define BD_PERFD_OUT_MAX 8192

typedef struct perfd_client {
  int fd;
  int id;
  char input[BD_PERFD_LINE_MAX];
  size_t input_length;
} perfd_client;

typedef struct perfd {
  bd_perf_policy policy;
  bd_perf_system system;
  bd_perf_snapshot snapshot;
  bool have_snapshot;
  bd_perf_lease_table leases;
  char sysfs_root[BD_PERF_PATH_MAX];
  char global_profile[BD_PERF_NAME_MAX + 1];
  char applied_key[3 * (BD_PERF_NAME_MAX + 1) + 4];
  char applied_profile[BD_PERF_NAME_MAX + 1];
  int next_client_id;
  uint64_t auto_next_ms;
  int auto_low_state; /* -1 unknown, 0 normal, 1 low-battery throttle */

  perfd_client clients[BD_PERFD_MAX_CLIENTS];
  size_t client_count;
  int listen_fd;
  int lock_fd;
  char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  bool socket_owned;
  bool verbose;
  bool running;
} perfd;

typedef struct perf_request {
  char profile[BD_PERF_NAME_MAX + 1];
  char app[BD_PERF_NAME_MAX + 1];
  char core[BD_PERF_NAME_MAX + 1];
} perf_request;

static volatile sig_atomic_t g_stop;

static void on_signal(int signal_number) {
  (void)signal_number;
  g_stop = 1;
}

static uint64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void log_message(const perfd *daemon, const char *format, ...) {
  (void)daemon;
  va_list args;
  va_start(args, format);
  fputs("brickdesk-perfd: ", stderr);
  vfprintf(stderr, format, args);
  fputc('\n', stderr);
  va_end(args);
}

static void json_escape(char *dst, size_t dst_size, const char *src) {
  if (dst_size == 0) {
    return;
  }
  size_t used = 0;
  if (src == NULL) {
    src = "";
  }
  for (const unsigned char *p = (const unsigned char *)src;
       *p != '\0' && used + 1 < dst_size; ++p) {
    if (*p == '"' || *p == '\\') {
      if (used + 2 >= dst_size) {
        break;
      }
      dst[used++] = '\\';
      dst[used++] = (char)*p;
    } else if (*p < 0x20) {
      if (used + 6 >= dst_size) {
        break;
      }
      int written = snprintf(dst + used, dst_size - used, "\\u%04x", *p);
      if (written < 0) {
        break;
      }
      used += (size_t)written;
    } else {
      dst[used++] = (char)*p;
    }
  }
  dst[used] = '\0';
}

static bool appendf(char *buffer, size_t size, size_t *used,
                    const char *format, ...) {
  if (*used >= size) {
    return false;
  }
  va_list args;
  va_start(args, format);
  int written = vsnprintf(buffer + *used, size - *used, format, args);
  va_end(args);
  if (written < 0 || (size_t)written >= size - *used) {
    return false;
  }
  *used += (size_t)written;
  return true;
}

static void client_close(perfd_client *client) {
  if (client != NULL && client->fd >= 0) {
    close(client->fd);
    client->fd = -1;
  }
}

static bool client_send_line(perfd_client *client, const char *line) {
  if (client == NULL || client->fd < 0 || line == NULL) {
    return false;
  }
  size_t length = strlen(line);
  char buffer[BD_PERFD_OUT_MAX];
  if (length + 1 >= sizeof(buffer)) {
    return false;
  }
  memcpy(buffer, line, length);
  buffer[length] = '\n';
  ssize_t sent = send(client->fd, buffer, length + 1, MSG_NOSIGNAL);
  if (sent != (ssize_t)(length + 1)) {
    client_close(client);
    return false;
  }
  return true;
}

static bool client_sendf(perfd_client *client, const char *format, ...) {
  char line[BD_PERFD_OUT_MAX];
  va_list args;
  va_start(args, format);
  int written = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (written < 0 || (size_t)written >= sizeof(line)) {
    return false;
  }
  return client_send_line(client, line);
}

static void client_compact(perfd *daemon) {
  size_t write_index = 0;
  for (size_t i = 0; i < daemon->client_count; ++i) {
    if (daemon->clients[i].fd < 0) {
      continue;
    }
    if (write_index != i) {
      daemon->clients[write_index] = daemon->clients[i];
    }
    ++write_index;
  }
  daemon->client_count = write_index;
}

static void send_error(perfd_client *client, long id, int code,
                       const char *message) {
  char escaped[512];
  json_escape(escaped, sizeof(escaped), message);
  client_sendf(client,
               "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"error\":{\"code\":%d,"
               "\"message\":\"%s\"}}",
               id, code, escaped);
}

static bool json_field_string(const char *json, const char *field, char *out,
                              size_t out_size) {
  char pattern[64];
  int written = snprintf(pattern, sizeof(pattern), "\"%s\"", field);
  if (written <= 0 || (size_t)written >= sizeof(pattern)) {
    return false;
  }
  const char *cursor = strstr(json, pattern);
  if (cursor == NULL) {
    return false;
  }
  cursor += strlen(pattern);
  while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
    ++cursor;
  }
  if (*cursor != ':') {
    return false;
  }
  ++cursor;
  while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
    ++cursor;
  }
  if (*cursor != '"') {
    return false;
  }
  ++cursor;
  size_t used = 0;
  while (*cursor != '\0' && *cursor != '"' && used + 1 < out_size) {
    if (*cursor == '\\' && cursor[1] != '\0') {
      ++cursor;
      if (*cursor == 'n') {
        out[used++] = '\n';
        ++cursor;
        continue;
      }
      if (*cursor == 'r') {
        out[used++] = '\r';
        ++cursor;
        continue;
      }
      if (*cursor == 't') {
        out[used++] = '\t';
        ++cursor;
        continue;
      }
    }
    out[used++] = *cursor++;
  }
  out[used] = '\0';
  return true;
}

static bool json_field_present(const char *json, const char *field) {
  char pattern[64];
  int written = snprintf(pattern, sizeof(pattern), "\"%s\"", field);
  return written > 0 && (size_t)written < sizeof(pattern) &&
         strstr(json, pattern) != NULL;
}

static bool json_field_long(const char *json, const char *field, long *out) {
  char pattern[64];
  int written = snprintf(pattern, sizeof(pattern), "\"%s\"", field);
  if (written <= 0 || (size_t)written >= sizeof(pattern)) {
    return false;
  }
  const char *cursor = strstr(json, pattern);
  if (cursor == NULL) {
    return false;
  }
  cursor += strlen(pattern);
  while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
    ++cursor;
  }
  if (*cursor != ':') {
    return false;
  }
  ++cursor;
  while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
    ++cursor;
  }
  errno = 0;
  char *end = NULL;
  long value = strtol(cursor, &end, 10);
  if (errno != 0 || end == cursor) {
    return false;
  }
  *out = value;
  return true;
}

static void request_key(const perf_request *request, char *out,
                        size_t out_size) {
  snprintf(out, out_size, "%s|%s|%s", request->profile, request->app,
           request->core);
}

static void parse_request(const char *line, perf_request *request) {
  memset(request, 0, sizeof(*request));
  json_field_string(line, "profile", request->profile,
                    sizeof(request->profile));
  json_field_string(line, "app", request->app, sizeof(request->app));
  json_field_string(line, "core", request->core, sizeof(request->core));
}

static void effective_request(const perfd *daemon, perf_request *request);
static void auto_power_state(const perfd *daemon, bool *charging,
                             int *capacity);

static bool daemon_apply(perfd *daemon, const perf_request *request,
                         bd_perf_plan *plan, char *error,
                         size_t error_size) {
  bd_perf_limits limits;
  if (!bd_perf_policy_resolve(&daemon->policy, request->profile, request->app,
                              request->core, &limits, error, error_size)) {
    return false;
  }

  bd_perf_system system;
  if (!bd_perf_system_probe(daemon->sysfs_root, &system, error, error_size)) {
    return false;
  }
  if (!bd_perf_plan_build(&system, &limits, plan, error, error_size)) {
    return false;
  }

  /*
   * Low-battery throttle: below 10% on battery, cap the requested maximum at
   * the highest step <= 816 MHz.  This runs for every profile, including an
   * explicit performance lease, and never raises a lower user limit.
   */
  bool charging = false;
  int capacity = -1;
  auto_power_state(daemon, &charging, &capacity);
  bool battery_capped = false;
  if (!charging && capacity >= 0 && capacity <= 10 && system.cpufreq) {
    int cap = system.cpuinfo_min_khz > 0 ? system.cpuinfo_min_khz : 816000;
    for (size_t i = 0; i < system.freq_count; ++i) {
      if (system.freqs[i] <= 816000 && system.freqs[i] > cap) {
        cap = system.freqs[i];
      }
    }
    if (plan->max_khz > cap) {
      plan->max_khz = cap;
      if (plan->min_khz > cap) {
        plan->min_khz = cap;
      }
      plan->max_change = plan->max_khz != plan->current_max_khz;
      plan->min_change = plan->min_khz != plan->current_min_khz;
      battery_capped = true;
    }
  }

  bd_perf_apply_report report;
  if (!bd_perf_apply(&system, plan, &report, error, error_size)) {
    return false;
  }
  daemon->system = system;
  snprintf(daemon->system.current_governor,
           sizeof(daemon->system.current_governor), "%s", plan->governor);
  daemon->system.current_min_khz = plan->min_khz;
  daemon->system.current_max_khz = plan->max_khz;
  daemon->system.online_count = plan->online_cores;
  if (plan->swappiness_change) {
    daemon->system.swappiness = plan->swappiness;
  }
  snprintf(daemon->applied_profile, sizeof(daemon->applied_profile), "%s",
           plan->profile);
  if (battery_capped) {
    log_message(daemon, "low battery (%d%%): max capped to %d kHz", capacity,
                plan->max_khz);
  }
  log_message(daemon,
              "applied profile=%s app=%s core=%s governor %s->%s "
              "min %d->%d max %d->%d cores %d->%d swappiness %d->%d",
              plan->profile, plan->app, plan->core, plan->current_governor,
              plan->governor, plan->current_min_khz, plan->min_khz,
              plan->current_max_khz, plan->max_khz,
              plan->current_online_cores, plan->online_cores,
              plan->current_swappiness,
              plan->swappiness_change ? plan->swappiness
                                      : plan->current_swappiness);
  return true;
}

static void build_root_path(const perfd *daemon, const char *relative,
                            char *out, size_t out_size) {
  if (out == NULL || out_size == 0) {
    return;
  }
  const char *root = daemon->sysfs_root;
  if (root == NULL || root[0] == '\0') {
    snprintf(out, out_size, "%s", relative);
    return;
  }
  size_t root_length = strlen(root);
  size_t relative_length = strlen(relative);
  bool slash = root_length > 0 && root[root_length - 1] == '/';
  size_t needed = root_length + (slash ? 0u : 1u) + relative_length;
  if (needed >= out_size) {
    out[0] = '\0';
    return;
  }
  memcpy(out, root, root_length);
  size_t used = root_length;
  if (!slash) {
    out[used++] = '/';
  }
  memcpy(out + used, relative, relative_length + 1);
}

static bool read_root_long(const perfd *daemon, const char *relative,
                           long *value) {
  char path[BD_PERF_PATH_MAX];
  build_root_path(daemon, relative, path, sizeof(path));
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  char text[64];
  bool ok = fgets(text, sizeof(text), file) != NULL;
  fclose(file);
  if (!ok) {
    return false;
  }
  errno = 0;
  char *end = NULL;
  long parsed = strtol(text, &end, 10);
  if (errno != 0 || end == text) {
    return false;
  }
  *value = parsed;
  return true;
}

/*
 * NextUI's auto policy follows external power and cuts to powersave on a low
 * battery.  brickdesk applies the user's existing performance/powersave
 * profiles instead of hard-coded frequencies, so custom perf.ini steps still
 * win; when a profile is missing we fall back to "default" or the first entry.
 */
static void auto_power_state(const perfd *daemon, bool *charging,
                             int *capacity) {
  *charging = false;
  *capacity = -1;
  long value = 0;
  if (read_root_long(daemon, "sys/class/power_supply/axp2202-usb/online",
                     &value)) {
    *charging = value != 0;
  } else {
    char base[BD_PERF_PATH_MAX];
    build_root_path(daemon, "sys/class/power_supply", base, sizeof(base));
    DIR *dir = opendir(base);
    if (dir != NULL) {
      struct dirent *entry;
      while ((entry = readdir(dir)) != NULL) {
        char relative[BD_PERF_PATH_MAX];
        snprintf(relative, sizeof(relative),
                 "sys/class/power_supply/%s/online", entry->d_name);
        if (read_root_long(daemon, relative, &value) && value != 0) {
          *charging = true;
          break;
        }
      }
      closedir(dir);
    }
  }
  if (read_root_long(daemon, "sys/class/power_supply/axp2202-battery/capacity",
                     &value) &&
      value >= 0 && value <= 100) {
    *capacity = (int)value;
  } else {
    char base[BD_PERF_PATH_MAX];
    build_root_path(daemon, "sys/class/power_supply", base, sizeof(base));
    DIR *dir = opendir(base);
    if (dir != NULL) {
      struct dirent *entry;
      while ((entry = readdir(dir)) != NULL) {
        char relative[BD_PERF_PATH_MAX];
        snprintf(relative, sizeof(relative),
                 "sys/class/power_supply/%s/capacity", entry->d_name);
        if (read_root_long(daemon, relative, &value) && value >= 0 &&
            value <= 100) {
          *capacity = (int)value;
          break;
        }
      }
      closedir(dir);
    }
  }
}

static void effective_request(const perfd *daemon, perf_request *request) {
  memset(request, 0, sizeof(*request));
  const bd_perf_lease *lease =
      bd_perf_leases_effective(&daemon->leases);
  if (lease != NULL) {
    snprintf(request->profile, sizeof(request->profile), "%s",
             lease->profile);
    snprintf(request->app, sizeof(request->app), "%s", lease->app);
    snprintf(request->core, sizeof(request->core), "%s", lease->core);
  } else {
    if (strcmp(daemon->global_profile, "auto") == 0 &&
        bd_perf_policy_find_profile(&daemon->policy, "auto") != NULL) {
      snprintf(request->profile, sizeof(request->profile), "auto");
    } else {
      snprintf(request->profile, sizeof(request->profile), "%s",
               daemon->global_profile);
    }
  }
}

static void recompute_effective(perfd *daemon, uint64_t now_ms,
                                const char *reason) {
  (void)now_ms;
  perf_request request;
  effective_request(daemon, &request);
  char key[sizeof(daemon->applied_key)];
  request_key(&request, key, sizeof(key));
  if (strcmp(key, daemon->applied_key) == 0) {
    return;
  }

  bd_perf_plan plan;
  char error[256] = "";
  if (!daemon_apply(daemon, &request, &plan, error, sizeof(error))) {
    log_message(daemon, "%s: cannot apply profile '%s': %s", reason,
                request.profile[0] != '\0' ? request.profile : "default",
                error);
    return;
  }
  snprintf(daemon->applied_key, sizeof(daemon->applied_key), "%s", key);
}

static void drop_client(perfd *daemon, perfd_client *client) {
  int owner = client->id;
  client_close(client);
  if (bd_perf_leases_drop_owner(&daemon->leases, owner) > 0) {
    recompute_effective(daemon, monotonic_ms(), "client disconnect");
  }
}

static bool format_plan_result(const bd_perf_plan *plan, uint64_t ttl_ms,
                               char *out, size_t out_size) {
  char profile[BD_PERF_NAME_MAX * 2 + 1];
  char app[BD_PERF_NAME_MAX * 2 + 1];
  char core[BD_PERF_NAME_MAX * 2 + 1];
  char governor[BD_PERF_GOVERNOR_MAX * 2 + 1];
  json_escape(profile, sizeof(profile), plan->profile);
  json_escape(app, sizeof(app), plan->app);
  json_escape(core, sizeof(core), plan->core);
  json_escape(governor, sizeof(governor), plan->governor);
  int swappiness = plan->swappiness_change ? plan->swappiness
                                            : plan->current_swappiness;
  int written = snprintf(
      out, out_size,
      "{\"profile\":\"%s\",\"app\":\"%s\",\"core\":\"%s\","
      "\"governor\":\"%s\",\"min_khz\":%d,\"max_khz\":%d,"
      "\"online_cores\":%d,\"swappiness\":%d,"
      "\"swappiness_supported\":%s,\"governor_fallback\":%s,"
      "\"ttl_ms\":%llu}",
      profile, app, core, governor, plan->min_khz, plan->max_khz,
      plan->online_cores, swappiness,
      plan->swappiness_unsupported ? "false" : "true",
      plan->governor_fallback ? "true" : "false",
      (unsigned long long)ttl_ms);
  return written > 0 && (size_t)written < out_size;
}

static void handle_apply(perfd *daemon, perfd_client *client, long id,
                         const char *line, uint64_t now_ms, bool must_exist) {
  char session[BD_PERF_SESSION_MAX + 1];
  if (!json_field_string(line, "session", session, sizeof(session)) ||
      session[0] == '\0') {
    send_error(client, id, -32602, "missing session");
    return;
  }
  if (strlen(session) > BD_PERF_SESSION_MAX) {
    send_error(client, id, -32602, "session is too long");
    return;
  }

  long ttl = 0;
  bool ttl_present = json_field_present(line, "ttl_ms");
  if (ttl_present && !json_field_long(line, "ttl_ms", &ttl)) {
    send_error(client, id, -32602, "ttl_ms must be a number");
    return;
  }
  if (ttl < 0) {
    send_error(client, id, -32602, "ttl_ms must not be negative");
    return;
  }

  perf_request request;
  parse_request(line, &request);

  bd_perf_lease *existing = bd_perf_leases_find(&daemon->leases, session);
  if (must_exist && existing == NULL) {
    send_error(client, id, -32002, "no lease for session");
    return;
  }
  bd_perf_lease previous;
  bool had_previous = false;
  if (existing != NULL) {
    previous = *existing;
    had_previous = true;
  }

  /* perf.set without ttl_ms keeps the original lease deadline instead of
   * accidentally turning it into an indefinite lease. */
  if (must_exist && existing != NULL && !ttl_present &&
      existing->expires_ms != 0) {
    if (existing->expires_ms <= now_ms) {
      send_error(client, id, -32002, "lease has expired");
      return;
    }
    uint64_t remaining = existing->expires_ms - now_ms;
    ttl = remaining > (uint64_t)LONG_MAX ? LONG_MAX : (long)remaining;
  }

  bd_perf_lease *lease = bd_perf_leases_apply(
      &daemon->leases, session, client->id, request.profile, request.app,
      request.core, (uint64_t)(ttl > 0 ? ttl : 0), now_ms);
  if (lease == NULL) {
    if (had_previous && existing != NULL) {
      *existing = previous;
    }
    send_error(client, id, -32000, "lease table is full");
    return;
  }

  bd_perf_plan plan;
  char error[256] = "";
  if (!daemon_apply(daemon, &request, &plan, error, sizeof(error))) {
    if (had_previous && existing != NULL) {
      *existing = previous;
    } else {
      bd_perf_leases_restore(&daemon->leases, session);
    }
    send_error(client, id, -32000, error);
    return;
  }

  char key[sizeof(daemon->applied_key)];
  request_key(&request, key, sizeof(key));
  snprintf(daemon->applied_key, sizeof(daemon->applied_key), "%s", key);

  char result[1024];
  if (!format_plan_result(&plan, (uint64_t)(ttl > 0 ? ttl : 0), result,
                          sizeof(result))) {
    send_error(client, id, -32000, "result is too large");
    return;
  }
  client_sendf(client, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":%s}", id,
               result);
}

static void handle_global(perfd *daemon, perfd_client *client, long id,
                         const char *line) {
  char profile[BD_PERF_NAME_MAX + 1];
  if (!json_field_string(line, "profile", profile, sizeof(profile)) ||
      profile[0] == '\0') {
    send_error(client, id, -32602, "missing profile");
    return;
  }
  if (bd_perf_policy_find_profile(&daemon->policy, profile) == NULL &&
      strcmp(profile, "auto") != 0) {
    send_error(client, id, -32602, "unknown profile");
    return;
  }
  if (bd_perf_leases_effective(&daemon->leases) != NULL) {
    send_error(client, id, -32003,
               "an app lease owns the profile; retry after it ends");
    return;
  }

  char previous[BD_PERF_NAME_MAX + 1];
  snprintf(previous, sizeof(previous), "%s", daemon->global_profile);
  snprintf(daemon->global_profile, sizeof(daemon->global_profile), "%s",
           profile);

  perf_request request;
  effective_request(daemon, &request);
  bd_perf_plan plan;
  char error[256] = "";
  if (!daemon_apply(daemon, &request, &plan, error, sizeof(error))) {
    snprintf(daemon->global_profile, sizeof(daemon->global_profile), "%s",
             previous);
    send_error(client, id, -32000, error[0] != '\0' ? error
                                                    : "cannot apply profile");
    return;
  }

  char key[sizeof(daemon->applied_key)];
  request_key(&request, key, sizeof(key));
  snprintf(daemon->applied_key, sizeof(daemon->applied_key), "%s", key);

  char result[1024];
  if (!format_plan_result(&plan, 0, result, sizeof(result))) {
    send_error(client, id, -32000, "result is too large");
    return;
  }
  client_sendf(client, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":%s}", id,
               result);
}

static void handle_restore(perfd *daemon, perfd_client *client, long id,
                           const char *line, uint64_t now_ms) {
  char session[BD_PERF_SESSION_MAX + 1];
  if (!json_field_string(line, "session", session, sizeof(session)) ||
      session[0] == '\0') {
    send_error(client, id, -32602, "missing session");
    return;
  }
  bool restored = bd_perf_leases_restore(&daemon->leases, session);
  recompute_effective(daemon, now_ms, "perf.restore");
  char profile[BD_PERF_NAME_MAX * 2 + 1];
  json_escape(profile, sizeof(profile), daemon->applied_profile);
  client_sendf(client,
               "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"restored\":%s,"
               "\"profile\":\"%s\"}}",
               id, restored ? "true" : "false", profile);
}

static bool append_thermal_json(const bd_perf_system *system, char *buffer,
                                size_t size, size_t *used) {
  if (!appendf(buffer, size, used, "\"thermal\":[")) {
    return false;
  }
  for (size_t i = 0; i < system->thermal_count; ++i) {
    char zone[64];
    char type[BD_PERF_NAME_MAX * 2 + 1];
    json_escape(zone, sizeof(zone), system->thermals[i].zone);
    json_escape(type, sizeof(type), system->thermals[i].type);
    if (!appendf(buffer, size, used,
                 "%s{\"zone\":\"%s\",\"type\":\"%s\",\"temp_mc\":%ld}",
                 i == 0 ? "" : ",", zone, type,
                 system->thermals[i].temp_mc)) {
      return false;
    }
  }
  return appendf(buffer, size, used, "]");
}

static bool append_devfreq_json(const bd_perf_system *system, char *buffer,
                                size_t size, size_t *used) {
  if (!appendf(buffer, size, used, ",\"devfreq\":[")) {
    return false;
  }
  for (size_t i = 0; i < system->devfreq_count; ++i) {
    char name[BD_PERF_NAME_MAX * 2 + 1];
    json_escape(name, sizeof(name), system->devfreqs[i].name);
    if (!appendf(buffer, size, used,
                 "%s{\"name\":\"%s\",\"cur_freq\":%ld,"
                 "\"available_frequencies\":[",
                 i == 0 ? "" : ",", name, system->devfreqs[i].cur_freq)) {
      return false;
    }
    for (size_t f = 0; f < system->devfreqs[i].available_count; ++f) {
      if (!appendf(buffer, size, used, "%s%ld", f == 0 ? "" : ",",
                   system->devfreqs[i].available[f])) {
        return false;
      }
    }
    if (!appendf(buffer, size, used, "]}")) {
      return false;
    }
  }
  return appendf(buffer, size, used, "]");
}

static void handle_get(perfd *daemon, perfd_client *client, long id) {
  bd_perf_system system;
  char error[256] = "";
  if (!bd_perf_system_probe(daemon->sysfs_root, &system, error,
                            sizeof(error))) {
    send_error(client, id, -32000, error);
    return;
  }

  perf_request request;
  effective_request(daemon, &request);
  bd_perf_limits limits;
  char profile[BD_PERF_NAME_MAX + 1];
  bool resolved = bd_perf_policy_resolve(
      &daemon->policy, request.profile, request.app, request.core, &limits,
      error, sizeof(error));
  snprintf(profile, sizeof(profile), "%s",
           resolved ? limits.profile : request.profile);

  const bd_perf_lease *lease =
      bd_perf_leases_effective(&daemon->leases);
  uint64_t now = monotonic_ms();
  uint64_t remaining = 0;
  if (lease != NULL && lease->expires_ms != 0) {
    remaining = lease->expires_ms > now ? lease->expires_ms - now : 0;
  }

  char buffer[BD_PERFD_OUT_MAX];
  size_t used = 0;
  char escaped_profile[BD_PERF_NAME_MAX * 2 + 1];
  char escaped_app[BD_PERF_NAME_MAX * 2 + 1];
  char escaped_core[BD_PERF_NAME_MAX * 2 + 1];
  json_escape(escaped_profile, sizeof(escaped_profile), profile);
  json_escape(escaped_app, sizeof(escaped_app), request.app);
  json_escape(escaped_core, sizeof(escaped_core), request.core);
  if (!appendf(buffer, sizeof(buffer), &used,
               "{\"profile\":\"%s\",\"app\":\"%s\",\"core\":\"%s\","
               "\"lease\":",
               escaped_profile, escaped_app, escaped_core)) {
    send_error(client, id, -32000, "status is too large");
    return;
  }
  if (lease != NULL) {
    char session[BD_PERF_SESSION_MAX * 2 + 1];
    json_escape(session, sizeof(session), lease->session);
    if (!appendf(buffer, sizeof(buffer), &used,
                 "{\"session\":\"%s\",\"ttl_remaining_ms\":%llu}",
                 session, (unsigned long long)remaining)) {
      send_error(client, id, -32000, "status is too large");
      return;
    }
  } else if (!appendf(buffer, sizeof(buffer), &used, "null")) {
    send_error(client, id, -32000, "status is too large");
    return;
  }

  char governor[BD_PERF_GOVERNOR_MAX * 2 + 1];
  json_escape(governor, sizeof(governor), system.current_governor);
  if (!appendf(buffer, sizeof(buffer), &used,
               ",\"cpufreq\":%s,\"governor\":\"%s\",\"cur_freq_khz\":%d,"
               "\"min_khz\":%d,\"max_khz\":%d,\"online_cores\":%d,"
               "\"cpu_count\":%d,\"swappiness\":%d,",
               system.cpufreq ? "true" : "false", governor,
               system.current_freq_khz, system.current_min_khz,
               system.current_max_khz, system.online_count,
               system.cpu_count, system.swappiness) ||
      !append_thermal_json(&system, buffer, sizeof(buffer), &used) ||
      !append_devfreq_json(&system, buffer, sizeof(buffer), &used) ||
      !appendf(buffer, sizeof(buffer), &used, "}")) {
    send_error(client, id, -32000, "status is too large");
    return;
  }
  client_sendf(client, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":%s}", id,
               buffer);
}

static void handle_list(perfd *daemon, perfd_client *client, long id) {
  bd_perf_system system;
  char error[256] = "";
  if (!bd_perf_system_probe(daemon->sysfs_root, &system, error,
                            sizeof(error))) {
    send_error(client, id, -32000, error);
    return;
  }
  char buffer[BD_PERFD_OUT_MAX];
  size_t used = 0;
  if (!appendf(buffer, sizeof(buffer), &used, "{\"profiles\":[")) {
    send_error(client, id, -32000, "list is too large");
    return;
  }
  for (size_t i = 0; i < daemon->policy.profile_count; ++i) {
    char name[BD_PERF_NAME_MAX * 2 + 1];
    json_escape(name, sizeof(name), daemon->policy.profiles[i].name);
    if (!appendf(buffer, sizeof(buffer), &used, "%s\"%s\"",
                 i == 0 ? "" : ",", name)) {
      send_error(client, id, -32000, "list is too large");
      return;
    }
  }
  if (!appendf(buffer, sizeof(buffer), &used, "],\"apps\":[")) {
    send_error(client, id, -32000, "list is too large");
    return;
  }
  for (size_t i = 0; i < daemon->policy.app_count; ++i) {
    char name[BD_PERF_NAME_MAX * 2 + 1];
    json_escape(name, sizeof(name), daemon->policy.apps[i].name);
    if (!appendf(buffer, sizeof(buffer), &used, "%s\"%s\"",
                 i == 0 ? "" : ",", name)) {
      send_error(client, id, -32000, "list is too large");
      return;
    }
  }
  if (!appendf(buffer, sizeof(buffer), &used, "],\"cores\":[")) {
    send_error(client, id, -32000, "list is too large");
    return;
  }
  for (size_t i = 0; i < daemon->policy.core_count; ++i) {
    char name[BD_PERF_NAME_MAX * 2 + 1];
    json_escape(name, sizeof(name), daemon->policy.cores[i].name);
    if (!appendf(buffer, sizeof(buffer), &used, "%s\"%s\"",
                 i == 0 ? "" : ",", name)) {
      send_error(client, id, -32000, "list is too large");
      return;
    }
  }
  if (!appendf(buffer, sizeof(buffer), &used, "],\"governors\":[")) {
    send_error(client, id, -32000, "list is too large");
    return;
  }
  for (size_t i = 0; i < system.governor_count; ++i) {
    char name[BD_PERF_GOVERNOR_MAX * 2 + 1];
    json_escape(name, sizeof(name), system.governors[i]);
    if (!appendf(buffer, sizeof(buffer), &used, "%s\"%s\"",
                 i == 0 ? "" : ",", name)) {
      send_error(client, id, -32000, "list is too large");
      return;
    }
  }
  if (!appendf(buffer, sizeof(buffer), &used, "],\"frequencies\":[")) {
    send_error(client, id, -32000, "list is too large");
    return;
  }
  for (size_t i = 0; i < system.freq_count; ++i) {
    if (!appendf(buffer, sizeof(buffer), &used, "%s%d",
                 i == 0 ? "" : ",", system.freqs[i])) {
      send_error(client, id, -32000, "list is too large");
      return;
    }
  }
  char global[BD_PERF_NAME_MAX * 2 + 1];
  json_escape(global, sizeof(global), daemon->global_profile);
  if (!appendf(buffer, sizeof(buffer), &used,
               "],\"cpu_count\":%d,\"global_profile\":\"%s\"}",
               system.cpu_count, global)) {
    send_error(client, id, -32000, "list is too large");
    return;
  }
  client_sendf(client, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":%s}", id,
               buffer);
}

static void handle_capabilities(perfd *daemon, perfd_client *client,
                                long id) {
  bd_perf_system system;
  char error[256] = "";
  if (!bd_perf_system_probe(daemon->sysfs_root, &system, error,
                            sizeof(error))) {
    send_error(client, id, -32000, error);
    return;
  }
  bool online_control = false;
  for (int cpu = 1; cpu < system.cpu_count; ++cpu) {
    if (system.online_path[cpu][0] != '\0') {
      online_control = true;
      break;
    }
  }
  client_sendf(
      client,
      "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{"
      "\"cpufreq\":%s,\"governor_control\":%s,\"frequency_control\":%s,"
      "\"online_cores\":%s,\"swappiness\":%s,\"thermal\":%zu,"
      "\"gpu_devfreq\":%zu,\"cpu_count\":%d,\"policy_count\":%d,"
      "\"profiles\":%zu}}",
      id, system.cpufreq ? "true" : "false",
      system.governor_count > 0 ? "true" : "false",
      system.freq_count > 0 ? "true" : "false",
      online_control ? "true" : "false",
      system.has_swappiness ? "true" : "false", system.thermal_count,
      system.devfreq_count, system.cpu_count, system.policy_count,
      daemon->policy.profile_count);
}

static void handle_client_line(perfd *daemon, perfd_client *client,
                               const char *line, uint64_t now_ms) {
  char method[64];
  if (!json_field_string(line, "method", method, sizeof(method))) {
    send_error(client, 0, -32600, "missing method");
    return;
  }
  long id = 0;
  json_field_long(line, "id", &id);

  if (strcmp(method, "perf.apply") == 0) {
    handle_apply(daemon, client, id, line, now_ms, false);
  } else if (strcmp(method, "perf.set") == 0) {
    handle_apply(daemon, client, id, line, now_ms, true);
  } else if (strcmp(method, "perf.global") == 0) {
    handle_global(daemon, client, id, line);
  } else if (strcmp(method, "perf.restore") == 0) {
    handle_restore(daemon, client, id, line, now_ms);
  } else if (strcmp(method, "perf.get") == 0) {
    handle_get(daemon, client, id);
  } else if (strcmp(method, "perf.list") == 0) {
    handle_list(daemon, client, id);
  } else if (strcmp(method, "perf.capabilities") == 0) {
    handle_capabilities(daemon, client, id);
  } else if (strcmp(method, "perf.ping") == 0) {
    client_sendf(client, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{"
                         "\"pong\":true}}",
                 id);
  } else {
    send_error(client, id, -32601, "unknown method");
  }
}

static void client_read(perfd *daemon, size_t index, uint64_t now_ms) {
  perfd_client *client = &daemon->clients[index];
  char buffer[1024];
  for (;;) {
    ssize_t bytes = recv(client->fd, buffer, sizeof(buffer), 0);
    if (bytes < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return;
      }
      client_close(client);
      return;
    }
    if (bytes == 0) {
      client_close(client);
      return;
    }
    size_t copied = 0;
    while (copied < (size_t)bytes) {
      size_t room = sizeof(client->input) - 1 - client->input_length;
      if (room == 0) {
        log_message(daemon, "client line too long; disconnecting");
        client_close(client);
        return;
      }
      size_t chunk = (size_t)bytes - copied;
      if (chunk > room) {
        chunk = room;
      }
      memcpy(client->input + client->input_length, buffer + copied, chunk);
      client->input_length += chunk;
      copied += chunk;

      char *newline = memchr(client->input, '\n', client->input_length);
      while (newline != NULL) {
        size_t line_length = (size_t)(newline - client->input);
        client->input[line_length] = '\0';
        handle_client_line(daemon, client, client->input, now_ms);
        if (client->fd < 0) {
          return;
        }
        size_t consumed = line_length + 1;
        memmove(client->input, client->input + consumed,
                client->input_length - consumed);
        client->input_length -= consumed;
        newline = memchr(client->input, '\n', client->input_length);
      }
    }
  }
}

static void client_accept(perfd *daemon) {
  int fd = accept(daemon->listen_fd, NULL, NULL);
  if (fd < 0) {
    return;
  }
  if (daemon->client_count >= BD_PERFD_MAX_CLIENTS) {
    close(fd);
    return;
  }
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  perfd_client *client = &daemon->clients[daemon->client_count++];
  memset(client, 0, sizeof(*client));
  client->fd = fd;
  client->id = ++daemon->next_client_id;

  char socket_path[sizeof(daemon->socket_path) * 2 + 1];
  char profile[BD_PERF_NAME_MAX * 2 + 1];
  json_escape(socket_path, sizeof(socket_path), daemon->socket_path);
  json_escape(profile, sizeof(profile), daemon->global_profile);
  client_sendf(client,
               "{\"type\":\"hello\",\"version\":%d,\"pid\":%ld,"
               "\"socket\":\"%s\",\"profile\":\"%s\",\"cpufreq\":%s}",
               BD_PERF_PROTOCOL_VERSION, (long)getpid(), socket_path,
               profile, daemon->system.cpufreq ? "true" : "false");
}

static bool create_socket(perfd *daemon, const char *path) {
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(address.sun_path)) {
    log_message(daemon, "socket path is too long: %s", path);
    return false;
  }
  snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
  unlink(path);

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    log_message(daemon, "socket: %s", strerror(errno));
    return false;
  }
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    log_message(daemon, "bind %s: %s", path, strerror(errno));
    close(fd);
    return false;
  }
  if (listen(fd, 8) != 0) {
    log_message(daemon, "listen %s: %s", path, strerror(errno));
    close(fd);
    unlink(path);
    return false;
  }
  chmod(path, 0660);
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  daemon->listen_fd = fd;
  snprintf(daemon->socket_path, sizeof(daemon->socket_path), "%s", path);
  daemon->socket_owned = true;
  return true;
}

static bool make_parent_directory(const char *path) {
  char directory[sizeof(((struct sockaddr_un *)0)->sun_path)];
  snprintf(directory, sizeof(directory), "%s", path);
  char *slash = strrchr(directory, '/');
  if (slash == NULL || slash == directory) {
    return true;
  }
  *slash = '\0';
  if (mkdir(directory, 0770) == 0 || errno == EEXIST) {
    return true;
  }
  return false;
}

static bool setup_socket(perfd *daemon, const char *requested) {
  char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  if (requested != NULL && *requested != '\0') {
    if (strlen(requested) >= sizeof(path)) {
      log_message(daemon, "socket path is too long: %s", requested);
      return false;
    }
    snprintf(path, sizeof(path), "%s", requested);
    make_parent_directory(path);
  } else {
    const char *environment = getenv("BRICKDESK_PERF_SOCKET");
    if (environment != NULL && *environment != '\0') {
      if (strlen(environment) >= sizeof(path)) {
        log_message(daemon, "socket path is too long: %s", environment);
        return false;
      }
      snprintf(path, sizeof(path), "%s", environment);
      make_parent_directory(path);
    } else {
      snprintf(path, sizeof(path), "/run/brickdesk/perfd.sock");
      if (!make_parent_directory(path)) {
        snprintf(path, sizeof(path), "/tmp/brickdesk-perfd.sock");
      }
    }
  }

  char lock_path[sizeof(path) + 8];
  snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
  int lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0660);
  if (lock_fd < 0) {
    log_message(daemon, "cannot open lock %s: %s", lock_path, strerror(errno));
    return false;
  }
  if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    log_message(daemon, "another brickdesk-perfd is running (%s)", lock_path);
    close(lock_fd);
    return false;
  }
  daemon->lock_fd = lock_fd;

  if (!create_socket(daemon, path)) {
    close(daemon->lock_fd);
    daemon->lock_fd = -1;
    return false;
  }
  return true;
}

static int compute_timeout(perfd *daemon, uint64_t now_ms) {
  uint64_t deadline = 0;
  int timeout = -1;
  if (bd_perf_leases_next_deadline(&daemon->leases, &deadline)) {
    if (deadline <= now_ms) {
      return 0;
    }
    uint64_t remaining = deadline - now_ms;
    timeout = remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
  }
  if (strcmp(daemon->global_profile, "auto") == 0 &&
      (timeout < 0 || timeout > 5000)) {
    /* Re-evaluate charging/low-battery at least every five seconds. */
    timeout = 5000;
  }
  return timeout;
}

static void daemon_cleanup(perfd *daemon) {
  if (daemon->have_snapshot) {
    bd_perf_system system;
    char error[256] = "";
    if (!bd_perf_system_probe(daemon->sysfs_root, &system, error,
                              sizeof(error))) {
      log_message(daemon, "cannot probe sysfs for restore: %s", error);
    } else if (!bd_perf_snapshot_restore(&system, &daemon->snapshot, error,
                                         sizeof(error))) {
      log_message(daemon, "cannot restore startup values: %s", error);
    } else if (daemon->verbose) {
      log_message(daemon, "restored startup values");
    }
    daemon->have_snapshot = false;
  }

  for (size_t i = 0; i < daemon->client_count; ++i) {
    client_close(&daemon->clients[i]);
  }
  daemon->client_count = 0;
  if (daemon->listen_fd >= 0) {
    close(daemon->listen_fd);
    daemon->listen_fd = -1;
  }
  if (daemon->socket_owned && daemon->socket_path[0] != '\0') {
    unlink(daemon->socket_path);
    daemon->socket_owned = false;
  }
  if (daemon->lock_fd >= 0) {
    close(daemon->lock_fd);
    daemon->lock_fd = -1;
  }
}

static void print_usage(FILE *stream) {
  fprintf(stream,
          "usage: brickdesk-perfd [options]\n"
          "\n"
          "  --config-dir DIR       configuration directory (perf.ini)\n"
          "  --socket PATH          Unix socket (default $BRICKDESK_PERF_SOCKET\n"
          "                         or /run/brickdesk/perfd.sock)\n"
          "  --sysfs-root DIR       prefix for /sys and /proc (tests)\n"
          "  --profile NAME         global profile (default: default)\n"
          "  --app ID               app id for --dry-run\n"
          "  --core NAME            emulator core for --dry-run\n"
          "  --dry-run              print the plan and exit; never writes\n"
          "  --status               print the probed state and exit\n"
          "  --verbose              log startup and restore details\n"
          "  -h, --help             show this help\n");
}

static const char *thermal_type_or_dash(const bd_perf_thermal *thermal) {
  return thermal->type[0] != '\0' ? thermal->type : "-";
}

static int status_command(const char *sysfs_root, const char *config_dir,
                          const char *global_profile) {
  bd_perf_system system;
  char error[256] = "";
  if (!bd_perf_system_probe(sysfs_root, &system, error, sizeof(error))) {
    fprintf(stderr, "brickdesk-perfd: %s\n", error);
    return 1;
  }

  printf("cpufreq: %s (%d policy)\n", system.cpufreq ? "available" : "missing",
         system.policy_count);
  printf("governor: %s\n", system.current_governor[0] != '\0'
                               ? system.current_governor
                               : "-");
  printf("available governors:");
  for (size_t i = 0; i < system.governor_count; ++i) {
    printf(" %s", system.governors[i]);
  }
  printf("\n");
  printf("frequencies: min %d kHz, max %d kHz (cpuinfo %d..%d)\n",
         system.current_min_khz, system.current_max_khz,
         system.cpuinfo_min_khz, system.cpuinfo_max_khz);
  printf("current frequency: %s%d kHz\n",
         system.has_current_freq ? "" : "unavailable ",
         system.current_freq_khz);
  printf("available frequencies:");
  for (size_t i = 0; i < system.freq_count; ++i) {
    printf(" %d", system.freqs[i]);
  }
  printf("\n");
  printf("cpus: %d online of %d\n", system.online_count, system.cpu_count);
  printf("swappiness: %s%d\n", system.has_swappiness ? "" : "unavailable ",
         system.swappiness);
  for (size_t i = 0; i < system.thermal_count; ++i) {
    printf("thermal: %s %s %ld mC\n", system.thermals[i].zone,
           thermal_type_or_dash(&system.thermals[i]),
           system.thermals[i].temp_mc);
  }
  for (size_t i = 0; i < system.devfreq_count; ++i) {
    printf("devfreq: %s %ld Hz (read-only)\n", system.devfreqs[i].name,
           system.devfreqs[i].cur_freq);
  }

  bd_perf_policy policy;
  if (bd_perf_policy_load(&policy, config_dir, error, sizeof(error))) {
    printf("global profile: %s\n",
           global_profile != NULL ? global_profile : "default");
    printf("profiles:");
    for (size_t i = 0; i < policy.profile_count; ++i) {
      printf(" %s", policy.profiles[i].name);
    }
    printf("\n");
  } else {
    printf("profiles: unavailable (%s)\n", error);
  }
  return 0;
}

static int dry_run_command(const char *sysfs_root, const char *config_dir,
                           const char *profile, const char *app,
                           const char *core) {
  bd_perf_policy policy;
  char error[256] = "";
  if (!bd_perf_policy_load(&policy, config_dir, error, sizeof(error))) {
    fprintf(stderr, "brickdesk-perfd: %s\n", error);
    return 1;
  }
  bd_perf_system system;
  if (!bd_perf_system_probe(sysfs_root, &system, error, sizeof(error))) {
    fprintf(stderr, "brickdesk-perfd: %s\n", error);
    return 1;
  }
  bd_perf_limits limits;
  if (!bd_perf_policy_resolve(&policy, profile, app, core, &limits, error,
                              sizeof(error))) {
    fprintf(stderr, "brickdesk-perfd: %s\n", error);
    return 1;
  }
  bd_perf_plan plan;
  if (!bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error))) {
    fprintf(stderr, "brickdesk-perfd: %s\n", error);
    return 1;
  }

  printf("profile: %s", plan.profile);
  if (plan.app[0] != '\0') {
    printf(" app=%s", plan.app);
  }
  if (plan.core[0] != '\0') {
    printf(" core=%s", plan.core);
  }
  printf("\n");
  printf("governor: %s (current %s)%s\n", plan.governor,
         plan.current_governor, plan.governor_fallback ? " [fallback]" : "");
  printf("min_khz: %d (current %d)\n", plan.min_khz, plan.current_min_khz);
  printf("max_khz: %d (current %d)\n", plan.max_khz, plan.current_max_khz);
  printf("online_cores: %d (current %d)\n", plan.online_cores,
         plan.current_online_cores);
  if (plan.swappiness_unsupported) {
    printf("swappiness: unsupported (no /proc/sys/vm/swappiness)\n");
  } else {
    printf("swappiness: %d (current %d)\n",
           plan.swappiness_change ? plan.swappiness
                                  : plan.current_swappiness,
           plan.current_swappiness);
  }

  int changes = 0;
  if (plan.governor_change) {
    printf("write %s = %s\n", system.governor_path, plan.governor);
    ++changes;
  }
  if (plan.min_change) {
    printf("write %s = %d\n", system.min_path, plan.min_khz);
    ++changes;
  }
  if (plan.max_change) {
    printf("write %s = %d\n", system.max_path, plan.max_khz);
    ++changes;
  }
  if (plan.online_change) {
    printf("write cpu online: %d cores\n", plan.online_cores);
    ++changes;
  }
  if (plan.swappiness_change) {
    printf("write %s = %d\n", system.swappiness_path, plan.swappiness);
    ++changes;
  }
  printf("plan: %d change%s\n", changes, changes == 1 ? "" : "s");
  printf("dry-run: no writes performed\n");
  return 0;
}

static bool run_daemon(perfd *daemon, uint64_t *exit_code) {
  struct pollfd fds[BD_PERFD_MAX_CLIENTS + 1];
  int owners[BD_PERFD_MAX_CLIENTS + 1];
  while (daemon->running && !g_stop) {
    uint64_t now = monotonic_ms();
    if (bd_perf_leases_expire(&daemon->leases, now) > 0) {
      recompute_effective(daemon, now, "lease expired");
    }

    size_t count = 0;
    fds[count].fd = daemon->listen_fd;
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    owners[count] = -1;
    ++count;
    for (size_t i = 0; i < daemon->client_count; ++i) {
      if (daemon->clients[i].fd < 0) {
        continue;
      }
      fds[count].fd = daemon->clients[i].fd;
      fds[count].events = POLLIN;
      fds[count].revents = 0;
      owners[count] = (int)i;
      ++count;
    }

    int timeout = compute_timeout(daemon, now);
    int ready = poll(fds, (nfds_t)count, timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      log_message(daemon, "poll: %s", strerror(errno));
      *exit_code = 1;
      return false;
    }

    now = monotonic_ms();
    if (bd_perf_leases_expire(&daemon->leases, now) > 0) {
      recompute_effective(daemon, now, "lease expired");
    }
    if (strcmp(daemon->global_profile, "auto") == 0 &&
        now >= daemon->auto_next_ms) {
      daemon->auto_next_ms = now + 5000;
      bool charging = false;
      int capacity = -1;
      auto_power_state(daemon, &charging, &capacity);
      int low_state =
          (!charging && capacity >= 0 && capacity <= 10) ? 1 : 0;
      if (daemon->auto_low_state < 0 ||
          low_state != daemon->auto_low_state) {
        daemon->auto_low_state = low_state;
        daemon->applied_key[0] = '\0';
        recompute_effective(daemon, now, "auto low-battery state");
      }
    }
    if (ready == 0) {
      continue;
    }

    if (fds[0].revents & POLLIN) {
      client_accept(daemon);
    }
    for (size_t k = 1; k < count; ++k) {
      int index = owners[k];
      if (index < 0 || (size_t)index >= daemon->client_count) {
        continue;
      }
      if (fds[k].revents & (POLLIN | POLLHUP | POLLERR)) {
        client_read(daemon, (size_t)index, now);
      }
      if (daemon->clients[index].fd < 0) {
        drop_client(daemon, &daemon->clients[index]);
      }
    }
    client_compact(daemon);
  }
  *exit_code = 0;
  return true;
}

int main(int argc, char **argv) {
  const char *config_dir = NULL;
  const char *socket_path = NULL;
  const char *sysfs_root = "";
  const char *profile = NULL;
  const char *app = NULL;
  const char *core = NULL;
  bool dry_run = false;
  bool status = false;
  bool verbose = false;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (strcmp(arg, "--config-dir") == 0 && i + 1 < argc) {
      config_dir = argv[++i];
    } else if (strcmp(arg, "--socket") == 0 && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (strcmp(arg, "--sysfs-root") == 0 && i + 1 < argc) {
      sysfs_root = argv[++i];
    } else if (strcmp(arg, "--profile") == 0 && i + 1 < argc) {
      profile = argv[++i];
    } else if (strcmp(arg, "--app") == 0 && i + 1 < argc) {
      app = argv[++i];
    } else if (strcmp(arg, "--core") == 0 && i + 1 < argc) {
      core = argv[++i];
    } else if (strcmp(arg, "--dry-run") == 0) {
      dry_run = true;
    } else if (strcmp(arg, "--status") == 0) {
      status = true;
    } else if (strcmp(arg, "--verbose") == 0) {
      verbose = true;
    } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      print_usage(stdout);
      return 0;
    } else {
      fprintf(stderr, "brickdesk-perfd: unknown option: %s\n", arg);
      print_usage(stderr);
      return 2;
    }
  }

  if (status && dry_run) {
    fprintf(stderr, "brickdesk-perfd: choose --status or --dry-run\n");
    return 2;
  }
  if (strlen(sysfs_root) >= BD_PERF_PATH_MAX) {
    fprintf(stderr, "brickdesk-perfd: --sysfs-root path is too long\n");
    return 2;
  }
  if (status) {
    return status_command(sysfs_root, config_dir, profile);
  }
  if (dry_run) {
    return dry_run_command(sysfs_root, config_dir, profile, app, core);
  }

  perfd daemon;
  memset(&daemon, 0, sizeof(daemon));
  daemon.listen_fd = -1;
  daemon.lock_fd = -1;
  daemon.running = true;
  daemon.verbose = verbose;
  snprintf(daemon.sysfs_root, sizeof(daemon.sysfs_root), "%s", sysfs_root);
  snprintf(daemon.global_profile, sizeof(daemon.global_profile), "%s",
           profile != NULL && *profile != '\0' ? profile : "default");
  bd_perf_leases_init(&daemon.leases);

  char error[256] = "";
  if (!bd_perf_policy_load(&daemon.policy, config_dir, error,
                           sizeof(error))) {
    log_message(&daemon, "%s", error);
    return 1;
  }
  if (bd_perf_policy_find_profile(&daemon.policy,
                                  daemon.global_profile) == NULL) {
    log_message(&daemon, "unknown global profile '%s'",
                daemon.global_profile);
    return 1;
  }
  if (!bd_perf_system_probe(daemon.sysfs_root, &daemon.system, error,
                            sizeof(error))) {
    log_message(&daemon, "%s", error);
    return 1;
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);

  if (!setup_socket(&daemon, socket_path)) {
    daemon_cleanup(&daemon);
    return 1;
  }

  bd_perf_snapshot_take(&daemon.system, &daemon.snapshot);
  daemon.have_snapshot = true;
  recompute_effective(&daemon, monotonic_ms(), "startup");

  if (daemon.verbose) {
    log_message(&daemon,
                "socket=%s sysfs=%s global=%s cpufreq=%s policy=%d",
                daemon.socket_path,
                daemon.sysfs_root[0] != '\0' ? daemon.sysfs_root : "/",
                daemon.global_profile,
                daemon.system.cpufreq ? "yes" : "no",
                daemon.system.policy_count);
  }

  uint64_t exit_code = 0;
  run_daemon(&daemon, &exit_code);
  daemon_cleanup(&daemon);
  return (int)exit_code;
}
