#define _POSIX_C_SOURCE 200809L

#include "brickdesk/input.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "brickdesk/config.h"

static void print_usage(FILE *stream) {
  fprintf(stream,
          "usage: bd-input [--list|--learn|--watch|--check] [options]\n"
          "\n"
          "  --list                 enumerate /dev/input and show capabilities\n"
          "  --learn                wait for one key and print its raw code/mapping\n"
          "  --watch                connect to brickdesk-inputd and print events\n"
          "  --check                validate hotkey bindings and conflicts\n"
          "  --device PATH          device for --learn (default: first gamepad)\n"
          "  --device-dir DIR       evdev directory (default /dev/input)\n"
          "  --config-dir DIR       configuration directory\n"
          "  --socket PATH          inputd socket for --watch\n"
          "  -h, --help             show this help\n");
}

static bd_config *load_config(const char *config_dir) {
  bd_config_options options;
  options.config_dir = config_dir;
  options.migrate = false;
  char error[256];
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  if (config == NULL) {
    fprintf(stderr, "bd-input: config: %s\n", error);
  }
  return config;
}

static const char *evdev_key_label(unsigned code, char *buffer,
                                   size_t buffer_size) {
  const char *name = bd_evdev_key_name(code);
  if (name != NULL) {
    return name;
  }
  snprintf(buffer, buffer_size, "0x%x", code);
  return buffer;
}

static const char *evdev_abs_label(unsigned code, char *buffer,
                                   size_t buffer_size) {
  const char *name = bd_evdev_abs_name(code);
  if (name != NULL) {
    return name;
  }
  snprintf(buffer, buffer_size, "0x%x", code);
  return buffer;
}

static void print_bits(const char *label, const bd_evdev *device, bool abs) {
  char buffer[32];
  unsigned max = abs ? BD_EVDEV_ABS_MAX : BD_EVDEV_KEY_MAX;
  int printed = 0;
  printf("  %s:", label);
  for (unsigned code = 0; code <= max; ++code) {
    bool present = abs ? bd_evdev_has_abs(device, code)
                       : bd_evdev_has_key(device, code);
    if (!present) {
      continue;
    }
    const char *name = abs ? evdev_abs_label(code, buffer, sizeof(buffer))
                           : evdev_key_label(code, buffer, sizeof(buffer));
    printf(" %s", name);
    ++printed;
  }
  if (printed == 0) {
    printf(" (none)");
  }
  printf("\n");
}

static void print_mapping(const bd_device_mapping *mapping) {
  printf("  mapping: %s%s\n", mapping->name,
         mapping->has_ids ? " (vendor/product)" : "");
  for (int i = 0; i < mapping->entry_count; ++i) {
    const bd_device_map_entry *entry = &mapping->entries[i];
    char code_buffer[32];
    printf("    %-12s = ", bd_key_name(entry->key));
    if (entry->type == BD_DEVICE_MAP_KEY) {
      printf("%s", evdev_key_label(entry->code, code_buffer,
                                   sizeof(code_buffer)));
    } else {
      printf("%s:%d", evdev_abs_label(entry->code, code_buffer,
                                      sizeof(code_buffer)),
             entry->direction);
    }
    if (entry->source_file[0] != '\0') {
      printf("  (%s:%d)", entry->source_file, entry->source_line);
    } else {
      printf("  (built-in)");
    }
    printf("\n");
  }
}

static int run_list(const char *device_dir, const char *config_dir) {
  bd_evdev devices[BD_EVDEV_MAX_DEVICES];
  char error[256] = {0};
  errno = 0;
  int count = bd_evdev_scan(device_dir, devices, BD_EVDEV_MAX_DEVICES, error,
                            sizeof(error));
  if (count < 0) {
    fprintf(stderr, "bd-input: %s\n", error);
    return 1;
  }
  printf("devices: %d\n", count);
  if (count == 0) {
    printf("no event devices found under %s\n",
           device_dir != NULL ? device_dir : "/dev/input");
    return 0;
  }

  bd_device_map map;
  bd_config *config = load_config(config_dir);
  if (config != NULL) {
    bd_device_map_load(&map, config);
  } else {
    memset(&map, 0, sizeof(map));
  }

  for (int i = 0; i < count; ++i) {
    const bd_evdev *device = &devices[i];
    printf("\n%s \"%s\"\n", device->path, device->name);
    printf("  ids: bustype=0x%04x vendor=0x%04x product=0x%04x version=0x%04x%s\n",
           device->bustype, device->vendor, device->product, device->version,
           device->is_gamepad ? " gamepad" : "");
    if (device->has_key) {
      print_bits("keys", device, false);
    }
    if (device->has_abs) {
      print_bits("abs", device, true);
    }
    if (device->has_sw) {
      char buffer[32];
      printf("  switches:");
      for (unsigned code = 0; code <= BD_EVDEV_SW_MAX; ++code) {
        if (bd_evdev_has_sw(device, code)) {
          const char *name = bd_evdev_sw_name(code);
          if (name == NULL) {
            snprintf(buffer, sizeof(buffer), "0x%x", code);
            name = buffer;
          }
          printf(" %s", name);
        }
      }
      printf("\n");
    }
    bd_device_mapping mapping;
    bd_device_map_match(&map, device, &mapping);
    print_mapping(&mapping);
    bd_evdev_close(&devices[i]);
  }

  if (config != NULL) {
    bd_config_free(config);
  }
  return 0;
}

