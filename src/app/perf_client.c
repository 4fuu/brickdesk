#define _POSIX_C_SOURCE 200809L

#include "brickdesk/perf_client.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define BD_PERF_CLIENT_LINE_MAX 4096

const char *bd_perf_client_status_name(bd_perf_client_status status) {
  switch (status) {
    case BD_PERF_CLIENT_OK:
      return "ok";
    case BD_PERF_CLIENT_UNAVAILABLE:
      return "unavailable";
    case BD_PERF_CLIENT_REJECTED:
      return "rejected";
    case BD_PERF_CLIENT_TIMEOUT:
      return "timeout";
    case BD_PERF_CLIENT_PROTOCOL:
      return "protocol-error";
  }
  return "protocol-error";
}

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

static int remaining_ms(uint64_t deadline) {
  uint64_t now = monotonic_ms();
  if (now >= deadline) {
    return 0;
  }
  uint64_t remaining = deadline - now;
  return remaining > 60000 ? 60000 : (int)remaining;
}

static bool wait_fd(int fd, short events, uint64_t deadline) {
  for (;;) {
    int remaining = remaining_ms(deadline);
    struct pollfd poll_fd;
    poll_fd.fd = fd;
    poll_fd.events = events;
    poll_fd.revents = 0;
    int ready = poll(&poll_fd, 1, remaining);
    if (ready > 0) {
      return (poll_fd.revents & (events | POLLHUP | POLLERR)) != 0;
    }
    if (ready == 0) {
      return false;
    }
    if (errno != EINTR) {
      return false;
    }
  }
}

const char *bd_perf_client_default_socket(void) {
  const char *env = getenv("BRICKDESK_PERF_SOCKET");
  if (env != NULL && env[0] != '\0') {
    return env;
  }
  static const char run_path[] = "/run/brickdesk/perfd.sock";
  static const char tmp_path[] = "/tmp/brickdesk-perfd.sock";
  if (access(run_path, F_OK) == 0) {
    return run_path;
  }
  if (access(tmp_path, F_OK) == 0) {
    return tmp_path;
  }
  return run_path;
}

static bool json_string_field(const char *json, const char *field, char *out,
                              size_t out_size) {
  if (json == NULL || field == NULL || out == NULL || out_size == 0) {
    return false;
  }
  char pattern[96];
  int written = snprintf(pattern, sizeof(pattern), "\"%s\"", field);
  if (written <= 0 || (size_t)written >= sizeof(pattern)) {
    return false;
  }
  const char *cursor = strstr(json, pattern);
  if (cursor == NULL) {
    return false;
  }
  cursor += strlen(pattern);
  while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
    ++cursor;
  }
  if (*cursor++ != ':') {
    return false;
  }
  while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
    ++cursor;
  }
  if (*cursor++ != '"') {
    return false;
  }
  size_t used = 0;
  while (*cursor != '\0' && *cursor != '"') {
    char value = *cursor++;
    if (value == '\\' && *cursor != '\0') {
      value = *cursor++;
    }
    if (used + 1 >= out_size) {
      return false;
    }
    out[used++] = value;
  }
  out[used] = '\0';
  return *cursor == '"';
}

static bool json_int_field(const char *json, const char *field, int *out) {
  if (json == NULL || field == NULL || out == NULL) {
    return false;
  }
  char pattern[96];
  int written = snprintf(pattern, sizeof(pattern), "\"%s\"", field);
  if (written <= 0 || (size_t)written >= sizeof(pattern)) {
    return false;
  }
  const char *cursor = strstr(json, pattern);
  if (cursor == NULL) {
    return false;
  }
  cursor += strlen(pattern);
  while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
    ++cursor;
  }
  if (*cursor++ != ':') {
    return false;
  }
  while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
    ++cursor;
  }
  char *end = NULL;
  long value = strtol(cursor, &end, 10);
  if (end == cursor || value < -2147483647L || value > 2147483647L) {
    return false;
  }
  *out = (int)value;
  return true;
}

static bool json_escape(char *out, size_t out_size, const char *text) {
  if (out == NULL || out_size == 0 || text == NULL) {
    return false;
  }
  size_t used = 0;
  for (const char *cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor == '"' || *cursor == '\\') {
      if (used + 2 >= out_size) {
        return false;
      }
      out[used++] = '\\';
    } else if ((unsigned char)*cursor < 0x20) {
      return false;
    }
    if (used + 1 >= out_size) {
      return false;
    }
    out[used++] = *cursor;
  }
  out[used] = '\0';
  return true;
}

static int connect_daemon(const char *socket_path, uint64_t deadline,
                          char *error, size_t error_size) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    set_error(error, error_size, "socket: %s", strerror(errno));
    return -(int)BD_PERF_CLIENT_UNAVAILABLE;
  }
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path) >=
      (int)sizeof(address.sun_path)) {
    close(fd);
    set_error(error, error_size, "perfd socket path is too long");
    return -(int)BD_PERF_CLIENT_UNAVAILABLE;
  }
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
    return fd;
  }
  if (errno != EINPROGRESS && errno != EAGAIN) {
    int code = errno;
    close(fd);
    set_error(error, error_size, "perfd unavailable at %s: %s", socket_path,
              strerror(code));
    return -(int)BD_PERF_CLIENT_UNAVAILABLE;
  }
  if (!wait_fd(fd, POLLOUT, deadline)) {
    close(fd);
    set_error(error, error_size, "perfd connect timed out at %s", socket_path);
    return -(int)BD_PERF_CLIENT_TIMEOUT;
  }
  int so_error = 0;
  socklen_t length = sizeof(so_error);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length) != 0 ||
      so_error != 0) {
    close(fd);
    set_error(error, error_size, "perfd unavailable at %s: %s", socket_path,
              strerror(so_error != 0 ? so_error : errno));
    return -(int)BD_PERF_CLIENT_UNAVAILABLE;
  }
  return fd;
}

