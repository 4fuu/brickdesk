#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

/*
 * Shell light-sleep integration.
 *
 * The shell is real: SDL/GLES offscreen, the real session manager, the real
 * plugin host and a fake brightness backend.  POWER is fed through the same
 * hotkey path as inputd/SDL, and the checks cover a ticking child process, a
 * responsive out-of-process plugin whose heartbeat timeout is shorter than the
 * sleep, and an inputd socket that only appears after sleep begins.
 */

#include "brickdesk/config.h"
#include "brickdesk/device_power.h"
#include "brickdesk/gfx.h"
#include "brickdesk/home.h"
#include "brickdesk/platform.h"
#include "brickdesk/plugin.h"
#include "brickdesk/settings.h"
#include "brickdesk/shell.h"
#include "settings_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef BRICKDESK_TEST_FIXTURE
#define BRICKDESK_TEST_FIXTURE "bd_plugin_fixture"
#endif

#define TEST_PATH 1024
#define TEST_APP "com.test.power.tick"
#define TEST_PLUGIN "com.test.power.heartbeat"
#define TEST_BRIGHTNESS 70

static int failures;

/*
 * Test-side signal interposition.  The session library calls kill() directly;
 * this lets the shell test reject exactly one SIGCONT for the real ticking
 * child, so a failed thaw can be checked without adding production hooks.
 */
static int g_deny_sigcont_pid;
static unsigned g_deny_sigcont_count;
static int g_stall_waitpid_pid;
static uint64_t g_stall_waitpid_until_ms;

static uint64_t real_ms(void);

int kill(pid_t pid, int signal_number) {
  if (signal_number == SIGCONT && g_deny_sigcont_count > 0 &&
      g_deny_sigcont_pid != 0 &&
      (pid == (pid_t)g_deny_sigcont_pid ||
       pid == -(pid_t)g_deny_sigcont_pid)) {
    --g_deny_sigcont_count;
    errno = EPERM;
    return -1;
  }
  return (int)syscall(SYS_kill, (long)pid, (long)signal_number);
}

pid_t waitpid(pid_t pid, int *status, int options) {
  if (g_stall_waitpid_pid != 0 && pid == (pid_t)g_stall_waitpid_pid &&
      (options & WUNTRACED) != 0 && real_ms() < g_stall_waitpid_until_ms) {
    if (status != NULL) {
      *status = 0;
    }
    return 0;
  }
  return (pid_t)syscall(SYS_wait4, (long)pid, status, (long)options, NULL);
}

static void deny_next_sigcont(int pid) {
  g_deny_sigcont_pid = pid;
  g_deny_sigcont_count = 1;
}

static void deny_sigcont_count(int pid, unsigned count) {
  g_deny_sigcont_pid = pid;
  g_deny_sigcont_count = count;
}

static void allow_sigcont(void) {
  g_deny_sigcont_pid = 0;
  g_deny_sigcont_count = 0;
}

static void stall_waitpid_for(int pid, int milliseconds) {
  g_stall_waitpid_pid = pid;
  g_stall_waitpid_until_ms = real_ms() + (uint64_t)milliseconds;
}

static void allow_waitpid(void) {
  g_stall_waitpid_pid = 0;
  g_stall_waitpid_until_ms = 0;
}

#define CHECK(condition, ...)                                                  \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "shell_power_test: FAIL: ");                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fputc('\n', stderr);                                                     \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static uint64_t real_ms(void) {
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

static bool make_dir(const char *path) {
  return mkdir(path, 0777) == 0 || errno == EEXIST;
}

static bool join(char *out, size_t size, const char *dir, const char *name) {
  int written = snprintf(out, size, "%s/%s", dir, name);
  return written > 0 && (size_t)written < size;
}

static bool write_text(const char *path, const char *text) {
  FILE *file = fopen(path, "wb");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs(text, file) >= 0;
  return fclose(file) == 0 && ok;
}

static bool read_text(const char *path, char *out, size_t size) {
  if (size == 0) {
    return false;
  }
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    out[0] = '\0';
    return false;
  }
  size_t used = fread(out, 1, size - 1, file);
  fclose(file);
  out[used] = '\0';
  return true;
}

static int count_lines(const char *path) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return 0;
  }
  int count = 0;
  int c;
  while ((c = fgetc(file)) != EOF) {
    if (c == '\n') {
      ++count;
    }
  }
  fclose(file);
  return count;
}

static char proc_state(int pid) {
  char path[TEST_PATH];
  snprintf(path, sizeof(path), "/proc/%d/stat", pid);
  char text[4096];
  if (!read_text(path, text, sizeof(text))) {
    return '?';
  }
  char *close = strrchr(text, ')');
  if (close == NULL || close[1] != ' ' || close[2] == '\0') {
    return '?';
  }
  return close[2];
}

/* --------------------------------------------------------- fake brightness */

typedef struct fake_av {
  int brightness;
  int reads;
  int writes;
  int zero_writes;
  int restore_writes;
  int restore_failures;
  int fail_zero;       /* fail the next N zero writes */
  int fail_restore;    /* fail the next N restore writes */
  bool fail_read;
  bool zero_changes_on_failure;
} fake_av;

static bool fake_av_get(void *user, int *percent) {
  fake_av *av = (fake_av *)user;
  if (percent == NULL) {
    return false;
  }
  ++av->reads;
  if (av->fail_read) {
    return false;
  }
  *percent = av->brightness;
  return true;
}

static bool fake_av_set(void *user, int percent) {
  fake_av *av = (fake_av *)user;
  ++av->writes;
  if (percent == 0) {
    ++av->zero_writes;
    if (av->fail_zero > 0) {
      --av->fail_zero;
      if (av->zero_changes_on_failure) {
        av->brightness = 0;
      }
      return false;
    }
    av->brightness = 0;
    return true;
  }
  ++av->restore_writes;
  if (av->fail_restore > 0) {
    --av->fail_restore;
    ++av->restore_failures;
    return false;
  }
  av->brightness = percent;
  return true;
}

/* ------------------------------------------------------------ fake backend */

typedef struct fake_backend {
  int deep_calls;
  int shutdown_calls;
} fake_backend;

static bd_power_backend_status fake_deep_sleep(void *user) {
  fake_backend *backend = (fake_backend *)user;
  ++backend->deep_calls;
  return BD_POWER_BACKEND_DISABLED;
}

static bd_power_backend_status fake_shutdown(void *user, bool reboot) {
  (void)reboot;
  fake_backend *backend = (fake_backend *)user;
  ++backend->shutdown_calls;
  return BD_POWER_BACKEND_DISABLED;
}

/* ---------------------------------------------------------- fake inputd */

typedef struct fake_inputd {
  pid_t pid;
  int commands;
  char socket_path[TEST_PATH];
  char log_path[TEST_PATH];
} fake_inputd;

static void inputd_log_line(int fd, const char *prefix, const char *data,
                            size_t length) {
  char line[2048];
  size_t used = (size_t)snprintf(line, sizeof(line), "%s ", prefix);
  if (used >= sizeof(line)) {
    return;
  }
  if (length > sizeof(line) - used - 2) {
    length = sizeof(line) - used - 2;
  }
  memcpy(line + used, data, length);
  used += length;
  if (used == 0 || line[used - 1] != '\n') {
    line[used++] = '\n';
  }
  ssize_t ignored = write(fd, line, used);
  (void)ignored;
}

