#define _POSIX_C_SOURCE 200809L

#include "brickdesk/led.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brickdesk/device_feedback.h"

#define BD_LED_REASON_MAX 256
#define BD_LED_DEFAULT_DURATION_MS 1000

typedef struct bd_led_color {
  const char *name;
  uint32_t rgb;
} bd_led_color;

static const bd_led_color BD_LED_COLORS[] = {
    {"white", 0xFFFFFF}, {"red", 0xFF0000},   {"orange", 0xFF3300},
    {"yellow", 0xFFFF00}, {"green", 0x00FF00}, {"cyan", 0x00FFFF},
    {"blue", 0x0000FF},   {"violet", 0x8000FF}, {"magenta", 0xFF00FF},
    {"off", 0x000000},
};

static const char *const BD_LED_EFFECTS[] = {
    "linear",  "breathe", "interval_breathe", "static", "blink1",
    "blink2",  "blink3",  "rainbow",          "twinkle", "fire",
    "glitter", "neonglow", "firefly",         "aurora",  "reactive",
};

struct bd_led {
  bd_config *config;
  bd_feedback_led_anim anim;
  bool available;
  char reason[BD_LED_REASON_MAX];
};

static bool set_error(char *error, size_t error_size, const char *format,
                      ...) {
  if (error != NULL && error_size > 0) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
  }
  return false;
}

static int config_int(bd_led *led, const char *key, int fallback, int min,
                      int max) {
  long value = bd_config_get_int(led->config, key, fallback);
  if (value < min) {
    value = min;
  }
  if (value > max) {
    value = max;
  }
  return (int)value;
}

static uint32_t color_rgb(bd_led *led, const char *zone) {
  char key[64];
  snprintf(key, sizeof(key), "led.%s.color", zone);
  const char *name = bd_config_get_string(led->config, key, "white");
  for (size_t i = 0; i < sizeof(BD_LED_COLORS) / sizeof(BD_LED_COLORS[0]);
       ++i) {
    if (strcmp(name, BD_LED_COLORS[i].name) == 0) {
      return BD_LED_COLORS[i].rgb;
    }
  }
  return 0xFFFFFF;
}

static int effect_id(bd_led *led, const char *zone) {
  char key[64];
  snprintf(key, sizeof(key), "led.%s.effect", zone);
  const char *name = bd_config_get_string(led->config, key, "static");
  for (size_t i = 0; i < sizeof(BD_LED_EFFECTS) / sizeof(BD_LED_EFFECTS[0]);
       ++i) {
    if (strcmp(name, BD_LED_EFFECTS[i]) == 0) {
      return (int)i + 1;
    }
  }
  return 4; /* static */
}

static int zone_brightness(bd_led *led, const char *zone) {
  char key[64];
  snprintf(key, sizeof(key), "led.%s.brightness", zone);
  return config_int(led, key, 100, 0, 100);
}

static int duration_ms(bd_led *led) {
  return config_int(led, "led.duration_ms", BD_LED_DEFAULT_DURATION_MS, 100,
                    60000);
}

static bool uniform(bd_led *led, int brightness, int effect, int cycles,
                    uint32_t rgb,
                    bd_feedback_led_anim_settings
                        settings[BD_FEEDBACK_LED_ZONE_COUNT]) {
  (void)led;
  for (int i = 0; i < BD_FEEDBACK_LED_ZONE_COUNT; ++i) {
    settings[i].brightness = brightness;
    settings[i].effect = effect;
    settings[i].cycles = cycles;
    settings[i].duration_ms = BD_LED_DEFAULT_DURATION_MS;
    settings[i].rgb = rgb;
  }
  return true;
}

