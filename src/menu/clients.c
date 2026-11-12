#define _POSIX_C_SOURCE 200809L

#include "menu_internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef BRICKDESK_ASSET_DIR
#define BRICKDESK_ASSET_DIR "assets"
#endif

/* ------------------------------------------------------------------ utils */

uint64_t bd_menu_now_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

const char *bd_menu_asset_dir(void) {
  const char *value = getenv("BRICKDESK_ASSET_DIR");
  if (value != NULL && value[0] != '\0') {
    return value;
  }
  if (access("/mnt/SDCARD/brickdesk/assets/fonts", R_OK) == 0) {
    return "/mnt/SDCARD/brickdesk/assets";
  }
  return BRICKDESK_ASSET_DIR;
}

static const char *find_field(const char *json, const char *field) {
  char pattern[96];
  int written = snprintf(pattern, sizeof(pattern), "\"%s\"", field);
  if (written <= 0 || (size_t)written >= sizeof(pattern)) {
    return NULL;
  }
  /*
   * A field name can also appear as another field's value (inputd sends
   * "type":"action" before "action":"menu.open"), so keep looking until a
   * quoted token is followed by a colon.
   */
  const char *cursor = json;
  while ((cursor = strstr(cursor, pattern)) != NULL) {
    const char *after = cursor + strlen(pattern);
    while (*after != '\0' && isspace((unsigned char)*after)) {
      ++after;
    }
    if (*after != ':') {
      cursor = after;
      continue;
    }
    ++after;
    while (*after != '\0' && isspace((unsigned char)*after)) {
      ++after;
    }
    return after;
  }
  return NULL;
}

bool bd_menu_json_string(const char *json, const char *field, char *out,
                         size_t out_size) {
  if (json == NULL || field == NULL || out == NULL || out_size == 0) {
    return false;
  }
  const char *cursor = find_field(json, field);
  if (cursor == NULL || *cursor != '"') {
    return false;
  }
  ++cursor;
  size_t used = 0;
  while (*cursor != '\0' && *cursor != '"') {
    if (*cursor == '\\' && cursor[1] != '\0') {
      ++cursor;
    }
    if (used + 1 < out_size) {
      out[used++] = *cursor;
    }
    ++cursor;
  }
  out[used] = '\0';
  return *cursor == '"';
}

bool bd_menu_json_bool(const char *json, const char *field, bool fallback) {
  const char *cursor = find_field(json, field);
  if (cursor == NULL) {
    return fallback;
  }
  if (strncmp(cursor, "true", 4) == 0) {
    return true;
  }
  if (strncmp(cursor, "false", 5) == 0) {
    return false;
  }
  return fallback;
}

static int connect_unix(const char *path, int timeout_ms) {
  if (path == NULL || path[0] == '\0') {
    return -1;
  }
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(address.sun_path)) {
    return -1;
  }
  snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
  (void)timeout_ms;
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  /*
   * Connect while the socket is still blocking. AF_UNIX connect either
   * completes immediately or fails with ENOENT/ECONNREFUSED; the shim then
   * switches to non-blocking reads so a closed menu never blocks a frame.
   */
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    close(fd);
    return -1;
  }
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  return fd;
}

static bool send_line_fd(int fd, const char *line) {
  size_t length = strlen(line);
  char buffer[BD_MENU_INPUT_LINE + 2];
  if (length + 1 >= sizeof(buffer)) {
    return false;
  }
  memcpy(buffer, line, length);
  buffer[length] = '\n';
  size_t sent = 0;
  while (sent < length + 1) {
    ssize_t amount = send(fd, buffer + sent, length + 1 - sent, MSG_NOSIGNAL);
    if (amount < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        struct pollfd poll_fd;
        poll_fd.fd = fd;
        poll_fd.events = POLLOUT;
        poll_fd.revents = 0;
        int ready;
        do {
          ready = poll(&poll_fd, 1, 100);
        } while (ready < 0 && errno == EINTR);
        if (ready > 0) {
          continue;
        }
      }
      if (getenv("BRICKDESK_MENU_DEBUG") != NULL) {
        fprintf(stderr, "brickdesk-menu: send: %s\n", strerror(errno));
      }
      return false;
    }
    sent += (size_t)amount;
  }
  return true;
}

/* ------------------------------------------------------------ input client */

void bd_menu_input_init(bd_menu_input *input) {
  memset(input, 0, sizeof(*input));
  input->fd = -1;
}

void bd_menu_input_close(bd_menu_input *input) {
  if (input->fd >= 0) {
    close(input->fd);
  }
  input->fd = -1;
  input->length = 0;
  input->grab_active = false;
  input->grab_pending = false;
}