static bool key_mapping_source(const bd_device_mapping *mapping, bd_key key,
                               unsigned code, bool abs, int direction,
                               const bd_device_map_entry **out) {
  for (int i = 0; i < mapping->entry_count; ++i) {
    const bd_device_map_entry *entry = &mapping->entries[i];
    if (entry->key != key) {
      continue;
    }
    if (abs && entry->type == BD_DEVICE_MAP_ABS && entry->code == code &&
        entry->direction == direction) {
      *out = entry;
      return true;
    }
    if (!abs && entry->type == BD_DEVICE_MAP_KEY && entry->code == code) {
      *out = entry;
      return true;
    }
  }
  return false;
}

static void print_learn_mapping(const bd_device_mapping *mapping, bd_key key,
                                unsigned code, bool abs, int direction,
                                const char *raw_name) {
  const bd_device_map_entry *entry = NULL;
  printf("  raw: %s (0x%x) %s\n", raw_name, code,
         abs ? "ABS" : "KEY");
  if (key == BD_KEY_UNKNOWN) {
    printf("  logical: (unmapped)\n");
    if (!abs) {
      printf("  suggestion: add '<LOGICAL> = %s' to input.ini\n", raw_name);
    }
    return;
  }
  printf("  logical: %s\n", bd_key_name(key));
  if (key_mapping_source(mapping, key, code, abs, direction, &entry) &&
      entry != NULL) {
    if (entry->source_file[0] != '\0') {
      printf("  mapping: %s:%d\n", entry->source_file, entry->source_line);
    } else {
      printf("  mapping: built-in\n");
    }
    if (abs) {
      printf("  config: %s = %s:%d\n", bd_key_name(key), raw_name, direction);
    } else {
      printf("  config: %s = %s\n", bd_key_name(key), raw_name);
    }
  }
}

