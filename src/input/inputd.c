#define _POSIX_C_SOURCE 200809L

#include "brickdesk/input.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "brickdesk/config.h"
#include "brickdesk/uinput.h"

#if defined(__linux__)
#include <linux/input.h>
#else
#error "brickdesk-inputd needs Linux evdev"
#endif

#define BD_INPUTD_MAX_CLIENTS 8
#define BD_INPUTD_MAX_DEVICES BD_EVDEV_MAX_DEVICES
#define BD_INPUTD_SCAN_RETRY_MIN_MS 250
#define BD_INPUTD_SCAN_RETRY_MAX_MS 5000
#define BD_INPUTD_WATCH_MASK                                                  \
  (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |          \
   IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR)

typedef struct inputd_client {
  int fd;
  bool actions;
  bool keys;
  char input[BD_INPUTD_LINE_MAX];
  size_t input_length;
} inputd_client;

typedef struct inputd_device {
  bd_evdev evdev;
  bd_device_mapping mapping;
  bd_input_state state;
  bool replay;
  bool grabbed;
  bd_uinput *uinput;
  bool suppress;
  dev_t node_dev;
  ino_t node_ino;
} inputd_device;

typedef enum replay_kind {
  REPLAY_KEY = 0,
  REPLAY_ABS,
  REPLAY_SWITCH,
  REPLAY_QUIT
} replay_kind;

typedef struct replay_event {
  uint64_t time_ms;
  replay_kind kind;
  bool logical_key;
  bd_key key;
  bd_evdev_event event;
} replay_event;

typedef struct inputd {
  bd_config *config;
  bd_config_options config_options;
  bd_hotkey_table hotkeys;
  bd_device_map map;
  int double_tap_ms;
  char scope[BD_HOTKEY_SCOPE_MAX];
  bd_key remap[BD_KEY_COUNT];
  bool suppress_mask[BD_KEY_COUNT];
  bool suppress_active;
  bool uinput_error_logged;

  inputd_device devices[BD_INPUTD_MAX_DEVICES];
  size_t device_count;
  char device_dir[BD_EVDEV_PATH_MAX];
  int watch_fd;
  int watch_wd;
  bool scan_pending;
  bool scan_error_logged;
  bool watch_error_logged;
  uint64_t scan_deadline_ms;
  unsigned scan_backoff_ms;
  char grab_rejected[BD_INPUTD_MAX_DEVICES][BD_EVDEV_PATH_MAX];
  size_t grab_rejected_count;

  inputd_client clients[BD_INPUTD_MAX_CLIENTS];
  size_t client_count;

  int listen_fd;
  char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  bool socket_owned;

  bool grab_active;
  bool grab_pending;
  bool grab_forced;
  uint64_t grab_deadline_ms;
  bool allow_grab;

  bool replay_mode;
  bool replay_fast;
  bool replay_exit;
  bool replay_done;
  FILE *replay_file;
  replay_event replay_next;
  bool replay_has_next;
  uint64_t replay_base_ms;
  uint64_t replay_last_ms;

  bool verbose;
  bool running;
} inputd;

static volatile sig_atomic_t g_stop;

static void on_signal(int signal_number) {
  (void)signal_number;
  g_stop = 1;
}

static uint64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static uint64_t daemon_now(const inputd *daemon) {
  uint64_t now = monotonic_ms();
  if (daemon->replay_mode && daemon->replay_base_ms != 0) {
    return now >= daemon->replay_base_ms ? now - daemon->replay_base_ms : 0;
  }
  return now;
}

static void log_message(const inputd *daemon, const char *format, ...) {
  (void)daemon;
  va_list args;
  va_start(args, format);
  fputs("brickdesk-inputd: ", stderr);
  vfprintf(stderr, format, args);
  fputc('\n', stderr);
  va_end(args);
}

static void json_escape(char *dst, size_t dst_size, const char *src) {
  if (dst_size == 0) {
    return;
  }
  size_t used = 0;
  if (src == NULL) {
    src = "";
  }
  for (const unsigned char *p = (const unsigned char *)src;
       *p != '\0' && used + 1 < dst_size; ++p) {
    if (*p == '"' || *p == '\\') {
      if (used + 2 >= dst_size) {
        break;
      }
      dst[used++] = '\\';
      dst[used++] = (char)*p;
    } else if (*p < 0x20) {
      if (used + 6 >= dst_size) {
        break;
      }
      int written = snprintf(dst + used, dst_size - used, "\\u%04x", *p);
      if (written < 0) {
        break;
      }
      used += (size_t)written;
    } else {
      dst[used++] = (char)*p;
    }
  }
  dst[used] = '\0';
}

static bool client_send_line(inputd_client *client, const char *line) {
  if (client == NULL || client->fd < 0 || line == NULL) {
    return false;
  }
  size_t length = strlen(line);
  char buffer[BD_INPUTD_LINE_MAX];
  if (length + 1 >= sizeof(buffer)) {
    return false;
  }
  memcpy(buffer, line, length);
  buffer[length] = '\n';
  ssize_t sent = send(client->fd, buffer, length + 1, MSG_NOSIGNAL);
  return sent == (ssize_t)(length + 1);
}

static bool client_sendf(inputd_client *client, const char *format, ...) {
  char line[BD_INPUTD_LINE_MAX];
  va_list args;
  va_start(args, format);
  int written = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (written < 0 || (size_t)written >= sizeof(line)) {
    return false;
  }
  return client_send_line(client, line);
}

static void client_close(inputd_client *client) {
  if (client != NULL && client->fd >= 0) {
    close(client->fd);
    client->fd = -1;
  }
}

static void client_compact(inputd *daemon) {
  size_t write_index = 0;
  for (size_t i = 0; i < daemon->client_count; ++i) {
    if (daemon->clients[i].fd < 0) {
      continue;
    }
    if (write_index != i) {
      daemon->clients[write_index] = daemon->clients[i];
    }
    ++write_index;
  }
  daemon->client_count = write_index;
}

static void broadcast_line(inputd *daemon, const char *line) {
  for (size_t i = 0; i < daemon->client_count; ++i) {
    inputd_client *client = &daemon->clients[i];
    if (client->fd >= 0 && !client_send_line(client, line)) {
      client_close(client);
    }
  }
}

static const bd_hotkey *output_hotkey(const inputd *daemon,
                                      const bd_input_output *event) {
  if (event->binding < 0 ||
      (size_t)event->binding >= daemon->hotkeys.count) {
    return NULL;
  }
  return &daemon->hotkeys.items[event->binding];
}

static void broadcast_action(inputd *daemon, const inputd_device *device,
                             const bd_input_output *event) {
  const bd_hotkey *hotkey = output_hotkey(daemon, event);
  if (hotkey == NULL) {
    return;
  }
  char binding[BD_HOTKEY_ID_MAX * 2];
  char action[BD_HOTKEY_ACTION_MAX * 2];
  char combo[BD_HOTKEY_COMBO_MAX * 2];
  char scope[BD_HOTKEY_SCOPE_MAX * 2];
  char source[BD_EVDEV_NAME_MAX * 2];
  json_escape(binding, sizeof(binding), hotkey->id);
  json_escape(action, sizeof(action), hotkey->action);
  json_escape(combo, sizeof(combo), hotkey->combo);
  json_escape(scope, sizeof(scope), hotkey->scope);
  json_escape(source, sizeof(source), device->evdev.name);

  const char *phase = event->pressed ? "down" : "up";
  bd_key key = BD_KEY_UNKNOWN;
  if (hotkey->step_count == 1 && hotkey->steps[0].key_count == 1) {
    key = hotkey->steps[0].keys[0];
  }
  char message[1200];
  int written = snprintf(
      message, sizeof(message),
      "{\"type\":\"action\",\"binding\":\"%s\",\"action\":\"%s\","
      "\"combo\":\"%s\",\"scope\":\"%s\",\"trigger\":\"%s\","
      "\"phase\":\"%s\",\"key\":\"%s\",\"source\":\"%s\","
      "\"time_ms\":%llu}",
      binding, action, combo, scope,
      bd_hotkey_trigger_name((int)event->trigger), phase,
      bd_key_name(key), source, (unsigned long long)event->time_ms);
  if (written < 0 || (size_t)written >= sizeof(message)) {
    return;
  }
  for (size_t i = 0; i < daemon->client_count; ++i) {
    inputd_client *client = &daemon->clients[i];
    if (client->fd >= 0 && client->actions &&
        !client_send_line(client, message)) {
      client_close(client);
    }
  }

  if (daemon->verbose) {
    log_message(daemon, "action %s (%s/%s) %s", hotkey->action,
                bd_hotkey_trigger_name((int)event->trigger), hotkey->combo,
                phase);
  }
}

static void broadcast_key(inputd *daemon, const inputd_device *device,
                          bd_key key, bool pressed, uint64_t time_ms) {
  char source[BD_EVDEV_NAME_MAX * 2];
  json_escape(source, sizeof(source), device->evdev.name);
  char message[512];
  int written = snprintf(
      message, sizeof(message),
      "{\"type\":\"key\",\"key\":\"%s\",\"pressed\":%s,"
      "\"source\":\"%s\",\"time_ms\":%llu}",
      bd_key_name(key), pressed ? "true" : "false", source,
      (unsigned long long)time_ms);
  if (written < 0 || (size_t)written >= sizeof(message)) {
    return;
  }
  for (size_t i = 0; i < daemon->client_count; ++i) {
    inputd_client *client = &daemon->clients[i];
    if (client->fd >= 0 && client->keys &&
        !client_send_line(client, message)) {
      client_close(client);
    }
  }
}

