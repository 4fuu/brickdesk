#ifndef BRICKDESK_INPUT_H
#define BRICKDESK_INPUT_H

/*
 * Logical input keys.
 *
 * Everything above the platform layer speaks this enum. SDL scancodes and
 * Linux evdev key codes are translated at the edges and never leak into the
 * desktop, the test harness or the plugin protocol.
 */

typedef enum bd_key {
  BD_KEY_UNKNOWN = 0,
  BD_KEY_A,
  BD_KEY_B,
  BD_KEY_X,
  BD_KEY_Y,
  BD_KEY_L1,
  BD_KEY_R1,
  BD_KEY_SELECT,
  BD_KEY_START,
  BD_KEY_MENU,
  BD_KEY_UP,
  BD_KEY_DOWN,
  BD_KEY_LEFT,
  BD_KEY_RIGHT,
  BD_KEY_COUNT
} bd_key;

/* Canonical upper-case name, e.g. "RIGHT". Never NULL. */
const char *bd_key_name(bd_key key);

/* Case-insensitive parse of a canonical name. Returns BD_KEY_UNKNOWN. */
bd_key bd_key_from_name(const char *name);

#endif /* BRICKDESK_INPUT_H */
