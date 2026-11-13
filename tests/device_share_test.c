#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "brickdesk/device_share.h"

static int failures;

#define CHECK(condition, ...)                      \
  do {                                             \
    if (!(condition)) {                            \
      fprintf(stderr, "device_share_test: FAIL: ");\
      fprintf(stderr, __VA_ARGS__);                \
      fputc('\n', stderr);                         \
      ++failures;                                  \
    }                                              \
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
  char root[] = "/tmp/bd-share-test-XXXXXX";
  if (mkdtemp(root) == NULL) {
    return 1;
  }
  char bin[256];
  char log[256];
  snprintf(bin, sizeof(bin), "%s/bin", root);
  snprintf(log, sizeof(log), "%s/calls.log", root);
  mkdir(bin, 0755);

  char path[1024];
  snprintf(path, sizeof(path), "%s/pgrep", bin);
  CHECK(write_script(path,
                     "#!/bin/sh\ncase \"$2\" in sshd) exit 0;; *) exit 1;; "
                     "esac\n"),
        "pgrep stub");
  snprintf(path, sizeof(path), "%s/killall", bin);
  char script[2048];
  snprintf(script, sizeof(script),
           "#!/bin/sh\necho \"killall $@\" >> '%s'\nexit 0\n", log);
  CHECK(write_script(path, script), "killall stub");
  snprintf(path, sizeof(path), "%s/ip", bin);
  snprintf(script, sizeof(script),
           "#!/bin/sh\necho '2: wlan0    inet 10.0.0.2/24 brd "
           "10.0.0.255 scope global wlan0'\n");
  CHECK(write_script(path, script), "ip stub");
  snprintf(path, sizeof(path), "%s/smbd", bin);
  snprintf(script, sizeof(script),
           "#!/bin/sh\necho \"smbd $@\" >> '%s'\nexit 0\n", log);
  CHECK(write_script(path, script), "smbd stub");

  const char *old_path = getenv("PATH");
  char full_path[2048];
  snprintf(full_path, sizeof(full_path), "%s:%s", bin,
           old_path != NULL ? old_path : "/usr/bin:/bin");
  setenv("PATH", full_path, 1);

  bd_share *share = bd_share_create("wlan0");
  CHECK(share != NULL, "create");
  if (share == NULL) {
    return 1;
  }
  bd_share_status status;
  CHECK(bd_share_get_status(share, &status), "status");
  CHECK(status.ssh, "sshd should be running");
  CHECK(!status.samba, "smbd should not be running");
  CHECK(strcmp(status.ip, "10.0.0.2") == 0, "ip %s", status.ip);

  char error[256] = {0};
  CHECK(bd_share_set_service(share, BD_SHARE_SSH, false, error,
                             sizeof(error)),
        "stop ssh: %s", error);
  char calls[4096];
  CHECK(read_text(log, calls, sizeof(calls)), "read log");
  CHECK(strstr(calls, "killall sshd") != NULL, "killall sshd");
  CHECK(strstr(calls, "killall dropbear") != NULL, "killall dropbear");

  CHECK(bd_share_set_service(share, BD_SHARE_SAMBA, true, error,
                             sizeof(error)),
        "start samba: %s", error);
  struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000L};
  nanosleep(&delay, NULL);
  CHECK(read_text(log, calls, sizeof(calls)), "read log after samba");
  CHECK(strstr(calls, "smbd -D") != NULL, "smbd -D");
  CHECK(!bd_share_set_usb_mode(share, "dock", error, sizeof(error)),
        "usbmode without script must fail");
  CHECK(strstr(error, "saved only") != NULL, "usbmode error %s", error);

  bd_share_destroy(share);
  char cleanup[600];
  snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", root);
  if (system(cleanup) != 0) {
    fprintf(stderr, "device_share_test: cleanup failed\n");
  }
  if (failures != 0) {
    fprintf(stderr, "device_share_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("device_share_test: ok\n");
  return 0;
}
