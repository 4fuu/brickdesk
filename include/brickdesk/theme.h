#ifndef BRICKDESK_THEME_H
#define BRICKDESK_THEME_H

#include <stdbool.h>

#include "brickdesk/gfx.h"

/*
 * Runtime accent tokens. The theme never hardcodes the persimmon colour:
 * callers pass one accent colour (usually from their config) and derive the
 * focus ring, pressed/strong and low-emphasis variants here.
 *
 *   accent      focus ring, progress fill, active tab underline
 *   strong      pressed / high-emphasis variant
 *   soft        low-emphasis tint for surfaces and current indicators
 *   focus_ring  light-on-dark outline used over cover art
 *   on_accent   readable text colour on top of accent
 */
typedef struct bd_accent_palette {
  bd_color accent;
  bd_color strong;
  bd_color soft;
  bd_color focus_ring;
  bd_color on_accent;
} bd_accent_palette;

bd_accent_palette bd_accent_palette_from(bd_color accent, bool dark);

/* Parses "#RRGGBB" (case-insensitive) into an opaque bd_color. */
bool bd_color_from_hex(const char *text, bd_color *out);

#endif /* BRICKDESK_THEME_H */