static void broadcast_switch(inputd *daemon, const inputd_device *device,
                             unsigned code, int value, uint64_t time_ms) {
  const char *name = bd_evdev_sw_name(code);
  char switch_name[64];
  if (name != NULL) {
    snprintf(switch_name, sizeof(switch_name), "%s", name);
  } else {
    snprintf(switch_name, sizeof(switch_name), "0x%x", code);
  }
  if (strcmp(switch_name, "SW_HEADPHONE_INSERT") == 0) {
    snprintf(switch_name, sizeof(switch_name), "headphone");
  } else if (strcmp(switch_name, "SW_TABLET_MODE") == 0) {
    snprintf(switch_name, sizeof(switch_name), "tablet");
  } else if (strcmp(switch_name, "SW_LID") == 0) {
    snprintf(switch_name, sizeof(switch_name), "lid");
  }
  char source[BD_EVDEV_NAME_MAX * 2];
  json_escape(source, sizeof(source), device->evdev.name);
  char message[512];
  int written = snprintf(
      message, sizeof(message),
      "{\"type\":\"switch\",\"switch\":\"%s\",\"value\":%d,"
      "\"source\":\"%s\",\"time_ms\":%llu}",
      switch_name, value, source, (unsigned long long)time_ms);
  if (written < 0 || (size_t)written >= sizeof(message)) {
    return;
  }
  broadcast_line(daemon, message);
}

static void broadcast_grab(inputd *daemon, bool active, bool pending,
                           bool forced) {
  char message[160];
  int written = snprintf(
      message, sizeof(message),
      "{\"type\":\"grab\",\"active\":%s,\"pending\":%s,\"forced\":%s}",
      active ? "true" : "false", pending ? "true" : "false",
      forced ? "true" : "false");
  if (written > 0 && (size_t)written < sizeof(message)) {
    broadcast_line(daemon, message);
  }
}

static void broadcast_release_all(inputd *daemon, uint64_t time_ms) {
  char message[128];
  int written = snprintf(message, sizeof(message),
                         "{\"type\":\"release_all\",\"time_ms\":%llu}",
                         (unsigned long long)time_ms);
  if (written > 0 && (size_t)written < sizeof(message)) {
    broadcast_line(daemon, message);
  }
}

/* ---------------------------------------------------- suppress forwarding */

typedef struct output_context {
  inputd *daemon;
  inputd_device *device;
} output_context;

static void dispatch_output(void *user, const bd_input_output *event);

static void load_remap(inputd *daemon) {
  memset(daemon->remap, 0, sizeof(daemon->remap));
  static const struct {
    const char *key;
    bd_key physical;
  } MAP[] = {{"input.remap_a", BD_KEY_A},
             {"input.remap_b", BD_KEY_B},
             {"input.remap_x", BD_KEY_X},
             {"input.remap_y", BD_KEY_Y}};
  for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); ++i) {
    const char *name = bd_config_get_string(daemon->config, MAP[i].key, NULL);
    if (name == NULL || name[0] == '\0') {
      continue;
    }
    bd_key target = bd_key_from_name(name);
    if (target > BD_KEY_UNKNOWN && target < BD_KEY_COUNT) {
      daemon->remap[MAP[i].physical] = target;
    }
  }
}

static void suppress_compute(inputd *daemon) {
  memset(daemon->suppress_mask, 0, sizeof(daemon->suppress_mask));
  daemon->suppress_active = false;
  for (size_t i = 0; i < daemon->hotkeys.count; ++i) {
    const bd_hotkey *hotkey = &daemon->hotkeys.items[i];
    if (!hotkey->suppress ||
        !bd_hotkey_scope_active(hotkey, daemon->scope)) {
      continue;
    }
    daemon->suppress_active = true;
    for (int step = 0; step < hotkey->step_count; ++step) {
      for (int key = 0; key < hotkey->steps[step].key_count; ++key) {
        bd_key value = hotkey->steps[step].keys[key];
        if (value > BD_KEY_UNKNOWN && value < BD_KEY_COUNT) {
          daemon->suppress_mask[value] = true;
        }
      }
    }
  }
}

static void device_suppress_apply(inputd *daemon, inputd_device *device) {
  if (device->replay || device->evdev.fd < 0) {
    return;
  }
  if (daemon->suppress_active) {
    if (device->uinput == NULL) {
      char error[160] = {0};
      device->uinput = bd_uinput_create(&device->evdev, error, sizeof(error));
      if (device->uinput == NULL) {
        device->suppress = false;
        if (!daemon->uinput_error_logged) {
          log_message(daemon, "suppress forwarding disabled: %s", error);
          daemon->uinput_error_logged = true;
        }
        if (!daemon->grab_active && device->grabbed) {
          bd_evdev_grab(&device->evdev, false);
          device->grabbed = false;
        }
        return;
      }
      if (daemon->verbose) {
        log_message(daemon, "suppress forwarding on %s", device->evdev.path);
      }
    }
    device->suppress = true;
    if (!device->grabbed) {
      if (!bd_evdev_grab(&device->evdev, true)) {
        int grab_errno = errno;
        bd_uinput_destroy(device->uinput);
        device->uinput = NULL;
        device->suppress = false;
        if (!daemon->uinput_error_logged) {
          log_message(daemon, "suppress grab %s: %s", device->evdev.path,
                      strerror(grab_errno));
          daemon->uinput_error_logged = true;
        }
        return;
      }
      device->grabbed = true;
    }
    return;
  }

  if (device->uinput != NULL) {
    bd_uinput_release_all(device->uinput);
    bd_uinput_destroy(device->uinput);
    device->uinput = NULL;
  }
  device->suppress = false;
  if (!daemon->grab_active && device->grabbed) {
    bd_evdev_grab(&device->evdev, false);
    device->grabbed = false;
  }
}

static void suppress_apply_all(inputd *daemon) {
  suppress_compute(daemon);
  for (size_t i = 0; i < daemon->device_count; ++i) {
    device_suppress_apply(daemon, &daemon->devices[i]);
  }
}

static void daemon_set_scope(inputd *daemon, const char *scope,
                             uint64_t now_ms) {
  (void)now_ms;
  if (scope == NULL || scope[0] == '\0' || strlen(scope) >= sizeof(daemon->scope)) {
    return;
  }
  if (strcmp(daemon->scope, scope) == 0) {
    return;
  }
  snprintf(daemon->scope, sizeof(daemon->scope), "%s", scope);
  for (size_t i = 0; i < daemon->device_count; ++i) {
    inputd_device *device = &daemon->devices[i];
    /* Re-arm without broadcasting release_all: another client may still hold
     * a key and must not see its state dropped by a scope change. */
    if (device->uinput != NULL) {
      bd_uinput_release_all(device->uinput);
    }
    bd_input_state_set_scope(&device->state, &daemon->hotkeys, daemon->scope,
                             daemon->double_tap_ms);
  }
  suppress_apply_all(daemon);
  char escaped[BD_HOTKEY_SCOPE_MAX * 2];
  json_escape(escaped, sizeof(escaped), daemon->scope);
  char message[256];
  int written = snprintf(message, sizeof(message),
                         "{\"type\":\"scope\",\"scope\":\"%s\"}", escaped);
  if (written > 0 && (size_t)written < sizeof(message)) {
    broadcast_line(daemon, message);
  }
}

static bool daemon_reload(inputd *daemon, uint64_t now_ms, char *error,
                          size_t error_size) {
  (void)now_ms;
  bd_config *config = bd_config_load(&daemon->config_options, error,
                                     error_size);
  if (config == NULL) {
    return false;
  }
  bd_hotkey_table hotkeys;
  if (!bd_hotkey_table_load(&hotkeys, config)) {
    snprintf(error, error_size, "cannot load hotkeys");
    bd_config_free(config);
    return false;
  }
  bd_device_map map;
  if (!bd_device_map_load(&map, config)) {
    snprintf(error, error_size, "cannot load input.ini");
    bd_config_free(config);
    return false;
  }
  for (size_t i = 0; i < daemon->device_count; ++i) {
    inputd_device *device = &daemon->devices[i];
    if (device->uinput != NULL) {
      bd_uinput_release_all(device->uinput);
    }
  }
  bd_config_free(daemon->config);
  daemon->config = config;
  daemon->hotkeys = hotkeys;
  daemon->map = map;
  load_remap(daemon);
  daemon->double_tap_ms =
      (int)bd_config_get_int(daemon->config, "input.double_tap_ms", 300);
  if (daemon->double_tap_ms <= 0) {
    daemon->double_tap_ms = 300;
  }
  for (size_t i = 0; i < daemon->device_count; ++i) {
    inputd_device *device = &daemon->devices[i];
    bd_device_map_match(&daemon->map, &device->evdev, &device->mapping);
    bd_input_state_set_scope(&device->state, &daemon->hotkeys, daemon->scope,
                             daemon->double_tap_ms);
  }
  suppress_apply_all(daemon);
  return true;
}

static bool suppress_drop_key(const inputd *daemon,
                              const inputd_device *device,
                              const bd_evdev_event *event) {
  if (event->kind == BD_EVDEV_EVENT_KEY) {
    bd_key key = BD_KEY_UNKNOWN;
    if (bd_device_map_translate_key(&device->mapping, event->code, &key) &&
        key > BD_KEY_UNKNOWN && key < BD_KEY_COUNT) {
      if (daemon->remap[key] != BD_KEY_UNKNOWN) {
        key = daemon->remap[key];
      }
      return daemon->suppress_mask[key];
    }
    return false;
  }
  if (event->kind == BD_EVDEV_EVENT_ABS) {
    bd_device_abs_key keys[BD_HOTKEY_MAX_KEYS];
    int count = bd_device_map_translate_abs(&device->mapping, event->code,
                                            event->value, keys,
                                            sizeof(keys) / sizeof(keys[0]));
    for (int i = 0; i < count; ++i) {
      if (keys[i].pressed && keys[i].key > BD_KEY_UNKNOWN &&
          keys[i].key < BD_KEY_COUNT &&
          daemon->suppress_mask[keys[i].key]) {
        return true;
      }
    }
  }
  return false;
}