bool bd_menu_input_ensure(bd_menu_input *input, uint64_t now_ms) {
  if (input->fd >= 0) {
    return true;
  }
  if (input->last_connect_ms != 0 &&
      now_ms - input->last_connect_ms < 1000) {
    return false;
  }
  input->last_connect_ms = now_ms;
  const char *environment = getenv("BRICKDESK_INPUT_SOCKET");
  const char *paths[3];
  size_t count = 0;
  if (environment != NULL && environment[0] != '\0') {
    paths[count++] = environment;
  }
  paths[count++] = "/run/brickdesk/inputd.sock";
  paths[count++] = "/tmp/brickdesk-inputd.sock";
  for (size_t i = 0; i < count; ++i) {
    int fd = connect_unix(paths[i], 100);
    if (fd < 0) {
      continue;
    }
    input->fd = fd;
    input->length = 0;
    if (!send_line_fd(fd, "{\"cmd\":\"subscribe\",\"actions\":true,\"keys\":true}")) {
      if (getenv("BRICKDESK_MENU_DEBUG") != NULL) {
        fprintf(stderr, "brickdesk-menu: subscribe send failed\n");
      }
      bd_menu_input_close(input);
      return false;
    }
    if (getenv("BRICKDESK_MENU_DEBUG") != NULL) {
      fprintf(stderr, "brickdesk-menu: inputd connected %s at %llu\n", paths[i],
              (unsigned long long)now_ms);
    }
    return true;
  }
  return false;
}

static int take_line(bd_menu_input *input, char *line, size_t line_size) {
  char *newline = memchr(input->buffer, '\n', input->length);
  if (newline == NULL) {
    return 0;
  }
  size_t length = (size_t)(newline - input->buffer);
  size_t copy = length < line_size - 1 ? length : line_size - 1;
  memcpy(line, input->buffer, copy);
  line[copy] = '\0';
  memmove(input->buffer, newline + 1, input->length - length - 1);
  input->length -= length + 1;
  return 1;
}

int bd_menu_input_read(bd_menu_input *input, int timeout_ms, char *line,
                       size_t line_size) {
  if (input->fd < 0 || line == NULL || line_size == 0) {
    return -1;
  }
  if (take_line(input, line, line_size) == 1) {
    return 1;
  }
  struct pollfd poll_fd;
  poll_fd.fd = input->fd;
  poll_fd.events = POLLIN;
  poll_fd.revents = 0;
  int result;
  do {
    result = poll(&poll_fd, 1, timeout_ms);
  } while (result < 0 && errno == EINTR);
  if (result == 0) {
    return 0;
  }
  if (result < 0 || (poll_fd.revents & (POLLERR | POLLNVAL)) != 0) {
    if (getenv("BRICKDESK_MENU_DEBUG") != NULL) {
      fprintf(stderr, "brickdesk-menu: input poll revents=%x errno=%s\n",
              (unsigned)poll_fd.revents, strerror(errno));
    }
    bd_menu_input_close(input);
    return -1;
  }
  for (;;) {
    size_t room = sizeof(input->buffer) - input->length;
    if (room == 0) {
      input->length = 0;
      room = sizeof(input->buffer);
    }
    ssize_t amount = recv(input->fd, input->buffer + input->length, room, 0);
    if (amount < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return take_line(input, line, line_size);
      }
      if (getenv("BRICKDESK_MENU_DEBUG") != NULL) {
        fprintf(stderr, "brickdesk-menu: input recv: %s\n", strerror(errno));
      }
      bd_menu_input_close(input);
      return -1;
    }
    if (amount == 0) {
      if (getenv("BRICKDESK_MENU_DEBUG") != NULL) {
        fprintf(stderr, "brickdesk-menu: input EOF\n");
      }
      bd_menu_input_close(input);
      return -1;
    }
    input->length += (size_t)amount;
    if (take_line(input, line, line_size) == 1) {
      return 1;
    }
    if ((size_t)amount < room) {
      return 0;
    }
  }
}

bool bd_menu_input_send(bd_menu_input *input, const char *line) {
  if (input->fd < 0) {
    return false;
  }
  return send_line_fd(input->fd, line);
}

