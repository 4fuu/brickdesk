#ifndef BRICKDESK_TESTS_SETTINGS_FIXTURE_H
#define BRICKDESK_TESTS_SETTINGS_FIXTURE_H

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef BRICKDESK_TEST_CONFIG_DIR
#define BRICKDESK_TEST_CONFIG_DIR "config"
#endif

static inline bool sf_copy_file(const char *source, const char *destination) {
  FILE *input = fopen(source, "rb");
  if (input == NULL) {
    return false;
  }
  FILE *output = fopen(destination, "wb");
  if (output == NULL) {
    fclose(input);
    return false;
  }
  char buffer[4096];
  bool ok = true;
  for (;;) {
    size_t got = fread(buffer, 1, sizeof(buffer), input);
    if (got > 0 && fwrite(buffer, 1, got, output) != got) {
      ok = false;
      break;
    }
    if (got < sizeof(buffer)) {
      if (ferror(input)) {
        ok = false;
      }
      break;
    }
  }
  fclose(input);
  if (fclose(output) != 0) {
    ok = false;
  }
  return ok;
}

static inline void sf_remove_tree(const char *path) {
  DIR *directory = opendir(path);
  if (directory == NULL) {
    return;
  }
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    char child[2048];
    size_t path_length = strlen(path);
    size_t name_length = strlen(entry->d_name);
    if (path_length + 1 + name_length + 1 > sizeof(child)) {
      continue;
    }
    memcpy(child, path, path_length);
    child[path_length] = '/';
    memcpy(child + path_length + 1, entry->d_name, name_length + 1);
    struct stat info;
    if (lstat(child, &info) == 0 && S_ISDIR(info.st_mode)) {
      sf_remove_tree(child);
    } else {
      unlink(child);
    }
  }
  closedir(directory);
  rmdir(path);
}

/*
 * Creates a throwaway config directory with the shipped INIs and, when
 * `conflict` is true, an extra binding that duplicates hotkey.menu.  The
 * input layer rejects the duplicate and reports a conflict, which the keys
 * page must show.
 */
static inline bool sf_make_config_dir(char *out, size_t out_size,
                                      bool conflict) {
  char pattern[] = "/tmp/bd-settings-XXXXXX";
  char *dir = mkdtemp(pattern);
  if (dir == NULL) {
    return false;
  }
  snprintf(out, out_size, "%s", dir);
  static const char *const FILES[] = {"brickdesk.ini", "perf.ini",
                                      "hotkeys.ini", "input.ini"};
  for (size_t i = 0; i < sizeof(FILES) / sizeof(FILES[0]); ++i) {
    char source[1024];
    char destination[1024];
    snprintf(source, sizeof(source), "%s/%s", BRICKDESK_TEST_CONFIG_DIR,
             FILES[i]);
    snprintf(destination, sizeof(destination), "%s/%s", out, FILES[i]);
    if (!sf_copy_file(source, destination)) {
      sf_remove_tree(out);
      out[0] = '\0';
      return false;
    }
  }
  if (conflict) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/hotkeys.ini", out);
    FILE *file = fopen(path, "ab");
    if (file == NULL) {
      sf_remove_tree(out);
      out[0] = '\0';
      return false;
    }
    fputs("\n[hotkey.menu_duplicate]\n"
          "# 测试用：与 hotkey.menu 完全相同的组合键，输入层应拒绝并报告冲突。\n"
          "combo = Menu\n"
          "hold_ms = 600\n"
          "action = menu.duplicate\n"
          "scope = global\n",
          file);
    fclose(file);
  }
  return true;
}

static inline bool sf_file_contains(const char *path, const char *needle) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return false;
  }
  char buffer[8192];
  size_t used = fread(buffer, 1, sizeof(buffer) - 1, file);
  fclose(file);
  buffer[used] = '\0';
  return strstr(buffer, needle) != NULL;
}

static inline bool sf_read_file(const char *path, char *out, size_t out_size) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return false;
  }
  size_t used = fread(out, 1, out_size - 1, file);
  fclose(file);
  out[used] = '\0';
  return true;
}

static inline uint64_t sf_now_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

#endif /* BRICKDESK_TESTS_SETTINGS_FIXTURE_H */
