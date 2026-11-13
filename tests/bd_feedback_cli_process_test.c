#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef BD_FEEDBACK_CLI_BIN
#define BD_FEEDBACK_CLI_BIN "build-host/bd-feedback"
#endif
#ifndef BD_FAKE_EVDEV_PRELOAD_PATH
#define BD_FAKE_EVDEV_PRELOAD_PATH "build-host/tests/bd-fake-evdev-preload.so"
#endif

#define MAX_EVENTS 64
#define LOG_TEXT_MAX 8192

static int g_failures;

#define CHECK(condition, ...)                          \
  do {                                                 \
    if (!(condition)) {                                \
      fprintf(stderr, "device_feedback_cli_test:%d: ", \
              __LINE__);                               \
      fprintf(stderr, __VA_ARGS__);                    \
      fprintf(stderr, "\n");                           \
      ++g_failures;                                    \
    }                                                  \
  } while (0)

typedef struct log_event {
  char name[32];
  long long timestamp_ms;
  long long value;
} log_event;

typedef struct child_result {
  int exit_code;
  int signal;
  long long elapsed_ms;
} child_result;

static bool log_contains(const char *path, const char *needle);

static bool make_log_path(char *out, size_t out_size) {
  int written = snprintf(out, out_size, "/tmp/bd-feedback-cli-XXXXXX");
  if (written < 0 || (size_t)written >= out_size) {
    return false;
  }
  int fd = mkstemp(out);
  if (fd < 0) {
    return false;
  }
  close(fd);
  return true;
}

static child_result run_cli(const char *log_path, bool upload_fail,
                            int duration_ms, int interrupt_after_ms) {
  child_result result;
  memset(&result, 0, sizeof(result));
  result.exit_code = -1;
  struct timespec start;
  struct timespec end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  pid_t pid = fork();
  if (pid < 0) {
    result.exit_code = -2;
    return result;
  }
  if (pid == 0) {
    setenv("LD_PRELOAD", BD_FAKE_EVDEV_PRELOAD_PATH, 1);
    setenv("BD_FAKE_EVDEV_LOG", log_path, 1);
    if (upload_fail) {
      setenv("BD_FAKE_EVDEV_UPLOAD_FAIL", "1", 1);
    } else {
      unsetenv("BD_FAKE_EVDEV_UPLOAD_FAIL");
    }
    char duration[32];
    snprintf(duration, sizeof(duration), "%d", duration_ms);
    char *const argv[] = {(char *)BD_FEEDBACK_CLI_BIN, "--event",
                          (char *)"/dev/input/event7", (char *)"rumble",
                          (char *)"play", (char *)"50", duration, NULL};
    execv(BD_FEEDBACK_CLI_BIN, argv);
    _exit(127);
  }

  if (interrupt_after_ms > 0) {
    for (int waited = 0; waited < interrupt_after_ms; waited += 5) {
      if (log_contains(log_path, "ff_play")) {
        break;
      }
      struct timespec delay = {0, 5 * 1000000L};
      nanosleep(&delay, NULL);
    }
    kill(pid, SIGINT);
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      result.exit_code = -3;
      return result;
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &end);
  result.elapsed_ms =
      (long long)(end.tv_sec - start.tv_sec) * 1000 +
      (long long)(end.tv_nsec - start.tv_nsec) / 1000000;
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.signal = WTERMSIG(status);
  }
  return result;
}

static bool read_log(const char *path, char *out, size_t out_size) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  size_t used = fread(out, 1, out_size - 1, file);
  out[used] = '\0';
  bool ok = !ferror(file);
  fclose(file);
  return ok;
}

static bool log_contains(const char *path, const char *needle) {
  char text[LOG_TEXT_MAX];
  return read_log(path, text, sizeof(text)) &&
         strstr(text, needle) != NULL;
}

static int parse_log(const char *text, log_event *events, int capacity) {
  char *copy = strdup(text);
  if (copy == NULL) {
    return -1;
  }
  int count = 0;
  char *save = NULL;
  for (char *line = strtok_r(copy, "\n", &save); line != NULL;
       line = strtok_r(NULL, "\n", &save)) {
    long long seconds = 0;
    long nanoseconds = 0;
    char name[32];
    long long value = 0;
    if (sscanf(line, "%lld.%ld %31s %lld", &seconds, &nanoseconds, name,
               &value) != 4) {
      continue;
    }
    if (count < capacity) {
      snprintf(events[count].name, sizeof(events[count].name), "%s", name);
      events[count].timestamp_ms =
          seconds * 1000 + nanoseconds / 1000000;
      events[count].value = value;
      ++count;
    }
  }
  free(copy);
  return count;
}

