#define _POSIX_C_SOURCE 200809L

#include "brickdesk/input.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brickdesk/config.h"

#if defined(__linux__)
#include <linux/input-event-codes.h>
#else
#error "brickdesk input needs Linux evdev headers"
#endif

typedef struct code_name {
  const char *name;
  unsigned code;
} code_name;

static const code_name g_key_codes[] = {
    {"KEY_ESC", KEY_ESC},
    {"KEY_1", KEY_1},
    {"KEY_2", KEY_2},
    {"KEY_3", KEY_3},
    {"KEY_4", KEY_4},
    {"KEY_5", KEY_5},
    {"KEY_6", KEY_6},
    {"KEY_7", KEY_7},
    {"KEY_8", KEY_8},
    {"KEY_9", KEY_9},
    {"KEY_0", KEY_0},
    {"KEY_MINUS", KEY_MINUS},
    {"KEY_EQUAL", KEY_EQUAL},
    {"KEY_BACKSPACE", KEY_BACKSPACE},
    {"KEY_TAB", KEY_TAB},
    {"KEY_Q", KEY_Q},
    {"KEY_W", KEY_W},
    {"KEY_E", KEY_E},
    {"KEY_R", KEY_R},
    {"KEY_T", KEY_T},
    {"KEY_Y", KEY_Y},
    {"KEY_U", KEY_U},
    {"KEY_I", KEY_I},
    {"KEY_O", KEY_O},
    {"KEY_P", KEY_P},
    {"KEY_LEFTBRACE", KEY_LEFTBRACE},
    {"KEY_RIGHTBRACE", KEY_RIGHTBRACE},
    {"KEY_ENTER", KEY_ENTER},
    {"KEY_LEFTCTRL", KEY_LEFTCTRL},
    {"KEY_A", KEY_A},
    {"KEY_S", KEY_S},
    {"KEY_D", KEY_D},
    {"KEY_F", KEY_F},
    {"KEY_G", KEY_G},
    {"KEY_H", KEY_H},
    {"KEY_J", KEY_J},
    {"KEY_K", KEY_K},
    {"KEY_L", KEY_L},
    {"KEY_SEMICOLON", KEY_SEMICOLON},
    {"KEY_APOSTROPHE", KEY_APOSTROPHE},
    {"KEY_GRAVE", KEY_GRAVE},
    {"KEY_LEFTSHIFT", KEY_LEFTSHIFT},
    {"KEY_BACKSLASH", KEY_BACKSLASH},
    {"KEY_Z", KEY_Z},
    {"KEY_X", KEY_X},
    {"KEY_C", KEY_C},
    {"KEY_V", KEY_V},
    {"KEY_B", KEY_B},
    {"KEY_N", KEY_N},
    {"KEY_M", KEY_M},
    {"KEY_COMMA", KEY_COMMA},
    {"KEY_DOT", KEY_DOT},
    {"KEY_SLASH", KEY_SLASH},
    {"KEY_RIGHTSHIFT", KEY_RIGHTSHIFT},
    {"KEY_KPASTERISK", KEY_KPASTERISK},
    {"KEY_LEFTALT", KEY_LEFTALT},
    {"KEY_SPACE", KEY_SPACE},
    {"KEY_CAPSLOCK", KEY_CAPSLOCK},
    {"KEY_F1", KEY_F1},
    {"KEY_F2", KEY_F2},
    {"KEY_F3", KEY_F3},
    {"KEY_F4", KEY_F4},
    {"KEY_F5", KEY_F5},
    {"KEY_F6", KEY_F6},
    {"KEY_F7", KEY_F7},
    {"KEY_F8", KEY_F8},
    {"KEY_F9", KEY_F9},
    {"KEY_F10", KEY_F10},
    {"KEY_F11", KEY_F11},
    {"KEY_F12", KEY_F12},
    {"KEY_SYSRQ", KEY_SYSRQ},
    {"KEY_SCROLLLOCK", KEY_SCROLLLOCK},
    {"KEY_PAUSE", KEY_PAUSE},
    {"KEY_HOME", KEY_HOME},
    {"KEY_UP", KEY_UP},
    {"KEY_PAGEUP", KEY_PAGEUP},
    {"KEY_LEFT", KEY_LEFT},
    {"KEY_RIGHT", KEY_RIGHT},
    {"KEY_END", KEY_END},
    {"KEY_DOWN", KEY_DOWN},
    {"KEY_PAGEDOWN", KEY_PAGEDOWN},
    {"KEY_INSERT", KEY_INSERT},
    {"KEY_DELETE", KEY_DELETE},
    {"KEY_MUTE", KEY_MUTE},
    {"KEY_VOLUMEDOWN", KEY_VOLUMEDOWN},
    {"KEY_VOLUMEUP", KEY_VOLUMEUP},
    {"KEY_POWER", KEY_POWER},
    {"KEY_KPEQUAL", KEY_KPEQUAL},
    {"KEY_KPPLUSMINUS", KEY_KPPLUSMINUS},
    {"KEY_KPCOMMA", KEY_KPCOMMA},
    {"KEY_LEFTMETA", KEY_LEFTMETA},
    {"KEY_RIGHTMETA", KEY_RIGHTMETA},
    {"KEY_COMPOSE", KEY_COMPOSE},
    {"KEY_STOP", KEY_STOP},
    {"KEY_AGAIN", KEY_AGAIN},
    {"KEY_PROPS", KEY_PROPS},
    {"KEY_UNDO", KEY_UNDO},
    {"KEY_COPY", KEY_COPY},
    {"KEY_OPEN", KEY_OPEN},
    {"KEY_PASTE", KEY_PASTE},
    {"KEY_FIND", KEY_FIND},
    {"KEY_CUT", KEY_CUT},
    {"KEY_HELP", KEY_HELP},
    {"KEY_MENU", KEY_MENU},
    {"KEY_CALC", KEY_CALC},
    {"KEY_SLEEP", KEY_SLEEP},
    {"KEY_WAKEUP", KEY_WAKEUP},
    {"KEY_PROG1", KEY_PROG1},
    {"KEY_PROG2", KEY_PROG2},
    {"KEY_WWW", KEY_WWW},
    {"KEY_BACK", KEY_BACK},
    {"KEY_FORWARD", KEY_FORWARD},
    {"KEY_EJECTCD", KEY_EJECTCD},
    {"KEY_NEXTSONG", KEY_NEXTSONG},
    {"KEY_PLAYPAUSE", KEY_PLAYPAUSE},
    {"KEY_PREVIOUSSONG", KEY_PREVIOUSSONG},
    {"KEY_STOPCD", KEY_STOPCD},
    {"KEY_RECORD", KEY_RECORD},
    {"KEY_REWIND", KEY_REWIND},
    {"KEY_PHONE", KEY_PHONE},
    {"KEY_CONFIG", KEY_CONFIG},
    {"KEY_HOMEPAGE", KEY_HOMEPAGE},
    {"KEY_REFRESH", KEY_REFRESH},
    {"KEY_EXIT", KEY_EXIT},
    {"KEY_EDIT", KEY_EDIT},
    {"KEY_SCROLLUP", KEY_SCROLLUP},
    {"KEY_SCROLLDOWN", KEY_SCROLLDOWN},
    {"KEY_NEW", KEY_NEW},
    {"KEY_REDO", KEY_REDO},
    {"KEY_F13", KEY_F13},
    {"KEY_F14", KEY_F14},
    {"KEY_F15", KEY_F15},
    {"KEY_F16", KEY_F16},
    {"KEY_F17", KEY_F17},
    {"KEY_F18", KEY_F18},
    {"KEY_F19", KEY_F19},
    {"KEY_F20", KEY_F20},
    {"KEY_F21", KEY_F21},
    {"KEY_F22", KEY_F22},
    {"KEY_F23", KEY_F23},
    {"KEY_F24", KEY_F24},
    {"KEY_PLAYCD", KEY_PLAYCD},
    {"KEY_PAUSECD", KEY_PAUSECD},
    {"KEY_PLAY", KEY_PLAY},
    {"KEY_FASTFORWARD", KEY_FASTFORWARD},
    {"KEY_PRINT", KEY_PRINT},
    {"KEY_CAMERA", KEY_CAMERA},
    {"KEY_SEARCH", KEY_SEARCH},
    {"KEY_CANCEL", KEY_CANCEL},
    {"KEY_BRIGHTNESSDOWN", KEY_BRIGHTNESSDOWN},
    {"KEY_BRIGHTNESSUP", KEY_BRIGHTNESSUP},
    {"KEY_MEDIA", KEY_MEDIA},
    {"KEY_SWITCHVIDEOMODE", KEY_SWITCHVIDEOMODE},
    {"KEY_KBDILLUMTOGGLE", KEY_KBDILLUMTOGGLE},
    {"KEY_KBDILLUMDOWN", KEY_KBDILLUMDOWN},
    {"KEY_KBDILLUMUP", KEY_KBDILLUMUP},
    {"KEY_SAVE", KEY_SAVE},
    {"KEY_DOCUMENTS", KEY_DOCUMENTS},
    {"KEY_BATTERY", KEY_BATTERY},
    {"KEY_BLUETOOTH", KEY_BLUETOOTH},
    {"KEY_WLAN", KEY_WLAN},
    {"KEY_UWB", KEY_UWB},
    {"KEY_MICMUTE", KEY_MICMUTE},
    {"BTN_0", BTN_0},
    {"BTN_1", BTN_1},
    {"BTN_2", BTN_2},
    {"BTN_3", BTN_3},
    {"BTN_4", BTN_4},
    {"BTN_5", BTN_5},
    {"BTN_6", BTN_6},
    {"BTN_7", BTN_7},
    {"BTN_8", BTN_8},
    {"BTN_9", BTN_9},
    {"BTN_LEFT", BTN_LEFT},
    {"BTN_RIGHT", BTN_RIGHT},
    {"BTN_MIDDLE", BTN_MIDDLE},
    {"BTN_SIDE", BTN_SIDE},
    {"BTN_EXTRA", BTN_EXTRA},
    {"BTN_FORWARD", BTN_FORWARD},
    {"BTN_BACK", BTN_BACK},
    {"BTN_TASK", BTN_TASK},
    {"BTN_TRIGGER", BTN_TRIGGER},
    {"BTN_THUMB", BTN_THUMB},
    {"BTN_THUMB2", BTN_THUMB2},
    {"BTN_TOP", BTN_TOP},
    {"BTN_TOP2", BTN_TOP2},
    {"BTN_PINKIE", BTN_PINKIE},
    {"BTN_BASE", BTN_BASE},
    {"BTN_BASE2", BTN_BASE2},
    {"BTN_BASE3", BTN_BASE3},
    {"BTN_BASE4", BTN_BASE4},
    {"BTN_BASE5", BTN_BASE5},
    {"BTN_BASE6", BTN_BASE6},
    {"BTN_DEAD", BTN_DEAD},
    {"BTN_GAMEPAD", BTN_GAMEPAD},
    {"BTN_SOUTH", BTN_SOUTH},
    {"BTN_A", BTN_A},
    {"BTN_EAST", BTN_EAST},
    {"BTN_B", BTN_B},
    {"BTN_C", BTN_C},
    {"BTN_NORTH", BTN_NORTH},
    {"BTN_X", BTN_X},
    {"BTN_WEST", BTN_WEST},
    {"BTN_Y", BTN_Y},
    {"BTN_Z", BTN_Z},
    {"BTN_TL", BTN_TL},
    {"BTN_TR", BTN_TR},
    {"BTN_TL2", BTN_TL2},
    {"BTN_TR2", BTN_TR2},
    {"BTN_SELECT", BTN_SELECT},
    {"BTN_START", BTN_START},
    {"BTN_MODE", BTN_MODE},
    {"BTN_THUMBL", BTN_THUMBL},
    {"BTN_THUMBR", BTN_THUMBR},
    {"BTN_DPAD_UP", BTN_DPAD_UP},
    {"BTN_DPAD_DOWN", BTN_DPAD_DOWN},
    {"BTN_DPAD_LEFT", BTN_DPAD_LEFT},
    {"BTN_DPAD_RIGHT", BTN_DPAD_RIGHT},
};

