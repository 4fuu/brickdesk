#include "gfx_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const VERTEX_SHADER =
    "attribute vec2 a_pos;\n"
    "uniform vec4 u_rect;\n"
    "uniform vec2 u_viewport;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 px = u_rect.xy + a_pos * u_rect.zw;\n"
    "  vec2 ndc = vec2(px.x / u_viewport.x * 2.0 - 1.0,\n"
    "                  1.0 - px.y / u_viewport.y * 2.0);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_uv = a_pos;\n"
    "}\n";

static const char *const COLOR_FRAGMENT_SHADER =
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main() {\n"
    "  gl_FragColor = u_color;\n"
    "}\n";

static const char *const TEXTURE_FRAGMENT_SHADER =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_tint;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(u_tex, v_uv) * u_tint;\n"
    "}\n";

void bd_gfx_set_error(bd_gfx *gfx, const char *message) {
  if (gfx == NULL) {
    return;
  }
  snprintf(gfx->error, sizeof(gfx->error), "%s", message != NULL ? message : "");
}

static GLuint compile_shader(bd_gfx *gfx, GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  if (shader == 0) {
    bd_gfx_set_error(gfx, "glCreateShader failed");
    return 0;
  }
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);

  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled != GL_TRUE) {
    char log[512] = {0};
    glGetShaderInfoLog(shader, (GLsizei)sizeof(log), NULL, log);
    bd_gfx_set_error(gfx, log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static GLuint link_program(bd_gfx *gfx, const char *vertex_source,
                           const char *fragment_source) {
  GLuint vertex = compile_shader(gfx, GL_VERTEX_SHADER, vertex_source);
  if (vertex == 0) {
    return 0;
  }
  GLuint fragment = compile_shader(gfx, GL_FRAGMENT_SHADER, fragment_source);
  if (fragment == 0) {
    glDeleteShader(vertex);
    return 0;
  }

  GLuint program = glCreateProgram();
  if (program == 0) {
    bd_gfx_set_error(gfx, "glCreateProgram failed");
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return 0;
  }

  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glBindAttribLocation(program, 0, "a_pos");
  glLinkProgram(program);

  glDetachShader(program, vertex);
  glDetachShader(program, fragment);
  glDeleteShader(vertex);
  glDeleteShader(fragment);

  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    char log[512] = {0};
    glGetProgramInfoLog(program, (GLsizei)sizeof(log), NULL, log);
    bd_gfx_set_error(gfx, log);
    glDeleteProgram(program);
    return 0;
  }
  return program;
}

bd_color bd_color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  bd_color color;
  color.r = (float)r / 255.0f;
  color.g = (float)g / 255.0f;
  color.b = (float)b / 255.0f;
  color.a = (float)a / 255.0f;
  return color;
}

bd_color bd_color_rgb(uint8_t r, uint8_t g, uint8_t b) {
  return bd_color_rgba(r, g, b, 255);
}

bd_rect bd_rect_make(int x, int y, int w, int h) {
  bd_rect rect;
  rect.x = x;
  rect.y = y;
  rect.w = w;
  rect.h = h;
  return rect;
}

static void set_viewport_uniform(bd_gfx *gfx, GLuint program) {
  GLint location = glGetUniformLocation(program, "u_viewport");
  if (location >= 0) {
    glUniform2f(location, (float)gfx->width, (float)gfx->height);
  }
}

bd_gfx *bd_gfx_create(int width, int height) {
  if (width <= 0 || height <= 0) {
    return NULL;
  }

  bd_gfx *gfx = (bd_gfx *)calloc(1, sizeof(*gfx));
  if (gfx == NULL) {
    return NULL;
  }
  gfx->width = width;
  gfx->height = height;

  gfx->color_program = link_program(gfx, VERTEX_SHADER, COLOR_FRAGMENT_SHADER);
  if (gfx->color_program == 0) {
    fprintf(stderr, "gfx: color shader: %s\n", gfx->error);
    bd_gfx_destroy(gfx);
    return NULL;
  }

  gfx->texture_program = link_program(gfx, VERTEX_SHADER, TEXTURE_FRAGMENT_SHADER);
  if (gfx->texture_program == 0) {
    fprintf(stderr, "gfx: texture shader: %s\n", gfx->error);
    bd_gfx_destroy(gfx);
    return NULL;
  }

  gfx->color_rect_uniform = glGetUniformLocation(gfx->color_program, "u_rect");
  gfx->color_uniform = glGetUniformLocation(gfx->color_program, "u_color");
  gfx->texture_rect_uniform = glGetUniformLocation(gfx->texture_program, "u_rect");
  gfx->texture_tint_uniform = glGetUniformLocation(gfx->texture_program, "u_tint");
  gfx->texture_sampler_uniform = glGetUniformLocation(gfx->texture_program, "u_tex");

  if (gfx->color_rect_uniform < 0 || gfx->color_uniform < 0 ||
      gfx->texture_rect_uniform < 0 || gfx->texture_tint_uniform < 0 ||
      gfx->texture_sampler_uniform < 0) {
    bd_gfx_set_error(gfx, "missing shader uniforms");
    fprintf(stderr, "gfx: %s\n", gfx->error);
    bd_gfx_destroy(gfx);
    return NULL;
  }

  static const GLfloat quad_vertices[] = {
      0.0f, 0.0f,
      1.0f, 0.0f,
      0.0f, 1.0f,
      1.0f, 1.0f,
  };
  glGenBuffers(1, &gfx->quad_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, gfx->quad_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);

  glUseProgram(gfx->color_program);
  set_viewport_uniform(gfx, gfx->color_program);
  glUseProgram(gfx->texture_program);
  set_viewport_uniform(gfx, gfx->texture_program);
  glUseProgram(0);

  glBindBuffer(GL_ARRAY_BUFFER, 0);

  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    char message[64];
    snprintf(message, sizeof(message), "GL error 0x%04x during init", (unsigned)error);
    bd_gfx_set_error(gfx, message);
    fprintf(stderr, "gfx: %s\n", gfx->error);
    bd_gfx_destroy(gfx);
    return NULL;
  }

  return gfx;
}

