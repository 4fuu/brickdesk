#define _POSIX_C_SOURCE 200809L

#include "app_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "brickdesk/app.h"
#include "json_util.h"

typedef struct pending_request {
  bool used;
  int id;
  char method[BD_APP_METHOD_MAX];
  uint64_t deadline_ms;
} pending_request;

struct bd_app_protocol {
  int listen_fd;
  int client_fd;
  bool connected;
  char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  char session_id[64];
  char menu_mode[32];
  char state_dir[BD_APP_PATH_MAX];

  char input[BD_APP_PROTOCOL_MAX_LINE];
  size_t input_length;
  char output[BD_APP_PROTOCOL_MAX_LINE];
  size_t output_length;

  pending_request pending[BD_APP_PROTOCOL_MAX_PENDING];
  int next_id;
  uint64_t now_ms;

  void (*on_event)(void *user, const bd_app_protocol_event *event);
  void *user;
};

static uint64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
}

static void json_escape(char *dst, size_t dst_size, const char *src) {
  if (dst_size == 0) {
    return;
  }
  size_t used = 0;
  if (src == NULL) {
    src = "";
  }
  for (const unsigned char *p = (const unsigned char *)src;
       *p != '\0' && used + 1 < dst_size; ++p) {
    if (*p == '"' || *p == '\\') {
      if (used + 2 >= dst_size) {
        break;
      }
      dst[used++] = '\\';
      dst[used++] = (char)*p;
    } else if (*p < 0x20) {
      if (used + 6 >= dst_size) {
        break;
      }
      int written = snprintf(dst + used, dst_size - used, "\\u%04x", *p);
      if (written < 0) {
        break;
      }
      used += (size_t)written;
    } else {
      dst[used++] = (char)*p;
    }
  }
  dst[used] = '\0';
}

static void emit_event(bd_app_protocol *protocol,
                       const bd_app_protocol_event *event) {
  if (protocol->on_event != NULL) {
    protocol->on_event(protocol->user, event);
  }
}

static void emit_simple(bd_app_protocol *protocol,
                        bd_app_protocol_event_kind kind, const char *message) {
  bd_app_protocol_event event;
  memset(&event, 0, sizeof(event));
  event.kind = kind;
  if (message != NULL) {
    snprintf(event.error_message, sizeof(event.error_message), "%s", message);
  }
  emit_event(protocol, &event);
}

static bool queue_line(bd_app_protocol *protocol, const char *line) {
  size_t length = strlen(line);
  if (length + 1 > sizeof(protocol->output) - protocol->output_length) {
    return false;
  }
  memcpy(protocol->output + protocol->output_length, line, length);
  protocol->output_length += length;
  protocol->output[protocol->output_length++] = '\n';
  return true;
}

static void close_client(bd_app_protocol *protocol, bool notify) {
  if (protocol->client_fd >= 0) {
    close(protocol->client_fd);
    protocol->client_fd = -1;
  }
  bool was_connected = protocol->connected;
  protocol->connected = false;
  protocol->input_length = 0;
  protocol->output_length = 0;
  memset(protocol->pending, 0, sizeof(protocol->pending));
  if (notify && was_connected) {
    emit_simple(protocol, BD_APP_PROTOCOL_EVENT_DISCONNECTED,
                "application disconnected");
  }
}

static bool flush_output(bd_app_protocol *protocol) {
  while (protocol->output_length > 0 && protocol->client_fd >= 0) {
    ssize_t sent = send(protocol->client_fd, protocol->output,
                        protocol->output_length, MSG_NOSIGNAL);
    if (sent < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return true;
      }
      close_client(protocol, true);
      return false;
    }
    if (sent == 0) {
      return true;
    }
    size_t amount = (size_t)sent;
    memmove(protocol->output, protocol->output + amount,
            protocol->output_length - amount);
    protocol->output_length -= amount;
  }
  return true;
}

static pending_request *find_pending(bd_app_protocol *protocol, int id) {
  for (size_t i = 0; i < BD_APP_PROTOCOL_MAX_PENDING; ++i) {
    if (protocol->pending[i].used && protocol->pending[i].id == id) {
      return &protocol->pending[i];
    }
  }
  return NULL;
}

