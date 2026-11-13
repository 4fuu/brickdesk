#define _POSIX_C_SOURCE 200809L

#include "brickdesk/plugin.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "host_internal.h"
#include "ipc.h"
#include "json.h"
#include "manifest.h"

#ifndef BRICKDESK_PLUGIN_DIR
#define BRICKDESK_PLUGIN_DIR "plugins"
#endif
#ifndef BRICKDESK_STATE_DIR
#define BRICKDESK_STATE_DIR ""
#endif

#define BD_PLUGIN_DATA_SUFFIX "/plugin-data"
#define BD_PLUGIN_CACHE_SUFFIX "/plugin-cache"
#define BD_PLUGIN_SETTINGS_DIR "plugin-settings"
#define BD_PLUGIN_SETTINGS_FILE "settings.json"
#define BD_PLUGIN_SETTINGS_FILE_MAX (16 * 1024)

static uint64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

const char *bd_plugin_state_name(bd_plugin_state state) {
  switch (state) {
    case BD_PLUGIN_DISCOVERED:
      return "discovered";
    case BD_PLUGIN_STARTING:
      return "starting";
    case BD_PLUGIN_HANDSHAKING:
      return "handshaking";
    case BD_PLUGIN_RUNNING:
      return "running";
    case BD_PLUGIN_STOPPING:
      return "stopping";
    case BD_PLUGIN_BACKOFF:
      return "backoff";
    case BD_PLUGIN_QUARANTINED:
      return "quarantined";
    case BD_PLUGIN_STOPPED:
      return "stopped";
    default:
      return "unknown";
  }
}

bd_plugin_limits bd_plugin_default_limits(void) {
  bd_plugin_limits limits;
  limits.handshake_timeout_ms = 2000;
  limits.ping_interval_ms = 5000;
  limits.ping_timeout_ms = 15000;
  limits.idle_stop_ms = 60000;
  limits.shutdown_timeout_ms = 3000;
  limits.restart_base_ms = 1000;
  limits.restart_max_ms = 8000;
  limits.restart_window_ms = 60000;
  limits.restart_limit = 3;
  return limits;
}

void bd_plugin_log(const bd_plugin *plugin, const char *format, ...) {
  if (plugin == NULL || plugin->host == NULL || !plugin->host->verbose) {
    return;
  }
  fprintf(stderr, "brickdesk-plugin: %s: ",
          plugin->manifest.id[0] != '\0' ? plugin->manifest.id : "?");
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
}

void bd_plugin_mark_dirty(bd_plugin_host *host) {
  if (host != NULL) {
    host->dirty = true;
  }
}

void bd_plugin_diagnostic(bd_plugin_host *host, const char *format, ...) {
  if (host == NULL) {
    return;
  }
  char line[256];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (host->diagnostic_count < BD_PLUGIN_MAX_DIAGNOSTICS) {
    snprintf(host->diagnostics[host->diagnostic_count++],
             sizeof(host->diagnostics[0]), "%s", line);
  }
  if (host->verbose) {
    fprintf(stderr, "brickdesk-plugin: %s\n", line);
  }
}

static void set_state(bd_plugin *plugin, bd_plugin_state state) {
  if (plugin->state == state) {
    return;
  }
  plugin->state = state;
  bd_plugin_mark_dirty(plugin->host);
  bd_plugin_log(plugin, "state=%s", bd_plugin_state_name(state));
}

static bool join_path(char *out, size_t out_size, const char *dir,
                      const char *name) {
  int written = snprintf(out, out_size, "%s/%s", dir, name);
  return written > 0 && (size_t)written < out_size;
}

static bool make_runtime_path(const bd_plugin *plugin, const char *suffix,
                              char *out, size_t out_size) {
  char safe[40];
  size_t used = 0;
  for (const char *cursor = plugin->manifest.id;
       *cursor != '\0' && used + 1 < sizeof(safe); ++cursor) {
    char c = *cursor;
    safe[used++] = ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '-' || c == '_')
                       ? c
                       : '_';
  }
  safe[used] = '\0';
  int written = snprintf(out, out_size, "%s/p%d-%s.%s", plugin->host->runtime_dir,
                         plugin->index, safe, suffix);
  if (written > 0 && (size_t)written < out_size) {
    return true;
  }
  written = snprintf(out, out_size, "%s/p%d.%s", plugin->host->runtime_dir,
                     plugin->index, suffix);
  return written > 0 && (size_t)written < out_size;
}

static void json_escape(char *out, size_t out_size, const char *text) {
  if (out_size == 0) {
    return;
  }
  size_t used = 0;
  if (text == NULL) {
    text = "";
  }
  for (const unsigned char *cursor = (const unsigned char *)text;
       *cursor != '\0' && used + 1 < out_size; ++cursor) {
    unsigned char c = *cursor;
    if (c == '"' || c == '\\') {
      if (used + 2 >= out_size) {
        break;
      }
      out[used++] = '\\';
      out[used++] = (char)c;
    } else if (c < 0x20) {
      if (used + 6 >= out_size) {
        break;
      }
      int written = snprintf(out + used, out_size - used, "\\u%04x", c);
      if (written < 0) {
        break;
      }
      used += (size_t)written;
    } else {
      out[used++] = (char)c;
    }
  }
  out[used] = '\0';
}

static bd_plugin_host *host_of(bd_plugin *plugin) {
  return plugin != NULL ? plugin->host : NULL;
}

static void close_connection(bd_plugin *plugin) {
  if (plugin->fd >= 0) {
    close(plugin->fd);
    plugin->fd = -1;
  }
  plugin->input_length = 0;
  plugin->output_length = 0;
}

static void close_listener(bd_plugin *plugin) {
  if (plugin->listen_fd >= 0) {
    close(plugin->listen_fd);
    plugin->listen_fd = -1;
  }
  if (plugin->socket_path[0] != '\0') {
    unlink(plugin->socket_path);
  }
}

static void release_surface(bd_plugin *plugin) {
  if (plugin->frame_header != NULL) {
    munmap(plugin->frame_header, plugin->frame_size);
    plugin->frame_header = NULL;
  }
  if (plugin->shm_fd >= 0) {
    close(plugin->shm_fd);
    plugin->shm_fd = -1;
  }
  if (plugin->shm_path[0] != '\0') {
    unlink(plugin->shm_path);
  }
  plugin->frame_size = 0;
  plugin->last_frame_seq = 0;
  plugin->last_frame_read_ms = 0;
  plugin->frame_clock_started = false;
}

static bool create_surface(bd_plugin *plugin) {
  if (!make_runtime_path(plugin, "shm", plugin->shm_path,
                         sizeof(plugin->shm_path))) {
    return false;
  }
  size_t size = sizeof(bd_plugin_frame_header) +
                (size_t)BD_PLUGIN_TILE_MAX_DIM * BD_PLUGIN_TILE_MAX_DIM * 4u;
  int fd = open(plugin->shm_path,
                O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
  if (fd >= 0) {
    fchmod(fd, 0660);
  }
  if (fd < 0) {
    plugin->shm_path[0] = '\0';
    return false;
  }
  if (ftruncate(fd, (off_t)size) != 0) {
    close(fd);
    unlink(plugin->shm_path);
    plugin->shm_path[0] = '\0';
    return false;
  }
  void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    close(fd);
    unlink(plugin->shm_path);
    plugin->shm_path[0] = '\0';
    return false;
  }
  plugin->shm_fd = fd;
  plugin->frame_header = (bd_plugin_frame_header *)mapping;
  plugin->frame_size = size;
  memset(plugin->frame_header, 0, sizeof(*plugin->frame_header));
  plugin->frame_header->magic = BD_PLUGIN_FRAME_MAGIC;
  plugin->frame_header->version = BD_PLUGIN_FRAME_VERSION;
  plugin->frame_header->format = BD_PLUGIN_FRAME_FORMAT_RGBA8888;
  return true;
}

static bool queue_line(bd_plugin *plugin, const char *line, size_t length);

static bool flush_output(bd_plugin *plugin) {
  while (plugin->fd >= 0 && plugin->output_length > 0) {
    ssize_t sent = send(plugin->fd, plugin->output, plugin->output_length,
                        MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return true;
      }
      return false;
    }
    if (sent == 0) {
      return false;
    }
    memmove(plugin->output, plugin->output + (size_t)sent,
            plugin->output_length - (size_t)sent);
    plugin->output_length -= (size_t)sent;
  }
  return true;
}

static bool queue_line(bd_plugin *plugin, const char *line, size_t length) {
  if (plugin->fd < 0 || line == NULL) {
    return false;
  }
  if (length + 1 > sizeof(plugin->output) - plugin->output_length) {
    bd_plugin_log(plugin, "output queue overflow; dropping connection");
    plugin->protocol_errors++;
    close_connection(plugin);
    return false;
  }
  memcpy(plugin->output + plugin->output_length, line, length);
  plugin->output_length += length;
  plugin->output[plugin->output_length++] = '\n';
  if (!flush_output(plugin)) {
    plugin->protocol_errors++;
    close_connection(plugin);
    return false;
  }
  return true;
}

static bool queue_format(bd_plugin *plugin, const char *format, ...) {
  char line[BD_PLUGIN_LINE_MAX];
  va_list args;
  va_start(args, format);
  int written = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (written < 0 || (size_t)written >= sizeof(line)) {
    return false;
  }
  return queue_line(plugin, line, (size_t)written);
}

static void send_hello(bd_plugin *plugin) {
  char data_path[BD_PLUGIN_MAX_PATH * 2 + 1];
  char cache_path[BD_PLUGIN_MAX_PATH * 2 + 1];
  char surface_path[BD_PLUGIN_MAX_PATH * 2 + 1];
  char locale[128];
  char accent[16];
  char session[32];
  char data_full[BD_PLUGIN_MAX_PATH + BD_PLUGIN_MAX_ID + 32];
  char cache_full[BD_PLUGIN_MAX_PATH + BD_PLUGIN_MAX_ID + 32];
  snprintf(data_full, sizeof(data_full), "%s/%s", plugin->host->data_dir,
           plugin->manifest.id);
  snprintf(cache_full, sizeof(cache_full), "%s/%s", plugin->host->runtime_dir,
           plugin->manifest.id);
  json_escape(data_path, sizeof(data_path), data_full);
  json_escape(cache_path, sizeof(cache_path), cache_full);
  json_escape(surface_path, sizeof(surface_path), plugin->shm_path);
  json_escape(locale, sizeof(locale), plugin->host->locale);
  snprintf(accent, sizeof(accent), "#%06x", plugin->host->accent & 0xffffffu);
  snprintf(session, sizeof(session), "%08x",
           (unsigned)((getpid() * 2654435761u) ^ (uint32_t)monotonic_ms()));
  bd_plugin_log(plugin, "handshake");
  queue_format(
      plugin,
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"host.hello\",\"params\":{"
      "\"api\":%d,\"plugin_id\":\"%s\",\"display\":{\"w\":%d,\"h\":%d,"
      "\"scale\":1.0},\"theme\":{\"name\":\"default\",\"dark\":%s,"
      "\"accent\":\"%s\"},\"locale\":\"%s\",\"paths\":{"
      "\"data\":\"%s\",\"cache\":\"%s\"},\"surface\":\"%s\","
      "\"session_id\":\"%s\"}}",
      BD_PLUGIN_API_VERSION, plugin->manifest.id, plugin->host->display_width,
      plugin->host->display_height, plugin->host->dark ? "true" : "false",
      accent, locale, data_path, cache_path, surface_path, session);
}