bool bd_menu_input_grab(bd_menu_input *input, int timeout_ms) {
  if (!bd_menu_input_send(input, "{\"cmd\":\"grab\"}")) {
    return false;
  }
  uint64_t deadline = bd_menu_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 150);
  for (;;) {
    uint64_t now = bd_menu_now_ms();
    if (now >= deadline) {
      return false;
    }
    char line[BD_MENU_INPUT_LINE];
    int result = bd_menu_input_read(input, (int)(deadline - now), line,
                                    sizeof(line));
    if (result < 0) {
      return false;
    }
    if (result == 0) {
      continue;
    }
    char type[32];
    if (!bd_menu_json_string(line, "type", type, sizeof(type))) {
      continue;
    }
    if (strcmp(type, "grab") == 0) {
      input->grab_active = bd_menu_json_bool(line, "active", false);
      input->grab_pending = bd_menu_json_bool(line, "pending", false);
      if (input->grab_active) {
        return true;
      }
    } else if (strcmp(type, "error") == 0) {
      char command[32];
      if (bd_menu_json_string(line, "cmd", command, sizeof(command)) &&
          strcmp(command, "grab") == 0) {
        return false;
      }
    }
  }
}

void bd_menu_input_release_all(bd_menu_input *input) {
  bd_menu_input_send(input, "{\"cmd\":\"release_all\"}");
}

void bd_menu_input_ungrab(bd_menu_input *input) {
  bd_menu_input_send(input, "{\"cmd\":\"ungrab\"}");
}

/* ------------------------------------------------------------- perf client */

void bd_menu_perf_init(bd_menu_perf *perf) {
  memset(perf, 0, sizeof(*perf));
  perf->fd = -1;
  perf->next_id = 1;
}

void bd_menu_perf_close(bd_menu_perf *perf) {
  if (perf->fd >= 0) {
    close(perf->fd);
  }
  perf->fd = -1;
  perf->length = 0;
}

static int perf_take_line(bd_menu_perf *perf, char *line, size_t line_size) {
  char *newline = memchr(perf->buffer, '\n', perf->length);
  if (newline == NULL) {
    return 0;
  }
  size_t length = (size_t)(newline - perf->buffer);
  size_t copy = length < line_size - 1 ? length : line_size - 1;
  memcpy(line, perf->buffer, copy);
  line[copy] = '\0';
  memmove(perf->buffer, newline + 1, perf->length - length - 1);
  perf->length -= length + 1;
  return 1;
}

static int perf_read_line(bd_menu_perf *perf, int timeout_ms, char *line,
                          size_t line_size) {
  if (perf->fd < 0) {
    return -1;
  }
  if (perf_take_line(perf, line, line_size) == 1) {
    return 1;
  }
  struct pollfd poll_fd;
  poll_fd.fd = perf->fd;
  poll_fd.events = POLLIN;
  poll_fd.revents = 0;
  int result;
  do {
    result = poll(&poll_fd, 1, timeout_ms);
  } while (result < 0 && errno == EINTR);
  if (result == 0) {
    return 0;
  }
  if (result < 0) {
    bd_menu_perf_close(perf);
    return -1;
  }
  for (;;) {
    size_t room = sizeof(perf->buffer) - perf->length;
    if (room == 0) {
      perf->length = 0;
      room = sizeof(perf->buffer);
    }
    ssize_t amount = recv(perf->fd, perf->buffer + perf->length, room, 0);
    if (amount < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return perf_take_line(perf, line, line_size);
      }
      bd_menu_perf_close(perf);
      return -1;
    }
    if (amount == 0) {
      bd_menu_perf_close(perf);
      return -1;
    }
    perf->length += (size_t)amount;
    if (perf_take_line(perf, line, line_size) == 1) {
      return 1;
    }
    if ((size_t)amount < room) {
      return 0;
    }
  }
}

/*
 * Sends one JSON-RPC request and waits for the matching id. Returns 1 on a
 * result, 0 on an error response, -1 on timeout or a closed socket.
 */
static int perf_request(bd_menu_perf *perf, const char *method,
                        const char *params, char *response,
                        size_t response_size, int timeout_ms) {
  if (perf->fd < 0) {
    return -1;
  }
  int id = perf->next_id++;
  char line[1024];
  int written = snprintf(
      line, sizeof(line),
      "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}", id,
      method, params != NULL ? params : "{}");
  if (written <= 0 || (size_t)written >= sizeof(line) ||
      !send_line_fd(perf->fd, line)) {
    return -1;
  }
  char pattern[32];
  snprintf(pattern, sizeof(pattern), "\"id\":%d", id);
  uint64_t deadline = bd_menu_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 250);
  for (;;) {
    uint64_t now = bd_menu_now_ms();
    if (now >= deadline) {
      return -1;
    }
    char incoming[BD_MENU_PERF_LINE];
    int result =
        perf_read_line(perf, (int)(deadline - now), incoming, sizeof(incoming));
    if (result <= 0) {
      if (result < 0) {
        return -1;
      }
      continue;
    }
    if (strstr(incoming, pattern) == NULL) {
      continue;
    }
    if (response != NULL && response_size > 0) {
      snprintf(response, response_size, "%s", incoming);
    }
    return strstr(incoming, "\"error\"") != NULL ? 0 : 1;
  }
}