static void inputd_child(int listener, int commands, const char *log_path) {
  int log = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0666);
  int client = accept(listener, NULL, NULL);
  close(listener);
  if (client < 0) {
    if (log >= 0) {
      close(log);
    }
    _exit(1);
  }
  int client_flags = fcntl(client, F_GETFL, 0);
  if (client_flags >= 0) {
    fcntl(client, F_SETFL, client_flags | O_NONBLOCK);
  }
  char pending[2048];
  size_t pending_length = 0;
  for (;;) {
    struct pollfd fds[2];
    fds[0].fd = commands;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = client;
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    int ready = poll(fds, 2, -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      char buffer[1024];
      ssize_t got = read(commands, buffer, sizeof(buffer));
      if (got <= 0) {
        break;
      }
      for (ssize_t i = 0; i < got; ++i) {
        if (buffer[i] == '\n') {
          ssize_t sent = send(client, pending, pending_length, MSG_NOSIGNAL);
          if (sent >= 0) {
            ssize_t extra = send(client, "\n", 1, MSG_NOSIGNAL);
            (void)extra;
          }
          if (log >= 0) {
            inputd_log_line(log, "tx", pending, pending_length);
          }
          pending_length = 0;
        } else if (pending_length < sizeof(pending)) {
          pending[pending_length++] = buffer[i];
        }
      }
      continue;
    }
    if ((fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      char buffer[2048];
      ssize_t got = recv(client, buffer, sizeof(buffer), 0);
      if (got <= 0) {
        break;
      }
      if (log >= 0) {
        inputd_log_line(log, "rx", buffer, (size_t)got);
      }
    }
  }
  close(client);
  if (log >= 0) {
    close(log);
  }
  _exit(0);
}

static bool inputd_start(fake_inputd *server, const char *socket_path,
                         const char *log_path) {
  memset(server, 0, sizeof(*server));
  server->commands = -1;
  snprintf(server->socket_path, sizeof(server->socket_path), "%s", socket_path);
  snprintf(server->log_path, sizeof(server->log_path), "%s", log_path);
  unlink(socket_path);
  int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener < 0) {
    return false;
  }
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (strlen(socket_path) >= sizeof(address.sun_path)) {
    close(listener);
    return false;
  }
  snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
  if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(listener, 1) != 0) {
    close(listener);
    unlink(socket_path);
    return false;
  }
  int commands[2];
  if (pipe2(commands, O_CLOEXEC) != 0) {
    close(listener);
    unlink(socket_path);
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(listener);
    close(commands[0]);
    close(commands[1]);
    unlink(socket_path);
    return false;
  }
  if (pid == 0) {
    close(commands[1]);
    inputd_child(listener, commands[0], log_path);
  }
  close(listener);
  close(commands[0]);
  server->pid = pid;
  server->commands = commands[1];
  return true;
}

static void inputd_stop(fake_inputd *server) {
  if (server->commands >= 0) {
    close(server->commands);
    server->commands = -1;
  }
  if (server->pid > 0) {
    for (int waited = 0; waited < 500; waited += 10) {
      int status = 0;
      pid_t done = waitpid(server->pid, &status, WNOHANG);
      if (done == server->pid || (done < 0 && errno == ECHILD)) {
        server->pid = -1;
        break;
      }
      sleep_ms(10);
    }
    if (server->pid > 0) {
      kill(server->pid, SIGTERM);
      int status = 0;
      waitpid(server->pid, &status, 0);
      server->pid = -1;
    }
  }
  unlink(server->socket_path);
}