static void mark_crash(bd_plugin *plugin, uint64_t now) {
  plugin->crash_times[plugin->crash_head] = now;
  plugin->crash_head = (plugin->crash_head + 1) % BD_PLUGIN_MAX_CRASHES;
  if (plugin->crash_count < BD_PLUGIN_MAX_CRASHES) {
    ++plugin->crash_count;
  }
}

static int crashes_in_window(const bd_plugin *plugin, uint64_t now) {
  int count = 0;
  uint64_t window = (uint64_t)plugin->host->limits.restart_window_ms;
  for (int i = 0; i < plugin->crash_count; ++i) {
    uint64_t stamp = plugin->crash_times[i];
    if (window == 0 || now - stamp <= window) {
      ++count;
    }
  }
  return count;
}

static bool wants_run(const bd_plugin *plugin) {
  return plugin->visible || plugin->settings_open ||
         (plugin->manifest.capabilities & BD_PLUGIN_CAP_BACKGROUND) != 0 ||
         plugin->manifest.autostart;
}

static void process_exit(bd_plugin *plugin, int status, uint64_t now);
static bool plugin_start(bd_plugin *plugin, uint64_t now);

static void schedule_after_failure(bd_plugin *plugin, uint64_t now) {
  bd_plugin_host *host = host_of(plugin);
  ++plugin->crashes;
  mark_crash(plugin, now);
  if (!wants_run(plugin)) {
    set_state(plugin, BD_PLUGIN_STOPPED);
    return;
  }
  int limit = host->limits.restart_limit;
  if (limit <= 0 || crashes_in_window(plugin, now) > limit) {
    set_state(plugin, BD_PLUGIN_QUARANTINED);
    bd_plugin_log(plugin, "crash limit reached; quarantined");
    bd_plugin_mark_dirty(host);
    return;
  }
  uint64_t delay = (uint64_t)(host->limits.restart_base_ms > 0
                                  ? host->limits.restart_base_ms
                                  : 1);
  for (int i = 0; i < plugin->restart_attempt; ++i) {
    if (delay >= (uint64_t)host->limits.restart_max_ms) {
      break;
    }
    delay *= 2;
  }
  uint64_t maximum = (uint64_t)(host->limits.restart_max_ms > 0
                                    ? host->limits.restart_max_ms
                                    : 1);
  if (delay > maximum) {
    delay = maximum;
  }
  ++plugin->restart_attempt;
  plugin->restart_at_ms = now + delay;
  set_state(plugin, BD_PLUGIN_BACKOFF);
  bd_plugin_log(plugin, "restart in %llums", (unsigned long long)delay);
}

static void process_exit(bd_plugin *plugin, int status, uint64_t now) {
  close_connection(plugin);
  close_listener(plugin);
  release_surface(plugin);
  plugin->pid = -1;
  if (plugin->state == BD_PLUGIN_STOPPING) {
    plugin->stop_graceful = false;
    plugin->stop_deadline_ms = 0;
    set_state(plugin, BD_PLUGIN_STOPPED);
    if (!plugin->host->shutting_down && wants_run(plugin)) {
      plugin_start(plugin, now);
    }
    return;
  }
  if (plugin->state == BD_PLUGIN_HANDSHAKING ||
      plugin->state == BD_PLUGIN_STARTING) {
    ++plugin->handshake_timeouts;
  }
  if (WIFEXITED(status)) {
    plugin->last_exit_status = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    plugin->last_exit_status = -WTERMSIG(status);
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    bd_plugin_log(plugin, "exited");
  } else if (WIFSIGNALED(status)) {
    bd_plugin_log(plugin, "killed by signal %d", WTERMSIG(status));
  } else if (WIFEXITED(status)) {
    bd_plugin_log(plugin, "exited with status %d", WEXITSTATUS(status));
  }
  if (plugin->running_since_ms != 0 &&
      now - plugin->running_since_ms >=
          (uint64_t)plugin->host->limits.restart_window_ms) {
    plugin->restart_attempt = 0;
  }
  plugin->running_since_ms = 0;
  schedule_after_failure(plugin, now);
}

