#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "brickdesk/device_bt.h"

static int failures;

#define CHECK(condition, ...)                   \
  do {                                          \
    if (!(condition)) {                         \
      fprintf(stderr, "device_bt_test: FAIL: ");\
      fprintf(stderr, __VA_ARGS__);             \
      fputc('\n', stderr);                      \
      ++failures;                               \
    }                                           \
  } while (0)

static bool write_script(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs(text, file) >= 0;
  if (fclose(file) != 0) {
    ok = false;
  }
  chmod(path, 0755);
  return ok;
}

static bool read_text(const char *path, char *out, size_t size) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  size_t length = fread(out, 1, size - 1, file);
  out[length] = '\0';
  fclose(file);
  return true;
}

int main(void) {
  char root[] = "/tmp/bd-bt-test-XXXXXX";
  if (mkdtemp(root) == NULL) {
    return 1;
  }
  char bin[256];
  char log[256];
  snprintf(bin, sizeof(bin), "%s/bin", root);
  snprintf(log, sizeof(log), "%s/calls.log", root);
  mkdir(bin, 0755);

  char path[1024];
  snprintf(path, sizeof(path), "%s/bluetoothctl", bin);
  char script[4096];
  snprintf(script, sizeof(script),
           "#!/bin/sh\n"
           "echo \"bluetoothctl $@\" >> '%s'\n"
           "case \"$*\" in\n"
           "  *'show'*) printf 'Controller AA:BB:CC:DD:EE:FF\\n"
           "\\tPowered: yes\\n\\tDiscovering: no\\n\\tName: BrickBT\\n' ;;\n"
           "  *'devices Connected'*) printf 'Device 11:22:33:44:55:66 Pad\\n' ;;\n"
           "  *'devices'*) printf 'Device 11:22:33:44:55:66 Pad\\n"
           "Device 77:88:99:AA:BB:CC Headphones\\n' ;;\n"
           "  *) echo OK ;;\n"
           "esac\n",
           log);
  CHECK(write_script(path, script), "write bluetoothctl stub");

  const char *old_path = getenv("PATH");
  char full_path[2048];
  snprintf(full_path, sizeof(full_path), "%s:%s", bin,
           old_path != NULL ? old_path : "/usr/bin:/bin");
  setenv("PATH", full_path, 1);

  bd_bt *bt = bd_bt_create();
  CHECK(bt != NULL, "create");
  if (bt == NULL) {
    return 1;
  }
  bd_bt_status status;
  CHECK(bd_bt_get_status(bt, &status), "status");
  CHECK(status.state == BD_BT_STATE_CONNECTED, "state %s",
        bd_bt_state_name(status.state));
  CHECK(strcmp(status.connected_name, "Pad") == 0, "connected %s",
        status.connected_name);
  CHECK(strcmp(status.connected_addr, "11:22:33:44:55:66") == 0,
        "address %s", status.connected_addr);

  bd_bt_device devices[8];
  size_t count = 0;
  char error[256] = {0};
  CHECK(bd_bt_list_devices(bt, devices, 8, &count, error, sizeof(error)),
        "list: %s", error);
  CHECK(count == 2, "device count %zu", count);
  if (count == 2) {
    CHECK(devices[0].connected, "Pad should be connected");
    CHECK(!devices[1].connected, "Headphones should not be connected");
    CHECK(strcmp(devices[1].addr, "77:88:99:AA:BB:CC") == 0, "second addr");
  }

  CHECK(bd_bt_connect(bt, "77:88:99:AA:BB:CC", error, sizeof(error)),
        "connect: %s", error);
  char calls[8192];
  CHECK(read_text(log, calls, sizeof(calls)), "read log");
  CHECK(strstr(calls, "pair 77:88:99:AA:BB:CC") != NULL, "pair");
  CHECK(strstr(calls, "trust 77:88:99:AA:BB:CC") != NULL, "trust");
  CHECK(strstr(calls, "connect 77:88:99:AA:BB:CC") != NULL, "connect");
  CHECK(bd_bt_disconnect(bt, "77:88:99:AA:BB:CC", error, sizeof(error)),
        "disconnect: %s", error);
  CHECK(bd_bt_forget(bt, "77:88:99:AA:BB:CC", error, sizeof(error)),
        "forget: %s", error);
  CHECK(bd_bt_set_powered(bt, false, error, sizeof(error)),
        "power off: %s", error);
  CHECK(read_text(log, calls, sizeof(calls)), "read log after power");
  CHECK(strstr(calls, "power off") != NULL, "power off command");

  CHECK(bd_bt_suspend(error, sizeof(error)), "suspend: %s", error);
  CHECK(bd_bt_resume(error, sizeof(error)), "resume: %s", error);

  bd_bt_destroy(bt);
  char cleanup[600];
  snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", root);
  if (system(cleanup) != 0) {
    fprintf(stderr, "device_bt_test: cleanup failed\n");
  }
  if (failures != 0) {
    fprintf(stderr, "device_bt_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("device_bt_test: ok\n");
  return 0;
}