static const code_name g_abs_codes[] = {
    {"ABS_X", ABS_X},
    {"ABS_Y", ABS_Y},
    {"ABS_Z", ABS_Z},
    {"ABS_RX", ABS_RX},
    {"ABS_RY", ABS_RY},
    {"ABS_RZ", ABS_RZ},
    {"ABS_THROTTLE", ABS_THROTTLE},
    {"ABS_RUDDER", ABS_RUDDER},
    {"ABS_WHEEL", ABS_WHEEL},
    {"ABS_GAS", ABS_GAS},
    {"ABS_BRAKE", ABS_BRAKE},
    {"ABS_HAT0X", ABS_HAT0X},
    {"ABS_HAT0Y", ABS_HAT0Y},
    {"ABS_HAT1X", ABS_HAT1X},
    {"ABS_HAT1Y", ABS_HAT1Y},
    {"ABS_HAT2X", ABS_HAT2X},
    {"ABS_HAT2Y", ABS_HAT2Y},
    {"ABS_HAT3X", ABS_HAT3X},
    {"ABS_HAT3Y", ABS_HAT3Y},
    {"ABS_PRESSURE", ABS_PRESSURE},
    {"ABS_DISTANCE", ABS_DISTANCE},
    {"ABS_TILT_X", ABS_TILT_X},
    {"ABS_TILT_Y", ABS_TILT_Y},
    {"ABS_TOOL_WIDTH", ABS_TOOL_WIDTH},
    {"ABS_VOLUME", ABS_VOLUME},
    {"ABS_MISC", ABS_MISC},
};

