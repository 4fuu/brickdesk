#define _POSIX_C_SOURCE 200809L

#include "brickdesk/sysinfo.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define BD_SYSINFO_PATH 512
#define BD_SYSINFO_TREE_LIMIT 64

typedef struct proc_entry {
  int pid;
  int ppid;
  unsigned long long rss_kb;
  bool rss_known;
} proc_entry;

static bool join_path(char *out, size_t out_size, const char *root,
                      const char *path) {
  const char *prefix = root != NULL && root[0] != '\0' ? root : "";
  size_t prefix_length = strlen(prefix);
  bool slash = prefix_length > 0 && prefix[prefix_length - 1] == '/';
  int written = snprintf(out, out_size, "%s%s%s", prefix, slash ? "" : "/", path);
  return written > 0 && (size_t)written < out_size;
}

static bool read_file_value(const char *path, char *out, size_t out_size) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  char *line = fgets(out, (int)out_size, file);
  fclose(file);
  if (line == NULL) {
    return false;
  }
  size_t length = strlen(out);
  while (length > 0 &&
         (out[length - 1] == '\n' || out[length - 1] == '\r' ||
          out[length - 1] == ' ' || out[length - 1] == '\t')) {
    out[--length] = '\0';
  }
  return length > 0;
}

static bool proc_status(const char *path, proc_entry *entry) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  bool has_ppid = false;
  bool zombie = false;
  char line[256];
  while (fgets(line, sizeof(line), file) != NULL) {
    if (strncmp(line, "PPid:", 5) == 0) {
      char *end = NULL;
      long value = strtol(line + 5, &end, 10);
      if (end != line + 5 && value >= 0 && value <= 2147483647L) {
        entry->ppid = (int)value;
        has_ppid = true;
      }
    } else if (strncmp(line, "State:", 6) == 0) {
      const char *state = line + 6;
      while (*state == ' ' || *state == '\t') {
        ++state;
      }
      zombie = *state == 'Z';
    } else if (strncmp(line, "VmRSS:", 6) == 0) {
      char *end = NULL;
      unsigned long long value = strtoull(line + 6, &end, 10);
      if (end != line + 6) {
        entry->rss_kb = value;
        entry->rss_known = true;
      }
    }
  }
  fclose(file);
  /* Some kernels expose "VmRSS: 0 kB" for zombies; that is not a sample. */
  if (zombie) {
    entry->rss_known = false;
  }
  return has_ppid;
}

static bool append_proc(proc_entry **items, size_t *count, size_t *capacity,
                        const proc_entry *entry) {
  if (*count == *capacity) {
    size_t next = *capacity > 0 ? *capacity * 2 : 128;
    proc_entry *grown =
        (proc_entry *)realloc(*items, next * sizeof(proc_entry));
    if (grown == NULL) {
      return false;
    }
    *items = grown;
    *capacity = next;
  }
  (*items)[(*count)++] = *entry;
  return true;
}

static const proc_entry *find_proc(const proc_entry *items, size_t count,
                                   int pid) {
  for (size_t i = 0; i < count; ++i) {
    if (items[i].pid == pid) {
      return &items[i];
    }
  }
  return NULL;
}

static bool process_is_in_tree(const proc_entry *items, size_t count, int pid,
                               int root) {
  int current = pid;
  for (int depth = 0; depth < BD_SYSINFO_TREE_LIMIT; ++depth) {
    if (current == root) {
      return true;
    }
    const proc_entry *entry = find_proc(items, count, current);
    if (entry == NULL || entry->ppid <= 0 || entry->ppid == current) {
      return false;
    }
    current = entry->ppid;
  }
  return false;
}

int bd_sys_process_tree_rss_mb(int pid) {
  if (pid <= 0) {
    return -1;
  }
  DIR *proc = opendir("/proc");
  if (proc == NULL) {
    return -1;
  }
  proc_entry *items = NULL;
  size_t count = 0;
  size_t capacity = 0;
  bool ok = true;
  struct dirent *entry;
  while ((entry = readdir(proc)) != NULL) {
    char *end = NULL;
    long value = strtol(entry->d_name, &end, 10);
    if (end == entry->d_name || *end != '\0' || value <= 0 ||
        value > 2147483647L) {
      continue;
    }
    char path[BD_SYSINFO_PATH];
    int written = snprintf(path, sizeof(path), "/proc/%ld/status", value);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
      continue;
    }
    proc_entry item;
    memset(&item, 0, sizeof(item));
    item.pid = (int)value;
    if (!proc_status(path, &item)) {
      continue;
    }
    if (!append_proc(&items, &count, &capacity, &item)) {
      ok = false;
      break;
    }
  }
  closedir(proc);
  if (!ok) {
    free(items);
    return -1;
  }

  const proc_entry *root = find_proc(items, count, pid);
  if (root == NULL || !root->rss_known) {
    free(items);
    return -1;
  }
  unsigned long long total_kb = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!process_is_in_tree(items, count, items[i].pid, pid)) {
      continue;
    }
    /*
     * A process can exist without VmRSS (zombies).  Reporting the remaining
     * descendants would undercount the tree, so the whole reading is unknown.
     */
    if (!items[i].rss_known) {
      free(items);
      return -1;
    }
    total_kb += items[i].rss_kb;
  }
  free(items);
  return (int)((total_kb + 1023u) / 1024u);
}

bd_wifi_state bd_sys_wifi_state(const char *root) {
  char base[BD_SYSINFO_PATH];
  if (!join_path(base, sizeof(base), root, "sys/class/net")) {
    return BD_WIFI_UNKNOWN;
  }
  DIR *directory = opendir(base);
  if (directory == NULL) {
    return BD_WIFI_UNKNOWN;
  }
  bool seen_wireless = false;
  bool seen_known = false;
  bool seen_up = false;
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    char interface_path[BD_SYSINFO_PATH];
    char wireless_path[BD_SYSINFO_PATH];
    char state_path[BD_SYSINFO_PATH];
    if (!join_path(interface_path, sizeof(interface_path), base, entry->d_name) ||
        !join_path(wireless_path, sizeof(wireless_path), interface_path,
                   "wireless") ||
        !join_path(state_path, sizeof(state_path), interface_path, "operstate")) {
      continue;
    }
    struct stat info;
    if (stat(wireless_path, &info) != 0 || !S_ISDIR(info.st_mode)) {
      continue;
    }
    seen_wireless = true;
    char state[64];
    if (!read_file_value(state_path, state, sizeof(state))) {
      continue;
    }
    seen_known = true;
    if (strcmp(state, "up") == 0) {
      seen_up = true;
    }
  }
  closedir(directory);
  if (seen_up) {
    return BD_WIFI_CONNECTED;
  }
  if (seen_known) {
    return BD_WIFI_DISCONNECTED;
  }
  if (seen_wireless) {
    return BD_WIFI_UNKNOWN;
  }
  return BD_WIFI_UNKNOWN;
}