static int run_learn(const char *device_dir, const char *config_dir,
                     const char *device_path) {
  bd_evdev devices[BD_EVDEV_MAX_DEVICES];
  char error[256] = {0};
  errno = 0;
  int count = bd_evdev_scan(device_dir, devices, BD_EVDEV_MAX_DEVICES, error,
                            sizeof(error));
  if (count < 0) {
    fprintf(stderr, "bd-input: %s\n", error);
    return 1;
  }
  if (count == 0) {
    fprintf(stderr, "bd-input: no event devices under %s\n",
            device_dir != NULL ? device_dir : "/dev/input");
    return 1;
  }

  int selected = -1;
  for (int i = 0; i < count; ++i) {
    if (device_path != NULL && strcmp(devices[i].path, device_path) == 0) {
      selected = i;
      break;
    }
  }
  if (selected < 0 && device_path != NULL) {
    fprintf(stderr, "bd-input: device not found: %s\n", device_path);
    for (int i = 0; i < count; ++i) {
      fprintf(stderr, "  %s \"%s\"\n", devices[i].path, devices[i].name);
    }
    return 1;
  }
  if (selected < 0) {
    for (int i = 0; i < count; ++i) {
      if (devices[i].is_gamepad) {
        selected = i;
        break;
      }
    }
  }
  if (selected < 0) {
    for (int i = 0; i < count; ++i) {
      if (devices[i].has_key) {
        selected = i;
        break;
      }
    }
  }
  if (selected < 0) {
    fprintf(stderr, "bd-input: no readable key device\n");
    return 1;
  }

  bd_config *config = load_config(config_dir);
  bd_device_map map;
  if (config != NULL) {
    bd_device_map_load(&map, config);
  } else {
    memset(&map, 0, sizeof(map));
  }
  bd_device_mapping mapping;
  bd_device_map_match(&map, &devices[selected], &mapping);

  printf("device: %s \"%s\"\n", devices[selected].path, devices[selected].name);
  printf("press one key (or move one HAT axis)...\n");
  fflush(stdout);

  int result = 0;
  for (;;) {
    struct pollfd fd;
    fd.fd = devices[selected].fd;
    fd.events = POLLIN;
    fd.revents = 0;
    int ready = poll(&fd, 1, -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      fprintf(stderr, "bd-input: poll: %s\n", strerror(errno));
      result = 1;
      break;
    }
    if ((fd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
      fprintf(stderr, "bd-input: device disappeared\n");
      result = 1;
      break;
    }
    bd_evdev_event event;
    int read_result = bd_evdev_read(&devices[selected], &event);
    if (read_result <= 0) {
      continue;
    }
    if (event.kind == BD_EVDEV_EVENT_KEY && event.value == 1) {
      char raw[32];
      const char *raw_name = evdev_key_label(event.code, raw, sizeof(raw));
      bd_key logical = BD_KEY_UNKNOWN;
      bd_device_map_translate_key(&mapping, event.code, &logical);
      print_learn_mapping(&mapping, logical, event.code, false, 0, raw_name);
      break;
    }
    if (event.kind == BD_EVDEV_EVENT_ABS && event.value != 0) {
      char raw[32];
      const char *raw_name = evdev_abs_label(event.code, raw, sizeof(raw));
      bd_device_abs_key keys[BD_HOTKEY_MAX_KEYS];
      int key_count = bd_device_map_translate_abs(&mapping, event.code,
                                                  event.value, keys,
                                                  sizeof(keys) / sizeof(keys[0]));
      if (key_count == 0) {
        print_learn_mapping(&mapping, BD_KEY_UNKNOWN, event.code, true,
                            event.value < 0 ? -1 : 1, raw_name);
        break;
      }
      for (int i = 0; i < key_count; ++i) {
        if (keys[i].pressed) {
          print_learn_mapping(&mapping, keys[i].key, event.code, true,
                              event.value < 0 ? -1 : 1, raw_name);
        }
      }
      break;
    }
  }

  for (int i = 0; i < count; ++i) {
    bd_evdev_close(&devices[i]);
  }
  if (config != NULL) {
    bd_config_free(config);
  }
  return result;
}

static const char *default_socket_path(void) {
  const char *environment = getenv("BRICKDESK_INPUT_SOCKET");
  if (environment != NULL && *environment != '\0') {
    return environment;
  }
  return "/run/brickdesk/inputd.sock";
}

static int connect_unix_socket(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(address.sun_path)) {
    close(fd);
    errno = ENAMETOOLONG;
    return -1;
  }
  snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  return fd;
}

static int run_watch(const char *socket_path) {
  const char *path = socket_path != NULL ? socket_path : default_socket_path();
  int fd = connect_unix_socket(path);
  if (fd < 0 && socket_path == NULL && errno == ENOENT) {
    path = "/tmp/brickdesk-inputd.sock";
    fd = connect_unix_socket(path);
  }
  if (fd < 0) {
    fprintf(stderr, "bd-input: connect %s: %s\n", path, strerror(errno));
    return 1;
  }

  const char subscribe[] = "{\"cmd\":\"subscribe\",\"actions\":true,\"keys\":true}\n";
  if (send(fd, subscribe, sizeof(subscribe) - 1, MSG_NOSIGNAL) !=
      (ssize_t)(sizeof(subscribe) - 1)) {
    fprintf(stderr, "bd-input: subscribe failed: %s\n", strerror(errno));
    close(fd);
    return 1;
  }

  char line[BD_INPUTD_LINE_MAX];
  size_t used = 0;
  for (;;) {
    ssize_t bytes = recv(fd, line + used, sizeof(line) - 1 - used, 0);
    if (bytes <= 0) {
      break;
    }
    used += (size_t)bytes;
    line[used] = '\0';
    char *newline = NULL;
    while ((newline = strchr(line, '\n')) != NULL) {
      *newline = '\0';
      printf("%s\n", line);
      fflush(stdout);
      size_t consumed = (size_t)(newline - line) + 1;
      memmove(line, line + consumed, used - consumed);
      used -= consumed;
    }
    if (used + 1 >= sizeof(line)) {
      fprintf(stderr, "bd-input: inputd line is too long\n");
      break;
    }
  }
  close(fd);
  return 0;
}

