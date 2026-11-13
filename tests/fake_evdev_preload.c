#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/*
 * Test-only LD_PRELOAD shim.  bd-feedback is run as a real child process with
 * an explicit /dev/input/event7 path; this shim supplies a fake FF_RUMBLE
 * device and logs open/upload/play/stop/erase/close with monotonic timestamps.
 * It is not shipped and does not touch real input devices.
 */

static int (*real_open)(const char *, int, ...);
static int (*real_open_2)(const char *, int);
static int (*real_open64_2)(const char *, int);
static int (*real_ioctl)(int, unsigned long, ...);
static ssize_t (*real_write)(int, const void *, size_t);
static int (*real_close)(int);
static int log_fd = -1;

static void init(void) {
  if (real_open == NULL) {
    real_open = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
    real_open_2 =
        (int (*)(const char *, int))dlsym(RTLD_NEXT, "__open_2");
    real_open64_2 =
        (int (*)(const char *, int))dlsym(RTLD_NEXT, "__open64_2");
    real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    real_write =
        (ssize_t (*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
    real_close = (int (*)(int))dlsym(RTLD_NEXT, "close");
  }
  if (log_fd < 0) {
    const char *path = getenv("BD_FAKE_EVDEV_LOG");
    if (path != NULL) {
      log_fd = real_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
  }
}

static void log_event(const char *name, long long value) {
  init();
  if (log_fd < 0) {
    return;
  }
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  char line[160];
  int written = snprintf(line, sizeof(line), "%lld.%09ld %s %lld\n",
                         (long long)now.tv_sec, now.tv_nsec, name, value);
  if (written > 0) {
    real_write(log_fd, line, (size_t)written);
  }
}

int open(const char *path, int flags, ...) {
  init();
  if (strcmp(path, "/dev/input/event7") == 0) {
    log_event("open", 0);
    return 987;
  }
  mode_t mode = 0;
  if ((flags & O_CREAT) != 0) {
    va_list args;
    va_start(args, flags);
    mode = (mode_t)va_arg(args, int);
    va_end(args);
  }
  return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...) {
  init();
  if (strcmp(path, "/dev/input/event7") == 0) {
    log_event("open", 0);
    return 987;
  }
  mode_t mode = 0;
  if ((flags & O_CREAT) != 0) {
    va_list args;
    va_start(args, flags);
    mode = (mode_t)va_arg(args, int);
    va_end(args);
  }
  static int (*real_open64)(const char *, int, ...);
  if (real_open64 == NULL) {
    real_open64 =
        (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open64");
  }
  return real_open64(path, flags, mode);
}

/* Ubuntu enables _FORTIFY_SOURCE, which turns two-argument open() into these
 * glibc entry points.  They must be interposed too or the fake event node is
 * bypassed in CI. */
int __open_2(const char *path, int flags) {
  init();
  if (strcmp(path, "/dev/input/event7") == 0) {
    log_event("open", 0);
    return 987;
  }
  if (real_open_2 != NULL) {
    return real_open_2(path, flags);
  }
  return real_open(path, flags, 0);
}

int __open64_2(const char *path, int flags) {
  init();
  if (strcmp(path, "/dev/input/event7") == 0) {
    log_event("open", 0);
    return 987;
  }
  if (real_open64_2 != NULL) {
    return real_open64_2(path, flags);
  }
  if (real_open_2 != NULL) {
    return real_open_2(path, flags);
  }
  return real_open(path, flags, 0);
}

static void set_bit(void *arg, unsigned int bit) {
  unsigned long *bits = (unsigned long *)arg;
  bits[bit / (8 * sizeof(unsigned long))] |=
      1UL << (bit % (8 * sizeof(unsigned long)));
}

int ioctl(int fd, unsigned long request, ...) {
  init();
  va_list args;
  va_start(args, request);
  void *arg = va_arg(args, void *);
  va_end(args);
  if (fd != 987) {
    return real_ioctl(fd, request, arg);
  }
  unsigned int nr = _IOC_NR(request);
  if (nr == 0x20u) {
    set_bit(arg, EV_FF);
    log_event("gbit0", 0);
    return 0;
  }
  if (nr == 0x20u + EV_FF) {
    set_bit(arg, FF_RUMBLE);
    log_event("gbitff", 0);
    return 0;
  }
  if (nr == 0x84u) {
    *(int *)arg = 16;
    log_event("geffects", 16);
    return 0;
  }
  if (nr == 0x06u) {
    snprintf((char *)arg, 64, "Fake Preload Pad");
    return 17;
  }
  if (nr == 0x80u) {
    if (getenv("BD_FAKE_EVDEV_UPLOAD_FAIL") != NULL) {
      errno = EINVAL;
      log_event("upload_fail", 0);
      return -1;
    }
    struct ff_effect *effect = (struct ff_effect *)arg;
    effect->id = 3;
    log_event("upload", effect->id);
    return 0;
  }
  if (nr == 0x81u) {
    log_event("erase", (long long)(intptr_t)arg);
    return 0;
  }
  errno = ENOTTY;
  return -1;
}

ssize_t write(int fd, const void *data, size_t size) {
  init();
  if (fd != 987) {
    return real_write(fd, data, size);
  }
  const struct input_event *event = (const struct input_event *)data;
  if (size == sizeof(*event) && event->type == EV_FF) {
    log_event(event->value == 0 ? "ff_stop" : "ff_play", event->value);
  }
  return (ssize_t)size;
}

int close(int fd) {
  init();
  if (fd == 987) {
    log_event("close", 0);
    return 0;
  }
  return real_close(fd);
}
