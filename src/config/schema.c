#include "config_internal.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BD_CONFIG_TEXT_MAX 256

static const char *const g_theme_modes[] = {"dark", NULL};
static const char *const g_ambient_modes[] = {"baked", "solid", NULL};
static const char *const g_languages[] = {"zh-CN", "en-US", NULL};
static const char *const g_time_formats[] = {"24h", "12h", NULL};
static const char *const g_quick_resume[] = {"auto", "always", "never", NULL};
static const char *const g_governors[] = {"performance", "schedutil", "ondemand",
                                          "powersave", NULL};
static const char *const g_power_long_press[] = {"shutdown", "reboot", "none",
                                                 NULL};
static const char *const g_accents[] = {"persimmon", "coral", "teal", "blue",
                                        "green", "violet", NULL};
static const char *const g_led_colors[] = {
    "white", "red", "orange", "yellow", "green",
    "cyan",  "blue", "violet", "magenta", "off", NULL};
static const char *const g_usb_modes[] = {"host", "dock", "data", NULL};
static const char *const g_remap_keys[] = {"A", "B", "X", "Y", NULL};
static const char *const g_timezones[] = {
    "UTC",           "Asia/Shanghai", "Asia/Tokyo",    "Asia/Seoul",
    "Asia/Singapore", "Europe/London", "Europe/Berlin", "America/New_York",
    "America/Los_Angeles", NULL};
static const char *const g_fn_actions[] = {
    "none", "mute", "led_off", "performance", "powersave", "turbo",
    "dpad_joystick", NULL};
static const char *const g_input_actions[] = {
    "none", "screenshot", "settings", "quick", "recent", "return_home",
    "menu", NULL};
static const char *const g_led_effects[] = {
    "linear",  "breathe", "interval_breathe", "static", "blink1",
    "blink2",  "blink3",  "rainbow",          "twinkle", "fire",
    "glitter", "neonglow", "firefly",         "aurora",  "reactive",
    NULL};

