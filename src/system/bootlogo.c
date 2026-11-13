#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "brickdesk/bootlogo.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define BD_BOOTLOGO_PARTITION "/dev/mmcblk0p1"
#define BD_BOOTLOGO_MOUNT "/tmp/bd-bootlogo"

const char *bd_bootlogo_result_name(bd_bootlogo_result result) {
  switch (result) {
    case BD_BOOTLOGO_OK: return "ok";
    case BD_BOOTLOGO_ERR_ARGUMENT: return "invalid-argument";
    case BD_BOOTLOGO_ERR_FORMAT: return "unsupported-format";
    case BD_BOOTLOGO_ERR_IO: return "io-error";
    case BD_BOOTLOGO_ERR_MOUNT: return "mount-error";
  }
  return "unknown";
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

static unsigned read_u16(const unsigned char *data) {
  return (unsigned)data[0] | ((unsigned)data[1] << 8);
}

static unsigned read_u32(const unsigned char *data) {
  return (unsigned)data[0] | ((unsigned)data[1] << 8) |
         ((unsigned)data[2] << 16) | ((unsigned)data[3] << 24);
}

bd_bootlogo_result bd_bootlogo_check(const char *path, bd_bootlogo_info *info,
                                     char *error, size_t error_size) {
  if (path == NULL || path[0] == '\0') {
    set_error(error, error_size, "missing BMP path");
    return BD_BOOTLOGO_ERR_ARGUMENT;
  }
  unsigned char header[54];
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    set_error(error, error_size, "cannot open %s: %s", path, strerror(errno));
    return BD_BOOTLOGO_ERR_IO;
  }
  size_t amount = fread(header, 1, sizeof(header), file);
  fseek(file, 0, SEEK_END);
  long length = ftell(file);
  fclose(file);
  if (amount < sizeof(header) || length < (long)sizeof(header)) {
    set_error(error, error_size, "%s is shorter than a BMP header", path);
    return BD_BOOTLOGO_ERR_FORMAT;
  }
  if (header[0] != 'B' || header[1] != 'M') {
    set_error(error, error_size, "%s is not a BMP file", path);
    return BD_BOOTLOGO_ERR_FORMAT;
  }
  unsigned data_offset = read_u32(header + 10);
  unsigned dib_size = read_u32(header + 14);
  int width = (int)read_u32(header + 18);
  int height = (int)read_u32(header + 22);
  unsigned planes = read_u16(header + 26);
  unsigned bpp = read_u16(header + 28);
  unsigned compression = read_u32(header + 30);
  if (dib_size < 40 || planes != 1 || (bpp != 24 && bpp != 32) ||
      compression != 0) {
    set_error(error, error_size,
              "%s: need uncompressed 24/32-bit BMP (dib=%u planes=%u bpp=%u "
              "compression=%u)",
              path, dib_size, planes, bpp, compression);
    return BD_BOOTLOGO_ERR_FORMAT;
  }
  if (width != BD_BOOTLOGO_WIDTH || height != BD_BOOTLOGO_HEIGHT) {
    set_error(error, error_size, "%s: expected %dx%d, got %dx%d", path,
              BD_BOOTLOGO_WIDTH, BD_BOOTLOGO_HEIGHT, width, height);
    return BD_BOOTLOGO_ERR_FORMAT;
  }
  size_t row = ((size_t)width * bpp + 31u) / 32u * 4u;
  size_t needed = (size_t)data_offset + row * (size_t)height;
  if (needed > (size_t)length) {
    set_error(error, error_size, "%s: truncated pixel data (%zu > %ld)", path,
              needed, length);
    return BD_BOOTLOGO_ERR_FORMAT;
  }
  if (info != NULL) {
    info->width = width;
    info->height = height;
    info->bits_per_pixel = (int)bpp;
    info->file_bytes = (unsigned)length;
  }
  return BD_BOOTLOGO_OK;
}