static bool inputd_send(fake_inputd *server, const char *json) {
  if (server->commands < 0) {
    return false;
  }
  char line[1024];
  int written = snprintf(line, sizeof(line), "%s\n", json);
  if (written <= 0 || (size_t)written >= sizeof(line)) {
    return false;
  }
  size_t sent = 0;
  while (sent < (size_t)written) {
    ssize_t amount =
        write(server->commands, line + sent, (size_t)written - sent);
    if (amount < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    sent += (size_t)amount;
  }
  return true;
}

static bool inputd_send_key_time(fake_inputd *server, const char *key,
                                 bool pressed, uint64_t time_ms) {
  char json[256];
  int written = snprintf(json, sizeof(json),
                         "{\"type\":\"key\",\"key\":\"%s\",\"pressed\":%s,"
                         "\"time_ms\":%llu}",
                         key, pressed ? "true" : "false",
                         (unsigned long long)time_ms);
  return written > 0 && (size_t)written < sizeof(json) &&
         inputd_send(server, json);
}

/* --------------------------------------------------------------- runtime */

typedef struct test_runtime {
  bd_platform *platform;
  bd_gfx *gfx;
  bd_config *config;
  bd_shell *shell;
  bd_home *home;
  bd_session *session;
  bd_plugin_host *plugins;
  bd_plugin *plugin;
  fake_av av;
  fake_backend backend;
  bd_power_backend power_backend;
  bd_settings_system system;
  fake_inputd inputd;
  char root[TEST_PATH];
  char config_dir[512];
  char apps[TEST_PATH];
  char sockets[TEST_PATH];
  char frames[TEST_PATH];
  char state[TEST_PATH];
  char plugin_dir[TEST_PATH];
  char input_socket[TEST_PATH];
  char input_log[TEST_PATH];
  char ping_log[TEST_PATH];
  char tick_log[TEST_PATH];
} test_runtime;

static bool write_tick_app(const test_runtime *runtime, const char *child_path) {
  char directory[TEST_PATH];
  char path[TEST_PATH];
  if (!join(directory, sizeof(directory), runtime->apps, "tick") ||
      !make_dir(directory) ||
      !join(path, sizeof(path), directory, "app.json")) {
    return false;
  }
  char manifest[4096];
  int written = snprintf(
      manifest, sizeof(manifest),
      "{\"schema\":1,\"id\":\"%s\",\"name\":\"Power Tick\",\"version\":\"1.0\","
      "\"type\":\"app\",\"category\":\"app\",\"exec\":[\"%s\",\"tick\"],"
      "\"cwd\":\".\",\"env\":{\"TEST_CHILD_LOG\":\"%s\"},"
      "\"capabilities\":{\"background\":true},\"hooks\":{},"
      "\"menu\":{\"mode\":\"none\"},\"accent\":\"#336699\"}\n",
      TEST_APP, child_path, runtime->tick_log);
  return written > 0 && (size_t)written < sizeof(manifest) &&
         write_text(path, manifest);
}

static bool write_heartbeat_plugin(test_runtime *runtime) {
  char directory[TEST_PATH];
  char path[TEST_PATH];
  if (!join(directory, sizeof(directory), runtime->plugin_dir, TEST_PLUGIN) ||
      !make_dir(directory) ||
      !join(path, sizeof(path), directory, "plugin.json")) {
    return false;
  }
  char manifest[4096];
  int written = snprintf(
      manifest, sizeof(manifest),
      "{\"schema\":1,\"id\":\"%s\",\"name\":\"Heartbeat Fixture\","
      "\"version\":\"0.1.0\",\"api\":1,\"runtime\":\"shell\","
      "\"entry\":[\"run.sh\"],"
      "\"capabilities\":[\"tile\",\"settings\",\"background\"],"
      "\"tile\":{\"w\":1,\"h\":1,\"refresh_hz\":15},"
      "\"limits\":{\"memory_mb\":64,\"nofile\":32},"
      "\"settings\":[{\"key\":\"show_meta\",\"type\":\"bool\","
      "\"label\":\"Show meta\",\"default\":true}]}\n",
      TEST_PLUGIN);
  if (written <= 0 || (size_t)written >= sizeof(manifest) ||
      !write_text(path, manifest)) {
    return false;
  }
  if (!join(path, sizeof(path), directory, "run.sh")) {
    return false;
  }
  char script[TEST_PATH * 2];
  written = snprintf(script, sizeof(script),
                     "#!/bin/sh\nexec \"%s\" heartbeat \"$@\"\n",
                     BRICKDESK_TEST_FIXTURE);
  if (written <= 0 || (size_t)written >= sizeof(script) ||
      !write_text(path, script)) {
    return false;
  }
  return chmod(path, 0755) == 0;
}

static int test_power_resources_prepare(void *user,
                                       bd_power_sleep_mode mode) {
  (void)user;
  (void)mode;
  return 0;
}

static int test_power_resources_restore(void *user,
                                       bd_power_sleep_mode mode) {
  (void)user;
  (void)mode;
  return 0;
}

static const bd_shell_power_resources test_power_resources = {
    .user = NULL,
    .prepare = test_power_resources_prepare,
    .restore = test_power_resources_restore,
};

static bool create_runtime(test_runtime *runtime, const char *label,
                           const char *child_path, bool with_app,
                           bool with_plugin) {
  memset(runtime, 0, sizeof(*runtime));
  runtime->inputd.pid = -1;
  runtime->inputd.commands = -1;
  runtime->av.brightness = TEST_BRIGHTNESS;
  char pattern[] = "/tmp/bd-shell-power-XXXXXX";
  char *root = mkdtemp(pattern);
  if (root == NULL) {
    return false;
  }
  snprintf(runtime->root, sizeof(runtime->root), "%s", root);
  CHECK(join(runtime->apps, sizeof(runtime->apps), runtime->root, "apps") &&
            make_dir(runtime->apps),
        "%s: apps dir", label);
  CHECK(join(runtime->sockets, sizeof(runtime->sockets), runtime->root,
             "sockets") &&
            make_dir(runtime->sockets),
        "%s: sockets dir", label);
  CHECK(join(runtime->frames, sizeof(runtime->frames), runtime->root,
             "frames") &&
            make_dir(runtime->frames),
        "%s: frames dir", label);
  CHECK(join(runtime->state, sizeof(runtime->state), runtime->root, "state") &&
            make_dir(runtime->state),
        "%s: state dir", label);
  CHECK(join(runtime->plugin_dir, sizeof(runtime->plugin_dir), runtime->root,
             "plugins") &&
            make_dir(runtime->plugin_dir),
        "%s: plugin dir", label);
  CHECK(join(runtime->input_socket, sizeof(runtime->input_socket),
             runtime->root, "inputd.sock"),
        "%s: input socket", label);
  CHECK(join(runtime->input_log, sizeof(runtime->input_log), runtime->root,
             "inputd.log"),
        "%s: input log", label);
  CHECK(join(runtime->ping_log, sizeof(runtime->ping_log), runtime->root,
             "plugin-pings.log"),
        "%s: ping log", label);
  CHECK(join(runtime->tick_log, sizeof(runtime->tick_log), runtime->root,
             "tick.log"),
        "%s: tick log", label);
  if (!sf_make_config_dir(runtime->config_dir, sizeof(runtime->config_dir),
                          false)) {
    CHECK(false, "%s: config dir", label);
    return false;
  }
  if (with_app) {
    CHECK(write_tick_app(runtime, child_path), "%s: tick app", label);
  }

  runtime->power_backend.user = &runtime->backend;
  runtime->power_backend.deep_sleep = fake_deep_sleep;
  runtime->power_backend.shutdown = fake_shutdown;
  runtime->system.user = &runtime->av;
  runtime->system.get_brightness = fake_av_get;
  runtime->system.set_brightness = fake_av_set;

  if (with_plugin) {
    CHECK(write_heartbeat_plugin(runtime), "%s: heartbeat plugin", label);
    setenv("BRICKDESK_TEST_PLUGIN_PING_LOG", runtime->ping_log, 1);
    bd_plugin_limits limits = bd_plugin_default_limits();
    limits.handshake_timeout_ms = 2000;
    limits.ping_interval_ms = 250;
    limits.ping_timeout_ms = 600;
    limits.idle_stop_ms = 200;
    limits.shutdown_timeout_ms = 500;
    limits.restart_limit = 0;
    char runtime_dir[TEST_PATH];
    char data_dir[TEST_PATH];
    char state_dir[TEST_PATH];
    CHECK(join(runtime_dir, sizeof(runtime_dir), runtime->root, "plugin-run") &&
              join(data_dir, sizeof(data_dir), runtime->root, "plugin-data") &&
              join(state_dir, sizeof(state_dir), runtime->root, "plugin-state"),
          "%s: plugin paths", label);
    make_dir(runtime_dir);
    make_dir(data_dir);
    make_dir(state_dir);
    bd_plugin_host_config plugin_config;
    memset(&plugin_config, 0, sizeof(plugin_config));
    plugin_config.plugin_dir = runtime->plugin_dir;
    plugin_config.data_dir = data_dir;
    plugin_config.runtime_dir = runtime_dir;
    plugin_config.state_dir = state_dir;
    plugin_config.limits = &limits;
    plugin_config.display_width = 1024;
    plugin_config.display_height = 768;
    plugin_config.dark = true;
    plugin_config.accent = 0xe06c4a;
    plugin_config.locale = "en-US";
    runtime->plugins = bd_plugin_host_create(&plugin_config);
    CHECK(runtime->plugins != NULL, "%s: plugin host", label);
    if (runtime->plugins != NULL) {
      char error[256] = {0};
      CHECK(bd_plugin_host_scan(runtime->plugins, error, sizeof(error)) == 1,
            "%s: plugin scan: %s", label, error);
      runtime->plugin = bd_plugin_host_find(runtime->plugins, TEST_PLUGIN);
      CHECK(runtime->plugin != NULL, "%s: find plugin", label);
    }
  }

  bd_platform_config platform_config;
  memset(&platform_config, 0, sizeof(platform_config));
  platform_config.title = "brickdesk shell power test";
  platform_config.width = 1024;
  platform_config.height = 768;
  platform_config.hidden = true;
  runtime->platform = bd_platform_create(&platform_config);
  if (runtime->platform == NULL) {
    CHECK(false, "%s: platform", label);
    return false;
  }
  runtime->gfx = bd_gfx_create(1024, 768);
  if (runtime->gfx == NULL) {
    CHECK(false, "%s: gfx", label);
    return false;
  }
  bd_config_options config_options;
  memset(&config_options, 0, sizeof(config_options));
  config_options.config_dir = runtime->config_dir;
  config_options.migrate = false;
  char config_error[256] = {0};
  runtime->config =
      bd_config_load(&config_options, config_error, sizeof(config_error));
  if (runtime->config == NULL) {
    CHECK(false, "%s: config: %s", label, config_error);
    return false;
  }

  bd_shell_config shell_config;
  memset(&shell_config, 0, sizeof(shell_config));
  shell_config.platform = runtime->platform;
  shell_config.gfx = runtime->gfx;
  shell_config.settings = runtime->config;
  shell_config.plugins = runtime->plugins;
  shell_config.apps_dir = runtime->apps;
  shell_config.input_socket = runtime->input_socket;
  shell_config.session_socket_dir = runtime->sockets;
  shell_config.session_frame_dir = runtime->frames;
  shell_config.session_state_root = runtime->state;
  shell_config.battery_root = BRICKDESK_TEST_BATTERY_FIXTURE;
  shell_config.settings_system = &runtime->system;
  shell_config.power_backend = &runtime->power_backend;
  shell_config.power_resources = &test_power_resources;
  shell_config.attach_plugin_tile = with_plugin;
  shell_config.verbose = false;
  runtime->shell = bd_shell_create(&shell_config);
  if (runtime->shell == NULL) {
    CHECK(false, "%s: shell", label);
    return false;
  }
  runtime->home = bd_shell_home(runtime->shell);
  runtime->session = bd_shell_test_session(runtime->shell);
  CHECK(runtime->home != NULL && runtime->session != NULL, "%s: shell parts",
        label);

  if (with_plugin && runtime->plugin != NULL) {
    int index = bd_home_find_item(runtime->home, "plugin-tile");
    CHECK(index >= 0, "%s: plugin tile item", label);
    if (index >= 0) {
      bd_home_set_focus(runtime->home, index);
    }
    bool running = false;
    for (int waited = 0; waited < 3000; waited += 10) {
      bd_shell_iterate(runtime->shell, 5);
      if (bd_plugin_state_of(runtime->plugin) == BD_PLUGIN_RUNNING) {
        running = true;
        break;
      }
      sleep_ms(5);
    }
    CHECK(running, "%s: plugin did not reach RUNNING", label);
  }
  return true;
}

static void destroy_runtime(test_runtime *runtime) {
  if (runtime == NULL) {
    return;
  }
  runtime->av.fail_zero = 0;
  runtime->av.fail_restore = 0;
  runtime->av.fail_read = false;
  allow_sigcont();
  allow_waitpid();
  if (runtime->shell != NULL) {
    bd_shell_destroy(runtime->shell);
    runtime->shell = NULL;
  }
  inputd_stop(&runtime->inputd);
  if (runtime->plugins != NULL) {
    bd_plugin_host_destroy(runtime->plugins);
    runtime->plugins = NULL;
  }
  if (runtime->config != NULL) {
    bd_config_free(runtime->config);
    runtime->config = NULL;
  }
  if (runtime->gfx != NULL) {
    bd_gfx_destroy(runtime->gfx);
    runtime->gfx = NULL;
  }
  if (runtime->platform != NULL) {
    bd_platform_destroy(runtime->platform);
    runtime->platform = NULL;
  }
  unsetenv("BRICKDESK_TEST_PLUGIN_PING_LOG");
  if (runtime->root[0] != '\0') {
    sf_remove_tree(runtime->root);
  }
  if (runtime->config_dir[0] != '\0') {
    sf_remove_tree(runtime->config_dir);
  }
  memset(runtime, 0, sizeof(*runtime));
}

static void runtime_run_for(test_runtime *runtime, int duration_ms) {
  uint64_t deadline = real_ms() + (uint64_t)duration_ms;
  while (real_ms() < deadline) {
    bd_shell_iterate(runtime->shell, 10);
    sleep_ms(2);
  }
}

static bd_power_state runtime_power_state(test_runtime *runtime) {
  bd_power_state state = BD_POWER_STATE_UNKNOWN;
  bool sleeping = false;
  bool input_suspended = false;
  if (!bd_shell_test_power(runtime->shell, &state, &sleeping,
                           &input_suspended)) {
    return BD_POWER_STATE_UNKNOWN;
  }
  return state;
}

static bool runtime_sleeping(test_runtime *runtime) {
  bool sleeping = false;
  bool input_suspended = false;
  bd_shell_test_power(runtime->shell, NULL, &sleeping, &input_suspended);
  return sleeping;
}

static bool runtime_input_suspended(test_runtime *runtime) {
  bool sleeping = false;
  bool input_suspended = false;
  bd_shell_test_power(runtime->shell, NULL, &sleeping, &input_suspended);
  return input_suspended;
}

static bool wait_power_state(test_runtime *runtime, bd_power_state state,
                             int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 10) {
    bd_shell_iterate(runtime->shell, 5);
    if (runtime_power_state(runtime) == state) {
      return true;
    }
    sleep_ms(5);
  }
  fprintf(stderr, "shell_power_test: power state %s, wanted %s\n",
          bd_power_state_name(runtime_power_state(runtime)),
          bd_power_state_name(state));
  return false;
}

