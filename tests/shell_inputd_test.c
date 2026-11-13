#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

/*
 * Desktop shell <-> inputd socket lifecycle.
 *
 * The shell is a real bd_shell with the host SDL/GLES backend.  A forked Unix
 * socket peer stands in for a daemon that starts late, disconnects, restarts
 * or is unreachable; a real brickdesk-inputd replay covers the protocol path.
 * Every assertion is made through public shell/protocol behaviour: the input
 * source, the subscribe/release lines the peer receives, and the hint bar.
 */

#include "brickdesk/config.h"
#include "brickdesk/gfx.h"
#include "brickdesk/home.h"
#include "brickdesk/platform.h"
#include "brickdesk/settings.h"
#include "brickdesk/shell.h"
#include "settings_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
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

#ifndef BRICKDESK_TEST_BATTERY_FIXTURE
#define BRICKDESK_TEST_BATTERY_FIXTURE "tests/fixtures/battery/full"
#endif

#define TEST_PATH_MAX 1024
#define TEST_LOG_MAX (128 * 1024)
#define TEST_FILLERS 64

static int failures;

#define CHECK(condition, ...)                                                  \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "shell_inputd_test: FAIL: ");                            \
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

static bool join(char *out, size_t size, const char *directory,
                 const char *name) {
  int written = snprintf(out, size, "%s/%s", directory, name);
  return written > 0 && (size_t)written < size;
}

static bool write_text(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs(text, file) >= 0;
  return fclose(file) == 0 && ok;
}

static void dump_file(const char *label, const char *path) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return;
  }
  fprintf(stderr, "--- %s (%s) ---\n", label, path);
  char line[1024];
  while (fgets(line, sizeof(line), file) != NULL) {
    fputs(line, stderr);
  }
  fprintf(stderr, "--- end %s ---\n", label);
  fclose(file);
}

/* --------------------------------------------------------- fake socket peer */

typedef enum server_command_kind {
  SERVER_COMMAND_SEND = 1,
  SERVER_COMMAND_CLOSE,
  SERVER_COMMAND_STOP
} server_command_kind;

typedef struct server_command {
  int kind;
  size_t length;
  char text[512];
} server_command;

typedef struct fake_server {
  pid_t pid;
  int command_fd;
  char socket_path[TEST_PATH_MAX];
  char log_path[TEST_PATH_MAX];
} fake_server;

static void server_stop(fake_server *server);

static void server_log(FILE *log, const char *format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(log, format, args);
  va_end(args);
  fputc('\n', log);
  fflush(log);
}

static void server_set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  int fd_flags = fcntl(fd, F_GETFD, 0);
  if (fd_flags >= 0) {
    fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
  }
}

static int server_create_listener(const char *path, int backlog) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  server_set_nonblocking(fd);
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(address.sun_path)) {
    close(fd);
    return -1;
  }
  snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
  unlink(path);
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(fd, backlog) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void server_close_client(int *client, FILE *log) {
  if (*client >= 0) {
    close(*client);
    *client = -1;
    server_log(log, "CLOSE");
  }
}

static void server_read_client(int *client, FILE *log, char *line,
                               size_t *line_length) {
  char buffer[1024];
  for (;;) {
    ssize_t amount = recv(*client, buffer, sizeof(buffer), 0);
    if (amount < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      server_log(log, "READ_ERROR %s", strerror(errno));
      server_close_client(client, log);
      return;
    }
    if (amount == 0) {
      server_log(log, "EOF");
      server_close_client(client, log);
      return;
    }
    size_t copied = 0;
    while (copied < (size_t)amount) {
      if (*line_length >= TEST_LOG_MAX - 1) {
        server_log(log, "LINE_TOO_LONG");
        *line_length = 0;
      }
      size_t room = TEST_LOG_MAX - 1 - *line_length;
      size_t chunk = (size_t)amount - copied;
      if (chunk > room) {
        chunk = room;
      }
      memcpy(line + *line_length, buffer + copied, chunk);
      *line_length += chunk;
      copied += chunk;
      char *newline = memchr(line, '\n', *line_length);
      while (newline != NULL) {
        size_t length = (size_t)(newline - line);
        if (length > 0 && line[length - 1] == '\r') {
          --length;
        }
        line[length] = '\0';
        server_log(log, "LINE %s", line);
        size_t consumed = (size_t)(newline - line) + 1;
        memmove(line, line + consumed, *line_length - consumed);
        *line_length -= consumed;
        newline = memchr(line, '\n', *line_length);
      }
    }
  }
}

static void server_send_bytes(int *client, const char *text, size_t length,
                              FILE *log) {
  if (*client < 0) {
    server_log(log, "SEND_WITHOUT_CLIENT");
    return;
  }
  size_t sent = 0;
  while (sent < length) {
    ssize_t amount = send(*client, text + sent, length - sent, MSG_NOSIGNAL);
    if (amount < 0) {
      if (errno == EINTR) {
        continue;
      }
      server_log(log, "SEND_ERROR %s", strerror(errno));
      server_close_client(client, log);
      return;
    }
    if (amount == 0) {
      server_log(log, "SEND_ZERO");
      server_close_client(client, log);
      return;
    }
    sent += (size_t)amount;
  }
  server_log(log, "SENT %zu", length);
}