static unsigned parse_capabilities(const bd_json *doc, int token) {
  unsigned caps = 0;
  int count = bd_json_array_size(doc, token);
  if (count < 0) {
    return 0;
  }
  for (int i = 0; i < count; ++i) {
    int value = bd_json_array_at(doc, token, i);
    char name[32];
    if (value < 0 || !bd_json_string(doc, value, name, sizeof(name), NULL)) {
      continue;
    }
    if (strcmp(name, "pause") == 0) {
      caps |= BD_APP_PROTOCOL_CAP_PAUSE;
    } else if (strcmp(name, "resume") == 0) {
      caps |= BD_APP_PROTOCOL_CAP_RESUME;
    } else if (strcmp(name, "save_state") == 0) {
      caps |= BD_APP_PROTOCOL_CAP_SAVE_STATE;
    } else if (strcmp(name, "load_state") == 0) {
      caps |= BD_APP_PROTOCOL_CAP_LOAD_STATE;
    } else if (strcmp(name, "screenshot") == 0) {
      caps |= BD_APP_PROTOCOL_CAP_SCREENSHOT;
    } else if (strcmp(name, "background") == 0) {
      caps |= BD_APP_PROTOCOL_CAP_BACKGROUND;
    } else if (strcmp(name, "display_release") == 0) {
      caps |= BD_APP_PROTOCOL_CAP_DISPLAY_RELEASE;
    }
  }
  return caps;
}

static void hello_reply(bd_app_protocol *protocol, int id) {
  char session[128];
  char menu[64];
  char state[BD_APP_PATH_MAX * 2];
  json_escape(session, sizeof(session), protocol->session_id);
  json_escape(menu, sizeof(menu), protocol->menu_mode);
  json_escape(state, sizeof(state), protocol->state_dir);
  char line[BD_APP_PATH_MAX * 2 + 256];
  int written = snprintf(
      line, sizeof(line),
      "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"session_id\":\"%s\","
      "\"menu_mode\":\"%s\",\"state_dir\":\"%s\"}}",
      id, session, menu, state);
  if (written > 0 && (size_t)written < sizeof(line)) {
    queue_line(protocol, line);
  }
}

static void handle_hello(bd_app_protocol *protocol, const bd_json *doc,
                         int root, int id, int params_token) {
  bd_app_protocol_event event;
  memset(&event, 0, sizeof(event));
  event.kind = BD_APP_PROTOCOL_EVENT_HELLO;
  event.id = id;
  event.protocol = 1;

  bool params_ok = params_token >= 0 && bd_json_is_object(doc, params_token);
  if (params_ok) {
    int protocol_token = bd_json_object_get(doc, params_token, "protocol");
    if (protocol_token >= 0) {
      long value = 0;
      if (bd_json_int(doc, protocol_token, &value)) {
        event.protocol = (int)value;
      }
    }
    int pid_token = bd_json_object_get(doc, params_token, "pid");
    if (pid_token >= 0) {
      long value = 0;
      if (bd_json_int(doc, pid_token, &value)) {
        event.pid = (int)value;
      }
    }
    int caps_token = bd_json_object_get(doc, params_token, "capabilities");
    if (caps_token >= 0) {
      event.capabilities = parse_capabilities(doc, caps_token);
    }
  } else {
    event.protocol = 0;
  }

  if (event.protocol != 1) {
    if (id >= 0) {
      bd_app_protocol_reply_error(protocol, id, BD_APP_PROTOCOL_ERROR_PARAMS,
                                  "unsupported protocol version");
    }
    emit_event(protocol, &event);
    return;
  }

  emit_event(protocol, &event);
  if (id >= 0) {
    hello_reply(protocol, id);
  }
  (void)root;
}

static void handle_response(bd_app_protocol *protocol, const bd_json *doc,
                            int root, int id) {
  pending_request *pending = find_pending(protocol, id);
  if (pending == NULL) {
    return;
  }
  bd_app_protocol_event event;
  memset(&event, 0, sizeof(event));
  event.kind = BD_APP_PROTOCOL_EVENT_RESPONSE;
  event.id = id;
  snprintf(event.method, sizeof(event.method), "%s", pending->method);
  event.ok = true;

  int result = bd_json_object_get(doc, root, "result");
  if (result >= 0) {
    bd_json_raw(doc, result, event.result, sizeof(event.result), NULL);
  }
  int error = bd_json_object_get(doc, root, "error");
  if (error >= 0) {
    event.ok = false;
    if (bd_json_is_object(doc, error)) {
      int code = bd_json_object_get(doc, error, "code");
      if (code >= 0) {
        long value = 0;
        if (bd_json_int(doc, code, &value)) {
          event.error_code = (int)value;
        }
      }
      int message = bd_json_object_get(doc, error, "message");
      if (message >= 0) {
        bd_json_string(doc, message, event.error_message,
                       sizeof(event.error_message), NULL);
      }
    }
  }
  pending->used = false;
  emit_event(protocol, &event);
}

