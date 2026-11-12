#define _POSIX_C_SOURCE 200809L

#include "brickdesk/plugin.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "ipc.h"
#include "json.h"

struct bd_plugin_client {
  int fd;
  bool ready;
  bool shutdown_requested;
  char error[192];
  char surface[BD_PLUGIN_MAX_PATH];
  char input[BD_PLUGIN_LINE_MAX + 1];
  size_t input_length;
  char event_params[BD_PLUGIN_LINE_MAX + 1];
  bd_plugin_client_event_fn event_fn;
  void *event_user;
};

static uint64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void client_set_error(bd_plugin_client *client, const char *message) {
  if (client != NULL) {
    snprintf(client->error, sizeof(client->error), "%s", message);
  }
}

static bool client_send_line(bd_plugin_client *client, const char *line,
                             size_t length) {
  if (client == NULL || client->fd < 0) {
    return false;
  }
  if (!bd_plugin_send_line(client->fd, line, length)) {
    client_set_error(client, "cannot send to the host");
    return false;
  }
  return true;
}

static bool client_send_format(bd_plugin_client *client, const char *format,
                               ...) {
  char line[BD_PLUGIN_LINE_MAX];
  va_list args;
  va_start(args, format);
  int written = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (written < 0 || (size_t)written >= sizeof(line)) {
    return false;
  }
  return client_send_line(client, line, (size_t)written);
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

static void handle_host_line(bd_plugin_client *client, const char *line,
                             size_t length, int *handled) {
  bd_plugin_json json;
  char error[160] = {0};
  if (!bd_plugin_json_parse(line, length, &json, BD_PLUGIN_JSON_MAX_TOKENS,
                            error, sizeof(error))) {
    client_set_error(client, error);
    return;
  }
  const jsmntok_t *root = &json.tokens[0];
  const jsmntok_t *method = bd_plugin_json_member(&json, root, "method");
  const jsmntok_t *params = bd_plugin_json_member(&json, root, "params");
  const jsmntok_t *id_token = bd_plugin_json_member(&json, root, "id");
  long id = 0;
  if (id_token != NULL) {
    bd_plugin_json_long(&json, id_token, &id);
  }
  char method_name[64] = {0};
  if (method != NULL) {
    bd_plugin_json_string(&json, method, method_name, sizeof(method_name));
  }
  if (strcmp(method_name, "host.hello") == 0) {
    char surface[BD_PLUGIN_MAX_PATH] = {0};
    if (params != NULL) {
      const jsmntok_t *surface_token =
          bd_plugin_json_member(&json, params, "surface");
      if (surface_token != NULL) {
        bd_plugin_json_string(&json, surface_token, surface, sizeof(surface));
      }
    }
    snprintf(client->surface, sizeof(client->surface), "%s", surface);
    client->ready = true;
    client_send_format(
        client,
        "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"pid\":%ld,\"api\":%d,"
        "\"capabilities\":[\"tile\",\"settings\"]}}",
        id, (long)getpid(), BD_PLUGIN_API_VERSION);
    ++*handled;
  } else if (strcmp(method_name, "host.ping") == 0) {
    client_send_format(
        client, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"pong\":true}}",
        id);
    ++*handled;
  } else if (strcmp(method_name, "host.shutdown") == 0) {
    client->shutdown_requested = true;
    ++*handled;
  } else if (strcmp(method_name, "host.event") == 0 ||
             strcmp(method_name, "host.action") == 0) {
    char type[64] = {0};
    client->event_params[0] = '\0';
    if (params != NULL) {
      const jsmntok_t *type_token = bd_plugin_json_member(&json, params, "type");
      if (type_token != NULL) {
        bd_plugin_json_string(&json, type_token, type, sizeof(type));
      }
      size_t span = (size_t)(params->end - params->start);
      if (span < sizeof(client->event_params)) {
        memcpy(client->event_params, json.text + params->start, span);
        client->event_params[span] = '\0';
      } else {
        client->event_params[0] = '\0';
      }
    }
    if (client->event_fn != NULL) {
      client->event_fn(client->event_user, type, client->event_params);
    }
    ++*handled;
  } else if (id != 0) {
    client_send_format(
        client,
        "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"error\":{\"code\":-32601,"
        "\"message\":\"unknown method\"}}",
        id);
  }
  bd_plugin_json_destroy(&json);
}

static int client_read_available(bd_plugin_client *client) {
  int handled = 0;
  char buffer[4096];
  for (;;) {
    ssize_t received = recv(client->fd, buffer, sizeof(buffer), 0);
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return handled;
      }
      client_set_error(client, "host socket read failed");
      return -1;
    }
    if (received == 0) {
      client_set_error(client, "host closed the socket");
      return -1;
    }
    if ((size_t)received > sizeof(client->input) - 1 - client->input_length) {
      client_set_error(client, "host message is too long");
      return -1;
    }
    memcpy(client->input + client->input_length, buffer, (size_t)received);
    client->input_length += (size_t)received;
    client->input[client->input_length] = '\0';
    char *newline = memchr(client->input, '\n', client->input_length);
    while (newline != NULL) {
      size_t line_length = (size_t)(newline - client->input);
      if (line_length > 0 && client->input[line_length - 1] == '\r') {
        --line_length;
      }
      client->input[line_length] = '\0';
      handle_host_line(client, client->input, line_length, &handled);
      size_t consumed = (size_t)(newline - client->input) + 1;
      memmove(client->input, client->input + consumed,
              client->input_length - consumed);
      client->input_length -= consumed;
      newline = memchr(client->input, '\n', client->input_length);
    }
  }
}