static void kill_and_reap(bd_plugin *plugin, uint64_t now) {
  close_connection(plugin);
  close_listener(plugin);
  release_surface(plugin);
  int status = 0;
  bool reaped = false;
  if (plugin->pid > 0) {
    kill(plugin->pid, SIGKILL);
    pid_t result = 0;
    do {
      result = waitpid(plugin->pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    reaped = result == plugin->pid;
    plugin->pid = -1;
  }
  if (reaped) {
    process_exit(plugin, status, now);
  } else if (plugin->state == BD_PLUGIN_STOPPING) {
    plugin->stop_graceful = false;
    plugin->stop_deadline_ms = 0;
    set_state(plugin, BD_PLUGIN_STOPPED);
  } else if (plugin->state == BD_PLUGIN_STARTING ||
             plugin->state == BD_PLUGIN_HANDSHAKING ||
             plugin->state == BD_PLUGIN_RUNNING) {
    process_exit(plugin, 1 << 8, now);
  }
}

static char *expand_arg(const char *arg, const bd_plugin *plugin, char *out,
                        size_t out_size) {
  char data_dir[BD_PLUGIN_MAX_PATH + BD_PLUGIN_MAX_ID + 32];
  char cache_dir[BD_PLUGIN_MAX_PATH + BD_PLUGIN_MAX_ID + 32];
  snprintf(data_dir, sizeof(data_dir), "%s/%s", plugin->host->data_dir,
           plugin->manifest.id);
  snprintf(cache_dir, sizeof(cache_dir), "%s/%s", plugin->host->runtime_dir,
           plugin->manifest.id);
  size_t used = 0;
  int written = 0;
  for (const char *cursor = arg; *cursor != '\0';) {
    const char *replacement = NULL;
    size_t skip = 0;
    if (strncmp(cursor, "{socket}", 8) == 0) {
      replacement = plugin->socket_path;
      skip = 8;
    } else if (strncmp(cursor, "{plugin_dir}", 12) == 0) {
      replacement = plugin->manifest.dir;
      skip = 12;
    } else if (strncmp(cursor, "{data_dir}", 10) == 0) {
      replacement = data_dir;
      skip = 10;
    } else if (strncmp(cursor, "{cache_dir}", 11) == 0) {
      replacement = cache_dir;
      skip = 11;
    }
    if (replacement != NULL) {
      written = snprintf(out + used, out_size - used, "%s", replacement);
      if (written < 0 || (size_t)written >= out_size - used) {
        return NULL;
      }
      used += (size_t)written;
      cursor += skip;
    } else {
      if (used + 1 >= out_size) {
        return NULL;
      }
      out[used++] = *cursor++;
    }
  }
  out[used] = '\0';
  return out;
}

static bool plugin_start(bd_plugin *plugin, uint64_t now) {
  bd_plugin_host *host = host_of(plugin);
  if (plugin->state == BD_PLUGIN_RUNNING ||
      plugin->state == BD_PLUGIN_STARTING ||
      plugin->state == BD_PLUGIN_HANDSHAKING) {
    return true;
  }
  if (plugin->state == BD_PLUGIN_QUARANTINED) {
    return false;
  }
  bool was_backoff = plugin->state == BD_PLUGIN_BACKOFF;
  char data_dir[BD_PLUGIN_MAX_PATH + BD_PLUGIN_MAX_ID + 32];
  char cache_dir[BD_PLUGIN_MAX_PATH + BD_PLUGIN_MAX_ID + 32];
  snprintf(data_dir, sizeof(data_dir), "%s/%s", host->data_dir,
           plugin->manifest.id);
  snprintf(cache_dir, sizeof(cache_dir), "%s/%s", host->runtime_dir,
           plugin->manifest.id);
  if (!bd_plugin_make_dirs(data_dir) || !bd_plugin_make_dirs(cache_dir)) {
    bd_plugin_diagnostic(host, "%s: cannot create data directories",
                         plugin->manifest.id);
    return false;
  }
  if (!make_runtime_path(plugin, "sock", plugin->socket_path,
                         sizeof(plugin->socket_path))) {
    bd_plugin_diagnostic(host, "%s: socket path is too long",
                         plugin->manifest.id);
    return false;
  }
  char error[128] = {0};
  plugin->listen_fd =
      bd_plugin_listen_socket(plugin->socket_path, error, sizeof(error));
  if (plugin->listen_fd < 0) {
    bd_plugin_diagnostic(host, "%s: %s", plugin->manifest.id, error);
    return false;
  }
  if (!create_surface(plugin)) {
    bd_plugin_diagnostic(host, "%s: cannot create the tile surface",
                         plugin->manifest.id);
    close_listener(plugin);
    return false;
  }

  char args[BD_PLUGIN_MAX_ENTRY_ARGS][BD_PLUGIN_MAX_PATH * 2];
  char *argv[BD_PLUGIN_MAX_ENTRY_ARGS + 1];
  memset(argv, 0, sizeof(argv));
  for (int i = 0; i < plugin->manifest.entry_count; ++i) {
    if (expand_arg(plugin->manifest.entry[i], plugin, args[i],
                   sizeof(args[i])) == NULL) {
      bd_plugin_diagnostic(host, "%s: entry argument is too long",
                           plugin->manifest.id);
      close_listener(plugin);
      release_surface(plugin);
      return false;
    }
    argv[i] = args[i];
  }

  pid_t pid = fork();
  if (pid < 0) {
    bd_plugin_diagnostic(host, "%s: fork failed", plugin->manifest.id);
    close_listener(plugin);
    release_surface(plugin);
    return false;
  }
  if (pid == 0) {
    setpgid(0, 0);
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    umask(077);
    struct rlimit limit;
    limit.rlim_cur = (rlim_t)plugin->manifest.memory_limit_bytes;
    limit.rlim_max = (rlim_t)plugin->manifest.memory_limit_bytes;
    setrlimit(RLIMIT_AS, &limit);
    limit.rlim_cur = (rlim_t)plugin->manifest.nofile_limit;
    limit.rlim_max = (rlim_t)plugin->manifest.nofile_limit;
    setrlimit(RLIMIT_NOFILE, &limit);
    if (plugin->manifest.nice_value > 0) {
      setpriority(PRIO_PROCESS, 0, plugin->manifest.nice_value);
    }
    if (chdir(plugin->manifest.dir) != 0) {
      fprintf(stderr, "brickdesk-plugin: chdir %s failed\n",
              plugin->manifest.dir);
      _exit(127);
    }
    setenv("BRICKDESK_PLUGIN_SOCKET", plugin->socket_path, 1);
    setenv("BRICKDESK_PLUGIN_ID", plugin->manifest.id, 1);
    setenv("BRICKDESK_PLUGIN_DIR", plugin->manifest.dir, 1);
    setenv("BRICKDESK_PLUGIN_DATA", data_dir, 1);
    setenv("BRICKDESK_PLUGIN_CACHE", cache_dir, 1);
    long maximum_fd = sysconf(_SC_OPEN_MAX);
    if (maximum_fd < 0 || maximum_fd > 1024) {
      maximum_fd = 1024;
    }
    for (int fd = 3; fd < (int)maximum_fd; ++fd) {
      close(fd);
    }
    execv(argv[0], argv);
    fprintf(stderr, "brickdesk-plugin: exec %s failed: %s\n", argv[0],
            strerror(errno));
    _exit(127);
  }
  setpgid(pid, pid);
  plugin->pid = (int)pid;
  plugin->fd = -1;
  plugin->input_length = 0;
  plugin->output_length = 0;
  plugin->started_ms = now;
  plugin->running_since_ms = 0;
  plugin->last_rx_ms = now;
  plugin->last_ping_ms = now;
  plugin->restart_at_ms = 0;
  plugin->stop_deadline_ms = 0;
  plugin->handshake_deadline_ms =
      now + (uint64_t)(host->limits.handshake_timeout_ms > 0
                           ? host->limits.handshake_timeout_ms
                           : 1);
  ++plugin->starts;
  if (was_backoff) {
    ++plugin->restarts;
  }
  set_state(plugin, BD_PLUGIN_STARTING);
  bd_plugin_log(plugin, "started pid=%d", plugin->pid);
  return true;
}

static void accept_connection(bd_plugin *plugin, uint64_t now) {
  if (plugin->listen_fd < 0) {
    return;
  }
  int fd = accept(plugin->listen_fd, NULL, NULL);
  if (fd < 0) {
    return;
  }
  if (plugin->fd >= 0) {
    close(fd);
    return;
  }
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  plugin->fd = fd;
  plugin->last_rx_ms = now;
  close_listener(plugin);
  set_state(plugin, BD_PLUGIN_HANDSHAKING);
  send_hello(plugin);
}

static void finish_handshake(bd_plugin *plugin, uint64_t now) {
  plugin->running_since_ms = now;
  plugin->last_rx_ms = now;
  plugin->idle_deadline_ms =
      plugin->visible || plugin->settings_open || plugin->manifest.autostart ||
              (plugin->manifest.capabilities & BD_PLUGIN_CAP_BACKGROUND) != 0
          ? 0
          : now + (uint64_t)plugin->host->limits.idle_stop_ms;
  set_state(plugin, BD_PLUGIN_RUNNING);
  bd_plugin_log(plugin, "ready api=%d", plugin->manifest.api);
}

static void send_error(bd_plugin *plugin, long id, int code, const char *message) {
  char escaped[256];
  json_escape(escaped, sizeof(escaped), message);
  queue_format(plugin,
               "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"error\":{\"code\":%d,"
               "\"message\":\"%s\"}}",
               id, code, escaped);
}

static bool header_valid(const bd_plugin *plugin) {
  const bd_plugin_frame_header *header = plugin->frame_header;
  if (header == NULL) {
    return false;
  }
  if (header->magic != BD_PLUGIN_FRAME_MAGIC ||
      header->version != BD_PLUGIN_FRAME_VERSION ||
      header->format != BD_PLUGIN_FRAME_FORMAT_RGBA8888) {
    return false;
  }
  if (header->width == 0 || header->height == 0 ||
      header->width > BD_PLUGIN_TILE_MAX_DIM ||
      header->height > BD_PLUGIN_TILE_MAX_DIM) {
    return false;
  }
  if (header->stride != header->width * 4u) {
    return false;
  }
  return true;
}

static bool parse_ui_token(const bd_plugin_json *json,
                           const jsmntok_t *token, bd_plugin_ui *out,
                           char *error, size_t error_size) {
  const char *start = json->text + token->start;
  size_t span = (size_t)(token->end - token->start);
  char *slice = (char *)malloc(span + 1);
  if (slice == NULL) {
    snprintf(error, error_size, "out of memory");
    return false;
  }
  memcpy(slice, start, span);
  slice[span] = '\0';
  bool ok = bd_plugin_ui_parse(slice, span, out, error, error_size);
  free(slice);
  return ok;
}

static void handle_ui_set(bd_plugin *plugin, const bd_plugin_json *json,
                          const jsmntok_t *params, long id) {
  if (!bd_plugin_json_is_object(params)) {
    ++plugin->protocol_errors;
    bd_plugin_log(plugin, "plugin.ui.set params must be an object");
    if (id > 0) {
      send_error(plugin, id, -32602, "params must be an object");
    }
    return;
  }
  const jsmntok_t *tile = bd_plugin_json_member(json, params, "tile");
  const jsmntok_t *settings = bd_plugin_json_member(json, params, "settings");
  if (tile == NULL && settings == NULL) {
    ++plugin->protocol_errors;
    bd_plugin_log(plugin, "plugin.ui.set has no tile or settings tree");
    if (id > 0) {
      send_error(plugin, id, -32602, "tile or settings is required");
    }
    return;
  }
  bd_plugin_ui *tile_ui = (bd_plugin_ui *)malloc(sizeof(*tile_ui));
  bd_plugin_ui *settings_ui = (bd_plugin_ui *)malloc(sizeof(*settings_ui));
  if (tile_ui == NULL || settings_ui == NULL) {
    free(tile_ui);
    free(settings_ui);
    if (id > 0) {
      send_error(plugin, id, -32005, "out of memory");
    }
    return;
  }
  char error[192] = {0};
  if (tile != NULL &&
      !parse_ui_token(json, tile, tile_ui, error, sizeof(error))) {
    ++plugin->protocol_errors;
    bd_plugin_log(plugin, "tile UI rejected: %s", error);
    if (id > 0) {
      send_error(plugin, id, -32602, error);
    }
    free(tile_ui);
    free(settings_ui);
    return;
  }
  if (settings != NULL &&
      !parse_ui_token(json, settings, settings_ui, error, sizeof(error))) {
    ++plugin->protocol_errors;
    bd_plugin_log(plugin, "settings UI rejected: %s", error);
    if (id > 0) {
      send_error(plugin, id, -32602, error);
    }
    free(tile_ui);
    free(settings_ui);
    return;
  }
  if (tile != NULL) {
    plugin->ui_tile = *tile_ui;
    plugin->has_tile_ui = true;
  }
  if (settings != NULL) {
    plugin->ui_settings = *settings_ui;
    plugin->has_settings_ui = true;
  }
  free(tile_ui);
  free(settings_ui);
  ++plugin->ui_generation;
  bd_plugin_mark_dirty(plugin->host);
  if (id > 0) {
    queue_format(plugin, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{}}", id);
  }
}

static void handle_tile_acquire(bd_plugin *plugin, const bd_plugin_json *json,
                                const jsmntok_t *params, long id) {
  int width = 0;
  int height = 0;
  if (params != NULL) {
    const jsmntok_t *value = bd_plugin_json_member(json, params, "w");
    if (value != NULL && !bd_plugin_json_int(json, value, &width)) {
      width = 0;
    }
    value = bd_plugin_json_member(json, params, "h");
    if (value != NULL && !bd_plugin_json_int(json, value, &height)) {
      height = 0;
    }
  }
  if (width <= 0 || height <= 0 || width > BD_PLUGIN_TILE_MAX_DIM ||
      height > BD_PLUGIN_TILE_MAX_DIM) {
    if (id > 0) {
      send_error(plugin, id, -32602, "tile size must be 1..400");
    }
    return;
  }
  if (plugin->frame_header != NULL) {
    __atomic_store_n(&plugin->frame_header->width, (uint32_t)width,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&plugin->frame_header->height, (uint32_t)height,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&plugin->frame_header->stride, (uint32_t)(width * 4),
                     __ATOMIC_RELAXED);
    __atomic_store_n(&plugin->frame_header->seq, 0u, __ATOMIC_RELEASE);
    plugin->last_frame_seq = 0;
    plugin->announced_seq = 0;
    plugin->last_frame_read_ms = 0;
    plugin->frame_clock_started = false;
  }
  if (id > 0) {
    char escaped[BD_PLUGIN_MAX_PATH * 2 + 1];
    json_escape(escaped, sizeof(escaped), plugin->shm_path);
    queue_format(plugin,
                 "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"w\":%d,"
                 "\"h\":%d,\"stride\":%d,\"surface\":\"%s\"}}",
                 id, width, height, width * 4, escaped);
  }
}

static void handle_tile_frame(bd_plugin *plugin, long id) {
  if (!header_valid(plugin)) {
    ++plugin->protocol_errors;
    bd_plugin_log(plugin, "frame header is invalid");
    if (id > 0) {
      send_error(plugin, id, -32602, "frame header is invalid");
    }
    return;
  }
  uint32_t seq =
      __atomic_load_n(&plugin->frame_header->seq, __ATOMIC_ACQUIRE);
  if (seq != plugin->announced_seq) {
    plugin->announced_seq = seq;
    bd_plugin_mark_dirty(plugin->host);
  }
  if (id > 0) {
    queue_format(plugin, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{}}", id);
  }
}

static int setting_index(const bd_plugin *plugin, const char *key) {
  for (int i = 0; i < plugin->manifest.setting_count; ++i) {
    if (strcmp(plugin->manifest.settings[i].key, key) == 0) {
      return i;
    }
  }
  return -1;
}

static bool setting_value_valid(const bd_plugin_setting_spec *spec,
                                const char *value) {
  if (value == NULL) {
    return false;
  }
  if (spec->type == BD_PLUGIN_SETTING_BOOL) {
    return strcmp(value, "true") == 0 || strcmp(value, "false") == 0;
  }
  if (spec->type == BD_PLUGIN_SETTING_INT) {
    char *end = NULL;
    errno = 0;
    strtol(value, &end, 10);
    return errno == 0 && end != value && *end == '\0';
  }
  if (spec->type == BD_PLUGIN_SETTING_FLOAT) {
    char *end = NULL;
    errno = 0;
    strtod(value, &end);
    return errno == 0 && end != value && *end == '\0';
  }
  return strlen(value) < BD_PLUGIN_MAX_SETTING_VALUE;
}

/* The manifest validates ids; repeat the check before using one in a path. */
static bool setting_id_safe(const char *id) {
  if (id == NULL || id[0] == '\0' || id[0] == '.' || id[0] == '-') {
    return false;
  }
  for (const char *cursor = id; *cursor != '\0'; ++cursor) {
    char c = *cursor;
    bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '.' || c == '_' || c == '-';
    if (!allowed) {
      return false;
    }
  }
  return true;
}

static bool plugin_settings_dir(const bd_plugin *plugin, char *out,
                                size_t out_size) {
  if (!setting_id_safe(plugin->manifest.id)) {
    return false;
  }
  int written = snprintf(out, out_size, "%s/%s/%s", plugin->host->state_dir,
                         BD_PLUGIN_SETTINGS_DIR, plugin->manifest.id);
  return written > 0 && (size_t)written < out_size;
}

static bool plugin_settings_path(const bd_plugin *plugin, char *out,
                                 size_t out_size) {
  char directory[BD_PLUGIN_MAX_PATH + BD_PLUGIN_MAX_ID + 32];
  if (!plugin_settings_dir(plugin, directory, sizeof(directory))) {
    return false;
  }
  int written = snprintf(out, out_size, "%s/%s", directory,
                         BD_PLUGIN_SETTINGS_FILE);
  return written > 0 && (size_t)written < out_size;
}

/*
 * Reads the host-owned settings file for one plugin. Missing files and
 * unreadable or corrupt files leave the manifest defaults in place; corruption
 * is reported as a scan diagnostic instead of failing the plugin.
 */
static bool plugin_settings_load(bd_plugin *plugin) {
  if (plugin->manifest.setting_count == 0) {
    return true;
  }
  char path[BD_PLUGIN_MAX_PATH + BD_PLUGIN_MAX_ID + 64];
  if (!plugin_settings_path(plugin, path, sizeof(path))) {
    bd_plugin_diagnostic(plugin->host, "%s: settings path is too long",
                         plugin->manifest.id);
    return false;
  }
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (errno != ENOENT) {
      bd_plugin_diagnostic(plugin->host, "%s: cannot read settings: %s",
                           plugin->manifest.id, strerror(errno));
    }
    return errno == ENOENT;
  }
  struct stat info;
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      info.st_size > BD_PLUGIN_SETTINGS_FILE_MAX) {
    bd_plugin_diagnostic(plugin->host,
                         "%s: settings file is not a valid regular file",
                         plugin->manifest.id);
    close(fd);
    return false;
  }
  size_t length = (size_t)info.st_size;
  char *text = (char *)malloc(length + 1);
  if (text == NULL) {
    bd_plugin_diagnostic(plugin->host, "%s: out of memory reading settings",
                         plugin->manifest.id);
    close(fd);
    return false;
  }
  size_t offset = 0;
  bool read_ok = true;
  while (offset < length) {
    ssize_t received = read(fd, text + offset, length - offset);
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      read_ok = false;
      break;
    }
    if (received == 0) {
      read_ok = false;
      break;
    }
    offset += (size_t)received;
  }
  close(fd);
  if (!read_ok) {
    bd_plugin_diagnostic(plugin->host, "%s: cannot read settings: %s",
                         plugin->manifest.id, strerror(errno));
    free(text);
    return false;
  }
  text[length] = '\0';

  bd_plugin_json json;
  char error[192] = {0};
  bool ok = false;
  if (!bd_plugin_json_parse(text, length, &json, 256, error, sizeof(error))) {
    goto done;
  }
  const jsmntok_t *root = &json.tokens[0];
  const jsmntok_t *version = bd_plugin_json_member(&json, root, "version");
  const jsmntok_t *values = bd_plugin_json_member(&json, root, "settings");
  int version_number = 0;
  if (version == NULL || !bd_plugin_json_int(&json, version, &version_number)) {
    snprintf(error, sizeof(error), "missing settings version");
    goto done;
  }
  if (version_number != 1) {
    snprintf(error, sizeof(error), "unsupported settings version %d",
             version_number);
    goto done;
  }
  if (!bd_plugin_json_is_object(values)) {
    snprintf(error, sizeof(error), "settings must be an object");
    goto done;
  }
  char saved[BD_PLUGIN_MAX_SETTINGS][BD_PLUGIN_MAX_SETTING_VALUE];
  for (int i = 0; i < plugin->manifest.setting_count; ++i) {
    snprintf(saved[i], sizeof(saved[i]), "%s",
             plugin->settings_values[i]);
  }
  int token_index = (int)(values - json.tokens) + 1;
  for (int pair = 0; pair < values->size; ++pair) {
    if (token_index + 1 >= json.count) {
      snprintf(error, sizeof(error), "truncated settings object");
      goto done;
    }
    const jsmntok_t *key_token = &json.tokens[token_index];
    const jsmntok_t *value_token = &json.tokens[token_index + 1];
    char key[BD_PLUGIN_MAX_ID + 1];
    if (!bd_plugin_json_string(&json, key_token, key, sizeof(key))) {
      snprintf(error, sizeof(error), "invalid setting key");
      goto done;
    }
    int index = setting_index(plugin, key);
    if (index >= 0) {
      const bd_plugin_setting_spec *spec = &plugin->manifest.settings[index];
      if (!bd_plugin_json_is_string(value_token) ||
          !bd_plugin_json_string(&json, value_token, saved[index],
                                 sizeof(saved[index])) ||
          !setting_value_valid(spec, saved[index])) {
        snprintf(error, sizeof(error), "invalid value for setting '%s'",
                 spec->key);
        goto done;
      }
    }
    const jsmntok_t *next = bd_plugin_json_skip(&json, value_token);
    if (next == NULL) {
      snprintf(error, sizeof(error), "truncated settings object");
      goto done;
    }
    token_index = (int)(next - json.tokens);
  }
  for (int i = 0; i < plugin->manifest.setting_count; ++i) {
    snprintf(plugin->settings_values[i], sizeof(plugin->settings_values[i]),
             "%s", saved[i]);
  }
  ok = true;

