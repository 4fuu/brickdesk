#define _POSIX_C_SOURCE 200809L

#include "brickdesk/input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/input.h>
#else
#error "brickdesk input needs Linux evdev"
#endif

#define BITSET_LONGS(max) ((max + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long)))

static uint64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static bool bit_test(const unsigned char *bits, size_t bits_size,
                     unsigned bit) {
  size_t byte = bit / 8u;
  if (byte >= bits_size) {
    return false;
  }
  return (bits[byte] & (unsigned char)(1u << (bit % 8u))) != 0;
}

static void copy_bits(unsigned char *dst, size_t dst_size,
                      const unsigned long *src, size_t src_longs,
                      unsigned max_bit) {
  memset(dst, 0, dst_size);
  for (unsigned bit = 0; bit <= max_bit; ++bit) {
    size_t word = bit / (8u * sizeof(unsigned long));
    if (word >= src_longs) {
      break;
    }
    if ((src[word] & (1ul << (bit % (8u * sizeof(unsigned long))))) != 0) {
      size_t byte = bit / 8u;
      if (byte < dst_size) {
        dst[byte] |= (unsigned char)(1u << (bit % 8u));
      }
    }
  }
}

static bool path_is_event(const char *name) {
  return strncmp(name, "event", 5) == 0 && name[5] != '\0';
}

static int compare_strings(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

static bool open_device(const char *dir, const char *name, bd_evdev *out) {
  memset(out, 0, sizeof(*out));
  out->fd = -1;
  int written = snprintf(out->path, sizeof(out->path), "%s/%s", dir, name);
  if (written <= 0 || (size_t)written >= sizeof(out->path)) {
    return false;
  }
  int fd = open(out->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  out->fd = fd;

  char name_buffer[BD_EVDEV_NAME_MAX] = {0};
  if (ioctl(fd, EVIOCGNAME(sizeof(name_buffer) - 1), name_buffer) >= 0) {
    snprintf(out->name, sizeof(out->name), "%s", name_buffer);
  } else {
    snprintf(out->name, sizeof(out->name), "%s", name);
  }
  struct input_id ids;
  memset(&ids, 0, sizeof(ids));
  if (ioctl(fd, EVIOCGID, &ids) == 0) {
    out->bustype = ids.bustype;
    out->vendor = ids.vendor;
    out->product = ids.product;
    out->version = ids.version;
  }

  unsigned long event_bits[BITSET_LONGS(EV_MAX)] = {0};
  if (ioctl(fd, EVIOCGBIT(0, sizeof(event_bits)), event_bits) >= 0) {
    out->has_key = (event_bits[EV_KEY / (8u * sizeof(unsigned long))] &
                    (1ul << (EV_KEY % (8u * sizeof(unsigned long))))) != 0;
    out->has_abs = (event_bits[EV_ABS / (8u * sizeof(unsigned long))] &
                    (1ul << (EV_ABS % (8u * sizeof(unsigned long))))) != 0;
    out->has_sw = (event_bits[EV_SW / (8u * sizeof(unsigned long))] &
                   (1ul << (EV_SW % (8u * sizeof(unsigned long))))) != 0;
  }

  if (out->has_key) {
    unsigned long key_bits[BITSET_LONGS(KEY_MAX)] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
      copy_bits(out->key_bits, sizeof(out->key_bits), key_bits,
                sizeof(key_bits) / sizeof(key_bits[0]), BD_EVDEV_KEY_MAX);
    }
  }
  if (out->has_abs) {
    unsigned long abs_bits[BITSET_LONGS(ABS_MAX)] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) >= 0) {
      copy_bits(out->abs_bits, sizeof(out->abs_bits), abs_bits,
                sizeof(abs_bits) / sizeof(abs_bits[0]), BD_EVDEV_ABS_MAX);
    }
  }
  if (out->has_sw) {
    unsigned long sw_bits[BITSET_LONGS(SW_MAX)] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof(sw_bits)), sw_bits) >= 0) {
      copy_bits(out->sw_bits, sizeof(out->sw_bits), sw_bits,
                sizeof(sw_bits) / sizeof(sw_bits[0]), BD_EVDEV_SW_MAX);
    }
  }

  out->is_gamepad =
      bd_evdev_has_key(out, BTN_GAMEPAD) || bd_evdev_has_key(out, BTN_SOUTH) ||
      bd_evdev_has_abs(out, ABS_HAT0X) || bd_evdev_has_abs(out, ABS_HAT0Y);

  if (!out->has_key && !out->has_abs && !out->has_sw) {
    bd_evdev_close(out);
    return false;
  }
  return true;
}

