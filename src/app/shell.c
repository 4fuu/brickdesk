#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "brickdesk/shell.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "brickdesk/device_av.h"
#include "brickdesk/device_battery.h"
#include "brickdesk/app_history.h"
#include "brickdesk/device_display.h"
#include "brickdesk/battery_history.h"
#include "brickdesk/haptics.h"
#include "brickdesk/led.h"
#include "brickdesk/notice.h"
#include "brickdesk/perf_client.h"
#include "brickdesk/sysinfo.h"
#include "brickdesk/theme.h"
#include "brickdesk/ui.h"

#define BD_SHELL_LINE_MAX 4096
#define BD_SHELL_FD_MAX 64
#define BD_SHELL_PENDING_MAX 16
#define BD_SHELL_PATH_MAX 1024
#define BD_SHELL_STATUS_MS 5000
#define BD_SHELL_MEMORY_MS 1000
#define BD_SHELL_MEMORY_RETRY_MS 5000
#define BD_SHELL_PLUGIN_ITEM_ID "plugin-tile"
#define BD_SHELL_SETTINGS_ITEM_ID "brickdesk-settings"
#define BD_SHELL_PERF_TIMEOUT_MS 250
#define BD_SHELL_BATTERY_RETRY_MS 60000
#define BD_SHELL_INPUTD_BACKOFF_MIN_MS 250u
#define BD_SHELL_INPUTD_BACKOFF_MAX_MS 5000u
#define BD_SHELL_INPUTD_CONNECT_TIMEOUT_MS 1000u
/* A tap shorter than this queues light sleep; long-press policy is separate. */
#define BD_SHELL_POWER_TAP_MS 1000u
/* Ignore the wake press and immediate follow-up taps for this long. */
#define BD_SHELL_POWER_WAKE_IGNORE_MS 1000u

/*
 * Sent once per accepted connection.  subscribe restores the action and key
 * streams.  It is deliberately side-effect-free for other clients: the
 * daemon's release_all command resets every device and broadcasts a release,
 * which would drop another subscribed game or menu client's held input.
 */
static const char BD_SHELL_INPUTD_SUBSCRIBE[] =
    "{\"cmd\":\"subscribe\",\"actions\":true,\"keys\":true}\n";

typedef enum shell_pending_kind {
  SHELL_PENDING_STATE = 0,
  SHELL_PENDING_EXIT,
  SHELL_PENDING_MENU_ACTION,
  SHELL_PENDING_ATTENTION,
  SHELL_PENDING_NOTIFICATION
} shell_pending_kind;

typedef enum shell_power_request {
  SHELL_POWER_REQUEST_NONE = 0,
  SHELL_POWER_REQUEST_SLEEP,
  SHELL_POWER_REQUEST_DEEP_SLEEP,
  SHELL_POWER_REQUEST_WAKE,
  SHELL_POWER_REQUEST_POWEROFF,
  SHELL_POWER_REQUEST_REBOOT
} shell_power_request;

typedef enum shell_power_long_action {
  SHELL_POWER_LONG_SHUTDOWN = 0,
  SHELL_POWER_LONG_REBOOT,
  SHELL_POWER_LONG_NONE
} shell_power_long_action;

typedef struct shell_pending {
  shell_pending_kind kind;
  bd_session_state state;
  char app_id[BD_APP_ID_MAX];
  char method[32];
  char params[512];
} shell_pending;

typedef struct shell_app_runtime {
  bool running;
  bool background;
  int memory_mb;           /* process-tree RSS; -1 until sampled / unknown */
  uint64_t memory_sampled_ms;
  char frame_path[BD_APP_PATH_MAX];
} shell_app_runtime;

struct bd_shell {
  bd_platform *platform;
  bd_gfx *gfx;
  bd_config *settings;
  bd_plugin_host *plugins;
  bool verbose;

  bd_settings *settings_page;
  bd_notice *notice;
  bd_settings_system system;
  bd_av_brightness av_brightness;
  bool av_brightness_ready;
  bd_av_audio av_audio;
  bool av_audio_ready;
  bd_display display;
  bool display_ready;
  bool dp_connected_known;
  bool dp_connected;
  uint64_t dp_next_ms;
  bd_home_quick_backend home_quick;
  char perf_socket[BD_PERF_PATH_MAX];
  char battery_root[BD_SHELL_PATH_MAX];

  bd_battery battery;
  bd_battery_policy battery_policy;
  bool battery_policy_ready;
  bool battery_opened;
  bool battery_charging;
  int battery_stage; /* 0 normal, 1 reminder, 2 critical */
  uint64_t battery_next_ms;
  bd_battery_history *battery_history;
  char battery_history_path[BD_SHELL_PATH_MAX];
  bd_app_history *app_history;
  char app_history_path[BD_SHELL_PATH_MAX];
  bool shutdown_on_low_battery;
  uint64_t low_battery_shutdown_ms;
  bool low_battery_shutdown_armed;
  bool battery_charging_known;

  bd_led *led;
  bd_haptics *haptics;
  bd_led_profile led_profile;
  bool led_profile_valid;
  bool led_pulse_active;
  uint64_t led_pulse_off_ms;
  bool led_ambient_known;
  bool led_ambient_dirty;
  uint32_t led_ambient_rgb;
  uint64_t led_ambient_next_ms;
  char led_app_id[BD_APP_ID_MAX];

  bd_app_list *apps;
  shell_app_runtime *runtime;
  size_t runtime_count;
  bd_home *home;
  bd_home_style home_style;
  bd_plugin *plugin_tile;
  char plugin_tile_name[BD_PLUGIN_MAX_NAME + 1];
  int plugin_tile_span_w;
  int plugin_tile_span_h;
  bd_ui_fonts plugin_fonts;
  bool plugin_fonts_loaded;
  bd_accent_palette plugin_accent;

  bd_session *session;
  int session_next_ms;

  bd_power_linux power_linux;
  bd_power *power;
  shell_power_request power_request;
  bool power_key_down;
  bool power_long_fired;
  bool power_press_ignored;
  shell_power_long_action power_long_action;
  uint32_t power_sleep_after_ms;
  uint32_t power_deep_sleep_after_ms;
  uint64_t power_down_ms;
  uint64_t power_ignore_until_ms;
  uint64_t last_activity_ms;
  uint64_t light_sleep_since_ms;
  bool recovery_notified;
  bool power_input_suspended;
  bool power_swallow;
  bool power_brightness_known;
  int power_brightness;
  char power_app_id[BD_APP_ID_MAX];
  bool power_app_frozen;

  bd_hotkey_table hotkeys;
  bd_input_state input;
  bd_input_state test_input;
  const bd_hotkey_table *hotkey_table;

  int inputd_fd;
  bool inputd_connected;
  bool inputd_connecting;
  uint64_t inputd_retry_ms;
  uint64_t inputd_deadline_ms;
  uint32_t inputd_backoff_ms;
  size_t inputd_sent;
  char input_socket[sizeof(((struct sockaddr_un *)0)->sun_path)];
  char input_line[BD_SHELL_LINE_MAX];
  size_t input_length;

  char menu_shim[BD_SHELL_PATH_MAX];
  bool menu_shim_checked;

  shell_pending pending[BD_SHELL_PENDING_MAX];
  int pending_count;
  bool rescan;

  char dialog_app[BD_APP_ID_MAX];    /* end-dialog target */
  char pending_launch[BD_APP_ID_MAX];/* launch waiting on an eviction */
  char evict_app[BD_APP_ID_MAX];     /* LRU app to end first */
  char return_app[BD_APP_ID_MAX];    /* return deferred by a launch */
  bool evict_failed;                 /* terminate failed; wait for user retry */

  char clock[16];
  int status_battery;
  bool status_wifi_known;
  bool status_wifi;
  int status_max_background;
  uint64_t status_sampled_ms;
  int memory_budget_mb;
  uint64_t memory_budget_next_ms;
  bool memory_budget_prompt;
  char memory_budget_victim[BD_APP_ID_MAX];
  bool menu_shim_warned;
  bool plugin_error_notified;
  bool crash_prompt;
  char crash_retry_app[BD_APP_ID_MAX];
  unsigned screenshot_seq;

  bool dirty;
  bool visible;
  bool quit;
  uint64_t presents;

  /* Scenario harness clock: home transitions advance only on test_advance. */
  bool test_clock;
  uint64_t test_now_ms;
};

static void shell_led_refresh_internal(bd_shell *shell);

static uint64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* Home animations use the scenario clock when the harness drives one. */
static uint64_t shell_now(const bd_shell *shell) {
  return shell != NULL && shell->test_clock ? shell->test_now_ms
                                            : monotonic_ms();
}

static void copy_text(char *dst, size_t size, const char *src) {
  if (size == 0) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  size_t i = 0;
  while (src[i] != '\0' && i + 1 < size) {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
}

static int merge_timeout(int best, int value) {
  if (value < 0) {
    return best;
  }
  if (best < 0 || value < best) {
    return value;
  }
  return best;
}

/* ------------------------------------------------------ device and status */

static void shell_publish_status(bd_shell *shell, bool force) {
  (void)force;
  if (shell == NULL || shell->home == NULL) {
    return;
  }
  int max_background =
      (int)bd_config_get_int(shell->settings, "session.max_background", 3);
  bd_home_status home_status;
  memset(&home_status, 0, sizeof(home_status));
  home_status.clock = shell->clock;
  home_status.wifi_known = shell->status_wifi_known;
  home_status.wifi = shell->status_wifi;
  home_status.battery_percent = shell->status_battery;
  home_status.max_background = max_background;
  bd_home_set_status(shell->home, &home_status);
  if (shell->settings_page != NULL) {
    bd_settings_status settings_status;
    memset(&settings_status, 0, sizeof(settings_status));
    settings_status.clock = shell->clock;
    settings_status.wifi_known = shell->status_wifi_known;
    settings_status.wifi = shell->status_wifi;
    settings_status.battery_percent = shell->status_battery;
    bd_settings_set_status(shell->settings_page, &settings_status);
  }
  shell->status_max_background = max_background;
  shell->dirty = true;
}

static void shell_refresh_storage(bd_shell *shell) {
  if (shell == NULL || shell->settings_page == NULL) {
    return;
  }
  const char *dir = bd_config_dir(shell->settings);
  struct statvfs info;
  if (dir == NULL || statvfs(dir, &info) != 0 || info.f_frsize == 0) {
    bd_settings_storage storage = {-1, -1};
    bd_settings_set_storage(shell->settings_page, &storage);
    return;
  }
  uint64_t total = (uint64_t)info.f_blocks * info.f_frsize;
  uint64_t free_bytes = (uint64_t)info.f_bavail * info.f_frsize;
  bd_settings_storage storage;
  storage.total_mb = (long)(total / (1024u * 1024u));
  storage.free_mb = (long)(free_bytes / (1024u * 1024u));
  bd_settings_set_storage(shell->settings_page, &storage);
}

static bool shell_system_read(void *user, bd_home_quick_control control,
                              int *percent) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL || percent == NULL) {
    return false;
  }
  if (control == BD_HOME_QUICK_BRIGHTNESS) {
    return shell->system.get_brightness != NULL &&
           shell->system.get_brightness(shell->system.user, percent);
  }
  return shell->system.get_volume != NULL &&
         shell->system.get_volume(shell->system.user, percent);
}

static bool shell_system_write(bd_shell *shell,
                               bd_home_quick_control control, int percent) {
  if (control == BD_HOME_QUICK_BRIGHTNESS) {
    return shell->system.set_brightness != NULL &&
           shell->system.set_brightness(shell->system.user, percent);
  }
  return shell->system.set_volume != NULL &&
         shell->system.set_volume(shell->system.user, percent);
}

static bool shell_home_adjust(void *user, bd_home_quick_control control,
                              int delta, int *percent) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL || percent == NULL) {
    return false;
  }
  int current = 0;
  if (!shell_system_read(shell, control, &current)) {
    return false;
  }
  int next = current + delta;
  if (next < 0) {
    next = 0;
  }
  if (next > 100) {
    next = 100;
  }
  if (!shell_system_write(shell, control, next)) {
    return false;
  }
  int actual = 0;
  if (!shell_system_read(shell, control, &actual)) {
    return false;
  }
  const char *key = control == BD_HOME_QUICK_BRIGHTNESS
                        ? "display.brightness"
                        : "audio.volume";
  char value[16];
  char error[128] = {0};
  snprintf(value, sizeof(value), "%d", actual);
  if (!bd_config_set(shell->settings, key, value, error, sizeof(error))) {
    if (shell->verbose) {
      fprintf(stderr, "brickdesk: config: %s\n", error);
    }
  } else if (!bd_config_save(shell->settings, error, sizeof(error))) {
    if (shell->verbose) {
      fprintf(stderr, "brickdesk: save: %s\n", error);
    }
  }
  *percent = actual;
  return true;
}

static void shell_home_quick_refresh(bd_shell *shell) {
  if (shell != NULL && shell->home != NULL) {
    bd_home_refresh_quick(shell->home);
  }
}

static bool shell_brightness_set(void *user, int percent) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL || !shell->av_brightness_ready) {
    return false;
  }
  bd_av_error error;
  return bd_av_brightness_set(&shell->av_brightness, percent, &error) ==
         BD_AV_OK;
}

static bool shell_brightness_get(void *user, int *percent) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL || percent == NULL || !shell->av_brightness_ready) {
    return false;
  }
  bd_av_error error;
  return bd_av_brightness_get(&shell->av_brightness, percent, &error) ==
         BD_AV_OK;
}

static bool shell_volume_set(void *user, int percent) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL || !shell->av_audio_ready) {
    return false;
  }
  bd_av_error error;
  return bd_av_audio_set_volume(&shell->av_audio, percent, &error) == BD_AV_OK;
}

static bool shell_volume_get(void *user, int *percent) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL || percent == NULL || !shell->av_audio_ready) {
    return false;
  }
  bd_av_error error;
  return bd_av_audio_get_volume(&shell->av_audio, percent, &error) == BD_AV_OK;
}

static bool shell_mute_set(void *user, bool muted) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL || !shell->av_audio_ready) {
    return false;
  }
  bd_av_error error;
  return bd_av_audio_set_muted(&shell->av_audio, muted, &error) == BD_AV_OK;
}

static bool shell_mute_get(void *user, bool *muted) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL || muted == NULL || !shell->av_audio_ready) {
    return false;
  }
  bd_av_error error;
  return bd_av_audio_get_muted(&shell->av_audio, muted, &error) == BD_AV_OK;
}

static void shell_open_devices(bd_shell *shell, const bd_shell_config *config) {
  memset(&shell->system, 0, sizeof(shell->system));
  memset(&shell->av_brightness, 0, sizeof(shell->av_brightness));
  memset(&shell->av_audio, 0, sizeof(shell->av_audio));
  shell->av_brightness_ready = false;
  shell->av_audio_ready = false;
  if (config->settings_system != NULL) {
    shell->system = *config->settings_system;
    shell->home_quick.read = shell_system_read;
    shell->home_quick.adjust = shell_home_adjust;
    shell->home_quick.user = shell;
    return;
  }

  bd_av_error error;
  bd_av_brightness_config brightness_config;
  memset(&brightness_config, 0, sizeof(brightness_config));
  brightness_config.sysfs_root = config->av_sysfs_root;
  brightness_config.backend = BD_AV_BRIGHTNESS_AUTO;
  bd_av_result brightness_result = bd_av_brightness_probe(
      &shell->av_brightness, &brightness_config, &error);
  shell->av_brightness_ready = brightness_result == BD_AV_OK;
  if (!shell->av_brightness_ready && shell->verbose) {
    fprintf(stderr, "brickdesk: brightness unavailable: %s\n",
            error.message[0] != '\0' ? error.message : "probe failed");
  }

  bd_av_audio_config audio_config;
  memset(&audio_config, 0, sizeof(audio_config));
  audio_config.snd_root = config->av_snd_root;
  audio_config.proc_cards = config->av_proc_cards;
  audio_config.speaker_mute = config->av_speaker_mute;
  bd_av_result audio_result =
      bd_av_audio_open(&shell->av_audio, &audio_config, &error);
  shell->av_audio_ready = audio_result == BD_AV_OK;
  if (!shell->av_audio_ready && shell->verbose) {
    fprintf(stderr, "brickdesk: audio unavailable: %s\n",
            error.message[0] != '\0' ? error.message : "probe failed");
  }

  shell->system.user = shell;
  shell->system.set_brightness = shell_brightness_set;
  shell->system.set_volume = shell_volume_set;
  shell->system.set_muted = shell_mute_set;
  shell->system.get_brightness = shell_brightness_get;
  shell->system.get_volume = shell_volume_get;
  shell->system.get_muted = shell_mute_get;
  shell->home_quick.read = shell_system_read;
  shell->home_quick.adjust = shell_home_adjust;
  shell->home_quick.user = shell;
}

static bool shell_display_apply(bd_shell *shell) {
  if (shell == NULL || !shell->display_ready) {
    return false;
  }
  bd_display_settings settings;
  memset(&settings, 0, sizeof(settings));
  settings.color_temperature =
      (int)bd_config_get_int(shell->settings, "display.color_temperature", 0);
  settings.contrast =
      (int)bd_config_get_int(shell->settings, "display.contrast", 50);
  settings.saturation =
      (int)bd_config_get_int(shell->settings, "display.saturation", 50);
  settings.exposure =
      (int)bd_config_get_int(shell->settings, "display.exposure", 50);
  settings.white_point =
      bd_config_get_bool(shell->settings, "display.white_point", true);
  settings.red_gain =
      (int)bd_config_get_int(shell->settings, "display.white_point_red", 100);
  settings.green_gain = (int)bd_config_get_int(
      shell->settings, "display.white_point_green", 94);
  settings.blue_gain = (int)bd_config_get_int(
      shell->settings, "display.white_point_blue", 54);
  bd_display_error error;
  bd_display_error_clear(&error);
  bd_display_result result =
      bd_display_apply(&shell->display, &settings, &error);
  if (result != BD_DISPLAY_OK) {
    if (shell->verbose) {
      fprintf(stderr, "brickdesk: display: %s\n",
              error.message[0] != '\0' ? error.message
                                       : bd_display_result_name(result));
    }
    return false;
  }
  if (shell->verbose && shell->display.white_point_error[0] != '\0') {
    fprintf(stderr, "brickdesk: display: %s\n",
            shell->display.white_point_error);
  }
  return true;
}

static void shell_display_dp_poll(bd_shell *shell, uint64_t now) {
  if (shell == NULL || !shell->display_ready || now < shell->dp_next_ms) {
    return;
  }
  shell->dp_next_ms = now + 5000;
  bool connected = false;
  char connector[64];
  if (!bd_display_dp_connected(&shell->display, &connected, connector,
                               sizeof(connector))) {
    return;
  }
  if (shell->dp_connected_known && shell->dp_connected == connected) {
    return;
  }
  shell->dp_connected_known = true;
  shell->dp_connected = connected;
  char message[160];
  if (connected) {
    snprintf(message, sizeof(message), "外接显示已连接：%s", connector);
  } else {
    snprintf(message, sizeof(message), "外接显示已断开");
  }
  bd_shell_notice(shell, BD_NOTICE_INFO, message, 0);
}

static bool shell_perf_apply(void *user, const char *profile, char *error,
                             size_t error_size) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL) {
    return false;
  }
  bd_perf_client_result result;
  bd_perf_client_status status = bd_perf_client_set_global(
      shell->perf_socket, profile, BD_SHELL_PERF_TIMEOUT_MS, &result, error,
      error_size);
  if (status != BD_PERF_CLIENT_OK) {
    if (error != NULL && error_size > 0 && error[0] == '\0') {
      snprintf(error, error_size, "perf 服务%s", bd_perf_client_status_name(status));
    }
    return false;
  }
  if (strcmp(result.profile, profile) != 0) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "perf 确认了 %s，而不是 %s", result.profile,
               profile);
    }
    return false;
  }
  return true;
}