const bd_config_spec bd_config_specs[] = {
    /* theme */
    {"theme", "mode", BD_CONFIG_ENUM, "dark", 0, 0, g_theme_modes, 0, 0, 0},
    {"theme", "accent", BD_CONFIG_ACCENT, "persimmon", 0, 0, g_accents, 0, 0, 0},
    {"theme", "ambient", BD_CONFIG_ENUM, "baked", 0, 0, g_ambient_modes, 0, 0, 0},

    /* ui */
    {"ui", "language", BD_CONFIG_ENUM, "zh-CN", 0, 0, g_languages, 0, 0, 0},
    {"ui", "animations", BD_CONFIG_BOOL, "true", 0, 0, NULL, 0, 0, 0},
    {"ui", "blur", BD_CONFIG_BOOL, "true", 0, 0, NULL, 0, 0, 0},
    {"ui", "time_format", BD_CONFIG_ENUM, "24h", 0, 0, g_time_formats, 0, 0, 0},
    {"ui", "clock_seconds", BD_CONFIG_BOOL, "false", 0, 0, NULL, 0, 0, 0},
    {"ui", "cover_cache_mb", BD_CONFIG_INT, "96", 8, 512, NULL, 0, 0, 0},

    /* display */
    {"display", "width", BD_CONFIG_INT, "1024", 320, 4096, NULL, 0, 0, 0},
    {"display", "height", BD_CONFIG_INT, "768", 240, 4096, NULL, 0, 0, 0},
    {"display", "refresh_hz", BD_CONFIG_INT, "60", 30, 120, NULL, 0, 0, 0},
    {"display", "brightness", BD_CONFIG_INT, "70", 0, 100, NULL, 0, 0, 0},
    {"display", "color_temperature", BD_CONFIG_INT, "0", -200, 200, NULL, 0, 0,
     0},
    {"display", "contrast", BD_CONFIG_INT, "50", 10, 100, NULL, 0, 0, 0},
    {"display", "saturation", BD_CONFIG_INT, "50", 0, 100, NULL, 0, 0, 0},
    {"display", "exposure", BD_CONFIG_INT, "50", 10, 100, NULL, 0, 0, 0},
    {"display", "white_point", BD_CONFIG_BOOL, "true", 0, 0, NULL, 0, 0, 0},
    {"display", "white_point_red", BD_CONFIG_INT, "100", 0, 200, NULL, 0, 0, 0},
    {"display", "white_point_green", BD_CONFIG_INT, "94", 0, 200, NULL, 0, 0, 0},
    {"display", "white_point_blue", BD_CONFIG_INT, "54", 0, 200, NULL, 0, 0, 0},

    /* audio */
    {"audio", "volume", BD_CONFIG_INT, "70", 0, 100, NULL, 0, 0, 0},
    {"audio", "mute", BD_CONFIG_BOOL, "false", 0, 0, NULL, 0, 0, 0},
    {"audio", "channel_swap", BD_CONFIG_BOOL, "false", 0, 0, NULL, 0, 0, 0},
    {"audio", "speaker_gain", BD_CONFIG_INT, "100", 0, 100, NULL, 0, 0, 0},
    {"audio", "headphone_gain", BD_CONFIG_INT, "100", 0, 100, NULL, 0, 0, 0},
    {"audio", "mic", BD_CONFIG_BOOL, "false", 0, 0, NULL, 0, 0, 0},

    /* input */
    {"input", "repeat_delay_ms", BD_CONFIG_INT, "400", 0, 2000, NULL, 0, 0, 0},
    {"input", "repeat_interval_ms", BD_CONFIG_INT, "80", 10, 1000, NULL, 0, 0, 0},
    {"input", "chord_window_ms", BD_CONFIG_INT, "50", 10, 500, NULL, 0, 0, 0},
    {"input", "long_press_ms", BD_CONFIG_INT, "500", 100, 3000, NULL, 0, 0, 0},
    {"input", "double_tap_ms", BD_CONFIG_INT, "300", 100, 1000, NULL, 0, 0, 0},
    {"input", "haptics", BD_CONFIG_BOOL, "true", 0, 0, NULL, 0, 0, 0},
    {"input", "haptic_strength", BD_CONFIG_INT, "60", 0, 100, NULL, 0, 0, 0},
    {"input", "stick_deadzone", BD_CONFIG_INT, "8", 0, 30, NULL, 0, 0, 0},
    {"input", "remap_a", BD_CONFIG_ENUM, "A", 0, 0, g_remap_keys, 0, 0, 0},
    {"input", "remap_b", BD_CONFIG_ENUM, "B", 0, 0, g_remap_keys, 0, 0, 0},
    {"input", "remap_x", BD_CONFIG_ENUM, "X", 0, 0, g_remap_keys, 0, 0, 0},
    {"input", "remap_y", BD_CONFIG_ENUM, "Y", 0, 0, g_remap_keys, 0, 0, 0},
    {"input", "haptic_duration_ms", BD_CONFIG_INT, "35", 5, 500, NULL, 0, 0, 0},
    {"input", "fn_switch_action", BD_CONFIG_ENUM, "mute", 0, 0, g_fn_actions, 0,
     0, 0},
    {"input", "f1_action", BD_CONFIG_ENUM, "none", 0, 0, g_input_actions, 0, 0,
     0},
    {"input", "f2_action", BD_CONFIG_ENUM, "none", 0, 0, g_input_actions, 0, 0,
     0},
    {"input", "home_action", BD_CONFIG_ENUM, "recent", 0, 0, g_input_actions, 0,
     0, 0},
    {"input", "f1_app", BD_CONFIG_STRING, "", 0, 0, NULL, 64, 0, 0},
    {"input", "f2_app", BD_CONFIG_STRING, "", 0, 0, NULL, 64, 0, 0},
    {"input", "dpad_to_joystick", BD_CONFIG_BOOL, "false", 0, 0, NULL, 0, 0, 0},
    {"input", "turbo", BD_CONFIG_BOOL, "false", 0, 0, NULL, 0, 0, 0},

    /* led: Brick Pro vendor zones (m, lr, f1, f2, rear) */
    {"led", "enabled", BD_CONFIG_BOOL, "true", 0, 0, NULL, 0, 0, 0},
    {"led", "duration_ms", BD_CONFIG_INT, "1000", 100, 60000, NULL, 0, 0, 0},
    {"led", "indicator_brightness", BD_CONFIG_INT, "100", 0, 100, NULL, 0, 0, 0},
    {"led", "ambient", BD_CONFIG_BOOL, "false", 0, 0, NULL, 0, 0, 0},
    {"led", "ambient_brightness", BD_CONFIG_INT, "100", 0, 100, NULL, 0, 0, 0},
    {"led.m", "brightness", BD_CONFIG_INT, "100", 0, 100, NULL, 0, 0, 0},
    {"led.m", "color", BD_CONFIG_ENUM, "white", 0, 0, g_led_colors, 0, 0, 0},
    {"led.m", "effect", BD_CONFIG_ENUM, "static", 0, 0, g_led_effects, 0, 0, 0},
    {"led.lr", "brightness", BD_CONFIG_INT, "100", 0, 100, NULL, 0, 0, 0},
    {"led.lr", "color", BD_CONFIG_ENUM, "white", 0, 0, g_led_colors, 0, 0, 0},
    {"led.lr", "effect", BD_CONFIG_ENUM, "static", 0, 0, g_led_effects, 0, 0, 0},
    {"led.f1", "brightness", BD_CONFIG_INT, "100", 0, 100, NULL, 0, 0, 0},
    {"led.f1", "color", BD_CONFIG_ENUM, "white", 0, 0, g_led_colors, 0, 0, 0},
    {"led.f1", "effect", BD_CONFIG_ENUM, "static", 0, 0, g_led_effects, 0, 0, 0},
    {"led.f2", "color", BD_CONFIG_ENUM, "white", 0, 0, g_led_colors, 0, 0, 0},
    {"led.f2", "effect", BD_CONFIG_ENUM, "static", 0, 0, g_led_effects, 0, 0, 0},
    {"led.rear", "brightness", BD_CONFIG_INT, "100", 0, 100, NULL, 0, 0, 0},
    {"led.rear", "color", BD_CONFIG_ENUM, "white", 0, 0, g_led_colors, 0, 0, 0},
    {"led.rear", "effect", BD_CONFIG_ENUM, "static", 0, 0, g_led_effects, 0, 0, 0},
    {"led.app.*", "ambient", BD_CONFIG_BOOL, "true", 0, 0, NULL, 0, 0, 0},

    /* power */
    {"power", "sleep_after", BD_CONFIG_DURATION, "60s", 0, 86400000, NULL, 0,
     1000, 0},
    {"power", "deep_sleep_after", BD_CONFIG_DURATION, "30s", 0, 86400000, NULL,
     0, 1000, 0},
    {"power", "long_press_action", BD_CONFIG_ENUM, "shutdown", 0, 0,
     g_power_long_press, 0, 0, 0},
    {"power", "safe_shutdown", BD_CONFIG_BOOL, "true", 0, 0, NULL, 0, 0, 0},
    {"power", "low_battery_percent", BD_CONFIG_INT, "5", 0, 100, NULL, 0, 0, 0},
    {"power", "shutdown_on_low_battery", BD_CONFIG_BOOL, "true", 0, 0, NULL, 0,
     0, 0},

    /* net */
    {"net", "wifi_enabled", BD_CONFIG_BOOL, "true", 0, 0, NULL, 0, 0, 0},
    {"net", "hotspot_enabled", BD_CONFIG_BOOL, "false", 0, 0, NULL, 0, 0, 0},
    {"net", "hotspot_ssid", BD_CONFIG_STRING, "BrickDesk", 0, 0, NULL, 32, 0, 0},
    {"net", "hotspot_psk", BD_CONFIG_STRING, "brickdesk", 0, 0, NULL, 63, 0, 0},

    /* bt */
    {"bt", "enabled", BD_CONFIG_BOOL, "true", 0, 0, NULL, 0, 0, 0},

    /* time */
    {"time", "ntp", BD_CONFIG_BOOL, "true", 0, 0, NULL, 0, 0, 0},
    {"time", "timezone", BD_CONFIG_ENUM, "UTC", 0, 0, g_timezones, 0, 0, 0},

    /* share */
    {"share", "ssh", BD_CONFIG_BOOL, "false", 0, 0, NULL, 0, 0, 0},
    {"share", "ftp", BD_CONFIG_BOOL, "false", 0, 0, NULL, 0, 0, 0},
    {"share", "samba", BD_CONFIG_BOOL, "false", 0, 0, NULL, 0, 0, 0},
    {"share", "adb", BD_CONFIG_BOOL, "false", 0, 0, NULL, 0, 0, 0},
    {"share", "usb_mode", BD_CONFIG_ENUM, "host", 0, 0, g_usb_modes, 0, 0, 0},

    /* plugins */
    {"plugins", "enabled", BD_CONFIG_BOOL, "true", 0, 0, NULL, 0, 0, 0},
    {"plugins", "dir", BD_CONFIG_STRING, "", 0, 0, NULL, 512, 0, 0},
    {"plugins", "max_total_rss_mb", BD_CONFIG_INT, "128", 16, 1024, NULL, 0, 0, 0},
    {"plugins", "idle_stop", BD_CONFIG_DURATION, "60s", 0, 86400000, NULL, 0,
     1000, 0},
    {"plugins", "autostart_delay", BD_CONFIG_DURATION, "2s", 0, 600000, NULL, 0,
     1000, 0},

    /* session */
    {"session", "state_quota_mb", BD_CONFIG_INT, "512", 64, 4096, NULL, 0, 0, 0},
    {"session", "max_background", BD_CONFIG_INT, "3", 1, 8, NULL, 0, 0, 0},
    {"session", "memory_budget_mb", BD_CONFIG_INT, "560", 128, 1024, NULL, 0, 0,
     0},
    {"session", "quick_resume", BD_CONFIG_ENUM, "auto", 0, 0, g_quick_resume, 0,
     0, 0},

    /* perf */
    {"perf", "global_profile", BD_CONFIG_STRING, "default", 0, 0, NULL, 64, 0,
     0},

    /* [hotkey.<id>] */
    {"hotkey.*", "combo", BD_CONFIG_COMBO, NULL, 0, 0, NULL, 128, 0,
     BD_CONFIG_SPEC_REQUIRED},
    {"hotkey.*", "hold_ms", BD_CONFIG_INT, "500", 50, 5000, NULL, 0, 0, 0},
    {"hotkey.*", "window_ms", BD_CONFIG_INT, "50", 10, 500, NULL, 0, 0, 0},
    {"hotkey.*", "sequence_ms", BD_CONFIG_INT, "800", 100, 5000, NULL, 0, 0, 0},
    {"hotkey.*", "action", BD_CONFIG_STRING, NULL, 0, 0, NULL, 64, 0,
     BD_CONFIG_SPEC_REQUIRED},
    {"hotkey.*", "scope", BD_CONFIG_STRING, "global", 0, 0, NULL, 64, 0, 0},
    {"hotkey.*", "suppress", BD_CONFIG_BOOL, "false", 0, 0, NULL, 0, 0, 0},
    {"hotkey.*", "confirm", BD_CONFIG_BOOL, "false", 0, 0, NULL, 0, 0, 0},
    {"hotkey.*", "repeat", BD_CONFIG_BOOL, "false", 0, 0, NULL, 0, 0, 0},
    {"hotkey.*", "repeat_ms", BD_CONFIG_INT, "100", 20, 1000, NULL, 0, 0, 0},
    {"hotkey.*", "plugin", BD_CONFIG_STRING, "", 0, 0, NULL, 64, 0, 0},
    {"hotkey.*", "action_id", BD_CONFIG_STRING, "", 0, 0, NULL, 64, 0, 0},

    /* [profile.<name>] */
    {"profile.*", "inherit", BD_CONFIG_STRING, NULL, 0, 0, NULL, 64, 0, 0},
    {"profile.*", "governor", BD_CONFIG_ENUM, NULL, 0, 0, g_governors, 0, 0, 0},
    {"profile.*", "min_khz", BD_CONFIG_INT, NULL, 0, 3000000, NULL, 0, 0, 0},
    {"profile.*", "max_khz", BD_CONFIG_INT, NULL, 0, 3000000, NULL, 0, 0, 0},
    {"profile.*", "online_cores", BD_CONFIG_INT, NULL, 2, 4, NULL, 0, 0, 0},
    {"profile.*", "swappiness", BD_CONFIG_INT, NULL, 0, 100, NULL, 0, 0, 0},

    /* [app.<id>] */
    {"app.*", "profile", BD_CONFIG_STRING, NULL, 0, 0, NULL, 64, 0,
     BD_CONFIG_SPEC_REQUIRED},

    /* [core.<name>] */
    {"core.*", "inherit", BD_CONFIG_STRING, NULL, 0, 0, NULL, 64, 0, 0},
    {"core.*", "governor", BD_CONFIG_ENUM, NULL, 0, 0, g_governors, 0, 0, 0},
    {"core.*", "min_khz", BD_CONFIG_INT, NULL, 0, 3000000, NULL, 0, 0, 0},
    {"core.*", "max_khz", BD_CONFIG_INT, NULL, 0, 3000000, NULL, 0, 0, 0},
    {"core.*", "online_cores", BD_CONFIG_INT, NULL, 2, 4, NULL, 0, 0, 0},
    {"core.*", "swappiness", BD_CONFIG_INT, NULL, 0, 100, NULL, 0, 0, 0},

    /* [device.<name>]: any logical key name maps to an evdev code/axis. */
    {"device.*", "*", BD_CONFIG_STRING, NULL, 0, 0, NULL, 64, 0, 0},
};