done:
  if (!ok) {
    bd_plugin_diagnostic(plugin->host, "%s: ignoring settings file: %s",
                         plugin->manifest.id, error);
  }
  bd_plugin_json_destroy(&json);
  free(text);
  return ok;
}

static long settings_fail_after(void) {
  const char *value = getenv("BRICKDESK_PLUGIN_SETTINGS_FAIL_WRITE");
  if (value == NULL || value[0] == '\0') {
    return -1;
  }
  char *end = NULL;
  errno = 0;
  long parsed = strtol(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < 0) {
    return -1;
  }
  return parsed;
}

static bool settings_fail_dir_sync(void) {
  const char *value = getenv("BRICKDESK_PLUGIN_SETTINGS_FAIL_DIRSYNC");
  return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

typedef enum plugin_settings_write_result {
  PLUGIN_SETTINGS_WRITE_FAILED = 0, /* old file and in-memory value remain */
  PLUGIN_SETTINGS_WRITE_OK,         /* file and directory are durable */
  PLUGIN_SETTINGS_WRITE_UNSYNCED    /* new file is visible, sync failed */
} plugin_settings_write_result;

/*
 * Writes through a temporary file in the same directory, fsyncs it, renames it
 * over the old file, then fsyncs the directory. A crash before the rename
 * leaves the old file in place. A failed directory sync after the rename
 * returns UNSYNCED: the new file is already visible, so the caller must commit
 * the in-memory value instead of rolling it back.
 */
static plugin_settings_write_result plugin_settings_write_atomic(
    const char *directory, const char *path, const char *data, size_t length,
    char *error, size_t error_size) {
  char temporary[BD_PLUGIN_MAX_PATH + BD_PLUGIN_MAX_ID + 64];
  int written = snprintf(temporary, sizeof(temporary), "%s/%s.tmp.XXXXXX",
                         directory, BD_PLUGIN_SETTINGS_FILE);
  if (written <= 0 || (size_t)written >= sizeof(temporary)) {
    snprintf(error, error_size, "settings path is too long");
    return PLUGIN_SETTINGS_WRITE_FAILED;
  }
  int fd = mkstemp(temporary);
  if (fd < 0) {
    snprintf(error, error_size, "cannot create settings file: %s",
             strerror(errno));
    return PLUGIN_SETTINGS_WRITE_FAILED;
  }
  (void)fchmod(fd, 0600);

  long fail_after = settings_fail_after();
  size_t offset = 0;
  bool ok = true;
  while (offset < length) {
    size_t chunk = length - offset;
    if (fail_after >= 0) {
      if (offset >= (size_t)fail_after) {
        snprintf(error, error_size, "injected write failure");
        ok = false;
        break;
      }
      size_t remaining = (size_t)fail_after - offset;
      if (remaining < chunk) {
        chunk = remaining;
      }
    }
    ssize_t result = write(fd, data + offset, chunk);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      snprintf(error, error_size, "cannot write settings: %s",
               strerror(errno));
      ok = false;
      break;
    }
    if (result == 0) {
      snprintf(error, error_size, "cannot write settings: short write");
      ok = false;
      break;
    }
    offset += (size_t)result;
  }
  if (ok && fsync(fd) != 0) {
    snprintf(error, error_size, "cannot sync settings: %s", strerror(errno));
    ok = false;
  }
  if (close(fd) != 0 && ok) {
    snprintf(error, error_size, "cannot close settings: %s", strerror(errno));
    ok = false;
  }
  if (!ok) {
    unlink(temporary);
    return PLUGIN_SETTINGS_WRITE_FAILED;
  }
  if (rename(temporary, path) != 0) {
    snprintf(error, error_size, "cannot replace settings file: %s",
             strerror(errno));
    unlink(temporary);
    return PLUGIN_SETTINGS_WRITE_FAILED;
  }
  if (settings_fail_dir_sync()) {
    snprintf(error, error_size, "injected directory sync failure");
    return PLUGIN_SETTINGS_WRITE_UNSYNCED;
  }
  int directory_fd = open(directory, O_RDONLY | O_CLOEXEC);
  if (directory_fd < 0) {
    snprintf(error, error_size,
             "settings file replaced but cannot open its directory: %s",
             strerror(errno));
    return PLUGIN_SETTINGS_WRITE_UNSYNCED;
  }
  if (fsync(directory_fd) != 0) {
    snprintf(error, error_size,
             "settings file replaced but directory sync failed: %s",
             strerror(errno));
    close(directory_fd);
    return PLUGIN_SETTINGS_WRITE_UNSYNCED;
  }
  if (close(directory_fd) != 0) {
    snprintf(error, error_size,
             "settings file replaced but directory close failed: %s",
             strerror(errno));
    return PLUGIN_SETTINGS_WRITE_UNSYNCED;
  }
  return PLUGIN_SETTINGS_WRITE_OK;
}

static plugin_settings_write_result plugin_settings_save(bd_plugin *plugin,
                                                         char *error,
                                                         size_t error_size) {
  if (plugin->manifest.setting_count == 0) {
    return PLUGIN_SETTINGS_WRITE_OK;
  }
  char directory[BD_PLUGIN_MAX_PATH + BD_PLUGIN_MAX_ID + 32];
  char path[BD_PLUGIN_MAX_PATH + BD_PLUGIN_MAX_ID + 64];
  if (!plugin_settings_dir(plugin, directory, sizeof(directory)) ||
      !plugin_settings_path(plugin, path, sizeof(path))) {
    snprintf(error, error_size, "settings path is too long");
    return PLUGIN_SETTINGS_WRITE_FAILED;
  }
  if (!bd_plugin_make_dirs(directory)) {
    snprintf(error, error_size, "cannot create settings directory: %s",
             strerror(errno));
    return PLUGIN_SETTINGS_WRITE_FAILED;
  }
  char data[8192];
  size_t used = 0;
  int written = snprintf(data, sizeof(data),
                         "{\"version\":1,\"settings\":{");
  if (written <= 0 || (size_t)written >= sizeof(data)) {
    snprintf(error, error_size, "settings file is too large");
    return PLUGIN_SETTINGS_WRITE_FAILED;
  }
  used += (size_t)written;
  for (int i = 0; i < plugin->manifest.setting_count; ++i) {
    char escaped_key[sizeof(plugin->manifest.settings[0].key) * 6 + 1];
    char escaped_value[sizeof(plugin->settings_values[0]) * 6 + 1];
    json_escape(escaped_key, sizeof(escaped_key),
                plugin->manifest.settings[i].key);
    json_escape(escaped_value, sizeof(escaped_value),
                plugin->settings_values[i]);
    written = snprintf(data + used, sizeof(data) - used, "%s\"%s\":\"%s\"",
                       i > 0 ? "," : "", escaped_key, escaped_value);
    if (written < 0 || (size_t)written >= sizeof(data) - used) {
      snprintf(error, error_size, "settings file is too large");
      return PLUGIN_SETTINGS_WRITE_FAILED;
    }
    used += (size_t)written;
  }
  written = snprintf(data + used, sizeof(data) - used, "}}");
  if (written < 0 || (size_t)written >= sizeof(data) - used) {
    snprintf(error, error_size, "settings file is too large");
    return PLUGIN_SETTINGS_WRITE_FAILED;
  }
  used += (size_t)written;
  return plugin_settings_write_atomic(directory, path, data, used, error,
                                      error_size);
}