static bool wait_input_source(test_runtime *runtime, const char *source,
                              int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 10) {
    bd_shell_iterate(runtime->shell, 10);
    if (strcmp(bd_shell_input_source(runtime->shell), source) == 0) {
      return true;
    }
    sleep_ms(5);
  }
  return false;
}

static bool wait_app_foreground(test_runtime *runtime, int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 10) {
    bd_shell_iterate(runtime->shell, 10);
    bd_session_app_info info;
    if (bd_session_find(runtime->session, TEST_APP, &info) &&
        info.state == BD_SESSION_FOREGROUND && !info.frozen) {
      return true;
    }
    sleep_ms(5);
  }
  return false;
}

static bool launch_tick(test_runtime *runtime) {
  CHECK(bd_shell_test_launch(runtime->shell, TEST_APP),
        "launch tick app through the shell");
  bool foreground = wait_app_foreground(runtime, 3000);
  CHECK(foreground, "tick app foreground");
  for (int waited = 0; waited < 3000; waited += 10) {
    bd_shell_iterate(runtime->shell, 5);
    if (!bd_shell_visible(runtime->shell)) {
      break;
    }
    sleep_ms(5);
  }
  CHECK(!bd_shell_visible(runtime->shell),
        "app owns the display after launch animation");
  return foreground && !bd_shell_visible(runtime->shell);
}

static void tap_power(test_runtime *runtime) {
  uint64_t now = real_ms();
  bd_shell_test_key(runtime->shell, BD_KEY_POWER, true, now);
  bd_shell_test_key(runtime->shell, BD_KEY_POWER, false, now + 20);
}

static void press_power(test_runtime *runtime) {
  bd_shell_test_key(runtime->shell, BD_KEY_POWER, true, real_ms());
}

static void release_power(test_runtime *runtime) {
  bd_shell_test_key(runtime->shell, BD_KEY_POWER, false, real_ms());
}

static void tap_key(test_runtime *runtime, bd_key key) {
  uint64_t now = real_ms();
  bd_shell_test_key(runtime->shell, key, true, now);
  bd_shell_test_key(runtime->shell, key, false, now + 20);
}

static bool enter_light_sleep(test_runtime *runtime) {
  uint64_t presents = bd_shell_present_count(runtime->shell);
  tap_power(runtime);
  bool ok = wait_power_state(runtime, BD_POWER_STATE_LIGHT_SLEEP, 2000);
  CHECK(ok, "POWER tap enters light sleep");
  CHECK(runtime_sleeping(runtime), "shell reports sleeping");
  CHECK(runtime_input_suspended(runtime), "power input hook suspended");
  CHECK(bd_shell_present_count(runtime->shell) == presents,
        "sleep entry presented a frame (%llu -> %llu)",
        (unsigned long long)presents,
        (unsigned long long)bd_shell_present_count(runtime->shell));
  return ok;
}

static void wake_with_power_press(test_runtime *runtime, bool expect_ok) {
  press_power(runtime);
  bd_power_state expected =
      expect_ok ? BD_POWER_STATE_IDLE : BD_POWER_STATE_DEGRADED;
  CHECK(wait_power_state(runtime, expected, 2000),
        "wake press reaches %s", bd_power_state_name(expected));
  release_power(runtime);
  bd_shell_iterate(runtime->shell, 5);
  CHECK(runtime_sleeping(runtime) == !expect_ok,
        "release after wake must not re-enter sleep");
}