bd_perf_client_status bd_perf_client_set_global(
    const char *socket_path, const char *profile, int timeout_ms,
    bd_perf_client_result *result, char *error, size_t error_size) {
  if (result != NULL) {
    memset(result, 0, sizeof(*result));
  }
  if (error != NULL && error_size > 0) {
    error[0] = '\0';
  }
  if (socket_path == NULL || socket_path[0] == '\0' || profile == NULL ||
      profile[0] == '\0' || strlen(profile) > BD_PERF_NAME_MAX) {
    set_error(error, error_size, "invalid perf request");
    return BD_PERF_CLIENT_PROTOCOL;
  }
  if (timeout_ms <= 0) {
    timeout_ms = 250;
  }
  uint64_t deadline = monotonic_ms() + (uint64_t)timeout_ms;

  int fd = connect_daemon(socket_path, deadline, error, error_size);
  if (fd < 0) {
    return (bd_perf_client_status)(-fd);
  }

  char escaped[BD_PERF_NAME_MAX * 2 + 1];
  if (!json_escape(escaped, sizeof(escaped), profile)) {
    close(fd);
    set_error(error, error_size, "profile name cannot be encoded");
    return BD_PERF_CLIENT_PROTOCOL;
  }
  char request[512];
  int request_length = snprintf(
      request, sizeof(request),
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"perf.global\","
      "\"profile\":\"%s\"}\n",
      escaped);
  if (request_length <= 0 || (size_t)request_length >= sizeof(request)) {
    close(fd);
    set_error(error, error_size, "perf request is too large");
    return BD_PERF_CLIENT_PROTOCOL;
  }

  size_t sent = 0;
  while (sent < (size_t)request_length) {
    ssize_t amount = send(fd, request + sent, (size_t)request_length - sent,
                          MSG_NOSIGNAL);
    if (amount > 0) {
      sent += (size_t)amount;
      continue;
    }
    if (amount < 0 && errno == EINTR) {
      continue;
    }
    if (amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (!wait_fd(fd, POLLOUT, deadline)) {
        close(fd);
        set_error(error, error_size, "perfd send timed out");
        return BD_PERF_CLIENT_TIMEOUT;
      }
      continue;
    }
    close(fd);
    set_error(error, error_size, "perfd write failed: %s", strerror(errno));
    return BD_PERF_CLIENT_UNAVAILABLE;
  }

  char response[BD_PERF_CLIENT_LINE_MAX];
  size_t response_length = 0;
  bool matched = false;
  while (!matched) {
    if (response_length + 1 >= sizeof(response)) {
      close(fd);
      set_error(error, error_size, "perfd response is too large");
      return BD_PERF_CLIENT_PROTOCOL;
    }
    ssize_t amount =
        recv(fd, response + response_length,
             sizeof(response) - 1 - response_length, 0);
    if (amount > 0) {
      response_length += (size_t)amount;
      response[response_length] = '\0';
      char *newline = memchr(response, '\n', response_length);
      while (newline != NULL) {
        *newline = '\0';
        if (strstr(response, "\"result\"") != NULL ||
            strstr(response, "\"error\"") != NULL) {
          matched = true;
          break;
        }
        /* Skip the daemon's hello and any unrelated notification. */
        size_t consumed = (size_t)(newline - response) + 1;
        memmove(response, response + consumed, response_length - consumed);
        response_length -= consumed;
        response[response_length] = '\0';
        newline = memchr(response, '\n', response_length);
      }
      continue;
    }
    if (amount == 0) {
      close(fd);
      set_error(error, error_size, "perfd closed the connection");
      return BD_PERF_CLIENT_PROTOCOL;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (!wait_fd(fd, POLLIN, deadline)) {
        close(fd);
        set_error(error, error_size, "perfd did not confirm the profile");
        return BD_PERF_CLIENT_TIMEOUT;
      }
      continue;
    }
    close(fd);
    set_error(error, error_size, "perfd read failed: %s", strerror(errno));
    return BD_PERF_CLIENT_UNAVAILABLE;
  }
  close(fd);
  response[strcspn(response, "\r\n")] = '\0';

  if (strstr(response, "\"error\"") != NULL) {
    char message[192] = {0};
    json_string_field(response, "message", message, sizeof(message));
    set_error(error, error_size, "%s",
              message[0] != '\0' ? message : "perfd rejected the profile");
    return BD_PERF_CLIENT_REJECTED;
  }
  if (strstr(response, "\"result\"") == NULL) {
    set_error(error, error_size, "perfd returned an invalid response");
    return BD_PERF_CLIENT_PROTOCOL;
  }
  if (result == NULL) {
    return BD_PERF_CLIENT_OK;
  }
  if (!json_string_field(response, "profile", result->profile,
                         sizeof(result->profile)) ||
      result->profile[0] == '\0') {
    set_error(error, error_size, "perfd did not confirm a profile");
    return BD_PERF_CLIENT_PROTOCOL;
  }
  if (strcmp(result->profile, profile) != 0) {
    set_error(error, error_size, "perfd applied %s instead of %s",
              result->profile, profile);
    return BD_PERF_CLIENT_PROTOCOL;
  }
  json_string_field(response, "governor", result->governor,
                    sizeof(result->governor));
  json_int_field(response, "min_khz", &result->min_khz);
  json_int_field(response, "max_khz", &result->max_khz);
  json_int_field(response, "online_cores", &result->online_cores);
  return BD_PERF_CLIENT_OK;
}
