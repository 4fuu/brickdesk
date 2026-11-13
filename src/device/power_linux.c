#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "brickdesk/device_power.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>

#define BD_POWER_I2C_DEVICE "/dev/i2c-6"
#define BD_POWER_AXP2202_ADDR 0x34
#define BD_POWER_DEEP_SLEEP_TRIES 5
#define BD_POWER_DEEP_SLEEP_RETRY_MS 3000
#define BD_POWER_DEEP_SLEEP_FALSE_NEGATIVE_MS 5000

static const char pmic_blocker[] =
    "AXP2202 soft power-off: /dev/i2c-6 is unavailable on this host; the "
    "TG4040 register sequence is implemented but has not run here.";

static void copy_text(char *dst, size_t dst_size, const char *src) {
  if (dst_size == 0) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  size_t length = strlen(src);
  if (length >= dst_size) {
    length = dst_size - 1;
  }
  memcpy(dst, src, length);
  dst[length] = '\0';
}

static void trim_text(char *text) {
  if (text == NULL) {
    return;
  }
  size_t length = strlen(text);
  while (length > 0 && isspace((unsigned char)text[length - 1])) {
    text[--length] = '\0';
  }
  char *start = text;
  while (*start != '\0' && isspace((unsigned char)*start)) {
    ++start;
  }
  if (start != text) {
    memmove(text, start, strlen(start) + 1);
  }
}

static bool build_path(const bd_power_linux *linux, const char *relative,
                       char *out, size_t out_size) {
  const char *root = linux->sysfs_root[0] != '\0' ? linux->sysfs_root : "/";
  size_t root_length = strlen(root);
  bool has_slash = root_length > 0 && root[root_length - 1] == '/';
  int written = snprintf(out, out_size, "%s%s%s", root,
                         has_slash ? "" : "/", relative);
  return written > 0 && (size_t)written < out_size;
}

static bool read_text(const char *path, char *out, size_t out_size) {
  if (path == NULL || out == NULL || out_size == 0) {
    return false;
  }
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  size_t used = fread(out, 1, out_size - 1, file);
  int saved = errno;
  bool ok = !ferror(file);
  out[used] = '\0';
  if (fclose(file) != 0 && ok) {
    ok = false;
    saved = errno;
  }
  if (!ok) {
    errno = saved;
    return false;
  }
  trim_text(out);
  return true;
}

static bool parse_int_text(const char *text, int minimum, int maximum,
                           int *out) {
  if (text == NULL || out == NULL || *text == '\0') {
    return false;
  }
  errno = 0;
  char *end = NULL;
  long value = strtol(text, &end, 10);
  if (errno != 0 || end == text) {
    return false;
  }
  while (*end != '\0' && isspace((unsigned char)*end)) {
    ++end;
  }
  if (*end != '\0' || value < minimum || value > maximum) {
    return false;
  }
  *out = (int)value;
  return true;
}

static bool token_exists(const char *text, const char *needle) {
  char copy[BD_POWER_TEXT_MAX];
  copy_text(copy, sizeof(copy), text);
  char *save = NULL;
  for (char *token = strtok_r(copy, " \t\r\n,", &save); token != NULL;
       token = strtok_r(NULL, " \t\r\n,", &save)) {
    if (strcmp(token, needle) == 0) {
      return true;
    }
  }
  return false;
}

static void parse_mem_sleep_current(const char *text, char *out,
                                    size_t out_size) {
  if (out_size == 0) {
    return;
  }
  out[0] = '\0';
  const char *open = strchr(text, '[');
  if (open == NULL) {
    return;
  }
  const char *close = strchr(open + 1, ']');
  if (close == NULL) {
    return;
  }
  size_t length = (size_t)(close - open - 1);
  if (length >= out_size) {
    length = out_size - 1;
  }
  memcpy(out, open + 1, length);
  out[length] = '\0';
  trim_text(out);
}