static void server_main(const char *socket_path, const char *log_path,
                        int backlog, int command_fd) {
  FILE *log = fopen(log_path, "a");
  if (log == NULL) {
    _exit(127);
  }
  setvbuf(log, NULL, _IOLBF, 0);
  int listener = server_create_listener(socket_path, backlog);
  if (listener < 0) {
    server_log(log, "LISTEN_ERROR %s", strerror(errno));
    fclose(log);
    _exit(127);
  }
  server_log(log, "LISTEN backlog=%d", backlog);
  int client = -1;
  char line[TEST_LOG_MAX];
  size_t line_length = 0;
  bool stop = false;
  while (!stop) {
    struct pollfd fds[3];
    int count = 0;
    int listener_index = count;
    fds[count].fd = listener;
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    ++count;
    int command_index = count;
    fds[count].fd = command_fd;
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    ++count;
    int client_index = -1;
    if (client >= 0) {
      client_index = count;
      fds[count].fd = client;
      fds[count].events = POLLIN;
      fds[count].revents = 0;
      ++count;
    }
    int ready = poll(fds, (nfds_t)count, 20);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      server_log(log, "POLL_ERROR %s", strerror(errno));
      break;
    }
    if (fds[command_index].revents != 0) {
      server_command command;
      ssize_t amount = read(command_fd, &command, sizeof(command));
      if (amount == 0) {
        break;
      }
      if (amount == (ssize_t)sizeof(command)) {
        if (command.kind == SERVER_COMMAND_SEND) {
          if (command.length > sizeof(command.text)) {
            command.length = sizeof(command.text);
          }
          server_send_bytes(&client, command.text, command.length, log);
        } else if (command.kind == SERVER_COMMAND_CLOSE) {
          server_close_client(&client, log);
        } else if (command.kind == SERVER_COMMAND_STOP) {
          stop = true;
        }
      }
    }
    if (client_index >= 0 && fds[client_index].revents != 0 &&
        client >= 0) {
      server_read_client(&client, log, line, &line_length);
    }
    if (client < 0) {
      line_length = 0;
    }
    if (fds[listener_index].revents != 0 && client < 0) {
      int accepted = accept(listener, NULL, NULL);
      if (accepted >= 0) {
        server_set_nonblocking(accepted);
        client = accepted;
        line_length = 0;
        server_log(log, "CONNECT");
      }
    }
  }
  server_close_client(&client, log);
  close(listener);
  unlink(socket_path);
  fclose(log);
  _exit(0);
}

static bool wait_for_path(const char *path, int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 10) {
    if (access(path, F_OK) == 0) {
      return true;
    }
    sleep_ms(10);
  }
  return false;
}

static bool server_start(fake_server *server, const char *socket_path,
                         const char *log_path, int backlog) {
  memset(server, 0, sizeof(*server));
  server->pid = -1;
  server->command_fd = -1;
  snprintf(server->socket_path, sizeof(server->socket_path), "%s",
           socket_path);
  snprintf(server->log_path, sizeof(server->log_path), "%s", log_path);
  unlink(socket_path);
  int commands[2];
  if (pipe(commands) != 0) {
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(commands[0]);
    close(commands[1]);
    return false;
  }
  if (pid == 0) {
    close(commands[1]);
    setsid();
    server_main(socket_path, log_path, backlog, commands[0]);
  }
  close(commands[0]);
  server->pid = pid;
  server->command_fd = commands[1];
  if (!wait_for_path(socket_path, 3000)) {
    server_stop(server);
    return false;
  }
  return true;
}

static bool server_send_part(fake_server *server, const char *text,
                             size_t length) {
  server_command command;
  memset(&command, 0, sizeof(command));
  command.kind = SERVER_COMMAND_SEND;
  if (length > sizeof(command.text)) {
    length = sizeof(command.text);
  }
  command.length = length;
  memcpy(command.text, text, length);
  ssize_t amount = write(server->command_fd, &command, sizeof(command));
  return amount == (ssize_t)sizeof(command);
}

static bool server_send(fake_server *server, const char *text) {
  return server_send_part(server, text, strlen(text));
}

static bool server_request_close(fake_server *server) {
  server_command command;
  memset(&command, 0, sizeof(command));
  command.kind = SERVER_COMMAND_CLOSE;
  ssize_t amount = write(server->command_fd, &command, sizeof(command));
  return amount == (ssize_t)sizeof(command);
}

static void server_stop(fake_server *server) {
  if (server->pid > 0) {
    if (server->command_fd >= 0) {
      server_command command;
      memset(&command, 0, sizeof(command));
      command.kind = SERVER_COMMAND_STOP;
      if (write(server->command_fd, &command, sizeof(command)) !=
          (ssize_t)sizeof(command)) {
        /* The peer may already have exited; waitpid below handles that. */
      }
    }
    for (int attempt = 0; attempt < 200; ++attempt) {
      int status = 0;
      pid_t result = waitpid(server->pid, &status, WNOHANG);
      if (result == server->pid || (result < 0 && errno == ECHILD)) {
        server->pid = -1;
        break;
      }
      sleep_ms(10);
    }
    if (server->pid > 0) {
      kill(server->pid, SIGKILL);
      waitpid(server->pid, NULL, 0);
      server->pid = -1;
    }
  }
  if (server->command_fd >= 0) {
    close(server->command_fd);
    server->command_fd = -1;
  }
  unlink(server->socket_path);
}

static char *read_whole_file(const char *path) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return NULL;
  }
  char *text = (char *)malloc(TEST_LOG_MAX);
  if (text == NULL) {
    fclose(file);
    return NULL;
  }
  size_t amount = fread(text, 1, TEST_LOG_MAX - 1, file);
  text[amount] = '\0';
  fclose(file);
  return text;
}

static int log_count(const char *path, const char *needle) {
  char *text = read_whole_file(path);
  if (text == NULL) {
    return 0;
  }
  int count = 0;
  const char *cursor = text;
  size_t length = strlen(needle);
  while (length > 0 && (cursor = strstr(cursor, needle)) != NULL) {
    ++count;
    cursor += length;
  }
  free(text);
  return count;
}

