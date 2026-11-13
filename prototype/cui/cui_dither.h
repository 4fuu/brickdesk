#ifndef BRICKDESK_PROTOTYPE_CUI_DITHER_H
#define BRICKDESK_PROTOTYPE_CUI_DITHER_H

#include <stdbool.h>
#include <stdint.h>

#include "brickdesk/gfx.h"

/*
 * Prototype-only ordered-dither bake.
 *
 * This is a C port of the 8x8 Bayer threshold used by CUI's pattern meter
 * (CUI commit 86d9781bbc3c35c264ec46287ad89c1d86a4c4de, MIT).  It is not
 * compiled into the product.  One RGBA texel represents one dither cell; the
 * caller draws the texture with BD_TEXTURE_NEAREST at an exact integer scale
 * so every cell stays a solid cell x cell block at 1x.
 */
typedef struct cui_dither_bitmap {
  uint8_t *pixels;
  int tex_width;
  int tex_height;
  int cell;
  int columns;
  int rows;
  int lit_cells;
  int total_cells;
  float density;
} cui_dither_bitmap;

/* Bayer threshold in [0, 1) for a cell coordinate. */
float cui_bayer8(int x, int y);

/*
 * Bakes pixel_width x pixel_height worth of cells.  density is clamped to
 * [0, 1]; cells are lit when bayer8(x, y) < density.  color is written
 * opaquely for lit cells and as transparent for unlit cells.
 */
bool cui_dither_bake(cui_dither_bitmap *out, int pixel_width, int pixel_height,
                     int cell, float density, bd_color color);
void cui_dither_free(cui_dither_bitmap *bake);

#endif /* BRICKDESK_PROTOTYPE_CUI_DITHER_H */