static void shell_config_changed(void *user, const char *key) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL || key == NULL) {
    return;
  }
  if (strcmp(key, "theme.accent") == 0) {
    const char *accent_name =
        bd_config_get_string(shell->settings, "theme.accent", "persimmon");
    const char *hex = bd_config_accent_hex(accent_name, true);
    bd_color accent;
    if (hex != NULL && bd_color_from_hex(hex, &accent)) {
      shell->home_style.accent_base = accent;
      bd_home_set_accent(shell->home, accent);
      shell->plugin_accent = bd_accent_palette_from(accent, true);
      if (shell->plugins != NULL) {
        uint32_t rgb = ((uint32_t)(accent.r * 255.0f + 0.5f) << 16) |
                       ((uint32_t)(accent.g * 255.0f + 0.5f) << 8) |
                       (uint32_t)(accent.b * 255.0f + 0.5f);
        bd_plugin_host_set_theme(shell->plugins, true, rgb);
      }
      shell->dirty = true;
    }
  } else if (strcmp(key, "ui.animations") == 0) {
    bd_home_set_animations(
        shell->home, bd_config_get_bool(shell->settings, "ui.animations", true));
    shell->dirty = true;
  } else if (strcmp(key, "ui.blur") == 0) {
    bool blur = bd_config_get_bool(shell->settings, "ui.blur", true);
    shell->home_style.blur_enabled = blur;
    bd_home_set_blur_enabled(shell->home, blur);
    shell->dirty = true;
  } else if (strcmp(key, "theme.ambient") == 0) {
    bool solid =
        strcmp(bd_config_get_string(shell->settings, "theme.ambient", "baked"),
               "solid") == 0;
    shell->home_style.ambient_solid = solid;
    bd_home_set_ambient_solid(shell->home, solid);
    shell->dirty = true;
  } else if (strcmp(key, "display.brightness") == 0 ||
             strcmp(key, "audio.volume") == 0 ||
             strcmp(key, "audio.mute") == 0) {
    shell_home_quick_refresh(shell);
  } else if (strncmp(key, "display.", 8) == 0) {
    shell_display_apply(shell);
  } else if (strcmp(key, "session.max_background") == 0) {
    shell_publish_status(shell, true);
  } else if (strncmp(key, "led.", 4) == 0 ||
             strncmp(key, "input.haptic", 12) == 0) {
    shell->led_profile_valid = false;
    shell->led_ambient_dirty = true;
    shell_led_refresh_internal(shell);
  } else if (strcmp(key, "power.sleep_after") == 0 ||
             strcmp(key, "power.deep_sleep_after") == 0) {
    long sleep_after =
        bd_config_get_int(shell->settings, "power.sleep_after", 60000);
    if (sleep_after < 0 || sleep_after > 86400000) {
      sleep_after = 60000;
    }
    long deep_sleep_after =
        bd_config_get_int(shell->settings, "power.deep_sleep_after", 30000);
    if (deep_sleep_after < 0 || deep_sleep_after > 86400000) {
      deep_sleep_after = 30000;
    }
    shell->power_sleep_after_ms = (uint32_t)sleep_after;
    shell->power_deep_sleep_after_ms = (uint32_t)deep_sleep_after;
  } else if (strcmp(key, "power.long_press_action") == 0) {
    const char *action = bd_config_get_string(
        shell->settings, "power.long_press_action", "shutdown");
    if (action != NULL && strcmp(action, "reboot") == 0) {
      shell->power_long_action = SHELL_POWER_LONG_REBOOT;
    } else if (action != NULL && strcmp(action, "none") == 0) {
      shell->power_long_action = SHELL_POWER_LONG_NONE;
    } else {
      shell->power_long_action = SHELL_POWER_LONG_SHUTDOWN;
    }
  } else if (strcmp(key, "power.safe_shutdown") == 0) {
    bd_power_linux_set_safe_poweroff(
        &shell->power_linux,
        bd_config_get_bool(shell->settings, "power.safe_shutdown", true));
  }
}

static void shell_battery_policy_config(int low_percent, bool recommend_shutdown,
                                        bd_battery_policy_config *config) {
  bd_battery_policy_config_default(config);
  int critical = low_percent;
  if (critical < 0) {
    critical = 0;
  }
  /* critical_percent must stay below reminder_percent (<=100). */
  if (critical > 99) {
    critical = 99;
  }
  int reminder = critical < 20 ? 20 : critical + 5;
  if (reminder > 100) {
    reminder = 100;
  }
  if (reminder <= critical) {
    reminder = critical + 1;
  }
  config->critical_percent = critical;
  config->reminder_percent = reminder;
  if (reminder + config->reminder_hysteresis_percent > 100) {
    config->reminder_hysteresis_percent = 100 - reminder;
  }
  if (critical + config->critical_hysteresis_percent > 100) {
    config->critical_hysteresis_percent = 100 - critical;
  }
  config->recommend_shutdown = recommend_shutdown;
}

static void shell_battery_sample(bd_shell *shell, uint64_t now) {
  if (shell == NULL || now < shell->battery_next_ms) {
    return;
  }
  if (!shell->battery_opened) {
    bd_battery_config config;
    memset(&config, 0, sizeof(config));
    config.sysfs_root = shell->battery_root[0] != '\0' ? shell->battery_root
                                                        : NULL;
    config.battery_name = NULL;
    bd_battery_error error;
    bd_battery_result opened = bd_battery_open(&shell->battery, &config, &error);
    shell->battery_opened = opened != BD_BATTERY_ERR_ARGUMENT;
    if (opened != BD_BATTERY_OK && shell->verbose) {
      fprintf(stderr, "brickdesk: battery: %s\n", error.message);
    }
  }
  bd_battery_status status;
  memset(&status, 0, sizeof(status));
  bd_battery_error error;
  bd_battery_result read = bd_battery_read(&shell->battery, &status, &error);
  if (read == BD_BATTERY_ERR_STALE) {
    shell->battery_opened = false;
  }
  int percent = -1;
  bool charging = false;
  if (read == BD_BATTERY_OK && status.capacity_known) {
    percent = status.capacity_percent;
  }
  if (read == BD_BATTERY_OK) {
    if (status.external_power_known) {
      charging = status.external_power;
    } else if (status.status_known &&
               (status.status == BD_BATTERY_CHARGE_CHARGING ||
                status.status == BD_BATTERY_CHARGE_FULL)) {
      charging = true;
    }
  }
  bool charging_known =
      read == BD_BATTERY_OK &&
      (status.external_power_known || status.status_known);
  if (charging_known != shell->battery_charging_known ||
      charging != shell->battery_charging) {
    shell->battery_charging_known = charging_known;
    shell->battery_charging = charging;
    shell->led_profile_valid = false;
  }
  if (percent >= 0 && shell->battery_history != NULL &&
      getenv("BRICKDESK_HEADLESS") == NULL) {
    /* Headless tests use the repository config dir; never write samples there. */
    size_t before = bd_battery_history_count(shell->battery_history);
    bd_battery_history_record(shell->battery_history, (int64_t)time(NULL),
                              percent, charging);
    if (bd_battery_history_count(shell->battery_history) > before &&
        shell->battery_history_path[0] != '\0') {
      bd_battery_history_save(shell->battery_history,
                              shell->battery_history_path);
    }
  }
  if (percent != shell->status_battery) {
    shell->status_battery = percent;
    shell_publish_status(shell, true);
  }
  if (shell->battery_policy_ready) {
    bd_battery_policy_output output;
    memset(&output, 0, sizeof(output));
    bd_battery_policy_result policy_result = bd_battery_policy_update(
        &shell->battery_policy, read == BD_BATTERY_OK ? &status : NULL, now,
        &output);
    if (policy_result != BD_BATTERY_POLICY_OK) {
      fprintf(stderr, "brickdesk: battery policy stopped: %s\n",
              bd_battery_policy_result_name(policy_result));
      shell->battery_policy_ready = false;
    } else {
      shell->battery_next_ms = output.next_sample_ms;
      if (output.events & BD_BATTERY_EVENT_CRITICAL) {
        if (shell->battery_stage < 2) {
          shell->battery_stage = 2;
          bd_shell_notice(shell, BD_NOTICE_ERROR, "电量极低，请立即连接充电器",
                          0);
        }
        if (shell->shutdown_on_low_battery && !charging &&
            !shell->low_battery_shutdown_armed) {
          shell->low_battery_shutdown_armed = true;
          shell->low_battery_shutdown_ms = now + 30000u;
          bd_shell_notice(shell, BD_NOTICE_WARNING,
                          "电量极低，30 秒后自动关机", 6000);
        }
      } else if (output.events & BD_BATTERY_EVENT_LOW) {
        if (shell->battery_stage < 1) {
          shell->battery_stage = 1;
          bd_shell_notice(shell, BD_NOTICE_WARNING, "电量低，请充电", 0);
        }
      }
      if (output.events & BD_BATTERY_EVENT_RECOVERED) {
        shell->battery_stage = 0;
        shell->low_battery_shutdown_armed = false;
      }
      if (output.events & BD_BATTERY_EVENT_PLUGGED) {
        shell->low_battery_shutdown_armed = false;
      }
      if (output.recommendation == BD_BATTERY_RECOMMEND_PREPARE_SHUTDOWN) {
        if (shell->shutdown_on_low_battery && !charging &&
            !shell->low_battery_shutdown_armed) {
          shell->low_battery_shutdown_armed = true;
          shell->low_battery_shutdown_ms = now + 30000u;
          bd_shell_notice(shell, BD_NOTICE_WARNING,
                          "电量极低，30 秒后自动关机", 6000);
        }
      }
    }
  }
  if (!shell->battery_policy_ready || shell->battery_next_ms == 0) {
    /* Explicit degraded cadence; never a silent fast polling loop. */
    shell->battery_next_ms = now + BD_SHELL_BATTERY_RETRY_MS;
  }
}

/* ------------------------------------------------------------ LED/haptics */

static int shell_low_battery_percent(bd_shell *shell) {
  long value =
      bd_config_get_int(shell->settings, "power.low_battery_percent", 5);
  if (value < 0) {
    value = 0;
  }
  if (value > 99) {
    value = 99;
  }
  return (int)value;
}

static bool shell_app_ambient_enabled(bd_shell *shell, const char *app_id) {
  if (app_id == NULL || app_id[0] == '\0') {
    return false;
  }
  if (!bd_config_get_bool(shell->settings, "led.ambient", false)) {
    return false;
  }
  char key[160];
  snprintf(key, sizeof(key), "led.app.%s.ambient", app_id);
  return bd_config_get_bool(shell->settings, key, true);
}

static void shell_led_apply(bd_shell *shell, bd_led_profile profile,
                            uint32_t ambient_rgb) {
  if (shell->led == NULL) {
    return;
  }
  char error[192] = {0};
  bool ok = profile == BD_LED_PROFILE_AMBIENT
                ? bd_led_apply_ambient(shell->led, ambient_rgb, error,
                                       sizeof(error))
                : bd_led_apply_profile(shell->led, profile, error,
                                       sizeof(error));
  if (!ok && shell->verbose) {
    fprintf(stderr, "brickdesk: led: %s\n",
            error[0] != '\0' ? error : "apply failed");
  }
  shell->led_profile = profile;
  shell->led_profile_valid = true;
}

static void shell_led_refresh_internal(bd_shell *shell) {
  if (shell == NULL || shell->led == NULL) {
    return;
  }
  uint64_t now = shell_now(shell);
  if (shell->led_pulse_active) {
    if (now < shell->led_pulse_off_ms) {
      return;
    }
    shell->led_pulse_active = false;
    char error[192] = {0};
    bd_led_off(shell->led, error, sizeof(error));
    shell->led_profile = BD_LED_PROFILE_OFF;
    shell->led_profile_valid = true;
    return;
  }
  if (!bd_led_available(shell->led)) {
    return;
  }

  int critical = shell_low_battery_percent(shell);
  bd_led_profile profile = BD_LED_PROFILE_DEFAULT;
  if (shell->battery_charging_known && shell->battery_charging) {
    profile = BD_LED_PROFILE_CHARGING;
  } else if (shell->status_battery >= 0 &&
             shell->status_battery <= critical) {
    profile = BD_LED_PROFILE_CRITICAL_BATTERY;
  } else if (shell->status_battery >= 0 &&
             shell->status_battery < critical + 10) {
    profile = BD_LED_PROFILE_LOW_BATTERY;
  } else if (shell->led_ambient_known &&
             shell_app_ambient_enabled(shell, shell->led_app_id)) {
    profile = BD_LED_PROFILE_AMBIENT;
  }
  if (shell->led_profile_valid && shell->led_profile == profile) {
    if (profile != BD_LED_PROFILE_AMBIENT || !shell->led_ambient_dirty) {
      return;
    }
  }
  shell_led_apply(shell, profile, shell->led_ambient_rgb);
  shell->led_ambient_dirty = false;
}

static void shell_haptic_nav(bd_shell *shell) {
  if (shell == NULL || shell->haptics == NULL) {
    return;
  }
  if (!bd_config_get_bool(shell->settings, "input.haptics", true)) {
    return;
  }
  int strength = (int)bd_config_get_int(shell->settings, "input.haptic_strength",
                                        60);
  int duration = (int)bd_config_get_int(
      shell->settings, "input.haptic_duration_ms", 35);
  bd_haptics_set_strength(shell->haptics, strength);
  bd_haptics_play(shell->haptics, strength, duration, shell_now(shell));
}

static void shell_haptic_boot(bd_shell *shell) {
  if (shell == NULL || shell->haptics == NULL) {
    return;
  }
  if (!bd_config_get_bool(shell->settings, "input.haptics", true)) {
    return;
  }
  static const int strengths[] = {55, 0, 70};
  static const int durations[] = {45, 55, 70};
  bd_haptics_play_pattern(shell->haptics, strengths, durations, 3,
                          shell_now(shell));
}

/* --------------------------------------------------------------- JSON bits */

static bool json_token(const char *json, const char *key, const char **value) {
  size_t key_length = strlen(key);
  for (const char *p = json; (p = strchr(p, '"')) != NULL; ++p) {
    const char *start = p + 1;
    const char *end = start;
    while (*end != '\0') {
      if (*end == '\\' && end[1] != '\0') {
        end += 2;
        continue;
      }
      if (*end == '"') {
        break;
      }
      ++end;
    }
    if (*end != '"') {
      return false;
    }
    if ((size_t)(end - start) == key_length &&
        memcmp(start, key, key_length) == 0) {
      const char *colon = end + 1;
      while (*colon == ' ' || *colon == '\t') {
        ++colon;
      }
      if (*colon == ':') {
        ++colon;
        while (*colon == ' ' || *colon == '\t') {
          ++colon;
        }
        *value = colon;
        return true;
      }
    }
    p = end;
  }
  return false;
}

static bool json_string(const char *json, const char *key, char *out,
                        size_t out_size) {
  const char *value = NULL;
  if (!json_token(json, key, &value) || *value != '"' || out_size == 0) {
    return false;
  }
  ++value;
  size_t used = 0;
  while (*value != '\0' && *value != '"' && used + 1 < out_size) {
    if (*value == '\\' && value[1] != '\0') {
      ++value;
      switch (*value) {
        case 'n':
          out[used++] = '\n';
          break;
        case 't':
          out[used++] = '\t';
          break;
        case 'r':
          out[used++] = '\r';
          break;
        default:
          out[used++] = *value;
          break;
      }
      ++value;
      continue;
    }
    out[used++] = *value++;
  }
  out[used] = '\0';
  return *value == '"';
}

static bool json_bool(const char *json, const char *key, bool *out) {
  const char *value = NULL;
  if (!json_token(json, key, &value)) {
    return false;
  }
  if (strncmp(value, "true", 4) == 0) {
    *out = true;
    return true;
  }
  if (strncmp(value, "false", 5) == 0) {
    *out = false;
    return true;
  }
  return false;
}

static bool json_int(const char *json, const char *key, int *out) {
  const char *value = NULL;
  if (!json_token(json, key, &value) || out == NULL) {
    return false;
  }
  char *end = NULL;
  long number = strtol(value, &end, 10);
  if (end == value || number < -2147483647L || number > 2147483647L) {
    return false;
  }
  *out = (int)number;
  return true;
}

static bool json_u64(const char *json, const char *key, uint64_t *out) {
  const char *value = NULL;
  if (!json_token(json, key, &value) || out == NULL || value[0] == '-') {
    return false;
  }
  errno = 0;
  char *end = NULL;
  unsigned long long number = strtoull(value, &end, 10);
  if (end == value || errno == ERANGE) {
    return false;
  }
  if (*end != '\0' && *end != ',' && *end != '}' && *end != ' ' &&
      *end != '\t' && *end != '\n' && *end != '\r') {
    return false;
  }
  *out = (uint64_t)number;
  return true;
}

static bool parse_hex_color(const char *text, uint32_t *rgb) {
  if (text == NULL || rgb == NULL) {
    return false;
  }
  if (*text == '#') {
    ++text;
  }
  size_t length = strlen(text);
  if (length != 6) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (!((text[i] >= '0' && text[i] <= '9') ||
          (text[i] >= 'a' && text[i] <= 'f') ||
          (text[i] >= 'A' && text[i] <= 'F'))) {
      return false;
    }
  }
  *rgb = (uint32_t)strtoul(text, NULL, 16) & 0xFFFFFFu;
  return true;
}

static void shell_handle_app_notification(bd_shell *shell,
                                          const char *app_id,
                                          const char *method,
                                          const char *params) {
  if (shell == NULL || method == NULL) {
    return;
  }
  if (params == NULL) {
    params = "";
  }
  if (strcmp(method, "led.ambient") == 0 || strcmp(method, "led.set") == 0) {
    if (app_id != NULL && app_id[0] != '\0' && shell->led_app_id[0] == '\0') {
      copy_text(shell->led_app_id, sizeof(shell->led_app_id), app_id);
    }
    if (!shell_app_ambient_enabled(shell, app_id)) {
      return;
    }
    char color[16];
    int rgb_value = 0;
    bool off = false;
    uint32_t rgb = 0;
    if (json_string(params, "color", color, sizeof(color)) &&
        parse_hex_color(color, &rgb)) {
      shell->led_ambient_rgb = rgb;
    } else if (json_int(params, "rgb", &rgb_value) && rgb_value >= 0) {
      shell->led_ambient_rgb = (uint32_t)rgb_value & 0xFFFFFFu;
    } else if (json_bool(params, "off", &off) && off) {
      shell->led_ambient_known = false;
      shell->led_ambient_dirty = false;
      shell->led_profile_valid = false;
      shell_led_refresh_internal(shell);
      return;
    } else {
      return;
    }
    shell->led_ambient_known = true;
    shell->led_ambient_dirty = true;
    shell_led_refresh_internal(shell);
    return;
  }
  if (strcmp(method, "rumble.play") == 0) {
    int strength = 60;
    int duration = 60;
    json_int(params, "strength", &strength);
    json_int(params, "duration_ms", &duration);
    bd_haptics_play(shell->haptics, strength, duration, shell_now(shell));
    return;
  }
  if (strcmp(method, "rumble.stop") == 0) {
    bd_haptics_stop(shell->haptics);
  }
}

static void shell_led_ambient_sample(bd_shell *shell, uint64_t now) {
  if (shell == NULL || shell->led == NULL || shell->session == NULL) {
    return;
  }
  if (now < shell->led_ambient_next_ms) {
    return;
  }
  shell->led_ambient_next_ms = now + 500;
  if (!shell_app_ambient_enabled(shell, shell->led_app_id)) {
    return;
  }
  const char *path =
      bd_session_last_frame_path(shell->session, shell->led_app_id);
  if (path == NULL || path[0] == '\0') {
    return;
  }
  bd_app_last_frame frame;
  char error[128] = {0};
  if (!bd_app_last_frame_open(path, &frame, error, sizeof(error))) {
    return;
  }
  size_t size = bd_app_last_frame_pixel_size(
      frame.header.format, frame.header.width, frame.header.height);
  if (size == 0 || size > 4u * 1024u * 1024u) {
    bd_app_last_frame_close(&frame);
    return;
  }
  uint8_t *pixels = malloc(size);
  if (pixels == NULL) {
    bd_app_last_frame_close(&frame);
    return;
  }
  bd_app_last_frame_header header;
  bool copied = bd_app_last_frame_copy(&frame, pixels, size, &header, error,
                                       sizeof(error));
  bd_app_last_frame_close(&frame);
  if (!copied) {
    free(pixels);
    return;
  }
  uint64_t sum_r = 0;
  uint64_t sum_g = 0;
  uint64_t sum_b = 0;
  uint64_t count = 0;
  for (uint32_t y = 0; y < header.height; y += 8) {
    for (uint32_t x = 0; x < header.width; x += 8) {
      if (header.format == BD_APP_PIXEL_RGBA8888) {
        const uint8_t *pixel = pixels + (size_t)y * header.stride + (size_t)x * 4;
        sum_r += pixel[0];
        sum_g += pixel[1];
        sum_b += pixel[2];
        ++count;
      } else if (header.format == BD_APP_PIXEL_RGB565) {
        const uint8_t *row = pixels + (size_t)y * header.stride;
        uint16_t pixel = (uint16_t)(row[(size_t)x * 2] |
                                    (row[(size_t)x * 2 + 1] << 8));
        sum_r += (uint64_t)((pixel >> 11) & 0x1F) << 3;
        sum_g += (uint64_t)((pixel >> 5) & 0x3F) << 2;
        sum_b += (uint64_t)(pixel & 0x1F) << 3;
        ++count;
      }
    }
  }
  free(pixels);
  if (count == 0) {
    return;
  }
  uint32_t r = (uint32_t)(sum_r / count);
  uint32_t g = (uint32_t)(sum_g / count);
  uint32_t b = (uint32_t)(sum_b / count);
  shell->led_ambient_rgb = (r << 16) | (g << 8) | b;
  shell->led_ambient_known = true;
  shell->led_ambient_dirty = true;
  shell_led_refresh_internal(shell);
}

/* ----------------------------------------------------------- input source */

static void shell_dispatch_action(bd_shell *shell, const char *action,
                                  bool pressed);
static void shell_dispatch_key(bd_shell *shell, bd_key key, bool pressed,
                               uint64_t time_ms);
static bool shell_launch_app(bd_shell *shell, const char *app_id);
static void shell_refresh_running(bd_shell *shell);
static void shell_service(bd_shell *shell);
static void shell_close_settings(bd_shell *shell);
static void shell_power_service(bd_shell *shell);
static void shell_power_check_hold(bd_shell *shell);
static void shell_auto_sleep_check(bd_shell *shell, uint64_t now);
static void shell_battery_deadline_check(bd_shell *shell, uint64_t now);
static void shell_memory_budget_check(bd_shell *shell, uint64_t now);
static void shell_crash_retry_confirm(void *user, bool confirmed);
static void shell_toggle_favorite(bd_shell *shell);
static void shell_mark_activity(bd_shell *shell);
static bool shell_power_asleep(const bd_shell *shell);
static void shell_power_cancel_gesture(bd_shell *shell);