static bool mem_sleep_supports(const char *text, const char *mode) {
  char copy[BD_POWER_TEXT_MAX];
  copy_text(copy, sizeof(copy), text);
  char *save = NULL;
  for (char *token = strtok_r(copy, " \t\r\n", &save); token != NULL;
       token = strtok_r(NULL, " \t\r\n", &save)) {
    size_t length = strlen(token);
    if (length >= 2 && token[0] == '[' && token[length - 1] == ']') {
      memmove(token, token + 1, length - 2);
      token[length - 2] = '\0';
    }
    if (strcmp(token, mode) == 0) {
      return true;
    }
  }
  return false;
}

static bool write_all(int fd, const char *data, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    ssize_t written = write(fd, data + offset, length - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      errno = EIO;
      return false;
    }
    offset += (size_t)written;
  }
  return true;
}

static bool write_text_file(const char *path, const char *value) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  char line[64];
  int written = snprintf(line, sizeof(line), "%s\n", value);
  bool ok = written > 0 && (size_t)written < sizeof(line) &&
            write_all(fd, line, (size_t)written);
  int saved = errno;
  if (close(fd) != 0 && ok) {
    ok = false;
    saved = errno;
  }
  if (!ok) {
    errno = saved;
  }
  return ok;
}

static bool apply_mem_sleep_mode(const bd_power_linux *linux, char *previous,
                                 size_t previous_size) {
  previous[0] = '\0';
  if (linux->mem_sleep_mode[0] == '\0') {
    return true;
  }

  char path[BD_POWER_PATH_MAX];
  if (!build_path(linux, "sys/power/mem_sleep", path, sizeof(path))) {
    errno = ENAMETOOLONG;
    return false;
  }
  char text[BD_POWER_TEXT_MAX];
  if (!read_text(path, text, sizeof(text))) {
    errno = ENOTSUP;
    return false;
  }
  if (!mem_sleep_supports(text, linux->mem_sleep_mode)) {
    errno = EINVAL;
    return false;
  }

  char current[BD_POWER_TEXT_MAX];
  parse_mem_sleep_current(text, current, sizeof(current));
  copy_text(previous, previous_size, current);
  if (strcmp(current, linux->mem_sleep_mode) == 0) {
    return true;
  }
  if (!write_text_file(path, linux->mem_sleep_mode)) {
    return false;
  }
  if (!read_text(path, text, sizeof(text))) {
    errno = EIO;
    return false;
  }
  parse_mem_sleep_current(text, current, sizeof(current));
  if (strcmp(current, linux->mem_sleep_mode) != 0) {
    errno = EIO;
    return false;
  }
  return true;
}

static void restore_mem_sleep_mode(const bd_power_linux *linux,
                                   const char *previous) {
  if (linux->mem_sleep_mode[0] == '\0' || previous == NULL ||
      previous[0] == '\0') {
    return;
  }
  char path[BD_POWER_PATH_MAX];
  if (build_path(linux, "sys/power/mem_sleep", path, sizeof(path))) {
    int saved = errno;
    (void)write_text_file(path, previous);
    errno = saved;
  }
}

static uint64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void sleep_ms(unsigned milliseconds) {
  struct timespec delay;
  delay.tv_sec = (time_t)(milliseconds / 1000u);
  delay.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
  while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
  }
}

