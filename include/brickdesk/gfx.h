#ifndef BRICKDESK_GFX_H
#define BRICKDESK_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Minimal GLES2 renderer interface.
 *
 * Everything is drawn in 1024x768-style top-left pixel coordinates. The
 * renderer keeps a persistent scene FBO: drawing only touches dirty
 * rectangles, and end_frame blits the complete scene to the default
 * framebuffer. Swap/present still belongs to the platform layer.
 *
 * The renderer is single-threaded for GL work. Image decoding runs on one
 * worker thread owned by the renderer; uploads happen on the render thread.
 * Create it after the platform layer has made a GLES2 context current.
 */

typedef struct bd_gfx bd_gfx;
typedef struct bd_texture bd_texture;
typedef struct bd_font bd_font;

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

typedef enum bd_gradient_axis {
  BD_GRADIENT_VERTICAL = 0,
  BD_GRADIENT_HORIZONTAL = 1
} bd_gradient_axis;

/* Edge of a blur region that fades into the sharp scene. */
typedef enum bd_blur_edge {
  BD_BLUR_EDGE_NONE = 0,
  BD_BLUR_EDGE_TOP = 1,
  BD_BLUR_EDGE_BOTTOM = 2
} bd_blur_edge;

typedef enum bd_image_state {
  BD_IMAGE_PENDING = 0,
  BD_IMAGE_READY = 1,
  BD_IMAGE_FAILED = 2
} bd_image_state;

/*
 * Statistics for the last completed frame. draw_calls includes scene,
 * blur-pass and blur-composite draws; present_draw_calls is the final
 * full-screen copy to the default framebuffer (0 or 1).
 */
typedef struct bd_gfx_stats {
  int draw_calls;
  int present_draw_calls;
  int vertices;
  int batches;
  int damage_rects;
  int64_t damage_pixels;
  int glyphs_rasterized;
  int missing_glyphs;
  int blur_passes;
  bool full_redraw;
  bool presented;
} bd_gfx_stats;

typedef struct bd_image_cache_stats {
  size_t budget_bytes;
  size_t resident_bytes;
  int textures;
  int pending;
  int failed;
} bd_image_cache_stats;

/*
 * Renderer-owned memory accounting. All values are logical byte counts for
 * the resources the renderer allocated: decoded cover textures, font atlases,
 * unique font file data, and the persistent scene/blur FBO attachments.
 * Driver overhead and non-cached textures are not included.
 */
typedef struct bd_gfx_memory_stats {
  size_t texture_cache_bytes;
  size_t texture_cache_budget_bytes;
  size_t glyph_atlas_bytes;
  size_t font_data_bytes;
  size_t fbo_bytes;
} bd_gfx_memory_stats;

bd_color bd_color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
bd_color bd_color_rgb(uint8_t r, uint8_t g, uint8_t b);
/* Mixes in straight sRGB space; t=0 returns a, t=1 returns b. */
bd_color bd_color_mix(bd_color a, bd_color b, float t);
bd_rect bd_rect_make(int x, int y, int w, int h);
bd_rect bd_rect_intersect(bd_rect a, bd_rect b);
int bd_rect_area(bd_rect rect);

/* Returns NULL on failure. Requires a current GLES2 context. */
bd_gfx *bd_gfx_create(int width, int height);
void bd_gfx_destroy(bd_gfx *gfx);

int bd_gfx_width(const bd_gfx *gfx);
int bd_gfx_height(const bd_gfx *gfx);

/*
 * Damage is queued before drawing (bd_desktop_draw does this itself).
 * begin_frame polls finished image jobs and prepares GL state. end_frame
 * draws only the queued damage, then blits the persistent scene to the
 * default framebuffer when the frame produced output. Neither swaps.
 */
void bd_gfx_begin_frame(bd_gfx *gfx);
void bd_gfx_end_frame(bd_gfx *gfx);

void bd_gfx_damage_all(bd_gfx *gfx);
void bd_gfx_damage_rect(bd_gfx *gfx, bd_rect rect);
bool bd_gfx_has_damage(const bd_gfx *gfx);
/* Copies the merged damage rects of the last completed frame. */
int bd_gfx_damage_rects(const bd_gfx *gfx, bd_rect *out, int capacity);

bd_gfx_stats bd_gfx_get_stats(const bd_gfx *gfx);
bd_gfx_memory_stats bd_gfx_memory_get_stats(const bd_gfx *gfx);

/* Solid and rounded primitives. radius <= 0 is a plain rectangle. */
void bd_gfx_clear(bd_gfx *gfx, bd_color color);
void bd_gfx_fill_rect(bd_gfx *gfx, bd_rect rect, bd_color color);
void bd_gfx_fill_round_rect(bd_gfx *gfx, bd_rect rect, int radius, bd_color color);
/* Stroke is drawn inside rect. radius <= 0 gives a rectangular ring. */
void bd_gfx_stroke_round_rect(bd_gfx *gfx, bd_rect rect, int radius, int thickness,
                              bd_color color);

/*
 * Linear gradients with two or three stops. radius clips the gradient to a
 * rounded rectangle (0 for a plain rectangle). Colors are straight RGBA and
 * may be translucent.
 */
void bd_gfx_fill_gradient(bd_gfx *gfx, bd_rect rect, int radius, bd_color start,
                          bd_color end, bd_gradient_axis axis);
void bd_gfx_fill_gradient3(bd_gfx *gfx, bd_rect rect, int radius, bd_color start,
                           bd_color middle, bd_color end, bd_gradient_axis axis);

/* Rectangular scissor stack. Pushes intersect with the current clip. */
void bd_gfx_push_clip(bd_gfx *gfx, bd_rect rect);
void bd_gfx_pop_clip(bd_gfx *gfx);