static const code_name g_sw_codes[] = {
    {"SW_LID", SW_LID},
    {"SW_TABLET_MODE", SW_TABLET_MODE},
    {"SW_HEADPHONE_INSERT", SW_HEADPHONE_INSERT},
    {"SW_MICROPHONE_INSERT", SW_MICROPHONE_INSERT},
    {"SW_LINEOUT_INSERT", SW_LINEOUT_INSERT},
    {"SW_JACK_PHYSICAL_INSERT", SW_JACK_PHYSICAL_INSERT},
    {"SW_VIDEOOUT_INSERT", SW_VIDEOOUT_INSERT},
    {"SW_CAMERA_LENS_COVER", SW_CAMERA_LENS_COVER},
    {"SW_KEYPAD_SLIDE", SW_KEYPAD_SLIDE},
    {"SW_FRONT_PROXIMITY", SW_FRONT_PROXIMITY},
    {"SW_ROTATE_LOCK", SW_ROTATE_LOCK},
    {"SW_LINEIN_INSERT", SW_LINEIN_INSERT},
    {"SW_MUTE_DEVICE", SW_MUTE_DEVICE},
    {"SW_PEN_INSERTED", SW_PEN_INSERTED},
};

static void copy_text(char *dst, size_t dst_size, const char *src) {
  if (dst == NULL || dst_size == 0) {
    return;
  }
  size_t length = src != NULL ? strlen(src) : 0;
  if (length >= dst_size) {
    length = dst_size - 1;
  }
  if (length > 0) {
    memcpy(dst, src, length);
  }
  dst[length] = '\0';
}

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