static void local_emit(void *user, const bd_input_output *event) {
  bd_shell *shell = (bd_shell *)user;
  if (event->kind == BD_INPUT_OUTPUT_KEY) {
    shell_dispatch_key(shell, event->key, event->pressed, event->time_ms);
    return;
  }
  if (event->kind == BD_INPUT_OUTPUT_ACTION && shell->hotkey_table != NULL &&
      event->binding >= 0 &&
      (size_t)event->binding < shell->hotkey_table->count) {
    shell_dispatch_action(shell,
                          shell->hotkey_table->items[event->binding].action,
                          event->pressed);
  }
}

static const char *preferred_input_socket(void) {
  const char *env = getenv("BRICKDESK_INPUT_SOCKET");
  if (env != NULL && env[0] != '\0') {
    return env;
  }
  if (access("/run/brickdesk/inputd.sock", F_OK) == 0) {
    return "/run/brickdesk/inputd.sock";
  }
  if (access("/tmp/brickdesk-inputd.sock", F_OK) == 0) {
    return "/tmp/brickdesk-inputd.sock";
  }
  return "/run/brickdesk/inputd.sock";
}

static void inputd_release_switch_state(bd_shell *shell) {
  /* A POWER gesture from the old source must not survive the switch. */
  shell_power_cancel_gesture(shell);
  bd_input_state_release_all(&shell->input, monotonic_ms(), local_emit, shell);
  bd_home_set_hint_visible(shell->home, false);
  if (shell->settings_page != NULL) {
    bd_settings_hold(shell->settings_page, BD_SETTINGS_ACTION_LEFT, false);
    bd_settings_hold(shell->settings_page, BD_SETTINGS_ACTION_RIGHT, false);
    bd_settings_set_hint_visible(shell->settings_page, false);
  }
  if (!shell_power_asleep(shell)) {
    shell->dirty = true;
  }
}

static void inputd_disconnect(bd_shell *shell, const char *reason) {
  if (shell == NULL) {
    return;
  }
  bool was_connected = shell->inputd_connected;
  if (shell->inputd_fd >= 0) {
    close(shell->inputd_fd);
    shell->inputd_fd = -1;
  }
  shell->inputd_connected = false;
  shell->inputd_connecting = false;
  shell->inputd_retry_ms = 0;
  shell->inputd_deadline_ms = 0;
  shell->inputd_sent = 0;
  shell->input_length = 0;
  if (!was_connected) {
    return;
  }
  if (shell->verbose) {
    fprintf(stderr, "brickdesk: inputd disconnected (%s); using SDL keys\n",
            reason != NULL ? reason : "eof");
  }
  /*
   * The daemon owns its pressed/hold state; this side owns the state fed by
   * SDL.  Release both views before the fallback becomes active so a hold or
   * repeat cannot survive the source switch, and a key held over SDL cannot
   * stay pressed when inputd returns.
   */
  inputd_release_switch_state(shell);
}

static void inputd_schedule_retry(bd_shell *shell, const char *reason) {
  uint64_t now = monotonic_ms();
  uint32_t delay = shell->inputd_backoff_ms;
  if (delay < BD_SHELL_INPUTD_BACKOFF_MIN_MS ||
      delay > BD_SHELL_INPUTD_BACKOFF_MAX_MS) {
    delay = BD_SHELL_INPUTD_BACKOFF_MIN_MS;
  }
  inputd_disconnect(shell, reason);
  shell->inputd_retry_ms = now + delay;
  shell->inputd_backoff_ms =
      delay > BD_SHELL_INPUTD_BACKOFF_MAX_MS / 2
          ? BD_SHELL_INPUTD_BACKOFF_MAX_MS
          : delay * 2;
  if (shell->verbose) {
    fprintf(stderr, "brickdesk: inputd retry in %u ms (%s)\n", delay,
            reason != NULL ? reason : "disconnected");
  }
}

/*
 * Called once the subscription is fully queued on the socket.  SDL state is
 * released before the daemon becomes authoritative; the reverse transition in
 * inputd_disconnect() does the same before SDL resumes.  The daemon is not
 * asked to release its devices: other clients may be holding keys or a menu.
 */
static void inputd_on_connected(bd_shell *shell) {
  if (shell->inputd_connected) {
    return;
  }
  inputd_release_switch_state(shell);
  shell->inputd_connected = true;
  shell->inputd_connecting = false;
  shell->inputd_retry_ms = 0;
  shell->inputd_deadline_ms = 0;
  shell->inputd_backoff_ms = BD_SHELL_INPUTD_BACKOFF_MIN_MS;
  shell->input_length = 0;
  if (!shell_power_asleep(shell)) {
    shell->dirty = true;
  }
  if (shell->verbose) {
    fprintf(stderr, "brickdesk: inputd connected at %s\n",
            shell->input_socket);
  }
}

static void inputd_flush_subscribe(bd_shell *shell) {
  static const size_t length = sizeof(BD_SHELL_INPUTD_SUBSCRIBE) - 1;
  while (shell->inputd_sent < length) {
    ssize_t amount =
        send(shell->inputd_fd, BD_SHELL_INPUTD_SUBSCRIBE + shell->inputd_sent,
             length - shell->inputd_sent, MSG_NOSIGNAL);
    if (amount > 0) {
      shell->inputd_sent += (size_t)amount;
      continue;
    }
    if (amount < 0 && errno == EINTR) {
      continue;
    }
    if (amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    inputd_schedule_retry(shell, amount < 0 ? strerror(errno) : "short write");
    return;
  }
  inputd_on_connected(shell);
}

static void inputd_finish_connect(bd_shell *shell, uint64_t now) {
  int error = 0;
  socklen_t length = sizeof(error);
  if (getsockopt(shell->inputd_fd, SOL_SOCKET, SO_ERROR, &error, &length) !=
      0) {
    inputd_schedule_retry(shell, strerror(errno));
    return;
  }
  if (error != 0) {
    errno = error;
    inputd_schedule_retry(shell, strerror(error));
    return;
  }
  shell->inputd_connecting = false;
  shell->inputd_sent = 0;
  shell->inputd_deadline_ms = now + BD_SHELL_INPUTD_CONNECT_TIMEOUT_MS;
  inputd_flush_subscribe(shell);
}

static void inputd_start_connect(bd_shell *shell, uint64_t now) {
  if (shell->input_socket[0] == '\0') {
    return;
  }
  shell->inputd_retry_ms = 0;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    inputd_schedule_retry(shell, strerror(errno));
    return;
  }
  int fd_flags = fcntl(fd, F_GETFD, 0);
  int flags = fcntl(fd, F_GETFL, 0);
  if (fd_flags < 0 || flags < 0 ||
      fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) != 0 ||
      fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    int error = errno;
    close(fd);
    errno = error;
    inputd_schedule_retry(shell, strerror(error));
    return;
  }
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  size_t path_length = strlen(shell->input_socket);
  if (path_length >= sizeof(address.sun_path)) {
    close(fd);
    inputd_schedule_retry(shell, "socket path too long");
    return;
  }
  memcpy(address.sun_path, shell->input_socket, path_length + 1);
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
    shell->inputd_fd = fd;
    shell->inputd_connecting = false;
    shell->inputd_sent = 0;
    shell->inputd_deadline_ms = now + BD_SHELL_INPUTD_CONNECT_TIMEOUT_MS;
    inputd_flush_subscribe(shell);
    return;
  }
  int error = errno;
  if (error == EINPROGRESS) {
    shell->inputd_fd = fd;
    shell->inputd_connecting = true;
    shell->inputd_deadline_ms = now + BD_SHELL_INPUTD_CONNECT_TIMEOUT_MS;
    return;
  }
  close(fd);
  errno = error;
  inputd_schedule_retry(shell, strerror(error));
}

static void inputd_service(bd_shell *shell, uint64_t now) {
  if (shell == NULL || shell->input_socket[0] == '\0') {
    return;
  }
  if (shell->inputd_fd < 0) {
    if (shell->inputd_retry_ms == 0 || now >= shell->inputd_retry_ms) {
      inputd_start_connect(shell, now);
    }
    return;
  }
  if (shell->inputd_connecting) {
    if (now >= shell->inputd_deadline_ms) {
      inputd_schedule_retry(shell, "connect timeout");
    }
    return;
  }
  const size_t length = sizeof(BD_SHELL_INPUTD_SUBSCRIBE) - 1;
  if (shell->inputd_sent < length) {
    if (now >= shell->inputd_deadline_ms) {
      inputd_schedule_retry(shell, "subscribe timeout");
    } else {
      inputd_flush_subscribe(shell);
    }
  }
}

static void inputd_handle_line(bd_shell *shell, const char *line) {
  char type[32];
  if (!json_string(line, "type", type, sizeof(type))) {
    return;
  }
  if (strcmp(type, "action") == 0) {
    char action[BD_HOTKEY_ACTION_MAX];
    char phase[16];
    if (!json_string(line, "action", action, sizeof(action))) {
      return;
    }
    bool pressed = true;
    if (json_string(line, "phase", phase, sizeof(phase))) {
      pressed = strcmp(phase, "up") != 0;
    }
    shell_dispatch_action(shell, action, pressed);
    return;
  }
  if (strcmp(type, "key") == 0) {
    char key_name[32];
    bool pressed = false;
    if (!json_string(line, "key", key_name, sizeof(key_name)) ||
        !json_bool(line, "pressed", &pressed)) {
      return;
    }
    bd_key key = bd_key_from_name(key_name);
    if (key != BD_KEY_UNKNOWN) {
      if (key != BD_KEY_POWER &&
          (shell_power_asleep(shell) || shell->power_input_suspended)) {
        return;
      }
      uint64_t event_ms = 0;
      if (!json_u64(line, "time_ms", &event_ms)) {
        /*
         * Compatibility for older clients/fixtures: production inputd emits
         * time_ms.  Without one, consume at the current monotonic time rather
         * than inventing a duration.
         */
        event_ms = monotonic_ms();
      }
      shell_dispatch_key(shell, key, pressed, event_ms);
    }
    return;
  }
  if (strcmp(type, "switch") == 0) {
    char name[32];
    int value = -1;
    if (!json_string(line, "switch", name, sizeof(name)) ||
        !json_int(line, "value", &value)) {
      return;
    }
    if (strcmp(name, "headphone") != 0 || (value != 0 && value != 1)) {
      return;
    }
    if (shell_power_asleep(shell) || shell->power_input_suspended) {
      return;
    }
    if (!shell->av_audio_ready) {
      if (shell->verbose) {
        fprintf(stderr, "brickdesk: headphone switch ignored: audio unavailable\n");
      }
      return;
    }
    bd_av_error error;
    bd_av_result result =
        bd_av_audio_set_jack(&shell->av_audio, value == 1, &error);
    if (result != BD_AV_OK && shell->verbose) {
      fprintf(stderr, "brickdesk: headphone switch: %s\n",
              error.message[0] != '\0' ? error.message
                                       : bd_av_result_name(result));
    }
    return;
  }
  if (strcmp(type, "release_all") == 0) {
    shell_power_cancel_gesture(shell);
    bd_home_set_hint_visible(shell->home, false);
    if (shell->settings_page != NULL) {
      bd_settings_hold(shell->settings_page, BD_SETTINGS_ACTION_LEFT, false);
      bd_settings_hold(shell->settings_page, BD_SETTINGS_ACTION_RIGHT, false);
      bd_settings_set_hint_visible(shell->settings_page, false);
    }
    if (!shell_power_asleep(shell)) {
      shell->dirty = true;
    }
  }
}

static void inputd_read(bd_shell *shell) {
  for (;;) {
    char buffer[512];
    ssize_t amount = recv(shell->inputd_fd, buffer, sizeof(buffer), 0);
    if (amount < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      inputd_schedule_retry(shell, strerror(errno));
      return;
    }
    if (amount == 0) {
      inputd_schedule_retry(shell, "closed");
      return;
    }
    if (shell->input_length + (size_t)amount >= sizeof(shell->input_line)) {
      inputd_schedule_retry(shell, "line too long");
      return;
    }
    memcpy(shell->input_line + shell->input_length, buffer, (size_t)amount);
    shell->input_length += (size_t)amount;
    shell->input_line[shell->input_length] = '\0';

    char *newline = memchr(shell->input_line, '\n', shell->input_length);
    while (newline != NULL) {
      size_t length = (size_t)(newline - shell->input_line);
      if (length > 0 && shell->input_line[length - 1] == '\r') {
        --length;
      }
      shell->input_line[length] = '\0';
      inputd_handle_line(shell, shell->input_line);
      size_t consumed = (size_t)(newline - shell->input_line) + 1;
      memmove(shell->input_line, shell->input_line + consumed,
              shell->input_length - consumed);
      shell->input_length -= consumed;
      shell->input_line[shell->input_length] = '\0';
      newline = memchr(shell->input_line, '\n', shell->input_length);
    }
  }
}

static void inputd_handle_poll(bd_shell *shell, short revents) {
  if (shell->inputd_fd < 0) {
    return;
  }
  uint64_t now = monotonic_ms();
  if (revents & POLLNVAL) {
    inputd_schedule_retry(shell, "invalid descriptor");
    return;
  }
  if (shell->inputd_connecting) {
    if ((revents & (POLLIN | POLLOUT | POLLERR | POLLHUP)) != 0) {
      inputd_finish_connect(shell, now);
    }
    if (shell->inputd_fd < 0 || shell->inputd_connecting) {
      return;
    }
  }
  if ((revents & POLLOUT) != 0) {
    inputd_flush_subscribe(shell);
    if (shell->inputd_fd < 0) {
      return;
    }
  }
  if ((revents & POLLIN) != 0) {
    inputd_read(shell);
    if (shell->inputd_fd < 0) {
      return;
    }
  }
  if ((revents & (POLLERR | POLLHUP)) != 0) {
    inputd_schedule_retry(shell, "hangup");
  }
}

/* --------------------------------------------------------------- menu env */

static bool app_env_set(bd_app *app, const char *key, const char *value,
                        bool prepend) {
  for (size_t i = 0; i < app->env_count; ++i) {
    if (strcmp(app->env_keys[i], key) != 0) {
      continue;
    }
    char *next = NULL;
    if (prepend && strstr(app->env_values[i], value) != NULL) {
      return true;
    }
    if (prepend && app->env_values[i][0] != '\0') {
      size_t size = strlen(value) + 1 + strlen(app->env_values[i]) + 1;
      next = (char *)malloc(size);
      if (next != NULL) {
        snprintf(next, size, "%s %s", value, app->env_values[i]);
      }
    } else {
      next = strdup(value);
    }
    if (next == NULL) {
      return false;
    }
    free(app->env_values[i]);
    app->env_values[i] = next;
    return true;
  }
  if (app->env_count >= 40) {
    return false;
  }
  size_t count = app->env_count + 1;
  char *key_copy = strdup(key);
  char *value_copy = strdup(value);
  char **keys = (char **)malloc(count * sizeof(char *));
  char **values = (char **)malloc(count * sizeof(char *));
  if (key_copy == NULL || value_copy == NULL || keys == NULL ||
      values == NULL) {
    free(key_copy);
    free(value_copy);
    free(keys);
    free(values);
    return false;
  }
  for (size_t i = 0; i < app->env_count; ++i) {
    keys[i] = app->env_keys[i];
    values[i] = app->env_values[i];
  }
  keys[app->env_count] = key_copy;
  values[app->env_count] = value_copy;
  free(app->env_keys);
  free(app->env_values);
  app->env_keys = keys;
  app->env_values = values;
  app->env_count = count;
  return true;
}

static bool file_readable(const char *path) {
  return path != NULL && path[0] != '\0' && access(path, R_OK) == 0;
}

static const char *menu_shim_path(bd_shell *shell, const char *configured) {
  if (shell->menu_shim_checked) {
    return shell->menu_shim[0] != '\0' ? shell->menu_shim : NULL;
  }
  shell->menu_shim_checked = true;
  shell->menu_shim[0] = '\0';

  if (file_readable(configured)) {
    copy_text(shell->menu_shim, sizeof(shell->menu_shim), configured);
    return shell->menu_shim;
  }
  const char *env = getenv("BRICKDESK_MENU_SHIM");
  if (file_readable(env)) {
    copy_text(shell->menu_shim, sizeof(shell->menu_shim), env);
    return shell->menu_shim;
  }

  char exe[BD_SHELL_PATH_MAX];
  ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (length > 0) {
    exe[length] = '\0';
    char *slash = strrchr(exe, '/');
    if (slash != NULL) {
      *slash = '\0';
      char candidate[BD_SHELL_PATH_MAX * 2];
      snprintf(candidate, sizeof(candidate), "%s/libbrickdesk-menu.so", exe);
      if (file_readable(candidate)) {
        copy_text(shell->menu_shim, sizeof(shell->menu_shim), candidate);
        return shell->menu_shim;
      }
      snprintf(candidate, sizeof(candidate), "%s/../lib/libbrickdesk-menu.so",
               exe);
      if (file_readable(candidate)) {
        copy_text(shell->menu_shim, sizeof(shell->menu_shim), candidate);
        return shell->menu_shim;
      }
    }
  }

  static const char *const candidates[] = {
      "/usr/lib/libbrickdesk-menu.so",
      "/usr/local/lib/libbrickdesk-menu.so",
      "/mnt/SDCARD/brickdesk/lib/libbrickdesk-menu.so",
  };
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
    if (file_readable(candidates[i])) {
      copy_text(shell->menu_shim, sizeof(shell->menu_shim), candidates[i]);
      return shell->menu_shim;
    }
  }
  return NULL;
}

static bool apply_menu_env(bd_shell *shell, bd_app *app,
                           const char *configured_shim) {
  if (app->menu.mode != BD_APP_MENU_OVERLAY) {
    return true;
  }
  const char *shim = menu_shim_path(shell, configured_shim);
  if (shim == NULL) {
    if (!shell->menu_shim_warned) {
      shell->menu_shim_warned = true;
      bd_shell_notice(shell, BD_NOTICE_WARNING,
                      "菜单叠加组件缺失，应用将全屏启动", 0);
    }
    if (shell->verbose) {
      fprintf(stderr,
              "brickdesk: menu shim not found; launching %s without overlay\n",
              app->id);
    }
    return true;
  }
  if (!app_env_set(app, "LD_PRELOAD", shim, true)) {
    return false;
  }
  if (shell->input_socket[0] != '\0') {
    app_env_set(app, "BRICKDESK_INPUT_SOCKET", shell->input_socket, false);
  }
  const char *perf_socket = getenv("BRICKDESK_PERF_SOCKET");
  if (perf_socket != NULL && perf_socket[0] != '\0') {
    app_env_set(app, "BRICKDESK_PERF_SOCKET", perf_socket, false);
  }
  const char *asset_dir = getenv("BRICKDESK_ASSET_DIR");
  if (asset_dir != NULL && asset_dir[0] != '\0') {
    app_env_set(app, "BRICKDESK_ASSET_DIR", asset_dir, false);
  }
  if (shell->settings != NULL) {
    const char *config_dir = bd_config_dir(shell->settings);
    if (config_dir != NULL && config_dir[0] != '\0') {
      app_env_set(app, "BRICKDESK_CONFIG_DIR", config_dir, false);
    }
  }
  return true;
}

/* ----------------------------------------------------------- session glue */

static void on_session_event(void *user, const bd_session_event *event) {
  bd_shell *shell = (bd_shell *)user;
  if (shell->pending_count >= BD_SHELL_PENDING_MAX) {
    shell->rescan = true;
    return;
  }
  shell_pending *pending = &shell->pending[shell->pending_count++];
  memset(pending, 0, sizeof(*pending));
  switch (event->kind) {
    case BD_SESSION_EVENT_STATE:
      pending->kind = SHELL_PENDING_STATE;
      pending->state = event->state;
      break;
    case BD_SESSION_EVENT_EXIT:
    case BD_SESSION_EVENT_CRASH:
      pending->kind = SHELL_PENDING_EXIT;
      pending->state = event->state;
      break;
    case BD_SESSION_EVENT_MENU_ACTION:
      pending->kind = SHELL_PENDING_MENU_ACTION;
      break;
    case BD_SESSION_EVENT_NEEDS_ATTENTION:
      pending->kind = SHELL_PENDING_ATTENTION;
      break;
    case BD_SESSION_EVENT_HOOK:
      if (!event->hook_ok) {
        char text[192];
        snprintf(text, sizeof(text), "应用 %s 的 %s 钩子未完成",
                 event->app_id, event->hook != NULL ? event->hook : "unknown");
        bd_shell_notice(shell, BD_NOTICE_WARNING, text, 0);
      }
      return;
    case BD_SESSION_EVENT_PROTOCOL_ERROR:
      bd_shell_notice(shell, BD_NOTICE_WARNING,
                      "应用协议错误，已忽略该消息", 0);
      return;
    case BD_SESSION_EVENT_NOTIFICATION:
      pending->kind = SHELL_PENDING_NOTIFICATION;
      break;
    default:
      return;
  }
  copy_text(pending->app_id, sizeof(pending->app_id), event->app_id);
  copy_text(pending->method, sizeof(pending->method), event->method);
  copy_text(pending->params, sizeof(pending->params), event->params);
}

static bool session_active_state(bd_session_state state) {
  switch (state) {
    case BD_SESSION_LAUNCHING:
    case BD_SESSION_FOREGROUND:
    case BD_SESSION_SUSPENDING:
    case BD_SESSION_RESUMING:
    case BD_SESSION_TERMINATING:
      return true;
    case BD_SESSION_IDLE:
    case BD_SESSION_BACKGROUND:
    case BD_SESSION_EXITED:
    case BD_SESSION_CRASHED:
    default:
      return false;
  }
}

