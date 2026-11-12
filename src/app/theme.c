#include "brickdesk/theme.h"

#include <string.h>

static const bd_color WHITE = {1.0f, 0.973f, 0.949f, 1.0f};
static const bd_color INK = {0.102f, 0.075f, 0.051f, 1.0f};

static float relative_luminance(bd_color color) {
  return 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
}

bd_accent_palette bd_accent_palette_from(bd_color accent, bool dark) {
  bd_accent_palette palette;
  palette.accent = accent;
  palette.accent.a = 1.0f;
  palette.strong = bd_color_mix(accent, dark ? WHITE : INK, dark ? 0.14f : 0.18f);
  palette.soft = bd_color_mix(accent, dark ? INK : WHITE, dark ? 0.55f : 0.78f);
  palette.focus_ring = bd_color_mix(accent, WHITE, dark ? 0.55f : 0.38f);
  palette.focus_ring.a = 1.0f;
  palette.on_accent = relative_luminance(accent) > 0.55f ? INK : WHITE;
  return palette;
}

bool bd_color_from_hex(const char *text, bd_color *out) {
  if (text == NULL || out == NULL || text[0] != '#' || strlen(text) != 7) {
    return false;
  }
  int values[3] = {0, 0, 0};
  for (int channel = 0; channel < 3; ++channel) {
    for (int digit = 0; digit < 2; ++digit) {
      char c = text[1 + channel * 2 + digit];
      int value = 0;
      if (c >= '0' && c <= '9') {
        value = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        value = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        value = c - 'A' + 10;
      } else {
        return false;
      }
      values[channel] = values[channel] * 16 + value;
    }
  }
  *out = bd_color_rgb((uint8_t)values[0], (uint8_t)values[1], (uint8_t)values[2]);
  return true;
}