static plugin_settings_write_result plugin_settings_commit(
    bd_plugin *plugin, int index, const char *value, char *error,
    size_t error_size) {
  char previous[BD_PLUGIN_MAX_SETTING_VALUE];
  snprintf(previous, sizeof(previous), "%s", plugin->settings_values[index]);
  snprintf(plugin->settings_values[index], sizeof(plugin->settings_values[index]),
           "%s", value);
  plugin_settings_write_result result =
      plugin_settings_save(plugin, error, error_size);
  if (result == PLUGIN_SETTINGS_WRITE_FAILED) {
    snprintf(plugin->settings_values[index],
             sizeof(plugin->settings_values[index]), "%s", previous);
  }
  return result;
}

static void settings_report_unsynced(bd_plugin *plugin, const char *error) {
  bd_plugin_diagnostic(plugin->host, "%s: settings saved but not durable: %s",
                       plugin->manifest.id, error);
  bd_plugin_log(plugin, "settings saved but not durable: %s", error);
}

static void handle_settings_get(bd_plugin *plugin, const bd_plugin_json *json,
                                const jsmntok_t *params, long id) {
  char key[64] = {0};
  const jsmntok_t *value = NULL;
  if (params != NULL) {
    value = bd_plugin_json_member(json, params, "key");
  }
  if (value == NULL ||
      !bd_plugin_json_string(json, value, key, sizeof(key))) {
    if (id > 0) {
      send_error(plugin, id, -32602, "missing key");
    }
    return;
  }
  int index = setting_index(plugin, key);
  if (index < 0) {
    if (id > 0) {
      send_error(plugin, id, -32602, "unknown setting");
    }
    return;
  }
  const bd_plugin_setting_spec *spec = &plugin->manifest.settings[index];
  char escaped_key[128];
  char escaped_value[BD_PLUGIN_MAX_SETTING_VALUE * 2 + 1];
  json_escape(escaped_key, sizeof(escaped_key), spec->key);
  json_escape(escaped_value, sizeof(escaped_value),
              plugin->settings_values[index]);
  if (id <= 0) {
    return;
  }
  if (spec->type == BD_PLUGIN_SETTING_BOOL ||
      spec->type == BD_PLUGIN_SETTING_INT ||
      spec->type == BD_PLUGIN_SETTING_FLOAT) {
    queue_format(plugin,
                 "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"key\":\"%s\","
                 "\"value\":%s}}",
                 id, escaped_key, plugin->settings_values[index]);
  } else {
    queue_format(plugin,
                 "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"key\":\"%s\","
                 "\"value\":\"%s\"}}",
                 id, escaped_key, escaped_value);
  }
}

static bool send_settings_event(bd_plugin *plugin, const bd_plugin_setting_spec *spec,
                                const char *value) {
  char escaped_key[128];
  char escaped_value[BD_PLUGIN_MAX_SETTING_VALUE * 2 + 1];
  json_escape(escaped_key, sizeof(escaped_key), spec->key);
  json_escape(escaped_value, sizeof(escaped_value), value);
  if (spec->type == BD_PLUGIN_SETTING_BOOL) {
    return queue_format(plugin,
                        "{\"jsonrpc\":\"2.0\",\"method\":\"host.event\","
                        "\"params\":{\"type\":\"settings.changed\",\"key\":\"%s\","
                        "\"value\":%s}}",
                        escaped_key, strcmp(value, "true") == 0 ? "true" : "false");
  }
  if (spec->type == BD_PLUGIN_SETTING_INT ||
      spec->type == BD_PLUGIN_SETTING_FLOAT) {
    return queue_format(plugin,
                        "{\"jsonrpc\":\"2.0\",\"method\":\"host.event\","
                        "\"params\":{\"type\":\"settings.changed\",\"key\":\"%s\","
                        "\"value\":%s}}",
                        escaped_key, value);
  }
  return queue_format(plugin,
                      "{\"jsonrpc\":\"2.0\",\"method\":\"host.event\","
                      "\"params\":{\"type\":\"settings.changed\",\"key\":\"%s\","
                      "\"value\":\"%s\"}}",
                      escaped_key, escaped_value);
}

static void handle_settings_set(bd_plugin *plugin, const bd_plugin_json *json,
                                const jsmntok_t *params, long id) {
  char key[64] = {0};
  char value[BD_PLUGIN_MAX_SETTING_VALUE] = {0};
  const jsmntok_t *key_token = NULL;
  const jsmntok_t *value_token = NULL;
  if (params != NULL) {
    key_token = bd_plugin_json_member(json, params, "key");
    value_token = bd_plugin_json_member(json, params, "value");
  }
  if (key_token == NULL ||
      !bd_plugin_json_string(json, key_token, key, sizeof(key))) {
    if (id > 0) {
      send_error(plugin, id, -32602, "missing key");
    }
    return;
  }
  if (value_token == NULL) {
    if (id > 0) {
      send_error(plugin, id, -32602, "missing value");
    }
    return;
  }
  int index = setting_index(plugin, key);
  if (index < 0) {
    if (id > 0) {
      send_error(plugin, id, -32602, "unknown setting");
    }
    return;
  }
  const bd_plugin_setting_spec *spec = &plugin->manifest.settings[index];
  bool valid = false;
  if (spec->type == BD_PLUGIN_SETTING_BOOL) {
    bool boolean = false;
    if (bd_plugin_json_bool(json, value_token, &boolean)) {
      snprintf(value, sizeof(value), "%s", boolean ? "true" : "false");
      valid = true;
    }
  } else if (spec->type == BD_PLUGIN_SETTING_INT) {
    int integer = 0;
    if (bd_plugin_json_int(json, value_token, &integer)) {
      snprintf(value, sizeof(value), "%d", integer);
      valid = true;
    }
  } else if (spec->type == BD_PLUGIN_SETTING_FLOAT) {
    float number = 0.0f;
    if (bd_plugin_json_float(json, value_token, &number)) {
      snprintf(value, sizeof(value), "%.6g", (double)number);
      valid = true;
    }
  } else if (bd_plugin_json_string(json, value_token, value, sizeof(value))) {
    valid = true;
  }
  if (!valid || !setting_value_valid(spec, value)) {
    if (id > 0) {
      send_error(plugin, id, -32602, "invalid value");
    }
    return;
  }
  char storage_error[192] = {0};
  plugin_settings_write_result write =
      plugin_settings_commit(plugin, index, value, storage_error,
                             sizeof(storage_error));
  if (write == PLUGIN_SETTINGS_WRITE_FAILED) {
    bd_plugin_log(plugin, "cannot save settings: %s", storage_error);
    if (id > 0) {
      send_error(plugin, id, -32603, storage_error);
    }
    return;
  }
  if (write == PLUGIN_SETTINGS_WRITE_UNSYNCED) {
    settings_report_unsynced(plugin, storage_error);
  }
  send_settings_event(plugin, spec, value);
  bd_plugin_mark_dirty(plugin->host);
  if (id > 0 && write == PLUGIN_SETTINGS_WRITE_UNSYNCED) {
    queue_format(plugin,
                 "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":"
                 "{\"durable\":false}}",
                 id);
  } else if (id > 0) {
    queue_format(plugin, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{}}", id);
  }
}

static void handle_message(bd_plugin *plugin, const char *line, size_t length,
                           uint64_t now) {
  if (plugin->state == BD_PLUGIN_HANDSHAKING) {
    bd_plugin_json json;
    char error[192] = {0};
    if (bd_plugin_json_parse(line, length, &json, BD_PLUGIN_JSON_MAX_TOKENS,
                             error, sizeof(error))) {
      const jsmntok_t *root = &json.tokens[0];
      const jsmntok_t *method =
          bd_plugin_json_member(&json, root, "method");
      char method_name[64] = {0};
      if (method != NULL) {
        bd_plugin_json_string(&json, method, method_name, sizeof(method_name));
      }
      long id = 0;
      const jsmntok_t *id_token = bd_plugin_json_member(&json, root, "id");
      if (id_token != NULL) {
        bd_plugin_json_long(&json, id_token, &id);
      }
      if (strcmp(method_name, "plugin.ready") == 0 ||
          (method == NULL && id == 1)) {
        finish_handshake(plugin, now);
      }
      bd_plugin_json_destroy(&json);
    } else {
      ++plugin->protocol_errors;
      bd_plugin_log(plugin, "bad handshake JSON: %s", error);
    }
    return;
  }
  if (plugin->state != BD_PLUGIN_RUNNING) {
    return;
  }

  bd_plugin_json json;
  char error[192] = {0};
  if (!bd_plugin_json_parse(line, length, &json, BD_PLUGIN_JSON_MAX_TOKENS,
                            error, sizeof(error))) {
    ++plugin->protocol_errors;
    bd_plugin_log(plugin, "bad JSON: %s", error);
    return;
  }
  const jsmntok_t *root = &json.tokens[0];
  const jsmntok_t *method = bd_plugin_json_member(&json, root, "method");
  const jsmntok_t *params = bd_plugin_json_member(&json, root, "params");
  long id = 0;
  const jsmntok_t *id_token = bd_plugin_json_member(&json, root, "id");
  if (id_token != NULL) {
    bd_plugin_json_long(&json, id_token, &id);
  }
  char method_name[64] = {0};
  if (method != NULL) {
    bd_plugin_json_string(&json, method, method_name, sizeof(method_name));
  }

  if (strcmp(method_name, "plugin.ui.set") == 0) {
    if (length > BD_PLUGIN_UI_JSON_MAX) {
      ++plugin->protocol_errors;
      bd_plugin_log(plugin, "plugin.ui.set exceeds %d bytes",
                    BD_PLUGIN_UI_JSON_MAX);
      if (id > 0) {
        send_error(plugin, id, -32602, "UI message is too large");
      }
    } else {
      handle_ui_set(plugin, &json, params, id);
    }
  } else if (strcmp(method_name, "plugin.tile.acquire") == 0) {
    handle_tile_acquire(plugin, &json, params, id);
  } else if (strcmp(method_name, "plugin.tile.frame") == 0) {
    handle_tile_frame(plugin, id);
  } else if (strcmp(method_name, "plugin.settings.get") == 0) {
    handle_settings_get(plugin, &json, params, id);
  } else if (strcmp(method_name, "plugin.settings.set") == 0) {
    handle_settings_set(plugin, &json, params, id);
  } else if (strcmp(method_name, "plugin.log") == 0) {
    char level[16] = "info";
    char message[256] = {0};
    const jsmntok_t *level_token = NULL;
    const jsmntok_t *message_token = NULL;
    if (params != NULL) {
      level_token = bd_plugin_json_member(&json, params, "level");
      message_token = bd_plugin_json_member(&json, params, "message");
    }
    if (level_token != NULL) {
      bd_plugin_json_string(&json, level_token, level, sizeof(level));
    }
    if (message_token != NULL) {
      bd_plugin_json_string(&json, message_token, message, sizeof(message));
    }
    bd_plugin_log(plugin, "%s: %s", level, message);
    if (id > 0) {
      queue_format(plugin, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{}}",
                   id);
    }
  } else if (strcmp(method_name, "plugin.toast") == 0) {
    char message[256] = {0};
    if (params != NULL) {
      const jsmntok_t *message_token =
          bd_plugin_json_member(&json, params, "message");
      if (message_token != NULL) {
        bd_plugin_json_string(&json, message_token, message, sizeof(message));
      }
    }
    bd_plugin_log(plugin, "toast: %s", message);
    if (id > 0) {
      queue_format(plugin, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{}}",
                   id);
    }
  } else if (strcmp(method_name, "plugin.ready") == 0) {
    /* duplicate ready notification: ignore */
  } else if (id != 0) {
    send_error(plugin, id, -32601, "unknown method");
  }
  bd_plugin_json_destroy(&json);
}