static bool session_running_state(bd_session_state state) {
  switch (state) {
    case BD_SESSION_LAUNCHING:
    case BD_SESSION_FOREGROUND:
    case BD_SESSION_SUSPENDING:
    case BD_SESSION_RESUMING:
    case BD_SESSION_TERMINATING:
    case BD_SESSION_BACKGROUND:
      return true;
    default:
      return false;
  }
}

static int shell_test_memory_fixture(void) {
  /*
   * The screenshot harness sets this to keep memory text deterministic.
   * Production never sets it, so the shell uses the real /proc sample.
   */
  const char *value = getenv("BRICKDESK_TEST_MEMORY_MB");
  if (value == NULL || value[0] == '\0') {
    return -1;
  }
  char *end = NULL;
  long mb = strtol(value, &end, 10);
  if (end == value || *end != '\0' || mb < 0 || mb > 4096) {
    return -1;
  }
  return (int)mb;
}

static int shell_runtime_memory(shell_app_runtime *runtime,
                                const bd_session_app_info *info, uint64_t now,
                                bool panel_open) {
  if (runtime == NULL || info == NULL) {
    return -1;
  }
  int fixture = shell_test_memory_fixture();
  if (fixture >= 0) {
    runtime->memory_mb = fixture;
    runtime->memory_sampled_ms = now;
    return fixture;
  }
  uint64_t interval =
      runtime->memory_mb < 0 ? BD_SHELL_MEMORY_RETRY_MS : BD_SHELL_MEMORY_MS;
  bool due = runtime->memory_sampled_ms == 0 ||
             (panel_open && now - runtime->memory_sampled_ms >= interval);
  if (!due) {
    return runtime->memory_mb;
  }
  runtime->memory_mb = bd_sys_process_tree_rss_mb(info->pid);
  runtime->memory_sampled_ms = now;
  return runtime->memory_mb;
}

/*
 * Hides the home once the launch card has reached full screen, or starts a
 * requested return as soon as the launch transition allows it.
 */
static void shell_update_home(bd_shell *shell, uint64_t now) {
  if (shell->home == NULL) {
    return;
  }
  bool settings_open = shell->settings_page != NULL &&
                       bd_settings_is_open(shell->settings_page);
  bd_home_set_surface_visible(shell->home, shell->visible && !settings_open);
  uint64_t wake_at = 0;
  bool was_active = bd_home_needs_frame_until(shell->home, &wake_at);
  bd_home_update(shell->home, now);
  if (was_active || bd_home_needs_frame_until(shell->home, &wake_at)) {
    /* Also guarantees the final frame after an animation deadline. */
    shell->dirty = true;
  }

  if (shell->return_app[0] != '\0') {
    if (bd_home_launch_target(shell->home) != NULL &&
        !bd_home_launch_complete(shell->home)) {
      return;
    }
    if (!bd_home_start_return(shell->home, shell->return_app)) {
      bd_home_damage(shell->home);
    }
    shell->return_app[0] = '\0';
    shell->visible = true;
    shell->dirty = true;
    return;
  }

  if (shell->visible && bd_home_launch_complete(shell->home)) {
    shell->visible = false;
    shell->dirty = false;
    bd_home_set_hint_visible(shell->home, false);
  }
}

static void shell_request_return(bd_shell *shell, const char *app_id) {
  if (shell->home == NULL || app_id == NULL || app_id[0] == '\0') {
    return;
  }
  shell_close_settings(shell);
  copy_text(shell->return_app, sizeof(shell->return_app), app_id);
  shell_update_home(shell, shell_now(shell));
}

static void shell_refresh_running(bd_shell *shell) {
  if (shell->home == NULL) {
    return;
  }
  bool panel_open =
      bd_home_quick_open(shell->home) || bd_home_dialog_open(shell->home);
  uint64_t now = monotonic_ms();
  size_t app_count = shell->apps != NULL ? bd_app_list_count(shell->apps) : 0;
  for (size_t i = 0; i < app_count; ++i) {
    const bd_app *app = bd_app_list_at(shell->apps, i);
    if (app == NULL || i >= shell->runtime_count) {
      continue;
    }
    shell_app_runtime *runtime = &shell->runtime[i];
    bd_session_app_info info;
    bool found = shell->session != NULL &&
                 bd_session_find(shell->session, app->id, &info);
    bool background = found && info.state == BD_SESSION_BACKGROUND;
    if (background) {
      const char *frame = bd_session_last_frame_path(shell->session, app->id);
      const char *path = frame != NULL ? frame : "";
      bool changed = !runtime->running || !runtime->background ||
                     strcmp(runtime->frame_path, path) != 0;
      int memory = shell_runtime_memory(runtime, &info, now, panel_open);
      if (changed) {
        if (bd_home_set_running(shell->home, app->id, frame,
                                info.stopped_by_session, memory)) {
          runtime->running = true;
          copy_text(runtime->frame_path, sizeof(runtime->frame_path), path);
          shell->dirty = true;
        }
      } else {
        bd_home_set_running_memory(shell->home, app->id, memory);
      }
      runtime->background = true;
      continue;
    }
    runtime->background = false;
    if (found && !session_running_state(info.state)) {
      if (runtime->running) {
        bd_home_clear_running(shell->home, app->id);
        runtime->running = false;
        runtime->frame_path[0] = '\0';
        shell->dirty = true;
      }
    }
  }
  if (shell->session != NULL) {
    bd_session_clear_terminated(shell->session);
  }
}

/*
 * The "background full" dialog ends the LRU process first; retry the launch
 * once the session has actually dropped that instance.
 */
static void shell_try_pending_launch(bd_shell *shell) {
  if (shell->pending_launch[0] == '\0' || shell->evict_failed) {
    return;
  }
  if (shell->evict_app[0] != '\0') {
    bd_session_app_info info;
    if (shell->session != NULL &&
        bd_session_find(shell->session, shell->evict_app, &info) &&
        session_running_state(info.state)) {
      return;
    }
    shell->evict_app[0] = '\0';
  }
  char app_id[BD_APP_ID_MAX];
  copy_text(app_id, sizeof(app_id), shell->pending_launch);
  shell->pending_launch[0] = '\0';
  if (!shell_launch_app(shell, app_id) && shell->verbose) {
    fprintf(stderr, "brickdesk: retry launch %s failed\n", app_id);
  }
}

static void shell_process_pending(bd_shell *shell) {
  while (shell->pending_count > 0) {
    shell_pending batch[BD_SHELL_PENDING_MAX];
    int count = shell->pending_count;
    memcpy(batch, shell->pending, (size_t)count * sizeof(batch[0]));
    shell->pending_count = 0;
    for (int i = 0; i < count; ++i) {
      shell_pending *pending = &batch[i];
      switch (pending->kind) {
        case SHELL_PENDING_STATE:
          if (pending->state == BD_SESSION_FOREGROUND) {
            shell_display_apply(shell);
            copy_text(shell->led_app_id, sizeof(shell->led_app_id),
                      pending->app_id);
            shell->led_profile_valid = false;
            shell->led_ambient_next_ms = 0;
            shell_led_refresh_internal(shell);
          }
          if (pending->state == BD_SESSION_BACKGROUND) {
            shell_refresh_running(shell);
            shell_request_return(shell, pending->app_id);
          }
          break;
        case SHELL_PENDING_EXIT: {
          shell_display_apply(shell);
          if (strcmp(shell->led_app_id, pending->app_id) == 0) {
            shell->led_app_id[0] = '\0';
            shell->led_ambient_known = false;
            shell->led_ambient_dirty = false;
            shell->led_profile_valid = false;
            shell_led_refresh_internal(shell);
          }
          shell_refresh_running(shell);
          const char *launching = bd_home_launch_target(shell->home);
          bool launch_owner = launching != NULL &&
                              strcmp(launching, pending->app_id) == 0;
          if (!shell->visible || launch_owner) {
            shell_request_return(shell, pending->app_id);
          }
          if (pending->state == BD_SESSION_CRASHED && !shell->crash_prompt &&
              shell->home != NULL && !bd_home_dialog_open(shell->home)) {
            copy_text(shell->crash_retry_app, sizeof(shell->crash_retry_app),
                      pending->app_id);
            char text[192];
            snprintf(text, sizeof(text), "应用 %s 异常退出，是否重新启动？",
                     pending->app_id);
            shell->crash_prompt = true;
            if (!bd_shell_confirm(shell, "应用异常退出", text, 0,
                                  shell_crash_retry_confirm, shell)) {
              shell->crash_prompt = false;
              shell->crash_retry_app[0] = '\0';
            }
          }
          shell->dirty = true;
          break;
        }
        case SHELL_PENDING_MENU_ACTION:
          if (shell->session == NULL) {
            break;
          }
          if (strcmp(pending->method, "background") == 0) {
            bd_session_result result =
                bd_session_background(shell->session, pending->app_id);
            if (shell->verbose) {
              fprintf(stderr, "brickdesk: menu background %s -> %s\n",
                      pending->app_id, bd_session_result_name(result));
            }
          } else if (strcmp(pending->method, "quit") == 0) {
            bd_session_result result =
                bd_session_terminate(shell->session, pending->app_id);
            if (shell->verbose) {
              fprintf(stderr, "brickdesk: menu quit %s -> %s\n",
                      pending->app_id, bd_session_result_name(result));
            }
          }
          shell->dirty = true;
          break;
        case SHELL_PENDING_NOTIFICATION:
          shell_handle_app_notification(shell, pending->app_id,
                                        pending->method, pending->params);
          break;
        case SHELL_PENDING_ATTENTION:
          if (shell->verbose) {
            fprintf(stderr, "brickdesk: background limit reached\n");
          }
          break;
        default:
          break;
      }
    }
  }
  if (shell->rescan) {
    shell->rescan = false;
  }
  shell_refresh_running(shell);
  shell_try_pending_launch(shell);
}

static void shell_refresh_status(bd_shell *shell, bool force) {
  if (shell == NULL || shell->home == NULL) {
    return;
  }
  uint64_t now = monotonic_ms();
  if (!force && now - shell->status_sampled_ms < BD_SHELL_STATUS_MS) {
    return;
  }
  shell->status_sampled_ms = now;
  bd_wifi_state wifi = bd_sys_wifi_state(NULL);
  bool known = wifi != BD_WIFI_UNKNOWN;
  bool connected = wifi == BD_WIFI_CONNECTED;
  if (!force && known == shell->status_wifi_known &&
      connected == shell->status_wifi) {
    return;
  }
  shell->status_wifi_known = known;
  shell->status_wifi = connected;
  shell_publish_status(shell, true);
}

static void shell_service(bd_shell *shell) {
  uint64_t now = shell_now(shell);
  bool asleep = shell_power_asleep(shell);
  bd_haptics_update(shell->haptics, now);
  inputd_service(shell, monotonic_ms());
  shell_battery_deadline_check(shell, now);
  shell_auto_sleep_check(shell, now);
  shell_memory_budget_check(shell, now);
  if (!asleep) {
    shell_update_home(shell, now);
    shell_refresh_status(shell, false);
    shell_battery_sample(shell, now);
    shell_led_ambient_sample(shell, now);
    shell_display_dp_poll(shell, now);
    if (shell->notice != NULL && shell->visible &&
        bd_notice_update(shell->notice, now)) {
      shell->dirty = true;
    }
    if (shell->settings_page != NULL &&
        bd_settings_is_open(shell->settings_page)) {
      bd_settings_update(shell->settings_page, now);
      uint64_t wake_at = 0;
      if (bd_settings_needs_frame_until(shell->settings_page, &wake_at)) {
        shell->dirty = true;
      }
    }
    if (!shell->inputd_connected) {
      bd_input_state_advance(&shell->input, monotonic_ms(), local_emit, shell);
    }
  }
  shell_led_refresh_internal(shell);
  shell->session_next_ms = -1;
  if (shell->session != NULL) {
    int next = bd_session_poll(shell->session);
    if (next >= 0) {
      shell->session_next_ms = next;
    }
  }
  if (shell->plugins != NULL) {
    if (bd_plugin_host_poll(shell->plugins, 0) < 0) {
      if (!shell->plugin_error_notified) {
        shell->plugin_error_notified = true;
        bd_shell_notice(shell, BD_NOTICE_WARNING, "插件服务异常，已跳过本轮", 0);
      }
      if (shell->verbose) {
        fprintf(stderr, "brickdesk: plugin poll failed\n");
      }
    } else {
      shell->plugin_error_notified = false;
      if (!asleep && bd_plugin_host_take_dirty(shell->plugins)) {
        /*
         * Leave the host's dirty flag queued while asleep: waking consumes it
         * and forces one frame without rendering or consuming tile deadlines.
         */
        shell->dirty = true;
      }
    }
  }
  /*
   * Session events stay queued while asleep.  They are UI actions (return to
   * home, refresh running tiles), not IPC service; poll() above still reaps
   * exits and serves the app sockets.
   */
  if (asleep) {
    return;
  }
  shell_process_pending(shell);
  shell_update_home(shell, shell_now(shell));
}

/* ---------------------------------------------------------- power path */

static bool shell_power_asleep(const bd_shell *shell) {
  if (shell == NULL || shell->power == NULL) {
    return false;
  }
  bd_power_state state = bd_power_current_state(shell->power);
  return state == BD_POWER_STATE_LIGHT_SLEEP ||
         state == BD_POWER_STATE_DEGRADED;
}

/*
 * Drops an incomplete POWER gesture when the authoritative input source
 * changes or inputd resets all keys.  It never injects a release or a
 * sleep/wake request: the new source must start a fresh gesture.
 */
static void shell_power_cancel_gesture(bd_shell *shell) {
  if (shell == NULL) {
    return;
  }
  bool was_down = shell->power_key_down;
  shell->power_key_down = false;
  shell->power_down_ms = 0;
  if (was_down && shell->power_request == SHELL_POWER_REQUEST_WAKE) {
    /* A wake request derived from an un-released press is stale too. */
    shell->power_request = SHELL_POWER_REQUEST_NONE;
  }
}

static int shell_power_input_release(void *user) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL) {
    return -1;
  }
  shell->power_input_suspended = true;
  return 0;
}

static int shell_power_input_acquire(void *user) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL) {
    return -1;
  }
  shell->power_input_suspended = false;
  return 0;
}

/*
 * Freezes only the foreground instance captured before the transition.  The
 * session keeps it FOREGROUND, so the same id/pid is used for thaw even after
 * a failed restore leaves the machine DEGRADED.
 */
static int shell_power_app_pause(void *user) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL || shell->session == NULL) {
    return -1;
  }
  if (shell->power_app_id[0] == '\0') {
    return 0;
  }
  bd_session_result result =
      bd_session_freeze(shell->session, shell->power_app_id);
  if (result == BD_SESSION_OK) {
    shell->power_app_frozen = true;
    return 0;
  }
  if (result == BD_SESSION_NOT_FOUND) {
    /* The captured instance exited before the hook ran; nothing to stop. */
    shell->power_app_id[0] = '\0';
    shell->power_app_frozen = false;
    return 0;
  }
  /*
   * A failed freeze can still own a stop when its rollback SIGCONT was
   * rejected.  Keep the exact instance so the rollback/wake path retries thaw.
   */
  bd_session_app_info info;
  if (bd_session_find(shell->session, shell->power_app_id, &info) &&
      info.frozen) {
    shell->power_app_frozen = true;
    fprintf(stderr,
            "brickdesk: light sleep freeze %s -> %s; thaw retry required\n",
            shell->power_app_id, bd_session_result_name(result));
    return 1;
  }
  if (shell->verbose) {
    fprintf(stderr, "brickdesk: light sleep freeze %s -> %s\n",
            shell->power_app_id, bd_session_result_name(result));
  }
  return 1;
}

static int shell_power_app_resume(void *user) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL || shell->session == NULL) {
    return -1;
  }
  if (shell->power_app_id[0] == '\0') {
    shell->power_app_frozen = false;
    return 0;
  }
  bd_session_app_info info;
  if (!bd_session_find(shell->session, shell->power_app_id, &info) ||
      !info.frozen) {
    shell->power_app_frozen = false;
    shell->power_app_id[0] = '\0';
    return 0;
  }
  shell->power_app_frozen = true;
  if (info.state != BD_SESSION_FOREGROUND) {
    /* Another lifecycle action owns the transition; do not signal blind. */
    fprintf(stderr, "brickdesk: light sleep thaw %s deferred in state %s\n",
            shell->power_app_id, bd_session_state_name(info.state));
    return 1;
  }
  bd_session_result result =
      bd_session_thaw(shell->session, shell->power_app_id);
  if (result == BD_SESSION_OK) {
    shell->power_app_frozen = false;
    shell->power_app_id[0] = '\0';
    return 0;
  }
  /*
   * IO_ERROR/BUSY is not "resumed".  Ownership is cleared only when the
   * session confirms the exact instance is no longer frozen or has exited.
   */
  if (!bd_session_find(shell->session, shell->power_app_id, &info) ||
      !info.frozen || info.exited) {
    shell->power_app_frozen = false;
    shell->power_app_id[0] = '\0';
    return 0;
  }
  fprintf(stderr, "brickdesk: light sleep thaw %s -> %s (retry on wake)\n",
          shell->power_app_id, bd_session_result_name(result));
  return 1;
}

/*
 * Brightness is the only display state this slice touches.  An unreadable
 * value fails the sleep instead of writing a blind zero; a failed write or
 * restore keeps `power_brightness_known` set so bd_power wake can retry it.
 */
static int shell_power_display_off(void *user) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL || shell->system.get_brightness == NULL ||
      shell->system.set_brightness == NULL) {
    if (shell != NULL && shell->verbose) {
      fprintf(stderr, "brickdesk: light sleep: brightness unavailable\n");
    }
    return 1;
  }
  int percent = 0;
  if (!shell->system.get_brightness(shell->system.user, &percent) ||
      percent < 0 || percent > 100) {
    if (shell->verbose) {
      fprintf(stderr, "brickdesk: light sleep: brightness read failed; "
                      "not writing\n");
    }
    return 1;
  }
  shell->power_brightness = percent;
  shell->power_brightness_known = true;
  if (shell->system.set_brightness(shell->system.user, 0)) {
    return 0;
  }
  /* A failed set may still have changed the hardware; put it back now. */
  if (shell->system.set_brightness(shell->system.user,
                                   shell->power_brightness)) {
    shell->power_brightness_known = false;
  }
  if (shell->verbose) {
    fprintf(stderr, "brickdesk: light sleep: brightness write failed%s\n",
            shell->power_brightness_known ? " (restore pending)" : "");
  }
  return 1;
}

static int shell_power_display_on(void *user) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL || !shell->power_brightness_known) {
    return 0;
  }
  if (shell->system.set_brightness == NULL ||
      !shell->system.set_brightness(shell->system.user,
                                    shell->power_brightness)) {
    if (shell->verbose) {
      fprintf(stderr, "brickdesk: light sleep: brightness restore failed "
                      "(%d%%); retry on wake\n",
              shell->power_brightness);
    }
    return 1;
  }
  shell->power_brightness_known = false;
  return 0;
}

static void shell_power_clear_local_input(bd_shell *shell) {
  shell->power_swallow = true;
  uint64_t now = monotonic_ms();
  bd_input_state_release_all(&shell->input, now, local_emit, shell);
  bd_input_state_release_all(&shell->test_input, now, local_emit, shell);
  shell->power_swallow = false;
  shell->power_key_down = false;
  if (shell->home != NULL) {
    bd_home_set_hint_visible(shell->home, false);
  }
  if (shell->settings_page != NULL) {
    bd_settings_hold(shell->settings_page, BD_SETTINGS_ACTION_LEFT, false);
    bd_settings_hold(shell->settings_page, BD_SETTINGS_ACTION_RIGHT, false);
    bd_settings_set_hint_visible(shell->settings_page, false);
  }
}

static void shell_power_enter_ui(bd_shell *shell) {
  shell_power_clear_local_input(shell);
  if (shell->home != NULL) {
    /* Custom tile callbacks receive visible=false without closing settings. */
    bd_home_set_surface_visible(shell->home, false);
  }
  bd_shell_led_sleep_pulse(shell);
  if (shell->haptics != NULL &&
      bd_config_get_bool(shell->settings, "input.haptics", true)) {
    static const int strengths[] = {40, 0, 50};
    static const int durations[] = {40, 60, 60};
    bd_haptics_play_pattern(shell->haptics, strengths, durations, 3,
                            shell_now(shell));
  }
  shell->dirty = false;
}

static void shell_power_leave_ui(bd_shell *shell) {
  shell_power_clear_local_input(shell);
  if (shell->home != NULL) {
    bool settings_open = shell->settings_page != NULL &&
                         bd_settings_is_open(shell->settings_page);
    bd_home_set_surface_visible(shell->home,
                                shell->visible && !settings_open);
  }
  bd_shell_led_wake(shell);
  shell->dirty = true;
}

/*
 * Records the single active session instance before the transition.  A
 * launching/suspending/resuming/terminating app is captured too: its
 * bd_session_freeze() returns BUSY, which fails the sleep and rolls it back
 * instead of turning the display off while the app keeps running.
 */