const size_t bd_config_spec_count =
    sizeof(bd_config_specs) / sizeof(bd_config_specs[0]);

static bool ascii_equal_ci(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    unsigned char ca = (unsigned char)*a;
    unsigned char cb = (unsigned char)*b;
    if (ca >= 'A' && ca <= 'Z') {
      ca = (unsigned char)(ca - 'A' + 'a');
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb = (unsigned char)(cb - 'A' + 'a');
    }
    if (ca != cb) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

static void set_error(char *error, size_t error_size, const char *fmt, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  vsnprintf(error, error_size, fmt, args);
  va_end(args);
}

static bool parse_bool(const char *value, bool *out) {
  static const struct {
    const char *text;
    bool value;
  } names[] = {
      {"true", true},  {"false", false}, {"yes", true}, {"no", false},
      {"1", true},     {"0", false},     {"on", true},  {"off", false},
  };
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    if (ascii_equal_ci(value, names[i].text)) {
      *out = names[i].value;
      return true;
    }
  }
  return false;
}

static bool parse_int(const char *value, long *out) {
  if (value == NULL || *value == '\0') {
    return false;
  }
  errno = 0;
  char *end = NULL;
  long parsed = strtol(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0') {
    return false;
  }
  *out = parsed;
  return true;
}

static bool parse_duration_value(const char *value, int default_unit_ms,
                                 long *out_ms, char *error,
                                 size_t error_size) {
  if (value == NULL || *value == '\0') {
    set_error(error, error_size, "expected a duration such as 180s or 500ms");
    return false;
  }
  errno = 0;
  char *end = NULL;
  long number = strtol(value, &end, 10);
  if (errno != 0 || end == value) {
    set_error(error, error_size, "expected a duration such as 180s or 500ms");
    return false;
  }
  long unit_ms = default_unit_ms > 0 ? default_unit_ms : 1;
  if (*end != '\0') {
    if (strcmp(end, "ms") == 0) {
      unit_ms = 1;
    } else if (strcmp(end, "s") == 0) {
      unit_ms = 1000;
    } else if (strcmp(end, "min") == 0) {
      unit_ms = 60000;
    } else {
      set_error(error, error_size,
                "unknown duration unit '%s' (use ms, s or min)", end);
      return false;
    }
  }
  if (number < 0) {
    set_error(error, error_size, "duration must not be negative");
    return false;
  }
  if (number > LONG_MAX / unit_ms) {
    set_error(error, error_size, "duration is too large");
    return false;
  }
  *out_ms = number * unit_ms;
  return true;
}

