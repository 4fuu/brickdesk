#include "gfx_internal.h"

#include <stdlib.h>

#include "stb_image_write.h"

void bd_image_free(bd_image *image) {
  if (image == NULL) {
    return;
  }
  free(image->pixels);
  image->pixels = NULL;
  image->width = 0;
  image->height = 0;
}

bool bd_gfx_save_png(bd_gfx *gfx, const char *path) {
  if (gfx == NULL || path == NULL) {
    return false;
  }

  bd_image image;
  if (!bd_gfx_read_image(gfx, &image)) {
    return false;
  }

  /* stb writes top-down rows exactly as they are in bd_image. */
  int written = stbi_write_png(path, image.width, image.height, 4, image.pixels,
                               image.width * 4);
  bd_image_free(&image);

  if (written == 0) {
    bd_gfx_set_error(gfx, "failed to write PNG");
    return false;
  }
  return true;
}
