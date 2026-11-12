#ifndef BRICKDESK_GFX_INTERNAL_H
#define BRICKDESK_GFX_INTERNAL_H

#include <GLES2/gl2.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "brickdesk/gfx.h"

#define BD_GFX_MAX_CLIP_DEPTH 16
#define BD_GFX_MAX_PENDING_DAMAGE 16
#define BD_GFX_MAX_DAMAGE_RECTS 4
#define BD_GFX_MAX_BLUR_REGIONS 8
#define BD_GFX_MAX_BATCHES 256
#define BD_GFX_MAX_QUADS 2048
#define BD_GFX_BLUR_DIVISOR 4

#define BD_GFX_MODE_SOLID 0
#define BD_GFX_MODE_LUMINANCE 1
#define BD_GFX_MODE_RGBA 2
#define BD_GFX_MODE_GRADIENT3 3

struct bd_texture {
  GLuint id;
  int width;
  int height;
  bool opaque;
  bool cache_managed;
};

typedef struct bd_vertex {
  float x;
  float y;
  float u;
  float v;
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;
  float cx;
  float cy;
  float hx;
  float hy;
  float radius;
} bd_vertex;

typedef struct bd_batch {
  GLuint texture;
  int mode;
  int flip;
  float stroke;
  bd_rect clip;
  bd_rect gradient_rect;
  bd_color gradient[3];
  int gradient_axis;
  int first_vertex;
  int quad_count;
} bd_batch;

typedef struct bd_image_job {
  char *path;
  int width;
  int height;
  int distance;
  uint64_t id;
  struct bd_image_entry *entry;
  struct bd_image_job *next;
} bd_image_job;

typedef struct bd_image_result {
  uint8_t *pixels;
  int width;
  int height;
  bool opaque;
  uint64_t id;
  struct bd_image_result *next;
} bd_image_result;

typedef struct bd_image_entry {
  char *path;
  int width;
  int height;
  uint64_t id;
  bd_image_state state;
  bd_texture *texture;
  size_t bytes;
  uint64_t last_used;
  bool wanted;
  bool job_queued;
  bool in_flight;
  int distance;
  struct bd_image_job *queued_job;
  struct bd_image_entry *next;
} bd_image_entry;

struct bd_gfx {
  int width;
  int height;

  GLuint main_program;
  GLuint copy_program;
  GLuint blur_program;
  GLuint composite_program;

  GLint main_viewport;
  GLint main_tex;
  GLint main_mode;
  GLint main_flip;
  GLint main_stroke;
  GLint main_grad0;
  GLint main_grad1;
  GLint main_grad2;
  GLint main_grad_rect;
  GLint main_grad_axis;

  GLint copy_uv_rect;
  GLint copy_tex;

  GLint blur_tex;
  GLint blur_step;
  GLint blur_uv_rect;
  GLint blur_uv_max;

  GLint composite_tex;
  GLint composite_rect;
  GLint composite_viewport;
  GLint composite_center;
  GLint composite_half;
  GLint composite_radius;
  GLint composite_uv_scale;
  GLint composite_overlay0;
  GLint composite_overlay1;
  GLint composite_axis;
  GLint composite_feather;
  GLint composite_feather_edge;

  GLuint vbo;
  GLuint ibo;
  GLuint pass_quad_vbo;
  GLuint white_texture;
  GLuint scene_fbo;
  GLuint scene_texture;

  GLuint blur_fbo;
  GLuint blur_texture[2];
  int blur_width;
  int blur_height;

  bd_vertex *vertices;
  bd_batch *batches;
  int vertex_count;
  int batch_count;

  bd_rect clip_stack[BD_GFX_MAX_CLIP_DEPTH];
  int clip_depth;

  bd_rect damage_pending[BD_GFX_MAX_PENDING_DAMAGE];
  int damage_pending_count;
  bool damage_all;
  bd_rect damage_final[BD_GFX_MAX_DAMAGE_RECTS];
  int damage_final_count;
  int64_t damage_pixels;
  bool damage_ready;

  bd_rect blur_active[BD_GFX_MAX_BLUR_REGIONS];
  int blur_active_count;
  bd_rect blur_frame[BD_GFX_MAX_BLUR_REGIONS];
  int blur_frame_count;

  bd_gfx_stats stats;
  bool in_frame;
  uint64_t frame_number;
  uint64_t glyph_clock;

  void *image_mutex;
  void *image_work_cond;
  void *image_done_cond;
  void *image_thread;
  bool image_quit;
  bd_image_job *image_jobs_head;
  bd_image_job *image_jobs_tail;
  bd_image_result *image_results_head;
  bd_image_result *image_results_tail;
  int image_pending;
  int image_result_count;
  bd_image_entry *image_entries;
  size_t image_budget;
  size_t image_resident;
  uint64_t image_next_id;
  uint64_t image_set_epoch;

  char error[256];
};

void bd_gfx_set_error(bd_gfx *gfx, const char *message);

bd_rect bd_rect_union(bd_rect a, bd_rect b);
bool bd_rect_overlaps_or_touches(bd_rect a, bd_rect b, int slop);

/* Batching, clipping and damage. */
void bd_gfx_batch_begin_frame(bd_gfx *gfx);
void bd_gfx_batch_end_frame(bd_gfx *gfx);
void bd_gfx_batch_reset(bd_gfx *gfx);
void bd_gfx_batch_flush(bd_gfx *gfx);
void bd_gfx_batch_add_quad(bd_gfx *gfx, GLuint texture, int mode, int flip,
                           float stroke, bd_rect rect, int radius, float u0,
                           float v0, float u1, float v1,
                           const bd_color colors[4]);
void bd_gfx_batch_add_gradient3(bd_gfx *gfx, bd_rect rect, int radius,
                                bd_color start, bd_color middle, bd_color end,
                                bd_gradient_axis axis);
void bd_gfx_batch_push_clip(bd_gfx *gfx, bd_rect rect);
void bd_gfx_batch_pop_clip(bd_gfx *gfx);
bd_rect bd_gfx_batch_current_clip(const bd_gfx *gfx);
void bd_gfx_batch_finalize_damage(bd_gfx *gfx);
int bd_gfx_batch_damage_rects(const bd_gfx *gfx, bd_rect *out, int capacity);
void bd_gfx_batch_damage_all(bd_gfx *gfx);
void bd_gfx_batch_damage_rect(bd_gfx *gfx, bd_rect rect);
bool bd_gfx_batch_has_damage(const bd_gfx *gfx);
void bd_gfx_batch_after_frame(bd_gfx *gfx);

/* Texture creation used by the public API, fonts and the image cache. */
GLuint bd_gfx_create_gl_texture(bd_gfx *gfx, int width, int height,
                                bd_texture_filter filter, GLenum internal_format,
                                GLenum format, GLenum type, const void *pixels);
bd_texture *bd_gfx_texture_create_internal(bd_gfx *gfx, int width, int height,
                                           bd_texture_filter filter,
                                           GLenum internal_format, GLenum format,
                                           GLenum type, const void *pixels,
                                           bool cache_managed, bool opaque);

/* Asynchronous image cache. */
bool bd_gfx_image_init(bd_gfx *gfx);
void bd_gfx_image_shutdown(bd_gfx *gfx);
void bd_gfx_image_poll(bd_gfx *gfx);
void bd_gfx_image_end_frame(bd_gfx *gfx);

#endif /* BRICKDESK_GFX_INTERNAL_H */