static bool normalize_enum(const bd_config_spec *spec, const char *value,
                           char *out, size_t out_size, char *error,
                           size_t error_size) {
  /*
   * The desktop is dark-only.  Keep old local.ini files working by accepting
   * the retired light/auto values as aliases for dark instead of emitting a
   * diagnostic and leaving the retired value effective.
   */
  if (strcmp(spec->section, "theme") == 0 && strcmp(spec->key, "mode") == 0 &&
      (ascii_equal_ci(value, "light") || ascii_equal_ci(value, "auto"))) {
    snprintf(out, out_size, "dark");
    return true;
  }
  for (size_t i = 0; spec->enum_values[i] != NULL; ++i) {
    if (ascii_equal_ci(value, spec->enum_values[i])) {
      snprintf(out, out_size, "%s", spec->enum_values[i]);
      return true;
    }
  }
  char allowed[128] = "";
  size_t used = 0;
  for (size_t i = 0; spec->enum_values[i] != NULL; ++i) {
    int written = snprintf(allowed + used, sizeof(allowed) - used, "%s%s",
                           i == 0 ? "" : ", ", spec->enum_values[i]);
    if (written < 0 || (size_t)written >= sizeof(allowed) - used) {
      break;
    }
    used += (size_t)written;
  }
  set_error(error, error_size, "must be one of: %s", allowed);
  return false;
}