static void plugin_read(bd_plugin *plugin, uint64_t now) {
  char buffer[4096];
  for (;;) {
    if (plugin->fd < 0) {
      return;
    }
    ssize_t received = recv(plugin->fd, buffer, sizeof(buffer), 0);
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      bd_plugin_log(plugin, "socket read failed: %s", strerror(errno));
      kill_and_reap(plugin, now);
      return;
    }
    if (received == 0) {
      bd_plugin_log(plugin, "socket closed");
      kill_and_reap(plugin, now);
      return;
    }
    plugin->last_rx_ms = now;
    if ((size_t)received > sizeof(plugin->input) - 1 - plugin->input_length) {
      ++plugin->protocol_errors;
      bd_plugin_log(plugin, "line exceeds %d bytes", BD_PLUGIN_LINE_MAX);
      kill_and_reap(plugin, now);
      return;
    }
    memcpy(plugin->input + plugin->input_length, buffer, (size_t)received);
    plugin->input_length += (size_t)received;
    plugin->input[plugin->input_length] = '\0';

    char *newline = memchr(plugin->input, '\n', plugin->input_length);
    while (newline != NULL) {
      size_t line_length = (size_t)(newline - plugin->input);
      if (line_length > 0 && plugin->input[line_length - 1] == '\r') {
        --line_length;
      }
      plugin->input[line_length] = '\0';
      handle_message(plugin, plugin->input, line_length, now);
      if (plugin->fd < 0) {
        return;
      }
      size_t consumed = (size_t)(newline - plugin->input) + 1;
      memmove(plugin->input, plugin->input + consumed,
              plugin->input_length - consumed);
      plugin->input_length -= consumed;
      newline = memchr(plugin->input, '\n', plugin->input_length);
    }
  }
}

static void run_timers(bd_plugin_host *host, uint64_t now) {
  for (int i = 0; i < host->count; ++i) {
    bd_plugin *plugin = &host->plugins[i];
    if ((plugin->state == BD_PLUGIN_RUNNING ||
         plugin->state == BD_PLUGIN_HANDSHAKING) &&
        plugin->fd < 0) {
      bd_plugin_log(plugin, "connection lost");
      kill_and_reap(plugin, now);
      continue;
    }
    if (plugin->state == BD_PLUGIN_BACKOFF && plugin->restart_at_ms != 0 &&
        now >= plugin->restart_at_ms) {
      plugin->restart_at_ms = 0;
      if (!plugin_start(plugin, now)) {
        schedule_after_failure(plugin, now);
      }
      continue;
    }
    if ((plugin->state == BD_PLUGIN_STARTING ||
         plugin->state == BD_PLUGIN_HANDSHAKING) &&
        plugin->handshake_deadline_ms != 0 &&
        now >= plugin->handshake_deadline_ms) {
      bd_plugin_log(plugin, "handshake timed out");
      kill_and_reap(plugin, now);
      continue;
    }
    if (plugin->state == BD_PLUGIN_RUNNING) {
      if (host->limits.ping_interval_ms > 0 &&
          now - plugin->last_ping_ms >=
              (uint64_t)host->limits.ping_interval_ms) {
        plugin->last_ping_ms = now;
        queue_format(plugin,
                     "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"host.ping\"}",
                     ++plugin->next_id);
      }
      if (host->limits.ping_timeout_ms > 0 &&
          now - plugin->last_rx_ms >=
              (uint64_t)host->limits.ping_timeout_ms) {
        bd_plugin_log(plugin, "heartbeat timed out");
        kill_and_reap(plugin, now);
        continue;
      }
      bool idle = !plugin->visible && !plugin->settings_open &&
                  !plugin->manifest.autostart &&
                  (plugin->manifest.capabilities & BD_PLUGIN_CAP_BACKGROUND) ==
                      0;
      if (idle && host->limits.idle_stop_ms > 0 &&
          plugin->idle_deadline_ms != 0 && now >= plugin->idle_deadline_ms) {
        bd_plugin_log(plugin, "idle; stopping");
        queue_format(plugin,
                     "{\"jsonrpc\":\"2.0\",\"method\":\"host.shutdown\"}");
        set_state(plugin, BD_PLUGIN_STOPPING);
        plugin->stop_graceful = true;
        plugin->stop_deadline_ms =
            now + (uint64_t)(host->limits.shutdown_timeout_ms > 0
                                 ? host->limits.shutdown_timeout_ms
                                 : 1);
      }
    }
    if (plugin->state == BD_PLUGIN_STOPPING &&
        plugin->stop_deadline_ms != 0 && now >= plugin->stop_deadline_ms) {
      if (plugin->stop_graceful) {
        plugin->stop_graceful = false;
        plugin->stop_deadline_ms = now + 500;
        if (plugin->pid > 0) {
          kill(plugin->pid, SIGTERM);
        }
      } else {
        kill_and_reap(plugin, now);
      }
    }
  }
}

static void reap_children(bd_plugin_host *host, uint64_t now) {
  for (int i = 0; i < host->count; ++i) {
    bd_plugin *plugin = &host->plugins[i];
    if (plugin->pid <= 0) {
      continue;
    }
    int status = 0;
    pid_t result = waitpid(plugin->pid, &status, WNOHANG);
    if (result == plugin->pid) {
      process_exit(plugin, status, now);
    } else if (result < 0 && errno == ECHILD) {
      plugin->pid = -1;
      process_exit(plugin, 1 << 8, now);
    }
  }
}

int bd_plugin_host_next_timeout(const bd_plugin_host *host) {
  if (host == NULL) {
    return -1;
  }
  uint64_t now = monotonic_ms();
  uint64_t best = 0;
  bool has = false;
  for (int i = 0; i < host->count; ++i) {
    const bd_plugin *plugin = &host->plugins[i];
    uint64_t deadline = 0;
    if ((plugin->state == BD_PLUGIN_RUNNING ||
         plugin->state == BD_PLUGIN_HANDSHAKING) &&
        plugin->fd < 0) {
      return 0;
    }
    if (plugin->output_length > 0) {
      return 5;
    }
    if ((plugin->state == BD_PLUGIN_STARTING ||
         plugin->state == BD_PLUGIN_HANDSHAKING) &&
        plugin->handshake_deadline_ms != 0) {
      deadline = plugin->handshake_deadline_ms;
    } else if (plugin->state == BD_PLUGIN_BACKOFF &&
               plugin->restart_at_ms != 0) {
      deadline = plugin->restart_at_ms;
    } else if (plugin->state == BD_PLUGIN_STOPPING &&
               plugin->stop_deadline_ms != 0) {
      deadline = plugin->stop_deadline_ms;
    } else if (plugin->state == BD_PLUGIN_RUNNING) {
      if (host->limits.ping_interval_ms > 0) {
        deadline = plugin->last_ping_ms +
                   (uint64_t)host->limits.ping_interval_ms;
      }
      if (host->limits.ping_timeout_ms > 0) {
        uint64_t timeout =
            plugin->last_rx_ms + (uint64_t)host->limits.ping_timeout_ms;
        if (deadline == 0 || timeout < deadline) {
          deadline = timeout;
        }
      }
      if (!plugin->visible && !plugin->settings_open &&
          !plugin->manifest.autostart &&
          (plugin->manifest.capabilities & BD_PLUGIN_CAP_BACKGROUND) == 0 &&
          host->limits.idle_stop_ms > 0 && plugin->idle_deadline_ms != 0) {
        if (deadline == 0 || plugin->idle_deadline_ms < deadline) {
          deadline = plugin->idle_deadline_ms;
        }
      }
      if (plugin->visible &&
          (plugin->manifest.capabilities & BD_PLUGIN_CAP_TILE) != 0 &&
          plugin->manifest.tile.refresh_hz > 0) {
        uint64_t frame =
            now + (uint64_t)(1000 / plugin->manifest.tile.refresh_hz);
        if (deadline == 0 || frame < deadline) {
          deadline = frame;
        }
      }
    }
    if (deadline == 0) {
      continue;
    }
    if (deadline <= now) {
      return 0;
    }
    if (!has || deadline < best) {
      best = deadline;
      has = true;
    }
  }
  if (!has) {
    return -1;
  }
  uint64_t remaining = best - now;
  if (remaining > (uint64_t)INT_MAX) {
    return INT_MAX;
  }
  return (int)remaining;
}

int bd_plugin_host_fds(const bd_plugin_host *host, int *out, int max) {
  if (host == NULL || out == NULL || max <= 0) {
    return 0;
  }
  int count = 0;
  for (int i = 0; i < host->count && count < max; ++i) {
    const bd_plugin *plugin = &host->plugins[i];
    if (plugin->listen_fd >= 0) {
      out[count++] = plugin->listen_fd;
    }
    if (plugin->fd >= 0 && count < max) {
      out[count++] = plugin->fd;
    }
  }
  return count;
}

