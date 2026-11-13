#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/input.h>
#else
#error "brickdesk inputd tests need Linux evdev"
#endif

#define TEST_PATH_MAX 512
#define TEST_LINE_MAX 4096

static char g_root[TEST_PATH_MAX];
static char g_socket[TEST_PATH_MAX];
static char g_log[TEST_PATH_MAX];
static const char *g_inputd;
static const char *g_config_dir;
static pid_t g_daemon = -1;
static unsigned g_stage;

typedef struct test_client {
  int fd;
  char buffer[TEST_LINE_MAX * 2];
  size_t length;
} test_client;

typedef struct fake_device {
  int fd;
  char name[128];
  char path[TEST_PATH_MAX];
  char meta[TEST_PATH_MAX];
} fake_device;

static uint64_t now_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void sleep_ms(int milliseconds) {
  struct timespec delay;
  delay.tv_sec = milliseconds / 1000;
  delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
  nanosleep(&delay, NULL);
}

static void dump_log(void) {
  FILE *file = fopen(g_log, "r");
  if (file == NULL) {
    return;
  }
  fputs("--- inputd log ---\n", stderr);
  char line[1024];
  while (fgets(line, sizeof(line), file) != NULL) {
    fputs(line, stderr);
  }
  fputs("--- end inputd log ---\n", stderr);
  fclose(file);
}

static void fail(const char *format, ...) {
  va_list args;
  va_start(args, format);
  fputs("inputd test: FAIL: ", stderr);
  vfprintf(stderr, format, args);
  fputc('\n', stderr);
  va_end(args);
  dump_log();
  exit(1);
}

#define CHECK(condition, ...)                                                  \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fail(__VA_ARGS__);                                                       \
    }                                                                          \
  } while (0)

static void join_path(char *out, size_t out_size, const char *directory,
                      const char *name) {
  size_t directory_length = strlen(directory);
  size_t name_length = strlen(name);
  CHECK(directory_length + name_length + 2 <= out_size, "path too long: %s/%s",
        directory, name);
  memcpy(out, directory, directory_length);
  out[directory_length] = '/';
  memcpy(out + directory_length + 1, name, name_length + 1);
}

static void append_suffix(char *out, size_t out_size, const char *base,
                          const char *suffix) {
  size_t base_length = strlen(base);
  size_t suffix_length = strlen(suffix);
  CHECK(base_length + suffix_length + 1 <= out_size, "path too long: %s%s",
        base, suffix);
  memcpy(out, base, base_length);
  memcpy(out + base_length, suffix, suffix_length + 1);
}

static void stop_daemon(void) {
  if (g_daemon <= 0) {
    return;
  }
  kill(g_daemon, SIGTERM);
  for (int attempt = 0; attempt < 300; ++attempt) {
    int status = 0;
    pid_t result = waitpid(g_daemon, &status, WNOHANG);
    if (result == g_daemon || (result < 0 && errno == ECHILD)) {
      g_daemon = -1;
      return;
    }
    sleep_ms(10);
  }
  kill(g_daemon, SIGKILL);
  waitpid(g_daemon, NULL, 0);
  g_daemon = -1;
}

static int remove_entry(const char *path, const struct stat *status, int flag,
                        struct FTW *walk) {
  (void)status;
  (void)flag;
  (void)walk;
  return remove(path);
}

static void cleanup(void) {
  stop_daemon();
  if (g_root[0] != '\0') {
    nftw(g_root, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
  }
}

static void start_daemon(const char *device_dir) {
  stop_daemon();
  FILE *log = fopen(g_log, "wb");
  CHECK(log != NULL, "cannot create %s: %s", g_log, strerror(errno));

  pid_t pid = fork();
  CHECK(pid >= 0, "fork: %s", strerror(errno));
  if (pid == 0) {
    if (dup2(fileno(log), STDOUT_FILENO) < 0 ||
        dup2(fileno(log), STDERR_FILENO) < 0) {
      _exit(127);
    }
    fclose(log);
    execl(g_inputd, g_inputd, "--config-dir", g_config_dir, "--socket",
          g_socket, "--device-dir", device_dir, "--verbose", (char *)NULL);
    _exit(127);
  }
  fclose(log);
  g_daemon = pid;
}

static test_client connect_client(void) {
  test_client client;
  memset(&client, 0, sizeof(client));
  client.fd = -1;

  for (int attempt = 0; attempt < 300; ++attempt) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
      struct sockaddr_un address;
      memset(&address, 0, sizeof(address));
      address.sun_family = AF_UNIX;
      size_t socket_length = strlen(g_socket);
      CHECK(socket_length < sizeof(address.sun_path),
            "socket path too long: %s", g_socket);
      memcpy(address.sun_path, g_socket, socket_length + 1);
      if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
          fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
        client.fd = fd;
        return client;
      }
      close(fd);
    }
    sleep_ms(10);
  }
  fail("inputd socket did not appear at %s", g_socket);
  return client;
}