static bool shell_power_capture_foreground(bd_shell *shell) {
  shell->power_app_id[0] = '\0';
  shell->power_app_frozen = false;
  if (shell->session == NULL) {
    return false;
  }
  size_t count = bd_session_running_count(shell->session);
  char fallback[BD_APP_ID_MAX] = {0};
  for (size_t i = 0; i < count; ++i) {
    bd_session_app_info info;
    if (!bd_session_running_at(shell->session, i, &info)) {
      continue;
    }
    if (info.stopped_by_session) {
      continue;
    }
    if (info.state == BD_SESSION_FOREGROUND) {
      copy_text(shell->power_app_id, sizeof(shell->power_app_id), info.app_id);
      return true;
    }
    if (session_active_state(info.state) && fallback[0] == '\0') {
      copy_text(fallback, sizeof(fallback), info.app_id);
    }
  }
  if (fallback[0] != '\0') {
    copy_text(shell->power_app_id, sizeof(shell->power_app_id), fallback);
    return true;
  }
  return false;
}

static bool shell_power_create(bd_shell *shell, const bd_shell_config *config) {
  char error[256] = {0};
  long sleep_after =
      bd_config_get_int(shell->settings, "power.sleep_after", 60000);
  if (sleep_after < 0 || sleep_after > 86400000) {
    sleep_after = 60000;
  }
  long deep_sleep_after =
      bd_config_get_int(shell->settings, "power.deep_sleep_after", 30000);
  if (deep_sleep_after < 0 || deep_sleep_after > 86400000) {
    deep_sleep_after = 30000;
  }
  shell->power_sleep_after_ms = (uint32_t)sleep_after;
  shell->power_deep_sleep_after_ms = (uint32_t)deep_sleep_after;
  const char *long_action =
      bd_config_get_string(shell->settings, "power.long_press_action",
                           "shutdown");
  if (long_action != NULL && strcmp(long_action, "reboot") == 0) {
    shell->power_long_action = SHELL_POWER_LONG_REBOOT;
  } else if (long_action != NULL && strcmp(long_action, "none") == 0) {
    shell->power_long_action = SHELL_POWER_LONG_NONE;
  } else {
    shell->power_long_action = SHELL_POWER_LONG_SHUTDOWN;
  }
  bool safe_shutdown =
      bd_config_get_bool(shell->settings, "power.safe_shutdown", true);
  if (!bd_power_linux_init(&shell->power_linux, NULL, true, NULL, error,
                           sizeof(error))) {
    fprintf(stderr, "brickdesk: power backend: %s\n", error);
    return false;
  }
  bd_power_linux_set_safe_poweroff(&shell->power_linux, safe_shutdown);
  bd_power_backend backend = bd_power_linux_backend(&shell->power_linux);
  if (config->power_backend != NULL) {
    backend = *config->power_backend;
  }
  bd_power_config power_config;
  memset(&power_config, 0, sizeof(power_config));
  power_config.hooks.user = shell;
  power_config.hooks.input_release = shell_power_input_release;
  power_config.hooks.app_pause = shell_power_app_pause;
  power_config.hooks.display_off = shell_power_display_off;
  power_config.hooks.display_on = shell_power_display_on;
  power_config.hooks.app_resume = shell_power_app_resume;
  power_config.hooks.input_acquire = shell_power_input_acquire;
  power_config.backend = backend;
  power_config.allow_light_sleep = true;
  power_config.allow_deep_sleep = true;
  power_config.allow_poweroff = true;
  power_config.allow_reboot = true;
  shell->power = bd_power_create(&power_config, error, sizeof(error));
  if (shell->power == NULL) {
    fprintf(stderr, "brickdesk: power state machine: %s\n", error);
    return false;
  }
  shell->last_activity_ms = shell_now(shell);
  return true;
}

static void shell_power_log(const bd_shell *shell, const char *action,
                            bd_power_result result,
                            const bd_power_report *report) {
  if (shell == NULL) {
    return;
  }
  if (!shell->verbose && result == BD_POWER_OK) {
    return;
  }
  fprintf(stderr, "brickdesk: %s -> %s", action,
          bd_power_result_name(result));
  if (report != NULL && report->failed_step != BD_POWER_STEP_NONE) {
    fprintf(stderr, " at %s", bd_power_step_name(report->failed_step));
  }
  if (report != NULL && report->detail[0] != '\0') {
    fprintf(stderr, " (%s)", report->detail);
  }
  fputc('\n', stderr);
}

static void shell_battery_deadline_check(bd_shell *shell, uint64_t now) {
  if (shell == NULL || !shell->low_battery_shutdown_armed) {
    return;
  }
  if (shell->battery_charging) {
    shell->low_battery_shutdown_armed = false;
    return;
  }
  if (now < shell->low_battery_shutdown_ms ||
      shell->power_request != SHELL_POWER_REQUEST_NONE) {
    return;
  }
  shell->low_battery_shutdown_armed = false;
  shell->power_request = SHELL_POWER_REQUEST_POWEROFF;
}

static void shell_crash_retry_confirm(void *user, bool confirmed) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL) {
    return;
  }
  shell->crash_prompt = false;
  char app_id[BD_APP_ID_MAX];
  copy_text(app_id, sizeof(app_id), shell->crash_retry_app);
  shell->crash_retry_app[0] = '\0';
  if (confirmed && app_id[0] != '\0') {
    shell_launch_app(shell, app_id);
  }
}

static void shell_memory_budget_confirm(void *user, bool confirmed) {
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL) {
    return;
  }
  shell->memory_budget_prompt = false;
  if (confirmed && shell->memory_budget_victim[0] != '\0' &&
      shell->session != NULL) {
    bd_session_result result =
        bd_session_terminate(shell->session, shell->memory_budget_victim);
    if (shell->verbose) {
      fprintf(stderr, "brickdesk: memory budget evict %s -> %s\n",
              shell->memory_budget_victim, bd_session_result_name(result));
    }
    shell->memory_budget_victim[0] = '\0';
    shell->dirty = true;
  }
  shell->memory_budget_next_ms = shell_now(shell) + 5000u;
}

static void shell_memory_budget_check(bd_shell *shell, uint64_t now) {
  if (shell == NULL || shell->session == NULL || shell->memory_budget_mb <= 0 ||
      now < shell->memory_budget_next_ms) {
    return;
  }
  shell->memory_budget_next_ms = now + 5000u;
  if (shell->memory_budget_prompt || shell->apps == NULL ||
      (shell->home != NULL && bd_home_dialog_open(shell->home))) {
    return;
  }
  int total = 0;
  size_t count = bd_app_list_count(shell->apps);
  for (size_t i = 0; i < count && i < shell->runtime_count; ++i) {
    const bd_app *app = bd_app_list_at(shell->apps, i);
    if (app == NULL) {
      continue;
    }
    bd_session_app_info info;
    if (!bd_session_find(shell->session, app->id, &info) ||
        info.state != BD_SESSION_BACKGROUND) {
      continue;
    }
    int memory = shell_runtime_memory(&shell->runtime[i], &info, now, true);
    if (memory > 0) {
      total += memory;
    }
  }
  if (total <= shell->memory_budget_mb) {
    return;
  }
  bd_session_eviction eviction;
  memset(&eviction, 0, sizeof(eviction));
  if (!bd_session_suggest_eviction(shell->session, &eviction) ||
      eviction.app_id[0] == '\0') {
    return;
  }
  const char *name = eviction.app_id;
  bd_session_app_info victim;
  if (bd_session_find(shell->session, eviction.app_id, &victim) &&
      victim.name[0] != '\0') {
    name = victim.name;
  }
  char text[224];
  snprintf(text, sizeof(text),
           "后台内存 %d MB 超过预算 %d MB，结束最久未用的「%s」？", total,
           shell->memory_budget_mb, name);
  shell->memory_budget_prompt = true;
  copy_text(shell->memory_budget_victim, sizeof(shell->memory_budget_victim),
            eviction.app_id);
  if (!bd_shell_confirm(shell, "后台内存超预算", text, 0,
                        shell_memory_budget_confirm, shell)) {
    shell->memory_budget_prompt = false;
    shell->memory_budget_victim[0] = '\0';
  }
}

static void shell_auto_sleep_check(bd_shell *shell, uint64_t now) {
  if (shell == NULL || shell->power == NULL) {
    return;
  }
  bd_power_state state = bd_power_current_state(shell->power);
  if (state == BD_POWER_STATE_IDLE) {
    if (shell->power_sleep_after_ms == 0 ||
        shell->power_request != SHELL_POWER_REQUEST_NONE) {
      return;
    }
    if (now >= shell->last_activity_ms + shell->power_sleep_after_ms) {
      shell->power_request = SHELL_POWER_REQUEST_SLEEP;
    }
    return;
  }
  if (state != BD_POWER_STATE_LIGHT_SLEEP ||
      shell->power_deep_sleep_after_ms == 0 ||
      shell->power_request != SHELL_POWER_REQUEST_NONE) {
    return;
  }
  if (shell->light_sleep_since_ms == 0) {
    shell->light_sleep_since_ms = now;
  }
  if (shell->battery_charging) {
    /* NextUI re-checks charging once a minute instead of deep sleeping. */
    if (now >= shell->light_sleep_since_ms + shell->power_deep_sleep_after_ms) {
      shell->light_sleep_since_ms = now;
    }
    return;
  }
  if (now >= shell->light_sleep_since_ms + shell->power_deep_sleep_after_ms) {
    shell->power_request = SHELL_POWER_REQUEST_DEEP_SLEEP;
  }
}

static void shell_mark_activity(bd_shell *shell) {
  if (shell != NULL) {
    shell->last_activity_ms = shell_now(shell);
  }
}

static void shell_end_all_sessions(bd_shell *shell) {
  if (shell == NULL || shell->session == NULL) {
    return;
  }
  size_t count = bd_session_running_count(shell->session);
  for (size_t i = 0; i < count; ++i) {
    bd_session_app_info info;
    if (!bd_session_running_at(shell->session, i, &info) ||
        !session_running_state(info.state)) {
      continue;
    }
    bd_session_result result =
        bd_session_terminate(shell->session, info.app_id);
    if (shell->verbose) {
      fprintf(stderr, "brickdesk: shutdown end %s -> %s\n", info.app_id,
              bd_session_result_name(result));
    }
  }
}

static void shell_execute_shutdown(bd_shell *shell, bool reboot) {
  if (shell == NULL || shell->power == NULL) {
    return;
  }
  shell_end_all_sessions(shell);
  sync();
  bd_power_report report;
  memset(&report, 0, sizeof(report));
  bd_power_result result =
      reboot ? bd_power_reboot(shell->power, &report)
             : bd_power_poweroff(shell->power, BD_POWER_POWEROFF_STANDARD,
                                 &report);
  shell_power_log(shell, reboot ? "reboot" : "poweroff", result, &report);
  if (result != BD_POWER_OK && result != BD_POWER_NO_EFFECT &&
      result != BD_POWER_DISABLED && result != BD_POWER_UNSUPPORTED) {
    bd_shell_notice(shell, BD_NOTICE_ERROR,
                    reboot ? "重启失败，请检查电源后端"
                           : "关机失败，请检查电源后端",
                    0);
  } else if (result == BD_POWER_DISABLED || result == BD_POWER_UNSUPPORTED) {
    bd_shell_notice(shell, BD_NOTICE_WARNING,
                    "电源后端不可用，操作已取消", 0);
  }
  /* A backend that returned left the machine alive; restore the desktop. */
  shell->power_brightness_known = false;
  shell->power_input_suspended = false;
  shell->dirty = true;
}

static void shell_power_service(bd_shell *shell) {
  if (shell == NULL || shell->power == NULL) {
    return;
  }
  shell_power_check_hold(shell);
  if (shell->power_request == SHELL_POWER_REQUEST_NONE) {
    return;
  }
  shell_power_request request = shell->power_request;
  shell->power_request = SHELL_POWER_REQUEST_NONE;
  bd_power_report report;
  memset(&report, 0, sizeof(report));

  if (request == SHELL_POWER_REQUEST_WAKE) {
    if (!shell_power_asleep(shell)) {
      return;
    }
    bd_power_result result = bd_power_wake(shell->power, &report);
    shell_power_log(shell, "wake", result, &report);
    if (bd_power_current_state(shell->power) == BD_POWER_STATE_IDLE) {
      shell->power_input_suspended = false;
      shell->recovery_notified = false;
      shell->power_ignore_until_ms =
          monotonic_ms() + BD_SHELL_POWER_WAKE_IGNORE_MS;
      shell->last_activity_ms = shell_now(shell);
      shell_power_leave_ui(shell);
    } else {
      /*
       * A failed restore stays DEGRADED; the next POWER press retries wake.
       * Keep input and drawing suppressed until the restore completes.
       */
      shell->power_input_suspended = true;
      if (shell->home != NULL) {
        bd_home_set_surface_visible(shell->home, false);
      }
      shell->dirty = false;
      if (!shell->recovery_notified) {
        shell->recovery_notified = true;
        bd_shell_notice(shell, BD_NOTICE_WARNING,
                        "唤醒恢复失败，请再按一次电源键重试", 0);
      }
    }
    return;
  }

  if (request == SHELL_POWER_REQUEST_POWEROFF ||
      request == SHELL_POWER_REQUEST_REBOOT) {
    shell_execute_shutdown(shell, request == SHELL_POWER_REQUEST_REBOOT);
    return;
  }

  if (request == SHELL_POWER_REQUEST_DEEP_SLEEP) {
    if (bd_power_current_state(shell->power) != BD_POWER_STATE_LIGHT_SLEEP) {
      return;
    }
    bd_power_result result = bd_power_sleep_deep(shell->power, &report);
    shell_power_log(shell, "deep sleep", result, &report);
    shell->light_sleep_since_ms = shell_now(shell);
    if (result == BD_POWER_OK) {
      return;
    }
    if (bd_power_current_state(shell->power) == BD_POWER_STATE_DEGRADED) {
      shell->power_input_suspended = true;
      if (!shell->recovery_notified) {
        shell->recovery_notified = true;
        bd_shell_notice(shell, BD_NOTICE_WARNING,
                        "深睡恢复失败，请按电源键重试", 0);
      }
      return;
    }
    /*
     * NextUI powers the device off when deep sleep is unsupported or keeps
     * failing, rather than leaving it awake in light sleep forever.
     */
    shell_execute_shutdown(shell, false);
    return;
  }

  if (shell_power_asleep(shell)) {
    return;
  }
  memset(&report, 0, sizeof(report));
  shell->power_brightness_known = false;
  shell_power_capture_foreground(shell);
  bd_power_result result = bd_power_sleep_light(shell->power, &report);
  shell_power_log(shell, "light sleep", result, &report);
  if (shell_power_asleep(shell)) {
    shell->power_input_suspended = true;
    shell->light_sleep_since_ms = shell_now(shell);
    shell_power_enter_ui(shell);
  } else {
    /* Recovery completed and the machine is IDLE again. */
    shell->power_input_suspended = false;
    shell->dirty = true;
  }
}

/* ---------------------------------------------------------- dispatch path */

static void shell_handle_dialog_result(bd_shell *shell) {
  bd_home_dialog_result result = bd_home_take_dialog_result(shell->home);
  if (result == BD_HOME_DIALOG_RESULT_NONE) {
    return;
  }
  if (result == BD_HOME_DIALOG_RESULT_CANCEL) {
    shell->pending_launch[0] = '\0';
    shell->evict_app[0] = '\0';
    shell->evict_failed = false;
    shell->dialog_app[0] = '\0';
    return;
  }
  if (shell->pending_launch[0] != '\0' && shell->evict_app[0] != '\0') {
    /* Background-full confirm: end the LRU app, then retry in the loop. */
    if (shell->session != NULL) {
      bd_session_result terminated =
          bd_session_terminate(shell->session, shell->evict_app);
      if (shell->verbose) {
        fprintf(stderr, "brickdesk: evict %s -> %s\n", shell->evict_app,
                bd_session_result_name(terminated));
      }
      if (terminated != BD_SESSION_OK) {
        const char *victim = shell->evict_app;
        bd_session_app_info info;
        if (bd_session_find(shell->session, shell->evict_app, &info) &&
            info.name[0] != '\0') {
          victim = info.name;
        }
        char message[160];
        snprintf(message, sizeof(message), "无法结束「%s」，请重试或取消", victim);
        shell->evict_failed = true;
        if (!bd_home_open_background_full(
                shell->home, shell->pending_launch, shell->evict_app,
                bd_session_max_background(shell->session))) {
          shell->pending_launch[0] = '\0';
          shell->evict_app[0] = '\0';
          shell->evict_failed = false;
        } else {
          bd_home_set_dialog_error(shell->home, message);
        }
        shell->dirty = true;
        return;
      }
      shell->evict_failed = false;
    }
  } else if (shell->dialog_app[0] != '\0' && shell->session != NULL) {
    bd_session_result terminated =
        bd_session_terminate(shell->session, shell->dialog_app);
    if (shell->verbose) {
      fprintf(stderr, "brickdesk: end %s -> %s\n", shell->dialog_app,
              bd_session_result_name(terminated));
    }
  }
  shell->dialog_app[0] = '\0';
  shell->dirty = true;
}

static bool shell_home_input(bd_shell *shell, bd_home_action action) {
  if (shell->home == NULL) {
    return false;
  }
  if (action == BD_HOME_ACTION_END) {
    const char *app_id = bd_home_selected_id(shell->home);
    if (app_id != NULL) {
      copy_text(shell->dialog_app, sizeof(shell->dialog_app), app_id);
    }
  }
  bool changed = bd_home_input(shell->home, action);
  if (action == BD_HOME_ACTION_END && !bd_home_dialog_open(shell->home)) {
    /* X on a non-running tile: do not leave a stale end target behind. */
    shell->dialog_app[0] = '\0';
  }
  shell_handle_dialog_result(shell);
  if (changed) {
    shell->dirty = true;
  }
  return changed;
}

bool bd_shell_open_settings(bd_shell *shell, int category) {
  if (shell == NULL || shell->home == NULL || shell->settings_page == NULL) {
    return false;
  }
  if (!bd_settings_open(shell->settings_page, category)) {
    return false;
  }
  bd_home_set_hint_visible(shell->home, false);
  bd_home_set_surface_visible(shell->home, false);
  shell_refresh_storage(shell);
  shell_publish_status(shell, true);
  shell->dirty = true;
  return true;
}

static void shell_close_settings(bd_shell *shell) {
  if (shell == NULL || shell->settings_page == NULL) {
    return;
  }
  bool was_open = bd_settings_is_open(shell->settings_page);
  if (was_open) {
    bd_settings_close(shell->settings_page);
  }
  if (shell->home != NULL) {
    bd_home_set_surface_visible(shell->home, shell->visible);
    bd_home_damage(shell->home);
  }
  if (was_open) {
    shell_home_quick_refresh(shell);
    shell->dirty = true;
  }
}

bool bd_shell_settings_visible(const bd_shell *shell) {
  return shell != NULL && shell->settings_page != NULL &&
         bd_settings_is_open(shell->settings_page);
}

static void shell_settings_key(bd_shell *shell, bd_key key, bool pressed) {
  bd_settings *page = shell->settings_page;
  if (page == NULL || !bd_settings_is_open(page)) {
    return;
  }
  if (!pressed) {
    if (key == BD_KEY_LEFT) {
      bd_settings_hold(page, BD_SETTINGS_ACTION_LEFT, false);
    } else if (key == BD_KEY_RIGHT) {
      bd_settings_hold(page, BD_SETTINGS_ACTION_RIGHT, false);
    }
    return;
  }
  bool changed = false;
  switch (key) {
    case BD_KEY_A:
      changed = bd_settings_input(page, BD_SETTINGS_ACTION_ACCEPT);
      break;
    case BD_KEY_B:
      changed = bd_settings_input(page, BD_SETTINGS_ACTION_CANCEL);
      break;
    case BD_KEY_LEFT:
      bd_settings_hold(page, BD_SETTINGS_ACTION_LEFT, true);
      changed = true;
      break;
    case BD_KEY_RIGHT:
      bd_settings_hold(page, BD_SETTINGS_ACTION_RIGHT, true);
      changed = true;
      break;
    case BD_KEY_UP:
      changed = bd_settings_input(page, BD_SETTINGS_ACTION_UP);
      break;
    case BD_KEY_DOWN:
      changed = bd_settings_input(page, BD_SETTINGS_ACTION_DOWN);
      break;
    case BD_KEY_L1:
      changed = bd_settings_input(page, BD_SETTINGS_ACTION_CATEGORY_PREV);
      break;
    case BD_KEY_R1:
      changed = bd_settings_input(page, BD_SETTINGS_ACTION_CATEGORY_NEXT);
      break;
    default:
      return;
  }
  if (changed) {
    shell->dirty = true;
  }
  if (!bd_settings_is_open(page)) {
    shell_close_settings(shell);
  }
}

static void shell_accept(bd_shell *shell) {
  if (shell->home == NULL) {
    return;
  }
  if (bd_home_dialog_open(shell->home)) {
    shell_home_input(shell, BD_HOME_ACTION_ACCEPT);
    return;
  }
  const char *app_id = bd_home_selected_id(shell->home);
  if (app_id != NULL) {
    if (strcmp(app_id, BD_SHELL_PLUGIN_ITEM_ID) == 0) {
      return;
    }
    if (strcmp(app_id, BD_SHELL_SETTINGS_ITEM_ID) == 0) {
      bd_shell_open_settings(shell, BD_SETTINGS_CATEGORY_APPEARANCE);
      return;
    }
    shell_launch_app(shell, app_id);
    return;
  }
  if (bd_home_quick_open(shell->home)) {
    /* Quick-panel control row: let the home toggle it. */
    shell_home_input(shell, BD_HOME_ACTION_ACCEPT);
  }
}