static void perf_parse_profiles(const char *json, bd_menu_model *model) {
  model->profile_count = 0;
  const char *cursor = strstr(json, "\"profiles\"");
  if (cursor == NULL) {
    return;
  }
  cursor = strchr(cursor, '[');
  if (cursor == NULL) {
    return;
  }
  ++cursor;
  while (*cursor != '\0' && *cursor != ']' &&
         model->profile_count < BD_MENU_MAX_PROFILES) {
    while (*cursor != '\0' &&
           (*cursor == ',' || isspace((unsigned char)*cursor))) {
      ++cursor;
    }
    if (*cursor != '"') {
      break;
    }
    ++cursor;
    size_t used = 0;
    while (*cursor != '\0' && *cursor != '"') {
      if (*cursor == '\\' && cursor[1] != '\0') {
        ++cursor;
      }
      if (used + 1 < BD_MENU_PROFILE_NAME) {
        model->profiles[model->profile_count][used++] = *cursor;
      }
      ++cursor;
    }
    model->profiles[model->profile_count][used] = '\0';
    if (*cursor == '"') {
      ++cursor;
    }
    if (used > 0) {
      ++model->profile_count;
    }
  }
}

bool bd_menu_perf_open(bd_menu_perf *perf, const char *app_id,
                       bd_menu_model *model) {
  model->profile_count = 0;
  model->profile_index = 0;
  model->perf_available = false;
  if (perf->fd < 0) {
    const char *environment = getenv("BRICKDESK_PERF_SOCKET");
    const char *paths[3];
    size_t count = 0;
    if (environment != NULL && environment[0] != '\0') {
      paths[count++] = environment;
    }
    paths[count++] = "/run/brickdesk/perfd.sock";
    paths[count++] = "/tmp/brickdesk-perfd.sock";
    for (size_t i = 0; i < count; ++i) {
      int fd = connect_unix(paths[i], 100);
      if (fd >= 0) {
        perf->fd = fd;
        perf->length = 0;
        perf->next_id = 1;
        break;
      }
    }
  }
  if (perf->fd < 0) {
    return false;
  }
  char response[BD_MENU_PERF_LINE];
  if (perf_request(perf, "perf.list", "{}", response, sizeof(response), 250) !=
      1) {
    bd_menu_perf_close(perf);
    return false;
  }
  perf_parse_profiles(response, model);
  if (model->profile_count == 0) {
    return false;
  }
  char params[512];
  snprintf(params, sizeof(params), "{\"app\":\"%s\"}",
           app_id != NULL ? app_id : "");
  char current[BD_MENU_PERF_LINE];
  if (perf_request(perf, "perf.get", params, current, sizeof(current), 250) ==
      1) {
    char profile[BD_MENU_PROFILE_NAME];
    if (bd_menu_json_string(current, "profile", profile, sizeof(profile))) {
      bool found = false;
      for (int i = 0; i < model->profile_count; ++i) {
        if (strcmp(model->profiles[i], profile) == 0) {
          model->profile_index = i;
          found = true;
          break;
        }
      }
      if (!found && model->profile_count < BD_MENU_MAX_PROFILES) {
        snprintf(model->profiles[model->profile_count],
                 sizeof(model->profiles[0]), "%s", profile);
        model->profile_index = model->profile_count;
        ++model->profile_count;
      }
    }
  }
  model->perf_available = true;
  return true;
}

bool bd_menu_perf_apply(bd_menu_perf *perf, const char *app_id,
                        const char *profile) {
  if (perf->fd < 0 || profile == NULL) {
    return false;
  }
  char params[512];
  snprintf(params, sizeof(params),
           "{\"session\":\"menu\",\"profile\":\"%s\",\"app\":\"%s\"}", profile,
           app_id != NULL ? app_id : "");
  char response[BD_MENU_PERF_LINE];
  return perf_request(perf, "perf.apply", params, response, sizeof(response),
                      500) == 1;
}

/* ---------------------------------------------------------- session client */

bool bd_menu_session_send(const char *action) {
  const char *path = getenv("BRICKDESK_MENU_SOCKET");
  const char *app = getenv("BRICKDESK_APP_ID");
  if (path == NULL || path[0] == '\0' || action == NULL) {
    return false;
  }
  int fd = connect_unix(path, 100);
  if (fd < 0) {
    return false;
  }
  char line[512];
  snprintf(line, sizeof(line),
           "{\"type\":\"menu\",\"app\":\"%s\",\"action\":\"%s\"}",
           app != NULL ? app : "", action);
  bool ok = send_line_fd(fd, line);
  close(fd);
  return ok;
}