static bool server_wait_count(fake_server *server, const char *needle,
                              int wanted, int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 10) {
    if (log_count(server->log_path, needle) >= wanted) {
      return true;
    }
    sleep_ms(10);
  }
  return false;
}

/* ------------------------------------------------------ independent subscriber */

typedef struct test_client {
  int fd;
  char history[64 * 1024];
  size_t history_length;
} test_client;

static bool client_connect(test_client *client, const char *path) {
  memset(client, 0, sizeof(*client));
  client->fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (client->fd < 0) {
    return false;
  }
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  size_t path_length = strlen(path);
  if (path_length >= sizeof(address.sun_path)) {
    close(client->fd);
    client->fd = -1;
    return false;
  }
  memcpy(address.sun_path, path, path_length + 1);
  if (connect(client->fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    close(client->fd);
    client->fd = -1;
    return false;
  }
  server_set_nonblocking(client->fd);
  static const char subscribe[] =
      "{\"cmd\":\"subscribe\",\"actions\":true,\"keys\":true}\n";
  size_t sent = 0;
  while (sent < sizeof(subscribe) - 1) {
    ssize_t amount = send(client->fd, subscribe + sent,
                          sizeof(subscribe) - 1 - sent, MSG_NOSIGNAL);
    if (amount < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(client->fd);
      client->fd = -1;
      return false;
    }
    if (amount == 0) {
      close(client->fd);
      client->fd = -1;
      return false;
    }
    sent += (size_t)amount;
  }
  return true;
}

static void client_close(test_client *client) {
  if (client->fd >= 0) {
    close(client->fd);
    client->fd = -1;
  }
}

static void client_pump(test_client *client, int timeout_ms) {
  if (client->fd < 0) {
    return;
  }
  struct pollfd pfd;
  pfd.fd = client->fd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  int ready = poll(&pfd, 1, timeout_ms);
  if (ready <= 0) {
    return;
  }
  /* Drain buffered data even when the peer has also hung up; otherwise the
   * final replay release could be lost at EOF. */
  for (;;) {
    char buffer[1024];
    ssize_t amount = recv(client->fd, buffer, sizeof(buffer), 0);
    if (amount < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      client_close(client);
      return;
    }
    if (amount == 0) {
      client_close(client);
      return;
    }
    size_t room = sizeof(client->history) - 1 - client->history_length;
    size_t chunk = (size_t)amount;
    if (chunk > room) {
      chunk = room;
    }
    memcpy(client->history + client->history_length, buffer, chunk);
    client->history_length += chunk;
    client->history[client->history_length] = '\0';
  }
}

static bool client_contains(const test_client *client, const char *needle) {
  return strstr(client->history, needle) != NULL;
}

static bool client_contains_after(const test_client *client, size_t mark,
                                  const char *needle) {
  return mark <= client->history_length &&
         strstr(client->history + mark, needle) != NULL;
}

static bool client_wait_for(test_client *client, const char *needle,
                            int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 10) {
    if (client_contains(client, needle)) {
      return true;
    }
    client_pump(client, 10);
  }
  return client_contains(client, needle);
}

static bool client_quiet_for(test_client *client, const char *needle,
                             int duration_ms) {
  size_t mark = client->history_length;
  uint64_t deadline = real_ms() + (uint64_t)duration_ms;
  while (real_ms() < deadline) {
    client_pump(client, 20);
  }
  return !client_contains_after(client, mark, needle);
}

/* -------------------------------------------------------------- socket proxy */

typedef enum proxy_command_kind {
  PROXY_COMMAND_DROP = 1,
  PROXY_COMMAND_STOP
} proxy_command_kind;

typedef struct proxy_command {
  int kind;
} proxy_command;

typedef struct socket_proxy {
  pid_t pid;
  int command_fd;
  char listen_path[TEST_PATH_MAX];
  char upstream_path[TEST_PATH_MAX];
  char log_path[TEST_PATH_MAX];
} socket_proxy;

static void proxy_drop_pair(int *client, int *upstream, FILE *log) {
  if (*client >= 0) {
    close(*client);
    *client = -1;
  }
  if (*upstream >= 0) {
    close(*upstream);
    *upstream = -1;
  }
  server_log(log, "DROP");
}

static bool proxy_forward(int from, int to, FILE *log) {
  char buffer[1024];
  ssize_t amount = recv(from, buffer, sizeof(buffer), 0);
  if (amount < 0) {
    if (errno == EINTR) {
      return true;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    }
    server_log(log, "PROXY_READ_ERROR %s", strerror(errno));
    return false;
  }
  if (amount == 0) {
    return false;
  }
  size_t sent = 0;
  while (sent < (size_t)amount) {
    ssize_t written =
        send(to, buffer + sent, (size_t)amount - sent, MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      server_log(log, "PROXY_WRITE_ERROR %s", strerror(errno));
      return false;
    }
    if (written == 0) {
      server_log(log, "PROXY_WRITE_ZERO");
      return false;
    }
    sent += (size_t)written;
  }
  return true;
}

static int proxy_connect_upstream(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  size_t path_length = strlen(path);
  if (path_length >= sizeof(address.sun_path)) {
    close(fd);
    return -1;
  }
  memcpy(address.sun_path, path, path_length + 1);
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    close(fd);
    return -1;
  }
  server_set_nonblocking(fd);
  return fd;
}

