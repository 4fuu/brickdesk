#define _POSIX_C_SOURCE 200809L

#include "brickdesk/shell.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "brickdesk/sysinfo.h"
#include "brickdesk/theme.h"

#define BD_SHELL_LINE_MAX 4096
#define BD_SHELL_FD_MAX 64
#define BD_SHELL_PENDING_MAX 16
#define BD_SHELL_PATH_MAX 1024
#define BD_SHELL_STATUS_MS 5000
#define BD_SHELL_MEMORY_MS 1000
#define BD_SHELL_MEMORY_RETRY_MS 5000

typedef enum shell_pending_kind {
  SHELL_PENDING_STATE = 0,
  SHELL_PENDING_EXIT,
  SHELL_PENDING_MENU_ACTION,
  SHELL_PENDING_ATTENTION
} shell_pending_kind;

typedef struct shell_pending {
  shell_pending_kind kind;
  bd_session_state state;
  char app_id[BD_APP_ID_MAX];
  char method[16];
} shell_pending;

typedef struct shell_app_runtime {
  bool running;
  bool background;
  int memory_mb;           /* process-tree RSS; -1 until sampled / unknown */
  uint64_t memory_sampled_ms;
  char frame_path[BD_APP_PATH_MAX];
} shell_app_runtime;

struct bd_shell {
  bd_platform *platform;
  bd_gfx *gfx;
  const bd_config *settings;
  bd_plugin_host *plugins;
  bool verbose;

  bd_app_list *apps;
  shell_app_runtime *runtime;
  size_t runtime_count;
  bd_home *home;
  bd_home_style home_style;

  bd_session *session;
  int session_next_ms;

  bd_hotkey_table hotkeys;
  bd_input_state input;
  bd_input_state test_input;
  const bd_hotkey_table *hotkey_table;

  int inputd_fd;
  bool inputd_connected;
  char input_socket[sizeof(((struct sockaddr_un *)0)->sun_path)];
  char input_line[BD_SHELL_LINE_MAX];
  size_t input_length;

  char menu_shim[BD_SHELL_PATH_MAX];
  bool menu_shim_checked;

  shell_pending pending[BD_SHELL_PENDING_MAX];
  int pending_count;
  bool rescan;

  char dialog_app[BD_APP_ID_MAX];    /* end-dialog target */
  char pending_launch[BD_APP_ID_MAX];/* launch waiting on an eviction */
  char evict_app[BD_APP_ID_MAX];     /* LRU app to end first */
  char return_app[BD_APP_ID_MAX];    /* return deferred by a launch */
  bool evict_failed;                 /* terminate failed; wait for user retry */

  char clock[16];
  int status_battery;
  bool status_wifi_known;
  bool status_wifi;
  uint64_t status_sampled_ms;

  bool dirty;
  bool visible;
  bool quit;
  uint64_t presents;

  /* Scenario harness clock: home transitions advance only on test_advance. */
  bool test_clock;
  uint64_t test_now_ms;
};

static uint64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* Home animations use the scenario clock when the harness drives one. */
static uint64_t shell_now(const bd_shell *shell) {
  return shell != NULL && shell->test_clock ? shell->test_now_ms
                                            : monotonic_ms();
}