static bd_power_backend_status linux_deep_sleep(void *user) {
  bd_power_linux *linux = (bd_power_linux *)user;
  if (linux == NULL) {
    errno = EINVAL;
    return BD_POWER_BACKEND_ERROR;
  }
  if (!linux->allow_state_changes) {
    errno = EPERM;
    return BD_POWER_BACKEND_DISABLED;
  }

  char state_path[BD_POWER_PATH_MAX];
  char state_text[BD_POWER_TEXT_MAX];
  if (!build_path(linux, "sys/power/state", state_path, sizeof(state_path))) {
    errno = ENAMETOOLONG;
    return BD_POWER_BACKEND_ERROR;
  }
  if (!read_text(state_path, state_text, sizeof(state_text))) {
    return BD_POWER_BACKEND_ERROR;
  }
  if (!token_exists(state_text, "mem")) {
    errno = ENOTSUP;
    return BD_POWER_BACKEND_UNSUPPORTED;
  }

  char previous[BD_POWER_TEXT_MAX] = "";
  if (!apply_mem_sleep_mode(linux, previous, sizeof(previous))) {
    restore_mem_sleep_mode(linux, previous);
    return errno == EINVAL || errno == ENOTSUP || errno == ENOENT
               ? BD_POWER_BACKEND_UNSUPPORTED
               : BD_POWER_BACKEND_ERROR;
  }

  sync();

  /*
   * NextUI retries "mem" five times.  The vendor kernel occasionally returns
   * a non-zero write after a real suspend/resume cycle; a write that stayed
   * blocked for more than five seconds is therefore treated as success so the
   * device does not immediately suspend again after the user wakes it.
   */
  bd_power_backend_status result = BD_POWER_BACKEND_ERROR;
  for (int attempt = 0; attempt < BD_POWER_DEEP_SLEEP_TRIES; ++attempt) {
    if (attempt > 0) {
      sleep_ms(BD_POWER_DEEP_SLEEP_RETRY_MS);
    }
    uint64_t started = monotonic_ms();
    int fd = open(state_path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
      result = BD_POWER_BACKEND_ERROR;
      continue;
    }
    bool ok = write_all(fd, "mem\n", 4);
    int saved = errno;
    if (close(fd) != 0 && ok) {
      ok = false;
      saved = errno;
    }
    if (ok) {
      result = BD_POWER_BACKEND_OK;
      break;
    }
    uint64_t elapsed = monotonic_ms() - started;
    if (started != 0 && elapsed > BD_POWER_DEEP_SLEEP_FALSE_NEGATIVE_MS) {
      result = BD_POWER_BACKEND_OK;
      break;
    }
    errno = saved;
    result = BD_POWER_BACKEND_ERROR;
  }

  /* The requested mem_sleep mode is scoped to the transition, not permanent. */
  restore_mem_sleep_mode(linux, previous);
  return result;
}

/* ------------------------------------------------------------ PMIC path */

static bool axp2202_write(int fd, uint8_t reg, uint8_t value) {
  uint8_t buffer[2] = {reg, value};
  size_t offset = 0;
  while (offset < sizeof(buffer)) {
    ssize_t written = write(fd, buffer + offset, sizeof(buffer) - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      errno = EIO;
      return false;
    }
    offset += (size_t)written;
  }
  return true;
}

static bd_power_backend_status axp2202_soft_poweroff(bd_power_linux *linux) {
  const char *device = linux->i2c_device[0] != '\0' ? linux->i2c_device
                                                    : BD_POWER_I2C_DEVICE;
  int fd = open(device, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    return BD_POWER_BACKEND_ERROR;
  }
  if (ioctl(fd, I2C_SLAVE, BD_POWER_AXP2202_ADDR) < 0) {
    if (ioctl(fd, I2C_SLAVE_FORCE, BD_POWER_AXP2202_ADDR) < 0) {
      int saved = errno;
      close(fd);
      errno = saved;
      return BD_POWER_BACKEND_ERROR;
    }
  }

  bool ok = true;
  for (int reg = 0x40; reg <= 0x44; ++reg) {
    ok = axp2202_write(fd, (uint8_t)reg, 0x00) && ok;
  }
  for (int reg = 0x48; reg <= 0x4C; ++reg) {
    ok = axp2202_write(fd, (uint8_t)reg, 0xFF) && ok;
  }
  ok = axp2202_write(fd, 0x22, 0x0A) && ok;
  sleep_ms(50);
  ok = axp2202_write(fd, 0x27, 0x01) && ok;
  int saved = errno;
  close(fd);
  if (!ok) {
    errno = saved != 0 ? saved : EIO;
    return BD_POWER_BACKEND_ERROR;
  }
  /* The PMIC cuts VBUS/battery after the latch; give it a moment first. */
  sleep_ms(1000);
  return BD_POWER_BACKEND_OK;
}