static void test_source_switch_and_event_time(const char *child_path) {
  test_runtime runtime;
  if (!create_runtime(&runtime, "switch", child_path, false, false)) {
    destroy_runtime(&runtime);
    return;
  }
  for (int i = 0; i < 5; ++i) {
    bd_shell_iterate(runtime.shell, 5);
  }
  CHECK(inputd_start(&runtime.inputd, runtime.input_socket, runtime.input_log),
        "start inputd for switch test");
  CHECK(wait_input_source(&runtime, "inputd", 8000),
        "inputd connected for switch test");

  /* inputd -> SDL: an un-released daemon POWER must not poison SDL. */
  uint64_t stale = real_ms();
  CHECK(inputd_send_key_time(&runtime.inputd, "POWER", true, stale),
        "inputd POWER down");
  runtime_run_for(&runtime, 1100);
  inputd_stop(&runtime.inputd);
  CHECK(wait_input_source(&runtime, "sdl", 3000),
        "switch to SDL after inputd stop");
  tap_power(&runtime);
  CHECK(wait_power_state(&runtime, BD_POWER_STATE_LIGHT_SLEEP, 2000),
        "fresh SDL tap after inputd disconnect did not sleep");
  wake_with_power_press(&runtime, true);
  runtime_run_for(&runtime, 1050); /* wake press guard */

  /* SDL -> inputd: an un-released SDL POWER must not poison inputd. */
  bd_shell_test_key(runtime.shell, BD_KEY_POWER, true, real_ms());
  runtime_run_for(&runtime, 1100);
  CHECK(inputd_start(&runtime.inputd, runtime.input_socket, runtime.input_log),
        "restart inputd after SDL gesture");
  CHECK(wait_input_source(&runtime, "inputd", 8000),
        "switch back to inputd");
  uint64_t short_time = real_ms();
  CHECK(inputd_send_key_time(&runtime.inputd, "POWER", true,
                             short_time - 300),
        "fresh inputd short down");
  CHECK(inputd_send_key_time(&runtime.inputd, "POWER", false,
                             short_time - 100),
        "fresh inputd short up");
  sleep_ms(30);
  CHECK(wait_power_state(&runtime, BD_POWER_STATE_LIGHT_SLEEP, 2000),
        "fresh inputd short tap after SDL gesture did not sleep");
  wake_with_power_press(&runtime, true);
  runtime_run_for(&runtime, 1050);

  /* release_all also cancels an incomplete POWER gesture. */
  CHECK(inputd_send_key_time(&runtime.inputd, "POWER", true, real_ms()),
        "inputd POWER down before release_all");
  runtime_run_for(&runtime, 1100);
  CHECK(inputd_send(&runtime.inputd, "{\"type\":\"release_all\"}"),
        "inputd release_all");
  sleep_ms(20);
  uint64_t after_release = real_ms();
  CHECK(inputd_send_key_time(&runtime.inputd, "POWER", true,
                             after_release - 200),
        "fresh inputd down after release_all");
  CHECK(inputd_send_key_time(&runtime.inputd, "POWER", false,
                             after_release - 20),
        "fresh inputd up after release_all");
  sleep_ms(30);
  CHECK(wait_power_state(&runtime, BD_POWER_STATE_LIGHT_SLEEP, 2000),
        "fresh tap after release_all did not sleep");
  wake_with_power_press(&runtime, true);
  runtime_run_for(&runtime, 1050);

  /* A queued 1500 ms hold is a long press, not a compressed tap. */
  uint64_t long_time = real_ms();
  CHECK(inputd_send_key_time(&runtime.inputd, "POWER", true,
                             long_time - 2000),
        "queued long down");
  CHECK(inputd_send_key_time(&runtime.inputd, "POWER", false,
                             long_time - 500),
        "queued long up");
  sleep_ms(30);
  runtime_run_for(&runtime, 150);
  CHECK(runtime_power_state(&runtime) == BD_POWER_STATE_IDLE,
        "queued 1500 ms hold became a sleep");
  CHECK(!runtime_sleeping(&runtime), "queued long hold marked sleeping");
  CHECK(runtime.backend.shutdown_calls >= 1,
        "queued 1500 ms hold did not request the long-press action");

  /* Exactly 1000 ms remains inside the documented short-tap window. */
  uint64_t boundary_time = real_ms();
  CHECK(inputd_send_key_time(&runtime.inputd, "POWER", true,
                             boundary_time - 1200),
        "queued boundary down");
  CHECK(inputd_send_key_time(&runtime.inputd, "POWER", false,
                             boundary_time - 200),
        "queued boundary up");
  sleep_ms(30);
  CHECK(wait_power_state(&runtime, BD_POWER_STATE_LIGHT_SLEEP, 2000),
        "queued 1000 ms boundary did not sleep");
  wake_with_power_press(&runtime, true);
  runtime_run_for(&runtime, 1050);

  /* Older clients/fixtures without time_ms stay compatible. */
  CHECK(inputd_send(&runtime.inputd,
                    "{\"type\":\"key\",\"key\":\"POWER\",\"pressed\":true}"),
        "legacy inputd down without time_ms");
  CHECK(inputd_send(&runtime.inputd,
                    "{\"type\":\"key\",\"key\":\"POWER\",\"pressed\":false}"),
        "legacy inputd up without time_ms");
  sleep_ms(30);
  CHECK(wait_power_state(&runtime, BD_POWER_STATE_LIGHT_SLEEP, 2000),
        "legacy no-timestamp tap did not sleep");
  wake_with_power_press(&runtime, true);
  destroy_runtime(&runtime);
}

/* ------------------------------------------------------------------ tests */

