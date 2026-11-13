#define _POSIX_C_SOURCE 200809L

#include "brickdesk/uinput.h"

#include "brickdesk/input.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct bd_uinput {
  int fd;
  unsigned char pressed[(KEY_MAX + 8) / 8];
};

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
}

static bool bit_test(const unsigned char *bits, size_t bits_size,
                     unsigned bit) {
  size_t byte = bit / 8u;
  if (byte >= bits_size) {
    return false;
  }
  return (bits[byte] & (unsigned char)(1u << (bit % 8u))) != 0;
}

static void bit_set(unsigned char *bits, size_t bits_size, unsigned bit,
                    bool value) {
  size_t byte = bit / 8u;
  if (byte >= bits_size) {
    return;
  }
  unsigned char mask = (unsigned char)(1u << (bit % 8u));
  if (value) {
    bits[byte] |= mask;
  } else {
    bits[byte] &= (unsigned char)~mask;
  }
}

static bool copy_rel_bits(int source_fd, bd_uinput *uinput, char *error,
                          size_t error_size) {
  unsigned long bits[(REL_MAX + 8 * sizeof(unsigned long)) /
                     (8 * sizeof(unsigned long))];
  memset(bits, 0, sizeof(bits));
  if (ioctl(source_fd, EVIOCGBIT(EV_REL, sizeof(bits)), bits) < 0) {
    return true;
  }
  for (unsigned bit = 0; bit <= REL_MAX; ++bit) {
    size_t word = bit / (8u * sizeof(unsigned long));
    if ((bits[word] & (1ul << (bit % (8u * sizeof(unsigned long))))) == 0) {
      continue;
    }
    if (ioctl(uinput->fd, UI_SET_RELBIT, bit) < 0) {
      set_error(error, error_size, "UI_SET_RELBIT %u: %s", bit,
                strerror(errno));
      return false;
    }
  }
  return true;
}

static bool copy_abs_bits(const bd_evdev *source, bd_uinput *uinput,
                          char *error, size_t error_size) {
  for (unsigned bit = 0; bit <= BD_EVDEV_ABS_MAX; ++bit) {
    if (!bit_test(source->abs_bits, sizeof(source->abs_bits), bit)) {
      continue;
    }
    struct uinput_abs_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.code = (uint16_t)bit;
    if (source->fd < 0 ||
        ioctl(source->fd, EVIOCGABS(bit), &setup.absinfo) < 0) {
      continue;
    }
    if (ioctl(uinput->fd, UI_SET_ABSBIT, bit) < 0) {
      set_error(error, error_size, "UI_SET_ABSBIT %u: %s", bit,
                strerror(errno));
      return false;
    }
    if (ioctl(uinput->fd, UI_ABS_SETUP, &setup) < 0) {
      set_error(error, error_size, "UI_ABS_SETUP %u: %s", bit,
                strerror(errno));
      return false;
    }
  }
  return true;
}

bd_uinput *bd_uinput_create(const bd_evdev *source, char *error,
                            size_t error_size) {
  if (source == NULL) {
    set_error(error, error_size, "missing source device");
    return NULL;
  }
  int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    set_error(error, error_size, "/dev/uinput: %s", strerror(errno));
    return NULL;
  }
  bd_uinput *uinput = (bd_uinput *)calloc(1, sizeof(*uinput));
  if (uinput == NULL) {
    close(fd);
    set_error(error, error_size, "out of memory");
    return NULL;
  }
  uinput->fd = fd;

  if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
      ioctl(fd, UI_SET_EVBIT, EV_REL) < 0 ||
      ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
      ioctl(fd, UI_SET_EVBIT, EV_MSC) < 0 ||
      ioctl(fd, UI_SET_EVBIT, EV_SW) < 0) {
    set_error(error, error_size, "UI_SET_EVBIT: %s", strerror(errno));
    goto fail;
  }

  bool any = false;
  for (unsigned bit = 0; bit <= BD_EVDEV_KEY_MAX; ++bit) {
    if (!bit_test(source->key_bits, sizeof(source->key_bits), bit)) {
      continue;
    }
    if (ioctl(fd, UI_SET_KEYBIT, bit) < 0) {
      set_error(error, error_size, "UI_SET_KEYBIT %u: %s", bit,
                strerror(errno));
      goto fail;
    }
    any = true;
  }
  for (unsigned bit = 0; bit <= BD_EVDEV_SW_MAX; ++bit) {
    if (!bit_test(source->sw_bits, sizeof(source->sw_bits), bit)) {
      continue;
    }
    if (ioctl(fd, UI_SET_SWBIT, bit) < 0) {
      set_error(error, error_size, "UI_SET_SWBIT %u: %s", bit,
                strerror(errno));
      goto fail;
    }
    any = true;
  }
  if (source->fd >= 0 && !copy_rel_bits(source->fd, uinput, error,
                                        error_size)) {
    goto fail;
  }
  if (!copy_abs_bits(source, uinput, error, error_size)) {
    goto fail;
  }
  if (source->has_abs) {
    any = true;
  }
  if (!any) {
    set_error(error, error_size, "device has no forwardable capabilities");
    goto fail;
  }

  struct uinput_setup setup;
  memset(&setup, 0, sizeof(setup));
  snprintf(setup.name, sizeof(setup.name), "brickdesk %.64s",
           source->name[0] != '\0' ? source->name : "gamepad");
  setup.id.bustype = source->bustype;
  setup.id.vendor = source->vendor;
  setup.id.product = source->product;
  setup.id.version = source->version;
  if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
    set_error(error, error_size, "UI_DEV_SETUP: %s", strerror(errno));
    goto fail;
  }
  if (ioctl(fd, UI_DEV_CREATE) < 0) {
    set_error(error, error_size, "UI_DEV_CREATE: %s", strerror(errno));
    goto fail;
  }
  return uinput;

fail:
  close(fd);
  free(uinput);
  return NULL;
}

bool bd_uinput_emit(bd_uinput *uinput, uint16_t type, uint16_t code,
                    int32_t value) {
  if (uinput == NULL || uinput->fd < 0) {
    return false;
  }
  if (type == EV_KEY && code <= KEY_MAX) {
    bit_set(uinput->pressed, sizeof(uinput->pressed), code, value != 0);
  }
  struct input_event event;
  memset(&event, 0, sizeof(event));
  event.type = type;
  event.code = code;
  event.value = value;
  ssize_t written = write(uinput->fd, &event, sizeof(event));
  return written == (ssize_t)sizeof(event);
}

void bd_uinput_sync(bd_uinput *uinput) {
  bd_uinput_emit(uinput, EV_SYN, SYN_REPORT, 0);
}

void bd_uinput_release_all(bd_uinput *uinput) {
  if (uinput == NULL) {
    return;
  }
  for (unsigned key = 0; key <= KEY_MAX; ++key) {
    if (bit_test(uinput->pressed, sizeof(uinput->pressed), key)) {
      bd_uinput_emit(uinput, EV_KEY, (uint16_t)key, 0);
    }
  }
  bd_uinput_sync(uinput);
}

void bd_uinput_destroy(bd_uinput *uinput) {
  if (uinput == NULL) {
    return;
  }
  if (uinput->fd >= 0) {
    ioctl(uinput->fd, UI_DEV_DESTROY);
    close(uinput->fd);
    uinput->fd = -1;
  }
  free(uinput);
}
