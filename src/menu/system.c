#define _POSIX_C_SOURCE 200809L

/*
 * Production brightness/volume backend for the in-app menu.
 *
 * The menu runs inside the application process, so it cannot reuse the shell's
 * bd_settings_system callbacks.  It owns its own bd_av_brightness/bd_av_audio
 * handles and probes the same device_av backends the shell and settings page
 * use.  Probing never writes; a missing or rejected device leaves the menu
 * reading unknown (-1) instead of substituting a software default.
 *
 * Adjustments pass through the device layer, which writes and verifies the
 * hardware readback.  The shim then reads the value again so a quantized
 * backlight or volume step is displayed as the device confirmed it.
 */

#include "menu_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brickdesk/device_av.h"

typedef struct bd_menu_av {
  bool opened;
  bd_av_brightness brightness;
  bool brightness_ready;
  bd_av_audio audio;
  bool audio_ready;
} bd_menu_av;

static bd_menu_av g_menu_av;

static const char *av_option(const char *name) {
  const char *value = getenv(name);
  return value != NULL && value[0] != '\0' ? value : NULL;
}

static void av_log_failure(const char *operation, bd_av_result result,
                           const bd_av_error *error) {
  if (getenv("BRICKDESK_MENU_DEBUG") == NULL) {
    return;
  }
  if (error != NULL && error->message[0] != '\0') {
    fprintf(stderr, "brickdesk-menu: %s: %s (%s)\n", operation,
            bd_av_result_name(result), error->message);
  } else {
    fprintf(stderr, "brickdesk-menu: %s: %s\n", operation,
            bd_av_result_name(result));
  }
}

static void av_open_devices(void) {
  if (g_menu_av.opened) {
    return;
  }
  g_menu_av.opened = true;

  bd_av_error error;
  bd_av_brightness_config brightness_config;
  memset(&brightness_config, 0, sizeof(brightness_config));
  /*
   * The BRICKDESK_AV_* options mirror the device_av configuration fields and
   * bd_shell_config's device-test roots.  They are for host tests and
   * diagnostics; production leaves them unset and gets the device_av defaults
   * (/sys, /dev/disp, /dev/snd, /proc/asound/cards).
   */
  brightness_config.sysfs_root = av_option("BRICKDESK_AV_SYSFS_ROOT");
  brightness_config.backend = BD_AV_BRIGHTNESS_AUTO;
  const char *backend = av_option("BRICKDESK_AV_BRIGHTNESS_BACKEND");
  if (backend != NULL && strcmp(backend, "sysfs") == 0) {
    brightness_config.backend = BD_AV_BRIGHTNESS_SYSFS;
  } else if (backend != NULL && strcmp(backend, "disp") == 0) {
    brightness_config.backend = BD_AV_BRIGHTNESS_DISP;
  } else if (backend != NULL && strcmp(backend, "auto") != 0) {
    av_log_failure("unknown BRICKDESK_AV_BRIGHTNESS_BACKEND", BD_AV_ERR_ARGUMENT,
                   NULL);
  }
  bd_av_result result =
      bd_av_brightness_probe(&g_menu_av.brightness, &brightness_config, &error);
  g_menu_av.brightness_ready = result == BD_AV_OK;
  if (!g_menu_av.brightness_ready) {
    av_log_failure("brightness unavailable", result, &error);
  }

  bd_av_audio_config audio_config;
  memset(&audio_config, 0, sizeof(audio_config));
  audio_config.snd_root = av_option("BRICKDESK_AV_SND_ROOT");
  audio_config.proc_cards = av_option("BRICKDESK_AV_PROC_CARDS");
  audio_config.speaker_mute = av_option("BRICKDESK_AV_SPEAKER_MUTE");
  result = bd_av_audio_open(&g_menu_av.audio, &audio_config, &error);
  g_menu_av.audio_ready = result == BD_AV_OK;
  if (!g_menu_av.audio_ready) {
    av_log_failure("audio unavailable", result, &error);
  }
}

static int av_brightness_get(void *user) {
  bd_menu_av *av = (bd_menu_av *)user;
  if (av == NULL || !av->brightness_ready) {
    return -1;
  }
  int percent = 0;
  bd_av_error error;
  if (bd_av_brightness_get(&av->brightness, &percent, &error) != BD_AV_OK) {
    av_log_failure("brightness read failed", BD_AV_ERR_IO, &error);
    return -1;
  }
  return percent;
}

static bool av_brightness_set(void *user, int percent) {
  bd_menu_av *av = (bd_menu_av *)user;
  if (av == NULL || !av->brightness_ready) {
    return false;
  }
  bd_av_error error;
  bd_av_result result = bd_av_brightness_set(&av->brightness, percent, &error);
  if (result != BD_AV_OK) {
    av_log_failure("brightness write failed", result, &error);
    return false;
  }
  return true;
}

static int av_volume_get(void *user) {
  bd_menu_av *av = (bd_menu_av *)user;
  if (av == NULL || !av->audio_ready) {
    return -1;
  }
  int percent = 0;
  bd_av_error error;
  if (bd_av_audio_get_volume(&av->audio, &percent, &error) != BD_AV_OK) {
    av_log_failure("volume read failed", BD_AV_ERR_IO, &error);
    return -1;
  }
  return percent;
}

static bool av_volume_set(void *user, int percent) {
  bd_menu_av *av = (bd_menu_av *)user;
  if (av == NULL || !av->audio_ready) {
    return false;
  }
  bd_av_error error;
  bd_av_result result = bd_av_audio_set_volume(&av->audio, percent, &error);
  if (result != BD_AV_OK) {
    av_log_failure("volume write failed", result, &error);
    return false;
  }
  return true;
}

void bd_menu_system_ops_device(bd_menu_system_ops *out) {
  if (out == NULL) {
    return;
  }
  av_open_devices();
  memset(out, 0, sizeof(*out));
  out->user = &g_menu_av;
  out->brightness_get = av_brightness_get;
  out->brightness_set = av_brightness_set;
  out->volume_get = av_volume_get;
  out->volume_set = av_volume_set;
}