static const char *lookup_name(const code_name *table, size_t count,
                               unsigned code) {
  for (size_t i = 0; i < count; ++i) {
    if (table[i].code == code) {
      return table[i].name;
    }
  }
  return NULL;
}

static bool lookup_code(const code_name *table, size_t count, const char *name,
                        unsigned *code) {
  for (size_t i = 0; i < count; ++i) {
    if (ascii_equal_ci(name, table[i].name)) {
      *code = table[i].code;
      return true;
    }
  }
  return false;
}

const char *bd_evdev_key_name(unsigned code) {
  return lookup_name(g_key_codes, sizeof(g_key_codes) / sizeof(g_key_codes[0]),
                     code);
}

const char *bd_evdev_abs_name(unsigned code) {
  return lookup_name(g_abs_codes, sizeof(g_abs_codes) / sizeof(g_abs_codes[0]),
                     code);
}

const char *bd_evdev_sw_name(unsigned code) {
  return lookup_name(g_sw_codes, sizeof(g_sw_codes) / sizeof(g_sw_codes[0]),
                     code);
}

bool bd_device_map_parse_value(const char *text, bd_device_map_type *type,
                               unsigned *code, int *direction) {
  if (text == NULL || *text == '\0' || type == NULL || code == NULL ||
      direction == NULL) {
    return false;
  }
  *direction = 0;

  char work[128];
  if (strlen(text) >= sizeof(work)) {
    return false;
  }
  snprintf(work, sizeof(work), "%s", text);
  char *colon = strchr(work, ':');
  if (colon != NULL) {
    *colon = '\0';
    char *end = NULL;
    errno = 0;
    long value = strtol(colon + 1, &end, 10);
    if (errno != 0 || end == colon + 1 || *end != '\0' ||
        (value != -1 && value != 0 && value != 1)) {
      return false;
    }
    *direction = (int)value;
  }

  for (char *cursor = work; *cursor != '\0'; ++cursor) {
    if (*cursor >= 'a' && *cursor <= 'z') {
      *cursor = (char)(*cursor - 'a' + 'A');
    }
  }

  if (strncmp(work, "ABS_", 4) == 0) {
    if (!lookup_code(g_abs_codes, sizeof(g_abs_codes) / sizeof(g_abs_codes[0]),
                     work, code)) {
      return false;
    }
    *type = BD_DEVICE_MAP_ABS;
    return true;
  }
  if (lookup_code(g_key_codes, sizeof(g_key_codes) / sizeof(g_key_codes[0]),
                  work, code)) {
    *type = BD_DEVICE_MAP_KEY;
    return true;
  }

  errno = 0;
  char *end = NULL;
  unsigned long numeric = strtoul(work, &end, 0);
  if (errno != 0 || end == work || *end != '\0' || numeric > 0xffffu) {
    return false;
  }
  *code = (unsigned)numeric;
  *type = BD_DEVICE_MAP_KEY;
  return true;
}

