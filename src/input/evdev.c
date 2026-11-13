#define _POSIX_C_SOURCE 200809L

#include "brickdesk/input.h"

#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
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
  if (strncmp(name, "event", 5) != 0 || name[5] == '\0') {
    return false;
  }
  for (const char *cursor = name + 5; *cursor != '\0'; ++cursor) {
    if (!isdigit((unsigned char)*cursor)) {
      return false;
    }
  }
  return true;
}

static int compare_strings(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

/* ------------------------------------------------------- test evdev backend */
/*
 * Real /dev/input/event* nodes are character devices. A FIFO named event*
 * under an explicit --device-dir is treated as a scripted device so the
 * daemon lifecycle can be exercised without uinput or hardware. The test
 * writer sends struct input_event records through the FIFO and describes
 * capabilities in <path>.meta.
 */

static void fake_set_key(bd_evdev *out, unsigned code) {
  if (code <= BD_EVDEV_KEY_MAX) {
    out->key_bits[code / 8u] |= (unsigned char)(1u << (code % 8u));
    out->has_key = true;
  }
}

static void fake_set_abs(bd_evdev *out, unsigned code) {
  if (code <= BD_EVDEV_ABS_MAX) {
    out->abs_bits[code / 8u] |= (unsigned char)(1u << (code % 8u));
    out->has_abs = true;
  }
}

static void fake_set_sw(bd_evdev *out, unsigned code) {
  if (code <= BD_EVDEV_SW_MAX) {
    out->sw_bits[code / 8u] |= (unsigned char)(1u << (code % 8u));
    out->has_sw = true;
  }
}

static void fake_defaults(bd_evdev *out) {
  const char *base = strrchr(out->path, '/');
  snprintf(out->name, sizeof(out->name), "%s", base != NULL ? base + 1
                                                           : out->path);
  out->is_fake = true;
  out->fake_grab_allowed = true;
  static const unsigned keys[] = {
      BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR,
      BTN_TL2,   BTN_TR2,   BTN_SELECT, BTN_START, BTN_MODE, BTN_DPAD_UP,
      BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
  };
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    fake_set_key(out, keys[i]);
  }
  fake_set_abs(out, ABS_HAT0X);
  fake_set_abs(out, ABS_HAT0Y);
  out->is_gamepad = true;
}

static char *trim_text(char *text) {
  while (*text != '\0' && isspace((unsigned char)*text)) {
    ++text;
  }
  char *end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) {
    --end;
  }
  *end = '\0';
  return text;
}

static bool parse_meta_u16(const char *text, uint16_t *out) {
  errno = 0;
  char *end = NULL;
  unsigned long value = strtoul(text, &end, 0);
  if (errno != 0 || end == text || *end != '\0' || value > 0xffffu) {
    return false;
  }
  *out = (uint16_t)value;
  return true;
}

static bool parse_meta_sw(const char *text, unsigned *out) {
  errno = 0;
  char *end = NULL;
  unsigned long numeric = strtoul(text, &end, 0);
  if (errno == 0 && end != text && *end == '\0' &&
      numeric <= BD_EVDEV_SW_MAX) {
    *out = (unsigned)numeric;
    return true;
  }
  for (unsigned code = 0; code <= BD_EVDEV_SW_MAX; ++code) {
    const char *name = bd_evdev_sw_name(code);
    if (name != NULL && strcasecmp(name, text) == 0) {
      *out = code;
      return true;
    }
  }
  return false;
}

static void fake_clear_key_bits(bd_evdev *out) {
  memset(out->key_bits, 0, sizeof(out->key_bits));
  out->has_key = false;
}

static void fake_clear_abs_bits(bd_evdev *out) {
  memset(out->abs_bits, 0, sizeof(out->abs_bits));
  out->has_abs = false;
}

static void fake_clear_sw_bits(bd_evdev *out) {
  memset(out->sw_bits, 0, sizeof(out->sw_bits));
  out->has_sw = false;
}