static bd_power_backend_status linux_shutdown(void *user, bool reboot_system) {
  bd_power_linux *linux = (bd_power_linux *)user;
  if (linux == NULL) {
    errno = EINVAL;
    return BD_POWER_BACKEND_ERROR;
  }
  if (!linux->allow_state_changes) {
    errno = EPERM;
    return BD_POWER_BACKEND_DISABLED;
  }

  sync();
  if (!reboot_system && linux->safe_poweroff) {
    /*
     * NextUI unloads the SD card before touching the PMIC so its filesystem
     * cannot be left dirty by the hard cut.  MNT_DETACH is deliberate: the
     * launcher itself runs from the card, so a normal unmount would fail.
     */
    if (umount2("/mnt/SDCARD", MNT_FORCE | MNT_DETACH) != 0 &&
        errno != EINVAL && errno != ENOENT) {
      /* Not fatal: the PMIC path still has to be attempted. */
    }
    bd_power_backend_status pmic = axp2202_soft_poweroff(linux);
    if (pmic == BD_POWER_BACKEND_OK) {
      /* The PMIC sequence already waited; this is only a safety net. */
      sync();
      int command = RB_POWER_OFF;
      if (reboot(command) == 0) {
        errno = ENOTSUP;
        return BD_POWER_BACKEND_NO_EFFECT;
      }
      return BD_POWER_BACKEND_ERROR;
    }
    /* Fall through to the standard path; never leave the device in limbo. */
  }

  sync();
  int command = reboot_system ? RB_AUTOBOOT : RB_POWER_OFF;
  if (reboot(command) == 0) {
    errno = ENOTSUP;
    return BD_POWER_BACKEND_NO_EFFECT;
  }
  return BD_POWER_BACKEND_ERROR;
}

bool bd_power_linux_init(bd_power_linux *linux, const char *sysfs_root,
                         bool allow_state_changes,
                         const char *mem_sleep_mode, char *error,
                         size_t error_size) {
  if (linux == NULL) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "missing Linux power backend");
    }
    return false;
  }
  memset(linux, 0, sizeof(*linux));
  const char *root = (sysfs_root == NULL || *sysfs_root == '\0')
                         ? "/"
                         : sysfs_root;
  if (strlen(root) >= sizeof(linux->sysfs_root)) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "sysfs root is too long");
    }
    return false;
  }
  copy_text(linux->sysfs_root, sizeof(linux->sysfs_root), root);
  size_t root_length = strlen(linux->sysfs_root);
  while (root_length > 1 && linux->sysfs_root[root_length - 1] == '/') {
    linux->sysfs_root[--root_length] = '\0';
  }

  if (mem_sleep_mode != NULL && *mem_sleep_mode != '\0') {
    size_t length = strlen(mem_sleep_mode);
    if (length >= sizeof(linux->mem_sleep_mode) ||
        strspn(mem_sleep_mode, "abcdefghijklmnopqrstuvwxyz"
                               "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                               "0123456789_-") != length) {
      if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "invalid mem_sleep mode: %s",
                 mem_sleep_mode);
      }
      return false;
    }
    copy_text(linux->mem_sleep_mode, sizeof(linux->mem_sleep_mode),
              mem_sleep_mode);
  }
  linux->allow_state_changes = allow_state_changes;
  return true;
}