static void forward_raw_event(inputd *daemon, inputd_device *device,
                              const bd_evdev_event *event) {
  if (!device->suppress || device->uinput == NULL || daemon->grab_active) {
    return;
  }
  uint16_t type = 0;
  if (event->kind == BD_EVDEV_EVENT_KEY) {
    if (event->value == 2) {
      return;
    }
    type = EV_KEY;
  } else if (event->kind == BD_EVDEV_EVENT_ABS) {
    type = EV_ABS;
  } else if (event->kind == BD_EVDEV_EVENT_SWITCH) {
    type = EV_SW;
  } else {
    return;
  }
  if (suppress_drop_key(daemon, device, event)) {
    return;
  }
  if (bd_uinput_emit(device->uinput, type, (uint16_t)event->code,
                     event->value)) {
    bd_uinput_sync(device->uinput);
  }
}

static bool any_keys_pressed(const inputd *daemon) {
  for (size_t i = 0; i < daemon->device_count; ++i) {
    if (bd_input_state_any_pressed(&daemon->devices[i].state)) {
      return true;
    }
  }
  return false;
}

static void ungrab_devices(inputd *daemon) {
  for (size_t i = 0; i < daemon->device_count; ++i) {
    inputd_device *device = &daemon->devices[i];
    if (device->evdev.fd >= 0 && device->grabbed && !device->suppress) {
      bd_evdev_grab(&device->evdev, false);
      device->grabbed = false;
    }
  }
}

static void finish_ungrab(inputd *daemon, bool forced, uint64_t now_ms) {
  if (!daemon->grab_active) {
    return;
  }
  ungrab_devices(daemon);
  for (size_t i = 0; i < daemon->device_count; ++i) {
    if (daemon->devices[i].uinput != NULL) {
      bd_uinput_release_all(daemon->devices[i].uinput);
    }
  }
  daemon->grab_active = false;
  daemon->grab_pending = false;
  daemon->grab_forced = false;
  broadcast_release_all(daemon, now_ms);
  broadcast_grab(daemon, false, false, forced);
  daemon->grab_rejected_count = 0;
  /* Devices rejected while the grab was active are retried now. */
  if (!daemon->replay_mode) {
    daemon->scan_pending = true;
  }
  if (daemon->verbose) {
    log_message(daemon, "ungrab%s", forced ? " (forced after 500 ms)" : "");
  }
}

static bool request_ungrab(inputd *daemon, uint64_t now_ms) {
  if (!daemon->grab_active) {
    return true;
  }
  if (!daemon->grab_pending && any_keys_pressed(daemon)) {
    daemon->grab_pending = true;
    daemon->grab_deadline_ms = now_ms + 500;
    broadcast_grab(daemon, true, true, false);
    return false;
  }
  if (!any_keys_pressed(daemon) || now_ms >= daemon->grab_deadline_ms) {
    bool forced = any_keys_pressed(daemon);
    finish_ungrab(daemon, forced, now_ms);
    return true;
  }
  return false;
}

static bool request_grab(inputd *daemon, inputd_client *client,
                         uint64_t now_ms) {
  (void)now_ms;
  if (!daemon->allow_grab) {
    client_sendf(client,
                 "{\"type\":\"error\",\"cmd\":\"grab\","
                 "\"error\":\"grab disabled by --no-grab\"}");
    return false;
  }
  if (daemon->grab_active) {
    broadcast_grab(daemon, true, daemon->grab_pending, false);
    return true;
  }

  bool newly[BD_INPUTD_MAX_DEVICES] = {false};
  for (size_t i = 0; i < daemon->device_count; ++i) {
    inputd_device *device = &daemon->devices[i];
    if (device->evdev.fd < 0 || device->grabbed) {
      continue;
    }
    if (!bd_evdev_grab(&device->evdev, true)) {
      int grab_errno = errno;
      char path[BD_EVDEV_PATH_MAX * 2];
      json_escape(path, sizeof(path), device->evdev.path);
      char message[512];
      int written = snprintf(
          message, sizeof(message),
          "{\"type\":\"error\",\"cmd\":\"grab\",\"error\":\"%s\","
          "\"device\":\"%s\"}",
          strerror(grab_errno), path);
      if (written > 0 && (size_t)written < sizeof(message)) {
        client_send_line(client, message);
      }
      for (size_t rollback = 0; rollback < daemon->device_count; ++rollback) {
        inputd_device *previous = &daemon->devices[rollback];
        if (newly[rollback] && previous->evdev.fd >= 0 &&
            previous->grabbed) {
          bd_evdev_grab(&previous->evdev, false);
          previous->grabbed = false;
        }
      }
      return false;
    }
    device->grabbed = true;
    newly[i] = true;
  }

  for (size_t i = 0; i < daemon->device_count; ++i) {
    if (daemon->devices[i].uinput != NULL) {
      bd_uinput_release_all(daemon->devices[i].uinput);
    }
  }
  daemon->grab_active = true;
  daemon->grab_pending = false;
  daemon->grab_forced = false;
  client->keys = true;
  broadcast_grab(daemon, true, false, false);
  if (daemon->verbose) {
    log_message(daemon, "grab");
  }
  return true;
}

static void dispatch_output(void *user, const bd_input_output *event) {
  output_context *context = (output_context *)user;
  inputd *daemon = context->daemon;
  inputd_device *device = context->device;

  if (event->kind == BD_INPUT_OUTPUT_ACTION) {
    broadcast_action(daemon, device, event);
    return;
  }
  if (event->kind == BD_INPUT_OUTPUT_KEY && !daemon->grab_active) {
    broadcast_key(daemon, device, event->key, event->pressed, event->time_ms);
  }
}

static void apply_key(inputd *daemon, inputd_device *device, bd_key key,
                      bool pressed, uint64_t now_ms) {
  if (key <= BD_KEY_UNKNOWN || key >= BD_KEY_COUNT) {
    return;
  }
  if (daemon->remap[key] != BD_KEY_UNKNOWN) {
    key = daemon->remap[key];
  }
  if (bd_input_state_is_pressed(&device->state, key) == pressed) {
    return;
  }
  if (daemon->grab_active) {
    broadcast_key(daemon, device, key, pressed, now_ms);
  }
  output_context context;
  context.daemon = daemon;
  context.device = device;
  bd_input_state_feed(&device->state, key, pressed, now_ms, dispatch_output,
                      &context);
}

static void apply_abs(inputd *daemon, inputd_device *device, unsigned code,
                      int value, uint64_t now_ms) {
  bd_device_abs_key keys[BD_HOTKEY_MAX_KEYS];
  int count = bd_device_map_translate_abs(&device->mapping, code, value, keys,
                                          sizeof(keys) / sizeof(keys[0]));
  for (int i = 0; i < count; ++i) {
    apply_key(daemon, device, keys[i].key, keys[i].pressed, now_ms);
  }
}

static void resync_device(inputd *daemon, inputd_device *device,
                          uint64_t now_ms) {
  unsigned char bits[(BD_EVDEV_KEY_MAX + 8) / 8];
  if (!bd_evdev_key_bits(&device->evdev, bits, sizeof(bits))) {
    return;
  }
  if (device->uinput != NULL) {
    bd_uinput_release_all(device->uinput);
  }
  for (int i = 0; i < device->mapping.entry_count; ++i) {
    const bd_device_map_entry *entry = &device->mapping.entries[i];
    if (entry->type != BD_DEVICE_MAP_KEY || entry->code > BD_EVDEV_KEY_MAX) {
      continue;
    }
    bool pressed = (bits[entry->code / 8u] &
                    (unsigned char)(1u << (entry->code % 8u))) != 0;
    apply_key(daemon, device, entry->key, pressed, now_ms);
    if (device->suppress && device->uinput != NULL &&
        !daemon->grab_active && entry->key > BD_KEY_UNKNOWN &&
        entry->key < BD_KEY_COUNT && !daemon->suppress_mask[entry->key]) {
      bd_uinput_emit(device->uinput, EV_KEY, (uint16_t)entry->code,
                     pressed ? 1 : 0);
      bd_uinput_sync(device->uinput);
    }
  }
  for (int i = 0; i < device->mapping.entry_count; ++i) {
    const bd_device_map_entry *entry = &device->mapping.entries[i];
    if (entry->type != BD_DEVICE_MAP_ABS) {
      continue;
    }
    int value = bd_evdev_abs_value(&device->evdev, entry->code);
    apply_abs(daemon, device, entry->code, value, now_ms);
    if (device->suppress && device->uinput != NULL &&
        !daemon->grab_active) {
      bd_uinput_emit(device->uinput, EV_ABS, (uint16_t)entry->code, value);
      bd_uinput_sync(device->uinput);
    }
  }

  char source[BD_EVDEV_NAME_MAX * 2];
  json_escape(source, sizeof(source), device->evdev.name);
  char message[384];
  int written = snprintf(
      message, sizeof(message),
      "{\"type\":\"warning\",\"message\":\"syn_dropped\","
      "\"device\":\"%s\",\"time_ms\":%llu}",
      source, (unsigned long long)now_ms);
  if (written > 0 && (size_t)written < sizeof(message)) {
    broadcast_line(daemon, message);
  }
}

