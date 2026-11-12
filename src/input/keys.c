#include "brickdesk/input.h"

#include <stdbool.h>
#include <stddef.h>

static const char *const g_key_names[] = {
    "UNKNOWN",   "A",      "B",      "X",          "Y",
    "L1",        "R1",     "L2",     "R2",         "SELECT",
    "START",     "MENU",   "HOME",   "UP",         "DOWN",
    "LEFT",      "RIGHT",  "POWER",  "VOLUME_UP",  "VOLUME_DOWN",
};

static bool ascii_equal_ci(const char *a, const char *b) {
  if (a == NULL || b == NULL) {
    return false;
  }
  while (*a != '\0' && *b != '\0') {
    char ca = *a;
    char cb = *b;
    if (ca >= 'a' && ca <= 'z') {
      ca = (char)(ca - 'a' + 'A');
    }
    if (cb >= 'a' && cb <= 'z') {
      cb = (char)(cb - 'a' + 'A');
    }
    if (ca != cb) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

const char *bd_key_name(bd_key key) {
  if (key <= BD_KEY_UNKNOWN || key >= BD_KEY_COUNT) {
    return g_key_names[BD_KEY_UNKNOWN];
  }
  return g_key_names[key];
}

bd_key bd_key_from_name(const char *name) {
  if (name == NULL) {
    return BD_KEY_UNKNOWN;
  }
  for (int i = 0; i < (int)BD_KEY_COUNT; ++i) {
    if (ascii_equal_ci(name, g_key_names[i])) {
      return (bd_key)i;
    }
  }
  return BD_KEY_UNKNOWN;
}