static int find_event(const log_event *events, int count, const char *name,
                      int start) {
  for (int i = start; i < count; ++i) {
    if (strcmp(events[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

static int count_event(const log_event *events, int count, const char *name) {
  int seen = 0;
  for (int i = 0; i < count; ++i) {
    if (strcmp(events[i].name, name) == 0) {
      ++seen;
    }
  }
  return seen;
}

static void test_cli_success(void) {
  char log_path[256];
  CHECK(make_log_path(log_path, sizeof(log_path)), "make success log");
  child_result result = run_cli(log_path, false, 200, 0);
  CHECK(result.exit_code == 0, "success exit %d signal %d", result.exit_code,
        result.signal);
  CHECK(result.elapsed_ms >= 150 && result.elapsed_ms < 2000,
        "success elapsed %lld ms", result.elapsed_ms);

  char text[LOG_TEXT_MAX];
  CHECK(read_log(log_path, text, sizeof(text)), "read success log");
  log_event events[MAX_EVENTS];
  int count = parse_log(text, events, MAX_EVENTS);
  CHECK(count > 0, "success log parse count %d", count);
  int upload = find_event(events, count, "upload", 0);
  int play = find_event(events, count, "ff_play", upload + 1);
  int stop = find_event(events, count, "ff_stop", play + 1);
  int erase = find_event(events, count, "erase", stop + 1);
  int close = find_event(events, count, "close", erase + 1);
  CHECK(upload >= 0 && play >= 0 && stop >= 0 && erase >= 0 && close >= 0,
        "success log order upload=%d play=%d stop=%d erase=%d close=%d",
        upload, play, stop, erase, close);
  if (play >= 0 && stop >= 0) {
    CHECK(events[stop].timestamp_ms - events[play].timestamp_ms >= 150,
          "success wait %lld ms",
          events[stop].timestamp_ms - events[play].timestamp_ms);
  }
  CHECK(count_event(events, count, "close") == 1,
        "success close count %d", count_event(events, count, "close"));
  unlink(log_path);
}

static void test_cli_failure_does_not_wait(void) {
  char log_path[256];
  CHECK(make_log_path(log_path, sizeof(log_path)), "make failure log");
  child_result result = run_cli(log_path, true, 1000, 0);
  CHECK(result.exit_code == 1, "failure exit %d signal %d", result.exit_code,
        result.signal);
  CHECK(result.elapsed_ms < 500, "failure elapsed %lld ms",
        result.elapsed_ms);

  char text[LOG_TEXT_MAX];
  CHECK(read_log(log_path, text, sizeof(text)), "read failure log");
  log_event events[MAX_EVENTS];
  int count = parse_log(text, events, MAX_EVENTS);
  CHECK(count_event(events, count, "upload_fail") == 1,
        "failure upload_fail count %d",
        count_event(events, count, "upload_fail"));
  CHECK(count_event(events, count, "ff_play") == 0,
        "failure unexpectedly played");
  CHECK(count_event(events, count, "close") == 1,
        "failure close count %d", count_event(events, count, "close"));
  int failed = find_event(events, count, "upload_fail", 0);
  int close = find_event(events, count, "close", failed + 1);
  CHECK(failed >= 0 && close >= 0, "failure cleanup order");
  unlink(log_path);
}

static void test_cli_interrupt_cleans_up(void) {
  char log_path[256];
  CHECK(make_log_path(log_path, sizeof(log_path)), "make interrupt log");
  child_result result = run_cli(log_path, false, 1000, 500);
  CHECK(result.exit_code == 130, "interrupt exit %d signal %d",
        result.exit_code, result.signal);
  CHECK(result.elapsed_ms < 800, "interrupt elapsed %lld ms",
        result.elapsed_ms);

  char text[LOG_TEXT_MAX];
  CHECK(read_log(log_path, text, sizeof(text)), "read interrupt log");
  log_event events[MAX_EVENTS];
  int count = parse_log(text, events, MAX_EVENTS);
  int play = find_event(events, count, "ff_play", 0);
  int stop = find_event(events, count, "ff_stop", play + 1);
  int erase = find_event(events, count, "erase", stop + 1);
  int close = find_event(events, count, "close", erase + 1);
  CHECK(play >= 0 && stop >= 0 && erase >= 0 && close >= 0,
        "interrupt order play=%d stop=%d erase=%d close=%d", play, stop, erase,
        close);
  CHECK(count_event(events, count, "close") == 1,
        "interrupt close count %d", count_event(events, count, "close"));
  unlink(log_path);
}

int main(void) {
  test_cli_success();
  test_cli_failure_does_not_wait();
  test_cli_interrupt_cleans_up();
  if (g_failures != 0) {
    fprintf(stderr, "device_feedback_cli_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("device_feedback_cli_test: all checks passed\n");
  return 0;
}