static void apply_evdev_event(inputd *daemon, inputd_device *device,
                              const bd_evdev_event *event, uint64_t now_ms) {
  forward_raw_event(daemon, device, event);
  if (event->kind == BD_EVDEV_EVENT_KEY) {
    if (event->value == 2) {
      return;
    }
    bd_key key = BD_KEY_UNKNOWN;
    if (bd_device_map_translate_key(&device->mapping, event->code, &key)) {
      apply_key(daemon, device, key, event->value != 0, now_ms);
    }
    return;
  }
  if (event->kind == BD_EVDEV_EVENT_ABS) {
    apply_abs(daemon, device, event->code, event->value, now_ms);
    return;
  }
  if (event->kind == BD_EVDEV_EVENT_SWITCH) {
    broadcast_switch(daemon, device, event->code, event->value, now_ms);
    return;
  }
  if (event->kind == BD_EVDEV_EVENT_SYN_DROPPED) {
    resync_device(daemon, device, now_ms);
  }
}

static bool parse_abs_code(const char *name, unsigned *code) {
  bd_device_map_type type;
  unsigned parsed = 0;
  int direction = 0;
  if (bd_device_map_parse_value(name, &type, &parsed, &direction)) {
    if (type != BD_DEVICE_MAP_ABS) {
      return false;
    }
    *code = parsed;
    return true;
  }
  errno = 0;
  char *end = NULL;
  unsigned long numeric = strtoul(name, &end, 0);
  if (errno != 0 || end == name || *end != '\0' || numeric > BD_EVDEV_ABS_MAX) {
    return false;
  }
  *code = (unsigned)numeric;
  return true;
}

static bool parse_switch_code(const char *name, unsigned *code) {
  if (strcmp(name, "headphone") == 0 || strcmp(name, "HEADPHONE") == 0) {
    *code = SW_HEADPHONE_INSERT;
    return true;
  }
  if (strcmp(name, "tablet") == 0 || strcmp(name, "fn") == 0) {
    *code = SW_TABLET_MODE;
    return true;
  }
  if (strcmp(name, "lid") == 0) {
    *code = SW_LID;
    return true;
  }
  if (strcmp(name, "mute") == 0) {
    *code = SW_MUTE_DEVICE;
    return true;
  }
  errno = 0;
  char *end = NULL;
  unsigned long numeric = strtoul(name, &end, 0);
  if (errno != 0 || end == name || *end != '\0' || numeric > BD_EVDEV_SW_MAX) {
    return false;
  }
  *code = (unsigned)numeric;
  return true;
}

static bool read_replay_line(inputd *daemon, replay_event *event) {
  char line[512];
  while (fgets(line, sizeof(line), daemon->replay_file) != NULL) {
    char *start = line;
    while (*start != '\0' && isspace((unsigned char)*start)) {
      ++start;
    }
    if (*start == '\0' || *start == '#') {
      continue;
    }
    char kind[16] = {0};
    char name[64] = {0};
    char value[32] = {0};
    unsigned long long time_ms = 0;
    int fields = sscanf(start, "%llu %15s %63s %31s", &time_ms, kind, name,
                        value);
    if (fields < 2) {
      log_message(daemon, "replay: skipping malformed line: %s", start);
      continue;
    }
    memset(event, 0, sizeof(*event));
    event->time_ms = (uint64_t)time_ms;
    if (event->time_ms < daemon->replay_last_ms) {
      event->time_ms = daemon->replay_last_ms;
    }
    daemon->replay_last_ms = event->time_ms;
    event->event.time_ms = event->time_ms;

    if (strcmp(kind, "quit") == 0) {
      event->kind = REPLAY_QUIT;
      return true;
    }
    if (strcmp(kind, "key") == 0) {
      if (fields < 4) {
        log_message(daemon, "replay: key needs a name and down/up");
        continue;
      }
      event->kind = REPLAY_KEY;
      bd_key logical = bd_key_from_name(name);
      if (logical != BD_KEY_UNKNOWN) {
        bool pressed = strcmp(value, "down") == 0 || strcmp(value, "1") == 0;
        if (!pressed && strcmp(value, "up") != 0 && strcmp(value, "0") != 0) {
          log_message(daemon, "replay: key action must be down/up");
          continue;
        }
        event->logical_key = true;
        event->key = logical;
        event->event.value = pressed ? 1 : 0;
        return true;
      }
      bd_device_map_type type;
      unsigned code = 0;
      int direction = 0;
      if (!bd_device_map_parse_value(name, &type, &code, &direction) ||
          type != BD_DEVICE_MAP_KEY) {
        log_message(daemon, "replay: unknown key '%s'", name);
        continue;
      }
      event->event.kind = BD_EVDEV_EVENT_KEY;
      event->event.code = code;
      event->event.value =
          (strcmp(value, "down") == 0 || strcmp(value, "1") == 0) ? 1 : 0;
      if (strcmp(value, "down") != 0 && strcmp(value, "up") != 0 &&
          strcmp(value, "1") != 0 && strcmp(value, "0") != 0) {
        log_message(daemon, "replay: key action must be down/up");
        continue;
      }
      return true;
    }
    if (strcmp(kind, "abs") == 0) {
      if (fields < 4) {
        log_message(daemon, "replay: abs needs a name and value");
        continue;
      }
      unsigned code = 0;
      if (!parse_abs_code(name, &code)) {
        log_message(daemon, "replay: unknown abs axis '%s'", name);
        continue;
      }
      event->kind = REPLAY_ABS;
      event->event.kind = BD_EVDEV_EVENT_ABS;
      event->event.code = code;
      event->event.value = atoi(value);
      return true;
    }
    if (strcmp(kind, "sw") == 0 || strcmp(kind, "switch") == 0) {
      if (fields < 4) {
        log_message(daemon, "replay: sw needs a name and value");
        continue;
      }
      unsigned code = 0;
      if (!parse_switch_code(name, &code)) {
        log_message(daemon, "replay: unknown switch '%s'", name);
        continue;
      }
      event->kind = REPLAY_SWITCH;
      event->event.kind = BD_EVDEV_EVENT_SWITCH;
      event->event.code = code;
      event->event.value = atoi(value);
      return true;
    }
    log_message(daemon, "replay: unknown record '%s'", kind);
    continue;
  }
  return false;
}

static void replay_next_event(inputd *daemon) {
  daemon->replay_has_next = read_replay_line(daemon, &daemon->replay_next);
  if (!daemon->replay_has_next) {
    daemon->replay_done = true;
  }
}

static void apply_replay_event(inputd *daemon, inputd_device *device,
                               const replay_event *event, uint64_t now_ms) {
  if (event->kind == REPLAY_QUIT) {
    daemon->running = false;
    return;
  }
  if (event->kind == REPLAY_KEY) {
    if (event->logical_key) {
      apply_key(daemon, device, event->key, event->event.value != 0, now_ms);
    } else {
      apply_evdev_event(daemon, device, &event->event, now_ms);
    }
    return;
  }
  apply_evdev_event(daemon, device, &event->event, now_ms);
}

static void run_replay_fast(inputd *daemon) {
  inputd_device *device = &daemon->devices[0];
  uint64_t sim_ms = 0;
  output_context context;
  context.daemon = daemon;
  context.device = device;

  while (daemon->replay_has_next) {
    const replay_event *event = &daemon->replay_next;
    if (event->time_ms > sim_ms) {
      sim_ms = event->time_ms;
    }
    /* Advance to the event time before feeding it, so a hold timer that
     * expires exactly at the next event fires before that event cancels it. */
    bd_input_state_advance(&device->state, sim_ms, dispatch_output, &context);
    apply_replay_event(daemon, device, event, sim_ms);
    if (!daemon->running) {
      break;
    }
    replay_next_event(daemon);
  }
  bd_input_state_advance(&device->state, sim_ms, dispatch_output, &context);
}

static void process_replay_due(inputd *daemon, uint64_t now_ms) {
  inputd_device *device = &daemon->devices[0];
  output_context context;
  context.daemon = daemon;
  context.device = device;

  while (daemon->replay_has_next &&
         daemon->replay_next.time_ms <= now_ms) {
    const replay_event *event = &daemon->replay_next;
    uint64_t event_time = event->time_ms;
    /* Timers due at this timestamp fire before the event is applied; a key
     * release at the hold deadline must not swallow the action. */
    bd_input_state_advance(&device->state, event_time, dispatch_output,
                           &context);
    apply_replay_event(daemon, device, event, event_time);
    if (!daemon->running) {
      return;
    }
    replay_next_event(daemon);
  }
  if (!daemon->replay_has_next && daemon->replay_exit) {
    daemon->running = false;
  }
}

static bool json_field_string(const char *json, const char *field, char *out,
                              size_t out_size) {
  char pattern[64];
  int written = snprintf(pattern, sizeof(pattern), "\"%s\"", field);
  if (written <= 0 || (size_t)written >= sizeof(pattern)) {
    return false;
  }
  const char *cursor = strstr(json, pattern);
  if (cursor == NULL) {
    return false;
  }
  cursor += strlen(pattern);
  while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
    ++cursor;
  }
  if (*cursor != ':') {
    return false;
  }
  ++cursor;
  while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
    ++cursor;
  }
  if (*cursor != '"') {
    return false;
  }
  ++cursor;
  size_t used = 0;
  while (*cursor != '\0' && *cursor != '"' && used + 1 < out_size) {
    if (*cursor == '\\' && cursor[1] != '\0') {
      ++cursor;
    }
    out[used++] = *cursor++;
  }
  out[used] = '\0';
  return true;
}

