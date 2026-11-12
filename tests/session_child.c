#define _POSIX_C_SOURCE 200809L
#define JSMN_STATIC
#include "jsmn.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define CHILD_LINE_MAX 8192

static const char *g_mode = "idle";

static uint64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void log_line(const char *format, ...) {
  const char *path = getenv("TEST_CHILD_LOG");
  if (path == NULL || path[0] == '\0') {
    return;
  }
  FILE *file = fopen(path, "a");
  if (file == NULL) {
    return;
  }
  fprintf(file, "%llu ", (unsigned long long)monotonic_ms());
  va_list args;
  va_start(args, format);
  vfprintf(file, format, args);
  va_end(args);
  fputc('\n', file);
  fclose(file);
}

static int json_skip(const jsmntok_t *tokens, int index) {
  int end = index + 1;
  if (tokens[index].type == JSMN_OBJECT) {
    for (int pair = 0; pair < tokens[index].size; ++pair) {
      end = json_skip(tokens, end);
      end = json_skip(tokens, end);
    }
  } else if (tokens[index].type == JSMN_ARRAY) {
    for (int element = 0; element < tokens[index].size; ++element) {
      end = json_skip(tokens, end);
    }
  }
  return end;
}

static int json_field(const jsmntok_t *tokens, int object, const char *json,
                      const char *key) {
  if (tokens[object].type != JSMN_OBJECT) {
    return -1;
  }
  int child = object + 1;
  for (int pair = 0; pair < tokens[object].size; ++pair) {
    int value = json_skip(tokens, child);
    if (tokens[child].type == JSMN_STRING) {
      size_t length = (size_t)(tokens[child].end - tokens[child].start);
      if (strlen(key) == length &&
          memcmp(json + tokens[child].start, key, length) == 0) {
        return value;
      }
    }
    child = json_skip(tokens, value);
  }
  return -1;
}

static int token_int(const char *json, const jsmntok_t *token) {
  if (token->type != JSMN_PRIMITIVE) {
    return -1;
  }
  char buffer[32];
  size_t length = (size_t)(token->end - token->start);
  if (length == 0 || length >= sizeof(buffer)) {
    return -1;
  }
  memcpy(buffer, json + token->start, length);
  buffer[length] = '\0';
  char *end = NULL;
  long value = strtol(buffer, &end, 10);
  if (end == buffer || *end != '\0' || value < 0 || value > 2147483647L) {
    return -1;
  }
  return (int)value;
}

static bool token_string(const char *json, const jsmntok_t *token, char *out,
                         size_t out_size) {
  if (token->type != JSMN_STRING) {
    return false;
  }
  size_t length = (size_t)(token->end - token->start);
  if (length == 0 || length >= out_size) {
    return false;
  }
  memcpy(out, json + token->start, length);
  out[length] = '\0';
  return true;
}

static bool send_line(int fd, const char *line) {
  if (fd < 0) {
    return false;
  }
  char buffer[CHILD_LINE_MAX + 2];
  size_t length = strlen(line);
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
      return false;
    }
    sent += (size_t)amount;
  }
  return true;
}

static void reply_ok(int fd, int id) {
  if (id < 0) {
    return;
  }
  char line[128];
  snprintf(line, sizeof(line),
           "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"ok\":true}}", id);
  send_line(fd, line);
}

static bool save_state(int slot) {
  const char *directory = getenv("BRICKDESK_STATE_DIR");
  if (directory == NULL || directory[0] == '\0') {
    return false;
  }
  char path[512];
  snprintf(path, sizeof(path), "%s/state.txt", directory);
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  fprintf(file, "slot=%d\nsaved_at_ms=%llu\n", slot,
          (unsigned long long)monotonic_ms());
  return fclose(file) == 0;
}

static int connect_session(void) {
  const char *path = getenv("BRICKDESK_SESSION_SOCKET");
  if (path == NULL || path[0] == '\0') {
    return -1;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(address.sun_path)) {
    close(fd);
    return -1;
  }
  snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      }
      return fd;
    }
    if (errno != ENOENT && errno != ECONNREFUSED) {
      break;
    }
    struct timespec delay = {0, 20 * 1000 * 1000};
    nanosleep(&delay, NULL);
  }
  close(fd);
  return -1;
}