static bool normalize_accent(const bd_config_spec *spec, const char *value,
                             char *out, size_t out_size, char *error,
                             size_t error_size) {
  size_t length = strlen(value);
  if (length == 7 && value[0] == '#') {
    for (size_t i = 1; i < length; ++i) {
      if (!isxdigit((unsigned char)value[i])) {
        set_error(error, error_size,
                  "expected #RRGGBB (hex digits), or a preset name");
        return false;
      }
    }
    for (size_t i = 0; i < length; ++i) {
      out[i] = (char)tolower((unsigned char)value[i]);
    }
    out[length] = '\0';
    return true;
  }
  if (value[0] == '#') {
    set_error(error, error_size, "expected #RRGGBB");
    return false;
  }
  for (size_t i = 0; spec->enum_values[i] != NULL; ++i) {
    if (ascii_equal_ci(value, spec->enum_values[i])) {
      snprintf(out, out_size, "%s", spec->enum_values[i]);
      return true;
    }
  }
  set_error(error, error_size,
            "must be a preset (persimmon, coral, teal, blue, green, violet) or "
            "#RRGGBB");
  return false;
}

static const char *const g_logical_keys[] = {
    "A",     "B",     "X",      "Y",     "L1",         "R1",
    "L2",    "R2",    "SELECT", "START", "MENU",       "HOME",
    "UP",    "DOWN",  "LEFT",   "RIGHT", "F1",         "F2",
    "POWER", "VOLUME_UP", "VOLUME_DOWN",
};