static void proxy_main(const char *listen_path, const char *upstream_path,
                       const char *log_path, int command_fd) {
  FILE *log = fopen(log_path, "a");
  if (log == NULL) {
    _exit(127);
  }
  setvbuf(log, NULL, _IOLBF, 0);
  int listener = server_create_listener(listen_path, 4);
  if (listener < 0) {
    server_log(log, "PROXY_LISTEN_ERROR %s", strerror(errno));
    fclose(log);
    _exit(127);
  }
  server_log(log, "PROXY_LISTEN");
  int client = -1;
  int upstream = -1;
  bool stop = false;
  while (!stop) {
    struct pollfd fds[4];
    int count = 0;
    int listener_index = count;
    fds[count].fd = listener;
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    ++count;
    int command_index = count;
    fds[count].fd = command_fd;
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    ++count;
    int client_index = -1;
    if (client >= 0) {
      client_index = count;
      fds[count].fd = client;
      fds[count].events = POLLIN;
      fds[count].revents = 0;
      ++count;
    }
    int upstream_index = -1;
    if (upstream >= 0) {
      upstream_index = count;
      fds[count].fd = upstream;
      fds[count].events = POLLIN;
      fds[count].revents = 0;
      ++count;
    }
    int ready = poll(fds, (nfds_t)count, 20);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      server_log(log, "PROXY_POLL_ERROR %s", strerror(errno));
      break;
    }
    if (fds[command_index].revents != 0) {
      proxy_command command;
      ssize_t amount = read(command_fd, &command, sizeof(command));
      if (amount == 0) {
        break;
      }
      if (amount == (ssize_t)sizeof(command)) {
        if (command.kind == PROXY_COMMAND_DROP) {
          proxy_drop_pair(&client, &upstream, log);
        } else if (command.kind == PROXY_COMMAND_STOP) {
          stop = true;
        }
      }
    }
    if (client_index >= 0 && fds[client_index].revents != 0 &&
        client >= 0 && upstream >= 0) {
      if (!proxy_forward(client, upstream, log)) {
        proxy_drop_pair(&client, &upstream, log);
      }
    }
    if (upstream_index >= 0 && fds[upstream_index].revents != 0 &&
        client >= 0 && upstream >= 0) {
      if (!proxy_forward(upstream, client, log)) {
        proxy_drop_pair(&client, &upstream, log);
      }
    }
    if (fds[listener_index].revents != 0 && client < 0) {
      int accepted = accept(listener, NULL, NULL);
      if (accepted >= 0) {
        server_set_nonblocking(accepted);
        int connected = proxy_connect_upstream(upstream_path);
        if (connected < 0) {
          server_log(log, "PROXY_UPSTREAM_ERROR %s", strerror(errno));
          close(accepted);
        } else {
          client = accepted;
          upstream = connected;
          server_log(log, "PROXY_CONNECT");
        }
      }
    }
  }
  proxy_drop_pair(&client, &upstream, log);
  close(listener);
  unlink(listen_path);
  fclose(log);
  _exit(0);
}

static void proxy_stop(socket_proxy *proxy);

static bool proxy_start(socket_proxy *proxy, const char *listen_path,
                        const char *upstream_path, const char *log_path) {
  memset(proxy, 0, sizeof(*proxy));
  proxy->pid = -1;
  proxy->command_fd = -1;
  snprintf(proxy->listen_path, sizeof(proxy->listen_path), "%s", listen_path);
  snprintf(proxy->upstream_path, sizeof(proxy->upstream_path), "%s",
           upstream_path);
  snprintf(proxy->log_path, sizeof(proxy->log_path), "%s", log_path);
  int commands[2];
  if (pipe(commands) != 0) {
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(commands[0]);
    close(commands[1]);
    return false;
  }
  if (pid == 0) {
    close(commands[1]);
    setsid();
    proxy_main(listen_path, upstream_path, log_path, commands[0]);
  }
  close(commands[0]);
  proxy->pid = pid;
  proxy->command_fd = commands[1];
  if (!wait_for_path(listen_path, 3000)) {
    proxy_stop(proxy);
    return false;
  }
  return true;
}

static bool proxy_drop(socket_proxy *proxy) {
  proxy_command command;
  memset(&command, 0, sizeof(command));
  command.kind = PROXY_COMMAND_DROP;
  ssize_t amount = write(proxy->command_fd, &command, sizeof(command));
  return amount == (ssize_t)sizeof(command);
}

static void proxy_stop(socket_proxy *proxy) {
  if (proxy->pid > 0) {
    if (proxy->command_fd >= 0) {
      proxy_command command;
      memset(&command, 0, sizeof(command));
      command.kind = PROXY_COMMAND_STOP;
      if (write(proxy->command_fd, &command, sizeof(command)) !=
          (ssize_t)sizeof(command)) {
        /* The proxy may already have exited; waitpid below handles that. */
      }
    }
    for (int attempt = 0; attempt < 200; ++attempt) {
      int status = 0;
      pid_t result = waitpid(proxy->pid, &status, WNOHANG);
      if (result == proxy->pid || (result < 0 && errno == ECHILD)) {
        proxy->pid = -1;
        break;
      }
      sleep_ms(10);
    }
    if (proxy->pid > 0) {
      kill(proxy->pid, SIGKILL);
      waitpid(proxy->pid, NULL, 0);
      proxy->pid = -1;
    }
  }
  if (proxy->command_fd >= 0) {
    close(proxy->command_fd);
    proxy->command_fd = -1;
  }
  unlink(proxy->listen_path);
}

/* --------------------------------------------------------------- shell side */

typedef struct test_runtime {
  bd_platform *platform;
  bd_gfx *gfx;
  bd_config *config;
  bd_shell *shell;
  bd_settings_system_stub stub;
  char root[TEST_PATH_MAX];
  char config_dir[TEST_PATH_MAX];
  char apps_dir[TEST_PATH_MAX];
  char socket_dir[TEST_PATH_MAX];
  char frame_dir[TEST_PATH_MAX];
  char state_dir[TEST_PATH_MAX];
  char input_socket[TEST_PATH_MAX];
  char fake_log[TEST_PATH_MAX];
  char inputd_log[TEST_PATH_MAX];
  char replay[TEST_PATH_MAX];
} test_runtime;