bd_plugin_client *bd_plugin_client_connect(const char *socket_path,
                                           int timeout_ms) {
  const char *path = socket_path;
  if (path == NULL || path[0] == '\0') {
    path = getenv("BRICKDESK_PLUGIN_SOCKET");
  }
  if (path == NULL || path[0] == '\0') {
    return NULL;
  }
  bd_plugin_client *client =
      (bd_plugin_client *)calloc(1, sizeof(*client));
  if (client == NULL) {
    return NULL;
  }
  client->fd = -1;
  if (timeout_ms <= 0) {
    timeout_ms = 3000;
  }
  char error[160] = {0};
  client->fd = bd_plugin_connect_socket(path, timeout_ms, error, sizeof(error));
  if (client->fd < 0) {
    client_set_error(client, error);
    return client;
  }
  int flags = fcntl(client->fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(client->fd, F_SETFL, flags | O_NONBLOCK);
  }
  uint64_t deadline = monotonic_ms() + (uint64_t)timeout_ms;
  while (!client->ready && monotonic_ms() < deadline &&
         !client->shutdown_requested) {
    int remaining = (int)(deadline - monotonic_ms());
    int result = bd_plugin_client_poll(client, remaining);
    if (result < 0) {
      break;
    }
  }
  if (!client->ready && client->error[0] == '\0') {
    client_set_error(client, "host handshake timed out");
  }
  return client;
}

void bd_plugin_client_destroy(bd_plugin_client *client) {
  if (client == NULL) {
    return;
  }
  if (client->fd >= 0) {
    close(client->fd);
  }
  free(client);
}

bool bd_plugin_client_ready(const bd_plugin_client *client) {
  return client != NULL && client->ready;
}

const char *bd_plugin_client_surface_path(const bd_plugin_client *client) {
  return client != NULL ? client->surface : "";
}

const char *bd_plugin_client_error(const bd_plugin_client *client) {
  return client != NULL ? client->error : "";
}

bool bd_plugin_client_shutdown_requested(const bd_plugin_client *client) {
  return client != NULL && client->shutdown_requested;
}

void bd_plugin_client_set_event_handler(bd_plugin_client *client,
                                        bd_plugin_client_event_fn fn,
                                        void *user) {
  if (client == NULL) {
    return;
  }
  client->event_fn = fn;
  client->event_user = user;
}