static void test_home_sleep_and_inputd(const char *child_path) {
  (void)child_path;
  test_runtime runtime;
  if (!create_runtime(&runtime, "home", child_path, false, false)) {
    destroy_runtime(&runtime);
    return;
  }
  for (int i = 0; i < 5; ++i) {
    bd_shell_iterate(runtime.shell, 5);
  }
  int focus = bd_home_focus(runtime.home);
  uint64_t presents = bd_shell_present_count(runtime.shell);

  if (!enter_light_sleep(&runtime)) {
    destroy_runtime(&runtime);
    return;
  }
  CHECK(runtime.av.brightness == 0, "sleep zeroes brightness, got %d",
        runtime.av.brightness);
  CHECK(runtime.av.zero_writes == 1, "exactly one zero write, got %d",
        runtime.av.zero_writes);
  CHECK(runtime.av.reads > 0, "brightness was read before writing");
  CHECK(!bd_home_surface_visible(runtime.home),
        "home surface hidden while asleep");

  runtime_run_for(&runtime, 300);
  CHECK(bd_shell_present_count(runtime.shell) == presents,
        "asleep loop presented a frame");
  CHECK(bd_home_surface_visible(runtime.home) == false,
        "asleep service restored the home surface");

  tap_key(&runtime, BD_KEY_A);
  tap_key(&runtime, BD_KEY_RIGHT);
  tap_key(&runtime, BD_KEY_START);
  runtime_run_for(&runtime, 100);
  CHECK(bd_home_focus(runtime.home) == focus, "ordinary key moved focus asleep");
  CHECK(!bd_home_quick_open(runtime.home), "ordinary key opened quick panel");
  CHECK(bd_shell_present_count(runtime.shell) == presents,
        "ordinary key presented a frame asleep");

  CHECK(inputd_start(&runtime.inputd, runtime.input_socket, runtime.input_log),
        "start inputd while asleep");
  CHECK(wait_input_source(&runtime, "inputd", 8000),
        "inputd reconnects while asleep");
  CHECK(runtime_sleeping(&runtime), "inputd reconnect woke the shell");

  CHECK(inputd_send(&runtime.inputd,
                    "{\"type\":\"key\",\"key\":\"RIGHT\",\"pressed\":true}"),
        "send ordinary key");
  CHECK(inputd_send(&runtime.inputd,
                    "{\"type\":\"key\",\"key\":\"RIGHT\",\"pressed\":false}"),
        "send ordinary release");
  CHECK(inputd_send(&runtime.inputd,
                    "{\"type\":\"action\",\"action\":\"ui.hint\","
                    "\"phase\":\"down\"}"),
        "send ordinary action");
  runtime_run_for(&runtime, 150);
  CHECK(bd_home_focus(runtime.home) == focus,
        "inputd key leaked through sleep");
  CHECK(!bd_shell_hint_visible(runtime.shell),
        "inputd action leaked through sleep");
  CHECK(runtime_sleeping(&runtime), "inputd event terminated sleep");

  uint64_t before_block = real_ms();
  int poll_result = bd_shell_iterate(runtime.shell, 300);
  uint64_t elapsed = real_ms() - before_block;
  CHECK(poll_result == 0, "quiet sleep iterate returned %d", poll_result);
  CHECK(elapsed >= 200, "sleep poll busy-looped (%llu ms)",
        (unsigned long long)elapsed);
  CHECK(bd_shell_present_count(runtime.shell) == presents,
        "sleep poll presented a frame");

  inputd_stop(&runtime.inputd);
  CHECK(wait_input_source(&runtime, "sdl", 3000),
        "inputd disconnect falls back to SDL");
  CHECK(runtime_sleeping(&runtime), "source switch woke the shell");
  CHECK(inputd_start(&runtime.inputd, runtime.input_socket, runtime.input_log),
        "restart inputd while asleep");
  CHECK(wait_input_source(&runtime, "inputd", 8000),
        "inputd reconnects after source switch");
  CHECK(runtime_sleeping(&runtime), "second reconnect woke the shell");
  CHECK(!bd_home_surface_visible(runtime.home),
        "source switch restored home surface asleep");
  char log_text[16384];
  CHECK(read_text(runtime.input_log, log_text, sizeof(log_text)),
        "read inputd log");
  CHECK(strstr(log_text, "release_all") == NULL,
        "shell sent global release_all to inputd");

  CHECK(inputd_send(&runtime.inputd,
                    "{\"type\":\"key\",\"key\":\"POWER\",\"pressed\":true}"),
        "inputd POWER press");
  CHECK(wait_power_state(&runtime, BD_POWER_STATE_IDLE, 2000),
        "inputd POWER wakes the shell");
  CHECK(inputd_send(&runtime.inputd,
                    "{\"type\":\"key\",\"key\":\"POWER\",\"pressed\":false}"),
        "inputd POWER release");
  runtime_run_for(&runtime, 150);
  CHECK(!runtime_sleeping(&runtime), "wake release re-entered sleep");
  CHECK(runtime.av.brightness == TEST_BRIGHTNESS,
        "wake restores original brightness, got %d", runtime.av.brightness);
  CHECK(!runtime_input_suspended(&runtime), "input resumed after wake");
  CHECK(bd_home_surface_visible(runtime.home),
        "home surface restored after wake");
  CHECK(bd_home_focus(runtime.home) == focus, "home focus changed across sleep");

  /* The wake press and release must not retrigger sleep. */
  tap_power(&runtime);
  runtime_run_for(&runtime, 120);
  CHECK(!runtime_sleeping(&runtime), "immediate tap re-entered sleep");
  destroy_runtime(&runtime);
}

static void test_app_freeze_and_plugin_heartbeat(const char *child_path) {
  test_runtime runtime;
  if (!create_runtime(&runtime, "app", child_path, true, true)) {
    destroy_runtime(&runtime);
    return;
  }
  if (runtime.plugin == NULL) {
    destroy_runtime(&runtime);
    return;
  }
  bd_plugin_stats plugin_before = bd_plugin_get_stats(runtime.plugin);
  if (!launch_tick(&runtime)) {
    destroy_runtime(&runtime);
    return;
  }
  bd_session_app_info app_before;
  CHECK(bd_session_find(runtime.session, TEST_APP, &app_before),
        "find foreground app");
  int app_pid = app_before.pid;
  int background_before = bd_session_background_count(runtime.session);
  CHECK(app_before.state == BD_SESSION_FOREGROUND && app_pid > 0,
        "app starts foreground pid=%d", app_pid);
  CHECK(!bd_home_surface_visible(runtime.home),
        "plugin tile is invisible while app owns display");
  runtime_run_for(&runtime, 120);
  int ticks_before = count_lines(runtime.tick_log);
  CHECK(ticks_before > 0, "tick child never ran");

  if (!enter_light_sleep(&runtime)) {
    destroy_runtime(&runtime);
    return;
  }
  uint64_t presents_asleep = bd_shell_present_count(runtime.shell);
  bd_session_app_info app_sleep;
  CHECK(bd_session_find(runtime.session, TEST_APP, &app_sleep),
        "find app while asleep");
  CHECK(app_sleep.state == BD_SESSION_FOREGROUND && app_sleep.frozen &&
            !app_sleep.background && app_sleep.pid == app_pid,
        "freeze keeps identity/state app=%s state=%s frozen=%d pid=%d",
        app_sleep.app_id, bd_session_state_name(app_sleep.state),
        app_sleep.frozen, app_sleep.pid);
  CHECK(bd_session_background_count(runtime.session) == background_before,
        "freeze changed background quota");
  CHECK(proc_state(app_pid) == 'T', "app process state %c, wanted T",
        proc_state(app_pid));

  int pings_at_sleep = count_lines(runtime.ping_log);
  int ticks_at_sleep = count_lines(runtime.tick_log);
  int max_plugin_timeout = 0;
  for (int sample = 0; sample < 20; ++sample) {
    bd_shell_iterate(runtime.shell, 10);
    int next = bd_plugin_host_next_timeout(runtime.plugins);
    if (next > max_plugin_timeout) {
      max_plugin_timeout = next;
    }
    sleep_ms(2);
  }
  runtime_run_for(&runtime, 900);
  CHECK(proc_state(app_pid) == 'T', "app resumed while asleep");
  CHECK(count_lines(runtime.tick_log) == ticks_at_sleep,
        "frozen app executed while asleep (%d -> %d)", ticks_at_sleep,
        count_lines(runtime.tick_log));
  CHECK(bd_shell_present_count(runtime.shell) == presents_asleep,
        "app sleep presented a frame");
  CHECK(max_plugin_timeout > 100,
        "invisible tile kept a refresh_hz deadline (%d ms)", max_plugin_timeout);
  CHECK(bd_plugin_state_of(runtime.plugin) == BD_PLUGIN_RUNNING,
        "plugin died across the sleep heartbeat window");
  bd_plugin_stats plugin_sleep = bd_plugin_get_stats(runtime.plugin);
  CHECK(plugin_sleep.pid == plugin_before.pid,
        "plugin pid changed %d -> %d", plugin_before.pid, plugin_sleep.pid);
  int pings_asleep = count_lines(runtime.ping_log);
  CHECK(pings_asleep > pings_at_sleep,
        "plugin heartbeat did not continue asleep (%d -> %d)",
        pings_at_sleep, pings_asleep);

  wake_with_power_press(&runtime, true);
  bd_session_app_info app_after;
  CHECK(bd_session_find(runtime.session, TEST_APP, &app_after),
        "find app after wake");
  CHECK(app_after.state == BD_SESSION_FOREGROUND && !app_after.frozen &&
            !app_after.background && app_after.pid == app_pid,
        "thaw keeps identity/state app=%s state=%s frozen=%d pid=%d",
        app_after.app_id, bd_session_state_name(app_after.state),
        app_after.frozen, app_after.pid);
  CHECK(bd_session_background_count(runtime.session) == background_before,
        "wake changed background quota");
  CHECK(proc_state(app_pid) != 'T', "app stayed stopped after wake");
  CHECK(runtime.av.brightness == TEST_BRIGHTNESS,
        "wake restores brightness after frozen app, got %d",
        runtime.av.brightness);
  runtime_run_for(&runtime, 300);
  CHECK(count_lines(runtime.tick_log) > ticks_at_sleep,
        "tick child did not resume after wake");
  CHECK(bd_plugin_state_of(runtime.plugin) == BD_PLUGIN_RUNNING,
        "plugin not running after wake");
  int pings_after_wake = count_lines(runtime.ping_log);
  CHECK(pings_after_wake > pings_asleep,
        "plugin communication did not resume after wake (%d -> %d)",
        pings_asleep, pings_after_wake);
  CHECK(!bd_home_surface_visible(runtime.home),
        "app-owned home surface turned visible after wake");
  CHECK(runtime.backend.deep_calls == 0 && runtime.backend.shutdown_calls == 0,
        "light sleep called deep/shutdown backend");
  destroy_runtime(&runtime);
}