static bool combo_key_name(const char *name, char *out, size_t out_size) {
  for (size_t i = 0; i < sizeof(g_logical_keys) / sizeof(g_logical_keys[0]);
       ++i) {
    if (ascii_equal_ci(name, g_logical_keys[i])) {
      snprintf(out, out_size, "%s", g_logical_keys[i]);
      return true;
    }
  }
  return false;
}

static void trim(char *text) {
  char *start = text;
  while (*start != '\0' && isspace((unsigned char)*start)) {
    ++start;
  }
  if (start != text) {
    memmove(text, start, strlen(start) + 1);
  }
  size_t length = strlen(text);
  while (length > 0 && isspace((unsigned char)text[length - 1])) {
    text[--length] = '\0';
  }
}

/* Parses one chord step (A+B(hold:800)) into a canonical form. */
static bool normalize_combo_step(char *step, char *out, size_t out_size,
                                 char *error, size_t error_size) {
  char *token = step;
  char keys[8][32];
  size_t key_count = 0;
  char modifier[64] = "";
  bool saw_modifier = false;

  while (token != NULL) {
    char *plus = strchr(token, '+');
    if (plus != NULL) {
      *plus = '\0';
    }
    trim(token);
    if (*token == '\0') {
      set_error(error, error_size, "empty key in combo");
      return false;
    }

    char *paren = strchr(token, '(');
    if (paren != NULL) {
      *paren = '\0';
      trim(token);
      char *close = strchr(paren + 1, ')');
      if (close == NULL) {
        set_error(error, error_size, "missing ')' in combo modifier");
        return false;
      }
      if (close[1] != '\0') {
        set_error(error, error_size, "unexpected text after combo modifier");
        return false;
      }
      *close = '\0';
      char *mod = paren + 1;
      trim(mod);
      if (saw_modifier) {
        set_error(error, error_size, "only one modifier per key is allowed");
        return false;
      }
      if (strncmp(mod, "hold:", 5) == 0) {
        long hold_ms = 0;
        if (!parse_int(mod + 5, &hold_ms) || hold_ms < 1 || hold_ms > 10000) {
          set_error(error, error_size, "hold modifier must be hold:<1..10000>");
          return false;
        }
        snprintf(modifier, sizeof(modifier), "(hold:%ld)", hold_ms);
        saw_modifier = true;
      } else if (strcmp(mod, "double") == 0) {
        snprintf(modifier, sizeof(modifier), "(double)");
        saw_modifier = true;
      } else {
        set_error(error, error_size,
                  "unknown combo modifier '%s' (use hold:<ms> or double)", mod);
        return false;
      }
    }

    char canonical_key[32];
    if (strcmp(token, "*") == 0) {
      if (plus != NULL || key_count != 0 || saw_modifier) {
        set_error(error, error_size, "'*' must be used alone");
        return false;
      }
      snprintf(canonical_key, sizeof(canonical_key), "*");
    } else if (!combo_key_name(token, canonical_key, sizeof(canonical_key))) {
      set_error(error, error_size, "unknown key '%s'", token);
      return false;
    }

    if (key_count >= sizeof(keys) / sizeof(keys[0])) {
      set_error(error, error_size, "too many keys in one chord");
      return false;
    }
    for (size_t i = 0; i < key_count; ++i) {
      if (strcmp(keys[i], canonical_key) == 0) {
        set_error(error, error_size, "duplicate key '%s' in chord",
                  canonical_key);
        return false;
      }
    }
    snprintf(keys[key_count], sizeof(keys[key_count]), "%s", canonical_key);
    ++key_count;

    token = plus != NULL ? plus + 1 : NULL;
  }

  if (key_count == 0) {
    set_error(error, error_size, "empty key in combo");
    return false;
  }
  if (saw_modifier && key_count != 1) {
    set_error(error, error_size,
              "a combo modifier applies to a single key, not a chord");
    return false;
  }

  char canonical[BD_CONFIG_TEXT_MAX] = "";
  size_t used = 0;
  for (size_t i = 0; i < key_count; ++i) {
    int written = snprintf(canonical + used, sizeof(canonical) - used,
                           "%s%s", i == 0 ? "" : "+", keys[i]);
    if (written < 0 || (size_t)written >= sizeof(canonical) - used) {
      set_error(error, error_size, "combo is too long");
      return false;
    }
    used += (size_t)written;
  }
  if (saw_modifier) {
    int written = snprintf(canonical + used, sizeof(canonical) - used, "%s",
                           modifier);
    if (written < 0 || (size_t)written >= sizeof(canonical) - used) {
      set_error(error, error_size, "combo is too long");
      return false;
    }
    used += (size_t)written;
  }
  if (used >= out_size) {
    set_error(error, error_size, "combo is too long");
    return false;
  }
  snprintf(out, out_size, "%s", canonical);
  return true;
}