static void shell_power_fire_long_action(bd_shell *shell) {
  if (shell == NULL || shell->power == NULL || shell_power_asleep(shell)) {
    return;
  }
  switch (shell->power_long_action) {
    case SHELL_POWER_LONG_REBOOT:
      if (shell->power_request == SHELL_POWER_REQUEST_NONE) {
        shell->power_request = SHELL_POWER_REQUEST_REBOOT;
      }
      break;
    case SHELL_POWER_LONG_SHUTDOWN:
      if (shell->power_request == SHELL_POWER_REQUEST_NONE) {
        shell->power_request = SHELL_POWER_REQUEST_POWEROFF;
      }
      break;
    case SHELL_POWER_LONG_NONE:
    default:
      break;
  }
}

static void shell_power_check_hold(bd_shell *shell) {
  if (shell == NULL || shell->power == NULL || !shell->power_key_down ||
      shell->power_long_fired || shell_power_asleep(shell)) {
    return;
  }
  uint64_t now = shell_now(shell);
  if (now < shell->power_down_ms + BD_SHELL_POWER_TAP_MS) {
    return;
  }
  shell->power_long_fired = true;
  shell->power_down_ms = 0;
  shell_power_fire_long_action(shell);
}

static void shell_power_key_event(bd_shell *shell, bool pressed,
                                  uint64_t time_ms) {
  if (shell->power == NULL || shell->power_swallow) {
    return;
  }
  if (pressed) {
    if (shell->power_key_down) {
      return;
    }
    if (shell_power_asleep(shell)) {
      /* Wake on the press so the release cannot re-enter sleep. */
      shell->power_request = SHELL_POWER_REQUEST_WAKE;
      return;
    }
    if (monotonic_ms() < shell->power_ignore_until_ms) {
      /* The press that woke the device can still be held; swallow this one. */
      shell->power_press_ignored = true;
      return;
    }
    shell->power_press_ignored = false;
    shell->power_key_down = true;
    shell->power_long_fired = false;
    shell->power_down_ms = time_ms;
    return;
  }
  if (shell->power_press_ignored) {
    shell->power_press_ignored = false;
    return;
  }
  if (!shell->power_key_down) {
    /* A source switch can synthesize a release without a power down. */
    return;
  }
  shell->power_key_down = false;
  if (shell_power_asleep(shell)) {
    /* The release belongs to the press that woke (or will wake) the shell. */
    return;
  }
  if (monotonic_ms() < shell->power_ignore_until_ms) {
    return;
  }
  uint64_t held = time_ms >= shell->power_down_ms
                      ? time_ms - shell->power_down_ms
                      : 0;
  shell->power_down_ms = 0;
  if (shell->power_long_fired) {
    shell->power_long_fired = false;
    return;
  }
  if (held > BD_SHELL_POWER_TAP_MS) {
    /* The poll normally fires at the threshold; a late release still acts. */
    shell->power_long_fired = true;
    shell_power_fire_long_action(shell);
    shell->power_long_fired = false;
    return;
  }
  if (shell->power_request == SHELL_POWER_REQUEST_NONE) {
    shell->power_request = SHELL_POWER_REQUEST_SLEEP;
  }
}

static void shell_dispatch_key(bd_shell *shell, bd_key key, bool pressed,
                               uint64_t time_ms) {
  if (pressed) {
    shell_mark_activity(shell);
  }
  if (key == BD_KEY_POWER) {
    /* POWER must work while an app owns the display and while asleep. */
    shell_power_key_event(shell, pressed, time_ms);
    return;
  }
  if (shell->power_swallow || shell->power_input_suspended ||
      shell_power_asleep(shell)) {
    return;
  }
  if (!shell->visible) {
    return;
  }
  if (shell->notice != NULL && bd_notice_confirm_open(shell->notice)) {
    if (!pressed) {
      return;
    }
    switch (key) {
      case BD_KEY_A:
        bd_notice_input(shell->notice, BD_NOTICE_ACTION_ACCEPT);
        break;
      case BD_KEY_B:
        bd_notice_input(shell->notice, BD_NOTICE_ACTION_CANCEL);
        break;
      case BD_KEY_LEFT:
        bd_notice_input(shell->notice, BD_NOTICE_ACTION_LEFT);
        break;
      case BD_KEY_RIGHT:
        bd_notice_input(shell->notice, BD_NOTICE_ACTION_RIGHT);
        break;
      default:
        break;
    }
    shell->dirty = true;
    return;
  }
  if (pressed && (key == BD_KEY_UP || key == BD_KEY_DOWN ||
                  key == BD_KEY_LEFT || key == BD_KEY_RIGHT ||
                  key == BD_KEY_L1 || key == BD_KEY_R1)) {
    shell_haptic_nav(shell);
  }
  if (shell->settings_page != NULL &&
      bd_settings_is_open(shell->settings_page)) {
    shell_settings_key(shell, key, pressed);
    return;
  }
  if (!pressed) {
    return;
  }
  switch (key) {
    case BD_KEY_A:
      shell_accept(shell);
      break;
    case BD_KEY_B:
      if (bd_home_dialog_open(shell->home) || bd_home_quick_open(shell->home)) {
        shell_home_input(shell, BD_HOME_ACTION_CANCEL);
      }
      break;
    case BD_KEY_X:
      shell_home_input(shell, BD_HOME_ACTION_END);
      break;
    case BD_KEY_START:
      shell_home_input(shell, BD_HOME_ACTION_QUICK);
      break;
    case BD_KEY_LEFT:
      shell_home_input(shell, BD_HOME_ACTION_LEFT);
      break;
    case BD_KEY_RIGHT:
      shell_home_input(shell, BD_HOME_ACTION_RIGHT);
      break;
    case BD_KEY_UP:
      shell_home_input(shell, BD_HOME_ACTION_UP);
      break;
    case BD_KEY_DOWN:
      shell_home_input(shell, BD_HOME_ACTION_DOWN);
      break;
    case BD_KEY_L1:
      shell_home_input(shell, BD_HOME_ACTION_TAB_PREV);
      break;
    case BD_KEY_R1:
      shell_home_input(shell, BD_HOME_ACTION_TAB_NEXT);
      break;
    case BD_KEY_Y:
      shell_toggle_favorite(shell);
      break;
    case BD_KEY_HOME:
      if (shell->app_history != NULL) {
        const char *recent = bd_app_history_most_recent(shell->app_history);
        int index =
            recent != NULL ? bd_home_find_item(shell->home, recent) : -1;
        if (index >= 0) {
          bd_home_set_focus(shell->home, index);
          shell->dirty = true;
          break;
        }
      }
      shell_home_input(shell, BD_HOME_ACTION_RECENT);
      break;
    default:
      break;
  }
}

static bool shell_terminate_foreground(bd_shell *shell) {
  if (shell->session == NULL) {
    return false;
  }
  size_t count = bd_session_running_count(shell->session);
  for (size_t i = 0; i < count; ++i) {
    bd_session_app_info info;
    if (!bd_session_running_at(shell->session, i, &info)) {
      continue;
    }
    if (session_active_state(info.state)) {
      bd_session_result result =
          bd_session_terminate(shell->session, info.app_id);
      if (shell->verbose) {
        fprintf(stderr, "brickdesk: return_home %s -> %s\n", info.app_id,
                bd_session_result_name(result));
      }
      return result == BD_SESSION_OK;
    }
  }
  return false;
}

static unsigned fb_channel(uint32_t pixel, const struct fb_bitfield *field) {
  if (field->length == 0 || field->length >= 32) {
    return 0;
  }
  uint32_t mask = (1u << field->length) - 1u;
  return (((pixel >> field->offset) & mask) * 255u) / mask;
}

static bool shell_capture_framebuffer(bd_image *out) {
  if (out == NULL) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  int fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  struct fb_var_screeninfo var;
  struct fb_fix_screeninfo fix;
  if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
      ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0 ||
      (var.bits_per_pixel != 16 && var.bits_per_pixel != 32) ||
      var.xres == 0 || var.yres == 0 || fix.line_length == 0) {
    close(fd);
    return false;
  }
  size_t needed = (size_t)fix.line_length * var.yres;
  if (fix.smem_len != 0 && needed > fix.smem_len) {
    close(fd);
    return false;
  }
  unsigned char *mapping =
      (unsigned char *)mmap(NULL, needed, PROT_READ, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    close(fd);
    return false;
  }
  size_t pixels_size = (size_t)var.xres * var.yres * 4u;
  uint8_t *pixels = (uint8_t *)malloc(pixels_size);
  bool ok = pixels != NULL;
  for (unsigned y = 0; ok && y < var.yres; ++y) {
    const unsigned char *row =
        mapping + (size_t)y * fix.line_length +
        (size_t)var.yoffset * fix.line_length;
    for (unsigned x = 0; x < var.xres; ++x) {
      uint32_t pixel;
      if (var.bits_per_pixel == 32) {
        memcpy(&pixel, row + (size_t)x * 4u, sizeof(pixel));
      } else {
        uint16_t value;
        memcpy(&value, row + (size_t)x * 2u, sizeof(value));
        pixel = value;
      }
      uint8_t *dst = pixels + ((size_t)y * var.xres + x) * 4u;
      dst[0] = (uint8_t)fb_channel(pixel, &var.red);
      dst[1] = (uint8_t)fb_channel(pixel, &var.green);
      dst[2] = (uint8_t)fb_channel(pixel, &var.blue);
      dst[3] = 255;
    }
  }
  munmap(mapping, needed);
  close(fd);
  if (!ok) {
    return false;
  }
  out->width = (int)var.xres;
  out->height = (int)var.yres;
  out->pixels = pixels;
  return true;
}

static void shell_app_history_save(bd_shell *shell) {
  if (shell == NULL || shell->app_history == NULL ||
      shell->app_history_path[0] == '\0' ||
      getenv("BRICKDESK_HEADLESS") != NULL) {
    return;
  }
  bd_app_history_save(shell->app_history, shell->app_history_path);
}

static void shell_record_app_use(bd_shell *shell, const char *app_id) {
  if (shell == NULL || shell->app_history == NULL || app_id == NULL) {
    return;
  }
  if (bd_app_history_record(shell->app_history, app_id, (int64_t)time(NULL))) {
    shell_app_history_save(shell);
  }
}

static void shell_toggle_favorite(bd_shell *shell) {
  if (shell == NULL || shell->home == NULL || shell->app_history == NULL) {
    return;
  }
  const char *id = bd_home_selected_id(shell->home);
  if (id == NULL || strcmp(id, BD_SHELL_SETTINGS_ITEM_ID) == 0 ||
      strcmp(id, BD_SHELL_PLUGIN_ITEM_ID) == 0) {
    return;
  }
  bool favorite = !bd_home_favorite(shell->home, id);
  bd_home_set_favorite(shell->home, id, favorite);
  bd_app_history_set_favorite(shell->app_history, id, favorite);
  shell_app_history_save(shell);
  bd_shell_notice(shell, BD_NOTICE_SUCCESS,
                  favorite ? "已加入收藏" : "已取消收藏", 1600);
  shell->dirty = true;
}

static void shell_screenshot(bd_shell *shell) {
  if (shell == NULL) {
    return;
  }
  const char *config_dir =
      shell->settings != NULL ? bd_config_dir(shell->settings) : NULL;
  char directory[BD_SHELL_PATH_MAX];
  int written = snprintf(directory, sizeof(directory), "%s/screenshots",
                         config_dir != NULL && config_dir[0] != '\0'
                             ? config_dir
                             : "/tmp");
  if (written <= 0 || (size_t)written >= sizeof(directory)) {
    bd_shell_notice(shell, BD_NOTICE_ERROR, "截图失败：路径过长", 0);
    return;
  }
  if (mkdir(directory, 0755) != 0 && errno != EEXIST) {
    bd_shell_notice(shell, BD_NOTICE_ERROR, "截图失败：无法创建目录", 0);
    return;
  }
  time_t wall = time(NULL);
  struct tm local;
  char stamp[32] = "00000000-000000";
  if (wall != (time_t)-1 && localtime_r(&wall, &local) != NULL) {
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
  }
  char path[BD_SHELL_PATH_MAX];
  written = snprintf(path, sizeof(path), "%s/screenshot-%s-%03u.png", directory,
                     stamp, ++shell->screenshot_seq % 1000u);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    bd_shell_notice(shell, BD_NOTICE_ERROR, "截图失败：文件名过长", 0);
    return;
  }

  bool ok = false;
  if (shell->visible && shell->gfx != NULL) {
    ok = bd_gfx_save_png(shell->gfx, path);
  } else {
    bd_image image;
    if (shell_capture_framebuffer(&image)) {
      ok = bd_image_write_png(&image, path);
      bd_image_free(&image);
    }
  }
  if (!ok && shell->gfx != NULL) {
    ok = bd_gfx_save_png(shell->gfx, path);
  }
  if (ok) {
    char message[256];
    char name[96];
    const char *slash = strrchr(path, '/');
    copy_text(name, sizeof(name), slash != NULL ? slash + 1 : path);
    snprintf(message, sizeof(message), "截图已保存：%s", name);
    bd_shell_notice(shell, BD_NOTICE_SUCCESS, message, 2500);
  } else {
    bd_shell_notice(shell, BD_NOTICE_ERROR, "截图失败：无法写入 PNG", 0);
  }
}

static void shell_dispatch_action(bd_shell *shell, const char *action,
                                  bool pressed) {
  if (action == NULL) {
    return;
  }
  if (pressed) {
    shell_mark_activity(shell);
  }
  if (shell->power_swallow || shell->power_input_suspended ||
      shell_power_asleep(shell)) {
    return;
  }
  if (strcmp(action, "ui.hint") == 0 && shell->visible) {
    if (shell->settings_page != NULL &&
        bd_settings_is_open(shell->settings_page)) {
      bd_settings_set_hint_visible(shell->settings_page, pressed);
    } else {
      bd_home_set_hint_visible(shell->home, pressed);
    }
    shell->dirty = true;
  }
  if (strcmp(action, "session.return_home") == 0 && pressed &&
      !shell->visible) {
    if (shell_terminate_foreground(shell)) {
      shell->dirty = true;
    }
  }
  if (pressed && strcmp(action, "builtin.screenshot") == 0) {
    shell_screenshot(shell);
  }
  if (shell->verbose &&
      (strcmp(action, "menu.open") == 0 || strcmp(action, "perf.cycle") == 0 ||
       strcmp(action, "builtin.screenshot") == 0)) {
    fprintf(stderr, "brickdesk: action %s\n", action);
  }
}

static bool shell_home_begin_launch(bd_shell *shell, const char *app_id) {
  if (shell->home == NULL) {
    return false;
  }
  uint64_t now = shell_now(shell);
  bd_home_update(shell->home, now);
  if (!bd_home_start_launch(shell->home, app_id)) {
    fprintf(stderr, "brickdesk: cannot start launch animation for %s\n",
            app_id);
    return false;
  }
  shell->visible = true;
  shell->dirty = true;
  return true;
}

/*
 * The session only refuses a *background* transition when the quota is full,
 * but the home must also refuse a new cold launch.  Show the choice and let
 * shell_handle_dialog_result() end the LRU app before retrying.
 */
static bool shell_request_eviction(bd_shell *shell, const char *app_id) {
  if (shell->session == NULL || shell->home == NULL) {
    return false;
  }
  bd_session_eviction eviction;
  memset(&eviction, 0, sizeof(eviction));
  if (!bd_session_suggest_eviction(shell->session, &eviction)) {
    return false;
  }
  copy_text(shell->pending_launch, sizeof(shell->pending_launch), app_id);
  copy_text(shell->evict_app, sizeof(shell->evict_app), eviction.app_id);
  shell->evict_failed = false;
  bd_home_update(shell->home, shell_now(shell));
  if (!bd_home_open_background_full(shell->home, app_id, eviction.app_id,
                                    bd_session_max_background(shell->session))) {
    shell->pending_launch[0] = '\0';
    shell->evict_app[0] = '\0';
    return false;
  }
  shell->visible = true;
  shell->dirty = true;
  return true;
}

static bool shell_launch_app(bd_shell *shell, const char *app_id) {
  if (shell->session == NULL || app_id == NULL || app_id[0] == '\0' ||
      strcmp(app_id, BD_SHELL_PLUGIN_ITEM_ID) == 0) {
    return false;
  }
  bd_session_app_info info;
  if (bd_session_find(shell->session, app_id, &info)) {
    if (info.state == BD_SESSION_BACKGROUND) {
      bd_session_result result = bd_session_resume(shell->session, app_id);
      if (result == BD_SESSION_OK) {
        shell_record_app_use(shell, app_id);
        return shell_home_begin_launch(shell, app_id);
      }
      if (shell->verbose) {
        fprintf(stderr, "brickdesk: resume %s -> %s\n", app_id,
                bd_session_result_name(result));
      }
      if (result == BD_SESSION_STATE_QUOTA) {
        const char *message = bd_session_last_error(shell->session);
        bd_shell_notice(shell, BD_NOTICE_ERROR,
                        message[0] != '\0' ? message : "存档配额已满，无法恢复应用",
                        0);
      }
      return false;
    }
    if (session_active_state(info.state) || info.state == BD_SESSION_CRASHED) {
      return false;
    }
    bd_session_clear_terminated(shell->session);
  }
  const bd_app *app = bd_app_list_find(shell->apps, app_id);
  if (app == NULL) {
    if (shell->verbose) {
      fprintf(stderr, "brickdesk: app '%s' is not installed\n", app_id);
    }
    return false;
  }
  int max_background = bd_session_max_background(shell->session);
  if (max_background > 0 &&
      bd_session_background_count(shell->session) >= max_background) {
    if (shell_request_eviction(shell, app_id)) {
      return true;
    }
    fprintf(stderr, "brickdesk: launch %s needs a free background slot\n",
            app_id);
    return false;
  }
  bd_app copy;
  memset(&copy, 0, sizeof(copy));
  if (!bd_app_copy(&copy, app)) {
    return false;
  }
  if (!apply_menu_env(shell, &copy, shell->menu_shim)) {
    bd_app_destroy(&copy);
    return false;
  }
  char error[256] = {0};
  bd_session_result result =
      bd_session_launch(shell->session, &copy, error, sizeof(error));
  bd_app_destroy(&copy);
  if (result == BD_SESSION_NEEDS_CONFIRM) {
    if (shell_request_eviction(shell, app_id)) {
      return true;
    }
    fprintf(stderr, "brickdesk: launch %s needs a free background slot\n",
            app_id);
    return false;
  }
  if (result != BD_SESSION_OK) {
    fprintf(stderr, "brickdesk: launch %s failed: %s%s%s\n", app_id,
            bd_session_result_name(result), error[0] != '\0' ? ": " : "",
            error);
    bd_shell_notice(shell,
                    result == BD_SESSION_STATE_QUOTA ? BD_NOTICE_ERROR
                                                     : BD_NOTICE_WARNING,
                    error[0] != '\0' ? error : "应用启动失败", 0);
    return false;
  }
  if (!shell_home_begin_launch(shell, app_id)) {
    return false;
  }
  shell_record_app_use(shell, app_id);
  if (shell->verbose) {
    const char *shim = menu_shim_path(shell, NULL);
    fprintf(stderr, "brickdesk: launched %s%s%s\n", app_id,
            shim != NULL ? " with " : "",
            shim != NULL ? shim : " without menu overlay");
  }
  return true;
}

/* ------------------------------------------------------------ applications */

typedef struct shell_demo_item {
  const char *id;
  const char *title;
  const char *subtitle;
  int span_w;
  int span_h;
  int cover;
  int r;
  int g;
  int b;
  uint32_t flags;
} shell_demo_item;

static const shell_demo_item SHELL_DEMO_ITEMS[] = {
    {"demo-town", "小镇四季", "第 3 年夏季 · 已玩 41 小时", 3, 2, 1, 109, 122,
     99, 0},
    {"demo-space", "星海拾荒者", "第 7 扇区 · 已玩 6 小时", 2, 2, 2, 76, 92,
     112, 0},
    {"demo-terminal", "终端", "本机 Shell", 1, 1, 3, 44, 46, 44, 0},
    {"demo-music", "音乐", "夜航 · 林间电台", 2, 2, 5, 123, 90, 140,
     BD_HOME_ITEM_LIVE},
    {"demo-reading", "阅读", "《三体》第 14 章", 1, 1, 6, 93, 124, 90, 0},
    {"demo-files", "文件", "SD 卡已用 41 GB", 1, 1, 7, 76, 92, 112, 0},
    {"demo-photos", "相册", "截图与照片 86 张", 1, 1, 8, 176, 138, 78, 0},
    {"demo-transfer", "无线传输", "浏览器传文件", 2, 1, 1, 63, 133, 135, 0},
    {"demo-system", "系统", "屏幕、网络与按键", 1, 1, 2, 111, 115, 123, 0},
    {"demo-harbor", "雾港", "第二章「灯塔」", 1, 1, 3, 81, 96, 108,
     BD_HOME_ITEM_GAME},
    {"demo-well", "深井", "到达地下 12 层", 1, 1, 4, 75, 94, 102,
     BD_HOME_ITEM_GAME},
    {"demo-road", "赤色公路", "最佳圈速 1:42.08", 1, 1, 5, 162, 90, 76,
     BD_HOME_ITEM_GAME},
    {"demo-favorites", "收藏夹", "12 个收藏应用", 1, 1, 6, 123, 90, 140,
     BD_HOME_ITEM_FAVORITE},
};