static void test_settings_location_and_repeat(const char *child_path) {
  test_runtime runtime;
  if (!create_runtime(&runtime, "settings", child_path, false, false)) {
    destroy_runtime(&runtime);
    return;
  }
  for (int i = 0; i < 5; ++i) {
    bd_shell_iterate(runtime.shell, 5);
  }
  CHECK(bd_shell_open_settings(runtime.shell, BD_SETTINGS_CATEGORY_DISPLAY),
        "open display settings");
  bd_shell_iterate(runtime.shell, 5);
  bd_settings *settings = bd_shell_settings(runtime.shell);
  CHECK(settings != NULL && bd_settings_is_open(settings),
        "settings page open");
  int home_focus = bd_home_focus(runtime.home);
  int category = bd_settings_category_index(settings);
  int focus = bd_settings_focus(settings);
  const char *row = bd_settings_row_key(settings, focus);
  CHECK(row != NULL && strcmp(row, "display.brightness") == 0,
        "focused row is brightness, got %s", row != NULL ? row : "(null)");
  int subpage = bd_settings_subpage(settings) ? 1 : 0;

  /* Hold LEFT on the brightness slider so a repeat deadline is live. */
  uint64_t hold_time = real_ms();
  bd_shell_test_key(runtime.shell, BD_KEY_LEFT, true, hold_time);
  runtime_run_for(&runtime, 80);
  uint64_t wake_at = 0;
  CHECK(bd_settings_needs_frame_until(settings, &wake_at),
        "held slider has a repeat frame deadline");
  int writes_before_sleep = runtime.av.writes;
  CHECK(writes_before_sleep > 0, "held slider did not write brightness");
  int held_brightness = runtime.av.brightness;
  CHECK(held_brightness > 0 && held_brightness < TEST_BRIGHTNESS,
        "held slider changed brightness to %d", held_brightness);

  uint64_t presents_before = bd_shell_present_count(runtime.shell);
  if (!enter_light_sleep(&runtime)) {
    destroy_runtime(&runtime);
    return;
  }
  CHECK(bd_shell_settings_visible(runtime.shell),
        "sleep closed the settings page");
  CHECK(bd_home_surface_visible(runtime.home) == false,
        "sleep restored the home surface under settings");
  CHECK(bd_settings_category_index(settings) == category &&
            bd_settings_focus(settings) == focus,
        "sleep moved settings location");
  CHECK(bd_settings_subpage(settings) == (subpage != 0),
        "sleep changed the settings subpage");
  const char *row_sleep = bd_settings_row_key(settings, focus);
  CHECK(row_sleep != NULL && strcmp(row_sleep, row) == 0,
        "sleep changed focused row");
  uint64_t repeat_wake = 0;
  CHECK(!bd_settings_needs_frame_until(settings, &repeat_wake),
        "held repeat survived sleep entry");
  int writes_at_sleep = runtime.av.writes;
  CHECK(runtime.av.brightness == 0, "settings sleep did not zero brightness");

  runtime_run_for(&runtime, 700);
  CHECK(bd_shell_present_count(runtime.shell) == presents_before,
        "settings sleep presented a frame");
  CHECK(runtime.av.writes == writes_at_sleep,
        "held repeat wrote while asleep (%d -> %d)", writes_at_sleep,
        runtime.av.writes);
  CHECK(bd_settings_category_index(settings) == category &&
            bd_settings_focus(settings) == focus,
        "settings moved while asleep");
  CHECK(bd_home_focus(runtime.home) == home_focus,
        "home focus moved while asleep");

  wake_with_power_press(&runtime, true);
  CHECK(bd_shell_settings_visible(runtime.shell),
        "settings closed on wake");
  CHECK(bd_settings_category_index(settings) == category &&
            bd_settings_focus(settings) == focus,
        "settings location changed on wake");
  CHECK(bd_settings_subpage(settings) == (subpage != 0),
        "settings subpage changed on wake");
  CHECK(runtime.av.brightness == held_brightness,
        "settings wake restored %d, wanted %d", runtime.av.brightness,
        held_brightness);
  CHECK(!bd_home_surface_visible(runtime.home),
        "settings wake made home visible under settings");
  int writes_after_wake = runtime.av.writes;
  runtime_run_for(&runtime, 700);
  CHECK(runtime.av.writes == writes_after_wake,
        "held repeat resumed after wake (%d -> %d)", writes_after_wake,
        runtime.av.writes);
  CHECK(bd_settings_category_index(settings) == category &&
            bd_settings_focus(settings) == focus,
        "settings moved after wake");
  CHECK(bd_home_focus(runtime.home) == home_focus,
        "home focus changed after wake");
  destroy_runtime(&runtime);
}

