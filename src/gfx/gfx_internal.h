#ifndef BRICKDESK_GFX_INTERNAL_H
#define BRICKDESK_GFX_INTERNAL_H

#include <GLES2/gl2.h>

#include "brickdesk/gfx.h"

typedef struct bd_texture {
  GLuint id;
  int width;
  int height;
} bd_texture;

struct bd_gfx {
  int width;
  int height;

  GLuint color_program;
  GLuint texture_program;
  GLuint quad_vbo;

  GLint color_rect_uniform;
  GLint color_uniform;
  GLint texture_rect_uniform;
  GLint texture_tint_uniform;
  GLint texture_sampler_uniform;

  char error[256];
};

void bd_gfx_set_error(bd_gfx *gfx, const char *message);

#endif /* BRICKDESK_GFX_INTERNAL_H */
