#include "brickdesk/settings.h"

#include <string.h>

/*
 * Stub system backend for M3.  It records the last brightness/volume/mute and
 * how often each hook was called.  M4 replaces it with the backlight and
 * mixer implementations; the settings page only knows bd_settings_system.
 */

static bool stub_brightness(void *user, int percent) {
  bd_settings_system_stub *stub = (bd_settings_system_stub *)user;
  if (stub == NULL) {
    return false;
  }
  stub->brightness = percent;
  ++stub->brightness_calls;
  return true;
}

static bool stub_volume(void *user, int percent) {
  bd_settings_system_stub *stub = (bd_settings_system_stub *)user;
  if (stub == NULL) {
    return false;
  }
  stub->volume = percent;
  ++stub->volume_calls;
  return true;
}

static bool stub_mute(void *user, bool muted) {
  bd_settings_system_stub *stub = (bd_settings_system_stub *)user;
  if (stub == NULL) {
    return false;
  }
  stub->muted = muted;
  ++stub->mute_calls;
  return true;
}

void bd_settings_system_stub_init(bd_settings_system_stub *stub,
                                  bd_settings_system *out) {
  if (stub == NULL || out == NULL) {
    return;
  }
  memset(stub, 0, sizeof(*stub));
  out->user = stub;
  out->set_brightness = stub_brightness;
  out->set_volume = stub_volume;
  out->set_muted = stub_mute;
}