/*
 * Cheap limited blur: copies rect from the scene FBO at 1/4 resolution,
 * runs two separable 9-tap Gaussian passes, and blends it back masked by a
 * rounded rect plus a start->end overlay gradient. Intended for floating
 * top/bottom bars, not full-screen blur. Records the region so later damage
 * inside it also dirties the whole bar.
 */
void bd_gfx_blur_region(bd_gfx *gfx, bd_rect rect, int radius, bd_color overlay_start,
                        bd_color overlay_end, bd_gradient_axis axis);
/*
 * Same blur, but alpha feathers to zero over `feather` pixels at the chosen
 * horizontal edge. This removes the hard seam where a floating bar meets the
 * sharp content underneath it.
 */
void bd_gfx_blur_region_feathered(bd_gfx *gfx, bd_rect rect, int radius,
                                  bd_color overlay_start, bd_color overlay_end,
                                  bd_gradient_axis axis, bd_blur_edge edge,
                                  int feather);

/*
 * Texture creation takes tightly packed RGBA8 pixels with the top row first.
 * Drawing scales the whole texture into dst; tint is multiplied into the
 * sampled color (use opaque white for none).
 */
bd_texture *bd_gfx_texture_create(bd_gfx *gfx, int width, int height,
                                  bd_texture_filter filter,
                                  const uint8_t *rgba_pixels);
/* Replaces the full contents; reallocates when the size changes. */
bool bd_gfx_texture_update(bd_gfx *gfx, bd_texture *texture, int width,
                           int height, const uint8_t *rgba_pixels);
void bd_gfx_texture_destroy(bd_gfx *gfx, bd_texture *texture);
void bd_gfx_draw_texture(bd_gfx *gfx, bd_texture *texture, bd_rect dst,
                         bd_color tint);
/* Draws a texture clipped to a rounded rectangle. radius <= 0 is square. */
void bd_gfx_draw_texture_round(bd_gfx *gfx, bd_texture *texture, bd_rect dst,
                               bd_color tint, int radius);
/*
 * Like draw_texture_round, but centre-crops the source to fill dst instead of
 * stretching it (CSS object-fit: cover). Used by cover art and app frames whose
 * aspect ratio differs from the tile.
 */
void bd_gfx_draw_texture_cover(bd_gfx *gfx, bd_texture *texture, bd_rect dst,
                               bd_color tint, int radius);

/*
 * Asynchronous PNG/JPEG loading. The decode and bilinear resize run on the
 * renderer's worker thread; the upload happens in begin_frame. The first
 * call starts a job and returns BD_IMAGE_PENDING with *out = NULL. Call it
 * again every frame and use the returned texture while its state is READY.
 *
 * Textures are owned by the cache and must not be passed to
 * bd_gfx_texture_destroy. On LRU eviction the struct stays valid but its GL
 * name is released; requesting the same key reloads it. The default budget
 * is 16 MiB (design.md 4.6).
 */
bd_image_state bd_gfx_image_texture(bd_gfx *gfx, const char *path, int width,
                                    int height, bd_texture **out);
/*
 * Visibility-set API used by the wall: begin_set, request every item in the
 * viewport plus prefetch ring with a distance-to-focus priority, then end_set.
 * Queued jobs that were not requested are cancelled; finished results for
 * them are discarded. Lower distance values decode first.
 */
void bd_gfx_image_begin_set(bd_gfx *gfx);
bd_image_state bd_gfx_image_request(bd_gfx *gfx, const char *path, int width,
                                    int height, int distance, bd_texture **out);
void bd_gfx_image_end_set(bd_gfx *gfx);
void bd_gfx_image_cache_set_budget(bd_gfx *gfx, size_t bytes);
bd_image_cache_stats bd_gfx_image_cache_get_stats(const bd_gfx *gfx);

/*
 * Fonts are loaded with stb_truetype and rasterized on demand into a
 * single-channel atlas with LRU eviction. pixel_size is the em height in
 * pixels. Fallback fonts are used for codepoints the primary font lacks.
 */
bd_font *bd_font_create(bd_gfx *gfx, const char *path, int pixel_size);
void bd_font_destroy(bd_font *font);
void bd_font_set_fallback(bd_font *font, bd_font *fallback);
int bd_font_pixel_size(const bd_font *font);
int bd_font_ascent(const bd_font *font);
int bd_font_line_height(const bd_font *font);
int bd_font_measure_text(const bd_font *font, const char *utf8);

/* x is the left edge; baseline_y is the text baseline. Returns advance width. */
int bd_gfx_draw_text(bd_gfx *gfx, const bd_font *font, int x, int baseline_y,
                     bd_color color, const char *utf8);
/* Single line, truncated with U+2026 when wider than max_width. */
int bd_gfx_draw_text_ellipsized(bd_gfx *gfx, const bd_font *font, int x,
                                int baseline_y, int max_width, bd_color color,
                                const char *utf8);

/*
 * Capture reads the persistent scene FBO and returns a top-down RGBA8 image.
 * Both wait for the GPU; use them for screenshots/tests, not per frame.
 */
bool bd_gfx_read_image(bd_gfx *gfx, bd_image *out);
void bd_image_free(bd_image *image);
/* Writes a tightly packed top-down RGBA8 image; used by framebuffer captures. */
bool bd_image_write_png(const bd_image *image, const char *path);
bool bd_gfx_save_png(bd_gfx *gfx, const char *path);

/* Last renderer error, or "" when there is none. Never NULL. */
const char *bd_gfx_last_error(const bd_gfx *gfx);

#endif /* BRICKDESK_GFX_H */