static void handle_request(bd_app_protocol *protocol, const bd_json *doc,
                           int root, int id, const char *method,
                           int params_token) {
  bd_app_protocol_event event;
  memset(&event, 0, sizeof(event));
  event.kind = id >= 0 ? BD_APP_PROTOCOL_EVENT_REQUEST
                       : BD_APP_PROTOCOL_EVENT_NOTIFICATION;
  event.id = id;
  snprintf(event.method, sizeof(event.method), "%s", method);
  if (params_token >= 0) {
    bd_json_raw(doc, params_token, event.params, sizeof(event.params), NULL);
  } else {
    snprintf(event.params, sizeof(event.params), "%s", "{}");
  }
  emit_event(protocol, &event);
  (void)root;
}

static void handle_line(bd_app_protocol *protocol, const char *line) {
  if (line[0] == '\0') {
    return;
  }
  bd_json doc;
  char error[128] = {0};
  if (!bd_json_parse(&doc, line, strlen(line), error, sizeof(error))) {
    emit_simple(protocol, BD_APP_PROTOCOL_EVENT_ERROR, error);
    return;
  }
  int root = bd_json_root(&doc);
  int method_token = bd_json_object_get(&doc, root, "method");
  int id_token = bd_json_object_get(&doc, root, "id");
  long id_value = -1;
  bool has_id = false;
  if (id_token >= 0 && bd_json_int(&doc, id_token, &id_value) &&
      id_value >= 0 && id_value <= 2147483647L) {
    has_id = true;
  }

  if (method_token >= 0) {
    char method[BD_APP_METHOD_MAX];
    if (!bd_json_string(&doc, method_token, method, sizeof(method), NULL) ||
        method[0] == '\0') {
      emit_simple(protocol, BD_APP_PROTOCOL_EVENT_ERROR,
                  "request method must be a string");
      bd_json_free(&doc);
      return;
    }
    int params = bd_json_object_get(&doc, root, "params");
    if (strcmp(method, "app.hello") == 0) {
      handle_hello(protocol, &doc, root, has_id ? (int)id_value : -1, params);
    } else {
      handle_request(protocol, &doc, root, has_id ? (int)id_value : -1,
                     method, params);
    }
  } else if (has_id) {
    handle_response(protocol, &doc, root, (int)id_value);
  } else {
    emit_simple(protocol, BD_APP_PROTOCOL_EVENT_ERROR,
                "message has neither method nor id");
  }
  bd_json_free(&doc);
}

static void process_input(bd_app_protocol *protocol) {
  for (;;) {
    char *newline = (char *)memchr(protocol->input, '\n',
                                   protocol->input_length);
    if (newline == NULL) {
      return;
    }
    size_t line_length = (size_t)(newline - protocol->input);
    protocol->input[line_length] = '\0';
    handle_line(protocol, protocol->input);
    size_t consumed = line_length + 1;
    memmove(protocol->input, protocol->input + consumed,
            protocol->input_length - consumed);
    protocol->input_length -= consumed;
  }
}

static void read_client(bd_app_protocol *protocol) {
  char buffer[4096];
  for (;;) {
    ssize_t bytes = recv(protocol->client_fd, buffer, sizeof(buffer), 0);
    if (bytes < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return;
      }
      close_client(protocol, true);
      return;
    }
    if (bytes == 0) {
      close_client(protocol, true);
      return;
    }
    size_t copied = 0;
    while (copied < (size_t)bytes) {
      size_t room = sizeof(protocol->input) - protocol->input_length;
      if (room == 0) {
        emit_simple(protocol, BD_APP_PROTOCOL_EVENT_ERROR,
                    "application line is too long");
        close_client(protocol, true);
        return;
      }
      size_t chunk = (size_t)bytes - copied;
      if (chunk > room) {
        chunk = room;
      }
      memcpy(protocol->input + protocol->input_length, buffer + copied, chunk);
      protocol->input_length += chunk;
      copied += chunk;
      process_input(protocol);
      if (protocol->input_length == sizeof(protocol->input)) {
        emit_simple(protocol, BD_APP_PROTOCOL_EVENT_ERROR,
                    "application line is too long");
        close_client(protocol, true);
        return;
      }
    }
  }
}