static void test_brightness_failures(const char *child_path) {
  test_runtime runtime;
  if (!create_runtime(&runtime, "failures", child_path, true, false)) {
    destroy_runtime(&runtime);
    return;
  }
  if (!launch_tick(&runtime)) {
    destroy_runtime(&runtime);
    return;
  }
  bd_session_app_info before_sleep;
  CHECK(bd_session_find(runtime.session, TEST_APP, &before_sleep),
        "failure app foreground");
  int app_pid = before_sleep.pid;
  int background = bd_session_background_count(runtime.session);
  uint64_t presents = bd_shell_present_count(runtime.shell);

  /* Unknown brightness: no blind write, no false sleep, app thawed. */
  runtime.av.fail_read = true;
  int zero_before = runtime.av.zero_writes;
  tap_power(&runtime);
  runtime_run_for(&runtime, 120);
  CHECK(runtime_power_state(&runtime) == BD_POWER_STATE_IDLE,
        "unknown brightness reported sleep");
  CHECK(!runtime_sleeping(&runtime), "unknown brightness marked sleeping");
  CHECK(runtime.av.zero_writes == zero_before,
        "unknown brightness caused a blind write");
  CHECK(runtime.av.brightness == TEST_BRIGHTNESS,
        "unknown brightness changed brightness");
  bd_session_app_info after_failed_sleep;
  CHECK(bd_session_find(runtime.session, TEST_APP, &after_failed_sleep) &&
            !after_failed_sleep.frozen && !after_failed_sleep.background &&
            after_failed_sleep.state == BD_SESSION_FOREGROUND &&
            after_failed_sleep.pid == app_pid,
        "failed sleep lost app ownership/state");
  CHECK(proc_state(app_pid) != 'T', "failed sleep left app stopped");
  CHECK(bd_session_background_count(runtime.session) == background,
        "failed sleep changed quota");
  runtime_run_for(&runtime, 100);
  CHECK(bd_shell_present_count(runtime.shell) >= presents,
        "failed sleep corrupted present count");

  /* Recovery: the same tap now succeeds. */
  runtime.av.fail_read = false;
  if (!enter_light_sleep(&runtime)) {
    destroy_runtime(&runtime);
    return;
  }
  CHECK(runtime.av.brightness == 0, "retry sleep did not zero brightness");

  /* Failed zero write restores the snapshot and leaves the shell awake. */
  wake_with_power_press(&runtime, true);
  CHECK(runtime.av.brightness == TEST_BRIGHTNESS,
        "wake before write failure restored %d", runtime.av.brightness);
  runtime_run_for(&runtime, 1050); /* wake press guard */
  runtime.av.fail_zero = 1;
  runtime.av.zero_changes_on_failure = true;
  int zero_writes_before = runtime.av.zero_writes;
  int restores_before = runtime.av.restore_writes;
  tap_power(&runtime);
  runtime_run_for(&runtime, 120);
  CHECK(runtime_power_state(&runtime) == BD_POWER_STATE_IDLE,
        "failed brightness write reported sleep");
  CHECK(!runtime_sleeping(&runtime), "failed brightness write marked sleeping");
  CHECK(runtime.av.zero_writes == zero_writes_before + 1,
        "failed brightness write not attempted");
  CHECK(runtime.av.restore_writes > restores_before,
        "failed brightness write skipped restore");
  CHECK(runtime.av.brightness == TEST_BRIGHTNESS,
        "failed brightness write left %d, wanted %d", runtime.av.brightness,
        TEST_BRIGHTNESS);
  bd_session_app_info after_write_failure;
  CHECK(bd_session_find(runtime.session, TEST_APP, &after_write_failure) &&
            !after_write_failure.frozen && proc_state(app_pid) != 'T',
        "failed brightness write lost app resume");

  /* A later tap succeeds after the write recovers. */
  runtime.av.fail_zero = 0;
  if (!enter_light_sleep(&runtime)) {
    destroy_runtime(&runtime);
    return;
  }

  /* Restore failure on wake stays DEGRADED and retries on the next press. */
  runtime.av.fail_restore = 1;
  uint64_t degraded_presents = bd_shell_present_count(runtime.shell);
  wake_with_power_press(&runtime, false);
  CHECK(runtime_power_state(&runtime) == BD_POWER_STATE_DEGRADED,
        "restore failure did not keep DEGRADED");
  CHECK(runtime_sleeping(&runtime), "DEGRADED is not treated as asleep");
  CHECK(runtime.av.brightness == 0, "failed restore changed brightness");
  CHECK(runtime.av.restore_failures >= 1, "restore failure not recorded");
  bd_session_app_info degraded_app;
  CHECK(bd_session_find(runtime.session, TEST_APP, &degraded_app) &&
            degraded_app.state == BD_SESSION_FOREGROUND &&
            degraded_app.pid == app_pid &&
            bd_session_background_count(runtime.session) == background,
        "restore failure lost foreground identity/quota");
  CHECK(bd_shell_present_count(runtime.shell) == degraded_presents,
        "DEGRADED presented a frame");
  CHECK(!bd_shell_render(runtime.shell), "DEGRADED rendered a frame");
  CHECK(bd_shell_present_count(runtime.shell) == degraded_presents,
        "DEGRADED direct render presented");

  wake_with_power_press(&runtime, true);
  CHECK(runtime.av.brightness == TEST_BRIGHTNESS,
        "wake retry restored %d, wanted %d", runtime.av.brightness,
        TEST_BRIGHTNESS);
  bd_session_app_info final_app;
  CHECK(bd_session_find(runtime.session, TEST_APP, &final_app) &&
            !final_app.frozen && final_app.state == BD_SESSION_FOREGROUND &&
            final_app.pid == app_pid && proc_state(app_pid) != 'T',
        "wake retry lost app identity/resume");
  CHECK(bd_session_background_count(runtime.session) == background,
        "failure paths changed quota");

  /* Rejected SIGCONT: keep the exact frozen instance and retry thaw. */
  runtime_run_for(&runtime, 1050); /* wake press guard */
  if (!enter_light_sleep(&runtime)) {
    destroy_runtime(&runtime);
    return;
  }
  deny_next_sigcont(app_pid);
  wake_with_power_press(&runtime, false);
  CHECK(runtime_power_state(&runtime) == BD_POWER_STATE_DEGRADED,
        "failed thaw did not stay DEGRADED");
  CHECK(runtime_sleeping(&runtime), "failed thaw marked the shell awake");
  CHECK(runtime.av.brightness == TEST_BRIGHTNESS,
        "failed thaw did not restore already-completed brightness");
  bd_session_app_info thaw_failed;
  CHECK(bd_session_find(runtime.session, TEST_APP, &thaw_failed) &&
            thaw_failed.frozen && thaw_failed.state == BD_SESSION_FOREGROUND &&
            thaw_failed.pid == app_pid && proc_state(app_pid) == 'T',
        "failed thaw dropped the exact frozen instance");
  wake_with_power_press(&runtime, true);
  bd_session_app_info thaw_retried;
  CHECK(bd_session_find(runtime.session, TEST_APP, &thaw_retried) &&
            !thaw_retried.frozen && thaw_retried.pid == app_pid &&
            proc_state(app_pid) != 'T',
        "thaw retry did not resume the exact instance");
  allow_sigcont();

  /*
   * Freeze confirmation timeout plus a rejected rollback SIGCONT: the app
   * remains frozen, the shell enters DEGRADED, and the next wake must consult
   * that exact instance and retry thaw before clearing ownership.
   */
  runtime_run_for(&runtime, 1050); /* wake press guard */
  int zero_before_freeze_failure = runtime.av.zero_writes;
  stall_waitpid_for(app_pid, 1400);
  deny_sigcont_count(app_pid, 2);
  tap_power(&runtime);
  runtime_run_for(&runtime, 1300);
  allow_waitpid();
  CHECK(runtime_power_state(&runtime) == BD_POWER_STATE_DEGRADED,
        "freeze rollback failure did not stay DEGRADED");
  CHECK(runtime_sleeping(&runtime) && runtime_input_suspended(&runtime),
        "freeze rollback failure left input active");
  CHECK(runtime.av.zero_writes == zero_before_freeze_failure,
        "failed freeze wrote the display");
  CHECK(runtime.av.brightness == TEST_BRIGHTNESS,
        "failed freeze changed brightness");
  bd_session_app_info freeze_failed;
  CHECK(bd_session_find(runtime.session, TEST_APP, &freeze_failed) &&
            freeze_failed.frozen && freeze_failed.state == BD_SESSION_FOREGROUND &&
            freeze_failed.pid == app_pid && proc_state(app_pid) == 'T',
        "failed freeze did not retain the exact frozen instance");
  wake_with_power_press(&runtime, true);
  bd_session_app_info freeze_retried;
  CHECK(bd_session_find(runtime.session, TEST_APP, &freeze_retried) &&
            !freeze_retried.frozen && freeze_retried.state == BD_SESSION_FOREGROUND &&
            freeze_retried.pid == app_pid && proc_state(app_pid) != 'T',
        "freeze recovery retry did not resume the exact instance");
  CHECK(runtime.av.brightness == TEST_BRIGHTNESS,
        "freeze recovery retry changed brightness");
  allow_sigcont();
  destroy_runtime(&runtime);
}

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  if (argc < 2) {
    fprintf(stderr, "usage: bd_shell_power_test <bd_session_child>\n");
    return 2;
  }
  const char *child_path = argv[1];
  test_home_sleep_and_inputd(child_path);
  test_source_switch_and_event_time(child_path);
  test_app_freeze_and_plugin_heartbeat(child_path);
  test_settings_location_and_repeat(child_path);
  test_brightness_failures(child_path);
  if (failures != 0) {
    fprintf(stderr, "shell_power_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("shell_power_test: OK\n");
  return 0;
}
