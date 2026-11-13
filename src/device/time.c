#define _POSIX_C_SOURCE 200809L

#include "brickdesk/device_time.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "device_process.h"

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
}

static void copy_text(char *dst, size_t size, const char *src) {
  if (dst == NULL || size == 0) {
    return;
  }
  snprintf(dst, size, "%s", src != NULL ? src : "");
}

static bool run_ok(char *const argv[], char *error, size_t error_size) {
  int code = 0;
  if (!bd_process_run(argv, NULL, 0, &code, error, error_size)) {
    return false;
  }
  if (code != 0) {
    set_error(error, error_size, "%s failed (%d)", argv[0], code);
    return false;
  }
  return true;
}

static bool read_uci(const char *key, char *out, size_t out_size) {
  char *argv[] = {"uci", "get", (char *)key, NULL};
  int code = 0;
  char error[64];
  if (!bd_process_run(argv, out, out_size, &code, error, sizeof(error)) ||
      code != 0) {
    out[0] = '\0';
    return false;
  }
  size_t length = strcspn(out, "\r\n");
  out[length] = '\0';
  return out[0] != '\0';
}

static void read_timezone(char *out, size_t out_size) {
  out[0] = '\0';
  if (read_uci("system.@system[0].zonename", out, out_size)) {
    return;
  }
  char link[256];
  ssize_t length = readlink("/etc/localtime", link, sizeof(link) - 1);
  if (length > 0) {
    link[length] = '\0';
    const char *marker = strstr(link, "zoneinfo/");
    if (marker != NULL) {
      copy_text(out, out_size, marker + 9);
      return;
    }
  }
  FILE *file = fopen("/etc/timezone", "r");
  if (file != NULL) {
    if (fgets(out, (int)out_size, file) == NULL) {
      out[0] = '\0';
    }
    fclose(file);
    size_t text_length = strcspn(out, "\r\n");
    out[text_length] = '\0';
  }
}

bool bd_time_get_status(bd_time_status *status) {
  if (status == NULL) {
    return false;
  }
  memset(status, 0, sizeof(*status));
  status->now = time(NULL);
  read_timezone(status->timezone, sizeof(status->timezone));
  char ntp[16];
  if (read_uci("system.ntp.enable", ntp, sizeof(ntp))) {
    status->ntp = strcmp(ntp, "1") == 0 || strcmp(ntp, "true") == 0;
  } else {
    char *argv[] = {"pgrep", "-x", "ntpd", NULL};
    int code = 0;
    char error[64];
    status->ntp = bd_process_run(argv, NULL, 0, &code, error, sizeof(error)) &&
                  code == 0;
  }
  status->rtc = access("/dev/rtc0", F_OK) == 0;
  return true;
}

bool bd_time_set_epoch(time_t epoch, char *error, size_t error_size) {
  struct tm utc;
  if (gmtime_r(&epoch, &utc) == NULL) {
    set_error(error, error_size, "invalid time");
    return false;
  }
  char text[32];
  strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &utc);
  char *argv[] = {"date", "-u", "-s", text, NULL};
  if (!run_ok(argv, error, error_size)) {
    return false;
  }
  return bd_time_write_rtc(error, error_size);
}

bool bd_time_set_timezone(const char *timezone, char *error,
                          size_t error_size) {
  if (timezone == NULL || timezone[0] == '\0') {
    set_error(error, error_size, "missing timezone");
    return false;
  }
  char zone_path[256];
  snprintf(zone_path, sizeof(zone_path), "/usr/share/zoneinfo/%s", timezone);
  if (access(zone_path, F_OK) != 0) {
    set_error(error, error_size, "unknown timezone %s", timezone);
    return false;
  }
  char *set_argv[] = {"uci", "set",
                      "system.@system[0].zonename", (char *)timezone, NULL};
  char ignored[64];
  run_ok(set_argv, ignored, sizeof(ignored));
  char *commit[] = {"uci", "commit", "system", NULL};
  run_ok(commit, ignored, sizeof(ignored));
  unlink("/etc/localtime");
  if (symlink(zone_path, "/etc/localtime") != 0) {
    set_error(error, error_size, "localtime: %s", strerror(errno));
    return false;
  }
  FILE *file = fopen("/etc/timezone", "w");
  if (file != NULL) {
    fprintf(file, "%s\n", timezone);
    fclose(file);
  }
  setenv("TZ", timezone, 1);
  tzset();
  if (error != NULL && error_size > 0) {
    error[0] = '\0';
  }
  return true;
}

bool bd_time_set_ntp(bool enabled, char *error, size_t error_size) {
  char *set_argv[] = {"uci", "set", "system.ntp.enable",
                      enabled ? "1" : "0", NULL};
  if (!run_ok(set_argv, error, error_size)) {
    return false;
  }
  char *commit[] = {"uci", "commit", "system", NULL};
  if (!run_ok(commit, error, error_size)) {
    return false;
  }
  char *reload[] = {"/etc/init.d/ntpd", enabled ? "restart" : "stop", NULL};
  if (access(reload[0], X_OK) == 0) {
    run_ok(reload, error, error_size);
  }
  if (enabled) {
    bd_time_sync_now(error, error_size);
  }
  return true;
}

bool bd_time_sync_now(char *error, size_t error_size) {
  char *argv[] = {"ntpd", "-q", "-n", NULL};
  if (!run_ok(argv, error, error_size)) {
    return false;
  }
  return bd_time_write_rtc(error, error_size);
}

bool bd_time_write_rtc(char *error, size_t error_size) {
  if (access("/dev/rtc0", F_OK) != 0) {
    set_error(error, error_size, "no /dev/rtc0");
    return false;
  }
  char *argv[] = {"hwclock", "-u", "-w", NULL};
  return run_ok(argv, error, error_size);
}