static void fake_load_metadata(bd_evdev *out) {
  fake_defaults(out);

  char meta_path[BD_EVDEV_PATH_MAX + 8];
  int written = snprintf(meta_path, sizeof(meta_path), "%s.meta", out->path);
  if (written <= 0 || (size_t)written >= sizeof(meta_path)) {
    return;
  }
  FILE *file = fopen(meta_path, "r");
  if (file == NULL) {
    return;
  }

  char line[256];
  while (fgets(line, sizeof(line), file) != NULL) {
    char *text = trim_text(line);
    if (*text == '\0' || *text == '#') {
      continue;
    }
    char *equals = strchr(text, '=');
    if (equals == NULL) {
      continue;
    }
    *equals = '\0';
    char *field = trim_text(text);
    char *value = trim_text(equals + 1);

    if (strcmp(field, "name") == 0) {
      snprintf(out->name, sizeof(out->name), "%s", value);
      continue;
    }
    if (strcmp(field, "vendor") == 0) {
      parse_meta_u16(value, &out->vendor);
      continue;
    }
    if (strcmp(field, "product") == 0) {
      parse_meta_u16(value, &out->product);
      continue;
    }
    if (strcmp(field, "grab") == 0) {
      out->fake_grab_allowed =
          strcasecmp(value, "deny") != 0 && strcasecmp(value, "false") != 0 &&
          strcmp(value, "0") != 0;
      continue;
    }
    if (strcmp(field, "keys") == 0) {
      fake_clear_key_bits(out);
      char *save = NULL;
      for (char *token = strtok_r(value, ",", &save); token != NULL;
           token = strtok_r(NULL, ",", &save)) {
        bd_device_map_type type;
        unsigned code = 0;
        int direction = 0;
        char *name = trim_text(token);
        if (bd_device_map_parse_value(name, &type, &code, &direction) &&
            type == BD_DEVICE_MAP_KEY) {
          fake_set_key(out, code);
        }
      }
      continue;
    }
    if (strcmp(field, "abs") == 0) {
      fake_clear_abs_bits(out);
      char *save = NULL;
      for (char *token = strtok_r(value, ",", &save); token != NULL;
           token = strtok_r(NULL, ",", &save)) {
        bd_device_map_type type;
        unsigned code = 0;
        int direction = 0;
        char *name = trim_text(token);
        if (bd_device_map_parse_value(name, &type, &code, &direction) &&
            type == BD_DEVICE_MAP_ABS) {
          fake_set_abs(out, code);
        }
      }
      continue;
    }
    if (strcmp(field, "sw") == 0 || strcmp(field, "switches") == 0) {
      fake_clear_sw_bits(out);
      char *save = NULL;
      for (char *token = strtok_r(value, ",", &save); token != NULL;
           token = strtok_r(NULL, ",", &save)) {
        unsigned code = 0;
        char *name = trim_text(token);
        if (parse_meta_sw(name, &code)) {
          fake_set_sw(out, code);
        }
      }
    }
  }
  fclose(file);

  out->is_gamepad = bd_evdev_has_key(out, BTN_GAMEPAD) ||
                    bd_evdev_has_key(out, BTN_SOUTH) ||
                    bd_evdev_has_abs(out, ABS_HAT0X) ||
                    bd_evdev_has_abs(out, ABS_HAT0Y);
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

  struct stat node;
  if (fstat(fd, &node) != 0) {
    bd_evdev_close(out);
    return false;
  }
  if (!S_ISCHR(node.st_mode)) {
    if (!S_ISFIFO(node.st_mode)) {
      bd_evdev_close(out);
      return false;
    }
    fake_load_metadata(out);
    if (!out->has_key && !out->has_abs && !out->has_sw) {
      bd_evdev_close(out);
      return false;
    }
    return true;
  }

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

int bd_evdev_scan_ex(const char *dir, bd_evdev *out, size_t max,
                     bool *incomplete, char *error, size_t error_size) {
  if (incomplete != NULL) {
    *incomplete = false;
  }
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
      if (incomplete != NULL) {
        *incomplete = true;
      }
      break;
    }
    int written = snprintf(paths[path_count], sizeof(paths[path_count]), "%s",
                           entry->d_name);
    if (written > 0 && (size_t)written < sizeof(paths[path_count])) {
      ++path_count;
    } else if (incomplete != NULL) {
      *incomplete = true;
    }
  }
  closedir(directory);

  qsort(paths, path_count, sizeof(paths[0]), compare_strings);

  size_t count = 0;
  for (size_t i = 0; i < path_count; ++i) {
    if (count >= max) {
      if (incomplete != NULL) {
        *incomplete = true;
      }
      break;
    }
    bd_evdev device;
    if (!open_device(root, paths[i], &device)) {
      if (incomplete != NULL) {
        *incomplete = true;
      }
      continue;
    }
    out[count++] = device;
  }
  return (int)count;
}

int bd_evdev_scan(const char *dir, bd_evdev *out, size_t max, char *error,
                  size_t error_size) {
  return bd_evdev_scan_ex(dir, out, max, NULL, error, error_size);
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
  if (device->is_fake) {
    if (!device->fake_grab_allowed) {
      errno = EBUSY;
      return false;
    }
    return true;
  }
  return ioctl(device->fd, EVIOCGRAB,
               (void *)(intptr_t)(grab ? 1 : 0)) == 0;
}
