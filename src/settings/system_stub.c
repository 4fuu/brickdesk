#include "brickdesk/settings.h"

#include <string.h>

/*
 * Explicit recording backend for tests, screenshot captures and demos.  It is
 * never installed by the production shell: there, device_av probe failure is
 * surfaced as unavailable and a failed write never reaches local.ini.
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

static bool stub_get_brightness(void *user, int *percent) {
  bd_settings_system_stub *stub = (bd_settings_system_stub *)user;
  if (stub == NULL || percent == NULL) {
    return false;
  }
  *percent = stub->brightness;
  return true;
}

static bool stub_get_volume(void *user, int *percent) {
  bd_settings_system_stub *stub = (bd_settings_system_stub *)user;
  if (stub == NULL || percent == NULL) {
    return false;
  }
  *percent = stub->volume;
  return true;
}

static bool stub_get_muted(void *user, bool *muted) {
  bd_settings_system_stub *stub = (bd_settings_system_stub *)user;
  if (stub == NULL || muted == NULL) {
    return false;
  }
  *muted = stub->muted;
  return true;
}

void bd_settings_system_stub_init(bd_settings_system_stub *stub,
                                  bd_settings_system *out) {
  if (stub == NULL || out == NULL) {
    return;
  }
  memset(stub, 0, sizeof(*stub));
  stub->brightness = 70;
  stub->volume = 70;
  out->user = stub;
  out->set_brightness = stub_brightness;
  out->set_volume = stub_volume;
  out->set_muted = stub_mute;
  out->get_brightness = stub_get_brightness;
  out->get_volume = stub_get_volume;
  out->get_muted = stub_get_muted;
}
