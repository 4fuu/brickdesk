#define _XOPEN_SOURCE 700

#include "brickdesk/perf_client.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef BRICKDESK_TEST_SYSFS_DIR
#define BRICKDESK_TEST_SYSFS_DIR "tests/fixtures/sysfs"
#endif
#ifndef BRICKDESK_TEST_PERF_CONFIG_DIR
#define BRICKDESK_TEST_PERF_CONFIG_DIR "tests/fixtures/perf"
#endif

static int failures;

#define CHECK(condition, ...)                                                  \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "perf_client_test: FAIL: ");                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fputc('\n', stderr);                                                     \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static const char *g_copy_source;
static const char *g_copy_destination;

static bool make_dir(const char *path) {
  if (mkdir(path, 0777) == 0) {
    return true;
  }
  return errno == EEXIST;
}

static bool copy_file(const char *source, const char *destination) {
  FILE *input = fopen(source, "rb");
  if (input == NULL) {
    return false;
  }
  FILE *output = fopen(destination, "wb");
  if (output == NULL) {
    fclose(input);
    return false;
  }
  char buffer[4096];
  bool ok = true;
  for (;;) {
    size_t got = fread(buffer, 1, sizeof(buffer), input);
    if (got > 0 && fwrite(buffer, 1, got, output) != got) {
      ok = false;
      break;
    }
    if (got < sizeof(buffer)) {
      ok = ferror(input) ? false : ok;
      break;
    }
  }
  fclose(input);
  if (fclose(output) != 0) {
    ok = false;
  }
  return ok;
}

static int copy_entry(const char *path, const struct stat *info, int flag,
                      struct FTW *state) {
  (void)flag;
  (void)state;
  const char *relative = path + strlen(g_copy_source);
  char destination[2048];
  snprintf(destination, sizeof(destination), "%s%s", g_copy_destination,
           relative);
  if (S_ISDIR(info->st_mode)) {
    return make_dir(destination) ? 0 : -1;
  }
  return copy_file(path, destination) ? 0 : -1;
}

static bool copy_tree(const char *source, const char *destination) {
  if (!make_dir(destination)) {
    return false;
  }
  g_copy_source = source;
  g_copy_destination = destination;
  return nftw(source, copy_entry, 16, FTW_PHYS) == 0;
}

static void sleep_ms(int milliseconds) {
  struct timespec delay;
  delay.tv_sec = milliseconds / 1000;
  delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
  nanosleep(&delay, NULL);
}

static bool wait_for_path(const char *path, int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 10) {
    if (access(path, F_OK) == 0) {
      return true;
    }
    sleep_ms(10);
  }
  return false;
}

static void stop_daemon(pid_t pid) {
  if (pid <= 0) {
    return;
  }
  kill(pid, SIGTERM);
  for (int attempt = 0; attempt < 200; ++attempt) {
    int status = 0;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD)) {
      return;
    }
    sleep_ms(10);
  }
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
}

static bool read_text(const char *path, char *out, size_t out_size) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  bool ok = fgets(out, (int)out_size, file) != NULL;
  fclose(file);
  if (!ok) {
    return false;
  }
  out[strcspn(out, "\r\n")] = '\0';
  return true;
}

