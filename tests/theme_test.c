#include <math.h>
#include <stdio.h>

#include "brickdesk/theme.h"

static float luminance(bd_color color) {
  return 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
}

static void print_palette(const char *name, bd_accent_palette palette) {
  printf("%s: accent=%.3f %.3f %.3f strong=%.3f soft=%.3f focus=%.3f on=%.3f %.3f %.3f\n",
         name, palette.accent.r, palette.accent.g, palette.accent.b,
         luminance(palette.strong), luminance(palette.soft),
         luminance(palette.focus_ring), palette.on_accent.r, palette.on_accent.g,
         palette.on_accent.b);
}

int main(void) {
  bd_accent_palette light = bd_accent_palette_from(bd_color_rgb(174, 67, 43), false);
  bd_accent_palette dark = bd_accent_palette_from(bd_color_rgb(224, 122, 95), true);
  print_palette("light", light);
  print_palette("dark", dark);

  if (!(luminance(light.strong) < luminance(light.accent))) {
    fprintf(stderr, "theme: light strong variant is not darker\n");
    return 1;
  }
  if (!(luminance(light.soft) > luminance(light.accent))) {
    fprintf(stderr, "theme: light soft variant is not lighter\n");
    return 1;
  }
  if (!(luminance(light.focus_ring) > luminance(light.accent))) {
    fprintf(stderr, "theme: focus ring is not lighter than accent\n");
    return 1;
  }
  if (!(luminance(light.on_accent) > 0.8f)) {
    fprintf(stderr, "theme: light accent should use light on-accent text\n");
    return 1;
  }
  if (!(luminance(dark.strong) > luminance(dark.accent))) {
    fprintf(stderr, "theme: dark strong variant is not lighter\n");
    return 1;
  }
  if (!(luminance(dark.soft) < luminance(dark.accent))) {
    fprintf(stderr, "theme: dark soft variant is not darker\n");
    return 1;
  }
  if (!(luminance(dark.on_accent) < 0.2f)) {
    fprintf(stderr, "theme: dark accent should use dark on-accent text\n");
    return 1;
  }
  return 0;
}
