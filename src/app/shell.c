#define _POSIX_C_SOURCE 200809L

#include "brickdesk/shell.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define BD_SHELL_LINE_MAX 4096
#define BD_SHELL_FD_MAX 64
#define BD_SHELL_PENDING_MAX 16
#define BD_SHELL_PATH_MAX 1024
#define BD_SHELL_ANIMATION_MS 180

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

struct bd_shell {
  bd_platform *platform;
  bd_gfx *gfx;
  bd_desktop *desktop;
  const bd_config *settings;
  bd_plugin_host *plugins;
  bool verbose;

  bd_app_list *apps;
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

  bd_shell_animation animation;
  bool dirty;
  bool visible;
  bool quit;
  uint64_t presents;
};

static uint64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
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
    bd_desktop_set_hint_visible(shell->desktop, false);
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
    bd_desktop_set_hint_visible(shell->desktop, false);
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

static void shell_begin_animation(bd_shell *shell, bd_shell_animation_kind kind,
                                  const char *app_id) {
  shell->animation.kind = kind;
  shell->animation.started_ms = monotonic_ms();
  shell->animation.duration_ms = BD_SHELL_ANIMATION_MS;
  shell->animation.progress = 0.0f;
  copy_text(shell->animation.app_id, sizeof(shell->animation.app_id), app_id);
}

static void shell_update_animation(bd_shell *shell) {
  if (shell->animation.kind == BD_SHELL_ANIMATION_NONE) {
    return;
  }
  uint64_t now = monotonic_ms();
  uint64_t elapsed = now >= shell->animation.started_ms
                         ? now - shell->animation.started_ms
                         : 0;
  if (elapsed >= shell->animation.duration_ms) {
    shell->animation.progress = 1.0f;
    shell->animation.kind = BD_SHELL_ANIMATION_NONE;
    return;
  }
  shell->animation.progress =
      (float)elapsed / (float)shell->animation.duration_ms;
}

static void shell_refresh_running(bd_shell *shell) {
  bool visible = true;
  if (shell->session != NULL) {
    size_t count = bd_session_running_count(shell->session);
    for (size_t i = 0; i < count; ++i) {
      bd_session_app_info info;
      if (bd_session_running_at(shell->session, i, &info) &&
          session_active_state(info.state)) {
        visible = false;
        break;
      }
    }
  }
  size_t app_count = shell->apps != NULL ? bd_app_list_count(shell->apps) : 0;
  for (size_t i = 0; i < app_count; ++i) {
    const bd_app *app = bd_app_list_at(shell->apps, i);
    if (app == NULL) {
      continue;
    }
    bd_session_app_info info;
    bool running = false;
    const char *frame = NULL;
    if (shell->session != NULL &&
        bd_session_find(shell->session, app->id, &info)) {
      running = session_running_state(info.state);
      if (running) {
        frame = bd_session_last_frame_path(shell->session, app->id);
      }
    }
    bd_desktop_set_app_running(shell->desktop, app->id, running, frame);
  }
  if (visible != shell->visible) {
    shell->visible = visible;
    bd_desktop_set_hint_visible(shell->desktop, false);
    shell->dirty = visible;
  }
  if (shell->session != NULL) {
    bd_session_clear_terminated(shell->session);
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
            shell_begin_animation(shell, BD_SHELL_ANIMATION_RETURN,
                                  pending->app_id);
            shell->dirty = true;
          }
          break;
        case SHELL_PENDING_EXIT:
          shell_begin_animation(shell, BD_SHELL_ANIMATION_RETURN,
                                pending->app_id);
          shell->dirty = true;
          break;
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
}

static void shell_service(bd_shell *shell) {
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
}

/* ---------------------------------------------------------- dispatch path */