static bool copy_file(const char *from, const char *to, char *error,
                      size_t error_size) {
  FILE *source = fopen(from, "rb");
  if (source == NULL) {
    set_error(error, error_size, "cannot open %s: %s", from, strerror(errno));
    return false;
  }
  FILE *target = fopen(to, "wb");
  if (target == NULL) {
    fclose(source);
    set_error(error, error_size, "cannot create %s: %s", to, strerror(errno));
    return false;
  }
  char buffer[8192];
  bool ok = true;
  size_t amount;
  while ((amount = fread(buffer, 1, sizeof(buffer), source)) > 0) {
    if (fwrite(buffer, 1, amount, target) != amount) {
      set_error(error, error_size, "write %s failed: %s", to, strerror(errno));
      ok = false;
      break;
    }
  }
  if (ok && ferror(source)) {
    set_error(error, error_size, "read %s failed", from);
    ok = false;
  }
  if (ok && fflush(target) != 0) {
    set_error(error, error_size, "flush %s failed: %s", to, strerror(errno));
    ok = false;
  }
  if (ok && fsync(fileno(target)) != 0) {
    set_error(error, error_size, "fsync %s failed: %s", to, strerror(errno));
    ok = false;
  }
  fclose(source);
  if (fclose(target) != 0 && ok) {
    set_error(error, error_size, "close %s failed: %s", to, strerror(errno));
    ok = false;
  }
  return ok;
}

static bool file_exists(const char *path) {
  struct stat info;
  return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

bd_bootlogo_result bd_bootlogo_apply(const char *path, bool restore,
                                     char *error, size_t error_size) {
  if (!restore) {
    bd_bootlogo_result checked = bd_bootlogo_check(path, NULL, error,
                                                   error_size);
    if (checked != BD_BOOTLOGO_OK) {
      return checked;
    }
  }

  const char *override_dir = getenv("BRICKDESK_BOOTLOGO_DIR");
  bool overridden = override_dir != NULL && override_dir[0] != '\0';
  char mount_dir[512];
  if (overridden) {
    snprintf(mount_dir, sizeof(mount_dir), "%s", override_dir);
  } else {
    snprintf(mount_dir, sizeof(mount_dir), "%s", BD_BOOTLOGO_MOUNT);
    if (mkdir(mount_dir, 0755) != 0 && errno != EEXIST) {
      set_error(error, error_size, "cannot create %s: %s", mount_dir,
                strerror(errno));
      return BD_BOOTLOGO_ERR_IO;
    }
    if (mount(BD_BOOTLOGO_PARTITION, mount_dir, "vfat", MS_NOATIME, NULL) !=
        0) {
      set_error(error, error_size, "cannot mount %s: %s",
                BD_BOOTLOGO_PARTITION, strerror(errno));
      return BD_BOOTLOGO_ERR_MOUNT;
    }
  }

  char target[1024];
  char backup[1024];
  snprintf(target, sizeof(target), "%s/%s", mount_dir, BD_BOOTLOGO_NAME);
  snprintf(backup, sizeof(backup), "%s/%s", mount_dir, BD_BOOTLOGO_BACKUP);
  bd_bootlogo_result result = BD_BOOTLOGO_OK;

  if (restore) {
    if (!file_exists(backup)) {
      set_error(error, error_size, "no backup at %s", backup);
      result = BD_BOOTLOGO_ERR_IO;
    } else if (!copy_file(backup, target, error, error_size)) {
      result = BD_BOOTLOGO_ERR_IO;
    }
  } else {
    if (!file_exists(backup) && file_exists(target)) {
      if (!copy_file(target, backup, error, error_size)) {
        result = BD_BOOTLOGO_ERR_IO;
      }
    }
    if (result == BD_BOOTLOGO_OK &&
        !copy_file(path, target, error, error_size)) {
      result = BD_BOOTLOGO_ERR_IO;
    }
  }

  if (result == BD_BOOTLOGO_OK) {
    sync();
  }
  if (!overridden) {
    if (umount2(mount_dir, MNT_DETACH) != 0 && result == BD_BOOTLOGO_OK) {
      set_error(error, error_size, "cannot unmount %s: %s", mount_dir,
                strerror(errno));
      result = BD_BOOTLOGO_ERR_MOUNT;
    }
  }
  return result;
}