static int run_check(const char *config_dir) {
  bd_config *config = load_config(config_dir);
  if (config == NULL) {
    return 1;
  }
  int errors = 0;
  for (size_t i = 0; i < bd_config_diagnostic_count(config); ++i) {
    bd_config_diagnostic diag;
    if (!bd_config_get_diagnostic(config, i, &diag)) {
      continue;
    }
    printf("%s: %s: %s", diag.severity == BD_CONFIG_ERROR ? "error" : "warning",
           diag.key != NULL ? diag.key : "(config)", diag.message);
    if (diag.file != NULL && diag.line > 0) {
      printf(" [%s:%d]", diag.file, diag.line);
    }
    printf("\n");
    if (diag.severity == BD_CONFIG_ERROR) {
      ++errors;
    }
  }
  bd_hotkey_table table;
  if (!bd_hotkey_table_load(&table, config)) {
    fprintf(stderr, "bd-input: cannot load hotkeys\n");
    bd_config_free(config);
    return 1;
  }
  for (size_t i = 0; i < table.diagnostic_count; ++i) {
    const bd_hotkey_diagnostic *diag = &table.diagnostics[i];
    printf("%s: %s: %s", diag->error ? "error" : "warning",
           diag->binding[0] != '\0' ? diag->binding : "(config)",
           diag->message);
    if (diag->file[0] != '\0' && diag->line > 0) {
      printf(" [%s:%d]", diag->file, diag->line);
    }
    printf("\n");
    if (diag->error) {
      ++errors;
    }
  }
  for (size_t i = 0; i < table.conflict_count; ++i) {
    const bd_hotkey_conflict *conflict = &table.conflicts[i];
    const char *kind = conflict->kind == BD_HOTKEY_CONFLICT_DUPLICATE
                           ? "duplicate"
                           : conflict->kind == BD_HOTKEY_CONFLICT_PREFIX
                                 ? "prefix"
                                 : "overlap";
    printf("%s: %s: %s\n",
           conflict->kind == BD_HOTKEY_CONFLICT_DUPLICATE ? "error" : "warning",
           kind, conflict->message);
    if (conflict->kind == BD_HOTKEY_CONFLICT_DUPLICATE) {
      ++errors;
    }
  }

  printf("bindings: %zu accepted, %zu rejected, %zu conflict(s)\n",
         table.count, table.rejected, table.conflict_count);
  for (size_t i = 0; i < table.count; ++i) {
    const bd_hotkey *hotkey = &table.items[i];
    printf("  %-24s combo=%-24s hold=%-4d window=%-3d seq=%-3d "
           "action=%s scope=%s%s%s\n",
           hotkey->id, hotkey->combo, hotkey->hold_ms, hotkey->window_ms,
           hotkey->sequence_ms, hotkey->action, hotkey->scope,
           hotkey->repeat ? " repeat" : "", hotkey->confirm ? " confirm" : "");
  }

  bd_config_free(config);
  return errors == 0 && table.rejected == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
  enum {
    MODE_NONE = 0,
    MODE_LIST,
    MODE_LEARN,
    MODE_WATCH,
    MODE_CHECK
  } mode = MODE_NONE;
  const char *device_dir = NULL;
  const char *config_dir = NULL;
  const char *socket_path = NULL;
  const char *device_path = NULL;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (strcmp(arg, "--list") == 0) {
      mode = MODE_LIST;
    } else if (strcmp(arg, "--learn") == 0) {
      mode = MODE_LEARN;
    } else if (strcmp(arg, "--watch") == 0) {
      mode = MODE_WATCH;
    } else if (strcmp(arg, "--check") == 0) {
      mode = MODE_CHECK;
    } else if (strcmp(arg, "--device-dir") == 0 && i + 1 < argc) {
      device_dir = argv[++i];
    } else if (strcmp(arg, "--config-dir") == 0 && i + 1 < argc) {
      config_dir = argv[++i];
    } else if (strcmp(arg, "--socket") == 0 && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (strcmp(arg, "--device") == 0 && i + 1 < argc) {
      device_path = argv[++i];
    } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      print_usage(stdout);
      return 0;
    } else {
      fprintf(stderr, "bd-input: unknown option: %s\n", arg);
      print_usage(stderr);
      return 2;
    }
  }

  if (mode == MODE_NONE) {
    print_usage(stderr);
    return 2;
  }
  if (mode == MODE_LIST) {
    return run_list(device_dir, config_dir);
  }
  if (mode == MODE_LEARN) {
    return run_learn(device_dir, config_dir, device_path);
  }
  if (mode == MODE_WATCH) {
    return run_watch(socket_path);
  }
  return run_check(config_dir);
}