static void accept_client(bd_app_protocol *protocol) {
  if (protocol->listen_fd < 0) {
    return;
  }
  for (;;) {
    int fd = accept(protocol->listen_fd, NULL, NULL);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return;
      }
      return;
    }
    if (protocol->client_fd >= 0) {
      close(fd);
      continue;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    int fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags >= 0) {
      fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    }
    protocol->client_fd = fd;
    protocol->connected = true;
    protocol->input_length = 0;
    protocol->output_length = 0;
    memset(protocol->pending, 0, sizeof(protocol->pending));
    return;
  }
}

static void expire_pending(bd_app_protocol *protocol) {
  for (size_t i = 0; i < BD_APP_PROTOCOL_MAX_PENDING; ++i) {
    pending_request *pending = &protocol->pending[i];
    if (!pending->used || pending->deadline_ms > protocol->now_ms) {
      continue;
    }
    bd_app_protocol_event event;
    memset(&event, 0, sizeof(event));
    event.kind = BD_APP_PROTOCOL_EVENT_TIMEOUT;
    event.id = pending->id;
    snprintf(event.method, sizeof(event.method), "%s", pending->method);
    event.error_code = BD_APP_PROTOCOL_ERROR_TIMEOUT;
    snprintf(event.error_message, sizeof(event.error_message), "%s",
             "request timed out");
    pending->used = false;
    emit_event(protocol, &event);
  }
}

int bd_app_protocol_request(bd_app_protocol *protocol, const char *method,
                            const char *params_json, int timeout_ms) {
  if (protocol == NULL || method == NULL || method[0] == '\0' ||
      !protocol->connected || protocol->client_fd < 0) {
    return -1;
  }
  pending_request *slot = NULL;
  for (size_t i = 0; i < BD_APP_PROTOCOL_MAX_PENDING; ++i) {
    if (!protocol->pending[i].used) {
      slot = &protocol->pending[i];
      break;
    }
  }
  if (slot == NULL) {
    return -1;
  }

  protocol->now_ms = monotonic_ms();
  int id = protocol->next_id++;
  if (protocol->next_id <= 0) {
    protocol->next_id = 1;
  }
  char escaped_method[BD_APP_METHOD_MAX * 2 + 2];
  json_escape(escaped_method, sizeof(escaped_method), method);
  char line[BD_APP_PROTOCOL_EVENT_TEXT + 256];
  int written = snprintf(line, sizeof(line),
                         "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\","
                         "\"params\":%s}",
                         id, escaped_method,
                         params_json != NULL ? params_json : "{}");
  if (written <= 0 || (size_t)written >= sizeof(line) ||
      !queue_line(protocol, line)) {
    return -1;
  }
  slot->used = true;
  slot->id = id;
  snprintf(slot->method, sizeof(slot->method), "%s", method);
  int timeout = timeout_ms > 0 ? timeout_ms : 1000;
  slot->deadline_ms = protocol->now_ms + (uint64_t)timeout;
  flush_output(protocol);
  return id;
}

bool bd_app_protocol_reply(bd_app_protocol *protocol, int id,
                           const char *result_json) {
  if (protocol == NULL || id < 0 || !protocol->connected) {
    return false;
  }
  char line[BD_APP_PROTOCOL_EVENT_TEXT + 256];
  int written = snprintf(line, sizeof(line),
                         "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}", id,
                         result_json != NULL ? result_json : "{}");
  if (written <= 0 || (size_t)written >= sizeof(line) ||
      !queue_line(protocol, line)) {
    return false;
  }
  flush_output(protocol);
  return true;
}

bool bd_app_protocol_reply_error(bd_app_protocol *protocol, int id, int code,
                                 const char *message) {
  if (protocol == NULL || id < 0 || !protocol->connected) {
    return false;
  }
  char escaped[512];
  json_escape(escaped, sizeof(escaped),
              message != NULL ? message : "error");
  char line[1024];
  int written = snprintf(
      line, sizeof(line),
      "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":%d,\"message\":"
      "\"%s\"}}",
      id, code, escaped);
  if (written <= 0 || (size_t)written >= sizeof(line) ||
      !queue_line(protocol, line)) {
    return false;
  }
  flush_output(protocol);
  return true;
}