static void default_mapping(bd_device_mapping *mapping) {
  memset(mapping, 0, sizeof(*mapping));
  snprintf(mapping->name, sizeof(mapping->name), "builtin");
}

static void add_entry(bd_device_mapping *mapping, bd_key key,
                      bd_device_map_type type, unsigned code, int direction,
                      const char *file, int line) {
  if (mapping->entry_count >= BD_DEVICE_MAX_MAPPINGS) {
    return;
  }
  bd_device_map_entry *entry = &mapping->entries[mapping->entry_count++];
  entry->key = key;
  entry->type = type;
  entry->code = code;
  entry->direction = direction;
  if (file != NULL) {
    snprintf(entry->source_file, sizeof(entry->source_file), "%s", file);
  }
  entry->source_line = line;
}

static void override_entry(bd_device_mapping *mapping, bd_key key,
                           bd_device_map_type type, unsigned code,
                           int direction, const char *file, int line) {
  for (int i = 0; i < mapping->entry_count; ++i) {
    if (mapping->entries[i].key == key) {
      mapping->entries[i].type = type;
      mapping->entries[i].code = code;
      mapping->entries[i].direction = direction;
      mapping->entries[i].source_file[0] = '\0';
      mapping->entries[i].source_line = 0;
      if (file != NULL) {
        snprintf(mapping->entries[i].source_file,
                 sizeof(mapping->entries[i].source_file), "%s", file);
      }
      mapping->entries[i].source_line = line;
      return;
    }
  }
  add_entry(mapping, key, type, code, direction, file, line);
}