int bd_plugin_host_poll(bd_plugin_host *host, int timeout_ms) {
  if (host == NULL) {
    return -1;
  }
  uint64_t now = monotonic_ms();
  struct pollfd fds[BD_PLUGIN_MAX_PLUGINS * 2];
  int owners[BD_PLUGIN_MAX_PLUGINS * 2];
  int kinds[BD_PLUGIN_MAX_PLUGINS * 2];
  int count = 0;
  for (int i = 0; i < host->count; ++i) {
    bd_plugin *plugin = &host->plugins[i];
    if (plugin->listen_fd >= 0) {
      fds[count].fd = plugin->listen_fd;
      fds[count].events = POLLIN;
      fds[count].revents = 0;
      owners[count] = i;
      kinds[count] = 0;
      ++count;
    }
    if (plugin->fd >= 0) {
      fds[count].fd = plugin->fd;
      fds[count].events = POLLIN;
      if (plugin->output_length > 0) {
        fds[count].events |= POLLOUT;
      }
      fds[count].revents = 0;
      owners[count] = i;
      kinds[count] = 1;
      ++count;
    }
  }
  int wait = timeout_ms;
  if (wait < 0) {
    wait = bd_plugin_host_next_timeout(host);
  }
  if (count == 0 && wait < 0) {
    return 0;
  }
  int ready = poll(fds, (nfds_t)count, wait);
  if (ready < 0) {
    if (errno == EINTR) {
      return 0;
    }
    return -1;
  }
  int handled = 0;
  now = monotonic_ms();
  for (int i = 0; i < count; ++i) {
    if (fds[i].revents == 0) {
      continue;
    }
    bd_plugin *plugin = &host->plugins[owners[i]];
    if (kinds[i] == 0) {
      if ((fds[i].revents & POLLIN) != 0) {
        accept_connection(plugin, now);
        ++handled;
      }
      continue;
    }
    if ((fds[i].revents & POLLOUT) != 0) {
      if (!flush_output(plugin)) {
        kill_and_reap(plugin, now);
        continue;
      }
      ++handled;
    }
    if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      plugin_read(plugin, now);
      ++handled;
    }
  }
  reap_children(host, now);
  run_timers(host, now);
  for (int i = 0; i < host->count; ++i) {
    if (host->plugins[i].output_length > 0 && host->plugins[i].fd >= 0) {
      if (!flush_output(&host->plugins[i])) {
        kill_and_reap(&host->plugins[i], now);
      }
    }
  }
  return handled;
}

bool bd_plugin_host_take_dirty(bd_plugin_host *host) {
  if (host == NULL || !host->dirty) {
    return false;
  }
  host->dirty = false;
  return true;
}

const char *bd_plugin_host_runtime_dir(const bd_plugin_host *host) {
  return host != NULL ? host->runtime_dir : "";
}

bd_plugin_host *bd_plugin_host_create(const bd_plugin_host_config *config) {
  bd_plugin_host *host = (bd_plugin_host *)calloc(1, sizeof(*host));
  if (host == NULL) {
    return NULL;
  }
  const char *plugin_dir =
      config != NULL && config->plugin_dir != NULL ? config->plugin_dir
                                                   : getenv("BRICKDESK_PLUGIN_DIR");
  if (plugin_dir == NULL || plugin_dir[0] == '\0') {
    plugin_dir = BRICKDESK_PLUGIN_DIR;
  }
  const char *runtime_dir = config != NULL && config->runtime_dir != NULL
                                ? config->runtime_dir
                                : getenv("BRICKDESK_RUNTIME_DIR");
  if (runtime_dir == NULL || runtime_dir[0] == '\0') {
    runtime_dir = "/run/brickdesk";
    if (!bd_plugin_make_dirs(runtime_dir)) {
      runtime_dir = "/tmp/brickdesk";
    }
  }
  if (strlen(plugin_dir) >= sizeof(host->plugin_dir) ||
      strlen(runtime_dir) >= sizeof(host->runtime_dir)) {
    free(host);
    return NULL;
  }
  snprintf(host->plugin_dir, sizeof(host->plugin_dir), "%s", plugin_dir);
  snprintf(host->runtime_dir, sizeof(host->runtime_dir), "%s", runtime_dir);
  if (!bd_plugin_make_dirs(host->runtime_dir)) {
    free(host);
    return NULL;
  }
  const char *state_dir = config != NULL ? config->state_dir : NULL;
  bool explicit_state = state_dir != NULL && state_dir[0] != '\0';
  if (!explicit_state) {
    state_dir = getenv("BRICKDESK_STATE_DIR");
    explicit_state = state_dir != NULL && state_dir[0] != '\0';
  }
  if (!explicit_state) {
    state_dir = BRICKDESK_STATE_DIR;
  }
  bool state_ready = false;
  if (state_dir != NULL && state_dir[0] != '\0' &&
      strlen(state_dir) < sizeof(host->state_dir)) {
    snprintf(host->state_dir, sizeof(host->state_dir), "%s", state_dir);
    state_ready = bd_plugin_make_dirs(host->state_dir);
  }
  if (!state_ready) {
    if (explicit_state) {
      free(host);
      return NULL;
    }
    if (snprintf(host->state_dir, sizeof(host->state_dir), "%s/state",
                 host->runtime_dir) >= (int)sizeof(host->state_dir) ||
        !bd_plugin_make_dirs(host->state_dir)) {
      free(host);
      return NULL;
    }
  }
  const char *data_dir = config != NULL && config->data_dir != NULL
                             ? config->data_dir
                             : NULL;
  if (data_dir != NULL && data_dir[0] != '\0') {
    if (strlen(data_dir) >= sizeof(host->data_dir)) {
      free(host);
      return NULL;
    }
    snprintf(host->data_dir, sizeof(host->data_dir), "%s", data_dir);
  } else {
    if (snprintf(host->data_dir, sizeof(host->data_dir), "%s%s",
                 host->runtime_dir, BD_PLUGIN_DATA_SUFFIX) >=
        (int)sizeof(host->data_dir)) {
      free(host);
      return NULL;
    }
  }
  if (!bd_plugin_make_dirs(host->data_dir)) {
    free(host);
    return NULL;
  }
  host->limits =
      config != NULL && config->limits != NULL ? *config->limits
                                               : bd_plugin_default_limits();
  host->display_width = config != NULL && config->display_width > 0
                            ? config->display_width
                            : 1024;
  host->display_height = config != NULL && config->display_height > 0
                             ? config->display_height
                             : 768;
  host->dark = config == NULL ? true : config->dark;
  host->accent = config != NULL ? config->accent : 0xe06c4a;
  host->verbose = config != NULL && config->verbose;
  const char *locale =
      config != NULL && config->locale != NULL ? config->locale : "zh-CN";
  snprintf(host->locale, sizeof(host->locale), "%s", locale);
  host->plugins =
      (bd_plugin *)calloc(BD_PLUGIN_MAX_PLUGINS, sizeof(bd_plugin));
  if (host->plugins == NULL) {
    free(host);
    return NULL;
  }
  return host;
}

static void free_plugin(bd_plugin *plugin) {
  close_connection(plugin);
  close_listener(plugin);
  release_surface(plugin);
  if (plugin->pid > 0) {
    kill(plugin->pid, SIGKILL);
    int status = 0;
    while (waitpid(plugin->pid, &status, 0) < 0 && errno == EINTR) {
      /* retry */
    }
    plugin->pid = -1;
  }
}

void bd_plugin_host_stop_all(bd_plugin_host *host) {
  if (host == NULL) {
    return;
  }
  uint64_t now = monotonic_ms();
  host->shutting_down = true;
  for (int i = 0; i < host->count; ++i) {
    bd_plugin *plugin = &host->plugins[i];
    plugin->visible = false;
    plugin->settings_open = false;
    if (plugin->state == BD_PLUGIN_BACKOFF) {
      plugin->restart_at_ms = 0;
      set_state(plugin, BD_PLUGIN_STOPPED);
    }
    if (plugin->state == BD_PLUGIN_RUNNING ||
        plugin->state == BD_PLUGIN_HANDSHAKING ||
        plugin->state == BD_PLUGIN_STARTING) {
      if (plugin->fd >= 0) {
        queue_format(plugin,
                     "{\"jsonrpc\":\"2.0\",\"method\":\"host.shutdown\"}");
      }
      set_state(plugin, BD_PLUGIN_STOPPING);
      plugin->stop_graceful = true;
      plugin->stop_deadline_ms =
          now + (uint64_t)(host->limits.shutdown_timeout_ms > 0
                               ? host->limits.shutdown_timeout_ms
                               : 1);
    }
  }
  uint64_t deadline =
      now + (uint64_t)(host->limits.shutdown_timeout_ms > 0
                           ? host->limits.shutdown_timeout_ms
                           : 1);
  while (monotonic_ms() < deadline) {
    bool busy = false;
    for (int i = 0; i < host->count; ++i) {
      if (host->plugins[i].state == BD_PLUGIN_STOPPING) {
        busy = true;
        break;
      }
    }
    if (!busy) {
      break;
    }
    bd_plugin_host_poll(host, 10);
  }
  for (int i = 0; i < host->count; ++i) {
    free_plugin(&host->plugins[i]);
  }
  host->shutting_down = false;
}

void bd_plugin_host_destroy(bd_plugin_host *host) {
  if (host == NULL) {
    return;
  }
  bd_plugin_host_stop_all(host);
  free(host->plugins);
  host->plugins = NULL;
  host->count = 0;
  free(host);
}

int bd_plugin_host_scan(bd_plugin_host *host, char *error, size_t error_size) {
  if (host == NULL) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "missing host");
    }
    return -1;
  }
  bd_plugin_host_stop_all(host);
  host->count = 0;
  host->diagnostic_count = 0;
  DIR *directory = opendir(host->plugin_dir);
  if (directory == NULL) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "cannot open plugin directory '%s': %s",
               host->plugin_dir, strerror(errno));
    }
    return -1;
  }
  struct dirent *entry = NULL;
  while ((entry = readdir(directory)) != NULL) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    if (host->count >= BD_PLUGIN_MAX_PLUGINS) {
      break;
    }
    char dir[BD_PLUGIN_MAX_PATH];
    if (!join_path(dir, sizeof(dir), host->plugin_dir, entry->d_name)) {
      continue;
    }
    struct stat info;
    if (lstat(dir, &info) != 0 || !S_ISDIR(info.st_mode)) {
      continue;
    }
    bd_plugin plugin;
    memset(&plugin, 0, sizeof(plugin));
    plugin.host = host;
    plugin.index = host->count;
    plugin.state = BD_PLUGIN_DISCOVERED;
    plugin.pid = -1;
    plugin.listen_fd = -1;
    plugin.fd = -1;
    plugin.shm_fd = -1;
    plugin.render_ui_generation = -1;
    char manifest_error[256] = {0};
    if (!bd_plugin_manifest_load(dir, &plugin.manifest, manifest_error,
                                 sizeof(manifest_error))) {
      bd_plugin_diagnostic(host, "%s: %s", entry->d_name, manifest_error);
      continue;
    }
    bool duplicate = false;
    for (int i = 0; i < host->count; ++i) {
      if (strcmp(host->plugins[i].manifest.id, plugin.manifest.id) == 0) {
        bd_plugin_diagnostic(host, "%s: duplicate plugin id '%s'",
                             entry->d_name, plugin.manifest.id);
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    for (int i = 0; i < plugin.manifest.setting_count; ++i) {
      snprintf(plugin.settings_values[i], sizeof(plugin.settings_values[i]),
               "%s", plugin.manifest.settings[i].default_value);
    }
    (void)plugin_settings_load(&plugin);
    host->plugins[host->count++] = plugin;
  }
  closedir(directory);
  for (int i = 0; i < host->count; ++i) {
    for (int j = i + 1; j < host->count; ++j) {
      if (strcmp(host->plugins[i].manifest.id,
                 host->plugins[j].manifest.id) > 0) {
        bd_plugin swap = host->plugins[i];
        host->plugins[i] = host->plugins[j];
        host->plugins[j] = swap;
      }
    }
  }
  for (int i = 0; i < host->count; ++i) {
    host->plugins[i].index = i;
    host->plugins[i].host = host;
  }
  return host->count;
}

size_t bd_plugin_host_count(const bd_plugin_host *host) {
  return host != NULL ? (size_t)host->count : 0;
}