static bool json_field_bool(const char *json, const char *field,
                            bool fallback) {
  char pattern[64];
  int written = snprintf(pattern, sizeof(pattern), "\"%s\"", field);
  if (written <= 0 || (size_t)written >= sizeof(pattern)) {
    return fallback;
  }
  const char *cursor = strstr(json, pattern);
  if (cursor == NULL) {
    return fallback;
  }
  cursor += strlen(pattern);
  while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
    ++cursor;
  }
  if (*cursor != ':') {
    return fallback;
  }
  ++cursor;
  while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
    ++cursor;
  }
  if (strncmp(cursor, "true", 4) == 0) {
    return true;
  }
  if (strncmp(cursor, "false", 5) == 0) {
    return false;
  }
  return fallback;
}

static void release_all_devices(inputd *daemon, uint64_t now_ms) {
  for (size_t i = 0; i < daemon->device_count; ++i) {
    output_context context;
    context.daemon = daemon;
    context.device = &daemon->devices[i];
    bd_input_state_release_all(&daemon->devices[i].state, now_ms,
                               dispatch_output, &context);
  }
  broadcast_release_all(daemon, now_ms);
}

static void handle_client_line(inputd *daemon, inputd_client *client,
                               const char *line, uint64_t now_ms) {
  char command[32];
  if (!json_field_string(line, "cmd", command, sizeof(command))) {
    client_sendf(client, "{\"type\":\"error\",\"error\":\"missing cmd\"}");
    return;
  }
  if (strcmp(command, "subscribe") == 0) {
    client->keys = json_field_bool(line, "keys", client->keys);
    client->actions = json_field_bool(line, "actions", true);
    return;
  }
  if (strcmp(command, "grab") == 0) {
    request_grab(daemon, client, now_ms);
    return;
  }
  if (strcmp(command, "ungrab") == 0) {
    request_ungrab(daemon, now_ms);
    return;
  }
  if (strcmp(command, "release_all") == 0) {
    release_all_devices(daemon, now_ms);
    return;
  }
  if (strcmp(command, "set_scope") == 0) {
    char scope[BD_HOTKEY_SCOPE_MAX];
    if (!json_field_string(line, "scope", scope, sizeof(scope))) {
      client_sendf(client,
                   "{\"type\":\"error\",\"cmd\":\"set_scope\","
                   "\"error\":\"missing scope\"}");
      return;
    }
    daemon_set_scope(daemon, scope, now_ms);
    return;
  }
  if (strcmp(command, "reload") == 0) {
    char error[160] = {0};
    if (!daemon_reload(daemon, now_ms, error, sizeof(error))) {
      char escaped[256];
      json_escape(escaped, sizeof(escaped), error);
      client_sendf(client,
                   "{\"type\":\"error\",\"cmd\":\"reload\","
                   "\"error\":\"%s\"}",
                   escaped);
      return;
    }
    client_sendf(client, "{\"type\":\"reloaded\"}");
    return;
  }
  if (strcmp(command, "ping") == 0) {
    client_sendf(client, "{\"type\":\"pong\",\"time_ms\":%llu}",
                 (unsigned long long)now_ms);
    return;
  }
  char escaped[64];
  json_escape(escaped, sizeof(escaped), command);
  client_sendf(client,
               "{\"type\":\"error\",\"cmd\":\"%s\",\"error\":"
               "\"unknown command\"}",
               escaped);
}

static void client_read(inputd *daemon, size_t index, uint64_t now_ms) {
  inputd_client *client = &daemon->clients[index];
  char buffer[1024];
  for (;;) {
    ssize_t bytes = recv(client->fd, buffer, sizeof(buffer), 0);
    if (bytes < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return;
      }
      client_close(client);
      return;
    }
    if (bytes == 0) {
      client_close(client);
      return;
    }
    size_t copied = 0;
    while (copied < (size_t)bytes) {
      size_t room = sizeof(client->input) - 1 - client->input_length;
      if (room == 0) {
        log_message(daemon, "client line too long; disconnecting");
        client_close(client);
        return;
      }
      size_t chunk = (size_t)bytes - copied;
      if (chunk > room) {
        chunk = room;
      }
      memcpy(client->input + client->input_length, buffer + copied, chunk);
      client->input_length += chunk;
      copied += chunk;

      char *newline = memchr(client->input, '\n', client->input_length);
      while (newline != NULL) {
        size_t line_length = (size_t)(newline - client->input);
        client->input[line_length] = '\0';
        handle_client_line(daemon, client, client->input, now_ms);
        if (client->fd < 0) {
          return;
        }
        size_t consumed = line_length + 1;
        memmove(client->input, client->input + consumed,
                client->input_length - consumed);
        client->input_length -= consumed;
        newline = memchr(client->input, '\n', client->input_length);
      }
    }
  }
}

static void client_accept(inputd *daemon) {
  int fd = accept(daemon->listen_fd, NULL, NULL);
  if (fd < 0) {
    return;
  }
  if (daemon->client_count >= BD_INPUTD_MAX_CLIENTS) {
    close(fd);
    return;
  }
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  inputd_client *client = &daemon->clients[daemon->client_count++];
  memset(client, 0, sizeof(*client));
  client->fd = fd;
  client->actions = true;
  client->keys = false;

  char devices[1024] = "";
  size_t used = 0;
  bool first_device = true;
  for (size_t i = 0; i < daemon->device_count; ++i) {
    if (daemon->devices[i].evdev.fd < 0 && !daemon->devices[i].replay) {
      continue;
    }
    char escaped[BD_EVDEV_NAME_MAX * 2];
    json_escape(escaped, sizeof(escaped), daemon->devices[i].evdev.name);
    int needed = snprintf(NULL, 0, "%s\"%s\"",
                          first_device ? "" : ",", escaped);
    if (needed < 0 || (size_t)needed >= sizeof(devices) - used) {
      break;
    }
    snprintf(devices + used, sizeof(devices) - used, "%s\"%s\"",
             first_device ? "" : ",", escaped);
    used += (size_t)needed;
    first_device = false;
  }
  client_sendf(client,
               "{\"type\":\"hello\",\"version\":%d,\"pid\":%ld,"
               "\"socket\":\"%s\",\"devices\":[%s]}",
               BD_INPUTD_PROTOCOL_VERSION, (long)getpid(), daemon->socket_path,
               devices);
}

static bool create_socket(inputd *daemon, const char *path) {
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(address.sun_path)) {
    log_message(daemon, "socket path is too long: %s", path);
    return false;
  }
  snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
  unlink(path);

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    log_message(daemon, "socket: %s", strerror(errno));
    return false;
  }
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    log_message(daemon, "bind %s: %s", path, strerror(errno));
    close(fd);
    return false;
  }
  if (listen(fd, 8) != 0) {
    log_message(daemon, "listen %s: %s", path, strerror(errno));
    close(fd);
    unlink(path);
    return false;
  }
  chmod(path, 0660);
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  daemon->listen_fd = fd;
  snprintf(daemon->socket_path, sizeof(daemon->socket_path), "%s", path);
  daemon->socket_owned = true;
  return true;
}

static bool make_parent_directory(const char *path) {
  char directory[sizeof(((struct sockaddr_un *)0)->sun_path)];
  snprintf(directory, sizeof(directory), "%s", path);
  char *slash = strrchr(directory, '/');
  if (slash == NULL || slash == directory) {
    return true;
  }
  *slash = '\0';
  if (mkdir(directory, 0770) == 0 || errno == EEXIST) {
    return true;
  }
  return false;
}

static bool setup_socket(inputd *daemon, const char *requested) {
  char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  if (requested != NULL && *requested != '\0') {
    snprintf(path, sizeof(path), "%s", requested);
    make_parent_directory(path);
  } else {
    const char *environment = getenv("BRICKDESK_INPUT_SOCKET");
    if (environment != NULL && *environment != '\0') {
      snprintf(path, sizeof(path), "%s", environment);
      make_parent_directory(path);
    } else {
      snprintf(path, sizeof(path), "/run/brickdesk/inputd.sock");
      if (!make_parent_directory(path)) {
        snprintf(path, sizeof(path), "/tmp/brickdesk-inputd.sock");
      }
    }
  }
  return create_socket(daemon, path);
}

static int compute_timeout(inputd *daemon, uint64_t now_ms) {
  int timeout = 500;
  for (size_t i = 0; i < daemon->device_count; ++i) {
    uint64_t deadline = 0;
    if (bd_input_state_next_deadline(&daemon->devices[i].state, &deadline)) {
      int remaining = deadline <= now_ms ? 0 : (int)(deadline - now_ms);
      if (remaining < timeout) {
        timeout = remaining;
      }
    }
  }
  if (daemon->grab_pending) {
    int remaining =
        daemon->grab_deadline_ms <= now_ms
            ? 0
            : (int)(daemon->grab_deadline_ms - now_ms);
    if (remaining < timeout) {
      timeout = remaining;
    }
  }
  if (daemon->replay_mode && !daemon->replay_fast && daemon->replay_has_next) {
    int remaining = daemon->replay_next.time_ms <= now_ms
                        ? 0
                        : (int)(daemon->replay_next.time_ms - now_ms);
    if (remaining < timeout) {
      timeout = remaining;
    }
  }
  if (!daemon->replay_mode) {
    if (daemon->scan_pending) {
      timeout = 0;
    } else if (daemon->scan_deadline_ms != 0) {
      int remaining = daemon->scan_deadline_ms <= now_ms
                          ? 0
                          : (int)(daemon->scan_deadline_ms - now_ms);
      if (remaining < timeout) {
        timeout = remaining;
      }
    }
  }
  return timeout;
}