static void builtin_mapping(bd_device_mapping *mapping) {
  default_mapping(mapping);
  add_entry(mapping, BD_KEY_A, BD_DEVICE_MAP_KEY, BTN_SOUTH, 0, NULL, 0);
  add_entry(mapping, BD_KEY_B, BD_DEVICE_MAP_KEY, BTN_EAST, 0, NULL, 0);
  add_entry(mapping, BD_KEY_X, BD_DEVICE_MAP_KEY, BTN_NORTH, 0, NULL, 0);
  add_entry(mapping, BD_KEY_Y, BD_DEVICE_MAP_KEY, BTN_WEST, 0, NULL, 0);
  add_entry(mapping, BD_KEY_L1, BD_DEVICE_MAP_KEY, BTN_TL, 0, NULL, 0);
  add_entry(mapping, BD_KEY_R1, BD_DEVICE_MAP_KEY, BTN_TR, 0, NULL, 0);
  add_entry(mapping, BD_KEY_L2, BD_DEVICE_MAP_KEY, BTN_TL2, 0, NULL, 0);
  add_entry(mapping, BD_KEY_R2, BD_DEVICE_MAP_KEY, BTN_TR2, 0, NULL, 0);
  add_entry(mapping, BD_KEY_SELECT, BD_DEVICE_MAP_KEY, BTN_SELECT, 0, NULL, 0);
  add_entry(mapping, BD_KEY_START, BD_DEVICE_MAP_KEY, BTN_START, 0, NULL, 0);
  add_entry(mapping, BD_KEY_MENU, BD_DEVICE_MAP_KEY, BTN_MODE, 0, NULL, 0);
  add_entry(mapping, BD_KEY_HOME, BD_DEVICE_MAP_KEY, KEY_HOME, 0, NULL, 0);
  add_entry(mapping, BD_KEY_F1, BD_DEVICE_MAP_KEY, KEY_F1, 0, NULL, 0);
  add_entry(mapping, BD_KEY_F2, BD_DEVICE_MAP_KEY, KEY_F2, 0, NULL, 0);
  add_entry(mapping, BD_KEY_UP, BD_DEVICE_MAP_ABS, ABS_HAT0Y, -1, NULL, 0);
  add_entry(mapping, BD_KEY_DOWN, BD_DEVICE_MAP_ABS, ABS_HAT0Y, 1, NULL, 0);
  add_entry(mapping, BD_KEY_LEFT, BD_DEVICE_MAP_ABS, ABS_HAT0X, -1, NULL, 0);
  add_entry(mapping, BD_KEY_RIGHT, BD_DEVICE_MAP_ABS, ABS_HAT0X, 1, NULL, 0);
  add_entry(mapping, BD_KEY_UP, BD_DEVICE_MAP_KEY, BTN_DPAD_UP, 0, NULL, 0);
  add_entry(mapping, BD_KEY_DOWN, BD_DEVICE_MAP_KEY, BTN_DPAD_DOWN, 0, NULL, 0);
  add_entry(mapping, BD_KEY_LEFT, BD_DEVICE_MAP_KEY, BTN_DPAD_LEFT, 0, NULL, 0);
  add_entry(mapping, BD_KEY_RIGHT, BD_DEVICE_MAP_KEY, BTN_DPAD_RIGHT, 0, NULL, 0);
  add_entry(mapping, BD_KEY_UP, BD_DEVICE_MAP_KEY, KEY_UP, 0, NULL, 0);
  add_entry(mapping, BD_KEY_DOWN, BD_DEVICE_MAP_KEY, KEY_DOWN, 0, NULL, 0);
  add_entry(mapping, BD_KEY_LEFT, BD_DEVICE_MAP_KEY, KEY_LEFT, 0, NULL, 0);
  add_entry(mapping, BD_KEY_RIGHT, BD_DEVICE_MAP_KEY, KEY_RIGHT, 0, NULL, 0);
  add_entry(mapping, BD_KEY_POWER, BD_DEVICE_MAP_KEY, KEY_POWER, 0, NULL, 0);
  add_entry(mapping, BD_KEY_VOLUME_UP, BD_DEVICE_MAP_KEY, KEY_VOLUMEUP, 0,
            NULL, 0);
  add_entry(mapping, BD_KEY_VOLUME_DOWN, BD_DEVICE_MAP_KEY, KEY_VOLUMEDOWN, 0,
            NULL, 0);
  add_entry(mapping, BD_KEY_MENU, BD_DEVICE_MAP_KEY, KEY_MENU, 0, NULL, 0);
}

static bd_device_mapping *find_or_add_mapping(bd_device_map *map,
                                              const char *name) {
  for (size_t i = 0; i < map->count; ++i) {
    if (strcmp(map->devices[i].name, name) == 0) {
      return &map->devices[i];
    }
  }
  if (map->count >= BD_DEVICE_MAX) {
    return NULL;
  }
  bd_device_mapping *mapping = &map->devices[map->count++];
  builtin_mapping(mapping);
  copy_text(mapping->name, sizeof(mapping->name), name);
  return mapping;
}

static bool parse_u16(const char *text, uint16_t *out) {
  errno = 0;
  char *end = NULL;
  unsigned long value = strtoul(text, &end, 0);
  if (errno != 0 || end == text || *end != '\0' || value > 0xffffu) {
    return false;
  }
  *out = (uint16_t)value;
  return true;
}