static bool setup_runtime(test_runtime *runtime) {
  memset(runtime, 0, sizeof(*runtime));
  char pattern[] = "/tmp/bd-shell-inputd-XXXXXX";
  char *root = mkdtemp(pattern);
  if (root == NULL) {
    return false;
  }
  snprintf(runtime->root, sizeof(runtime->root), "%s", root);
  if (!sf_make_config_dir(runtime->config_dir, sizeof(runtime->config_dir),
                          false)) {
    return false;
  }
  if (!join(runtime->apps_dir, sizeof(runtime->apps_dir), runtime->root,
            "apps") ||
      !join(runtime->socket_dir, sizeof(runtime->socket_dir), runtime->root,
            "sockets") ||
      !join(runtime->frame_dir, sizeof(runtime->frame_dir), runtime->root,
            "frames") ||
      !join(runtime->state_dir, sizeof(runtime->state_dir), runtime->root,
            "state") ||
      !join(runtime->input_socket, sizeof(runtime->input_socket), runtime->root,
            "inputd.sock") ||
      !join(runtime->fake_log, sizeof(runtime->fake_log), runtime->root,
            "fake.log") ||
      !join(runtime->inputd_log, sizeof(runtime->inputd_log), runtime->root,
            "inputd.log") ||
      !join(runtime->replay, sizeof(runtime->replay), runtime->root,
            "replay.txt")) {
    return false;
  }
  if (!make_dir(runtime->apps_dir) || !make_dir(runtime->socket_dir) ||
      !make_dir(runtime->frame_dir) || !make_dir(runtime->state_dir)) {
    return false;
  }

  bd_config_options options;
  memset(&options, 0, sizeof(options));
  options.config_dir = runtime->config_dir;
  options.migrate = false;
  char error[256] = {0};
  runtime->config = bd_config_load(&options, error, sizeof(error));
  if (runtime->config == NULL) {
    fprintf(stderr, "shell_inputd_test: config: %s\n", error);
    return false;
  }

  bd_settings_system system;
  bd_settings_system_stub_init(&runtime->stub, &system);

  bd_platform_config platform_config;
  memset(&platform_config, 0, sizeof(platform_config));
  platform_config.title = "brickdesk shell inputd test";
  platform_config.width = 1024;
  platform_config.height = 768;
  platform_config.hidden = true;
  runtime->platform = bd_platform_create(&platform_config);
  if (runtime->platform == NULL) {
    return false;
  }
  runtime->gfx = bd_gfx_create(1024, 768);
  if (runtime->gfx == NULL) {
    return false;
  }

  bd_shell_config shell_config;
  memset(&shell_config, 0, sizeof(shell_config));
  shell_config.platform = runtime->platform;
  shell_config.gfx = runtime->gfx;
  shell_config.settings = runtime->config;
  shell_config.apps_dir = runtime->apps_dir;
  shell_config.input_socket = runtime->input_socket;
  shell_config.session_socket_dir = runtime->socket_dir;
  shell_config.session_frame_dir = runtime->frame_dir;
  shell_config.session_state_root = runtime->state_dir;
  shell_config.battery_root = BRICKDESK_TEST_BATTERY_FIXTURE;
  shell_config.settings_system = &system;
  shell_config.verbose = false;
  runtime->shell = bd_shell_create(&shell_config);
  return runtime->shell != NULL;
}

static void destroy_runtime(test_runtime *runtime) {
  if (runtime->shell != NULL) {
    bd_shell_destroy(runtime->shell);
    runtime->shell = NULL;
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
  sf_remove_tree(runtime->config_dir);
  sf_remove_tree(runtime->root);
}

static bool pump_for(bd_shell *shell, int duration_ms) {
  uint64_t deadline = real_ms() + (uint64_t)(duration_ms > 0 ? duration_ms : 0);
  for (;;) {
    uint64_t now = real_ms();
    if (now >= deadline) {
      return true;
    }
    uint64_t remaining = deadline - now;
    int timeout = remaining > 20 ? 20 : (int)remaining;
    if (bd_shell_iterate(shell, timeout) < 0) {
      return false;
    }
  }
}

static bool wait_for_source(bd_shell *shell, const char *source,
                            int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 10) {
    if (strcmp(bd_shell_input_source(shell), source) == 0) {
      return true;
    }
    if (!pump_for(shell, 10)) {
      return false;
    }
  }
  return strcmp(bd_shell_input_source(shell), source) == 0;
}

static bool wait_for_hint(bd_shell *shell, bool visible, int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 10) {
    if (bd_shell_hint_visible(shell) == visible) {
      return true;
    }
    if (!pump_for(shell, 10)) {
      return false;
    }
  }
  return bd_shell_hint_visible(shell) == visible;
}

static pid_t start_inputd(const char *inputd, const char *config_dir,
                          const char *socket_path, const char *replay,
                          const char *log_path) {
  unlink(socket_path);
  pid_t pid = fork();
  if (pid != 0) {
    return pid;
  }
  setsid();
  int log = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (log >= 0) {
    dup2(log, STDOUT_FILENO);
    dup2(log, STDERR_FILENO);
    close(log);
  }
  execl(inputd, inputd, "--config-dir", config_dir, "--socket", socket_path,
        "--replay", replay, "--verbose", (char *)NULL);
  _exit(127);
}