int bd_evdev_scan(const char *dir, bd_evdev *out, size_t max, char *error,
                  size_t error_size) {
  if (out == NULL || max == 0) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "invalid device buffer");
    }
    return -1;
  }
  const char *root = dir != NULL && *dir != '\0' ? dir : "/dev/input";
  DIR *directory = opendir(root);
  if (directory == NULL) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "%s: %s", root, strerror(errno));
    }
    return -1;
  }

  char paths[BD_EVDEV_MAX_DEVICES * 2][BD_EVDEV_PATH_MAX];
  size_t path_count = 0;
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (!path_is_event(entry->d_name)) {
      continue;
    }
    if (path_count >= sizeof(paths) / sizeof(paths[0])) {
      break;
    }
    int written = snprintf(paths[path_count], sizeof(paths[path_count]), "%s",
                           entry->d_name);
    if (written > 0 && (size_t)written < sizeof(paths[path_count])) {
      ++path_count;
    }
  }
  closedir(directory);

  qsort(paths, path_count, sizeof(paths[0]), compare_strings);

  size_t count = 0;
  for (size_t i = 0; i < path_count && count < max; ++i) {
    bd_evdev device;
    if (!open_device(root, paths[i], &device)) {
      continue;
    }
    out[count++] = device;
  }
  return (int)count;
}

void bd_evdev_close(bd_evdev *device) {
  if (device == NULL) {
    return;
  }
  if (device->fd >= 0) {
    close(device->fd);
    device->fd = -1;
  }
}

bool bd_evdev_has_key(const bd_evdev *device, unsigned code) {
  if (device == NULL || code > BD_EVDEV_KEY_MAX) {
    return false;
  }
  return bit_test(device->key_bits, sizeof(device->key_bits), code);
}

bool bd_evdev_has_abs(const bd_evdev *device, unsigned code) {
  if (device == NULL || code > BD_EVDEV_ABS_MAX) {
    return false;
  }
  return bit_test(device->abs_bits, sizeof(device->abs_bits), code);
}

bool bd_evdev_has_sw(const bd_evdev *device, unsigned code) {
  if (device == NULL || code > BD_EVDEV_SW_MAX) {
    return false;
  }
  return bit_test(device->sw_bits, sizeof(device->sw_bits), code);
}

int bd_evdev_read(const bd_evdev *device, bd_evdev_event *out) {
  if (device == NULL || out == NULL || device->fd < 0) {
    return -1;
  }
  struct input_event raw;
  ssize_t bytes = read(device->fd, &raw, sizeof(raw));
  if (bytes < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return 0;
    }
    return -1;
  }
  if (bytes == 0) {
    return 0;
  }
  if ((size_t)bytes != sizeof(raw)) {
    return 0;
  }

  memset(out, 0, sizeof(*out));
  out->time_ms = monotonic_ms();
  if (raw.type == EV_KEY) {
    out->kind = BD_EVDEV_EVENT_KEY;
    out->code = raw.code;
    out->value = raw.value;
    return 1;
  }
  if (raw.type == EV_ABS) {
    out->kind = BD_EVDEV_EVENT_ABS;
    out->code = raw.code;
    out->value = raw.value;
    return 1;
  }
  if (raw.type == EV_SW) {
    out->kind = BD_EVDEV_EVENT_SWITCH;
    out->code = raw.code;
    out->value = raw.value;
    return 1;
  }
  if (raw.type == EV_SYN && raw.code == SYN_DROPPED) {
    out->kind = BD_EVDEV_EVENT_SYN_DROPPED;
    return 1;
  }
  out->kind = BD_EVDEV_EVENT_NONE;
  return 1;
}

bool bd_evdev_key_bits(const bd_evdev *device, unsigned char *bits,
                       size_t bits_size) {
  if (device == NULL || device->fd < 0 || bits == NULL || bits_size == 0) {
    return false;
  }
  unsigned long key_bits[BITSET_LONGS(KEY_MAX)] = {0};
  if (ioctl(device->fd, EVIOCGKEY(sizeof(key_bits)), key_bits) < 0) {
    return false;
  }
  copy_bits(bits, bits_size, key_bits,
            sizeof(key_bits) / sizeof(key_bits[0]), BD_EVDEV_KEY_MAX);
  return true;
}

int bd_evdev_abs_value(const bd_evdev *device, unsigned code) {
  if (device == NULL || device->fd < 0 || code > BD_EVDEV_ABS_MAX) {
    return 0;
  }
  struct input_absinfo info;
  memset(&info, 0, sizeof(info));
  if (ioctl(device->fd, EVIOCGABS(code), &info) < 0) {
    return 0;
  }
  return info.value;
}

bool bd_evdev_grab(bd_evdev *device, bool grab) {
  if (device == NULL || device->fd < 0) {
    errno = EBADF;
    return false;
  }
  return ioctl(device->fd, EVIOCGRAB,
               (void *)(intptr_t)(grab ? 1 : 0)) == 0;
}
