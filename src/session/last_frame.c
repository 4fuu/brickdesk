#define _POSIX_C_SOURCE 200809L

#include "brickdesk/app.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
}

static uint32_t bytes_per_pixel(uint32_t format) {
  switch (format) {
    case BD_APP_PIXEL_RGBA8888: return 4;
    case BD_APP_PIXEL_RGB565: return 2;
    default: return 0;
  }
}

size_t bd_app_last_frame_pixel_size(uint32_t format, uint32_t width,
                                    uint32_t height) {
  uint32_t bpp = bytes_per_pixel(format);
  if (bpp == 0 || width == 0 || height == 0 ||
      width > BD_APP_LAST_FRAME_MAX_DIMENSION ||
      height > BD_APP_LAST_FRAME_MAX_DIMENSION) {
    return 0;
  }
  uint64_t size = (uint64_t)width * (uint64_t)height * bpp;
  if (size > (uint64_t)SIZE_MAX) {
    return 0;
  }
  return (size_t)size;
}

static bool validate_header(const bd_app_last_frame_header *header,
                            size_t map_size, char *error, size_t error_size) {
  if (header->magic != BD_APP_LAST_FRAME_MAGIC) {
    set_error(error, error_size, "last-frame magic is 0x%08x, expected 0x%08x",
              header->magic, BD_APP_LAST_FRAME_MAGIC);
    return false;
  }
  if (header->version != BD_APP_LAST_FRAME_VERSION) {
    set_error(error, error_size, "last-frame version %u is unsupported",
              (unsigned)header->version);
    return false;
  }
  if (header->header_size != sizeof(bd_app_last_frame_header)) {
    set_error(error, error_size, "last-frame header size %u, expected %zu",
              (unsigned)header->header_size, sizeof(bd_app_last_frame_header));
    return false;
  }
  if (header->width == 0 || header->height == 0 ||
      header->width > BD_APP_LAST_FRAME_MAX_DIMENSION ||
      header->height > BD_APP_LAST_FRAME_MAX_DIMENSION) {
    set_error(error, error_size, "last-frame dimensions %ux%u are invalid",
              header->width, header->height);
    return false;
  }
  uint32_t bpp = bytes_per_pixel(header->format);
  if (bpp == 0) {
    set_error(error, error_size, "last-frame format %u is unsupported",
              header->format);
    return false;
  }
  uint64_t minimum_stride = (uint64_t)header->width * bpp;
  if ((uint64_t)header->stride < minimum_stride) {
    set_error(error, error_size,
              "last-frame stride %u is smaller than %llu bytes",
              header->stride, (unsigned long long)minimum_stride);
    return false;
  }
  if ((uint64_t)header->stride * header->height > header->data_size) {
    set_error(error, error_size,
              "last-frame data_size %llu is smaller than stride * height",
              (unsigned long long)header->data_size);
    return false;
  }
  if ((uint64_t)header->header_size + header->data_size >
      (uint64_t)map_size) {
    set_error(error, error_size,
              "last-frame data_size %llu exceeds the mapped file",
              (unsigned long long)header->data_size);
    return false;
  }
  return true;
}

bool bd_app_last_frame_open(const char *path, bd_app_last_frame *frame,
                            char *error, size_t error_size) {
  if (path == NULL || frame == NULL) {
    set_error(error, error_size, "invalid last-frame arguments");
    return false;
  }
  memset(frame, 0, sizeof(*frame));
  frame->fd = -1;

  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    set_error(error, error_size, "%s: %s", path, strerror(errno));
    return false;
  }
  struct stat status;
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
    set_error(error, error_size, "%s: not a regular file", path);
    close(fd);
    return false;
  }
  if ((uint64_t)status.st_size < sizeof(bd_app_last_frame_header)) {
    set_error(error, error_size, "%s: file is smaller than the header", path);
    close(fd);
    return false;
  }
  size_t size = (size_t)status.st_size;
  void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (map == MAP_FAILED) {
    set_error(error, error_size, "%s: mmap failed: %s", path, strerror(errno));
    close(fd);
    return false;
  }

  bd_app_last_frame_header header;
  memcpy(&header, map, sizeof(header));
  if (!validate_header(&header, size, error, error_size)) {
    munmap(map, size);
    close(fd);
    return false;
  }

  frame->fd = fd;
  frame->map = map;
  frame->map_size = size;
  frame->header = header;
  frame->pixels = (const uint8_t *)map + header.header_size;
  return true;
}

void bd_app_last_frame_close(bd_app_last_frame *frame) {
  if (frame == NULL) {
    return;
  }
  if (frame->map != NULL && frame->map_size > 0) {
    munmap(frame->map, frame->map_size);
  }
  if (frame->fd >= 0) {
    close(frame->fd);
  }
  memset(frame, 0, sizeof(*frame));
  frame->fd = -1;
}