static bool apply_settings(bd_led *led,
                           const bd_feedback_led_anim_settings
                               settings[BD_FEEDBACK_LED_ZONE_COUNT],
                           char *error, size_t error_size) {
  bd_feedback_error feedback;
  bd_feedback_error_clear(&feedback);
  bd_feedback_result result =
      bd_feedback_led_anim_apply_all(&led->anim, settings, &feedback);
  if (result != BD_FEEDBACK_OK) {
    return set_error(error, error_size, "%s",
                     feedback.message[0] != '\0'
                         ? feedback.message
                         : bd_feedback_result_name(result));
  }
  return true;
}

bd_led *bd_led_create(bd_config *config, const char *sysfs_root, char *error,
                      size_t error_size) {
  if (config == NULL) {
    set_error(error, error_size, "missing config");
    return NULL;
  }
  bd_led *led = calloc(1, sizeof(*led));
  if (led == NULL) {
    set_error(error, error_size, "out of memory");
    return NULL;
  }
  led->config = config;
  bd_feedback_led_anim_config anim_config;
  memset(&anim_config, 0, sizeof(anim_config));
  anim_config.sysfs_root = sysfs_root;
  anim_config.max_brightness = 100;
  bd_feedback_error feedback;
  bd_feedback_error_clear(&feedback);
  bd_feedback_result result =
      bd_feedback_led_anim_probe(&led->anim, &anim_config, &feedback);
  if (result == BD_FEEDBACK_OK) {
    led->available = true;
  } else {
    led->available = false;
    snprintf(led->reason, sizeof(led->reason), "%s",
             feedback.message[0] != '\0'
                 ? feedback.message
                 : bd_feedback_result_name(result));
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "%s", led->reason);
    }
  }
  return led;
}

void bd_led_destroy(bd_led *led) { free(led); }

bool bd_led_available(const bd_led *led) {
  return led != NULL && led->available;
}

const char *bd_led_unavailable_reason(const bd_led *led) {
  return led != NULL ? led->reason : "missing LED handle";
}

void bd_led_reload(bd_led *led) { (void)led; }

bool bd_led_off(bd_led *led, char *error, size_t error_size) {
  if (led == NULL) {
    return set_error(error, error_size, "missing LED handle");
  }
  if (!led->available) {
    return set_error(error, error_size, "%s", led->reason);
  }
  bd_feedback_error feedback;
  bd_feedback_error_clear(&feedback);
  bd_feedback_result result = bd_feedback_led_anim_off(&led->anim, &feedback);
  if (result != BD_FEEDBACK_OK) {
    return set_error(error, error_size, "%s",
                     feedback.message[0] != '\0'
                         ? feedback.message
                         : bd_feedback_result_name(result));
  }
  return true;
}

bool bd_led_apply_default(bd_led *led, char *error, size_t error_size) {
  if (led == NULL) {
    return set_error(error, error_size, "missing LED handle");
  }
  if (!led->available) {
    return set_error(error, error_size, "%s", led->reason);
  }
  if (!bd_config_get_bool(led->config, "led.enabled", true)) {
    return bd_led_off(led, error, error_size);
  }

  static const char *const ZONES[BD_FEEDBACK_LED_ZONE_COUNT] = {
      "m", "lr", "f1", "f2", "rear"};
  bd_feedback_led_anim_settings settings[BD_FEEDBACK_LED_ZONE_COUNT];
  memset(settings, 0, sizeof(settings));
  int duration = duration_ms(led);
  int f1_brightness = zone_brightness(led, "f1");
  for (int i = 0; i < BD_FEEDBACK_LED_ZONE_COUNT; ++i) {
    settings[i].brightness =
        strcmp(ZONES[i], "f2") == 0 ? f1_brightness
                                    : zone_brightness(led, ZONES[i]);
    settings[i].effect = effect_id(led, ZONES[i]);
    settings[i].cycles = -1;
    settings[i].duration_ms = duration;
    settings[i].rgb = color_rgb(led, ZONES[i]);
  }
  return apply_settings(led, settings, error, error_size);
}