static void client_close(test_client *client) {
  if (client->fd >= 0) {
    close(client->fd);
    client->fd = -1;
  }
}

static void client_send(test_client *client, const char *line) {
  char buffer[512];
  int written = snprintf(buffer, sizeof(buffer), "%s\n", line);
  CHECK(written > 0 && (size_t)written < sizeof(buffer),
        "client command too long");
  size_t sent = 0;
  while (sent < (size_t)written) {
    ssize_t amount =
        send(client->fd, buffer + sent, (size_t)written - sent, MSG_NOSIGNAL);
    if (amount > 0) {
      sent += (size_t)amount;
      continue;
    }
    if (amount < 0 && (errno == EINTR || errno == EAGAIN ||
                       errno == EWOULDBLOCK)) {
      struct pollfd poll_fd;
      poll_fd.fd = client->fd;
      poll_fd.events = POLLOUT;
      poll_fd.revents = 0;
      if (poll(&poll_fd, 1, 1000) > 0) {
        continue;
      }
    }
    fail("client send: %s", strerror(errno));
  }
}

static bool client_line(test_client *client, int timeout_ms, char *line,
                        size_t line_size) {
  uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
  for (;;) {
    char *newline = memchr(client->buffer, '\n', client->length);
    if (newline != NULL) {
      size_t length = (size_t)(newline - client->buffer);
      size_t copy = length < line_size - 1 ? length : line_size - 1;
      memcpy(line, client->buffer, copy);
      line[copy] = '\0';
      size_t consumed = length + 1;
      memmove(client->buffer, client->buffer + consumed,
              client->length - consumed);
      client->length -= consumed;
      return true;
    }

    uint64_t now = now_ms();
    if (now >= deadline) {
      return false;
    }
    struct pollfd poll_fd;
    poll_fd.fd = client->fd;
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;
    int ready = poll(&poll_fd, 1, (int)(deadline - now));
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (ready == 0) {
      return false;
    }
    if ((poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      return false;
    }
    ssize_t amount = recv(client->fd, client->buffer + client->length,
                          sizeof(client->buffer) - client->length, 0);
    if (amount <= 0) {
      if (amount < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    client->length += (size_t)amount;
    CHECK(client->length < sizeof(client->buffer), "client line overflow");
  }
}

static const char *expect_line2(test_client *client, int timeout_ms,
                                const char *first, const char *second) {
  static char line[TEST_LINE_MAX];
  uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
  for (;;) {
    uint64_t now = now_ms();
    if (now >= deadline ||
        !client_line(client, (int)(deadline - now), line, sizeof(line))) {
      fail("timeout waiting for '%s'%s%s", first, second != NULL ? "' and '" : "",
           second != NULL ? second : "");
    }
    if (strstr(line, first) != NULL &&
        (second == NULL || strstr(line, second) != NULL)) {
      return line;
    }
  }
}

static const char *expect_line(test_client *client, int timeout_ms,
                               const char *needle) {
  return expect_line2(client, timeout_ms, needle, NULL);
}

static int count_text(const char *text, const char *needle) {
  int count = 0;
  size_t length = strlen(needle);
  const char *cursor = text;
  while ((cursor = strstr(cursor, needle)) != NULL) {
    ++count;
    cursor += length;
  }
  return count;
}

static void write_text_file(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  CHECK(file != NULL, "cannot write %s: %s", path, strerror(errno));
  CHECK(fputs(text, file) >= 0, "cannot write %s", path);
  CHECK(fclose(file) == 0, "cannot close %s", path);
}

static void fake_plug(fake_device *device, const char *device_dir, int index,
                      const char *name, const char *extra) {
  memset(device, 0, sizeof(*device));
  device->fd = -1;
  snprintf(device->name, sizeof(device->name), "%s", name);

  char stage[TEST_PATH_MAX];
  char stage_name[64];
  snprintf(stage_name, sizeof(stage_name), "stage-%u-XXXXXX", g_stage++);
  join_path(stage, sizeof(stage), g_root, stage_name);
  CHECK(mkdtemp(stage) != NULL, "mkdtemp: %s", strerror(errno));

  char event_name[32];
  snprintf(event_name, sizeof(event_name), "event%d", index);
  char fifo[TEST_PATH_MAX];
  char meta[TEST_PATH_MAX];
  join_path(fifo, sizeof(fifo), stage, event_name);
  append_suffix(meta, sizeof(meta), fifo, ".meta");
  CHECK(mkfifo(fifo, 0600) == 0, "mkfifo %s: %s", fifo, strerror(errno));

  device->fd = open(fifo, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  CHECK(device->fd >= 0, "open %s: %s", fifo, strerror(errno));

  char metadata[512];
  int written = snprintf(metadata, sizeof(metadata),
                         "name=%s\nvendor=0xbeef\nproduct=0x0001\n%s", name,
                         extra != NULL ? extra : "");
  CHECK(written > 0 && (size_t)written < sizeof(metadata),
        "metadata too long");
  write_text_file(meta, metadata);

  join_path(device->path, sizeof(device->path), device_dir, event_name);
  append_suffix(device->meta, sizeof(device->meta), device->path, ".meta");
  CHECK(rename(meta, device->meta) == 0, "rename %s: %s", meta,
        strerror(errno));
  CHECK(rename(fifo, device->path) == 0, "rename %s: %s", fifo,
        strerror(errno));
}

static void fake_unplug(fake_device *device) {
  if (device->fd >= 0) {
    close(device->fd);
    device->fd = -1;
  }
  unlink(device->path);
  unlink(device->meta);
}

static void fake_send(fake_device *device, unsigned type, unsigned code,
                      int value) {
  struct input_event event;
  memset(&event, 0, sizeof(event));
  event.type = (uint16_t)type;
  event.code = (uint16_t)code;
  event.value = value;
  ssize_t amount = write(device->fd, &event, sizeof(event));
  CHECK(amount == (ssize_t)sizeof(event), "fake write: %s", strerror(errno));
}

static void fake_key(fake_device *device, unsigned code, int value) {
  fake_send(device, EV_KEY, code, value);
}

static void fake_switch(fake_device *device, unsigned code, int value) {
  fake_send(device, EV_SW, code, value);
}

static bool wait_removed_with_key(test_client *client, const char *device_name,
                                  const char *key_name, int timeout_ms,
                                  bool *other_key_released,
                                  bool *global_release) {
  uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
  bool released = false;
  bool removed = false;
  char needle[256];
  char released_key[128];
  snprintf(needle, sizeof(needle), "\"action\":\"removed\",\"device\":\"%s\"",
           device_name);
  snprintf(released_key, sizeof(released_key),
           "\"type\":\"key\",\"key\":\"%s\",\"pressed\":false", key_name);

  while (now_ms() < deadline) {
    char line[TEST_LINE_MAX];
    uint64_t now = now_ms();
    if (!client_line(client, (int)(deadline - now), line, sizeof(line))) {
      break;
    }
    if (strstr(line, needle) != NULL) {
      removed = true;
      break;
    }
    if (strstr(line, released_key) != NULL) {
      released = true;
    }
    if (other_key_released != NULL &&
        strstr(line, "\"type\":\"key\",\"key\":\"B\",\"pressed\":false") !=
            NULL) {
      *other_key_released = true;
    }
    if (global_release != NULL &&
        strstr(line, "\"type\":\"release_all\"") != NULL) {
      *global_release = true;
    }
  }
  return removed && released;
}

static long long process_cpu_ticks(void) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%ld/stat", (long)g_daemon);
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return -1;
  }
  char line[2048];
  if (fgets(line, sizeof(line), file) == NULL) {
    fclose(file);
    return -1;
  }
  fclose(file);
  char *after = strrchr(line, ')');
  if (after == NULL) {
    return -1;
  }
  char *save = NULL;
  long long utime = -1;
  long long stime = -1;
  int index = 0;
  for (char *token = strtok_r(after + 2, " ", &save); token != NULL;
       token = strtok_r(NULL, " ", &save), ++index) {
    if (index == 11) {
      utime = strtoll(token, NULL, 10);
    } else if (index == 12) {
      stime = strtoll(token, NULL, 10);
      break;
    }
  }
  if (utime < 0 || stime < 0) {
    return -1;
  }
  return utime + stime;
}

static void test_lifecycle(void) {
  char device_dir[TEST_PATH_MAX];
  join_path(device_dir, sizeof(device_dir), g_root, "input-life");

  /* Startup with no device directory: socket must still come up. */
  start_daemon(device_dir);
  test_client client = connect_client();
  const char *hello = expect_line(&client, 3000, "\"type\":\"hello\"");
  CHECK(strstr(hello, "\"devices\":[]") != NULL,
        "expected an empty device list: %s", hello);

  /* Missing directory retries must use a bounded wait, not a busy loop. */
  sleep_ms(200);
  long long missing_before = process_cpu_ticks();
  sleep_ms(600);
  long long missing_after = process_cpu_ticks();
  CHECK(missing_before >= 0 && missing_after >= 0,
        "cannot read daemon CPU time");
  CHECK(missing_after - missing_before < 20,
        "daemon consumed %lld ticks while the device directory was missing",
        missing_after - missing_before);

  CHECK(mkdir(device_dir, 0755) == 0, "mkdir %s: %s", device_dir,
        strerror(errno));

  fake_device a;
  fake_plug(&a, device_dir, 0, "Fake A", NULL);
  expect_line(&client, 3000,
              "\"action\":\"added\",\"device\":\"Fake A\"");

  test_client hello_client = connect_client();
  hello = expect_line(&hello_client, 3000, "\"type\":\"hello\"");
  CHECK(count_text(hello, "Fake A") == 1,
        "device registered more than once: %s", hello);
  client_close(&hello_client);

  client_send(&client, "{\"cmd\":\"subscribe\",\"actions\":true,\"keys\":true}");

  fake_key(&a, BTN_SOUTH, 1);
  expect_line2(&client, 3000, "\"type\":\"key\",\"key\":\"A\"",
               "\"pressed\":true");

  fake_device b;
  fake_plug(&b, device_dir, 1, "Fake B", NULL);
  expect_line(&client, 3000,
              "\"action\":\"added\",\"device\":\"Fake B\"");
  fake_key(&b, BTN_EAST, 1);
  expect_line2(&client, 3000, "\"type\":\"key\",\"key\":\"B\"",
               "\"pressed\":true");

  /* Unplug A while both devices hold a key: only A is released. */
  fake_unplug(&a);
  bool b_released_early = false;
  bool global_release = false;
  CHECK(wait_removed_with_key(&client, "Fake A", "A", 3000,
                              &b_released_early, &global_release),
        "removal did not release A and report the device removal");
  CHECK(!b_released_early, "removing A released B's held key");
  CHECK(!global_release,
        "removing A sent the global release_all notification");

  fake_key(&b, BTN_EAST, 0);
  expect_line2(&client, 3000, "\"type\":\"key\",\"key\":\"B\"",
               "\"pressed\":false");

  /* Replug: the same path is re-matched and registered exactly once. */
  fake_plug(&a, device_dir, 0, "Fake A", NULL);
  expect_line(&client, 3000,
              "\"action\":\"added\",\"device\":\"Fake A\"");
  char decoy[TEST_PATH_MAX];
  join_path(decoy, sizeof(decoy), device_dir, "decoy");
  write_text_file(decoy, "not an event node\n");
  sleep_ms(150);

  hello_client = connect_client();
  hello = expect_line(&hello_client, 3000, "\"type\":\"hello\"");
  CHECK(count_text(hello, "Fake A") == 1,
        "replug registered Fake A more than once: %s", hello);
  CHECK(count_text(hello, "Fake B") == 1,
        "rescan registered Fake B more than once: %s", hello);
  client_close(&hello_client);

  fake_key(&a, BTN_SOUTH, 1);
  expect_line2(&client, 3000, "\"type\":\"key\",\"key\":\"A\"",
               "\"pressed\":true");
  fake_key(&a, BTN_SOUTH, 0);
  expect_line2(&client, 3000, "\"type\":\"key\",\"key\":\"A\"",
               "\"pressed\":false");

  /* A hold action must get its release phase when the device disappears. */
  fake_key(&a, BTN_MODE, 1);
  expect_line2(&client, 1500, "\"action\":\"menu.open\"", "\"phase\":\"down\"");
  fake_unplug(&a);

  uint64_t deadline = now_ms() + 3000;
  bool action_up = false;
  bool removed = false;
  while (now_ms() < deadline) {
    char line[TEST_LINE_MAX];
    uint64_t now = now_ms();
    if (!client_line(&client, (int)(deadline - now), line, sizeof(line))) {
      break;
    }
    if (strstr(line, "\"action\":\"menu.open\"") != NULL &&
        strstr(line, "\"phase\":\"up\"") != NULL) {
      action_up = true;
    }
    if (strstr(line, "\"action\":\"removed\",\"device\":\"Fake A\"") != NULL) {
      removed = true;
      break;
    }
  }
  CHECK(removed, "hold device removal was not reported");
  CHECK(action_up, "hold action was not released on device removal");

  /* The existing headphone switch protocol must keep working. */
  fake_device headphones;
  fake_plug(&headphones, device_dir, 3, "Fake Switch",
            "keys=\nabs=\nsw=SW_HEADPHONE_INSERT\n");
  expect_line(&client, 3000,
              "\"action\":\"added\",\"device\":\"Fake Switch\"");
  fake_switch(&headphones, SW_HEADPHONE_INSERT, 1);
  expect_line2(&client, 3000, "\"type\":\"switch\"",
               "\"switch\":\"headphone\",\"value\":1");
  fake_unplug(&headphones);

  /* Idle daemon with a live device and watch must block in poll(). */
  sleep_ms(200);
  long long before = process_cpu_ticks();
  sleep_ms(600);
  long long after = process_cpu_ticks();
  CHECK(before >= 0 && after >= 0, "cannot read daemon CPU time");
  CHECK(after - before < 20,
        "daemon consumed %lld ticks while idle (busy loop)", after - before);

  client_close(&client);
  stop_daemon();
}

static void test_grab(void) {
  char device_dir[TEST_PATH_MAX];
  join_path(device_dir, sizeof(device_dir), g_root, "input-grab");
  CHECK(mkdir(device_dir, 0755) == 0, "mkdir %s: %s", device_dir,
        strerror(errno));

  /* A device that refuses EVIOCGRAB makes the explicit grab fail. */
  fake_device denied;
  fake_plug(&denied, device_dir, 0, "Deny D", "grab=deny\n");
  start_daemon(device_dir);

  test_client client = connect_client();
  const char *hello = expect_line(&client, 3000, "\"type\":\"hello\"");
  CHECK(strstr(hello, "Deny D") != NULL, "startup device missing: %s", hello);

  client_send(&client, "{\"cmd\":\"grab\"}");
  expect_line2(&client, 3000, "\"type\":\"error\",\"cmd\":\"grab\"",
               "\"error\":\"Device or resource busy\"");

  fake_unplug(&denied);
  expect_line(&client, 3000,
              "\"action\":\"removed\",\"device\":\"Deny D\"");

  /* Grab succeeds once the ungrabbable device is gone. */
  fake_device a;
  fake_plug(&a, device_dir, 0, "Allow A", NULL);
  expect_line(&client, 3000,
              "\"action\":\"added\",\"device\":\"Allow A\"");
  client_send(&client, "{\"cmd\":\"grab\"}");
  expect_line(&client, 3000, "\"type\":\"grab\",\"active\":true");

  /* A denied device appearing during the grab is diagnosed and excluded. */
  fake_device b;
  fake_plug(&b, device_dir, 1, "Deny B", "grab=deny\n");
  expect_line(&client, 3000,
              "\"message\":\"hotplug_grab_failed\"");

  test_client hello_client = connect_client();
  hello = expect_line(&hello_client, 3000, "\"type\":\"hello\"");
  CHECK(strstr(hello, "Deny B") == NULL,
        "ungrabbable hotplug device was registered as grabbed: %s", hello);
  CHECK(strstr(hello, "Allow A") != NULL,
        "existing grabbed device disappeared: %s", hello);
  client_close(&hello_client);

  /* A grabbable device is grabbed before it is registered. */
  fake_device c;
  fake_plug(&c, device_dir, 2, "Allow C", NULL);
  expect_line2(&client, 3000,
               "\"action\":\"added\",\"device\":\"Allow C\"",
               "\"grabbed\":true");

  hello_client = connect_client();
  hello = expect_line(&hello_client, 3000, "\"type\":\"hello\"");
  CHECK(count_text(hello, "Allow C") == 1,
        "hotplug device registered more than once: %s", hello);
  CHECK(strstr(hello, "Deny B") == NULL,
        "denied device appeared later: %s", hello);
  client_close(&hello_client);

  fake_key(&c, BTN_SOUTH, 1);
  expect_line2(&client, 3000, "\"type\":\"key\",\"key\":\"A\"",
               "\"source\":\"Allow C\"");
  fake_unplug(&c);

  uint64_t deadline = now_ms() + 3000;
  bool released = false;
  bool removed = false;
  bool grab_ended = false;
  while (now_ms() < deadline) {
    char line[TEST_LINE_MAX];
    uint64_t now = now_ms();
    if (!client_line(&client, (int)(deadline - now), line, sizeof(line))) {
      break;
    }
    if (strstr(line, "\"type\":\"key\",\"key\":\"A\",\"pressed\":false") !=
        NULL) {
      released = true;
    }
    if (strstr(line, "\"action\":\"removed\",\"device\":\"Allow C\"") != NULL) {
      removed = true;
    }
    if (strstr(line, "\"type\":\"grab\",\"active\":false") != NULL) {
      grab_ended = true;
    }
    if (removed) {
      break;
    }
  }
  CHECK(removed, "grabbed hotplug device removal was not reported");
  CHECK(released, "grabbed hotplug device key was not released");
  CHECK(!grab_ended, "device removal silently ended the active grab");

  /* Remove the denied device before ungrab, so it is not retried as normal. */
  fake_unplug(&b);
  sleep_ms(100);
  client_send(&client, "{\"cmd\":\"ungrab\"}");
  expect_line(&client, 3000, "\"type\":\"release_all\"");
  expect_line(&client, 3000, "\"type\":\"grab\",\"active\":false");

  client_close(&client);
  stop_daemon();
}

static void print_usage(FILE *stream) {
  fprintf(stream,
          "usage: bd_inputd_test --inputd PATH --config-dir DIR\n");
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--inputd") == 0 && i + 1 < argc) {
      g_inputd = argv[++i];
    } else if (strcmp(argv[i], "--config-dir") == 0 && i + 1 < argc) {
      g_config_dir = argv[++i];
    } else if (strcmp(argv[i], "--help") == 0) {
      print_usage(stdout);
      return 0;
    } else {
      print_usage(stderr);
      return 2;
    }
  }
  if (g_inputd == NULL || g_config_dir == NULL) {
    print_usage(stderr);
    return 2;
  }

  snprintf(g_root, sizeof(g_root), "/tmp/brickdesk-inputd-XXXXXX");
  CHECK(mkdtemp(g_root) != NULL, "mkdtemp: %s", strerror(errno));
  join_path(g_socket, sizeof(g_socket), g_root, "inputd.sock");
  join_path(g_log, sizeof(g_log), g_root, "inputd.log");
  atexit(cleanup);

  test_lifecycle();
  printf("inputd_hotplug: lifecycle, replug, hold release and idle checks passed\n");
  test_grab();
  printf("inputd_hotplug: grab hotplug checks passed\n");
  return 0;
}