static void broadcast_device(inputd *daemon, const char *action,
                             const inputd_device *device, const char *reason,
                             uint64_t now_ms) {
  char name[BD_EVDEV_NAME_MAX * 2];
  char path[BD_EVDEV_PATH_MAX * 2];
  char reason_text[64];
  json_escape(name, sizeof(name), device->evdev.name);
  json_escape(path, sizeof(path), device->evdev.path);
  json_escape(reason_text, sizeof(reason_text), reason != NULL ? reason : "");

  char message[768];
  int written;
  if (strcmp(action, "added") == 0) {
    written = snprintf(
        message, sizeof(message),
        "{\"type\":\"device\",\"action\":\"added\",\"device\":\"%s\","
        "\"path\":\"%s\",\"grabbed\":%s,\"time_ms\":%llu}",
        name, path, device->grabbed ? "true" : "false",
        (unsigned long long)now_ms);
  } else {
    written = snprintf(
        message, sizeof(message),
        "{\"type\":\"device\",\"action\":\"removed\",\"device\":\"%s\","
        "\"path\":\"%s\",\"reason\":\"%s\",\"time_ms\":%llu}",
        name, path, reason_text, (unsigned long long)now_ms);
  }
  if (written > 0 && (size_t)written < sizeof(message)) {
    broadcast_line(daemon, message);
  }
}

static void broadcast_hotplug_grab_failure(inputd *daemon, const bd_evdev *evdev,
                                           int grab_errno, uint64_t now_ms) {
  char path[BD_EVDEV_PATH_MAX * 2];
  char error[128];
  json_escape(path, sizeof(path), evdev->path);
  json_escape(error, sizeof(error), strerror(grab_errno));
  char message[512];
  int written = snprintf(
      message, sizeof(message),
      "{\"type\":\"warning\",\"message\":\"hotplug_grab_failed\","
      "\"device\":\"%s\",\"error\":\"%s\",\"time_ms\":%llu}",
      path, error, (unsigned long long)now_ms);
  if (written > 0 && (size_t)written < sizeof(message)) {
    broadcast_line(daemon, message);
  }
}

static bool device_identity(const bd_evdev *evdev, dev_t *node_dev,
                            ino_t *node_ino) {
  struct stat node;
  if (evdev->fd < 0 || fstat(evdev->fd, &node) != 0) {
    return false;
  }
  *node_dev = node.st_dev;
  *node_ino = node.st_ino;
  return true;
}

static bool device_add(inputd *daemon, const bd_evdev *evdev, bool replay,
                       bool grabbed, uint64_t now_ms) {
  size_t slot = daemon->device_count;
  for (size_t i = 0; i < daemon->device_count; ++i) {
    if (daemon->devices[i].evdev.fd < 0 && !daemon->devices[i].replay) {
      slot = i;
      break;
    }
  }
  if (slot == daemon->device_count) {
    if (daemon->device_count >= BD_INPUTD_MAX_DEVICES) {
      return false;
    }
    daemon->device_count++;
  }

  inputd_device *device = &daemon->devices[slot];
  memset(device, 0, sizeof(*device));
  device->evdev = *evdev;
  device->replay = replay;
  device->grabbed = grabbed;
  device_identity(&device->evdev, &device->node_dev, &device->node_ino);
  bd_device_map_match(&daemon->map, &device->evdev, &device->mapping);
  bd_input_state_init(&device->state, &daemon->hotkeys, daemon->scope,
                      daemon->double_tap_ms);
  device_suppress_apply(daemon, device);

  if (!replay) {
    broadcast_device(daemon, "added", device, NULL, now_ms);
  }
  if (daemon->verbose) {
    log_message(daemon,
                "device %s \"%s\" %04x:%04x keys=%s abs=%s sw=%s%s%s",
                device->evdev.path, device->evdev.name, device->evdev.vendor,
                device->evdev.product, device->evdev.has_key ? "yes" : "no",
                device->evdev.has_abs ? "yes" : "no",
                device->evdev.has_sw ? "yes" : "no",
                device->evdev.is_gamepad ? " gamepad" : "",
                device->evdev.is_fake ? " fake" : "");
  }
  return true;
}

static void device_retry_schedule(inputd *daemon, uint64_t now_ms);

static void device_remove(inputd *daemon, inputd_device *device, uint64_t now_ms,
                          const char *reason, bool schedule_retry) {
  if (device->evdev.fd < 0) {
    return;
  }
  if (device->uinput != NULL) {
    bd_uinput_release_all(device->uinput);
    bd_uinput_destroy(device->uinput);
    device->uinput = NULL;
  }
  device->suppress = false;
  if (device->grabbed) {
    bd_evdev_grab(&device->evdev, false);
    device->grabbed = false;
  }

  /*
   * Release only this device's logical state. Other devices may still have
   * keys held, so this deliberately does not send the global release_all
   * notification used by ungrab.
   */
  if (daemon->grab_active) {
    for (int key = 1; key < (int)BD_KEY_COUNT; ++key) {
      if (bd_input_state_is_pressed(&device->state, (bd_key)key)) {
        broadcast_key(daemon, device, (bd_key)key, false, now_ms);
      }
    }
  }
  output_context context;
  context.daemon = daemon;
  context.device = device;
  bd_input_state_release_all(&device->state, now_ms, dispatch_output, &context);

  if (daemon->verbose) {
    log_message(daemon, "device %s \"%s\" removed (%s)", device->evdev.path,
                device->evdev.name, reason != NULL ? reason : "");
  }
  bd_evdev_close(&device->evdev);
  broadcast_device(daemon, "removed", device, reason, now_ms);
  if (schedule_retry) {
    device_retry_schedule(daemon, now_ms);
  }
}

static void device_compact(inputd *daemon) {
  size_t write_index = 0;
  for (size_t i = 0; i < daemon->device_count; ++i) {
    if (daemon->devices[i].evdev.fd < 0 && !daemon->devices[i].replay) {
      continue;
    }
    if (write_index != i) {
      daemon->devices[write_index] = daemon->devices[i];
    }
    ++write_index;
  }
  daemon->device_count = write_index;
}

static bool grab_rejected_contains(const inputd *daemon, const char *path) {
  for (size_t i = 0; i < daemon->grab_rejected_count; ++i) {
    if (strcmp(daemon->grab_rejected[i], path) == 0) {
      return true;
    }
  }
  return false;
}

static void grab_rejected_forget(inputd *daemon, const char *path) {
  size_t write_index = 0;
  for (size_t i = 0; i < daemon->grab_rejected_count; ++i) {
    if (strcmp(daemon->grab_rejected[i], path) == 0) {
      continue;
    }
    if (write_index != i) {
      snprintf(daemon->grab_rejected[write_index],
               sizeof(daemon->grab_rejected[write_index]), "%s",
               daemon->grab_rejected[i]);
    }
    ++write_index;
  }
  daemon->grab_rejected_count = write_index;
}

static void device_attach(inputd *daemon, bd_evdev *evdev, uint64_t now_ms) {
  bool grabbed = false;
  if (daemon->grab_active) {
    if (!bd_evdev_grab(evdev, true)) {
      int grab_errno = errno;
      if (!grab_rejected_contains(daemon, evdev->path)) {
        broadcast_hotplug_grab_failure(daemon, evdev, grab_errno, now_ms);
        if (daemon->verbose) {
          log_message(daemon, "device %s not added: grab: %s", evdev->path,
                      strerror(grab_errno));
        }
        if (daemon->grab_rejected_count < BD_INPUTD_MAX_DEVICES) {
          snprintf(daemon->grab_rejected[daemon->grab_rejected_count],
                   sizeof(daemon->grab_rejected[0]), "%s", evdev->path);
          daemon->grab_rejected_count++;
        }
      }
      bd_evdev_close(evdev);
      return;
    }
    grabbed = true;
  }
  grab_rejected_forget(daemon, evdev->path);
  if (!device_add(daemon, evdev, false, grabbed, now_ms)) {
    log_message(daemon, "device %s dropped: device table full", evdev->path);
    if (grabbed) {
      bd_evdev_grab(evdev, false);
    }
    bd_evdev_close(evdev);
  }
}