int bd_plugin_client_poll(bd_plugin_client *client, int timeout_ms) {
  if (client == NULL || client->fd < 0) {
    return -1;
  }
  struct pollfd fd;
  fd.fd = client->fd;
  fd.events = POLLIN;
  fd.revents = 0;
  int ready = poll(&fd, 1, timeout_ms);
  if (ready < 0) {
    if (errno == EINTR) {
      return 0;
    }
    client_set_error(client, "poll failed");
    return -1;
  }
  if (ready == 0) {
    return 0;
  }
  if ((fd.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
    return 0;
  }
  return client_read_available(client);
}

bool bd_plugin_client_set_ui(bd_plugin_client *client, const char *tile_json,
                             const char *settings_json) {
  if (client == NULL || client->fd < 0 ||
      (tile_json == NULL && settings_json == NULL)) {
    return false;
  }
  size_t tile_length = tile_json != NULL ? strlen(tile_json) : 0;
  size_t settings_length = settings_json != NULL ? strlen(settings_json) : 0;
  size_t capacity = tile_length + settings_length + 256;
  char *line = (char *)malloc(capacity);
  if (line == NULL) {
    return false;
  }
  size_t used = 0;
  int written = snprintf(line, capacity,
                         "{\"jsonrpc\":\"2.0\",\"method\":\"plugin.ui.set\","
                         "\"params\":{");
  if (written < 0) {
    free(line);
    return false;
  }
  used += (size_t)written;
  if (tile_json != NULL) {
    written = snprintf(line + used, capacity - used, "\"tile\":%s", tile_json);
    if (written < 0) {
      free(line);
      return false;
    }
    used += (size_t)written;
  }
  if (settings_json != NULL) {
    written = snprintf(line + used, capacity - used, "%s\"settings\":%s",
                       tile_json != NULL ? "," : "", settings_json);
    if (written < 0) {
      free(line);
      return false;
    }
    used += (size_t)written;
  }
  written = snprintf(line + used, capacity - used, "}}");
  if (written < 0 || (size_t)written >= capacity - used) {
    free(line);
    return false;
  }
  used += (size_t)written;
  bool ok = client_send_line(client, line, used);
  free(line);
  return ok;
}

bool bd_plugin_client_notify(bd_plugin_client *client, const char *method,
                             const char *params_json) {
  if (client == NULL || client->fd < 0 || method == NULL) {
    return false;
  }
  char escaped[128];
  json_escape(escaped, sizeof(escaped), method);
  char line[BD_PLUGIN_LINE_MAX];
  int written = snprintf(line, sizeof(line),
                         "{\"jsonrpc\":\"2.0\",\"method\":\"%s\","
                         "\"params\":%s}",
                         escaped,
                         params_json != NULL ? params_json : "{}");
  if (written < 0 || (size_t)written >= sizeof(line)) {
    return false;
  }
  return client_send_line(client, line, (size_t)written);
}

bool bd_plugin_client_tile_frame(bd_plugin_client *client, uint32_t seq) {
  return client_send_format(
      client,
      "{\"jsonrpc\":\"2.0\",\"method\":\"plugin.tile.frame\",\"params\":{"
      "\"seq\":%u}}",
      (unsigned)seq);
}

bool bd_plugin_client_log(bd_plugin_client *client, const char *level,
                          const char *message) {
  if (client == NULL || client->fd < 0) {
    return false;
  }
  char escaped_level[32];
  char escaped_message[512];
  json_escape(escaped_level, sizeof(escaped_level), level);
  json_escape(escaped_message, sizeof(escaped_message), message);
  return client_send_format(
      client,
      "{\"jsonrpc\":\"2.0\",\"method\":\"plugin.log\",\"params\":{"
      "\"level\":\"%s\",\"message\":\"%s\"}}",
      escaped_level, escaped_message);
}