bd_plugin *bd_plugin_host_at(bd_plugin_host *host, size_t index) {
  if (host == NULL || index >= (size_t)host->count) {
    return NULL;
  }
  return &host->plugins[index];
}

bd_plugin *bd_plugin_host_find(bd_plugin_host *host, const char *id) {
  if (host == NULL || id == NULL) {
    return NULL;
  }
  for (int i = 0; i < host->count; ++i) {
    if (strcmp(host->plugins[i].manifest.id, id) == 0) {
      return &host->plugins[i];
    }
  }
  return NULL;
}

const bd_plugin_manifest *bd_plugin_host_manifest(const bd_plugin_host *host,
                                                   size_t index) {
  if (host == NULL || index >= (size_t)host->count) {
    return NULL;
  }
  return &host->plugins[index].manifest;
}

size_t bd_plugin_host_diagnostic_count(const bd_plugin_host *host) {
  return host != NULL ? (size_t)host->diagnostic_count : 0;
}

bool bd_plugin_host_diagnostic(const bd_plugin_host *host, size_t index,
                               char *out, size_t out_size) {
  if (host == NULL || index >= (size_t)host->diagnostic_count || out == NULL ||
      out_size == 0) {
    return false;
  }
  snprintf(out, out_size, "%s", host->diagnostics[index]);
  return true;
}

const bd_plugin_manifest *bd_plugin_manifest_of(const bd_plugin *plugin) {
  return plugin != NULL ? &plugin->manifest : NULL;
}

const char *bd_plugin_id(const bd_plugin *plugin) {
  return plugin != NULL ? plugin->manifest.id : "";
}

bd_plugin_state bd_plugin_state_of(const bd_plugin *plugin) {
  return plugin != NULL ? plugin->state : BD_PLUGIN_DISCOVERED;
}

bd_plugin_stats bd_plugin_get_stats(const bd_plugin *plugin) {
  bd_plugin_stats stats;
  memset(&stats, 0, sizeof(stats));
  if (plugin == NULL) {
    return stats;
  }
  stats.pid = plugin->pid;
  stats.state = plugin->state;
  stats.starts = plugin->starts;
  stats.restarts = plugin->restarts;
  stats.crashes = plugin->crashes;
  stats.protocol_errors = plugin->protocol_errors;
  stats.handshake_timeouts = plugin->handshake_timeouts;
  stats.last_exit_status = plugin->last_exit_status;
  stats.ui_generation = plugin->ui_generation;
  stats.frame_seq = plugin->frame_header != NULL
                        ? __atomic_load_n(&plugin->frame_header->seq,
                                          __ATOMIC_ACQUIRE)
                        : 0;
  if (plugin->pid > 0) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/statm", plugin->pid);
    FILE *file = fopen(path, "r");
    if (file != NULL) {
      long total = 0;
      long resident = 0;
      if (fscanf(file, "%ld %ld", &total, &resident) == 2 && resident > 0) {
        stats.rss_kb = (int)(resident * (long)(sysconf(_SC_PAGESIZE) / 1024));
      }
      fclose(file);
    }
  }
  return stats;
}

bool bd_plugin_activate(bd_plugin *plugin) {
  if (plugin == NULL) {
    return false;
  }
  if (plugin->state == BD_PLUGIN_RUNNING ||
      plugin->state == BD_PLUGIN_STARTING ||
      plugin->state == BD_PLUGIN_HANDSHAKING) {
    return true;
  }
  if (plugin->state == BD_PLUGIN_QUARANTINED ||
      plugin->state == BD_PLUGIN_STOPPING) {
    return false;
  }
  uint64_t now = monotonic_ms();
  if (plugin->state == BD_PLUGIN_BACKOFF && plugin->restart_at_ms > now) {
    return false;
  }
  plugin->restart_at_ms = 0;
  return plugin_start(plugin, now);
}

bool bd_plugin_set_tile_visible(bd_plugin *plugin, bool visible) {
  if (plugin == NULL || plugin->visible == visible) {
    return true;
  }
  plugin->visible = visible;
  uint64_t now = monotonic_ms();
  if (visible) {
    plugin->idle_deadline_ms = 0;
    plugin->visible_since_ms = now;
    if (!bd_plugin_activate(plugin)) {
      if (plugin->state == BD_PLUGIN_QUARANTINED) {
        bd_plugin_mark_dirty(plugin->host);
      }
      return false;
    }
  } else if (plugin->host->limits.idle_stop_ms > 0) {
    plugin->idle_deadline_ms =
        now + (uint64_t)plugin->host->limits.idle_stop_ms;
  }
  if (plugin->state == BD_PLUGIN_RUNNING && plugin->fd >= 0) {
    queue_format(plugin,
                 "{\"jsonrpc\":\"2.0\",\"method\":\"host.event\",\"params\":"
                 "{\"type\":\"tile.visible\",\"visible\":%s}}",
                 visible ? "true" : "false");
  }
  return true;
}

bool bd_plugin_set_settings_open(bd_plugin *plugin, bool open) {
  if (plugin == NULL || plugin->settings_open == open) {
    return true;
  }
  plugin->settings_open = open;
  if (open) {
    plugin->idle_deadline_ms = 0;
    if (!bd_plugin_activate(plugin)) {
      return false;
    }
  } else if (plugin->host->limits.idle_stop_ms > 0) {
    plugin->idle_deadline_ms =
        monotonic_ms() + (uint64_t)plugin->host->limits.idle_stop_ms;
  }
  if (plugin->state == BD_PLUGIN_RUNNING && plugin->fd >= 0) {
    queue_format(plugin,
                 "{\"jsonrpc\":\"2.0\",\"method\":\"host.event\",\"params\":"
                 "{\"type\":\"settings.visible\",\"visible\":%s}}",
                 open ? "true" : "false");
  }
  return true;
}

const bd_plugin_ui *bd_plugin_ui_tile(const bd_plugin *plugin) {
  return plugin != NULL && plugin->has_tile_ui ? &plugin->ui_tile : NULL;
}

const bd_plugin_ui *bd_plugin_ui_settings(const bd_plugin *plugin) {
  return plugin != NULL && plugin->has_settings_ui ? &plugin->ui_settings : NULL;
}

int bd_plugin_ui_generation(const bd_plugin *plugin) {
  return plugin != NULL ? plugin->ui_generation : 0;
}

const char *bd_plugin_get_setting(const bd_plugin *plugin, const char *key) {
  if (plugin == NULL || key == NULL) {
    return NULL;
  }
  int index = setting_index(plugin, key);
  return index >= 0 ? plugin->settings_values[index] : NULL;
}

bool bd_plugin_set_setting(bd_plugin *plugin, const char *key,
                           const char *value) {
  if (plugin == NULL || key == NULL || value == NULL) {
    return false;
  }
  int index = setting_index(plugin, key);
  if (index < 0) {
    return false;
  }
  const bd_plugin_setting_spec *spec = &plugin->manifest.settings[index];
  if (!setting_value_valid(spec, value)) {
    return false;
  }
  char storage_error[192] = {0};
  plugin_settings_write_result write =
      plugin_settings_commit(plugin, index, value, storage_error,
                             sizeof(storage_error));
  if (write == PLUGIN_SETTINGS_WRITE_FAILED) {
    bd_plugin_log(plugin, "cannot save settings: %s", storage_error);
    return false;
  }
  if (write == PLUGIN_SETTINGS_WRITE_UNSYNCED) {
    settings_report_unsynced(plugin, storage_error);
  }
  if (plugin->state == BD_PLUGIN_RUNNING && plugin->fd >= 0) {
    send_settings_event(plugin, spec, value);
  }
  bd_plugin_mark_dirty(plugin->host);
  return true;
}

bool bd_plugin_send_event(bd_plugin *plugin, const char *type,
                          const char *extra_json) {
  if (plugin == NULL || type == NULL || plugin->state != BD_PLUGIN_RUNNING ||
      plugin->fd < 0) {
    return false;
  }
  char escaped[128];
  json_escape(escaped, sizeof(escaped), type);
  if (extra_json == NULL || extra_json[0] == '\0') {
    return queue_format(plugin,
                        "{\"jsonrpc\":\"2.0\",\"method\":\"host.event\","
                        "\"params\":{\"type\":\"%s\"}}",
                        escaped);
  }
  return queue_format(plugin,
                      "{\"jsonrpc\":\"2.0\",\"method\":\"host.event\","
                      "\"params\":{\"type\":\"%s\",%s}}",
                      escaped, extra_json);
}

bool bd_plugin_activate_node(bd_plugin *plugin, const char *node_id,
                             const char *event) {
  if (plugin == NULL || node_id == NULL || event == NULL) {
    return false;
  }
  char escaped_id[128];
  char escaped_event[64];
  json_escape(escaped_id, sizeof(escaped_id), node_id);
  json_escape(escaped_event, sizeof(escaped_event), event);
  char extra[256];
  int written = snprintf(extra, sizeof(extra),
                         "\"node\":\"%s\",\"event\":\"%s\"", escaped_id,
                         escaped_event);
  if (written < 0 || (size_t)written >= sizeof(extra)) {
    return false;
  }
  return bd_plugin_send_event(plugin, "ui.activate", extra);
}

const char *bd_plugin_tile_surface_path(const bd_plugin *plugin) {
  return plugin != NULL ? plugin->shm_path : "";
}

uint32_t bd_plugin_tile_seq(const bd_plugin *plugin) {
  if (plugin == NULL || !header_valid(plugin)) {
    return 0;
  }
  return __atomic_load_n(&plugin->frame_header->seq, __ATOMIC_ACQUIRE);
}

bool bd_plugin_tile_consume(bd_plugin *plugin, bd_plugin_frame *out) {
  if (plugin == NULL || out == NULL || !header_valid(plugin)) {
    return false;
  }
  uint32_t seq =
      __atomic_load_n(&plugin->frame_header->seq, __ATOMIC_ACQUIRE);
  if (seq == 0 || seq == plugin->last_frame_seq) {
    return false;
  }
  uint64_t now = monotonic_ms();
  int fps = plugin->manifest.tile.refresh_hz;
  if (fps <= 0 || fps > BD_PLUGIN_TILE_MAX_FPS) {
    fps = BD_PLUGIN_TILE_MAX_FPS;
  }
  uint64_t interval = (uint64_t)(1000 / fps);
  if (plugin->frame_clock_started &&
      now - plugin->last_frame_read_ms < interval) {
    return false;
  }
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  plugin->last_frame_seq = seq;
  plugin->last_frame_read_ms = now;
  plugin->frame_clock_started = true;
  out->width = (int)plugin->frame_header->width;
  out->height = (int)plugin->frame_header->height;
  out->stride = (int)plugin->frame_header->stride;
  out->seq = seq;
  out->rgba = (const uint8_t *)(plugin->frame_header + 1);
  return true;
}

void bd_plugin_reset_crash_limit(bd_plugin *plugin) {
  if (plugin == NULL) {
    return;
  }
  plugin->crash_count = 0;
  plugin->crash_head = 0;
  plugin->restart_attempt = 0;
  if (plugin->state == BD_PLUGIN_QUARANTINED) {
    set_state(plugin, BD_PLUGIN_DISCOVERED);
  }
}