static bool device_reconcile(inputd *daemon, uint64_t now_ms, char *error,
                             size_t error_size, bool *retry_needed) {
  if (retry_needed != NULL) {
    *retry_needed = false;
  }
  if (daemon->replay_mode) {
    return true;
  }
  bd_evdev found[BD_INPUTD_MAX_DEVICES];
  bool incomplete = false;
  errno = 0;
  int found_count =
      bd_evdev_scan_ex(daemon->device_dir, found, BD_INPUTD_MAX_DEVICES,
                       &incomplete, error, error_size);
  if (found_count < 0) {
    return false;
  }
  if (incomplete && retry_needed != NULL) {
    *retry_needed = true;
  }

  bool matched[BD_INPUTD_MAX_DEVICES] = {false};
  for (int f = 0; f < found_count; ++f) {
    bool existing_path = false;
    for (size_t i = 0; i < daemon->device_count; ++i) {
      inputd_device *device = &daemon->devices[i];
      if (device->evdev.fd < 0 ||
          strcmp(device->evdev.path, found[f].path) != 0) {
        continue;
      }
      existing_path = true;
      matched[i] = true;

      dev_t node_dev = 0;
      ino_t node_ino = 0;
      bool same_node = device_identity(&found[f], &node_dev, &node_ino) &&
                       node_dev == device->node_dev &&
                       node_ino == device->node_ino;
      if (same_node) {
        /*
         * The device is already open. Do not readiness-probe this second fd:
         * a probe byte read from a FIFO would otherwise be discarded with
         * the candidate.
         */
        bd_evdev_close(&found[f]);
      } else {
        device_remove(daemon, device, now_ms, "replaced", false);
        if (!bd_evdev_ready(&found[f])) {
          /*
           * The replacement node exists but has no readable peer yet. Keep
           * the directory watch and retry with backoff instead of
           * registering and immediately HUPing it.
           */
          bd_evdev_close(&found[f]);
          if (retry_needed != NULL) {
            *retry_needed = true;
          }
        } else {
          device_attach(daemon, &found[f], now_ms);
        }
      }
      break;
    }
    if (existing_path) {
      continue;
    }
    if (!bd_evdev_ready(&found[f])) {
      /*
       * A node can exist but be immediately dead (a FIFO without a writer,
       * or a device invalidated by resume). Keep the directory watch, close
       * the probe fd and retry with backoff; do not register and then
       * immediately HUP the same node.
       */
      bd_evdev_close(&found[f]);
      if (retry_needed != NULL) {
        *retry_needed = true;
      }
      continue;
    }
    device_attach(daemon, &found[f], now_ms);
  }

  for (size_t i = 0; i < daemon->device_count; ++i) {
    inputd_device *device = &daemon->devices[i];
    if (device->evdev.fd < 0 || device->replay || matched[i]) {
      continue;
    }
    struct stat node;
    if (stat(device->evdev.path, &node) != 0 ||
        node.st_dev != device->node_dev || node.st_ino != device->node_ino) {
      device_remove(daemon, device, now_ms, "gone", false);
    }
  }
  return true;
}

static void device_retry_schedule(inputd *daemon, uint64_t now_ms) {
  if (daemon->scan_deadline_ms != 0) {
    return;
  }
  unsigned delay = daemon->scan_backoff_ms;
  if (delay < BD_INPUTD_SCAN_RETRY_MIN_MS) {
    delay = BD_INPUTD_SCAN_RETRY_MIN_MS;
  }
  daemon->scan_deadline_ms = now_ms + delay;
  daemon->scan_backoff_ms =
      delay >= BD_INPUTD_SCAN_RETRY_MAX_MS / 2 ? BD_INPUTD_SCAN_RETRY_MAX_MS
                                               : delay * 2;
}

static void device_watch_update(inputd *daemon, uint64_t now_ms) {
  if (daemon->replay_mode) {
    return;
  }
  if (daemon->watch_fd < 0) {
    daemon->watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (daemon->watch_fd < 0) {
      if (!daemon->watch_error_logged) {
        log_message(daemon, "inotify: %s; falling back to polling",
                    strerror(errno));
        daemon->watch_error_logged = true;
      }
      device_retry_schedule(daemon, now_ms);
      return;
    }
  }
  if (daemon->watch_wd < 0) {
    daemon->watch_wd =
        inotify_add_watch(daemon->watch_fd, daemon->device_dir,
                          BD_INPUTD_WATCH_MASK);
    if (daemon->watch_wd < 0) {
      if (!daemon->watch_error_logged) {
        log_message(daemon, "watch %s: %s; retrying", daemon->device_dir,
                    strerror(errno));
        daemon->watch_error_logged = true;
      }
      device_retry_schedule(daemon, now_ms);
      return;
    }
    if (daemon->verbose) {
      log_message(daemon, "watching %s", daemon->device_dir);
    }
  }
  daemon->watch_error_logged = false;
  daemon->scan_deadline_ms = 0;
}

