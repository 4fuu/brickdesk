#define _POSIX_C_SOURCE 200809L

#include "ipc.h"

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

static void set_error(char *error, size_t error_size, const char *message) {
  if (error != NULL && error_size > 0) {
    snprintf(error, error_size, "%s", message);
  }
}

bool bd_plugin_make_dirs(const char *path) {
  if (path == NULL || path[0] == '\0') {
    return false;
  }
  char buffer[1024];
  if (strlen(path) >= sizeof(buffer)) {
    return false;
  }
  snprintf(buffer, sizeof(buffer), "%s", path);
  size_t length = strlen(buffer);
  while (length > 1 && buffer[length - 1] == '/') {
    buffer[--length] = '\0';
  }
  for (char *cursor = buffer + 1; *cursor != '\0'; ++cursor) {
    if (*cursor != '/') {
      continue;
    }
    *cursor = '\0';
    if (mkdir(buffer, 0770) != 0 && errno != EEXIST) {
      return false;
    }
    *cursor = '/';
  }
  if (mkdir(buffer, 0770) != 0 && errno != EEXIST) {
    return false;
  }
  return true;
}

int bd_plugin_listen_socket(const char *path, char *error, size_t error_size) {
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (path == NULL || strlen(path) >= sizeof(address.sun_path)) {
    set_error(error, error_size, "plugin socket path is too long");
    return -1;
  }
  snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
  unlink(path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    set_error(error, error_size, "cannot create plugin socket");
    return -1;
  }
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    set_error(error, error_size, "cannot bind plugin socket");
    close(fd);
    return -1;
  }
  if (listen(fd, 4) != 0) {
    set_error(error, error_size, "cannot listen on plugin socket");
    close(fd);
    unlink(path);
    return -1;
  }
  chmod(path, 0660);
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  return fd;
}

int bd_plugin_connect_socket(const char *path, int timeout_ms, char *error,
                             size_t error_size) {
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (path == NULL || strlen(path) >= sizeof(address.sun_path)) {
    set_error(error, error_size, "plugin socket path is too long");
    return -1;
  }
  snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    set_error(error, error_size, "cannot create client socket");
    return -1;
  }
  int waited = 0;
  for (;;) {
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
      return fd;
    }
    if (errno != ENOENT && errno != ECONNREFUSED && errno != EAGAIN) {
      set_error(error, error_size, "cannot connect to the host socket");
      close(fd);
      return -1;
    }
    if (waited >= timeout_ms) {
      set_error(error, error_size, "host socket connect timed out");
      close(fd);
      return -1;
    }
    struct timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 10 * 1000 * 1000;
    nanosleep(&delay, NULL);
    waited += 10;
  }
}

bool bd_plugin_send_all(int fd, const char *data, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    ssize_t result = send(fd, data + sent, length - sent, MSG_NOSIGNAL);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (result == 0) {
      return false;
    }
    sent += (size_t)result;
  }
  return true;
}

bool bd_plugin_send_line(int fd, const char *line, size_t length) {
  if (line == NULL || length + 2 > 1024 * 1024) {
    return false;
  }
  char *buffer = (char *)malloc(length + 2);
  if (buffer == NULL) {
    return false;
  }
  memcpy(buffer, line, length);
  buffer[length] = '\n';
  bool ok = bd_plugin_send_all(fd, buffer, length + 1);
  free(buffer);
  return ok;
}