static void copy_text(char *dst, size_t size, const char *src) {
  if (size == 0) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  size_t i = 0;
  while (src[i] != '\0' && i + 1 < size) {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
}

static int merge_timeout(int best, int value) {
  if (value < 0) {
    return best;
  }
  if (best < 0 || value < best) {
    return value;
  }
  return best;
}

/* --------------------------------------------------------------- JSON bits */

static bool json_token(const char *json, const char *key, const char **value) {
  size_t key_length = strlen(key);
  for (const char *p = json; (p = strchr(p, '"')) != NULL; ++p) {
    const char *start = p + 1;
    const char *end = start;
    while (*end != '\0') {
      if (*end == '\\' && end[1] != '\0') {
        end += 2;
        continue;
      }
      if (*end == '"') {
        break;
      }
      ++end;
    }
    if (*end != '"') {
      return false;
    }
    if ((size_t)(end - start) == key_length &&
        memcmp(start, key, key_length) == 0) {
      const char *colon = end + 1;
      while (*colon == ' ' || *colon == '\t') {
        ++colon;
      }
      if (*colon == ':') {
        ++colon;
        while (*colon == ' ' || *colon == '\t') {
          ++colon;
        }
        *value = colon;
        return true;
      }
    }
    p = end;
  }
  return false;
}

static bool json_string(const char *json, const char *key, char *out,
                        size_t out_size) {
  const char *value = NULL;
  if (!json_token(json, key, &value) || *value != '"' || out_size == 0) {
    return false;
  }
  ++value;
  size_t used = 0;
  while (*value != '\0' && *value != '"' && used + 1 < out_size) {
    if (*value == '\\' && value[1] != '\0') {
      ++value;
      switch (*value) {
        case 'n':
          out[used++] = '\n';
          break;
        case 't':
          out[used++] = '\t';
          break;
        case 'r':
          out[used++] = '\r';
          break;
        default:
          out[used++] = *value;
          break;
      }
      ++value;
      continue;
    }
    out[used++] = *value++;
  }
  out[used] = '\0';
  return *value == '"';
}

static bool json_bool(const char *json, const char *key, bool *out) {
  const char *value = NULL;
  if (!json_token(json, key, &value)) {
    return false;
  }
  if (strncmp(value, "true", 4) == 0) {
    *out = true;
    return true;
  }
  if (strncmp(value, "false", 5) == 0) {
    *out = false;
    return true;
  }
  return false;
}

/* ----------------------------------------------------------- input source */

static void shell_dispatch_action(bd_shell *shell, const char *action,
                                  bool pressed);
static void shell_dispatch_key(bd_shell *shell, bd_key key, bool pressed);
static bool shell_launch_app(bd_shell *shell, const char *app_id);
static void shell_refresh_running(bd_shell *shell);
static void shell_service(bd_shell *shell);

static void local_emit(void *user, const bd_input_output *event) {
  bd_shell *shell = (bd_shell *)user;
  if (event->kind == BD_INPUT_OUTPUT_KEY) {
    shell_dispatch_key(shell, event->key, event->pressed);
    return;
  }
  if (event->kind == BD_INPUT_OUTPUT_ACTION && shell->hotkey_table != NULL &&
      event->binding >= 0 &&
      (size_t)event->binding < shell->hotkey_table->count) {
    shell_dispatch_action(shell,
                          shell->hotkey_table->items[event->binding].action,
                          event->pressed);
  }
}

static const char *preferred_input_socket(void) {
  const char *env = getenv("BRICKDESK_INPUT_SOCKET");
  if (env != NULL && env[0] != '\0') {
    return env;
  }
  if (access("/run/brickdesk/inputd.sock", F_OK) == 0) {
    return "/run/brickdesk/inputd.sock";
  }
  if (access("/tmp/brickdesk-inputd.sock", F_OK) == 0) {
    return "/tmp/brickdesk-inputd.sock";
  }
  return "/run/brickdesk/inputd.sock";
}

static void inputd_disconnect(bd_shell *shell, const char *reason) {
  if (shell->inputd_fd >= 0) {
    close(shell->inputd_fd);
    shell->inputd_fd = -1;
  }
  if (shell->inputd_connected) {
    shell->inputd_connected = false;
    if (shell->verbose) {
      fprintf(stderr, "brickdesk: inputd disconnected (%s); using SDL keys\n",
              reason != NULL ? reason : "eof");
    }
    bd_home_set_hint_visible(shell->home, false);
    shell->dirty = true;
  }
}

static bool send_all(int fd, const char *text) {
  size_t length = strlen(text);
  size_t sent = 0;
  while (sent < length) {
    ssize_t amount = send(fd, text + sent, length - sent, MSG_NOSIGNAL);
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

static bool inputd_connect(bd_shell *shell) {
  if (shell->input_socket[0] == '\0') {
    return false;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  int fd_flags = fcntl(fd, F_GETFD, 0);
  if (fd_flags >= 0) {
    fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
  }
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  snprintf(address.sun_path, sizeof(address.sun_path), "%s", shell->input_socket);
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    close(fd);
    return false;
  }
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  static const char subscribe[] =
      "{\"cmd\":\"subscribe\",\"actions\":true,\"keys\":true}\n";
  if (!send_all(fd, subscribe)) {
    close(fd);
    return false;
  }
  shell->inputd_fd = fd;
  shell->inputd_connected = true;
  shell->input_length = 0;
  return true;
}

static void inputd_handle_line(bd_shell *shell, const char *line) {
  char type[32];
  if (!json_string(line, "type", type, sizeof(type))) {
    return;
  }
  if (strcmp(type, "action") == 0) {
    char action[BD_HOTKEY_ACTION_MAX];
    char phase[16];
    if (!json_string(line, "action", action, sizeof(action))) {
      return;
    }
    bool pressed = true;
    if (json_string(line, "phase", phase, sizeof(phase))) {
      pressed = strcmp(phase, "up") != 0;
    }
    shell_dispatch_action(shell, action, pressed);
    return;
  }
  if (strcmp(type, "key") == 0) {
    char key_name[32];
    bool pressed = false;
    if (!json_string(line, "key", key_name, sizeof(key_name)) ||
        !json_bool(line, "pressed", &pressed)) {
      return;
    }
    bd_key key = bd_key_from_name(key_name);
    if (key != BD_KEY_UNKNOWN) {
      shell_dispatch_key(shell, key, pressed);
    }
    return;
  }
  if (strcmp(type, "release_all") == 0) {
    bd_home_set_hint_visible(shell->home, false);
    shell->dirty = true;
  }
}

static void inputd_read(bd_shell *shell) {
  for (;;) {
    char buffer[512];
    ssize_t amount = recv(shell->inputd_fd, buffer, sizeof(buffer), 0);
    if (amount < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      inputd_disconnect(shell, strerror(errno));
      return;
    }
    if (amount == 0) {
      inputd_disconnect(shell, "closed");
      return;
    }
    if (shell->input_length + (size_t)amount >= sizeof(shell->input_line)) {
      inputd_disconnect(shell, "line too long");
      return;
    }
    memcpy(shell->input_line + shell->input_length, buffer, (size_t)amount);
    shell->input_length += (size_t)amount;
    shell->input_line[shell->input_length] = '\0';

    char *newline = memchr(shell->input_line, '\n', shell->input_length);
    while (newline != NULL) {
      size_t length = (size_t)(newline - shell->input_line);
      if (length > 0 && shell->input_line[length - 1] == '\r') {
        --length;
      }
      shell->input_line[length] = '\0';
      inputd_handle_line(shell, shell->input_line);
      size_t consumed = (size_t)(newline - shell->input_line) + 1;
      memmove(shell->input_line, shell->input_line + consumed,
              shell->input_length - consumed);
      shell->input_length -= consumed;
      shell->input_line[shell->input_length] = '\0';
      newline = memchr(shell->input_line, '\n', shell->input_length);
    }
  }
}

/* --------------------------------------------------------------- menu env */

static bool app_env_set(bd_app *app, const char *key, const char *value,
                        bool prepend) {
  for (size_t i = 0; i < app->env_count; ++i) {
    if (strcmp(app->env_keys[i], key) != 0) {
      continue;
    }
    char *next = NULL;
    if (prepend && strstr(app->env_values[i], value) != NULL) {
      return true;
    }
    if (prepend && app->env_values[i][0] != '\0') {
      size_t size = strlen(value) + 1 + strlen(app->env_values[i]) + 1;
      next = (char *)malloc(size);
      if (next != NULL) {
        snprintf(next, size, "%s %s", value, app->env_values[i]);
      }
    } else {
      next = strdup(value);
    }
    if (next == NULL) {
      return false;
    }
    free(app->env_values[i]);
    app->env_values[i] = next;
    return true;
  }
  if (app->env_count >= 40) {
    return false;
  }
  size_t count = app->env_count + 1;
  char *key_copy = strdup(key);
  char *value_copy = strdup(value);
  char **keys = (char **)malloc(count * sizeof(char *));
  char **values = (char **)malloc(count * sizeof(char *));
  if (key_copy == NULL || value_copy == NULL || keys == NULL ||
      values == NULL) {
    free(key_copy);
    free(value_copy);
    free(keys);
    free(values);
    return false;
  }
  for (size_t i = 0; i < app->env_count; ++i) {
    keys[i] = app->env_keys[i];
    values[i] = app->env_values[i];
  }
  keys[app->env_count] = key_copy;
  values[app->env_count] = value_copy;
  free(app->env_keys);
  free(app->env_values);
  app->env_keys = keys;
  app->env_values = values;
  app->env_count = count;
  return true;
}

static bool file_readable(const char *path) {
  return path != NULL && path[0] != '\0' && access(path, R_OK) == 0;
}

static const char *menu_shim_path(bd_shell *shell, const char *configured) {
  if (shell->menu_shim_checked) {
    return shell->menu_shim[0] != '\0' ? shell->menu_shim : NULL;
  }
  shell->menu_shim_checked = true;
  shell->menu_shim[0] = '\0';

  if (file_readable(configured)) {
    copy_text(shell->menu_shim, sizeof(shell->menu_shim), configured);
    return shell->menu_shim;
  }
  const char *env = getenv("BRICKDESK_MENU_SHIM");
  if (file_readable(env)) {
    copy_text(shell->menu_shim, sizeof(shell->menu_shim), env);
    return shell->menu_shim;
  }

  char exe[BD_SHELL_PATH_MAX];
  ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (length > 0) {
    exe[length] = '\0';
    char *slash = strrchr(exe, '/');
    if (slash != NULL) {
      *slash = '\0';
      char candidate[BD_SHELL_PATH_MAX * 2];
      snprintf(candidate, sizeof(candidate), "%s/libbrickdesk-menu.so", exe);
      if (file_readable(candidate)) {
        copy_text(shell->menu_shim, sizeof(shell->menu_shim), candidate);
        return shell->menu_shim;
      }
      snprintf(candidate, sizeof(candidate), "%s/../lib/libbrickdesk-menu.so",
               exe);
      if (file_readable(candidate)) {
        copy_text(shell->menu_shim, sizeof(shell->menu_shim), candidate);
        return shell->menu_shim;
      }
    }
  }

  static const char *const candidates[] = {
      "/usr/lib/libbrickdesk-menu.so",
      "/usr/local/lib/libbrickdesk-menu.so",
      "/mnt/SDCARD/brickdesk/lib/libbrickdesk-menu.so",
  };
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
    if (file_readable(candidates[i])) {
      copy_text(shell->menu_shim, sizeof(shell->menu_shim), candidates[i]);
      return shell->menu_shim;
    }
  }
  return NULL;
}

static bool apply_menu_env(bd_shell *shell, bd_app *app,
                           const char *configured_shim) {
  if (app->menu.mode != BD_APP_MENU_OVERLAY) {
    return true;
  }
  const char *shim = menu_shim_path(shell, configured_shim);
  if (shim == NULL) {
    if (shell->verbose) {
      fprintf(stderr,
              "brickdesk: menu shim not found; launching %s without overlay\n",
              app->id);
    }
    return true;
  }
  if (!app_env_set(app, "LD_PRELOAD", shim, true)) {
    return false;
  }
  if (shell->input_socket[0] != '\0') {
    app_env_set(app, "BRICKDESK_INPUT_SOCKET", shell->input_socket, false);
  }
  const char *perf_socket = getenv("BRICKDESK_PERF_SOCKET");
  if (perf_socket != NULL && perf_socket[0] != '\0') {
    app_env_set(app, "BRICKDESK_PERF_SOCKET", perf_socket, false);
  }
  const char *asset_dir = getenv("BRICKDESK_ASSET_DIR");
  if (asset_dir != NULL && asset_dir[0] != '\0') {
    app_env_set(app, "BRICKDESK_ASSET_DIR", asset_dir, false);
  }
  if (shell->settings != NULL) {
    const char *config_dir = bd_config_dir(shell->settings);
    if (config_dir != NULL && config_dir[0] != '\0') {
      app_env_set(app, "BRICKDESK_CONFIG_DIR", config_dir, false);
    }
  }
  return true;
}

/* ----------------------------------------------------------- session glue */

static void on_session_event(void *user, const bd_session_event *event) {
  bd_shell *shell = (bd_shell *)user;
  if (shell->pending_count >= BD_SHELL_PENDING_MAX) {
    shell->rescan = true;
    return;
  }
  shell_pending *pending = &shell->pending[shell->pending_count++];
  memset(pending, 0, sizeof(*pending));
  switch (event->kind) {
    case BD_SESSION_EVENT_STATE:
      pending->kind = SHELL_PENDING_STATE;
      pending->state = event->state;
      break;
    case BD_SESSION_EVENT_EXIT:
    case BD_SESSION_EVENT_CRASH:
      pending->kind = SHELL_PENDING_EXIT;
      pending->state = event->state;
      break;
    case BD_SESSION_EVENT_MENU_ACTION:
      pending->kind = SHELL_PENDING_MENU_ACTION;
      break;
    case BD_SESSION_EVENT_NEEDS_ATTENTION:
      pending->kind = SHELL_PENDING_ATTENTION;
      break;
    default:
      return;
  }
  copy_text(pending->app_id, sizeof(pending->app_id), event->app_id);
  copy_text(pending->method, sizeof(pending->method), event->method);
}

static bool session_active_state(bd_session_state state) {
  switch (state) {
    case BD_SESSION_LAUNCHING:
    case BD_SESSION_FOREGROUND:
    case BD_SESSION_SUSPENDING:
    case BD_SESSION_RESUMING:
    case BD_SESSION_TERMINATING:
      return true;
    case BD_SESSION_IDLE:
    case BD_SESSION_BACKGROUND:
    case BD_SESSION_EXITED:
    case BD_SESSION_CRASHED:
    default:
      return false;
  }
}

static bool session_running_state(bd_session_state state) {
  switch (state) {
    case BD_SESSION_LAUNCHING:
    case BD_SESSION_FOREGROUND:
    case BD_SESSION_SUSPENDING:
    case BD_SESSION_RESUMING:
    case BD_SESSION_TERMINATING:
    case BD_SESSION_BACKGROUND:
      return true;
    default:
      return false;
  }
}

static int shell_test_memory_fixture(void) {
  /*
   * The screenshot harness sets this to keep memory text deterministic.
   * Production never sets it, so the shell uses the real /proc sample.
   */
  const char *value = getenv("BRICKDESK_TEST_MEMORY_MB");
  if (value == NULL || value[0] == '\0') {
    return -1;
  }
  char *end = NULL;
  long mb = strtol(value, &end, 10);
  if (end == value || *end != '\0' || mb < 0 || mb > 4096) {
    return -1;
  }
  return (int)mb;
}

static int shell_runtime_memory(shell_app_runtime *runtime,
                                const bd_session_app_info *info, uint64_t now,
                                bool panel_open) {
  if (runtime == NULL || info == NULL) {
    return -1;
  }
  int fixture = shell_test_memory_fixture();
  if (fixture >= 0) {
    runtime->memory_mb = fixture;
    runtime->memory_sampled_ms = now;
    return fixture;
  }
  uint64_t interval =
      runtime->memory_mb < 0 ? BD_SHELL_MEMORY_RETRY_MS : BD_SHELL_MEMORY_MS;
  bool due = runtime->memory_sampled_ms == 0 ||
             (panel_open && now - runtime->memory_sampled_ms >= interval);
  if (!due) {
    return runtime->memory_mb;
  }
  runtime->memory_mb = bd_sys_process_tree_rss_mb(info->pid);
  runtime->memory_sampled_ms = now;
  return runtime->memory_mb;
}

/*
 * Hides the home once the launch card has reached full screen, or starts a
 * requested return as soon as the launch transition allows it.
 */
static void shell_update_home(bd_shell *shell, uint64_t now) {
  if (shell->home == NULL) {
    return;
  }
  uint64_t wake_at = 0;
  bool was_active = bd_home_needs_frame_until(shell->home, &wake_at);
  bd_home_update(shell->home, now);
  if (was_active || bd_home_needs_frame_until(shell->home, &wake_at)) {
    /* Also guarantees the final frame after an animation deadline. */
    shell->dirty = true;
  }

  if (shell->return_app[0] != '\0') {
    if (bd_home_launch_target(shell->home) != NULL &&
        !bd_home_launch_complete(shell->home)) {
      return;
    }
    if (!bd_home_start_return(shell->home, shell->return_app)) {
      bd_home_damage(shell->home);
    }
    shell->return_app[0] = '\0';
    shell->visible = true;
    shell->dirty = true;
    return;
  }

  if (shell->visible && bd_home_launch_complete(shell->home)) {
    shell->visible = false;
    shell->dirty = false;
    bd_home_set_hint_visible(shell->home, false);
  }
}

static void shell_request_return(bd_shell *shell, const char *app_id) {
  if (shell->home == NULL || app_id == NULL || app_id[0] == '\0') {
    return;
  }
  copy_text(shell->return_app, sizeof(shell->return_app), app_id);
  shell_update_home(shell, shell_now(shell));
}

static void shell_refresh_running(bd_shell *shell) {
  if (shell->home == NULL) {
    return;
  }
  bool panel_open =
      bd_home_quick_open(shell->home) || bd_home_dialog_open(shell->home);
  uint64_t now = monotonic_ms();
  size_t app_count = shell->apps != NULL ? bd_app_list_count(shell->apps) : 0;
  for (size_t i = 0; i < app_count; ++i) {
    const bd_app *app = bd_app_list_at(shell->apps, i);
    if (app == NULL || i >= shell->runtime_count) {
      continue;
    }
    shell_app_runtime *runtime = &shell->runtime[i];
    bd_session_app_info info;
    bool found = shell->session != NULL &&
                 bd_session_find(shell->session, app->id, &info);
    bool background = found && info.state == BD_SESSION_BACKGROUND;
    if (background) {
      const char *frame = bd_session_last_frame_path(shell->session, app->id);
      const char *path = frame != NULL ? frame : "";
      bool changed = !runtime->running || !runtime->background ||
                     strcmp(runtime->frame_path, path) != 0;
      int memory = shell_runtime_memory(runtime, &info, now, panel_open);
      if (changed) {
        if (bd_home_set_running(shell->home, app->id, frame,
                                info.stopped_by_session, memory)) {
          runtime->running = true;
          copy_text(runtime->frame_path, sizeof(runtime->frame_path), path);
          shell->dirty = true;
        }
      } else {
        bd_home_set_running_memory(shell->home, app->id, memory);
      }
      runtime->background = true;
      continue;
    }
    runtime->background = false;
    if (found && !session_running_state(info.state)) {
      if (runtime->running) {
        bd_home_clear_running(shell->home, app->id);
        runtime->running = false;
        runtime->frame_path[0] = '\0';
        shell->dirty = true;
      }
    }
  }
  if (shell->session != NULL) {
    bd_session_clear_terminated(shell->session);
  }
}

/*
 * The "background full" dialog ends the LRU process first; retry the launch
 * once the session has actually dropped that instance.
 */
static void shell_try_pending_launch(bd_shell *shell) {
  if (shell->pending_launch[0] == '\0' || shell->evict_failed) {
    return;
  }
  if (shell->evict_app[0] != '\0') {
    bd_session_app_info info;
    if (shell->session != NULL &&
        bd_session_find(shell->session, shell->evict_app, &info) &&
        session_running_state(info.state)) {
      return;
    }
    shell->evict_app[0] = '\0';
  }
  char app_id[BD_APP_ID_MAX];
  copy_text(app_id, sizeof(app_id), shell->pending_launch);
  shell->pending_launch[0] = '\0';
  if (!shell_launch_app(shell, app_id) && shell->verbose) {
    fprintf(stderr, "brickdesk: retry launch %s failed\n", app_id);
  }
}

static void shell_process_pending(bd_shell *shell) {
  while (shell->pending_count > 0) {
    shell_pending batch[BD_SHELL_PENDING_MAX];
    int count = shell->pending_count;
    memcpy(batch, shell->pending, (size_t)count * sizeof(batch[0]));
    shell->pending_count = 0;
    for (int i = 0; i < count; ++i) {
      shell_pending *pending = &batch[i];
      switch (pending->kind) {
        case SHELL_PENDING_STATE:
          if (pending->state == BD_SESSION_BACKGROUND) {
            shell_refresh_running(shell);
            shell_request_return(shell, pending->app_id);
          }
          break;
        case SHELL_PENDING_EXIT: {
          shell_refresh_running(shell);
          const char *launching = bd_home_launch_target(shell->home);
          bool launch_owner = launching != NULL &&
                              strcmp(launching, pending->app_id) == 0;
          if (!shell->visible || launch_owner) {
            shell_request_return(shell, pending->app_id);
          }
          shell->dirty = true;
          break;
        }
        case SHELL_PENDING_MENU_ACTION:
          if (shell->session == NULL) {
            break;
          }
          if (strcmp(pending->method, "background") == 0) {
            bd_session_result result =
                bd_session_background(shell->session, pending->app_id);
            if (shell->verbose) {
              fprintf(stderr, "brickdesk: menu background %s -> %s\n",
                      pending->app_id, bd_session_result_name(result));
            }
          } else if (strcmp(pending->method, "quit") == 0) {
            bd_session_result result =
                bd_session_terminate(shell->session, pending->app_id);
            if (shell->verbose) {
              fprintf(stderr, "brickdesk: menu quit %s -> %s\n",
                      pending->app_id, bd_session_result_name(result));
            }
          }
          shell->dirty = true;
          break;
        case SHELL_PENDING_ATTENTION:
          if (shell->verbose) {
            fprintf(stderr, "brickdesk: background limit reached\n");
          }
          break;
        default:
          break;
      }
    }
  }
  if (shell->rescan) {
    shell->rescan = false;
  }
  shell_refresh_running(shell);
  shell_try_pending_launch(shell);
}

static void shell_refresh_status(bd_shell *shell, bool force) {
  if (shell == NULL || shell->home == NULL) {
    return;
  }
  uint64_t now = monotonic_ms();
  if (!force && now - shell->status_sampled_ms < BD_SHELL_STATUS_MS) {
    return;
  }
  shell->status_sampled_ms = now;
  int battery = -1;
  if (!bd_sys_battery_percent(NULL, &battery)) {
    battery = -1;
  }
  bd_wifi_state wifi = bd_sys_wifi_state(NULL);
  bool known = wifi != BD_WIFI_UNKNOWN;
  bool connected = wifi == BD_WIFI_CONNECTED;
  if (!force && battery == shell->status_battery &&
      known == shell->status_wifi_known && connected == shell->status_wifi) {
    return;
  }
  shell->status_battery = battery;
  shell->status_wifi_known = known;
  shell->status_wifi = connected;
  bd_home_status status;
  memset(&status, 0, sizeof(status));
  status.clock = shell->clock;
  status.wifi_known = known;
  status.wifi = connected;
  status.battery_percent = battery;
  status.max_background =
      (int)bd_config_get_int(shell->settings, "session.max_background", 3);
  bd_home_set_status(shell->home, &status);
  shell->dirty = true;
}

static void shell_service(bd_shell *shell) {
  uint64_t now = shell_now(shell);
  shell_update_home(shell, now);
  shell_refresh_status(shell, false);
  if (!shell->inputd_connected) {
    bd_input_state_advance(&shell->input, monotonic_ms(), local_emit, shell);
  }
  shell->session_next_ms = -1;
  if (shell->session != NULL) {
    int next = bd_session_poll(shell->session);
    if (next >= 0) {
      shell->session_next_ms = next;
    }
  }
  if (shell->plugins != NULL) {
    if (bd_plugin_host_poll(shell->plugins, 0) < 0) {
      if (shell->verbose) {
        fprintf(stderr, "brickdesk: plugin poll failed\n");
      }
    } else if (bd_plugin_host_take_dirty(shell->plugins)) {
      shell->dirty = true;
    }
  }
  shell_process_pending(shell);
  shell_update_home(shell, shell_now(shell));
}

/* ---------------------------------------------------------- dispatch path */

static void shell_handle_dialog_result(bd_shell *shell) {
  bd_home_dialog_result result = bd_home_take_dialog_result(shell->home);
  if (result == BD_HOME_DIALOG_RESULT_NONE) {
    return;
  }
  if (result == BD_HOME_DIALOG_RESULT_CANCEL) {
    shell->pending_launch[0] = '\0';
    shell->evict_app[0] = '\0';
    shell->evict_failed = false;
    shell->dialog_app[0] = '\0';
    return;
  }
  if (shell->pending_launch[0] != '\0' && shell->evict_app[0] != '\0') {
    /* Background-full confirm: end the LRU app, then retry in the loop. */
    if (shell->session != NULL) {
      bd_session_result terminated =
          bd_session_terminate(shell->session, shell->evict_app);
      if (shell->verbose) {
        fprintf(stderr, "brickdesk: evict %s -> %s\n", shell->evict_app,
                bd_session_result_name(terminated));
      }
      if (terminated != BD_SESSION_OK) {
        const char *victim = shell->evict_app;
        bd_session_app_info info;
        if (bd_session_find(shell->session, shell->evict_app, &info) &&
            info.name[0] != '\0') {
          victim = info.name;
        }
        char message[160];
        snprintf(message, sizeof(message), "无法结束「%s」，请重试或取消", victim);
        shell->evict_failed = true;
        if (!bd_home_open_background_full(
                shell->home, shell->pending_launch, shell->evict_app,
                bd_session_max_background(shell->session))) {
          shell->pending_launch[0] = '\0';
          shell->evict_app[0] = '\0';
          shell->evict_failed = false;
        } else {
          bd_home_set_dialog_error(shell->home, message);
        }
        shell->dirty = true;
        return;
      }
      shell->evict_failed = false;
    }
  } else if (shell->dialog_app[0] != '\0' && shell->session != NULL) {
    bd_session_result terminated =
        bd_session_terminate(shell->session, shell->dialog_app);
    if (shell->verbose) {
      fprintf(stderr, "brickdesk: end %s -> %s\n", shell->dialog_app,
              bd_session_result_name(terminated));
    }
  }
  shell->dialog_app[0] = '\0';
  shell->dirty = true;
}

static bool shell_home_input(bd_shell *shell, bd_home_action action) {
  if (shell->home == NULL) {
    return false;
  }
  if (action == BD_HOME_ACTION_END) {
    const char *app_id = bd_home_selected_id(shell->home);
    if (app_id != NULL) {
      copy_text(shell->dialog_app, sizeof(shell->dialog_app), app_id);
    }
  }
  bool changed = bd_home_input(shell->home, action);
  if (action == BD_HOME_ACTION_END && !bd_home_dialog_open(shell->home)) {
    /* X on a non-running tile: do not leave a stale end target behind. */
    shell->dialog_app[0] = '\0';
  }
  shell_handle_dialog_result(shell);
  if (changed) {
    shell->dirty = true;
  }
  return changed;
}

static void shell_accept(bd_shell *shell) {
  if (shell->home == NULL) {
    return;
  }
  if (bd_home_dialog_open(shell->home)) {
    shell_home_input(shell, BD_HOME_ACTION_ACCEPT);
    return;
  }
  const char *app_id = bd_home_selected_id(shell->home);
  if (app_id != NULL) {
    shell_launch_app(shell, app_id);
    return;
  }
  if (bd_home_quick_open(shell->home)) {
    /* Quick-panel control row: let the home toggle it. */
    shell_home_input(shell, BD_HOME_ACTION_ACCEPT);
  }
}

static void shell_dispatch_key(bd_shell *shell, bd_key key, bool pressed) {
  if (!shell->visible || !pressed) {
    return;
  }
  switch (key) {
    case BD_KEY_A:
      shell_accept(shell);
      break;
    case BD_KEY_B:
      if (bd_home_dialog_open(shell->home) || bd_home_quick_open(shell->home)) {
        shell_home_input(shell, BD_HOME_ACTION_CANCEL);
      }
      break;
    case BD_KEY_X:
      shell_home_input(shell, BD_HOME_ACTION_END);
      break;
    case BD_KEY_START:
      shell_home_input(shell, BD_HOME_ACTION_QUICK);
      break;
    case BD_KEY_LEFT:
      shell_home_input(shell, BD_HOME_ACTION_LEFT);
      break;
    case BD_KEY_RIGHT:
      shell_home_input(shell, BD_HOME_ACTION_RIGHT);
      break;
    case BD_KEY_UP:
      shell_home_input(shell, BD_HOME_ACTION_UP);
      break;
    case BD_KEY_DOWN:
      shell_home_input(shell, BD_HOME_ACTION_DOWN);
      break;
    case BD_KEY_L1:
      shell_home_input(shell, BD_HOME_ACTION_TAB_PREV);
      break;
    case BD_KEY_R1:
      shell_home_input(shell, BD_HOME_ACTION_TAB_NEXT);
      break;
    case BD_KEY_HOME:
      shell_home_input(shell, BD_HOME_ACTION_RECENT);
      break;
    default:
      break;
  }
}

static bool shell_terminate_foreground(bd_shell *shell) {
  if (shell->session == NULL) {
    return false;
  }
  size_t count = bd_session_running_count(shell->session);
  for (size_t i = 0; i < count; ++i) {
    bd_session_app_info info;
    if (!bd_session_running_at(shell->session, i, &info)) {
      continue;
    }
    if (session_active_state(info.state)) {
      bd_session_result result =
          bd_session_terminate(shell->session, info.app_id);
      if (shell->verbose) {
        fprintf(stderr, "brickdesk: return_home %s -> %s\n", info.app_id,
                bd_session_result_name(result));
      }
      return result == BD_SESSION_OK;
    }
  }
  return false;
}

static void shell_dispatch_action(bd_shell *shell, const char *action,
                                  bool pressed) {
  if (action == NULL) {
    return;
  }
  if (strcmp(action, "ui.hint") == 0 && shell->visible) {
    bd_home_set_hint_visible(shell->home, pressed);
    shell->dirty = true;
  }
  if (strcmp(action, "session.return_home") == 0 && pressed &&
      !shell->visible) {
    if (shell_terminate_foreground(shell)) {
      shell->dirty = true;
    }
  }
  if (shell->verbose &&
      (strcmp(action, "menu.open") == 0 || strcmp(action, "perf.cycle") == 0 ||
       strcmp(action, "builtin.screenshot") == 0)) {
    fprintf(stderr, "brickdesk: action %s\n", action);
  }
}

static bool shell_home_begin_launch(bd_shell *shell, const char *app_id) {
  if (shell->home == NULL) {
    return false;
  }
  uint64_t now = shell_now(shell);
  bd_home_update(shell->home, now);
  if (!bd_home_start_launch(shell->home, app_id)) {
    fprintf(stderr, "brickdesk: cannot start launch animation for %s\n",
            app_id);
    return false;
  }
  shell->visible = true;
  shell->dirty = true;
  return true;
}

/*
 * The session only refuses a *background* transition when the quota is full,
 * but the home must also refuse a new cold launch.  Show the choice and let
 * shell_handle_dialog_result() end the LRU app before retrying.
 */
static bool shell_request_eviction(bd_shell *shell, const char *app_id) {
  if (shell->session == NULL || shell->home == NULL) {
    return false;
  }
  bd_session_eviction eviction;
  memset(&eviction, 0, sizeof(eviction));
  if (!bd_session_suggest_eviction(shell->session, &eviction)) {
    return false;
  }
  copy_text(shell->pending_launch, sizeof(shell->pending_launch), app_id);
  copy_text(shell->evict_app, sizeof(shell->evict_app), eviction.app_id);
  shell->evict_failed = false;
  bd_home_update(shell->home, shell_now(shell));
  if (!bd_home_open_background_full(shell->home, app_id, eviction.app_id,
                                    bd_session_max_background(shell->session))) {
    shell->pending_launch[0] = '\0';
    shell->evict_app[0] = '\0';
    return false;
  }
  shell->visible = true;
  shell->dirty = true;
  return true;
}

static bool shell_launch_app(bd_shell *shell, const char *app_id) {
  if (shell->session == NULL || app_id == NULL || app_id[0] == '\0') {
    return false;
  }
  bd_session_app_info info;
  if (bd_session_find(shell->session, app_id, &info)) {
    if (info.state == BD_SESSION_BACKGROUND) {
      bd_session_result result = bd_session_resume(shell->session, app_id);
      if (result == BD_SESSION_OK) {
        return shell_home_begin_launch(shell, app_id);
      }
      if (shell->verbose) {
        fprintf(stderr, "brickdesk: resume %s -> %s\n", app_id,
                bd_session_result_name(result));
      }
      return false;
    }
    if (session_active_state(info.state) || info.state == BD_SESSION_CRASHED) {
      return false;
    }
    bd_session_clear_terminated(shell->session);
  }
  const bd_app *app = bd_app_list_find(shell->apps, app_id);
  if (app == NULL) {
    if (shell->verbose) {
      fprintf(stderr, "brickdesk: app '%s' is not installed\n", app_id);
    }
    return false;
  }
  int max_background = bd_session_max_background(shell->session);
  if (max_background > 0 &&
      bd_session_background_count(shell->session) >= max_background) {
    if (shell_request_eviction(shell, app_id)) {
      return true;
    }
    fprintf(stderr, "brickdesk: launch %s needs a free background slot\n",
            app_id);
    return false;
  }
  bd_app copy;
  memset(&copy, 0, sizeof(copy));
  if (!bd_app_copy(&copy, app)) {
    return false;
  }
  if (!apply_menu_env(shell, &copy, shell->menu_shim)) {
    bd_app_destroy(&copy);
    return false;
  }
  char error[256] = {0};
  bd_session_result result =
      bd_session_launch(shell->session, &copy, error, sizeof(error));
  bd_app_destroy(&copy);
  if (result == BD_SESSION_NEEDS_CONFIRM) {
    if (shell_request_eviction(shell, app_id)) {
      return true;
    }
    fprintf(stderr, "brickdesk: launch %s needs a free background slot\n",
            app_id);
    return false;
  }
  if (result != BD_SESSION_OK) {
    fprintf(stderr, "brickdesk: launch %s failed: %s%s%s\n", app_id,
            bd_session_result_name(result), error[0] != '\0' ? ": " : "",
            error);
    return false;
  }
  if (!shell_home_begin_launch(shell, app_id)) {
    return false;
  }
  if (shell->verbose) {
    const char *shim = menu_shim_path(shell, NULL);
    fprintf(stderr, "brickdesk: launched %s%s%s\n", app_id,
            shim != NULL ? " with " : "",
            shim != NULL ? shim : " without menu overlay");
  }
  return true;
}

/* ------------------------------------------------------------ applications */

typedef struct shell_demo_item {
  const char *id;
  const char *title;
  const char *subtitle;
  int span_w;
  int span_h;
  int cover;
  int r;
  int g;
  int b;
  uint32_t flags;
} shell_demo_item;

static const shell_demo_item SHELL_DEMO_ITEMS[] = {
    {"demo-town", "小镇四季", "第 3 年夏季 · 已玩 41 小时", 3, 2, 1, 109, 122,
     99, 0},
    {"demo-space", "星海拾荒者", "第 7 扇区 · 已玩 6 小时", 2, 2, 2, 76, 92,
     112, 0},
    {"demo-terminal", "终端", "本机 Shell", 1, 1, 3, 44, 46, 44, 0},
    {"demo-settings", "设置", "显示与声音", 1, 1, 4, 111, 115, 123, 0},
    {"demo-music", "音乐", "夜航 · 林间电台", 2, 2, 5, 123, 90, 140,
     BD_HOME_ITEM_LIVE},
    {"demo-reading", "阅读", "《三体》第 14 章", 1, 1, 6, 93, 124, 90, 0},
    {"demo-files", "文件", "SD 卡已用 41 GB", 1, 1, 7, 76, 92, 112, 0},
    {"demo-photos", "相册", "截图与照片 86 张", 1, 1, 8, 176, 138, 78, 0},
    {"demo-transfer", "无线传输", "浏览器传文件", 2, 1, 1, 63, 133, 135, 0},
    {"demo-system", "系统", "屏幕、网络与按键", 1, 1, 2, 111, 115, 123, 0},
    {"demo-harbor", "雾港", "第二章「灯塔」", 1, 1, 3, 81, 96, 108,
     BD_HOME_ITEM_GAME},
    {"demo-well", "深井", "到达地下 12 层", 1, 1, 4, 75, 94, 102,
     BD_HOME_ITEM_GAME},
    {"demo-road", "赤色公路", "最佳圈速 1:42.08", 1, 1, 5, 162, 90, 76,
     BD_HOME_ITEM_GAME},
    {"demo-favorites", "收藏夹", "12 个收藏应用", 1, 1, 6, 123, 90, 140,
     BD_HOME_ITEM_FAVORITE},
};

static void shell_span_for_index(int index, int *width, int *height) {
  static const int CYCLE[24][2] = {
      {3, 2}, {1, 1}, {1, 1}, {2, 2}, {1, 1}, {1, 2}, {1, 1}, {1, 1},
      {1, 1}, {2, 1}, {1, 1}, {1, 1}, {2, 2}, {1, 1}, {2, 1}, {1, 2},
      {1, 1}, {2, 2}, {1, 1}, {2, 1}, {1, 1}, {2, 2}, {1, 1}, {1, 1},
  };
  int cycle = index % 24;
  *width = CYCLE[cycle][0];
  *height = CYCLE[cycle][1];
}

static bd_app_cover_span shell_cover_span(int width, int height) {
  if (width >= 3 && height >= 2) {
    return BD_APP_SPAN_3X2;
  }
  if (width >= 2 && height >= 2) {
    return BD_APP_SPAN_2X2;
  }
  if (width >= 2) {
    return BD_APP_SPAN_2X1;
  }
  if (height >= 2) {
    return BD_APP_SPAN_1X2;
  }
  return BD_APP_SPAN_1X1;
}

static bd_color shell_app_placeholder(const bd_app *app,
                                      const bd_app_asset *asset) {
  bd_color color;
  if (asset != NULL && asset->color[0] != '\0' &&
      bd_color_from_hex(asset->color, &color)) {
    return bd_color_mix(bd_color_rgb(18, 16, 13), color, 0.55f);
  }
  const char *hex = app->accent[0] != '\0' ? app->accent : app->fallback_color;
  if (hex[0] != '\0' && bd_color_from_hex(hex, &color)) {
    return bd_color_mix(bd_color_rgb(18, 16, 13), color, 0.55f);
  }
  return bd_color_rgb(58, 52, 46);
}

static bool shell_set_demo_items(bd_shell *shell) {
  int count = (int)(sizeof(SHELL_DEMO_ITEMS) / sizeof(SHELL_DEMO_ITEMS[0]));
  bd_home_item *items =
      (bd_home_item *)calloc((size_t)count, sizeof(*items));
  char(*covers)[512] = (char(*)[512])calloc((size_t)count, 512);
  if (items == NULL || covers == NULL) {
    free(items);
    free(covers);
    return false;
  }
  for (int i = 0; i < count; ++i) {
    const shell_demo_item *demo = &SHELL_DEMO_ITEMS[i];
    snprintf(covers[i], 512, "%s/covers/home-%02d.jpg", BRICKDESK_ASSET_DIR,
             demo->cover);
    items[i].id = demo->id;
    items[i].title = demo->title;
    items[i].subtitle = demo->subtitle;
    items[i].image_path = covers[i];
    items[i].placeholder =
        bd_color_rgb((uint8_t)demo->r, (uint8_t)demo->g, (uint8_t)demo->b);
    items[i].span_w = demo->span_w;
    items[i].span_h = demo->span_h;
    items[i].flags = demo->flags;
    items[i].badge_number = i + 1;
  }
  bool ok = bd_home_set_items(shell->home, items, count);
  free(items);
  free(covers);
  return ok;
}

static bool shell_set_app_items(bd_shell *shell) {
  size_t count = shell->apps != NULL ? bd_app_list_count(shell->apps) : 0;
  if (count == 0) {
    return shell_set_demo_items(shell);
  }
  if (count > 100000) {
    return false;
  }
  bd_home_item *items = (bd_home_item *)calloc(count, sizeof(*items));
  char(*covers)[BD_APP_PATH_MAX] =
      (char(*)[BD_APP_PATH_MAX])calloc(count, BD_APP_PATH_MAX);
  if (items == NULL || covers == NULL) {
    free(items);
    free(covers);
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    const bd_app *app = bd_app_list_at(shell->apps, i);
    if (app == NULL) {
      continue;
    }
    int span_w = 1;
    int span_h = 1;
    shell_span_for_index((int)i, &span_w, &span_h);
    bd_app_asset asset;
    memset(&asset, 0, sizeof(asset));
    bool have_asset =
        bd_app_asset_cover(app, shell_cover_span(span_w, span_h), &asset);
    if (have_asset && asset.path[0] != '\0') {
      copy_text(covers[i], BD_APP_PATH_MAX, asset.path);
    }
    items[i].id = app->id;
    items[i].title = app->name;
    items[i].subtitle =
        app->subtitle[0] != '\0' ? app->subtitle
                                 : bd_app_category_name(app->category);
    items[i].image_path = covers[i][0] != '\0' ? covers[i] : NULL;
    items[i].placeholder = shell_app_placeholder(app, &asset);
    items[i].span_w = span_w;
    items[i].span_h = span_h;
    items[i].flags = (app->category == BD_APP_CATEGORY_GAME ||
                      app->category == BD_APP_CATEGORY_EMULATOR)
                         ? BD_HOME_ITEM_GAME
                         : 0;
    items[i].badge_number = (int)i + 1;
  }
  bool ok = bd_home_set_items(shell->home, items, (int)count);
  free(items);
  free(covers);
  return ok;
}

static void shell_load_apps(bd_shell *shell, const char *apps_dir) {
  free(shell->runtime);
  shell->runtime = NULL;
  shell->runtime_count = 0;
  if (shell->apps != NULL) {
    bd_app_list_destroy(shell->apps);
  }
  shell->apps = bd_app_list_create();
  const char *dir = apps_dir != NULL ? apps_dir : bd_app_default_dir();
  if (shell->apps != NULL) {
    char error[256] = {0};
    if (!bd_app_list_scan(shell->apps, dir, NULL, NULL, error,
                          sizeof(error))) {
      fprintf(stderr, "brickdesk: apps: %s\n", error);
    }
  }
  size_t count = shell->apps != NULL ? bd_app_list_count(shell->apps) : 0;
  if (count > 0) {
    shell->runtime =
        (shell_app_runtime *)calloc(count, sizeof(*shell->runtime));
    if (shell->runtime != NULL) {
      shell->runtime_count = count;
      for (size_t i = 0; i < count; ++i) {
        shell->runtime[i].memory_mb = -1;
      }
    }
  }
  if (!shell_set_app_items(shell)) {
    fprintf(stderr, "brickdesk: cannot build the home wall\n");
  }
  if (shell->verbose) {
    fprintf(stderr, "brickdesk: %zu app(s) from %s\n", count, dir);
  }
}

/* ------------------------------------------------------------------- loop */

bool bd_shell_render(bd_shell *shell) {
  if (shell == NULL || !shell->visible || shell->home == NULL) {
    return false;
  }
  bd_home_update(shell->home, shell_now(shell));
  bd_gfx_begin_frame(shell->gfx);
  bd_home_draw(shell->home, shell->gfx);
  bd_gfx_end_frame(shell->gfx);
  bd_gfx_stats stats = bd_gfx_get_stats(shell->gfx);
  if (stats.presented) {
    bd_platform_present(shell->platform);
    ++shell->presents;
  }
  shell->dirty = false;
  return stats.presented;
}

static void shell_handle_platform_event(bd_shell *shell, const bd_event *event) {
  switch (event->type) {
    case BD_EVENT_QUIT:
      shell->quit = true;
      break;
    case BD_EVENT_REDRAW:
      shell->dirty = true;
      break;
    case BD_EVENT_KEY:
      if (!shell->inputd_connected) {
        bd_input_state_feed(&shell->input, event->key, event->pressed,
                            monotonic_ms(), local_emit, shell);
      }
      break;
    default:
      break;
  }
}

static int shell_poll_timeout(bd_shell *shell) {
  int best = -1;
  best = merge_timeout(best, shell->session_next_ms);
  if (shell->plugins != NULL) {
    best = merge_timeout(best, bd_plugin_host_next_timeout(shell->plugins));
  }
  if (!shell->inputd_connected) {
    uint64_t deadline = 0;
    if (bd_input_state_next_deadline(&shell->input, &deadline)) {
      uint64_t now = monotonic_ms();
      uint64_t remaining = deadline > now ? deadline - now : 0;
      if (remaining > 60000) {
        remaining = 60000;
      }
      best = merge_timeout(best, (int)remaining);
    }
  }
  if (shell->visible && shell->home != NULL) {
    uint64_t wake_at = 0;
    if (bd_home_needs_frame_until(shell->home, &wake_at)) {
      uint64_t now = shell_now(shell);
      uint64_t remaining = wake_at > now ? wake_at - now : 0;
      /* A transition or decode keeps a 60 Hz frame deadline. */
      if (remaining > 16 || (shell->test_clock && remaining == 0)) {
        remaining = 16;
      }
      best = merge_timeout(best, (int)remaining);
    }
  }
  if (shell->dirty && shell->visible) {
    best = 0;
  }
  return best;
}

static int shell_build_fds(bd_shell *shell, struct pollfd *fds, int capacity,
                           int *kinds) {
  if (capacity <= 0) {
    return 0;
  }
  int count = 0;
  int platform_fd = bd_platform_event_fd(shell->platform);
  if (platform_fd >= 0) {
    fds[count].fd = platform_fd;
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    kinds[count] = 0;
    ++count;
  }
  if (shell->inputd_fd >= 0 && count < capacity) {
    fds[count].fd = shell->inputd_fd;
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    kinds[count] = 1;
    ++count;
  }
  int session_fds[BD_SHELL_FD_MAX];
  int session_count = 0;
  if (shell->session != NULL) {
    session_count = bd_session_fds(shell->session, session_fds,
                                   BD_SHELL_FD_MAX);
  }
  for (int i = 0; i < session_count && count < capacity; ++i) {
    fds[count].fd = session_fds[i];
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    kinds[count] = 2;
    ++count;
  }
  int plugin_fds[BD_SHELL_FD_MAX];
  int plugin_count = 0;
  if (shell->plugins != NULL) {
    plugin_count = bd_plugin_host_fds(shell->plugins, plugin_fds,
                                      BD_SHELL_FD_MAX);
  }
  for (int i = 0; i < plugin_count && count < capacity; ++i) {
    fds[count].fd = plugin_fds[i];
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    kinds[count] = 3;
    ++count;
  }
  return count;
}

int bd_shell_iterate(bd_shell *shell, int timeout_ms) {
  if (shell == NULL || shell->quit) {
    return -1;
  }
  shell_service(shell);
  if (shell->quit) {
    return -1;
  }
  if (shell->visible && shell->home != NULL) {
    uint64_t wake_at = 0;
    if (bd_home_needs_frame_until(shell->home, &wake_at)) {
      shell->dirty = true;
    }
  }
  if (shell->dirty && shell->visible) {
    bd_shell_render(shell);
  } else if (shell->dirty && !shell->visible) {
    shell->dirty = false;
  }

  int timeout = shell_poll_timeout(shell);
  if (timeout_ms >= 0 && (timeout < 0 || timeout_ms < timeout)) {
    timeout = timeout_ms;
  }

  struct pollfd fds[BD_SHELL_FD_MAX * 2 + 2];
  int kinds[BD_SHELL_FD_MAX * 2 + 2];
  int count = shell_build_fds(shell, fds, (int)(sizeof(fds) / sizeof(fds[0])),
                              kinds);
  int ready = poll(fds, (nfds_t)count, timeout);
  if (ready < 0) {
    if (errno == EINTR) {
      return 0;
    }
    return -1;
  }
  if (ready == 0) {
    return 0;
  }

  bool handled = false;
  for (int i = 0; i < count; ++i) {
    if (fds[i].revents == 0) {
      continue;
    }
    if (kinds[i] == 0) {
      bd_event event;
      while (bd_platform_pump_event(shell->platform, &event) > 0) {
        shell_handle_platform_event(shell, &event);
        handled = true;
        if (shell->quit) {
          return -1;
        }
      }
    } else if (kinds[i] == 1) {
      inputd_read(shell);
      handled = true;
    } else {
      handled = true;
    }
  }

  if (handled) {
    shell_service(shell);
    if (shell->visible && shell->home != NULL) {
      uint64_t wake_at = 0;
      if (bd_home_needs_frame_until(shell->home, &wake_at)) {
        shell->dirty = true;
      }
    }
    if (shell->dirty && shell->visible) {
      bd_shell_render(shell);
    } else if (shell->dirty && !shell->visible) {
      shell->dirty = false;
    }
  }
  return handled ? 1 : 0;
}

bool bd_shell_run_for(bd_shell *shell, int duration_ms) {
  if (shell == NULL) {
    return false;
  }
  uint64_t deadline = monotonic_ms() + (uint64_t)(duration_ms > 0 ? duration_ms : 0);
  for (;;) {
    if (shell->quit) {
      return false;
    }
    uint64_t now = monotonic_ms();
    if (now >= deadline) {
      return true;
    }
    uint64_t remaining = deadline - now;
    int cap = remaining > 60000 ? 60000 : (int)remaining;
    int result = bd_shell_iterate(shell, cap);
    if (result < 0) {
      return false;
    }
  }
}

bd_shell *bd_shell_create(const bd_shell_config *config) {
  if (config == NULL || config->platform == NULL || config->gfx == NULL ||
      config->settings == NULL) {
    return NULL;
  }
  bd_shell *shell = (bd_shell *)calloc(1, sizeof(*shell));
  if (shell == NULL) {
    return NULL;
  }
  shell->platform = config->platform;
  shell->gfx = config->gfx;
  shell->settings = config->settings;
  shell->plugins = config->plugins;
  shell->verbose = config->verbose;
  shell->inputd_fd = -1;
  shell->visible = true;
  shell->dirty = true;

  const char *theme_mode =
      bd_config_get_string(config->settings, "theme.mode", "dark");
  bool dark = strcmp(theme_mode, "light") != 0;
  const char *accent_name =
      bd_config_get_string(config->settings, "theme.accent", "persimmon");
  const char *accent_hex = bd_config_accent_hex(accent_name, dark);
  bd_color accent = bd_color_rgb(224, 122, 95);
  if (accent_hex == NULL || !bd_color_from_hex(accent_hex, &accent)) {
    fprintf(stderr, "brickdesk: config: invalid theme accent '%s'\n",
            accent_name);
    accent = bd_color_rgb(224, 122, 95);
  }
  memset(&shell->home_style, 0, sizeof(shell->home_style));
  shell->home_style.accent_base = accent;
  shell->home_style.dark = dark;
  shell->home_style.blur_enabled =
      bd_config_get_bool(config->settings, "ui.blur", true);
  shell->home_style.width = bd_gfx_width(config->gfx);
  shell->home_style.height = bd_gfx_height(config->gfx);
  shell->home = bd_home_create(config->gfx, &shell->home_style);
  if (shell->home == NULL) {
    fprintf(stderr, "brickdesk: cannot create the home page\n");
    free(shell);
    return NULL;
  }

  bd_home_status status;
  memset(&status, 0, sizeof(status));
  copy_text(shell->clock, sizeof(shell->clock), "22:26");
  if (getenv("BRICKDESK_HEADLESS") == NULL) {
    time_t wall = time(NULL);
    struct tm local;
    if (wall != (time_t)-1 && localtime_r(&wall, &local) != NULL) {
      strftime(shell->clock, sizeof(shell->clock), "%H:%M", &local);
    }
  }
  long max_background =
      bd_config_get_int(config->settings, "session.max_background", 3);
  status.clock = shell->clock;
  status.max_background = (int)max_background;
  bd_home_set_status(shell->home, &status);
  shell_refresh_status(shell, true);

  bd_hotkey_table_load(&shell->hotkeys, config->settings);
  shell->hotkey_table = &shell->hotkeys;
  if (shell->verbose && (shell->hotkeys.diagnostic_count > 0 ||
                         shell->hotkeys.conflict_count > 0)) {
    fprintf(stderr, "brickdesk: %zu hotkey diagnostic(s), %zu conflict(s)\n",
            shell->hotkeys.diagnostic_count, shell->hotkeys.conflict_count);
  }
  bd_input_state_init(&shell->input, &shell->hotkeys, "global",
                      (int)bd_config_get_int(config->settings,
                                             "input.double_tap_ms", 300));
  bd_input_state_init(&shell->test_input, &shell->hotkeys, "global",
                      (int)bd_config_get_int(config->settings,
                                             "input.double_tap_ms", 300));

  shell_load_apps(shell, config->apps_dir);

  bd_session_config session_config;
  memset(&session_config, 0, sizeof(session_config));
  session_config.socket_dir = config->session_socket_dir;
  session_config.frame_dir = config->session_frame_dir;
  session_config.state_root = config->session_state_root;
  session_config.max_background =
      (int)bd_config_get_int(config->settings, "session.max_background", 3);
  session_config.on_event = on_session_event;
  session_config.event_user = shell;
  char error[256] = {0};
  shell->session = bd_session_create(&session_config, error, sizeof(error));
  if (shell->session == NULL) {
    fprintf(stderr, "brickdesk: session: %s\n", error);
  }

  copy_text(shell->input_socket, sizeof(shell->input_socket),
            config->input_socket != NULL ? config->input_socket
                                         : preferred_input_socket());
  if (!inputd_connect(shell)) {
    if (shell->verbose) {
      fprintf(stderr, "brickdesk: inputd not reachable at %s; using SDL keys\n",
              shell->input_socket);
    }
  }
  if (config->menu_shim != NULL && file_readable(config->menu_shim)) {
    copy_text(shell->menu_shim, sizeof(shell->menu_shim), config->menu_shim);
    shell->menu_shim_checked = true;
  }
  return shell;
}

void bd_shell_destroy(bd_shell *shell) {
  if (shell == NULL) {
    return;
  }
  if (shell->session != NULL) {
    bd_session_destroy(shell->session);
  }
  inputd_disconnect(shell, "shutdown");
  free(shell->runtime);
  if (shell->apps != NULL) {
    bd_app_list_destroy(shell->apps);
  }
  bd_home_destroy(shell->home);
  free(shell);
}

bool bd_shell_visible(const bd_shell *shell) {
  return shell != NULL && shell->visible;
}

bool bd_shell_quit_requested(const bd_shell *shell) {
  return shell == NULL || shell->quit;
}

bool bd_shell_has_pending_images(const bd_shell *shell) {
  return shell != NULL && bd_home_has_pending_images(shell->home);
}

bool bd_shell_hint_visible(const bd_shell *shell) {
  return shell != NULL && bd_home_hint_visible(shell->home);
}

uint64_t bd_shell_present_count(const bd_shell *shell) {
  return shell != NULL ? shell->presents : 0;
}

const char *bd_shell_input_source(const bd_shell *shell) {
  return shell != NULL && shell->inputd_connected ? "inputd" : "sdl";
}

size_t bd_shell_app_count(const bd_shell *shell) {
  return shell != NULL && shell->apps != NULL ? bd_app_list_count(shell->apps)
                                              : 0;
}

const bd_app *bd_shell_app_at(const bd_shell *shell, size_t index) {
  return shell != NULL && shell->apps != NULL
             ? bd_app_list_at(shell->apps, index)
             : NULL;
}

bd_home *bd_shell_home(bd_shell *shell) {
  return shell != NULL ? shell->home : NULL;
}

void bd_shell_test_key(bd_shell *shell, bd_key key, bool pressed,
                       uint64_t time_ms) {
  if (shell == NULL) {
    return;
  }
  bd_input_state_feed(&shell->test_input, key, pressed, time_ms, local_emit,
                      shell);
}

void bd_shell_test_advance(bd_shell *shell, uint64_t time_ms) {
  if (shell == NULL) {
    return;
  }
  shell->test_clock = true;
  shell->test_now_ms = time_ms;
  bd_input_state_advance(&shell->test_input, time_ms, local_emit, shell);
}

bd_session *bd_shell_test_session(bd_shell *shell) {
  return shell != NULL ? shell->session : NULL;
}

bool bd_shell_test_launch(bd_shell *shell, const char *app_id) {
  return shell != NULL ? shell_launch_app(shell, app_id) : false;
}