static bool normalize_combo(const bd_config_spec *spec, const char *value,
                            char *out, size_t out_size, char *error,
                            size_t error_size) {
  (void)spec;
  if (value == NULL || *value == '\0') {
    set_error(error, error_size, "empty combo");
    return false;
  }
  if (strlen(value) >= BD_CONFIG_TEXT_MAX) {
    set_error(error, error_size, "combo is too long");
    return false;
  }
  char work[BD_CONFIG_TEXT_MAX];
  snprintf(work, sizeof(work), "%s", value);

  char canonical[BD_CONFIG_TEXT_MAX] = "";
  size_t used = 0;
  char *step = work;
  while (step != NULL) {
    char *comma = strchr(step, ',');
    if (comma != NULL) {
      *comma = '\0';
    }
    trim(step);
    if (*step == '\0') {
      set_error(error, error_size, "empty step in combo");
      return false;
    }
    char step_out[BD_CONFIG_TEXT_MAX];
    if (!normalize_combo_step(step, step_out, sizeof(step_out), error,
                              error_size)) {
      return false;
    }
    if (strcmp(step_out, "*") == 0 &&
        (comma != NULL || used != 0)) {
      set_error(error, error_size, "'*' must be used alone");
      return false;
    }
    int written = snprintf(canonical + used, sizeof(canonical) - used, "%s%s",
                           used == 0 ? "" : ",", step_out);
    if (written < 0 || (size_t)written >= sizeof(canonical) - used) {
      set_error(error, error_size, "combo is too long");
      return false;
    }
    used += (size_t)written;
    step = comma != NULL ? comma + 1 : NULL;
  }
  if (used >= out_size) {
    set_error(error, error_size, "combo is too long");
    return false;
  }
  snprintf(out, out_size, "%s", canonical);
  return true;
}

