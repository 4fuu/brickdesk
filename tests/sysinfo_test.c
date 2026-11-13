#define _XOPEN_SOURCE 700

#include "brickdesk/sysinfo.h"

#include <errno.h>
#include <ftw.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(condition, ...)                                                  \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "sysinfo_test: FAIL: ");                                 \
      fprintf(stderr, __VA_ARGS__);                                            \
      fputc('\n', stderr);                                                     \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static bool make_dir(const char *path) {
  if (mkdir(path, 0777) == 0) {
    return true;
  }
  return errno == EEXIST;
}

static bool join(char *out, size_t size, const char *a, const char *b) {
  int written = snprintf(out, size, "%s/%s", a, b);
  return written > 0 && (size_t)written < size;
}

static bool write_text(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs(text, file) >= 0;
  return fclose(file) == 0 && ok;
}

static int remove_entry(const char *path, const struct stat *info, int flag,
                        struct FTW *state) {
  (void)info;
  (void)flag;
  (void)state;
  return remove(path);
}

static void remove_tree(const char *path) {
  nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static void prepare_root(const char *root) {
  char path[512];
  snprintf(path, sizeof(path), "%s/sys", root);
  make_dir(path);
  snprintf(path, sizeof(path), "%s/sys/class", root);
  make_dir(path);
  snprintf(path, sizeof(path), "%s/sys/class/power_supply", root);
  make_dir(path);
  snprintf(path, sizeof(path), "%s/sys/class/net", root);
  make_dir(path);
}

static void test_battery(const char *root) {
  char supply[512];
  char path[512];
  snprintf(supply, sizeof(supply), "%s/sys/class/power_supply/BAT0", root);
  CHECK(make_dir(supply), "create battery fixture directory");
  CHECK(join(path, sizeof(path), supply, "type"), "battery type path");
  CHECK(write_text(path, "Battery\n"), "write battery type");
  CHECK(join(path, sizeof(path), supply, "capacity"), "battery capacity path");
  CHECK(write_text(path, "57\n"), "write battery capacity");

  int percent = -1;
  CHECK(bd_sys_battery_percent(root, &percent), "battery should be sampled");
  CHECK(percent == 57, "battery percent %d, wanted 57", percent);

  CHECK(write_text(path, "not-a-number\n"), "write invalid battery capacity");
  percent = 12;
  CHECK(!bd_sys_battery_percent(root, &percent), "invalid capacity is unknown");
  CHECK(percent == 12, "unknown battery must not overwrite output");

  CHECK(write_text(path, "999\n"), "write out-of-range battery capacity");
  CHECK(!bd_sys_battery_percent(root, &percent), "out-of-range is unknown");
}

static void test_wifi(const char *root) {
  char net[512];
  char iface[512];
  char wireless[512];
  char path[512];
  snprintf(net, sizeof(net), "%s/sys/class/net", root);
  bd_wifi_state state = bd_sys_wifi_state(root);
  CHECK(state == BD_WIFI_UNKNOWN, "missing /sys/class/net is unknown");

  CHECK(join(iface, sizeof(iface), net, "eth0"), "eth0 path");
  CHECK(make_dir(iface), "create eth0 fixture");
  CHECK(join(path, sizeof(path), iface, "operstate"), "eth0 state path");
  CHECK(write_text(path, "up\n"), "write eth0 operstate");
  CHECK(bd_sys_wifi_state(root) == BD_WIFI_UNKNOWN,
        "non-wireless interface is not Wi-Fi");

  CHECK(join(iface, sizeof(iface), net, "wlan0"), "wlan0 path");
  CHECK(make_dir(iface), "create wlan0 fixture");
  CHECK(join(wireless, sizeof(wireless), iface, "wireless"),
        "wireless dir path");
  CHECK(make_dir(wireless), "create wireless directory");
  CHECK(join(path, sizeof(path), iface, "operstate"), "wlan0 operstate path");
  CHECK(write_text(path, "down\n"), "write wlan0 down");
  CHECK(bd_sys_wifi_state(root) == BD_WIFI_DISCONNECTED,
        "down wlan0 is disconnected");
  CHECK(write_text(path, "up\n"), "write wlan0 up");
  CHECK(bd_sys_wifi_state(root) == BD_WIFI_CONNECTED,
        "up wlan0 is connected");
  CHECK(remove(path) == 0, "remove wlan0 operstate");
  CHECK(bd_sys_wifi_state(root) == BD_WIFI_UNKNOWN,
        "wireless without operstate is unknown");
}

static void test_memory(void) {
  int rss = bd_sys_process_tree_rss_mb((int)getpid());
  CHECK(rss >= 0, "self process tree rss is sampled, got %d", rss);
  CHECK(bd_sys_process_tree_rss_mb(-1) == -1, "invalid pid is unknown");
  CHECK(bd_sys_process_tree_rss_mb(99999999) == -1,
        "missing pid is unknown");

  int ready_pipe[2];
  if (pipe(ready_pipe) != 0) {
    CHECK(false, "create zombie sync pipe: %s", strerror(errno));
    return;
  }
  pid_t child = fork();
  if (child < 0) {
    CHECK(false, "fork zombie child: %s", strerror(errno));
    close(ready_pipe[0]);
    close(ready_pipe[1]);
    return;
  }
  if (child == 0) {
    close(ready_pipe[0]);
    char ready = '1';
    (void)write(ready_pipe[1], &ready, 1);
    close(ready_pipe[1]);
    _exit(0);
  }
  close(ready_pipe[1]);

  char ready = 0;
  ssize_t received = read(ready_pipe[0], &ready, 1);
  close(ready_pipe[0]);
  CHECK(received == 1, "zombie child signaled readiness, got %zd", received);

  siginfo_t info;
  memset(&info, 0, sizeof(info));
  if (waitid(P_PID, (id_t)child, &info, WEXITED | WNOWAIT) != 0) {
    CHECK(false, "wait for zombie child: %s", strerror(errno));
    waitpid(child, NULL, 0);
    return;
  }

  int zombie_root = bd_sys_process_tree_rss_mb((int)child);
  CHECK(zombie_root == -1, "zombie root rss is unknown, got %d",
        zombie_root);
  int zombie_tree = bd_sys_process_tree_rss_mb((int)getpid());
  CHECK(zombie_tree == -1, "tree containing a zombie is unknown, got %d",
        zombie_tree);

  CHECK(waitpid(child, NULL, 0) == child, "reap zombie child");
  rss = bd_sys_process_tree_rss_mb((int)getpid());
  CHECK(rss >= 0, "self process tree rss recovers after reaping, got %d", rss);
}

int main(void) {
  char root[] = "/tmp/bd_sysinfo.XXXXXX";
  if (mkdtemp(root) == NULL) {
    fprintf(stderr, "sysinfo_test: mkdtemp failed: %s\n", strerror(errno));
    return 1;
  }
  prepare_root(root);
  test_memory();
  test_battery(root);
  test_wifi(root);
  remove_tree(root);

  if (failures != 0) {
    fprintf(stderr, "sysinfo_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("sysinfo_test: ok\n");
  return 0;
}