static void handle_line(int fd, const char *line) {
  if (line[0] == '\0') {
    return;
  }
  jsmn_parser parser;
  jsmntok_t tokens[128];
  jsmn_init(&parser);
  int count = jsmn_parse(&parser, line, strlen(line), tokens,
                         sizeof(tokens) / sizeof(tokens[0]));
  if (count < 1 || tokens[0].type != JSMN_OBJECT) {
    return;
  }
  int method_token = json_field(tokens, 0, line, "method");
  int id_token = json_field(tokens, 0, line, "id");
  if (method_token < 0 || id_token < 0) {
    return;
  }
  char method[64];
  if (!token_string(line, &tokens[method_token], method, sizeof(method))) {
    return;
  }
  int id = token_int(line, &tokens[id_token]);
  log_line("method %s id %d", method, id);

  if (strcmp(method, "app.quit") == 0) {
    if (strcmp(g_mode, "ignore_quit") == 0) {
      log_line("quit ignored");
      return;
    }
    reply_ok(fd, id);
    save_state(0);
    exit(0);
  }
  if (strcmp(method, "state.save") == 0) {
    int slot = 0;
    int params = json_field(tokens, 0, line, "params");
    if (params >= 0) {
      int slot_token = json_field(tokens, params, line, "slot");
      if (slot_token >= 0) {
        int value = token_int(line, &tokens[slot_token]);
        if (value >= 0) {
          slot = value;
        }
      }
    }
    save_state(slot);
    reply_ok(fd, id);
    return;
  }
  if (strcmp(method, "state.load") == 0) {
    reply_ok(fd, id);
    return;
  }
  if (strcmp(method, "app.pause") == 0 || strcmp(method, "app.resume") == 0 ||
      strcmp(method, "app.display_release") == 0 ||
      strcmp(method, "app.display_acquire") == 0 ||
      strcmp(method, "app.ping") == 0) {
    reply_ok(fd, id);
    return;
  }
  char error[256];
  snprintf(error, sizeof(error),
           "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":-32601,"
           "\"message\":\"method not found\"}}",
           id);
  send_line(fd, error);
}

int main(int argc, char **argv) {
  if (argc > 1) {
    g_mode = argv[1];
  }
  signal(SIGPIPE, SIG_IGN);
  if (strcmp(g_mode, "ignore_quit") == 0) {
    signal(SIGTERM, SIG_IGN);
  }

  if (strcmp(g_mode, "crash") == 0) {
    struct timespec delay = {0, 50 * 1000 * 1000};
    nanosleep(&delay, NULL);
    _exit(3);
  }

  int fd = -1;
  if (strcmp(g_mode, "idle") != 0) {
    fd = connect_session();
    if (fd >= 0) {
      char hello[512];
      snprintf(hello, sizeof(hello),
               "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"app.hello\","
               "\"params\":{\"pid\":%ld,\"protocol\":1,\"capabilities\":["
               "\"pause\",\"resume\",\"save_state\",\"load_state\","
               "\"display_release\"]}}",
               (long)getpid());
      send_line(fd, hello);
      log_line("hello");
    }
  }

  char input[CHILD_LINE_MAX];
  size_t input_length = 0;
  for (;;) {
    if (fd < 0) {
      pause();
      continue;
    }
    struct pollfd poll_fd;
    poll_fd.fd = fd;
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;
    int result = poll(&poll_fd, 1, 1000);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (result == 0) {
      continue;
    }
    char buffer[2048];
    ssize_t bytes = recv(fd, buffer, sizeof(buffer), 0);
    if (bytes < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        continue;
      }
      break;
    }
    if (bytes == 0) {
      break;
    }
    if (input_length + (size_t)bytes >= sizeof(input)) {
      input_length = 0;
      continue;
    }
    memcpy(input + input_length, buffer, (size_t)bytes);
    input_length += (size_t)bytes;
    char *newline = NULL;
    while ((newline = memchr(input, '\n', input_length)) != NULL) {
      size_t length = (size_t)(newline - input);
      input[length] = '\0';
      handle_line(fd, input);
      memmove(input, input + length + 1, input_length - length - 1);
      input_length -= length + 1;
    }
  }
  if (fd >= 0) {
    close(fd);
  }
  return 0;
}