bool bd_config_normalize(const bd_config_spec *spec, const char *value,
                         char *out, size_t out_size, char *error,
                         size_t error_size) {
  if (value == NULL) {
    set_error(error, error_size, "missing value");
    return false;
  }
  switch (spec->type) {
    case BD_CONFIG_BOOL: {
      bool parsed = false;
      if (!parse_bool(value, &parsed)) {
        set_error(error, error_size, "expected true/false/yes/no/1/0");
        return false;
      }
      snprintf(out, out_size, "%s", parsed ? "true" : "false");
      return true;
    }
    case BD_CONFIG_INT: {
      long parsed = 0;
      if (!parse_int(value, &parsed)) {
        set_error(error, error_size, "expected an integer");
        return false;
      }
      if (parsed < spec->min_value || parsed > spec->max_value) {
        set_error(error, error_size, "must be between %ld and %ld (got %ld)",
                  spec->min_value, spec->max_value, parsed);
        return false;
      }
      snprintf(out, out_size, "%ld", parsed);
      return true;
    }
    case BD_CONFIG_ENUM:
      return normalize_enum(spec, value, out, out_size, error, error_size);
    case BD_CONFIG_STRING: {
      size_t max_length = spec->max_length > 0 ? spec->max_length : 128;
      if ((spec->flags & BD_CONFIG_SPEC_REQUIRED) != 0 && *value == '\0') {
        set_error(error, error_size, "must not be empty");
        return false;
      }
      if (strchr(value, '\n') != NULL || strchr(value, '\r') != NULL) {
        set_error(error, error_size, "must be a single line");
        return false;
      }
      if (strlen(value) > max_length) {
        set_error(error, error_size, "must be at most %zu characters",
                  max_length);
        return false;
      }
      snprintf(out, out_size, "%s", value);
      return true;
    }
    case BD_CONFIG_ACCENT:
      return normalize_accent(spec, value, out, out_size, error, error_size);
    case BD_CONFIG_DURATION: {
      long ms = 0;
      if (!parse_duration_value(value, spec->duration_unit_ms, &ms, error,
                                error_size)) {
        return false;
      }
      if (ms < spec->min_value || ms > spec->max_value) {
        set_error(error, error_size, "must be between %ldms and %ldms",
                  spec->min_value, spec->max_value);
        return false;
      }
      snprintf(out, out_size, "%ld", ms);
      return true;
    }
    case BD_CONFIG_COMBO:
      return normalize_combo(spec, value, out, out_size, error, error_size);
    default:
      set_error(error, error_size, "unsupported schema type");
      return false;
  }
}

const char *bd_config_accent_hex(const char *accent, bool dark) {
  static const struct {
    const char *name;
    const char *dark;
    const char *light;
  } presets[] = {
      {"persimmon", "#e07a5f", "#ae432b"},
      {"coral", "#fd5c47", "#c23b2a"},
      {"teal", "#4fb3a9", "#1f6f68"},
      {"blue", "#6c9bd1", "#2f6690"},
      {"green", "#86b36b", "#4a7a3a"},
      {"violet", "#a98bd6", "#6a4c9c"},
  };
  if (accent == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); ++i) {
    if (ascii_equal_ci(accent, presets[i].name)) {
      return dark ? presets[i].dark : presets[i].light;
    }
  }
  if (accent[0] == '#') {
    size_t length = strlen(accent);
    if (length == 7) {
      for (size_t i = 1; i < length; ++i) {
        if (!isxdigit((unsigned char)accent[i])) {
          return NULL;
        }
      }
      return accent;
    }
  }
  return NULL;
}