static pid_t start_daemon(const char *perfd, const char *sysfs_root,
                          const char *socket_path) {
  pid_t pid = fork();
  if (pid != 0) {
    return pid;
  }
  int null_fd = open("/dev/null", O_WRONLY);
  if (null_fd >= 0) {
    dup2(null_fd, STDOUT_FILENO);
    dup2(null_fd, STDERR_FILENO);
    close(null_fd);
  }
  execl(perfd, perfd, "--config-dir", BRICKDESK_TEST_PERF_CONFIG_DIR,
        "--sysfs-root", sysfs_root, "--socket", socket_path, (char *)NULL);
  _exit(127);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: bd_perf_client_test PERFD\n");
    return 2;
  }
  char root[] = "/tmp/bd-perf-client.XXXXXX";
  char *temp = mkdtemp(root);
  if (temp == NULL) {
    fprintf(stderr, "perf_client_test: mkdtemp: %s\n", strerror(errno));
    return 1;
  }
  char sysfs[1024];
  char socket_path[1024];
  snprintf(sysfs, sizeof(sysfs), "%s/sysfs", temp);
  snprintf(socket_path, sizeof(socket_path), "%s/perfd.sock", temp);
  if (!copy_tree(BRICKDESK_TEST_SYSFS_DIR, sysfs)) {
    fprintf(stderr, "perf_client_test: cannot copy sysfs fixture\n");
    return 1;
  }

  pid_t daemon = start_daemon(argv[1], sysfs, socket_path);
  CHECK(daemon > 0, "start perfd");
  if (daemon <= 0 || !wait_for_path(socket_path, 3000)) {
    fprintf(stderr, "perf_client_test: perfd socket did not appear\n");
    stop_daemon(daemon);
    return 1;
  }

  char error[256] = {0};
  bd_perf_client_result result;
  bd_perf_client_status status = bd_perf_client_set_global(
      socket_path, "performance", 1000, &result, error, sizeof(error));
  CHECK(status == BD_PERF_CLIENT_OK, "apply performance: %s (%s)",
        bd_perf_client_status_name(status), error);
  CHECK(strcmp(result.profile, "performance") == 0, "profile %s",
        result.profile);
  CHECK(strcmp(result.governor, "performance") == 0, "governor %s",
        result.governor);
  CHECK(result.min_khz == 816000, "min %d", result.min_khz);
  CHECK(result.max_khz == 1800000, "max %d", result.max_khz);

  char path[1200];
  char value[128];
  snprintf(path, sizeof(path),
           "%s/sys/devices/system/cpu/cpufreq/policy0/scaling_governor", sysfs);
  CHECK(read_text(path, value, sizeof(value)) &&
            strcmp(value, "performance") == 0,
        "governor file %s", value);
  snprintf(path, sizeof(path),
           "%s/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq", sysfs);
  CHECK(read_text(path, value, sizeof(value)) &&
            strcmp(value, "816000") == 0,
        "min file %s", value);
  snprintf(path, sizeof(path),
           "%s/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq", sysfs);
  CHECK(read_text(path, value, sizeof(value)) &&
            strcmp(value, "1800000") == 0,
        "max file %s", value);

  error[0] = '\0';
  status = bd_perf_client_set_global(socket_path, "no-such-profile", 1000,
                                     &result, error, sizeof(error));
  CHECK(status == BD_PERF_CLIENT_REJECTED, "unknown profile status %s",
        bd_perf_client_status_name(status));
  CHECK(strstr(error, "unknown profile") != NULL, "unknown profile error %s",
        error);

  status = bd_perf_client_set_global(socket_path, "powersave", 1000, &result,
                                     error, sizeof(error));
  CHECK(status == BD_PERF_CLIENT_OK, "apply powersave: %s (%s)",
        bd_perf_client_status_name(status), error);
  snprintf(path, sizeof(path),
           "%s/sys/devices/system/cpu/cpufreq/policy0/scaling_governor", sysfs);
  CHECK(read_text(path, value, sizeof(value)) &&
            strcmp(value, "powersave") == 0,
        "powersave governor file %s", value);

  stop_daemon(daemon);
  daemon = -1;
  error[0] = '\0';
  status = bd_perf_client_set_global(socket_path, "default", 250, &result,
                                     error, sizeof(error));
  CHECK(status == BD_PERF_CLIENT_UNAVAILABLE,
        "disconnected status %s", bd_perf_client_status_name(status));
  CHECK(error[0] != '\0', "disconnected error is empty");

  unlink(socket_path);
  char cleanup[1400];
  snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", temp);
  if (system(cleanup) != 0) {
    fprintf(stderr, "perf_client_test: cleanup failed\n");
  }
  if (failures != 0) {
    fprintf(stderr, "perf_client_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("perf_client_test: ok\n");
  return 0;
}
