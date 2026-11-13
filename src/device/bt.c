#define _POSIX_C_SOURCE 200809L

#include "brickdesk/device_bt.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "device_process.h"

#define BD_BT_OUTPUT_MAX 8192

struct bd_bt {
  char adapter[BD_BT_NAME_MAX];
};

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

static bool bt_command(char *const extra[], char *output, size_t output_size,
                       char *error, size_t error_size) {
  char command[16];
  char *argv[16];
  size_t index = 0;
  snprintf(command, sizeof(command), "%s", "bluetoothctl");
  argv[index++] = command;
  for (size_t i = 0; extra != NULL && extra[i] != NULL && index + 1 < 16; ++i) {
    argv[index++] = extra[i];
  }
  argv[index] = NULL;
  int code = 0;
  if (!bd_process_run(argv, output, output_size, &code, error, error_size)) {
    return false;
  }
  if (code != 0) {
    set_error(error, error_size, "bluetoothctl %s failed (%d)", extra[0], code);
    return false;
  }
  return true;
}

static void parse_status(const char *text, bd_bt_status *status) {
  char line[512];
  const char *cursor = text;
  while (cursor != NULL && *cursor != '\0') {
    const char *newline = strchr(cursor, '\n');
    size_t length = newline != NULL ? (size_t)(newline - cursor) : strlen(cursor);
    if (length >= sizeof(line)) {
      length = sizeof(line) - 1;
    }
    memcpy(line, cursor, length);
    line[length] = '\0';
    char *start = line;
    while (*start == ' ' || *start == '\t') {
      ++start;
    }
    char *colon = strchr(start, ':');
    if (colon != NULL) {
      *colon = '\0';
      const char *key = start;
      const char *value = colon + 1;
      while (*value == ' ') {
        ++value;
      }
      if (strcmp(key, "Powered") == 0) {
        status->powered = strcmp(value, "yes") == 0;
      } else if (strcmp(key, "Discovering") == 0) {
        status->discovering = strcmp(value, "yes") == 0;
      } else if (strcmp(key, "Name") == 0) {
        copy_text(status->adapter, sizeof(status->adapter), value);
      }
    }
    cursor = newline != NULL ? newline + 1 : NULL;
  }
}

bd_bt *bd_bt_create(void) {
  bd_bt *bt = (bd_bt *)calloc(1, sizeof(*bt));
  if (bt == NULL) {
    return NULL;
  }
  copy_text(bt->adapter, sizeof(bt->adapter), "hci0");
  return bt;
}

void bd_bt_destroy(bd_bt *bt) {
  free(bt);
}

bool bd_bt_get_status(bd_bt *bt, bd_bt_status *status) {
  if (bt == NULL || status == NULL) {
    return false;
  }
  memset(status, 0, sizeof(*status));
  status->state = BD_BT_STATE_UNKNOWN;
  char *show[] = {"show", NULL};
  char output[BD_BT_OUTPUT_MAX];
  char error[128] = {0};
  if (!bt_command(show, output, sizeof(output), error, sizeof(error))) {
    status->state = BD_BT_STATE_DISABLED;
    return true;
  }
  parse_status(output, status);
  if (!status->powered) {
    status->state = BD_BT_STATE_DISABLED;
    return true;
  }
  status->state = BD_BT_STATE_ENABLED;
  char *connected[] = {"devices", "Connected", NULL};
  if (bt_command(connected, output, sizeof(output), error, sizeof(error))) {
    char line[512];
    const char *cursor = output;
    while (cursor != NULL && *cursor != '\0') {
      const char *newline = strchr(cursor, '\n');
      size_t length =
          newline != NULL ? (size_t)(newline - cursor) : strlen(cursor);
      if (length >= sizeof(line)) {
        length = sizeof(line) - 1;
      }
      memcpy(line, cursor, length);
      line[length] = '\0';
      if (strncmp(line, "Device ", 7) == 0) {
        char addr[BD_BT_ADDR_MAX];
        char name[BD_BT_NAME_MAX];
        if (sscanf(line + 7, "%17s %63[^\n]", addr, name) == 2) {
          copy_text(status->connected_addr, sizeof(status->connected_addr),
                    addr);
          copy_text(status->connected_name, sizeof(status->connected_name),
                    name);
          status->state = BD_BT_STATE_CONNECTED;
          break;
        }
      }
      cursor = newline != NULL ? newline + 1 : NULL;
    }
  }
  return true;
}

bool bd_bt_set_powered(bd_bt *bt, bool powered, char *error,
                       size_t error_size) {
  if (bt == NULL) {
    set_error(error, error_size, "missing Bluetooth backend");
    return false;
  }
  char *extra[] = {"power", powered ? "on" : "off", NULL};
  char output[512];
  return bt_command(extra, output, sizeof(output), error, error_size);
}

bool bd_bt_scan_start(bd_bt *bt, int scan_seconds, char *error,
                      size_t error_size) {
  if (bt == NULL) {
    set_error(error, error_size, "missing Bluetooth backend");
    return false;
  }
  if (scan_seconds <= 0) {
    scan_seconds = 6;
  }
  char timeout[16];
  snprintf(timeout, sizeof(timeout), "%d", scan_seconds);
  char *argv[] = {"bluetoothctl", "--timeout", timeout, "scan", "on", NULL};
  return bd_process_spawn(argv, error, error_size);
}

