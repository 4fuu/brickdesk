#define _POSIX_C_SOURCE 200809L

#include "device_process.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BD_PROCESS_TIMEOUT_MS 3000

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
}

static uint64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

bool bd_process_run(char *const argv[], char *output, size_t output_size,
                    int *exit_code, char *error, size_t error_size) {
  if (output != NULL && output_size > 0) {
    output[0] = '\0';
  }
  if (exit_code != NULL) {
    *exit_code = -1;
  }
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    set_error(error, error_size, "pipe: %s", strerror(errno));
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    int saved = errno;
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    set_error(error, error_size, "fork: %s", strerror(saved));
    return false;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    dup2(pipe_fds[1], STDOUT_FILENO);
    dup2(pipe_fds[1], STDERR_FILENO);
    close(pipe_fds[1]);
    execvp(argv[0], argv);
    _exit(127);
  }
  close(pipe_fds[1]);
  int flags = fcntl(pipe_fds[0], F_GETFL, 0);
  if (flags >= 0) {
    fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
  }
  size_t used = 0;
  uint64_t start = monotonic_ms();
  bool exited = false;
  int status = 0;
  while (!exited) {
    struct pollfd descriptor = {.fd = pipe_fds[0], .events = POLLIN};
    int ready = poll(&descriptor, 1, 50);
    if (ready > 0 && (descriptor.revents & (POLLIN | POLLHUP)) != 0) {
      char buffer[512];
      ssize_t amount = read(pipe_fds[0], buffer, sizeof(buffer));
      if (amount > 0 && output != NULL && used + (size_t)amount < output_size) {
        memcpy(output + used, buffer, (size_t)amount);
        used += (size_t)amount;
        output[used] = '\0';
      }
    }
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      exited = true;
    } else if (result < 0 && errno != EINTR) {
      break;
    }
    if (!exited && monotonic_ms() - start > BD_PROCESS_TIMEOUT_MS) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      exited = true;
      break;
    }
  }
  for (;;) {
    char buffer[512];
    ssize_t amount = read(pipe_fds[0], buffer, sizeof(buffer));
    if (amount <= 0) {
      break;
    }
    if (output != NULL && used + (size_t)amount < output_size) {
      memcpy(output + used, buffer, (size_t)amount);
      used += (size_t)amount;
      output[used] = '\0';
    }
  }
  close(pipe_fds[0]);
  if (exit_code != NULL) {
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
    set_error(error, error_size, "%s: command not found", argv[0]);
    return false;
  }
  return true;
}

bool bd_process_spawn(char *const argv[], char *error, size_t error_size) {
  pid_t pid = fork();
  if (pid < 0) {
    set_error(error, error_size, "fork: %s", strerror(errno));
    return false;
  }
  if (pid == 0) {
    setsid();
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
      dup2(null_fd, STDIN_FILENO);
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
      if (null_fd > STDERR_FILENO) {
        close(null_fd);
      }
    }
    execvp(argv[0], argv);
    _exit(127);
  }
  return true;
}