static void device_drain_watch(inputd *daemon, uint64_t now_ms) {
  if (daemon->watch_fd < 0) {
    return;
  }
  for (;;) {
    char buffer[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t bytes = read(daemon->watch_fd, buffer, sizeof(buffer));
    if (bytes < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return;
      }
      log_message(daemon, "inotify read: %s; falling back", strerror(errno));
      close(daemon->watch_fd);
      daemon->watch_fd = -1;
      daemon->watch_wd = -1;
      daemon->scan_pending = true;
      device_retry_schedule(daemon, now_ms);
      return;
    }
    if (bytes == 0) {
      return;
    }
    for (char *cursor = buffer; cursor < buffer + bytes;) {
      const struct inotify_event *event =
          (const struct inotify_event *)cursor;
      if ((event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
        daemon->watch_wd = -1;
      }
      daemon->scan_pending = true;
      cursor += sizeof(struct inotify_event) + event->len;
    }
  }
}

static void device_service(inputd *daemon, uint64_t now_ms) {
  if (daemon->replay_mode) {
    return;
  }
  device_compact(daemon);
  bool due = daemon->scan_pending ||
             (daemon->scan_deadline_ms != 0 &&
              now_ms >= daemon->scan_deadline_ms);
  if (!due) {
    return;
  }
  daemon->scan_pending = false;
  daemon->scan_deadline_ms = 0;

  char error[512] = {0};
  bool retry_needed = false;
  if (device_reconcile(daemon, now_ms, error, sizeof(error), &retry_needed)) {
    daemon->scan_error_logged = false;
    device_watch_update(daemon, now_ms);
    if (retry_needed) {
      device_retry_schedule(daemon, now_ms);
    } else if (daemon->watch_wd >= 0) {
      daemon->scan_backoff_ms = 0;
    }
    return;
  }
  if (!daemon->scan_error_logged) {
    log_message(daemon, "%s; retrying", error);
    daemon->scan_error_logged = true;
  }
  device_retry_schedule(daemon, now_ms);
}

static bool setup_devices(inputd *daemon, char *error, size_t error_size) {
  if (daemon->replay_mode) {
    bd_evdev replay;
    memset(&replay, 0, sizeof(replay));
    replay.fd = -1;
    snprintf(replay.path, sizeof(replay.path), "(replay)");
    snprintf(replay.name, sizeof(replay.name), "replay");
    replay.is_gamepad = true;
    device_add(daemon, &replay, true, false, monotonic_ms());
    return true;
  }

  if (daemon->device_dir[0] == '\0') {
    snprintf(daemon->device_dir, sizeof(daemon->device_dir), "/dev/input");
  }
  uint64_t now_ms = monotonic_ms();
  device_watch_update(daemon, now_ms);

  bool retry_needed = false;
  if (!device_reconcile(daemon, now_ms, error, error_size, &retry_needed)) {
    if (errno == ENOENT) {
      log_message(daemon, "%s; waiting for devices", error);
      daemon->scan_error_logged = true;
      return true;
    }
    return false;
  }
  device_watch_update(daemon, now_ms);
  if (daemon->watch_wd >= 0) {
    daemon->scan_backoff_ms = 0;
  }
  if (retry_needed) {
    device_retry_schedule(daemon, now_ms);
  }
  if (daemon->device_count == 0) {
    log_message(daemon, "no devices under %s; socket stays open",
                daemon->device_dir);
  }
  return true;
}

static void daemon_cleanup(inputd *daemon) {
  if (daemon->grab_active) {
    ungrab_devices(daemon);
    daemon->grab_active = false;
  }
  for (size_t i = 0; i < daemon->client_count; ++i) {
    if (daemon->clients[i].fd >= 0) {
      close(daemon->clients[i].fd);
    }
  }
  daemon->client_count = 0;
  for (size_t i = 0; i < daemon->device_count; ++i) {
    if (daemon->devices[i].uinput != NULL) {
      bd_uinput_release_all(daemon->devices[i].uinput);
      bd_uinput_destroy(daemon->devices[i].uinput);
      daemon->devices[i].uinput = NULL;
    }
    if (daemon->devices[i].evdev.fd >= 0) {
      bd_evdev_close(&daemon->devices[i].evdev);
    }
  }
  daemon->device_count = 0;
  if (daemon->watch_fd >= 0) {
    close(daemon->watch_fd);
    daemon->watch_fd = -1;
    daemon->watch_wd = -1;
  }
  if (daemon->listen_fd >= 0) {
    close(daemon->listen_fd);
    daemon->listen_fd = -1;
  }
  if (daemon->socket_owned && daemon->socket_path[0] != '\0') {
    unlink(daemon->socket_path);
    daemon->socket_owned = false;
  }
  if (daemon->replay_file != NULL) {
    fclose(daemon->replay_file);
    daemon->replay_file = NULL;
  }
  if (daemon->config != NULL) {
    bd_config_free(daemon->config);
    daemon->config = NULL;
  }
}

static void print_usage(FILE *stream) {
  fprintf(stream,
          "usage: brickdesk-inputd [options]\n"
          "\n"
          "  --config-dir DIR       configuration directory\n"
          "  --socket PATH          Unix socket (default $BRICKDESK_INPUT_SOCKET\n"
          "                         or /run/brickdesk/inputd.sock)\n"
          "  --device-dir DIR       evdev directory (default /dev/input)\n"
          "  --replay FILE          read timestamped events instead of evdev\n"
          "  --replay-fast          process the replay instantly (implies\n"
          "                         --exit-after-replay)\n"
          "  --exit-after-replay    exit when the replay file ends\n"
          "  --no-grab              reject grab requests\n"
          "  --verbose              log devices and actions\n"
          "  -h, --help             show this help\n"
          "\n"
          "Replay lines: <time_ms> key <name|code> <down|up>\n"
          "              <time_ms> abs <ABS_NAME|code> <value>\n"
          "              <time_ms> sw <headphone|tablet|lid|code> <0|1>\n"
          "              <time_ms> quit\n");
}

int main(int argc, char **argv) {
  inputd daemon;
  memset(&daemon, 0, sizeof(daemon));
  daemon.listen_fd = -1;
  daemon.watch_fd = -1;
  daemon.watch_wd = -1;
  daemon.allow_grab = true;
  daemon.running = true;
  daemon.double_tap_ms = 300;
  snprintf(daemon.scope, sizeof(daemon.scope), "global");

  const char *config_dir = NULL;
  const char *socket_path = NULL;
  const char *device_dir = NULL;
  const char *replay_path = NULL;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (strcmp(arg, "--config-dir") == 0 && i + 1 < argc) {
      config_dir = argv[++i];
    } else if (strcmp(arg, "--socket") == 0 && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (strcmp(arg, "--device-dir") == 0 && i + 1 < argc) {
      device_dir = argv[++i];
    } else if (strcmp(arg, "--replay") == 0 && i + 1 < argc) {
      replay_path = argv[++i];
    } else if (strcmp(arg, "--replay-fast") == 0) {
      daemon.replay_fast = true;
      daemon.replay_exit = true;
    } else if (strcmp(arg, "--exit-after-replay") == 0) {
      daemon.replay_exit = true;
    } else if (strcmp(arg, "--no-grab") == 0) {
      daemon.allow_grab = false;
    } else if (strcmp(arg, "--verbose") == 0) {
      daemon.verbose = true;
    } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      print_usage(stdout);
      return 0;
    } else {
      fprintf(stderr, "brickdesk-inputd: unknown option: %s\n", arg);
      print_usage(stderr);
      return 2;
    }
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);

  bd_config_options options;
  options.config_dir = config_dir;
  options.migrate = false;
  daemon.config_options = options;
  char error[512] = {0};
  daemon.config = bd_config_load(&options, error, sizeof(error));
  if (daemon.config == NULL) {
    fprintf(stderr, "brickdesk-inputd: config: %s\n", error);
    return 1;
  }
  if (bd_config_diagnostic_count(daemon.config) > 0) {
    fprintf(stderr, "brickdesk-inputd: %zu config diagnostic(s)\n",
            bd_config_diagnostic_count(daemon.config));
  }
  load_remap(&daemon);
  if (!bd_hotkey_table_load(&daemon.hotkeys, daemon.config)) {
    fprintf(stderr, "brickdesk-inputd: cannot load hotkeys\n");
    daemon_cleanup(&daemon);
    return 1;
  }
  for (size_t i = 0; i < daemon.hotkeys.diagnostic_count; ++i) {
    const bd_hotkey_diagnostic *diag = &daemon.hotkeys.diagnostics[i];
    fprintf(stderr, "brickdesk-inputd: %s: %s%s%s\n",
            diag->error ? "error" : "warning",
            diag->binding[0] ? diag->binding : "",
            diag->binding[0] ? ": " : "", diag->message);
    if (diag->file[0] != '\0' && diag->line > 0) {
      fprintf(stderr, "brickdesk-inputd:   at %s:%d\n", diag->file, diag->line);
    }
  }
  for (size_t i = 0; i < daemon.hotkeys.conflict_count; ++i) {
    const bd_hotkey_conflict *conflict = &daemon.hotkeys.conflicts[i];
    fprintf(stderr, "brickdesk-inputd: %s: %s\n",
            conflict->kind == BD_HOTKEY_CONFLICT_DUPLICATE ? "error"
                                                           : "warning",
            conflict->message);
  }
  if (!bd_device_map_load(&daemon.map, daemon.config)) {
    fprintf(stderr, "brickdesk-inputd: cannot load input.ini\n");
    daemon_cleanup(&daemon);
    return 1;
  }
  daemon.double_tap_ms =
      (int)bd_config_get_int(daemon.config, "input.double_tap_ms", 300);
  if (daemon.double_tap_ms <= 0) {
    daemon.double_tap_ms = 300;
  }

  if (replay_path != NULL) {
    daemon.replay_mode = true;
    daemon.replay_file = fopen(replay_path, "r");
    if (daemon.replay_file == NULL) {
      fprintf(stderr, "brickdesk-inputd: %s: %s\n", replay_path,
              strerror(errno));
      daemon_cleanup(&daemon);
      return 1;
    }
    daemon.replay_base_ms = monotonic_ms();
    daemon.replay_last_ms = 0;
  }

  if (device_dir != NULL && *device_dir != '\0') {
    int written =
        snprintf(daemon.device_dir, sizeof(daemon.device_dir), "%s",
                 device_dir);
    if (written <= 0 || (size_t)written >= sizeof(daemon.device_dir)) {
      fprintf(stderr, "brickdesk-inputd: device directory path too long\n");
      daemon_cleanup(&daemon);
      return 1;
    }
  }

  suppress_compute(&daemon);
  if (!setup_devices(&daemon, error, sizeof(error))) {
    fprintf(stderr, "brickdesk-inputd: %s\n", error);
    daemon_cleanup(&daemon);
    return 1;
  }
  if (!setup_socket(&daemon, socket_path)) {
    daemon_cleanup(&daemon);
    return 1;
  }
  if (daemon.verbose) {
    log_message(&daemon, "listening on %s (%zu device(s))", daemon.socket_path,
                daemon.device_count);
  }

  if (daemon.replay_mode) {
    replay_next_event(&daemon);
    if (daemon.replay_fast) {
      run_replay_fast(&daemon);
      daemon.running = false;
    }
  }

  while (daemon.running && !g_stop) {
    uint64_t now_ms = daemon_now(&daemon);
    /*
     * Replay events carry their own timestamps. Apply them before advancing
     * the state to the polling time; otherwise a late poll stamps a hold
     * press with the poll time and the following release can cancel it.
     */
    if (daemon.replay_mode && !daemon.replay_fast) {
      process_replay_due(&daemon, now_ms);
    }
    device_service(&daemon, now_ms);
    for (size_t i = 0; i < daemon.device_count; ++i) {
      output_context context;
      context.daemon = &daemon;
      context.device = &daemon.devices[i];
      bd_input_state_advance(&daemon.devices[i].state, now_ms, dispatch_output,
                             &context);
    }
    if (daemon.grab_pending) {
      request_ungrab(&daemon, now_ms);
    }
    if (!daemon.running) {
      break;
    }

    client_compact(&daemon);
    struct pollfd
        fds[2 + BD_INPUTD_MAX_CLIENTS + BD_INPUTD_MAX_DEVICES];
    size_t count = 0;
    fds[count].fd = daemon.listen_fd;
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    ++count;
    size_t client_offset = count;
    size_t clients_before = daemon.client_count;
    for (size_t i = 0; i < clients_before; ++i) {
      fds[count].fd = daemon.clients[i].fd;
      fds[count].events = POLLIN;
      fds[count].revents = 0;
      ++count;
    }
    size_t device_offset = count;
    for (size_t i = 0; i < daemon.device_count; ++i) {
      if (daemon.devices[i].evdev.fd < 0) {
        continue;
      }
      fds[count].fd = daemon.devices[i].evdev.fd;
      fds[count].events = POLLIN;
      fds[count].revents = 0;
      ++count;
    }
    size_t watch_index = count;
    if (daemon.watch_fd >= 0) {
      fds[count].fd = daemon.watch_fd;
      fds[count].events = POLLIN;
      fds[count].revents = 0;
      ++count;
    }

    int timeout = compute_timeout(&daemon, now_ms);
    int result = poll(fds, (nfds_t)count, timeout);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      log_message(&daemon, "poll: %s", strerror(errno));
      break;
    }
    if (result == 0) {
      continue;
    }

    now_ms = daemon_now(&daemon);
    if ((fds[0].revents & POLLIN) != 0) {
      client_accept(&daemon);
    }
    for (size_t i = 0; i < clients_before; ++i) {
      int fd = fds[client_offset + i].fd;
      short revents = fds[client_offset + i].revents;
      if ((revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        continue;
      }
      for (size_t c = 0; c < daemon.client_count; ++c) {
        if (daemon.clients[c].fd == fd) {
          client_read(&daemon, c, now_ms);
          break;
        }
      }
    }
    size_t fd_index = device_offset;
    for (size_t i = 0; i < daemon.device_count; ++i) {
      if (daemon.devices[i].evdev.fd < 0) {
        continue;
      }
      short revents = fds[fd_index].revents;
      bool lost = (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
      int read_errno = 0;
      if ((revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
        for (;;) {
          bd_evdev_event event;
          errno = 0;
          int read_result = bd_evdev_read(&daemon.devices[i].evdev, &event);
          if (read_result > 0) {
            apply_evdev_event(&daemon, &daemon.devices[i], &event,
                              event.time_ms);
            continue;
          }
          if (read_result < 0) {
            read_errno = errno;
            lost = true;
          }
          break;
        }
      }
      if (lost) {
        char reason[96];
        if (read_errno != 0) {
          snprintf(reason, sizeof(reason), "read: %s", strerror(read_errno));
        } else if ((revents & POLLNVAL) != 0) {
          snprintf(reason, sizeof(reason), "invalid fd");
        } else {
          snprintf(reason, sizeof(reason), "hangup");
        }
        device_remove(&daemon, &daemon.devices[i], now_ms, reason, true);
      }
      ++fd_index;
    }
    if (watch_index < count &&
        (fds[watch_index].revents &
         (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0) {
      device_drain_watch(&daemon, now_ms);
    }
    client_compact(&daemon);
  }

  daemon_cleanup(&daemon);
  return 0;
}