static bool apply_uniform(bd_led *led, int brightness, int effect, int cycles,
                          uint32_t rgb, char *error, size_t error_size) {
  bd_feedback_led_anim_settings settings[BD_FEEDBACK_LED_ZONE_COUNT];
  memset(settings, 0, sizeof(settings));
  uniform(led, brightness, effect, cycles, rgb, settings);
  int duration = duration_ms(led);
  for (int i = 0; i < BD_FEEDBACK_LED_ZONE_COUNT; ++i) {
    settings[i].duration_ms = duration;
  }
  return apply_settings(led, settings, error, error_size);
}

bool bd_led_apply_profile(bd_led *led, bd_led_profile profile, char *error,
                          size_t error_size) {
  if (led == NULL) {
    return set_error(error, error_size, "missing LED handle");
  }
  if (profile == BD_LED_PROFILE_OFF) {
    return bd_led_off(led, error, error_size);
  }
  if (profile == BD_LED_PROFILE_DEFAULT) {
    return bd_led_apply_default(led, error, error_size);
  }
  if (!led->available) {
    return set_error(error, error_size, "%s", led->reason);
  }
  if (!bd_config_get_bool(led->config, "led.enabled", true)) {
    return bd_led_off(led, error, error_size);
  }
  int indicator = config_int(led, "led.indicator_brightness", 100, 0, 100);
  switch (profile) {
    case BD_LED_PROFILE_LOW_BATTERY:
      return apply_uniform(led, indicator, 3, -1, 0xFF3300, error,
                           error_size);
    case BD_LED_PROFILE_CRITICAL_BATTERY:
      return apply_uniform(led, indicator, 3, -1, 0xFF0000, error,
                           error_size);
    case BD_LED_PROFILE_CHARGING:
      return apply_uniform(led, indicator, 2, -1, 0x00FF00, error,
                           error_size);
    case BD_LED_PROFILE_SLEEP: {
      static const char *const ZONES[BD_FEEDBACK_LED_ZONE_COUNT] = {
          "m", "lr", "f1", "f2", "rear"};
      bd_feedback_led_anim_settings settings[BD_FEEDBACK_LED_ZONE_COUNT];
      memset(settings, 0, sizeof(settings));
      int duration = duration_ms(led);
      for (int i = 0; i < BD_FEEDBACK_LED_ZONE_COUNT; ++i) {
        settings[i].brightness = indicator;
        settings[i].effect = 2; /* breathe */
        settings[i].cycles = 5;
        settings[i].duration_ms = duration;
        settings[i].rgb = color_rgb(led, ZONES[i]);
      }
      return apply_settings(led, settings, error, error_size);
    }
    case BD_LED_PROFILE_AMBIENT:
      return bd_led_apply_ambient(led, 0xFFFFFF, error, error_size);
    case BD_LED_PROFILE_DEFAULT:
    case BD_LED_PROFILE_OFF:
      break;
  }
  return set_error(error, error_size, "unknown LED profile %d", (int)profile);
}

bool bd_led_apply_ambient(bd_led *led, uint32_t rgb, char *error,
                          size_t error_size) {
  if (led == NULL) {
    return set_error(error, error_size, "missing LED handle");
  }
  if (!led->available) {
    return set_error(error, error_size, "%s", led->reason);
  }
  if (!bd_config_get_bool(led->config, "led.enabled", true) ||
      !bd_config_get_bool(led->config, "led.ambient", false)) {
    return bd_led_apply_default(led, error, error_size);
  }
  int brightness = config_int(led, "led.ambient_brightness", 100, 0, 100);
  return apply_uniform(led, brightness, 4, -1, rgb & 0xFFFFFFu, error,
                       error_size);
}

uint64_t bd_led_sleep_pulse_ms(const bd_led *led) {
  if (led == NULL) {
    return 5000;
  }
  return (uint64_t)duration_ms((bd_led *)led) * 5u;
}
