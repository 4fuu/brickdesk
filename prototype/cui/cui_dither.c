#include "cui_dither.h"

#include <stdlib.h>
#include <string.h>

float cui_bayer8(int x, int y) {
  int qx = x & 7;
  int qy = y & 7;
  int a = qx ^ qy;
  int m = ((a & 1) << 5) | ((qy & 1) << 4) | ((a & 2) << 2) |
          ((qy & 2) << 1) | ((a & 4) >> 1) | ((qy & 4) >> 2);
  return ((float)m + 0.5f) / 64.0f;
}

static uint8_t color_byte(float value) {
  if (value <= 0.0f) {
    return 0;
  }
  if (value >= 1.0f) {
    return 255;
  }
  return (uint8_t)(value * 255.0f + 0.5f);
}

bool cui_dither_bake(cui_dither_bitmap *out, int pixel_width, int pixel_height,
                     int cell, float density, bd_color color) {
  if (out == NULL || pixel_width <= 0 || pixel_height <= 0 || cell <= 0) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  if (density < 0.0f) {
    density = 0.0f;
  }
  if (density > 1.0f) {
    density = 1.0f;
  }

  int columns = (pixel_width + cell - 1) / cell;
  int rows = (pixel_height + cell - 1) / cell;
  size_t count = (size_t)columns * (size_t)rows;
  uint8_t *pixels = (uint8_t *)calloc(count, 4u);
  if (pixels == NULL) {
    return false;
  }

  const uint8_t r = color_byte(color.r);
  const uint8_t g = color_byte(color.g);
  const uint8_t b = color_byte(color.b);
  const uint8_t a = color_byte(color.a);
  int lit = 0;
  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < columns; ++x) {
      if (cui_bayer8(x, y) >= density) {
        continue;
      }
      uint8_t *pixel = pixels + ((size_t)y * (size_t)columns + (size_t)x) * 4u;
      pixel[0] = r;
      pixel[1] = g;
      pixel[2] = b;
      pixel[3] = a;
      ++lit;
    }
  }

  out->pixels = pixels;
  out->tex_width = columns;
  out->tex_height = rows;
  out->cell = cell;
  out->columns = columns;
  out->rows = rows;
  out->lit_cells = lit;
  out->total_cells = (int)count;
  out->density = density;
  return true;
}

void cui_dither_free(cui_dither_bitmap *bake) {
  if (bake == NULL) {
    return;
  }
  free(bake->pixels);
  memset(bake, 0, sizeof(*bake));
}