static void shell_span_for_index(int index, int *width, int *height) {
  static const int CYCLE[24][2] = {
      {3, 2}, {1, 1}, {1, 1}, {2, 2}, {1, 1}, {1, 2}, {1, 1}, {1, 1},
      {1, 1}, {2, 1}, {1, 1}, {1, 1}, {2, 2}, {1, 1}, {2, 1}, {1, 2},
      {1, 1}, {2, 2}, {1, 1}, {2, 1}, {1, 1}, {2, 2}, {1, 1}, {1, 1},
  };
  int cycle = index % 24;
  *width = CYCLE[cycle][0];
  *height = CYCLE[cycle][1];
}

static bd_app_cover_span shell_cover_span(int width, int height) {
  if (width >= 3 && height >= 2) {
    return BD_APP_SPAN_3X2;
  }
  if (width >= 2 && height >= 2) {
    return BD_APP_SPAN_2X2;
  }
  if (width >= 2) {
    return BD_APP_SPAN_2X1;
  }
  if (height >= 2) {
    return BD_APP_SPAN_1X2;
  }
  return BD_APP_SPAN_1X1;
}

static bd_color shell_app_placeholder(const bd_app *app,
                                      const bd_app_asset *asset) {
  bd_color color;
  if (asset != NULL && asset->color[0] != '\0' &&
      bd_color_from_hex(asset->color, &color)) {
    return bd_color_mix(bd_color_rgb(18, 16, 13), color, 0.55f);
  }
  const char *hex = app->accent[0] != '\0' ? app->accent : app->fallback_color;
  if (hex[0] != '\0' && bd_color_from_hex(hex, &color)) {
    return bd_color_mix(bd_color_rgb(18, 16, 13), color, 0.55f);
  }
  return bd_color_rgb(58, 52, 46);
}

static bool shell_has_plugin_tile(const bd_shell *shell) {
  return shell != NULL && shell->plugin_tile != NULL;
}

static void shell_settings_item(const bd_shell *shell, bd_home_item *item) {
  memset(item, 0, sizeof(*item));
  item->id = BD_SHELL_SETTINGS_ITEM_ID;
  item->title = "设置";
  item->subtitle = "显示、声音、网络与按键";
  item->placeholder =
      bd_color_mix(bd_color_rgb(18, 16, 13), shell->home_style.accent_base,
                   0.34f);
  item->span_w = 1;
  item->span_h = 1;
  item->flags = 0;
}

static void shell_plugin_tile_item(const bd_shell *shell, bd_home_item *item) {
  memset(item, 0, sizeof(*item));
  item->id = BD_SHELL_PLUGIN_ITEM_ID;
  item->title = shell->plugin_tile_name;
  item->subtitle = "插件";
  item->placeholder =
      bd_color_mix(bd_color_rgb(18, 16, 13), shell->home_style.accent_base,
                   0.42f);
  item->span_w = shell->plugin_tile_span_w;
  item->span_h = shell->plugin_tile_span_h;
  item->flags = 0;
  item->badge_number = 0;
}

static void shell_plugin_tile_draw(void *user, bd_home *home, bd_gfx *gfx,
                                   bd_rect rect, bool visible) {
  (void)home;
  bd_shell *shell = (bd_shell *)user;
  if (shell == NULL || shell->plugin_tile == NULL) {
    return;
  }
  bd_plugin_set_tile_visible(shell->plugin_tile, visible);
  if (!visible) {
    return;
  }
  bool changed = bd_plugin_tile_begin_frame(shell->plugin_tile, gfx);
  if (changed) {
    bd_rect damage = rect;
    damage.x -= 4;
    damage.y -= 4;
    damage.w += 8;
    damage.h += 8;
    bd_gfx_damage_rect(gfx, damage);
  }
  bd_plugin_render_style style;
  memset(&style, 0, sizeof(style));
  style.fonts = shell->plugin_fonts_loaded ? &shell->plugin_fonts : NULL;
  style.accent = shell->plugin_accent;
  style.dark = true;
  bd_plugin_tile_draw(shell->plugin_tile, gfx, rect, &style);
}

static bool shell_attach_plugin_tile(bd_shell *shell) {
  if (!shell_has_plugin_tile(shell) || shell->home == NULL) {
    return true;
  }
  return bd_home_set_custom_draw(shell->home, BD_SHELL_PLUGIN_ITEM_ID,
                                 shell_plugin_tile_draw, shell);
}

static int shell_item_count_with_builtins(const bd_shell *shell, int count) {
  return count + 1 + (shell_has_plugin_tile(shell) ? 1 : 0);
}


static bool shell_set_demo_items(bd_shell *shell) {
  int base = (int)(sizeof(SHELL_DEMO_ITEMS) / sizeof(SHELL_DEMO_ITEMS[0]));
  int count = shell_item_count_with_builtins(shell, base);
  bd_home_item *items =
      (bd_home_item *)calloc((size_t)count, sizeof(*items));
  char(*covers)[512] = (char(*)[512])calloc((size_t)count, 512);
  if (items == NULL || covers == NULL) {
    free(items);
    free(covers);
    return false;
  }
  for (int i = 0; i < base; ++i) {
    const shell_demo_item *demo = &SHELL_DEMO_ITEMS[i];
    snprintf(covers[i], 512, "%s/covers/home-%02d.jpg", BRICKDESK_ASSET_DIR,
             demo->cover);
    items[i].id = demo->id;
    items[i].title = demo->title;
    items[i].subtitle = demo->subtitle;
    items[i].image_path = covers[i];
    items[i].placeholder =
        bd_color_rgb((uint8_t)demo->r, (uint8_t)demo->g, (uint8_t)demo->b);
    items[i].span_w = demo->span_w;
    items[i].span_h = demo->span_h;
    items[i].flags = demo->flags;
    items[i].badge_number = i + 1;
  }
  shell_settings_item(shell, &items[base]);
  items[base].badge_number = base + 1;
  if (shell_has_plugin_tile(shell)) {
    shell_plugin_tile_item(shell, &items[base + 1]);
  }
  bool ok = bd_home_set_items(shell->home, items, count);
  free(items);
  free(covers);
  if (ok) {
    ok = shell_attach_plugin_tile(shell);
  }
  return ok;
}

static bool shell_set_app_items(bd_shell *shell) {
  size_t count = shell->apps != NULL ? bd_app_list_count(shell->apps) : 0;
  if (count == 0) {
    return shell_set_demo_items(shell);
  }
  if (count > 100000) {
    return false;
  }
  int base = (int)count;
  int total = shell_item_count_with_builtins(shell, base);
  bd_home_item *items = (bd_home_item *)calloc((size_t)total, sizeof(*items));
  char(*covers)[BD_APP_PATH_MAX] =
      (char(*)[BD_APP_PATH_MAX])calloc((size_t)total, BD_APP_PATH_MAX);
  if (items == NULL || covers == NULL) {
    free(items);
    free(covers);
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    const bd_app *app = bd_app_list_at(shell->apps, i);
    if (app == NULL) {
      continue;
    }
    int span_w = 1;
    int span_h = 1;
    shell_span_for_index((int)i, &span_w, &span_h);
    bd_app_asset asset;
    memset(&asset, 0, sizeof(asset));
    bool have_asset =
        bd_app_asset_cover(app, shell_cover_span(span_w, span_h), &asset);
    if (have_asset && asset.path[0] != '\0') {
      copy_text(covers[i], BD_APP_PATH_MAX, asset.path);
    }
    items[i].id = app->id;
    items[i].title = app->name;
    items[i].subtitle =
        app->subtitle[0] != '\0' ? app->subtitle
                                 : bd_app_category_name(app->category);
    items[i].image_path = covers[i][0] != '\0' ? covers[i] : NULL;
    items[i].placeholder = shell_app_placeholder(app, &asset);
    items[i].span_w = span_w;
    items[i].span_h = span_h;
    items[i].flags = (app->category == BD_APP_CATEGORY_GAME ||
                      app->category == BD_APP_CATEGORY_EMULATOR)
                         ? BD_HOME_ITEM_GAME
                         : 0;
    items[i].badge_number = (int)i + 1;
  }
  shell_settings_item(shell, &items[base]);
  items[base].badge_number = base + 1;
  if (shell_has_plugin_tile(shell)) {
    shell_plugin_tile_item(shell, &items[base + 1]);
  }
  bool ok = bd_home_set_items(shell->home, items, total);
  free(items);
  free(covers);
  if (ok) {
    ok = shell_attach_plugin_tile(shell);
  }
  return ok;
}

static void shell_load_apps(bd_shell *shell, const char *apps_dir) {
  free(shell->runtime);
  shell->runtime = NULL;
  shell->runtime_count = 0;
  if (shell->apps != NULL) {
    bd_app_list_destroy(shell->apps);
  }
  shell->apps = bd_app_list_create();
  const char *dir = apps_dir != NULL ? apps_dir : bd_app_default_dir();
  if (shell->apps != NULL) {
    char error[256] = {0};
    if (!bd_app_list_scan(shell->apps, dir, NULL, NULL, error,
                          sizeof(error))) {
      fprintf(stderr, "brickdesk: apps: %s\n", error);
    }
  }
  size_t count = shell->apps != NULL ? bd_app_list_count(shell->apps) : 0;
  if (count > 0) {
    shell->runtime =
        (shell_app_runtime *)calloc(count, sizeof(*shell->runtime));
    if (shell->runtime != NULL) {
      shell->runtime_count = count;
      for (size_t i = 0; i < count; ++i) {
        shell->runtime[i].memory_mb = -1;
      }
    }
  }
  if (!shell_set_app_items(shell)) {
    fprintf(stderr, "brickdesk: cannot build the home wall\n");
  }
  if (shell->verbose) {
    fprintf(stderr, "brickdesk: %zu app(s) from %s\n", count, dir);
  }
}

/* ------------------------------------------------------------------- loop */

bool bd_shell_render(bd_shell *shell) {
  if (shell == NULL) {
    return false;
  }
  /* A queued POWER request runs outside input and before any frame. */
  shell_power_service(shell);
  if (shell->power_input_suspended || shell_power_asleep(shell) ||
      !shell->visible || shell->home == NULL) {
    return false;
  }
  bool settings_open = shell->settings_page != NULL &&
                       bd_settings_is_open(shell->settings_page);
  bd_gfx_begin_frame(shell->gfx);
  if (settings_open) {
    bd_settings_draw(shell->settings_page, shell->gfx);
  } else {
    bd_home_update(shell->home, shell_now(shell));
    bd_home_draw(shell->home, shell->gfx);
  }
  if (shell->notice != NULL) {
    bd_notice_draw(shell->notice, shell->gfx, shell_now(shell));
  }
  bd_gfx_end_frame(shell->gfx);
  bd_gfx_stats stats = bd_gfx_get_stats(shell->gfx);
  if (stats.presented) {
    bd_platform_present(shell->platform);
    ++shell->presents;
  }
  shell->dirty = false;
  return stats.presented;
}

static void shell_handle_platform_event(bd_shell *shell, const bd_event *event) {
  switch (event->type) {
    case BD_EVENT_QUIT:
      shell->quit = true;
      break;
    case BD_EVENT_REDRAW:
      shell->dirty = true;
      break;
    case BD_EVENT_KEY:
      if (event->key != BD_KEY_POWER &&
          (shell->power_input_suspended || shell_power_asleep(shell))) {
        /* Do not accumulate an ordinary key across the sleep boundary. */
        break;
      }
      if (!shell->inputd_connected) {
        bd_input_state_feed(&shell->input, event->key, event->pressed,
                            monotonic_ms(), local_emit, shell);
      }
      break;
    default:
      break;
  }
}

static int shell_poll_timeout(bd_shell *shell) {
  int best = -1;
  bool asleep = shell_power_asleep(shell);
  if (shell->power_request != SHELL_POWER_REQUEST_NONE) {
    /* A queued sleep/wake/shutdown must run on the next loop turn. */
    return 0;
  }
  if (shell->power_key_down && !shell->power_long_fired && !asleep) {
    uint64_t now = shell_now(shell);
    uint64_t deadline = shell->power_down_ms + BD_SHELL_POWER_TAP_MS;
    uint64_t remaining = deadline > now ? deadline - now : 0;
    best = merge_timeout(best, (int)remaining);
  }
  best = merge_timeout(best, shell->session_next_ms);
  if (shell->plugins != NULL) {
    best = merge_timeout(best, bd_plugin_host_next_timeout(shell->plugins));
  }
  if (!shell->inputd_connected && shell->input_socket[0] != '\0') {
    uint64_t now = monotonic_ms();
    uint64_t deadline = 0;
    if (shell->inputd_fd >= 0) {
      deadline = shell->inputd_deadline_ms;
    } else {
      deadline = shell->inputd_retry_ms;
    }
    uint64_t remaining = deadline > now ? deadline - now : 0;
    if (remaining > 60000) {
      remaining = 60000;
    }
    best = merge_timeout(best, (int)remaining);
  }
  uint64_t pulse_deadline = 0;
  if (bd_haptics_next_deadline(shell->haptics, &pulse_deadline)) {
    uint64_t now = monotonic_ms();
    uint64_t remaining = pulse_deadline > now ? pulse_deadline - now : 0;
    if (remaining > 60000) {
      remaining = 60000;
    }
    best = merge_timeout(best, (int)remaining);
  }
  if (shell->led_pulse_active) {
    uint64_t now = monotonic_ms();
    uint64_t remaining =
        shell->led_pulse_off_ms > now ? shell->led_pulse_off_ms - now : 0;
    if (remaining > 60000) {
      remaining = 60000;
    }
    best = merge_timeout(best, (int)remaining);
  }
  if (asleep) {
    /*
     * No input tap/hold, battery, home or settings deadline may wake the loop
     * while asleep.  Only session/plugin/inputd service remains above.
     */
    return best;
  }
  if (!shell->inputd_connected) {
    uint64_t deadline = 0;
    if (bd_input_state_next_deadline(&shell->input, &deadline)) {
      uint64_t now = monotonic_ms();
      uint64_t remaining = deadline > now ? deadline - now : 0;
      if (remaining > 60000) {
        remaining = 60000;
      }
      best = merge_timeout(best, (int)remaining);
    }
  }
  if (shell->battery_next_ms != 0) {
    uint64_t now = shell_now(shell);
    uint64_t remaining = shell->battery_next_ms > now
                             ? shell->battery_next_ms - now
                             : 0;
    best = merge_timeout(best, (int)remaining);
  }
  if (shell->notice != NULL) {
    uint64_t deadline = bd_notice_next_deadline(shell->notice);
    if (deadline != 0) {
      uint64_t now = shell_now(shell);
      uint64_t remaining = deadline > now ? deadline - now : 0;
      best = merge_timeout(best, (int)remaining);
    }
  }
  if (shell->low_battery_shutdown_armed) {
    uint64_t now = shell_now(shell);
    uint64_t remaining = shell->low_battery_shutdown_ms > now
                             ? shell->low_battery_shutdown_ms - now
                             : 0;
    best = merge_timeout(best, (int)remaining);
  }
  if (shell->visible && shell->home != NULL) {
    uint64_t wake_at = 0;
    bool frame_needed = false;
    if (shell->settings_page != NULL &&
        bd_settings_is_open(shell->settings_page)) {
      frame_needed = bd_settings_needs_frame_until(shell->settings_page,
                                                   &wake_at);
    } else {
      frame_needed = bd_home_needs_frame_until(shell->home, &wake_at);
    }
    if (frame_needed) {
      uint64_t now = shell_now(shell);
      uint64_t remaining = wake_at > now ? wake_at - now : 0;
      /* A transition, decode or slider repeat keeps a 60 Hz deadline. */
      if (remaining > 16 || (shell->test_clock && remaining == 0)) {
        remaining = 16;
      }
      best = merge_timeout(best, (int)remaining);
    }
  }
  if (shell->dirty && shell->visible) {
    best = 0;
  }
  return best;
}

static int shell_build_fds(bd_shell *shell, struct pollfd *fds, int capacity,
                           int *kinds) {
  if (capacity <= 0) {
    return 0;
  }
  int count = 0;
  int platform_fd = bd_platform_event_fd(shell->platform);
  if (platform_fd >= 0) {
    fds[count].fd = platform_fd;
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    kinds[count] = 0;
    ++count;
  }
  if (shell->inputd_fd >= 0 && count < capacity) {
    fds[count].fd = shell->inputd_fd;
    fds[count].events = POLLIN;
    if (shell->inputd_connecting ||
        shell->inputd_sent < sizeof(BD_SHELL_INPUTD_SUBSCRIBE) - 1) {
      fds[count].events |= POLLOUT;
    }
    fds[count].revents = 0;
    kinds[count] = 1;
    ++count;
  }
  int session_fds[BD_SHELL_FD_MAX];
  int session_count = 0;
  if (shell->session != NULL) {
    session_count = bd_session_fds(shell->session, session_fds,
                                   BD_SHELL_FD_MAX);
  }
  for (int i = 0; i < session_count && count < capacity; ++i) {
    fds[count].fd = session_fds[i];
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    kinds[count] = 2;
    ++count;
  }
  int plugin_fds[BD_SHELL_FD_MAX];
  int plugin_count = 0;
  if (shell->plugins != NULL) {
    plugin_count = bd_plugin_host_fds(shell->plugins, plugin_fds,
                                      BD_SHELL_FD_MAX);
  }
  for (int i = 0; i < plugin_count && count < capacity; ++i) {
    fds[count].fd = plugin_fds[i];
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    kinds[count] = 3;
    ++count;
  }
  return count;
}

int bd_shell_iterate(bd_shell *shell, int timeout_ms) {
  if (shell == NULL || shell->quit) {
    return -1;
  }
  /* Execute a queued POWER request before ordinary service or a frame. */
  shell_power_service(shell);
  shell_service(shell);
  if (shell->quit) {
    return -1;
  }
  if (!shell->power_input_suspended && !shell_power_asleep(shell) &&
      shell->visible && shell->home != NULL) {
    uint64_t wake_at = 0;
    bool frame_needed = shell->settings_page != NULL &&
                                bd_settings_is_open(shell->settings_page)
                            ? bd_settings_needs_frame_until(shell->settings_page,
                                                            &wake_at)
                            : bd_home_needs_frame_until(shell->home, &wake_at);
    if (frame_needed) {
      shell->dirty = true;
    }
  }
  if (!shell->power_input_suspended && !shell_power_asleep(shell) &&
      shell->dirty && shell->visible) {
    bd_shell_render(shell);
  } else if (!shell_power_asleep(shell) && shell->dirty && !shell->visible) {
    shell->dirty = false;
  }

  int timeout = shell_poll_timeout(shell);
  if (timeout_ms >= 0 && (timeout < 0 || timeout_ms < timeout)) {
    timeout = timeout_ms;
  }

  struct pollfd fds[BD_SHELL_FD_MAX * 2 + 2];
  int kinds[BD_SHELL_FD_MAX * 2 + 2];
  int count = shell_build_fds(shell, fds, (int)(sizeof(fds) / sizeof(fds[0])),
                              kinds);
  int ready = poll(fds, (nfds_t)count, timeout);
  if (ready < 0) {
    if (errno == EINTR) {
      return 0;
    }
    return -1;
  }
  if (ready == 0) {
    return 0;
  }

  bool handled = false;
  for (int i = 0; i < count; ++i) {
    if (fds[i].revents == 0) {
      continue;
    }
    if (kinds[i] == 0) {
      bd_event event;
      while (bd_platform_pump_event(shell->platform, &event) > 0) {
        shell_handle_platform_event(shell, &event);
        handled = true;
        if (shell->quit) {
          return -1;
        }
      }
    } else if (kinds[i] == 1) {
      inputd_handle_poll(shell, fds[i].revents);
      handled = true;
    } else {
      handled = true;
    }
  }

  if (handled) {
    shell_power_service(shell);
    shell_service(shell);
    if (!shell->power_input_suspended && !shell_power_asleep(shell) &&
        shell->visible && shell->home != NULL) {
      uint64_t wake_at = 0;
      bool frame_needed =
          shell->settings_page != NULL &&
                  bd_settings_is_open(shell->settings_page)
              ? bd_settings_needs_frame_until(shell->settings_page, &wake_at)
              : bd_home_needs_frame_until(shell->home, &wake_at);
      if (frame_needed) {
        shell->dirty = true;
      }
    }
    if (!shell->power_input_suspended && !shell_power_asleep(shell) &&
        shell->dirty && shell->visible) {
      bd_shell_render(shell);
    } else if (!shell_power_asleep(shell) && shell->dirty && !shell->visible) {
      shell->dirty = false;
    }
  }
  return handled ? 1 : 0;
}

bool bd_shell_run_for(bd_shell *shell, int duration_ms) {
  if (shell == NULL) {
    return false;
  }
  uint64_t deadline = monotonic_ms() + (uint64_t)(duration_ms > 0 ? duration_ms : 0);
  for (;;) {
    if (shell->quit) {
      return false;
    }
    uint64_t now = monotonic_ms();
    if (now >= deadline) {
      return true;
    }
    uint64_t remaining = deadline - now;
    int cap = remaining > 60000 ? 60000 : (int)remaining;
    int result = bd_shell_iterate(shell, cap);
    if (result < 0) {
      return false;
    }
  }
}