bool bd_power_linux_probe(const bd_power_linux *linux,
                          bd_power_probe *probe) {
  if (probe == NULL) {
    return false;
  }
  memset(probe, 0, sizeof(*probe));
  probe->read_only = true;
  probe->pmic_safe_poweroff = BD_POWER_SUPPORT_UNSUPPORTED;
  copy_text(probe->pmic_blocker, sizeof(probe->pmic_blocker), pmic_blocker);
  if (linux == NULL) {
    snprintf(probe->error, sizeof(probe->error),
             "missing Linux power backend");
    return false;
  }
  const char *i2c = linux->i2c_device[0] != '\0' ? linux->i2c_device
                                                : BD_POWER_I2C_DEVICE;
  if (access(i2c, F_OK) == 0) {
    probe->pmic_safe_poweroff = BD_POWER_SUPPORT_SUPPORTED;
    copy_text(probe->pmic_blocker, sizeof(probe->pmic_blocker),
              "AXP2202 soft power-off available through the vendor I2C bus; "
              "register writes must still be validated on TG4040 hardware.");
  } else {
    probe->pmic_safe_poweroff = BD_POWER_SUPPORT_UNSUPPORTED;
    copy_text(probe->pmic_blocker, sizeof(probe->pmic_blocker), pmic_blocker);
  }

  char path[BD_POWER_PATH_MAX];
  if (!build_path(linux, "sys/power/state", path, sizeof(path))) {
    snprintf(probe->error, sizeof(probe->error),
             "sysfs path is too long");
    return false;
  }
  if (!read_text(path, probe->states, sizeof(probe->states))) {
    snprintf(probe->error, sizeof(probe->error),
             "cannot read /sys/power/state: %s", strerror(errno));
    return false;
  }
  probe->state_readable = true;
  probe->mem_supported = token_exists(probe->states, "mem");

  if (build_path(linux, "sys/power/mem_sleep", path, sizeof(path)) &&
      read_text(path, probe->mem_sleep_modes,
                sizeof(probe->mem_sleep_modes))) {
    probe->mem_sleep_present = true;
    parse_mem_sleep_current(probe->mem_sleep_modes,
                            probe->mem_sleep_current,
                            sizeof(probe->mem_sleep_current));
  }

  if (build_path(linux, "sys/class/power_supply/axp2202-usb/online", path,
                 sizeof(path))) {
    char text[32];
    if (read_text(path, text, sizeof(text))) {
      int value = 0;
      if (parse_int_text(text, 0, 1, &value)) {
        probe->usb_online_known = true;
        probe->usb_online = value != 0;
      }
    }
  }

  if (build_path(linux, "sys/class/power_supply/axp2202-battery/capacity",
                 path, sizeof(path))) {
    char text[32];
    if (read_text(path, text, sizeof(text))) {
      int value = 0;
      if (parse_int_text(text, 0, 100, &value)) {
        probe->battery_capacity_known = true;
        probe->battery_capacity_percent = value;
      }
    }
  }
  return true;
}

bd_power_backend bd_power_linux_backend(bd_power_linux *linux) {
  bd_power_backend backend;
  backend.user = linux;
  backend.deep_sleep = linux != NULL ? linux_deep_sleep : NULL;
  backend.shutdown = linux != NULL ? linux_shutdown : NULL;
  return backend;
}

bd_power_backend_status bd_power_linux_pmic_poweroff(bd_power_linux *linux) {
  if (linux == NULL || !linux->allow_state_changes) {
    errno = EPERM;
    return BD_POWER_BACKEND_UNSUPPORTED;
  }
  if (!linux->safe_poweroff) {
    errno = ENOTSUP;
    return BD_POWER_BACKEND_UNSUPPORTED;
  }
  return axp2202_soft_poweroff(linux);
}

void bd_power_linux_set_safe_poweroff(bd_power_linux *linux, bool enabled) {
  if (linux != NULL) {
    linux->safe_poweroff = enabled;
  }
}

const char *bd_power_linux_pmic_blocker(void) {
  return pmic_blocker;
}