static void stop_inputd(pid_t pid) {
  if (pid <= 0) {
    return;
  }
  kill(pid, SIGTERM);
  for (int attempt = 0; attempt < 300; ++attempt) {
    int status = 0;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD)) {
      return;
    }
    sleep_ms(10);
  }
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
}

/* ---------------------------------------------------------------- scenarios */

static void test_lifecycle(test_runtime *runtime, fake_server *server,
                           const char *inputd) {
  bd_shell *shell = runtime->shell;

  CHECK(strcmp(bd_shell_input_source(shell), "sdl") == 0,
        "missing inputd starts in SDL mode, got %s",
        bd_shell_input_source(shell));

  CHECK(server_start(server, runtime->input_socket, runtime->fake_log, 4),
        "start fake inputd");
  if (!wait_for_source(shell, "inputd", 4000)) {
    CHECK(false, "late daemon was never picked up (input source %s)",
          bd_shell_input_source(shell));
    dump_file("fake server", runtime->fake_log);
    return;
  }
  CHECK(server_wait_count(server, "\"cmd\":\"subscribe\"", 1, 2000),
        "new connection subscribes");
  CHECK(server_wait_count(server, "\"actions\":true", 1, 2000),
        "subscription requests actions");
  CHECK(server_wait_count(server, "\"keys\":true", 1, 2000),
        "subscription requests keys");
  CHECK(log_count(runtime->fake_log, "\"cmd\":\"release_all\"") == 0,
        "subscription must not flush another client's input");
  CHECK(server_send(server,
                    "{\"type\":\"hello\",\"version\":1,\"pid\":1,"
                    "\"devices\":[]}\n"),
        "send hello");

  /* Malformed and split lines must not disturb the connection or drop a
   * complete line that arrives in two writes. */
  const char *half =
      "{\"type\":\"action\",\"action\":\"ui.hint\",\"phase\":\"down\"}\n";
  size_t split = strlen(half) / 2;
  CHECK(server_send(server, "not-json\n"), "send malformed line");
  CHECK(server_send_part(server, half, split), "send first half line");
  CHECK(pump_for(shell, 80), "pump after half line");
  CHECK(!bd_shell_hint_visible(shell),
        "an incomplete action line must not dispatch");
  CHECK(strcmp(bd_shell_input_source(shell), "inputd") == 0,
        "malformed line keeps the connection");
  CHECK(server_send(server, half + split), "send second half line");
  CHECK(wait_for_hint(shell, true, 1500), "split action line reaches the UI");

  CHECK(server_send(server,
                    "{\"type\":\"release_all\",\"time_ms\":1}\n"),
        "send release_all");
  CHECK(wait_for_hint(shell, false, 1500), "release_all hides the hint");

  /* A hold interrupted by the socket closing must hide the hint and drop
   * back to SDL; the same listener accepts again and the shell resubscribes. */
  CHECK(server_send(server, half), "send hint hold");
  CHECK(wait_for_hint(shell, true, 1500), "hold shows the hint");
  CHECK(server_request_close(server), "close the client connection");
  CHECK(wait_for_source(shell, "sdl", 2000),
        "disconnect falls back to SDL, got %s",
        bd_shell_input_source(shell));
  CHECK(!bd_shell_hint_visible(shell), "disconnect clears the hint hold");
  if (!wait_for_source(shell, "inputd", 4000)) {
    CHECK(false, "shell did not reconnect to the same listener (source %s)",
          bd_shell_input_source(shell));
    dump_file("fake server", runtime->fake_log);
    return;
  }
  CHECK(server_wait_count(server, "\"cmd\":\"subscribe\"", 2, 2000),
        "reconnect subscribes again");
  CHECK(!bd_shell_hint_visible(shell),
        "reconnect must not restore the interrupted hold");
  CHECK(server_send(server, half), "send hint hold after reconnect");
  CHECK(wait_for_hint(shell, true, 1500),
        "subscription works after reconnect");
  CHECK(server_send(server,
                    "{\"type\":\"release_all\",\"time_ms\":2}\n"),
        "release after reconnect");
  CHECK(wait_for_hint(shell, false, 1500),
        "release after reconnect hides the hint");

  /* Daemon restart: the listener goes away and a new process takes over the
   * same path. */
  server_stop(server);
  CHECK(wait_for_source(shell, "sdl", 2000),
        "daemon exit falls back to SDL, got %s",
        bd_shell_input_source(shell));
  CHECK(server_start(server, runtime->input_socket, runtime->fake_log, 4),
        "restart fake inputd");
  if (!wait_for_source(shell, "inputd", 6000)) {
    CHECK(false, "shell did not connect to the restarted daemon (source %s)",
          bd_shell_input_source(shell));
    dump_file("fake server", runtime->fake_log);
    return;
  }
  CHECK(server_wait_count(server, "\"cmd\":\"subscribe\"", 3, 2000),
        "restarted daemon receives a fresh subscription");
  CHECK(strcmp(bd_shell_input_source(shell), "inputd") == 0,
        "input source is inputd after restart");

  /* Real daemon integration: replay holds Select, the shell shows the hint,
   * then the daemon is killed mid-hold and restarted with a different replay.
   * The old hold must not reappear; a later fresh hold must still arrive. */
  if (inputd == NULL) {
    CHECK(false, "real inputd path was not supplied");
    return;
  }
  server_stop(server);
  CHECK(wait_for_source(shell, "sdl", 2000), "stop fake peer before inputd");
  CHECK(write_text(runtime->replay,
                   "2000 key Select down\n"
                   "4500 key Select up\n"
                   "6000 quit\n"),
        "write inputd replay");
  pid_t pid = start_inputd(inputd, runtime->config_dir, runtime->input_socket,
                           runtime->replay, runtime->inputd_log);
  CHECK(pid > 0 && wait_for_path(runtime->input_socket, 3000),
        "start brickdesk-inputd");
  if (!wait_for_source(shell, "inputd", 6000)) {
    CHECK(false, "shell did not connect to brickdesk-inputd (source %s)",
          bd_shell_input_source(shell));
    dump_file("inputd", runtime->inputd_log);
    stop_inputd(pid);
    return;
  }
  CHECK(wait_for_hint(shell, true, 5000),
        "real inputd Select hold shows the hint");
  stop_inputd(pid);
  CHECK(wait_for_source(shell, "sdl", 2000),
        "inputd death falls back to SDL, got %s",
        bd_shell_input_source(shell));
  CHECK(!bd_shell_hint_visible(shell),
        "inputd death clears the active hint hold");

  CHECK(write_text(runtime->replay,
                   "4000 key Select down\n"
                   "5000 key Select up\n"
                   "7000 quit\n"),
        "write second inputd replay");
  pid = start_inputd(inputd, runtime->config_dir, runtime->input_socket,
                     runtime->replay, runtime->inputd_log);
  CHECK(pid > 0 && wait_for_path(runtime->input_socket, 3000),
        "restart brickdesk-inputd");
  if (!wait_for_source(shell, "inputd", 6000)) {
    CHECK(false, "shell did not reconnect to brickdesk-inputd (source %s)",
          bd_shell_input_source(shell));
    dump_file("inputd", runtime->inputd_log);
    stop_inputd(pid);
    return;
  }
  CHECK(pump_for(shell, 1500), "settle after inputd restart");
  CHECK(!bd_shell_hint_visible(shell),
        "restart does not replay the old hold");
  CHECK(wait_for_hint(shell, true, 4000),
        "fresh real inputd hold arrives after reconnect");
  stop_inputd(pid);
  CHECK(wait_for_hint(shell, false, 2000),
        "hint clears when the restarted daemon disappears");
}