bool bd_app_last_frame_copy(const bd_app_last_frame *frame, void *dst,
                            size_t dst_size,
                            bd_app_last_frame_header *out_header, char *error,
                            size_t error_size) {
  if (frame == NULL || frame->map == NULL || dst == NULL) {
    set_error(error, error_size, "invalid last-frame buffer");
    return false;
  }
  bd_app_last_frame_header header;
  memcpy(&header, frame->map, sizeof(header));
  if (!validate_header(&header, frame->map_size, error, error_size)) {
    return false;
  }
  size_t packed =
      bd_app_last_frame_pixel_size(header.format, header.width, header.height);
  if (packed == 0 || dst_size < packed) {
    set_error(error, error_size,
              "last-frame destination is too small (%zu < %zu)", dst_size,
              packed);
    return false;
  }

  const uint32_t bpp = bytes_per_pixel(header.format);
  const uint8_t *base = (const uint8_t *)frame->map + header.header_size;
  for (int attempt = 0; attempt < 4; ++attempt) {
    bd_app_last_frame_header before;
    memcpy(&before, frame->map, sizeof(before));
    if ((before.flags & BD_APP_LAST_FRAME_FLAG_WRITING) != 0) {
      continue;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    for (uint32_t row = 0; row < header.height; ++row) {
      memcpy((uint8_t *)dst + (size_t)row * header.width * bpp,
             base + (size_t)row * header.stride,
             (size_t)header.width * bpp);
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    bd_app_last_frame_header after;
    memcpy(&after, frame->map, sizeof(after));
    if ((after.flags & BD_APP_LAST_FRAME_FLAG_WRITING) == 0 &&
        after.frame_seq == before.frame_seq) {
      if (out_header != NULL) {
        *out_header = after;
      }
      return true;
    }
  }
  set_error(error, error_size, "last-frame kept changing while copying");
  return false;
}

static bool safe_token(const char *token) {
  if (token == NULL || token[0] == '\0') {
    return false;
  }
  for (const char *p = token; *p != '\0'; ++p) {
    char c = *p;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool bd_app_last_frame_path(const char *dir, const char *id, char *out,
                            size_t out_size) {
  if (dir == NULL || dir[0] == '\0' || !safe_token(id) || out == NULL) {
    return false;
  }
  int written = snprintf(out, out_size, "%s/last-frame-%s.bin", dir, id);
  return written > 0 && (size_t)written < out_size;
}

bool bd_app_last_frame_write(const char *path, uint32_t width, uint32_t height,
                             bd_app_pixel_format format, const void *pixels,
                             size_t stride, uint64_t *frame_seq, char *error,
                             size_t error_size) {
  uint32_t bpp = bytes_per_pixel((uint32_t)format);
  if (path == NULL || path[0] == '\0' || pixels == NULL || bpp == 0 ||
      width == 0 || height == 0 ||
      width > BD_APP_LAST_FRAME_MAX_DIMENSION ||
      height > BD_APP_LAST_FRAME_MAX_DIMENSION ||
      stride < (size_t)width * bpp || stride > UINT32_MAX) {
    set_error(error, error_size, "invalid last-frame arguments");
    return false;
  }
  uint64_t data_size = (uint64_t)stride * height;
  if (data_size > (uint64_t)SIZE_MAX ||
      data_size + BD_APP_LAST_FRAME_HEADER_SIZE > (uint64_t)SIZE_MAX) {
    set_error(error, error_size, "last-frame is too large");
    return false;
  }
  size_t total = (size_t)data_size + BD_APP_LAST_FRAME_HEADER_SIZE;

  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    set_error(error, error_size, "%s: %s", path, strerror(errno));
    return false;
  }
  if (ftruncate(fd, (off_t)total) != 0) {
    set_error(error, error_size, "%s: ftruncate: %s", path, strerror(errno));
    close(fd);
    return false;
  }
  void *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    set_error(error, error_size, "%s: mmap: %s", path, strerror(errno));
    close(fd);
    return false;
  }

  bd_app_last_frame_header header;
  memset(&header, 0, sizeof(header));
  header.magic = BD_APP_LAST_FRAME_MAGIC;
  header.version = BD_APP_LAST_FRAME_VERSION;
  header.header_size = (uint16_t)sizeof(bd_app_last_frame_header);
  header.width = width;
  header.height = height;
  header.format = (uint32_t)format;
  header.stride = (uint32_t)stride;
  header.data_size = data_size;
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
    header.timestamp_ms =
        (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
  }
  header.flags = BD_APP_LAST_FRAME_FLAG_WRITING;
  memcpy(map, &header, sizeof(header));
  __atomic_thread_fence(__ATOMIC_RELEASE);
  memcpy((uint8_t *)map + BD_APP_LAST_FRAME_HEADER_SIZE, pixels, data_size);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  header.frame_seq = frame_seq != NULL ? *frame_seq + 1 : 1;
  header.flags = 0;
  memcpy(map, &header, sizeof(header));

  bool ok = msync(map, total, MS_SYNC) == 0;
  if (!ok) {
    set_error(error, error_size, "%s: msync: %s", path, strerror(errno));
  }
  munmap(map, total);
  close(fd);
  if (ok && frame_seq != NULL) {
    *frame_seq = header.frame_seq;
  }
  return ok;
}