static void shell_dispatch_key(bd_shell *shell, bd_key key, bool pressed) {
  if (!shell->visible) {
    return;
  }
  if (pressed && key == BD_KEY_A) {
    const char *app_id = bd_desktop_focused_app_id(shell->desktop);
    if (app_id != NULL) {
      shell_launch_app(shell, app_id);
    }
    return;
  }
  if (bd_desktop_handle_key(shell->desktop, key, pressed)) {
    shell->dirty = true;
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
  if (bd_desktop_handle_action(shell->desktop, action, pressed)) {
    shell->dirty = true;
  }
  if (strcmp(action, "session.return_home") == 0 && pressed) {
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

static bool shell_launch_app(bd_shell *shell, const char *app_id) {
  if (shell->session == NULL || app_id == NULL) {
    return false;
  }
  bd_session_app_info info;
  if (bd_session_find(shell->session, app_id, &info)) {
    if (info.state == BD_SESSION_BACKGROUND) {
      bd_session_result result = bd_session_resume(shell->session, app_id);
      if (result == BD_SESSION_OK) {
        shell_begin_animation(shell, BD_SHELL_ANIMATION_LAUNCH, app_id);
        shell->dirty = false;
        return true;
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
  if (result != BD_SESSION_OK) {
    fprintf(stderr, "brickdesk: launch %s failed: %s%s%s\n", app_id,
            bd_session_result_name(result), error[0] != '\0' ? ": " : "",
            error);
    return false;
  }
  shell_begin_animation(shell, BD_SHELL_ANIMATION_LAUNCH, app_id);
  shell->dirty = false;
  if (shell->verbose) {
    const char *shim = menu_shim_path(shell, NULL);
    fprintf(stderr, "brickdesk: launched %s%s%s\n", app_id,
            shim != NULL ? " with " : "",
            shim != NULL ? shim : " without menu overlay");
  }
  return true;
}

/* ------------------------------------------------------------ applications */

static void shell_load_apps(bd_shell *shell, const char *apps_dir) {
  shell->apps = bd_app_list_create();
  if (shell->apps == NULL) {
    return;
  }
  const char *dir = apps_dir != NULL ? apps_dir : bd_app_default_dir();
  char error[256] = {0};
  if (!bd_app_list_scan(shell->apps, dir, NULL, NULL, error,
                        sizeof(error))) {
    fprintf(stderr, "brickdesk: apps: %s\n", error);
    return;
  }
  size_t count = bd_app_list_count(shell->apps);
  const bd_app **apps =
      count > 0 ? (const bd_app **)calloc(count, sizeof(*apps)) : NULL;
  if (count > 0 && apps == NULL) {
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    apps[i] = bd_app_list_at(shell->apps, i);
  }
  bd_desktop_set_apps(shell->desktop, apps, count);
  free(apps);
  if (shell->verbose) {
    fprintf(stderr, "brickdesk: %zu app(s) from %s\n", count, dir);
  }
}

/* ------------------------------------------------------------------- loop */

bool bd_shell_render(bd_shell *shell) {
  if (shell == NULL || !shell->visible) {
    return false;
  }
  bd_gfx_begin_frame(shell->gfx);
  bd_desktop_draw(shell->desktop, shell->gfx);
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
  if (shell->animation.kind != BD_SHELL_ANIMATION_NONE) {
    best = merge_timeout(best, 16);
  }
  if (bd_desktop_has_pending_images(shell->desktop)) {
    best = merge_timeout(best, 16);
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
  shell_update_animation(shell);
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
    shell_update_animation(shell);
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
      config->desktop == NULL || config->settings == NULL) {
    return NULL;
  }
  bd_shell *shell = (bd_shell *)calloc(1, sizeof(*shell));
  if (shell == NULL) {
    return NULL;
  }
  shell->platform = config->platform;
  shell->gfx = config->gfx;
  shell->desktop = config->desktop;
  shell->settings = config->settings;
  shell->plugins = config->plugins;
  shell->verbose = config->verbose;
  shell->inputd_fd = -1;
  shell->visible = true;
  shell->dirty = true;

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
      (int)bd_config_get_int(config->settings, "session.max_background", 2);
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
  if (shell->apps != NULL) {
    bd_app_list_destroy(shell->apps);
  }
  free(shell);
}

bool bd_shell_visible(const bd_shell *shell) {
  return shell != NULL && shell->visible;
}

bool bd_shell_quit_requested(const bd_shell *shell) {
  return shell == NULL || shell->quit;
}

bool bd_shell_has_pending_images(const bd_shell *shell) {
  return shell != NULL && bd_desktop_has_pending_images(shell->desktop);
}

bool bd_shell_hint_visible(const bd_shell *shell) {
  return shell != NULL && bd_desktop_hint_visible(shell->desktop);
}

uint64_t bd_shell_present_count(const bd_shell *shell) {
  return shell != NULL ? shell->presents : 0;
}

const bd_shell_animation *bd_shell_animation_state(const bd_shell *shell) {
  return shell != NULL ? &shell->animation : NULL;
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
  bd_input_state_advance(&shell->test_input, time_ms, local_emit, shell);
}
