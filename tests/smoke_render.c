#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "brickdesk/gfx.h"
#include "brickdesk/platform.h"

#define SMOKE_WIDTH 1024
#define SMOKE_HEIGHT 768
#define TEXTURE_SIZE 64

static bd_texture *make_test_texture(bd_gfx *gfx) {
  const size_t pixel_count = (size_t)TEXTURE_SIZE * TEXTURE_SIZE;
  uint8_t *pixels = (uint8_t *)malloc(pixel_count * 4u);
  if (pixels == NULL) {
    return NULL;
  }

  for (int y = 0; y < TEXTURE_SIZE; ++y) {
    for (int x = 0; x < TEXTURE_SIZE; ++x) {
      const bool stripe = ((x / 8) + (y / 8)) % 2 == 0;
      uint8_t *pixel = pixels + ((size_t)y * TEXTURE_SIZE + (size_t)x) * 4u;
      pixel[0] = stripe ? 235 : 40;
      pixel[1] = (uint8_t)(x * 4);
      pixel[2] = (uint8_t)(y * 4);
      pixel[3] = 255;
    }
  }

  bd_texture *texture =
      bd_gfx_texture_create(gfx, TEXTURE_SIZE, TEXTURE_SIZE, BD_TEXTURE_NEAREST, pixels);
  free(pixels);
  return texture;
}

int main(int argc, char **argv) {
  const char *output_path = argc > 1 ? argv[1] : "smoke.png";

  bd_platform_config config;
  config.title = "brickdesk smoke";
  config.width = SMOKE_WIDTH;
  config.height = SMOKE_HEIGHT;
  config.hidden = true;

  bd_platform *platform = bd_platform_create(&config);
  if (platform == NULL) {
    return 1;
  }

  bd_gl_info gl;
  bd_platform_gl_info(platform, &gl);
  bd_gl_print_info(&gl, false);

  if (bd_platform_width(platform) != SMOKE_WIDTH ||
      bd_platform_height(platform) != SMOKE_HEIGHT) {
    fprintf(stderr, "smoke: unexpected drawable size %dx%d\n",
            bd_platform_width(platform), bd_platform_height(platform));
    bd_platform_destroy(platform);
    return 1;
  }

  bd_gfx *gfx = bd_gfx_create(SMOKE_WIDTH, SMOKE_HEIGHT);
  if (gfx == NULL) {
    fprintf(stderr, "smoke: renderer creation failed\n");
    bd_platform_destroy(platform);
    return 1;
  }

  bd_texture *texture = make_test_texture(gfx);
  if (texture == NULL) {
    fprintf(stderr, "smoke: texture creation failed\n");
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }

  bd_gfx_begin_frame(gfx);
  bd_gfx_clear(gfx, bd_color_rgba(0, 0, 0, 255));
  bd_gfx_fill_rect(gfx, bd_rect_make(0, 0, SMOKE_WIDTH, 96),
                   bd_color_rgb(32, 64, 96));
  bd_gfx_fill_rect(gfx, bd_rect_make(48, 32, 320, 16), bd_color_rgb(230, 180, 80));
  bd_gfx_fill_rect(gfx, bd_rect_make(64, 144, 272, 208), bd_color_rgb(60, 110, 143));
  bd_gfx_fill_rect(gfx, bd_rect_make(376, 144, 272, 208), bd_color_rgb(143, 93, 60));
  bd_gfx_fill_rect(gfx, bd_rect_make(688, 144, 272, 208), bd_color_rgb(79, 143, 93));
  bd_gfx_fill_rect(gfx, bd_rect_make(64, 384, 896, 96), bd_color_rgb(36, 50, 68));
  bd_gfx_fill_rect(gfx, bd_rect_make(64, 384, 896, 8), bd_color_rgb(242, 193, 78));
  bd_gfx_draw_texture(gfx, texture, bd_rect_make(64, 544, 128, 128),
                      bd_color_rgba(255, 255, 255, 255));
  bd_gfx_draw_texture(gfx, texture, bd_rect_make(240, 544, TEXTURE_SIZE, TEXTURE_SIZE),
                      bd_color_rgba(255, 255, 255, 255));
  bd_gfx_draw_texture(gfx, texture, bd_rect_make(360, 544, 256, 64),
                      bd_color_rgba(255, 255, 255, 255));
  bd_gfx_end_frame(gfx);

  if (!bd_gfx_save_png(gfx, output_path)) {
    fprintf(stderr, "smoke: save failed: %s\n", bd_gfx_last_error(gfx));
    bd_gfx_texture_destroy(gfx, texture);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }

  bd_image image;
  if (!bd_gfx_read_image(gfx, &image)) {
    fprintf(stderr, "smoke: readback failed: %s\n", bd_gfx_last_error(gfx));
    bd_gfx_texture_destroy(gfx, texture);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }

  const size_t pixel_count = (size_t)image.width * (size_t)image.height;
  size_t non_black = 0;
  size_t bad_alpha = 0;
  for (size_t i = 0; i < pixel_count; ++i) {
    const uint8_t *pixel = image.pixels + i * 4u;
    if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0) {
      ++non_black;
    }
    if (pixel[3] != 255) {
      ++bad_alpha;
    }
  }

  const double non_black_percent = 100.0 * (double)non_black / (double)pixel_count;
  printf("smoke: size=%dx%d non_black=%.2f%% alpha_bad=%lu file=%s\n", image.width,
         image.height, non_black_percent, (unsigned long)bad_alpha, output_path);

  bd_image_free(&image);
  bd_gfx_texture_destroy(gfx, texture);
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);

  if (non_black_percent < 10.0) {
    fprintf(stderr, "smoke: frame is mostly black\n");
    return 1;
  }
  if (bad_alpha != 0) {
    fprintf(stderr, "smoke: captured alpha is not opaque\n");
    return 1;
  }
  return 0;
}