static void shell_find_plugin_tile(bd_shell *shell, bd_gfx *gfx) {
  shell->plugin_tile = NULL;
  shell->plugin_tile_name[0] = '\0';
  shell->plugin_tile_span_w = 1;
  shell->plugin_tile_span_h = 1;
  if (shell->plugins == NULL) {
    return;
  }
  for (size_t i = 0; i < bd_plugin_host_count(shell->plugins); ++i) {
    bd_plugin *plugin = bd_plugin_host_at(shell->plugins, i);
    const bd_plugin_manifest *manifest =
        bd_plugin_host_manifest(shell->plugins, i);
    if (plugin == NULL || manifest == NULL ||
        (manifest->capabilities & BD_PLUGIN_CAP_TILE) == 0) {
      continue;
    }
    shell->plugin_tile = plugin;
    copy_text(shell->plugin_tile_name, sizeof(shell->plugin_tile_name),
              manifest->name[0] != '\0' ? manifest->name : "插件");
    shell->plugin_tile_span_w = manifest->tile.w > 1 ? 2 : 1;
    shell->plugin_tile_span_h = manifest->tile.h > 1 ? 2 : 1;
    shell->plugin_accent =
        bd_accent_palette_from(shell->home_style.accent_base, true);
    shell->plugin_fonts_loaded = bd_ui_fonts_load(&shell->plugin_fonts, gfx);
    if (!shell->plugin_fonts_loaded) {
      fprintf(stderr, "brickdesk: cannot load plugin tile fonts\n");
    }
    break;
  }
}

bd_shell *bd_shell_create(const bd_shell_config *config) {
  if (config == NULL || config->platform == NULL || config->gfx == NULL ||
      config->settings == NULL) {
    return NULL;
  }
  bd_shell *shell = (bd_shell *)calloc(1, sizeof(*shell));
  if (shell == NULL) {
    return NULL;
  }
  shell->platform = config->platform;
  shell->gfx = config->gfx;
  shell->settings = config->settings;
  shell->plugins = config->plugins;
  shell->verbose = config->verbose;
  shell->inputd_fd = -1;
  shell->inputd_backoff_ms = BD_SHELL_INPUTD_BACKOFF_MIN_MS;
  shell->visible = true;
  shell->dirty = true;
  shell->status_battery = -1;
  if (config->battery_root != NULL) {
    copy_text(shell->battery_root, sizeof(shell->battery_root),
              config->battery_root);
  }
  copy_text(shell->perf_socket, sizeof(shell->perf_socket),
            bd_perf_client_default_socket());

  const char *accent_name =
      bd_config_get_string(config->settings, "theme.accent", "persimmon");
  const char *accent_hex = bd_config_accent_hex(accent_name, true);
  bd_color accent = bd_color_rgb(224, 122, 95);
  if (accent_hex == NULL || !bd_color_from_hex(accent_hex, &accent)) {
    fprintf(stderr, "brickdesk: config: invalid theme accent '%s'\n",
            accent_name);
    accent = bd_color_rgb(224, 122, 95);
  }
  memset(&shell->home_style, 0, sizeof(shell->home_style));
  shell->home_style.accent_base = accent;
  shell->home_style.blur_enabled =
      bd_config_get_bool(config->settings, "ui.blur", true);
  shell->home_style.ambient_solid =
      strcmp(bd_config_get_string(config->settings, "theme.ambient", "baked"),
             "solid") == 0;
  shell->home_style.width = bd_gfx_width(config->gfx);
  shell->home_style.height = bd_gfx_height(config->gfx);
  shell->home = bd_home_create(config->gfx, &shell->home_style);
  if (shell->home == NULL) {
    fprintf(stderr, "brickdesk: cannot create the home page\n");
    free(shell);
    return NULL;
  }
  bd_home_set_animations(
      shell->home, bd_config_get_bool(config->settings, "ui.animations", true));
  if (config->attach_plugin_tile) {
    shell_find_plugin_tile(shell, config->gfx);
  }
  shell->notice = bd_notice_create(config->gfx);
  if (shell->notice == NULL) {
    fprintf(stderr, "brickdesk: notice layer unavailable\n");
  }

  shell_open_devices(shell, config);
  {
    bd_display_config display_config;
    memset(&display_config, 0, sizeof(display_config));
    display_config.sysfs_root = config->av_sysfs_root;
    bd_display_error display_error;
    bd_display_error_clear(&display_error);
    shell->display_ready =
        bd_display_probe(&shell->display, &display_config, &display_error) ==
        BD_DISPLAY_OK;
    if (!shell->display_ready && shell->verbose) {
      fprintf(stderr, "brickdesk: display enhancements: %s\n",
              display_error.message[0] != '\0'
                  ? display_error.message
                  : bd_display_result_name(display_error.code));
    } else if (shell->display_ready) {
      shell_display_apply(shell);
    }
  }
  {
    char device_error[192] = {0};
    shell->led = bd_led_create(config->settings, config->av_sysfs_root,
                               device_error, sizeof(device_error));
    if (shell->led != NULL && !bd_led_available(shell->led) &&
        shell->verbose) {
      fprintf(stderr, "brickdesk: led: %s\n",
              bd_led_unavailable_reason(shell->led));
    }
    device_error[0] = '\0';
    shell->haptics = bd_haptics_create(config->av_sysfs_root, device_error,
                                       sizeof(device_error));
    if (shell->haptics != NULL && !bd_haptics_available(shell->haptics) &&
        shell->verbose) {
      fprintf(stderr, "brickdesk: haptics: %s\n",
              bd_haptics_unavailable_reason(shell->haptics));
    }
    shell_led_refresh_internal(shell);
    shell_haptic_boot(shell);
  }
  shell->power_input_suspended = false;
  shell->power_request = SHELL_POWER_REQUEST_NONE;
  shell->power_brightness_known = false;
  if (!shell_power_create(shell, config)) {
    fprintf(stderr, "brickdesk: light sleep disabled\n");
  }
  bd_home_set_quick_backend(shell->home, &shell->home_quick);

  bd_settings_style settings_style;
  memset(&settings_style, 0, sizeof(settings_style));
  settings_style.accent_base = accent;
  settings_style.width = bd_gfx_width(config->gfx);
  settings_style.height = bd_gfx_height(config->gfx);
  shell->settings_page =
      bd_settings_create(config->gfx, config->settings, &settings_style);
  if (shell->settings_page == NULL) {
    fprintf(stderr, "brickdesk: settings page unavailable\n");
  } else {
    bd_settings_set_system(shell->settings_page, &shell->system);
    bd_settings_perf perf_backend = {.user = shell,
                                     .apply_global_profile = shell_perf_apply};
    bd_settings_set_perf(shell->settings_page, &perf_backend);
    bd_settings_set_plugins(shell->settings_page, shell->plugins);
    char system_error[128] = {0};
    if (!bd_settings_sync_system(shell->settings_page, system_error,
                                 sizeof(system_error)) &&
        shell->verbose) {
      fprintf(stderr, "brickdesk: settings: %s\n", system_error);
    }
    shell_refresh_storage(shell);
  }
  bd_config_set_change_callback(config->settings, shell_config_changed, shell);

  bd_battery_policy_config battery_config;
  long low_battery =
      bd_config_get_int(config->settings, "power.low_battery_percent", 5);
  if (low_battery < 0 || low_battery > 100) {
    low_battery = 5;
  }
  shell->shutdown_on_low_battery = bd_config_get_bool(
      config->settings, "power.shutdown_on_low_battery", true);
  shell_battery_policy_config((int)low_battery,
                              shell->shutdown_on_low_battery,
                              &battery_config);
  bd_battery_error battery_error;
  bd_battery_policy_output battery_output;
  bd_battery_policy_result battery_result = bd_battery_policy_init(
      &shell->battery_policy, &battery_config, shell_now(shell),
      &battery_output, &battery_error);
  shell->battery_policy_ready = battery_result == BD_BATTERY_POLICY_OK;
  shell->battery_next_ms =
      shell->battery_policy_ready ? shell_now(shell)
                                  : shell_now(shell) + BD_SHELL_BATTERY_RETRY_MS;
  shell->battery_opened = false;
  shell->battery_history = bd_battery_history_create();
  const char *data_dir = bd_config_dir(config->settings);
  if (shell->battery_history != NULL && data_dir != NULL &&
      data_dir[0] != '\0') {
    int written = snprintf(shell->battery_history_path,
                           sizeof(shell->battery_history_path),
                           "%s/battery-history.csv", data_dir);
    if (written <= 0 ||
        (size_t)written >= sizeof(shell->battery_history_path)) {
      shell->battery_history_path[0] = '\0';
    } else {
      bd_battery_history_load(shell->battery_history,
                              shell->battery_history_path);
    }
  }
  if (shell->settings_page != NULL) {
    bd_settings_set_battery_history(shell->settings_page,
                                    shell->battery_history);
  }
  shell->app_history = bd_app_history_create();
  if (shell->app_history != NULL && data_dir != NULL && data_dir[0] != '\0') {
    int written = snprintf(shell->app_history_path,
                           sizeof(shell->app_history_path), "%s/app-history.txt",
                           data_dir);
    if (written <= 0 ||
        (size_t)written >= sizeof(shell->app_history_path)) {
      shell->app_history_path[0] = '\0';
    } else {
      bd_app_history_load(shell->app_history, shell->app_history_path);
      for (size_t i = 0; i < bd_app_history_count(shell->app_history); ++i) {
        const bd_app_history_entry *entry =
            bd_app_history_at(shell->app_history, i);
        if (entry != NULL && entry->favorite) {
          bd_home_set_favorite(shell->home, entry->id, true);
        }
      }
    }
  }
  if (!shell->battery_policy_ready) {
    fprintf(stderr, "brickdesk: battery policy unavailable: %s\n",
            battery_error.message[0] != '\0' ? battery_error.message
                                              : bd_battery_policy_result_name(
                                                    battery_result));
  }

  bd_home_status status;
  memset(&status, 0, sizeof(status));
  copy_text(shell->clock, sizeof(shell->clock), "22:26");
  if (getenv("BRICKDESK_HEADLESS") == NULL) {
    time_t wall = time(NULL);
    struct tm local;
    if (wall != (time_t)-1 && localtime_r(&wall, &local) != NULL) {
      strftime(shell->clock, sizeof(shell->clock), "%H:%M", &local);
    }
  }
  long max_background =
      bd_config_get_int(config->settings, "session.max_background", 3);
  long memory_budget =
      bd_config_get_int(config->settings, "session.memory_budget_mb", 560);
  if (memory_budget < 0 || memory_budget > 4096) {
    memory_budget = 560;
  }
  shell->memory_budget_mb = (int)memory_budget;
  shell->memory_budget_next_ms = shell_now(shell);
  status.clock = shell->clock;
  status.max_background = (int)max_background;
  status.battery_percent = -1;
  bd_home_set_status(shell->home, &status);
  shell_refresh_status(shell, true);
  shell_battery_sample(shell, shell_now(shell));
  shell_publish_status(shell, true);
  shell_home_quick_refresh(shell);

  bd_hotkey_table_load(&shell->hotkeys, config->settings);
  shell->hotkey_table = &shell->hotkeys;
  if (shell->verbose && (shell->hotkeys.diagnostic_count > 0 ||
                         shell->hotkeys.conflict_count > 0)) {
    fprintf(stderr, "brickdesk: %zu hotkey diagnostic(s), %zu conflict(s)\n",
            shell->hotkeys.diagnostic_count, shell->hotkeys.conflict_count);
  }
  bd_input_state_init(&shell->input, &shell->hotkeys, "global",
                      (int)bd_config_get_int(config->settings,
                                             "input.double_tap_ms", 300));
  bd_input_state_init(&shell->test_input, &shell->hotkeys, "global",
                      (int)bd_config_get_int(config->settings,
                                             "input.double_tap_ms", 300));

  shell_load_apps(shell, config->apps_dir);

  bd_session_config session_config;
  memset(&session_config, 0, sizeof(session_config));
  session_config.socket_dir = config->session_socket_dir;
  session_config.frame_dir = config->session_frame_dir;
  session_config.state_root = config->session_state_root;
  session_config.max_background =
      (int)bd_config_get_int(config->settings, "session.max_background", 3);
  session_config.state_quota_mb =
      (int)bd_config_get_int(config->settings, "session.state_quota_mb", 512);
  session_config.on_event = on_session_event;
  session_config.event_user = shell;
  char error[256] = {0};
  shell->session = bd_session_create(&session_config, error, sizeof(error));
  if (shell->session == NULL) {
    fprintf(stderr, "brickdesk: session: %s\n", error);
  }

  copy_text(shell->input_socket, sizeof(shell->input_socket),
            config->input_socket != NULL ? config->input_socket
                                         : preferred_input_socket());
  inputd_service(shell, monotonic_ms());
  if (!shell->inputd_connected && shell->verbose) {
    fprintf(stderr,
            "brickdesk: inputd not reachable at %s; using SDL keys and "
            "retrying\n",
            shell->input_socket);
  }
  if (config->menu_shim != NULL && file_readable(config->menu_shim)) {
    copy_text(shell->menu_shim, sizeof(shell->menu_shim), config->menu_shim);
    shell->menu_shim_checked = true;
  }
  return shell;
}

void bd_shell_destroy(bd_shell *shell) {
  if (shell == NULL) {
    return;
  }
  if (shell->power != NULL) {
    if (bd_power_current_state(shell->power) != BD_POWER_STATE_IDLE) {
      bd_power_report report;
      memset(&report, 0, sizeof(report));
      bd_power_result result = bd_power_wake(shell->power, &report);
      if (result != BD_POWER_OK) {
        shell_power_log(shell, "shutdown wake", result, &report);
      }
    }
    bd_power_destroy(shell->power);
    shell->power = NULL;
  }
  if (shell->session != NULL) {
    bd_session_destroy(shell->session);
  }
  inputd_disconnect(shell, "shutdown");
  bd_haptics_destroy(shell->haptics);
  shell->haptics = NULL;
  bd_led_destroy(shell->led);
  shell->led = NULL;
  if (shell->settings_page != NULL) {
    bd_config_set_change_callback(shell->settings, NULL, NULL);
    bd_settings_destroy(shell->settings_page);
    shell->settings_page = NULL;
  }
  if (shell->notice != NULL) {
    bd_notice_destroy(shell->notice);
    shell->notice = NULL;
  }
  if (shell->battery_history != NULL) {
    if (shell->battery_history_path[0] != '\0') {
      bd_battery_history_save(shell->battery_history,
                              shell->battery_history_path);
    }
    bd_battery_history_destroy(shell->battery_history);
    shell->battery_history = NULL;
  }
  if (shell->app_history != NULL) {
    shell_app_history_save(shell);
    bd_app_history_destroy(shell->app_history);
    shell->app_history = NULL;
  }
  if (shell->home != NULL) {
    bd_home_set_surface_visible(shell->home, false);
  }
  if (shell->plugin_tile != NULL) {
    bd_plugin_tile_release(shell->plugin_tile, shell->gfx);
    shell->plugin_tile = NULL;
  }
  if (shell->plugin_fonts_loaded) {
    bd_ui_fonts_free(&shell->plugin_fonts);
    shell->plugin_fonts_loaded = false;
  }
  free(shell->runtime);
  if (shell->apps != NULL) {
    bd_app_list_destroy(shell->apps);
  }
  bd_home_destroy(shell->home);
  bd_av_audio_close(&shell->av_audio);
  free(shell);
}

bool bd_shell_visible(const bd_shell *shell) {
  return shell != NULL && shell->visible;
}

bool bd_shell_quit_requested(const bd_shell *shell) {
  return shell == NULL || shell->quit;
}

bool bd_shell_has_pending_images(const bd_shell *shell) {
  return shell != NULL && bd_home_has_pending_images(shell->home);
}

bool bd_shell_hint_visible(const bd_shell *shell) {
  return shell != NULL && bd_home_hint_visible(shell->home);
}

uint64_t bd_shell_present_count(const bd_shell *shell) {
  return shell != NULL ? shell->presents : 0;
}

const char *bd_shell_input_source(const bd_shell *shell) {
  return shell != NULL && shell->inputd_connected ? "inputd" : "sdl";
}

size_t bd_shell_app_count(const bd_shell *shell) {
  return shell != NULL && shell->apps != NULL ? bd_app_list_count(shell->apps)
                                              : 0;
}

const bd_app *bd_shell_app_at(const bd_shell *shell, size_t index) {
  return shell != NULL && shell->apps != NULL
             ? bd_app_list_at(shell->apps, index)
             : NULL;
}

bd_home *bd_shell_home(bd_shell *shell) {
  return shell != NULL ? shell->home : NULL;
}

bd_settings *bd_shell_settings(bd_shell *shell) {
  return shell != NULL ? shell->settings_page : NULL;
}

const bd_battery_history *bd_shell_battery_history(const bd_shell *shell) {
  return shell != NULL ? shell->battery_history : NULL;
}

bool bd_shell_notice(bd_shell *shell, bd_notice_level level,
                     const char *text, uint32_t timeout_ms) {
  if (shell == NULL || shell->notice == NULL) {
    return false;
  }
  if (!bd_notice_show(shell->notice, level, text, timeout_ms,
                      shell_now(shell))) {
    return false;
  }
  if (shell->visible) {
    shell->dirty = true;
  }
  return true;
}

bool bd_shell_confirm(bd_shell *shell, const char *title, const char *text,
                      uint32_t timeout_ms, bd_notice_confirm_fn on_done,
                      void *user) {
  if (shell == NULL || shell->notice == NULL) {
    return false;
  }
  if (!bd_notice_confirm(shell->notice, title, text, timeout_ms, on_done, user,
                         shell_now(shell))) {
    return false;
  }
  if (shell->visible) {
    shell->dirty = true;
  }
  return true;
}

bool bd_shell_confirm_open(const bd_shell *shell) {
  return shell != NULL && shell->notice != NULL &&
         bd_notice_confirm_open(shell->notice);
}

void bd_shell_test_key(bd_shell *shell, bd_key key, bool pressed,
                       uint64_t time_ms) {
  if (shell == NULL || shell->power_swallow) {
    return;
  }
  if (key != BD_KEY_POWER &&
      (shell->power_input_suspended || shell_power_asleep(shell))) {
    return;
  }
  bd_input_state_feed(&shell->test_input, key, pressed, time_ms, local_emit,
                      shell);
}

void bd_shell_test_advance(bd_shell *shell, uint64_t time_ms) {
  if (shell == NULL || shell->power_swallow ||
      shell->power_input_suspended || shell_power_asleep(shell)) {
    return;
  }
  shell->test_clock = true;
  shell->test_now_ms = time_ms;
  bd_input_state_advance(&shell->test_input, time_ms, local_emit, shell);
}

bool bd_shell_test_battery_policy(const bd_shell *shell, uint64_t *next_ms,
                                  bool *policy_ready, int *percent) {
  if (shell == NULL) {
    return false;
  }
  if (next_ms != NULL) {
    *next_ms = shell->battery_next_ms;
  }
  if (policy_ready != NULL) {
    *policy_ready = shell->battery_policy_ready;
  }
  if (percent != NULL) {
    *percent = shell->status_battery;
  }
  return true;
}

bool bd_shell_test_power(const bd_shell *shell, bd_power_state *state,
                         bool *sleeping, bool *input_suspended) {
  if (shell == NULL) {
    return false;
  }
  if (state != NULL) {
    *state = shell->power != NULL ? bd_power_current_state(shell->power)
                                  : BD_POWER_STATE_UNKNOWN;
  }
  if (sleeping != NULL) {
    *sleeping = shell_power_asleep(shell);
  }
  if (input_suspended != NULL) {
    *input_suspended = shell->power_input_suspended;
  }
  return true;
}

bd_session *bd_shell_test_session(bd_shell *shell) {
  return shell != NULL ? shell->session : NULL;
}

bool bd_shell_test_launch(bd_shell *shell, const char *app_id) {
  return shell != NULL ? shell_launch_app(shell, app_id) : false;
}

bool bd_shell_led_refresh(bd_shell *shell) {
  if (shell == NULL || shell->led == NULL) {
    return false;
  }
  shell->led_profile_valid = false;
  shell_led_refresh_internal(shell);
  return bd_led_available(shell->led);
}

bool bd_shell_led_sleep_pulse(bd_shell *shell) {
  if (shell == NULL || shell->led == NULL) {
    return false;
  }
  char error[192] = {0};
  if (!bd_led_apply_profile(shell->led, BD_LED_PROFILE_SLEEP, error,
                            sizeof(error))) {
    if (shell->verbose) {
      fprintf(stderr, "brickdesk: led sleep: %s\n",
              error[0] != '\0' ? error : "apply failed");
    }
    return false;
  }
  shell->led_profile = BD_LED_PROFILE_SLEEP;
  shell->led_profile_valid = true;
  shell->led_pulse_active = true;
  shell->led_pulse_off_ms =
      shell_now(shell) + bd_led_sleep_pulse_ms(shell->led);
  return true;
}

void bd_shell_led_wake(bd_shell *shell) {
  if (shell == NULL) {
    return;
  }
  shell->led_pulse_active = false;
  shell->led_pulse_off_ms = 0;
  shell->led_profile_valid = false;
  shell_led_refresh_internal(shell);
}

bool bd_shell_haptic(bd_shell *shell, int strength, int duration_ms) {
  if (shell == NULL || shell->haptics == NULL) {
    return false;
  }
  if (!bd_config_get_bool(shell->settings, "input.haptics", true)) {
    return false;
  }
  return bd_haptics_play(shell->haptics, strength, duration_ms,
                         shell_now(shell));
}
