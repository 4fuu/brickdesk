#ifndef BRICKDESK_GFX_H
#define BRICKDESK_GFX_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Minimal GLES2 renderer interface for M1.
 *
 * The API is intentionally small: a frame, solid rectangles, textured
 * rectangles and framebuffer capture. Everything is drawn in 1024x768-style
 * top-left pixel coordinates. Later milestones add rounded rectangles,
 * gradients, text and an internal FBO/damage list without changing these
 * entry points; new capabilities get new bd_gfx_* functions.
 *
 * The renderer is single-threaded and owns no window/context. Create it after
 * the platform layer has made a GLES2 context current.
 */

typedef struct bd_gfx bd_gfx;
typedef struct bd_texture bd_texture;

/* Straight (non-premultiplied) RGBA, components in [0, 1]. */
typedef struct bd_color {
  float r;
  float g;
  float b;
  float a;
} bd_color;

/* Pixel rectangle, top-left origin, width/height >= 0. */
typedef struct bd_rect {
  int x;
  int y;
  int w;
  int h;
} bd_rect;

/* RGBA8 image, top row first, tightly packed. */
typedef struct bd_image {
  int width;
  int height;
  uint8_t *pixels;
} bd_image;

typedef enum bd_texture_filter {
  BD_TEXTURE_NEAREST = 0,
  BD_TEXTURE_LINEAR = 1
} bd_texture_filter;

bd_color bd_color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
bd_color bd_color_rgb(uint8_t r, uint8_t g, uint8_t b);
bd_rect bd_rect_make(int x, int y, int w, int h);

/* Returns NULL on failure. Requires a current GLES2 context. */
bd_gfx *bd_gfx_create(int width, int height);
void bd_gfx_destroy(bd_gfx *gfx);

int bd_gfx_width(const bd_gfx *gfx);
int bd_gfx_height(const bd_gfx *gfx);

/*
 * begin_frame binds the render target and sets up GL state. end_frame
 * completes the frame; it never swaps buffers. Swap/present belongs to the
 * platform layer.
 */
void bd_gfx_begin_frame(bd_gfx *gfx);
void bd_gfx_end_frame(bd_gfx *gfx);

void bd_gfx_clear(bd_gfx *gfx, bd_color color);
void bd_gfx_fill_rect(bd_gfx *gfx, bd_rect rect, bd_color color);

/*
 * Texture creation takes tightly packed RGBA8 pixels with the top row first.
 * Textures are immutable in M1. Drawing scales the whole texture into dst;
 * tint is multiplied into the sampled color (use opaque white for none).
 */
bd_texture *bd_gfx_texture_create(bd_gfx *gfx, int width, int height,
                                  bd_texture_filter filter,
                                  const uint8_t *rgba_pixels);
void bd_gfx_texture_destroy(bd_gfx *gfx, bd_texture *texture);
void bd_gfx_draw_texture(bd_gfx *gfx, bd_texture *texture, bd_rect dst,
                         bd_color tint);

/*
 * Capture reads the current render target (the back buffer in M1) and returns
 * a top-down RGBA8 image. bd_gfx_save_png is read + PNG encode in one call.
 * Both wait for the GPU; use them for screenshots/tests, not per frame.
 */
bool bd_gfx_read_image(bd_gfx *gfx, bd_image *out);
void bd_image_free(bd_image *image);
bool bd_gfx_save_png(bd_gfx *gfx, const char *path);

/* Last renderer error, or "" when there is none. Never NULL. */
const char *bd_gfx_last_error(const bd_gfx *gfx);

#endif /* BRICKDESK_GFX_H */