bool bd_bt_list_devices(bd_bt *bt, bd_bt_device *out, size_t max,
                        size_t *count, char *error, size_t error_size) {
  if (bt == NULL || out == NULL || count == NULL) {
    return false;
  }
  *count = 0;
  char *extra[] = {"devices", NULL};
  char output[BD_BT_OUTPUT_MAX];
  if (!bt_command(extra, output, sizeof(output), error, error_size)) {
    return false;
  }
  char connected_output[BD_BT_OUTPUT_MAX] = {0};
  char *connected_extra[] = {"devices", "Connected", NULL};
  char ignored[64];
  bt_command(connected_extra, connected_output, sizeof(connected_output),
             ignored, sizeof(ignored));

  char *save = NULL;
  char *line = strtok_r(output, "\n", &save);
  while (line != NULL && *count < max) {
    if (strncmp(line, "Device ", 7) == 0) {
      char addr[BD_BT_ADDR_MAX];
      char name[BD_BT_NAME_MAX];
      if (sscanf(line + 7, "%17s %63[^\n]", addr, name) == 2) {
        bd_bt_device *device = &out[*count];
        memset(device, 0, sizeof(*device));
        copy_text(device->addr, sizeof(device->addr), addr);
        copy_text(device->name, sizeof(device->name), name);
        char needle[64];
        snprintf(needle, sizeof(needle), "Device %s", addr);
        device->connected = strstr(connected_output, needle) != NULL;
        device->paired = device->connected;
        ++*count;
      }
    }
    line = strtok_r(NULL, "\n", &save);
  }
  return true;
}

bool bd_bt_connect(bd_bt *bt, const char *addr, char *error,
                   size_t error_size) {
  if (bt == NULL || addr == NULL || addr[0] == '\0') {
    set_error(error, error_size, "missing Bluetooth address");
    return false;
  }
  char output[512];
  char *pair[] = {"pair", (char *)addr, NULL};
  bt_command(pair, output, sizeof(output), error, error_size);
  char *trust[] = {"trust", (char *)addr, NULL};
  bt_command(trust, output, sizeof(output), error, error_size);
  char *connect[] = {"connect", (char *)addr, NULL};
  return bt_command(connect, output, sizeof(output), error, error_size);
}

bool bd_bt_disconnect(bd_bt *bt, const char *addr, char *error,
                      size_t error_size) {
  if (bt == NULL || addr == NULL || addr[0] == '\0') {
    set_error(error, error_size, "missing Bluetooth address");
    return false;
  }
  char output[512];
  char *extra[] = {"disconnect", (char *)addr, NULL};
  return bt_command(extra, output, sizeof(output), error, error_size);
}

bool bd_bt_forget(bd_bt *bt, const char *addr, char *error,
                  size_t error_size) {
  if (bt == NULL || addr == NULL || addr[0] == '\0') {
    set_error(error, error_size, "missing Bluetooth address");
    return false;
  }
  char output[512];
  char *extra[] = {"remove", (char *)addr, NULL};
  return bt_command(extra, output, sizeof(output), error, error_size);
}

static void run_quiet(char *const argv[]) {
  int code = 0;
  char error[64];
  bd_process_run(argv, NULL, 0, &code, error, sizeof(error));
}

bool bd_bt_suspend(char *error, size_t error_size) {
  char output[256];
  char *power[] = {"bluetoothctl", "power", "off", NULL};
  run_quiet(power);
  char *kill_bluealsa[] = {"killall", "bluealsa", NULL};
  run_quiet(kill_bluealsa);
  char *kill_bluetoothd[] = {"killall", "bluetoothd", NULL};
  run_quiet(kill_bluetoothd);
  char *rfkill[] = {"rfkill", "block", "bluetooth", NULL};
  if (!bd_process_run(rfkill, output, sizeof(output), NULL, error,
                      error_size)) {
    FILE *state = fopen("/sys/class/rfkill/rfkill0/state", "w");
    if (state != NULL) {
      fputs("0", state);
      fclose(state);
    }
  }
  if (error != NULL && error_size > 0) {
    error[0] = '\0';
  }
  return true;
}

bool bd_bt_resume(char *error, size_t error_size) {
  char output[256];
  char ignored[64];
  char *rfkill[] = {"rfkill", "unblock", "bluetooth", NULL};
  bd_process_run(rfkill, output, sizeof(output), NULL, ignored,
                 sizeof(ignored));
  const char *daemon = access("/usr/libexec/bluetooth/bluetoothd", X_OK) == 0
                           ? "/usr/libexec/bluetooth/bluetoothd"
                           : "/usr/sbin/bluetoothd";
  char *bluetoothd[] = {(char *)daemon, "-B", NULL};
  if (access(daemon, X_OK) == 0) {
    bd_process_spawn(bluetoothd, ignored, sizeof(ignored));
  }
  char *hci[] = {"hciconfig", "hci0", "up", NULL};
  run_quiet(hci);
  char *power[] = {"bluetoothctl", "power", "on", NULL};
  run_quiet(power);
  char *bluealsa[] = {"bluealsa", "-p", "a2dp-sink", NULL};
  bd_process_spawn(bluealsa, ignored, sizeof(ignored));
  if (error != NULL && error_size > 0) {
    error[0] = '\0';
  }
  return true;
}

const char *bd_bt_state_name(bd_bt_state state) {
  switch (state) {
    case BD_BT_STATE_DISABLED:
      return "disabled";
    case BD_BT_STATE_ENABLED:
      return "enabled";
    case BD_BT_STATE_CONNECTED:
      return "connected";
    case BD_BT_STATE_UNKNOWN:
    default:
      return "unknown";
  }
}