static void test_subscriber_survives_reconnect(test_runtime *runtime,
                                               fake_server *server,
                                               const char *inputd) {
  bd_shell *shell = runtime->shell;
  server_stop(server);
  CHECK(wait_for_source(shell, "sdl", 2000),
        "Sdl before independent subscriber check");

  char real_socket[TEST_PATH_MAX];
  char proxy_log[TEST_PATH_MAX];
  CHECK(join(real_socket, sizeof(real_socket), runtime->root,
             "inputd-real.sock") &&
            join(proxy_log, sizeof(proxy_log), runtime->root, "proxy.log"),
        "build subscriber test paths");
  CHECK(write_text(runtime->replay,
                   "0 key Select down\n"
                   "15000 key Select up\n"
                   "20000 quit\n"),
        "write independent subscriber replay");

  pid_t pid = start_inputd(inputd, runtime->config_dir, real_socket,
                           runtime->replay, runtime->inputd_log);
  CHECK(pid > 0 && wait_for_path(real_socket, 3000),
        "start inputd for independent subscriber");
  if (pid <= 0) {
    return;
  }

  test_client subscriber;
  memset(&subscriber, 0, sizeof(subscriber));
  subscriber.fd = -1;
  CHECK(client_connect(&subscriber, real_socket),
        "connect an independent subscriber");
  if (subscriber.fd < 0) {
    stop_inputd(pid);
    return;
  }
  if (!client_wait_for(&subscriber, "\"action\":\"ui.hint\"", 3000) ||
      !client_wait_for(&subscriber, "\"phase\":\"down\"", 1000)) {
    CHECK(false, "independent subscriber never saw the active hold");
    dump_file("inputd", runtime->inputd_log);
    client_close(&subscriber);
    stop_inputd(pid);
    return;
  }

  socket_proxy proxy;
  memset(&proxy, 0, sizeof(proxy));
  proxy.pid = -1;
  proxy.command_fd = -1;
  CHECK(proxy_start(&proxy, runtime->input_socket, real_socket, proxy_log),
        "start proxy for the shell connection");
  if (!wait_for_source(shell, "inputd", 7000)) {
    CHECK(false, "shell did not connect through the proxy (source %s)",
          bd_shell_input_source(shell));
    dump_file("proxy", proxy_log);
    dump_file("inputd", runtime->inputd_log);
    proxy_stop(&proxy);
    client_close(&subscriber);
    stop_inputd(pid);
    return;
  }
  size_t mark = subscriber.history_length;
  CHECK(client_quiet_for(&subscriber, "\"type\":\"release_all\"", 500),
        "shell connection must not release another subscriber");
  CHECK(!client_contains_after(&subscriber, mark, "\"phase\":\"up\""),
        "shell connection must not end another subscriber's hold");

  CHECK(proxy_drop(&proxy), "drop the shell connection");
  CHECK(wait_for_source(shell, "sdl", 2000),
        "dropped proxy falls back to SDL, got %s",
        bd_shell_input_source(shell));
  if (!wait_for_source(shell, "inputd", 7000)) {
    CHECK(false, "shell did not reconnect through the proxy (source %s)",
          bd_shell_input_source(shell));
    dump_file("proxy", proxy_log);
    dump_file("inputd", runtime->inputd_log);
    proxy_stop(&proxy);
    client_close(&subscriber);
    stop_inputd(pid);
    return;
  }
  mark = subscriber.history_length;
  CHECK(client_quiet_for(&subscriber, "\"type\":\"release_all\"", 500),
        "shell reconnect must not release another subscriber");
  CHECK(!client_contains_after(&subscriber, mark, "\"phase\":\"up\""),
        "shell reconnect must not end another subscriber's hold");

  bool released = client_wait_for(&subscriber, "\"phase\":\"up\"", 20000);
  if (!released) {
    fprintf(stderr,
            "shell_inputd_test: subscriber history at missing release:\n%s\n",
            subscriber.history);
    dump_file("proxy", proxy_log);
    dump_file("inputd", runtime->inputd_log);
  }
  CHECK(released, "held input must release at its real replay time");

  proxy_stop(&proxy);
  client_close(&subscriber);
  stop_inputd(pid);
}