bool bd_device_map_load(bd_device_map *map, const struct bd_config *config) {
  if (map == NULL || config == NULL) {
    return false;
  }
  memset(map, 0, sizeof(*map));

  size_t key_count = bd_config_key_count(config);
  for (size_t i = 0; i < key_count; ++i) {
    const char *key = bd_config_key(config, i);
    if (key == NULL || strncmp(key, "device.", 7) != 0) {
      continue;
    }
    const char *last_dot = strrchr(key, '.');
    if (last_dot == NULL || last_dot == key + 6) {
      continue;
    }
    char section[160];
    size_t section_length = (size_t)(last_dot - key);
    if (section_length >= sizeof(section)) {
      continue;
    }
    memcpy(section, key, section_length);
    section[section_length] = '\0';
    const char *field = last_dot + 1;

    bd_device_mapping *mapping = find_or_add_mapping(map, section + 7);
    if (mapping == NULL) {
      continue;
    }
    const char *value = bd_config_get_string(config, key, NULL);
    if (value == NULL) {
      continue;
    }
    int line = 0;
    const char *file = bd_config_source_file(config, key, &line);

    if (strcmp(field, "vendor") == 0) {
      uint16_t vendor = 0;
      if (parse_u16(value, &vendor)) {
        mapping->vendor = vendor;
        mapping->has_ids = true;
      }
      continue;
    }
    if (strcmp(field, "product") == 0) {
      uint16_t product = 0;
      if (parse_u16(value, &product)) {
        mapping->product = product;
        mapping->has_ids = true;
      }
      continue;
    }

    bd_key logical = bd_key_from_name(field);
    if (logical == BD_KEY_UNKNOWN) {
      continue;
    }
    bd_device_map_type type;
    unsigned code = 0;
    int direction = 0;
    if (!bd_device_map_parse_value(value, &type, &code, &direction)) {
      continue;
    }
    if (type == BD_DEVICE_MAP_KEY) {
      direction = 0;
    }
    override_entry(mapping, logical, type, code, direction, file, line);
  }

  long deadzone = bd_config_get_int(config, "input.stick_deadzone", 8);
  if (deadzone < 0) {
    deadzone = 0;
  } else if (deadzone > 30) {
    deadzone = 30;
  }
  map->deadzone = (int)deadzone;
  for (size_t i = 0; i < map->count; ++i) {
    map->devices[i].deadzone = (int)deadzone;
  }
  return true;
}

bool bd_device_map_match(const bd_device_map *map, const bd_evdev *device,
                         bd_device_mapping *out) {
  if (map == NULL || device == NULL || out == NULL) {
    return false;
  }
  for (size_t i = 0; i < map->count; ++i) {
    if (map->devices[i].has_ids && map->devices[i].vendor == device->vendor &&
        map->devices[i].product == device->product) {
      *out = map->devices[i];
      return true;
    }
  }
  for (size_t i = 0; i < map->count; ++i) {
    if (strcmp(map->devices[i].name, device->name) == 0) {
      *out = map->devices[i];
      return true;
    }
  }
  builtin_mapping(out);
  out->deadzone = map->deadzone;
  return true;
}

bool bd_device_map_translate_key(const bd_device_mapping *mapping, unsigned code,
                                 bd_key *out) {
  if (mapping == NULL || out == NULL) {
    return false;
  }
  for (int i = 0; i < mapping->entry_count; ++i) {
    const bd_device_map_entry *entry = &mapping->entries[i];
    if (entry->type == BD_DEVICE_MAP_KEY && entry->code == code) {
      *out = entry->key;
      return true;
    }
  }
  return false;
}

int bd_device_map_translate_abs(const bd_device_mapping *mapping, unsigned code,
                                int value, bd_device_abs_key *out,
                                size_t max) {
  if (mapping == NULL || out == NULL || max == 0) {
    return 0;
  }
  int count = 0;
  if (mapping->deadzone > 0 &&
      (code == ABS_X || code == ABS_Y) &&
      abs(value) < mapping->deadzone * 127 / 100) {
    value = 0;
  }
  for (int i = 0; i < mapping->entry_count && (size_t)count < max; ++i) {
    const bd_device_map_entry *entry = &mapping->entries[i];
    if (entry->type != BD_DEVICE_MAP_ABS || entry->code != code) {
      continue;
    }
    bool pressed = false;
    if (entry->direction < 0) {
      pressed = value < 0;
    } else if (entry->direction > 0) {
      pressed = value > 0;
    } else {
      pressed = value != 0;
    }
    out[count].key = entry->key;
    out[count].pressed = pressed;
    ++count;
  }
  return count;
}