void bd_gfx_destroy(bd_gfx *gfx) {
  if (gfx == NULL) {
    return;
  }
  if (gfx->quad_vbo != 0) {
    glDeleteBuffers(1, &gfx->quad_vbo);
  }
  if (gfx->color_program != 0) {
    glDeleteProgram(gfx->color_program);
  }
  if (gfx->texture_program != 0) {
    glDeleteProgram(gfx->texture_program);
  }
  free(gfx);
}

int bd_gfx_width(const bd_gfx *gfx) {
  return gfx != NULL ? gfx->width : 0;
}

int bd_gfx_height(const bd_gfx *gfx) {
  return gfx != NULL ? gfx->height : 0;
}

void bd_gfx_begin_frame(bd_gfx *gfx) {
  if (gfx == NULL) {
    return;
  }
  glViewport(0, 0, gfx->width, gfx->height);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  /* Dithering is not useful for deterministic screenshots. */
  glDisable(GL_DITHER);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glBindBuffer(GL_ARRAY_BUFFER, gfx->quad_vbo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
}

void bd_gfx_end_frame(bd_gfx *gfx) {
  if (gfx == NULL) {
    return;
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
}

void bd_gfx_clear(bd_gfx *gfx, bd_color color) {
  if (gfx == NULL) {
    return;
  }
  glClearColor(color.r, color.g, color.b, color.a);
  glClear(GL_COLOR_BUFFER_BIT);
}

void bd_gfx_fill_rect(bd_gfx *gfx, bd_rect rect, bd_color color) {
  if (gfx == NULL || rect.w <= 0 || rect.h <= 0) {
    return;
  }
  glUseProgram(gfx->color_program);
  glUniform4f(gfx->color_rect_uniform, (float)rect.x, (float)rect.y,
              (float)rect.w, (float)rect.h);
  glUniform4f(gfx->color_uniform, color.r, color.g, color.b, color.a);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

bd_texture *bd_gfx_texture_create(bd_gfx *gfx, int width, int height,
                                  bd_texture_filter filter,
                                  const uint8_t *rgba_pixels) {
  if (gfx == NULL || rgba_pixels == NULL || width <= 0 || height <= 0) {
    return NULL;
  }

  bd_texture *texture = (bd_texture *)calloc(1, sizeof(*texture));
  if (texture == NULL) {
    return NULL;
  }

  glGenTextures(1, &texture->id);
  if (texture->id == 0) {
    free(texture);
    return NULL;
  }
  texture->width = width;
  texture->height = height;

  glBindTexture(GL_TEXTURE_2D, texture->id);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, rgba_pixels);

  GLint gl_filter = filter == BD_TEXTURE_LINEAR ? GL_LINEAR : GL_NEAREST;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    glDeleteTextures(1, &texture->id);
    free(texture);
    bd_gfx_set_error(gfx, "glTexImage2D failed");
    return NULL;
  }

  return texture;
}

void bd_gfx_texture_destroy(bd_gfx *gfx, bd_texture *texture) {
  (void)gfx;
  if (texture == NULL) {
    return;
  }
  if (texture->id != 0) {
    glDeleteTextures(1, &texture->id);
  }
  free(texture);
}

void bd_gfx_draw_texture(bd_gfx *gfx, bd_texture *texture, bd_rect dst,
                         bd_color tint) {
  if (gfx == NULL || texture == NULL || dst.w <= 0 || dst.h <= 0) {
    return;
  }
  glUseProgram(gfx->texture_program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture->id);
  glUniform1i(gfx->texture_sampler_uniform, 0);
  glUniform4f(gfx->texture_rect_uniform, (float)dst.x, (float)dst.y,
              (float)dst.w, (float)dst.h);
  glUniform4f(gfx->texture_tint_uniform, tint.r, tint.g, tint.b, tint.a);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
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

  /* Screenshots are the one place where waiting for the GPU is correct. */
  glFinish();
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

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

const char *bd_gfx_last_error(const bd_gfx *gfx) {
  if (gfx == NULL) {
    return "";
  }
  return gfx->error;
}