static void test_congested_socket(test_runtime *runtime, fake_server *server) {
  bd_shell *shell = runtime->shell;
  server_stop(server);
  CHECK(wait_for_source(shell, "sdl", 2000), "Sdl before congestion");

  /* A listening socket with a zero backlog and no accept loop makes a
   * blocking connect wait for queue space.  Fill the queue, then let the
   * shell's retry run and prove each loop turn stays bounded. */
  pid_t silent = fork();
  if (silent == 0) {
    setsid();
    int listener = server_create_listener(runtime->input_socket, 0);
    if (listener < 0) {
      _exit(127);
    }
    for (;;) {
      pause();
    }
  }
  CHECK(silent > 0, "start congested listener");
  CHECK(wait_for_path(runtime->input_socket, 3000),
        "congested listener socket appears");

  int fillers[TEST_FILLERS];
  int filler_count = 0;
  for (int i = 0; i < TEST_FILLERS; ++i) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      break;
    }
    server_set_nonblocking(fd);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    size_t path_length = strlen(runtime->input_socket);
    if (path_length >= sizeof(address.sun_path)) {
      close(fd);
      break;
    }
    memcpy(address.sun_path, runtime->input_socket, path_length + 1);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
      fillers[filler_count++] = fd;
    } else {
      close(fd);
    }
  }
  CHECK(filler_count > 0, "filled the congested accept queue (%d)",
        filler_count);

  uint64_t started = real_ms();
  uint64_t worst = 0;
  int iterations = 0;
  while (real_ms() - started < 3000) {
    uint64_t before = real_ms();
    int result = bd_shell_iterate(shell, 10);
    uint64_t elapsed = real_ms() - before;
    if (elapsed > worst) {
      worst = elapsed;
    }
    ++iterations;
    CHECK(result >= 0, "shell loop stays alive while congested");
    if (strcmp(bd_shell_input_source(shell), "inputd") == 0) {
      break;
    }
    sleep_ms(5);
  }
  CHECK(strcmp(bd_shell_input_source(shell), "sdl") == 0,
        "a congested socket cannot become the input source");
  CHECK(worst < 200, "congested connect blocks the loop for %llu ms",
        (unsigned long long)worst);
  CHECK(iterations < 500, "congested retry busy-polls (%d turns)", iterations);

  for (int i = 0; i < filler_count; ++i) {
    close(fillers[i]);
  }
  if (silent > 0) {
    kill(silent, SIGKILL);
    waitpid(silent, NULL, 0);
  }
  unlink(runtime->input_socket);

  CHECK(server_start(server, runtime->input_socket, runtime->fake_log, 4),
        "restart fake inputd after congestion");
  CHECK(wait_for_source(shell, "inputd", 7000),
        "recovers after congestion (source %s)",
        bd_shell_input_source(shell));
}

static void test_unreachable_idle(test_runtime *runtime, fake_server *server) {
  bd_shell *shell = runtime->shell;
  server_stop(server);
  CHECK(wait_for_source(shell, "sdl", 2000), "Sdl before idle measurement");
  CHECK(pump_for(shell, 500), "settle before idle measurement");
  clock_t cpu_start = clock();
  uint64_t wall_start = real_ms();
  int iterations = 0;
  while (real_ms() - wall_start < 1000) {
    if (bd_shell_iterate(shell, 100) < 0) {
      CHECK(false, "shell loop quit during idle measurement");
      return;
    }
    ++iterations;
  }
  double cpu_ms = (double)(clock() - cpu_start) * 1000.0 / CLOCKS_PER_SEC;
  CHECK(cpu_ms < 400.0,
        "unreachable inputd burns %.1f ms of CPU per second", cpu_ms);
  CHECK(iterations < 100,
        "unreachable inputd busy-polls (%d turns/second)", iterations);
  CHECK(strcmp(bd_shell_input_source(shell), "sdl") == 0,
        "input source stays SDL while unreachable");
}

int main(int argc, char **argv) {
  const char *inputd = NULL;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--inputd") == 0 && i + 1 < argc) {
      inputd = argv[++i];
    } else {
      fprintf(stderr, "usage: bd_shell_inputd_test [--inputd PATH]\n");
      return 2;
    }
  }
  signal(SIGPIPE, SIG_IGN);
  setenv("SDL_VIDEODRIVER", "offscreen", 1);
  setenv("SDL_AUDIODRIVER", "dummy", 1);
  setenv("BRICKDESK_HEADLESS", "1", 1);

  test_runtime runtime;
  if (!setup_runtime(&runtime)) {
    fprintf(stderr, "shell_inputd_test: setup failed\n");
    destroy_runtime(&runtime);
    return 1;
  }
  fake_server server;
  memset(&server, 0, sizeof(server));
  server.pid = -1;
  server.command_fd = -1;

  test_lifecycle(&runtime, &server, inputd);
  if (failures == 0) {
    test_subscriber_survives_reconnect(&runtime, &server, inputd);
  }
  if (failures == 0) {
    test_congested_socket(&runtime, &server);
  }
  if (failures == 0) {
    test_unreachable_idle(&runtime, &server);
  }
  server_stop(&server);
  destroy_runtime(&runtime);

  if (failures != 0) {
    fprintf(stderr, "shell_inputd_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("shell_inputd_test: reconnect, protocol, hold and idle checks passed\n");
  return 0;
}
