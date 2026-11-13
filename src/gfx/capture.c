#include "gfx_internal.h"

#include <stdlib.h>
#include <string.h>

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

bool bd_gfx_read_image(bd_gfx *gfx, bd_image *out) {
  if (gfx == NULL || out == NULL) {
    return false;
  }
  out->width = 0;
  out->height = 0;
  out->pixels = NULL;

  const int width = gfx->width;
  const int height = gfx->height;
  const size_t row_bytes = (size_t)width * 4u;
  const size_t total_bytes = row_bytes * (size_t)height;

  uint8_t *pixels = (uint8_t *)malloc(total_bytes);
  if (pixels == NULL) {
    bd_gfx_set_error(gfx, "out of memory for framebuffer capture");
    return false;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, gfx->scene_fbo);
  glViewport(0, 0, width, height);

  /* Screenshots are the one place where waiting for the GPU is correct. */
  glFinish();
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    free(pixels);
    bd_gfx_set_error(gfx, "glReadPixels failed");
    return false;
  }

  /* GL rows are bottom-up; images and PNG are top-down. */
  uint8_t *swap = (uint8_t *)malloc(row_bytes);
  if (swap == NULL) {
    free(pixels);
    bd_gfx_set_error(gfx, "out of memory for framebuffer flip");
    return false;
  }
  for (int y = 0; y < height / 2; ++y) {
    uint8_t *top = pixels + (size_t)y * row_bytes;
    uint8_t *bottom = pixels + (size_t)(height - 1 - y) * row_bytes;
    memcpy(swap, top, row_bytes);
    memcpy(top, bottom, row_bytes);
    memcpy(bottom, swap, row_bytes);
  }
  free(swap);

  out->width = width;
  out->height = height;
  out->pixels = pixels;
  return true;
}

bool bd_image_write_png(const bd_image *image, const char *path) {
  if (image == NULL || image->pixels == NULL || path == NULL ||
      image->width <= 0 || image->height <= 0) {
    return false;
  }
  return stbi_write_png(path, image->width, image->height, 4, image->pixels,
                        image->width * 4) != 0;
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