void bd_app_protocol_poll(bd_app_protocol *protocol, uint64_t now_ms) {
  if (protocol == NULL) {
    return;
  }
  protocol->now_ms = now_ms != 0 ? now_ms : monotonic_ms();
  accept_client(protocol);
  if (protocol->client_fd >= 0) {
    read_client(protocol);
  }
  expire_pending(protocol);
  if (protocol->client_fd >= 0) {
    flush_output(protocol);
  }
}

int bd_app_protocol_next_timeout(const bd_app_protocol *protocol,
                                 uint64_t now_ms) {
  if (protocol == NULL) {
    return -1;
  }
  uint64_t now = now_ms != 0 ? now_ms : monotonic_ms();
  uint64_t earliest = 0;
  bool found = false;
  for (size_t i = 0; i < BD_APP_PROTOCOL_MAX_PENDING; ++i) {
    const pending_request *pending = &protocol->pending[i];
    if (!pending->used) {
      continue;
    }
    if (!found || pending->deadline_ms < earliest) {
      earliest = pending->deadline_ms;
      found = true;
    }
  }
  if (!found) {
    return -1;
  }
  if (earliest <= now) {
    return 0;
  }
  uint64_t remaining = earliest - now;
  return remaining > 2147483647u ? 2147483647 : (int)remaining;
}

bool bd_app_protocol_connected(const bd_app_protocol *protocol) {
  return protocol != NULL && protocol->connected;
}

const char *bd_app_protocol_socket_path(const bd_app_protocol *protocol) {
  return protocol != NULL ? protocol->path : NULL;
}

int bd_app_protocol_fds(const bd_app_protocol *protocol, int *out, int max) {
  if (protocol == NULL || out == NULL || max <= 0) {
    return 0;
  }
  int count = 0;
  if (protocol->listen_fd >= 0 && count < max) {
    out[count++] = protocol->listen_fd;
  }
  if (protocol->client_fd >= 0 && count < max) {
    out[count++] = protocol->client_fd;
  }
  return count;
}

static bool create_listen_socket(bd_app_protocol *protocol,
                                 const char *path, char *error,
                                 size_t error_size) {
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  if (strlen(path) >= sizeof(address.sun_path)) {
    set_error(error, error_size, "session socket path is too long: %s", path);
    return false;
  }
  address.sun_family = AF_UNIX;
  snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
  unlink(path);

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    set_error(error, error_size, "session socket: %s", strerror(errno));
    return false;
  }
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    set_error(error, error_size, "bind %s: %s", path, strerror(errno));
    close(fd);
    return false;
  }
  if (listen(fd, 4) != 0) {
    set_error(error, error_size, "listen %s: %s", path, strerror(errno));
    close(fd);
    unlink(path);
    return false;
  }
  chmod(path, 0660);
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  protocol->listen_fd = fd;
  snprintf(protocol->path, sizeof(protocol->path), "%s", path);
  return true;
}

bd_app_protocol *bd_app_protocol_create(const bd_app_protocol_config *config,
                                        char *error, size_t error_size) {
  if (config == NULL || config->socket_path == NULL ||
      config->socket_path[0] == '\0') {
    set_error(error, error_size, "invalid app protocol configuration");
    return NULL;
  }
  bd_app_protocol *protocol =
      (bd_app_protocol *)calloc(1, sizeof(bd_app_protocol));
  if (protocol == NULL) {
    set_error(error, error_size, "out of memory creating app protocol");
    return NULL;
  }
  protocol->listen_fd = -1;
  protocol->client_fd = -1;
  protocol->next_id = 1;
  protocol->now_ms = monotonic_ms();
  snprintf(protocol->session_id, sizeof(protocol->session_id), "%s",
           config->session_id != NULL ? config->session_id : "");
  snprintf(protocol->menu_mode, sizeof(protocol->menu_mode), "%s",
           config->menu_mode != NULL ? config->menu_mode : "overlay");
  snprintf(protocol->state_dir, sizeof(protocol->state_dir), "%s",
           config->state_dir != NULL ? config->state_dir : "");
  protocol->on_event = config->on_event;
  protocol->user = config->user;

  if (!create_listen_socket(protocol, config->socket_path, error,
                            error_size)) {
    free(protocol);
    return NULL;
  }
  return protocol;
}

void bd_app_protocol_destroy(bd_app_protocol *protocol) {
  if (protocol == NULL) {
    return;
  }
  close_client(protocol, false);
  if (protocol->listen_fd >= 0) {
    close(protocol->listen_fd);
    protocol->listen_fd = -1;
  }
  if (protocol->path[0] != '\0') {
    unlink(protocol->path);
  }
  free(protocol);
}
