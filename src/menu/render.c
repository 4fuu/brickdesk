#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "menu_internal.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLES2/gl2.h>

#include "stb_image_write.h"
#include "stb_truetype.h"

#define BD_MENU_MAX_GLYPHS 384
#define BD_MENU_MAX_VERTICES 8192
#define BD_MENU_ATLAS_SIZE 512

typedef struct bd_vertex {
  float x;
  float y;
  float u;
  float v;
} bd_vertex;

typedef struct bd_gl_api {
#define BD_GL_FIELD(name) __typeof__(&gl##name) name
  BD_GL_FIELD(ActiveTexture);
  BD_GL_FIELD(AttachShader);
  BD_GL_FIELD(BindAttribLocation);
  BD_GL_FIELD(BindBuffer);
  BD_GL_FIELD(BindFramebuffer);
  BD_GL_FIELD(BindTexture);
  BD_GL_FIELD(BlendEquation);
  BD_GL_FIELD(BlendEquationSeparate);
  BD_GL_FIELD(BlendFuncSeparate);
  BD_GL_FIELD(BufferData);
  BD_GL_FIELD(ClearColor);
  BD_GL_FIELD(CompileShader);
  BD_GL_FIELD(CreateProgram);
  BD_GL_FIELD(CreateShader);
  BD_GL_FIELD(DeleteBuffers);
  BD_GL_FIELD(DeleteProgram);
  BD_GL_FIELD(DeleteShader);
  BD_GL_FIELD(DeleteTextures);
  BD_GL_FIELD(Disable);
  BD_GL_FIELD(DisableVertexAttribArray);
  BD_GL_FIELD(DrawArrays);
  BD_GL_FIELD(Enable);
  BD_GL_FIELD(EnableVertexAttribArray);
  BD_GL_FIELD(GenBuffers);
  BD_GL_FIELD(GenTextures);
  BD_GL_FIELD(GetFloatv);
  BD_GL_FIELD(GetIntegerv);
  BD_GL_FIELD(GetProgramInfoLog);
  BD_GL_FIELD(GetProgramiv);
  BD_GL_FIELD(GetShaderInfoLog);
  BD_GL_FIELD(GetShaderiv);
  BD_GL_FIELD(GetUniformLocation);
  BD_GL_FIELD(GetVertexAttribPointerv);
  BD_GL_FIELD(GetVertexAttribiv);
  BD_GL_FIELD(IsEnabled);
  BD_GL_FIELD(LinkProgram);
  BD_GL_FIELD(PixelStorei);
  BD_GL_FIELD(ReadPixels);
  BD_GL_FIELD(Scissor);
  BD_GL_FIELD(ShaderSource);
  BD_GL_FIELD(TexImage2D);
  BD_GL_FIELD(TexParameteri);
  BD_GL_FIELD(TexSubImage2D);
  BD_GL_FIELD(Uniform1f);
  BD_GL_FIELD(Uniform1i);
  BD_GL_FIELD(Uniform2f);
  BD_GL_FIELD(Uniform4f);
  BD_GL_FIELD(UseProgram);
  BD_GL_FIELD(VertexAttribPointer);
  BD_GL_FIELD(Viewport);
#undef BD_GL_FIELD
} bd_gl_api;

typedef struct bd_attrib_state {
  GLint enabled;
  GLint buffer;
  GLint size;
  GLint type;
  GLint normalized;
  GLint stride;
  GLvoid *pointer;
} bd_attrib_state;

typedef struct bd_menu_gl_state {
  GLint viewport[4];
  GLint scissor[4];
  GLint framebuffer;
  GLint program;
  GLint array_buffer;
  GLint element_buffer;
  GLint active_texture;
  GLint texture_2d;
  GLint unpack_alignment;
  GLint pack_alignment;
  GLint blend_src_rgb;
  GLint blend_dst_rgb;
  GLint blend_src_alpha;
  GLint blend_dst_alpha;
  GLint blend_equation_rgb;
  GLint blend_equation_alpha;
  GLfloat clear_color[4];
  GLboolean scissor_test;
  GLboolean blend;
  GLboolean depth_test;
  GLboolean cull_face;
  GLboolean stencil_test;
  int attrib_count;
  bd_attrib_state attribs[8];
} bd_menu_gl_state;

typedef struct bd_menu_glyph {
  uint32_t codepoint;
  float u0;
  float v0;
  float u1;
  float v1;
  int width;
  int height;
  int xoff;
  int yoff;
  int advance;
} bd_menu_glyph;

typedef struct bd_menu_typeface {
  unsigned char *data;
  size_t size;
  stbtt_fontinfo info;
  bool ok;
} bd_menu_typeface;

typedef struct bd_menu_font {
  bd_menu_typeface *typeface;
  GLuint atlas;
  int pixel_size;
  int ascent;
  int line_height;
  int pen_x;
  int pen_y;
  int row_height;
  int glyph_count;
  bd_menu_glyph glyphs[BD_MENU_MAX_GLYPHS];
} bd_menu_font;

struct bd_menu_gl {
  bd_gl_api gl;
  int width;
  int height;
  float scale;
  float origin_x;
  float origin_y;
  bd_menu_color accent;
  GLuint program;
  GLuint vbo;
  GLint uniform_viewport;
  GLint uniform_mode;
  GLint uniform_color;
  GLint uniform_texture;
  GLint uniform_rect_center;
  GLint uniform_rect_half;
  GLint uniform_radius;
  GLuint background;
  bool background_valid;
  bd_menu_gl_state saved;
  bd_menu_typeface serif;
  bd_menu_typeface sans;
  bd_menu_font title_font;
  bd_menu_font body_font;
  bd_menu_font cap_font;
  bd_vertex vertices[BD_MENU_MAX_VERTICES];
  int vertex_count;
};

/* ------------------------------------------------------------------ colors */

bd_menu_color bd_menu_color_rgba(int r, int g, int b, int a) {
  bd_menu_color color;
  color.r = (float)r / 255.0f;
  color.g = (float)g / 255.0f;
  color.b = (float)b / 255.0f;
  color.a = (float)a / 255.0f;
  return color;
}

bd_menu_color bd_menu_color_hex(const char *hex, bd_menu_color fallback) {
  if (hex == NULL || hex[0] != '#' || strlen(hex) < 7) {
    return fallback;
  }
  unsigned value = 0;
  for (int i = 1; i < 7; ++i) {
    char c = hex[i];
    unsigned digit;
    if (c >= '0' && c <= '9') {
      digit = (unsigned)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = (unsigned)(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = (unsigned)(c - 'A' + 10);
    } else {
      return fallback;
    }
    value = (value << 4) | digit;
  }
  return bd_menu_color_rgba((int)((value >> 16) & 0xffu),
                            (int)((value >> 8) & 0xffu), (int)(value & 0xffu),
                            255);
}

/* ---------------------------------------------------------------- GL setup */

static bool load_gl_api(bd_gl_api *api) {
#define BD_GL_LOAD(field, name)                                              \
  do {                                                                       \
    api->field = (__typeof__(api->field))dlsym(RTLD_DEFAULT, "gl" #name);    \
    if (api->field == NULL) {                                                \
      return false;                                                          \
    }                                                                        \
  } while (0)
  BD_GL_LOAD(ActiveTexture, ActiveTexture);
  BD_GL_LOAD(AttachShader, AttachShader);
  BD_GL_LOAD(BindAttribLocation, BindAttribLocation);
  BD_GL_LOAD(BindBuffer, BindBuffer);
  BD_GL_LOAD(BindFramebuffer, BindFramebuffer);
  BD_GL_LOAD(BindTexture, BindTexture);
  BD_GL_LOAD(BlendEquation, BlendEquation);
  BD_GL_LOAD(BlendEquationSeparate, BlendEquationSeparate);
  BD_GL_LOAD(BlendFuncSeparate, BlendFuncSeparate);
  BD_GL_LOAD(BufferData, BufferData);
  BD_GL_LOAD(ClearColor, ClearColor);
  BD_GL_LOAD(CompileShader, CompileShader);
  BD_GL_LOAD(CreateProgram, CreateProgram);
  BD_GL_LOAD(CreateShader, CreateShader);
  BD_GL_LOAD(DeleteBuffers, DeleteBuffers);
  BD_GL_LOAD(DeleteProgram, DeleteProgram);
  BD_GL_LOAD(DeleteShader, DeleteShader);
  BD_GL_LOAD(DeleteTextures, DeleteTextures);
  BD_GL_LOAD(Disable, Disable);
  BD_GL_LOAD(DisableVertexAttribArray, DisableVertexAttribArray);
  BD_GL_LOAD(DrawArrays, DrawArrays);
  BD_GL_LOAD(Enable, Enable);
  BD_GL_LOAD(EnableVertexAttribArray, EnableVertexAttribArray);
  BD_GL_LOAD(GenBuffers, GenBuffers);
  BD_GL_LOAD(GenTextures, GenTextures);
  BD_GL_LOAD(GetFloatv, GetFloatv);
  BD_GL_LOAD(GetIntegerv, GetIntegerv);
  BD_GL_LOAD(GetProgramInfoLog, GetProgramInfoLog);
  BD_GL_LOAD(GetProgramiv, GetProgramiv);
  BD_GL_LOAD(GetShaderInfoLog, GetShaderInfoLog);
  BD_GL_LOAD(GetShaderiv, GetShaderiv);
  BD_GL_LOAD(GetUniformLocation, GetUniformLocation);
  BD_GL_LOAD(GetVertexAttribPointerv, GetVertexAttribPointerv);
  BD_GL_LOAD(GetVertexAttribiv, GetVertexAttribiv);
  BD_GL_LOAD(IsEnabled, IsEnabled);
  BD_GL_LOAD(LinkProgram, LinkProgram);
  BD_GL_LOAD(PixelStorei, PixelStorei);
  BD_GL_LOAD(ReadPixels, ReadPixels);
  BD_GL_LOAD(Scissor, Scissor);
  BD_GL_LOAD(ShaderSource, ShaderSource);
  BD_GL_LOAD(TexImage2D, TexImage2D);
  BD_GL_LOAD(TexParameteri, TexParameteri);
  BD_GL_LOAD(TexSubImage2D, TexSubImage2D);
  BD_GL_LOAD(Uniform1f, Uniform1f);
  BD_GL_LOAD(Uniform1i, Uniform1i);
  BD_GL_LOAD(Uniform2f, Uniform2f);
  BD_GL_LOAD(Uniform4f, Uniform4f);
  BD_GL_LOAD(UseProgram, UseProgram);
  BD_GL_LOAD(VertexAttribPointer, VertexAttribPointer);
  BD_GL_LOAD(Viewport, Viewport);
#undef BD_GL_LOAD
  return true;
}

static void save_state(bd_menu_gl *gl) {
  bd_menu_gl_state *state = &gl->saved;
  memset(state, 0, sizeof(*state));
  gl->gl.GetIntegerv(GL_VIEWPORT, state->viewport);
  gl->gl.GetIntegerv(GL_SCISSOR_BOX, state->scissor);
  gl->gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &state->framebuffer);
  gl->gl.GetIntegerv(GL_CURRENT_PROGRAM, &state->program);
  gl->gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &state->array_buffer);
  gl->gl.GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,
                     &state->element_buffer);
  gl->gl.GetIntegerv(GL_ACTIVE_TEXTURE, &state->active_texture);
  gl->gl.ActiveTexture(GL_TEXTURE0);
  gl->gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &state->texture_2d);
  gl->gl.ActiveTexture((GLenum)state->active_texture);
  gl->gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &state->unpack_alignment);
  gl->gl.GetIntegerv(GL_PACK_ALIGNMENT, &state->pack_alignment);
  gl->gl.GetIntegerv(GL_BLEND_SRC_RGB, &state->blend_src_rgb);
  gl->gl.GetIntegerv(GL_BLEND_DST_RGB, &state->blend_dst_rgb);
  gl->gl.GetIntegerv(GL_BLEND_SRC_ALPHA, &state->blend_src_alpha);
  gl->gl.GetIntegerv(GL_BLEND_DST_ALPHA, &state->blend_dst_alpha);
  gl->gl.GetIntegerv(GL_BLEND_EQUATION_RGB, &state->blend_equation_rgb);
  gl->gl.GetIntegerv(GL_BLEND_EQUATION_ALPHA, &state->blend_equation_alpha);
  gl->gl.GetFloatv(GL_COLOR_CLEAR_VALUE, state->clear_color);
  state->scissor_test = gl->gl.IsEnabled(GL_SCISSOR_TEST);
  state->blend = gl->gl.IsEnabled(GL_BLEND);
  state->depth_test = gl->gl.IsEnabled(GL_DEPTH_TEST);
  state->cull_face = gl->gl.IsEnabled(GL_CULL_FACE);
  state->stencil_test = gl->gl.IsEnabled(GL_STENCIL_TEST);

  GLint max_attribs = 0;
  gl->gl.GetIntegerv(GL_MAX_VERTEX_ATTRIBS, &max_attribs);
  if (max_attribs > 8) {
    max_attribs = 8;
  }
  state->attrib_count = max_attribs;
  for (int i = 0; i < max_attribs; ++i) {
    bd_attrib_state *attrib = &state->attribs[i];
    gl->gl.GetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED,
                             &attrib->enabled);
    gl->gl.GetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                             &attrib->buffer);
    gl->gl.GetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_SIZE,
                             &attrib->size);
    gl->gl.GetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_TYPE,
                             &attrib->type);
    gl->gl.GetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                             &attrib->normalized);
    gl->gl.GetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_STRIDE,
                             &attrib->stride);
    gl->gl.GetVertexAttribPointerv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                                    &attrib->pointer);
  }
}

static void restore_state(bd_menu_gl *gl) {
  const bd_menu_gl_state *state = &gl->saved;
  gl->gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)state->framebuffer);
  gl->gl.Viewport(state->viewport[0], state->viewport[1], state->viewport[2],
                  state->viewport[3]);
  gl->gl.Scissor(state->scissor[0], state->scissor[1], state->scissor[2],
                 state->scissor[3]);
  if (state->scissor_test) {
    gl->gl.Enable(GL_SCISSOR_TEST);
  } else {
    gl->gl.Disable(GL_SCISSOR_TEST);
  }
  gl->gl.BlendEquationSeparate((GLenum)state->blend_equation_rgb,
                               (GLenum)state->blend_equation_alpha);
  gl->gl.BlendFuncSeparate((GLenum)state->blend_src_rgb,
                           (GLenum)state->blend_dst_rgb,
                           (GLenum)state->blend_src_alpha,
                           (GLenum)state->blend_dst_alpha);
  if (state->blend) {
    gl->gl.Enable(GL_BLEND);
  } else {
    gl->gl.Disable(GL_BLEND);
  }
  if (state->depth_test) {
    gl->gl.Enable(GL_DEPTH_TEST);
  } else {
    gl->gl.Disable(GL_DEPTH_TEST);
  }
  if (state->cull_face) {
    gl->gl.Enable(GL_CULL_FACE);
  } else {
    gl->gl.Disable(GL_CULL_FACE);
  }
  if (state->stencil_test) {
    gl->gl.Enable(GL_STENCIL_TEST);
  } else {
    gl->gl.Disable(GL_STENCIL_TEST);
  }
  gl->gl.PixelStorei(GL_UNPACK_ALIGNMENT, state->unpack_alignment);
  gl->gl.PixelStorei(GL_PACK_ALIGNMENT, state->pack_alignment);
  gl->gl.ClearColor(state->clear_color[0], state->clear_color[1],
                    state->clear_color[2], state->clear_color[3]);

  gl->gl.UseProgram((GLuint)state->program);
  gl->gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)state->element_buffer);
  for (int i = 0; i < state->attrib_count; ++i) {
    const bd_attrib_state *attrib = &state->attribs[i];
    gl->gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)attrib->buffer);
    gl->gl.VertexAttribPointer((GLuint)i, attrib->size, (GLenum)attrib->type,
                               (GLboolean)attrib->normalized, attrib->stride,
                               attrib->pointer);
    if (attrib->enabled) {
      gl->gl.EnableVertexAttribArray((GLuint)i);
    } else {
      gl->gl.DisableVertexAttribArray((GLuint)i);
    }
  }
  gl->gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)state->array_buffer);
  gl->gl.ActiveTexture(GL_TEXTURE0);
  gl->gl.BindTexture(GL_TEXTURE_2D, (GLuint)state->texture_2d);
  gl->gl.ActiveTexture((GLenum)state->active_texture);
}

static GLuint compile_shader(bd_menu_gl *gl, GLenum type,
                             const char *source) {
  GLuint shader = gl->gl.CreateShader(type);
  if (shader == 0) {
    return 0;
  }
  gl->gl.ShaderSource(shader, 1, &source, NULL);
  gl->gl.CompileShader(shader);
  GLint compiled = 0;
  gl->gl.GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (!compiled) {
    if (getenv("BRICKDESK_MENU_DEBUG") != NULL) {
      char log[512];
      gl->gl.GetShaderInfoLog(shader, sizeof(log), NULL, log);
      fprintf(stderr, "brickdesk-menu: shader: %s\n", log);
    }
    gl->gl.DeleteShader(shader);
    return 0;
  }
  return shader;
}

static bool create_program(bd_menu_gl *gl) {
  static const char *vertex_source =
      "attribute vec2 a_pos;\n"
      "attribute vec2 a_uv;\n"
      "uniform vec2 u_viewport;\n"
      "varying vec2 v_pos;\n"
      "varying vec2 v_uv;\n"
      "void main() {\n"
      "  v_pos = a_pos;\n"
      "  v_uv = a_uv;\n"
      "  vec2 ndc = vec2(a_pos.x / u_viewport.x * 2.0 - 1.0,\n"
      "                  1.0 - a_pos.y / u_viewport.y * 2.0);\n"
      "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
      "}\n";
  static const char *fragment_source =
      "precision mediump float;\n"
      "uniform int u_mode;\n"
      "uniform vec4 u_color;\n"
      "uniform sampler2D u_texture;\n"
      "uniform vec2 u_rect_center;\n"
      "uniform vec2 u_rect_half;\n"
      "uniform float u_radius;\n"
      "varying vec2 v_pos;\n"
      "varying vec2 v_uv;\n"
      "void main() {\n"
      "  vec4 color = u_color;\n"
      "  if (u_mode == 1) {\n"
      "    color.a *= texture2D(u_texture, v_uv).a;\n"
      "  } else if (u_mode == 3) {\n"
      "    color *= texture2D(u_texture, v_uv);\n"
      "  } else if (u_mode == 2) {\n"
      "    vec2 q = abs(v_pos - u_rect_center) - u_rect_half + vec2(u_radius);\n"
      "    float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) -"
      " u_radius;\n"
      "    color.a *= 1.0 - smoothstep(-0.7, 0.7, d);\n"
      "  }\n"
      "  gl_FragColor = color;\n"
      "}\n";
  GLuint vertex = compile_shader(gl, GL_VERTEX_SHADER, vertex_source);
  GLuint fragment = compile_shader(gl, GL_FRAGMENT_SHADER, fragment_source);
  if (vertex == 0 || fragment == 0) {
    if (vertex != 0) {
      gl->gl.DeleteShader(vertex);
    }
    if (fragment != 0) {
      gl->gl.DeleteShader(fragment);
    }
    return false;
  }
  GLuint program = gl->gl.CreateProgram();
  gl->gl.AttachShader(program, vertex);
  gl->gl.AttachShader(program, fragment);
  gl->gl.BindAttribLocation(program, 0, "a_pos");
  gl->gl.BindAttribLocation(program, 1, "a_uv");
  gl->gl.LinkProgram(program);
  gl->gl.DeleteShader(vertex);
  gl->gl.DeleteShader(fragment);
  GLint linked = 0;
  gl->gl.GetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    if (getenv("BRICKDESK_MENU_DEBUG") != NULL) {
      char log[512];
      gl->gl.GetProgramInfoLog(program, sizeof(log), NULL, log);
      fprintf(stderr, "brickdesk-menu: program: %s\n", log);
    }
    gl->gl.DeleteProgram(program);
    return false;
  }
  gl->program = program;
  gl->uniform_viewport = gl->gl.GetUniformLocation(program, "u_viewport");
  gl->uniform_mode = gl->gl.GetUniformLocation(program, "u_mode");
  gl->uniform_color = gl->gl.GetUniformLocation(program, "u_color");
  gl->uniform_texture = gl->gl.GetUniformLocation(program, "u_texture");
  gl->uniform_rect_center = gl->gl.GetUniformLocation(program, "u_rect_center");
  gl->uniform_rect_half = gl->gl.GetUniformLocation(program, "u_rect_half");
  gl->uniform_radius = gl->gl.GetUniformLocation(program, "u_radius");
  return true;
}

/* ------------------------------------------------------------------- fonts */

static uint32_t utf8_next(const char **cursor) {
  const unsigned char *bytes = (const unsigned char *)*cursor;
  if (*bytes == 0) {
    return 0;
  }
  uint32_t value;
  int length;
  if ((bytes[0] & 0x80u) == 0) {
    value = bytes[0];
    length = 1;
  } else if ((bytes[0] & 0xE0u) == 0xC0u) {
    value = bytes[0] & 0x1Fu;
    length = 2;
  } else if ((bytes[0] & 0xF0u) == 0xE0u) {
    value = bytes[0] & 0x0Fu;
    length = 3;
  } else if ((bytes[0] & 0xF8u) == 0xF0u) {
    value = bytes[0] & 0x07u;
    length = 4;
  } else {
    *cursor += 1;
    return 0xFFFDu;
  }
  for (int i = 1; i < length; ++i) {
    if ((bytes[i] & 0xC0u) != 0x80u) {
      *cursor += 1;
      return 0xFFFDu;
    }
    value = (value << 6) | (bytes[i] & 0x3Fu);
  }
  *cursor += length;
  return value;
}

static bool load_typeface(bd_menu_typeface *typeface, const char *path) {
  memset(typeface, 0, sizeof(*typeface));
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return false;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return false;
  }
  long size = ftell(file);
  if (size <= 0 || size > 32 * 1024 * 1024) {
    fclose(file);
    return false;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return false;
  }
  typeface->data = (unsigned char *)malloc((size_t)size);
  if (typeface->data == NULL) {
    fclose(file);
    return false;
  }
  if (fread(typeface->data, 1, (size_t)size, file) != (size_t)size) {
    free(typeface->data);
    typeface->data = NULL;
    fclose(file);
    return false;
  }
  fclose(file);
  typeface->size = (size_t)size;
  int offset = stbtt_GetFontOffsetForIndex(typeface->data, 0);
  if (offset < 0 || !stbtt_InitFont(&typeface->info, typeface->data, offset)) {
    free(typeface->data);
    typeface->data = NULL;
    return false;
  }
  typeface->ok = true;
  return true;
}

static bool font_create(bd_menu_gl *gl, bd_menu_font *font,
                        bd_menu_typeface *typeface, int pixel_size) {
  memset(font, 0, sizeof(*font));
  if (typeface == NULL || !typeface->ok || pixel_size < 6) {
    return false;
  }
  font->typeface = typeface;
  font->pixel_size = pixel_size;
  float scale = stbtt_ScaleForPixelHeight(&typeface->info, (float)pixel_size);
  int ascent = 0;
  int descent = 0;
  int line_gap = 0;
  stbtt_GetFontVMetrics(&typeface->info, &ascent, &descent, &line_gap);
  font->ascent = (int)ceilf((float)ascent * scale);
  font->line_height = font->ascent - (int)floorf((float)descent * scale) +
                      (int)floorf((float)line_gap * scale + 0.5f);
  if (font->line_height < pixel_size) {
    font->line_height = pixel_size;
  }
  font->pen_x = 1;
  font->pen_y = 1;
  font->row_height = 0;

  unsigned char *empty = (unsigned char *)calloc(
      (size_t)BD_MENU_ATLAS_SIZE * BD_MENU_ATLAS_SIZE, 1);
  if (empty == NULL) {
    return false;
  }
  GLuint atlas = 0;
  gl->gl.GenTextures(1, &atlas);
  gl->gl.BindTexture(GL_TEXTURE_2D, atlas);
  gl->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, BD_MENU_ATLAS_SIZE,
                    BD_MENU_ATLAS_SIZE, 0, GL_ALPHA, GL_UNSIGNED_BYTE, empty);
  gl->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  free(empty);
  font->atlas = atlas;
  return true;
}

static void font_destroy(bd_menu_gl *gl, bd_menu_font *font) {
  if (font->atlas != 0) {
    gl->gl.DeleteTextures(1, &font->atlas);
    font->atlas = 0;
  }
}

static const bd_menu_glyph *font_glyph(bd_menu_gl *gl, bd_menu_font *font,
                                       uint32_t codepoint) {
  for (int i = 0; i < font->glyph_count; ++i) {
    if (font->glyphs[i].codepoint == codepoint) {
      return &font->glyphs[i];
    }
  }
  if (font->glyph_count >= BD_MENU_MAX_GLYPHS) {
    return NULL;
  }
  stbtt_fontinfo *info = &font->typeface->info;
  float scale = stbtt_ScaleForPixelHeight(info, (float)font->pixel_size);
  int advance = 0;
  int bearing = 0;
  stbtt_GetCodepointHMetrics(info, (int)codepoint, &advance, &bearing);
  int width = 0;
  int height = 0;
  int xoff = 0;
  int yoff = 0;
  unsigned char *bitmap = stbtt_GetCodepointBitmap(
      info, scale, scale, (int)codepoint, &width, &height, &xoff, &yoff);
  if (bitmap == NULL) {
    width = 0;
    height = 0;
  }
  int cell_width = width + 2;
  int cell_height = height + 2;
  if (font->pen_x + cell_width >= BD_MENU_ATLAS_SIZE) {
    font->pen_x = 1;
    font->pen_y += font->row_height + 1;
    font->row_height = 0;
  }
  if (font->pen_y + cell_height >= BD_MENU_ATLAS_SIZE) {
    if (bitmap != NULL) {
      stbtt_FreeBitmap(bitmap, NULL);
    }
    return NULL;
  }
  if (width > 0 && height > 0) {
    gl->gl.BindTexture(GL_TEXTURE_2D, font->atlas);
    gl->gl.TexSubImage2D(GL_TEXTURE_2D, 0, font->pen_x + 1, font->pen_y + 1,
                         width, height, GL_ALPHA, GL_UNSIGNED_BYTE, bitmap);
  }
  bd_menu_glyph *glyph = &font->glyphs[font->glyph_count++];
  memset(glyph, 0, sizeof(*glyph));
  glyph->codepoint = codepoint;
  glyph->width = width;
  glyph->height = height;
  glyph->xoff = xoff;
  glyph->yoff = yoff;
  glyph->advance =
      (int)ceilf((float)advance * scale);
  glyph->u0 = (float)(font->pen_x + 1) / (float)BD_MENU_ATLAS_SIZE;
  glyph->v0 = (float)(font->pen_y + 1) / (float)BD_MENU_ATLAS_SIZE;
  glyph->u1 = (float)(font->pen_x + 1 + width) / (float)BD_MENU_ATLAS_SIZE;
  glyph->v1 = (float)(font->pen_y + 1 + height) / (float)BD_MENU_ATLAS_SIZE;
  if (cell_height > font->row_height) {
    font->row_height = cell_height;
  }
  font->pen_x += cell_width + 1;
  if (bitmap != NULL) {
    stbtt_FreeBitmap(bitmap, NULL);
  }
  return glyph;
}

static int text_width(bd_menu_gl *gl, bd_menu_font *font, const char *text) {
  int width = 0;
  const char *cursor = text;
  while (cursor != NULL && *cursor != '\0') {
    uint32_t codepoint = utf8_next(&cursor);
    if (codepoint == 0) {
      break;
    }
    const bd_menu_glyph *glyph = font_glyph(gl, font, codepoint);
    if (glyph == NULL) {
      continue;
    }
    width += glyph->advance;
  }
  return width;
}

/* --------------------------------------------------------------- rendering */

static void batch_flush(bd_menu_gl *gl) {
  if (gl->vertex_count == 0) {
    return;
  }
  gl->gl.BindBuffer(GL_ARRAY_BUFFER, gl->vbo);
  gl->gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(gl->vertex_count * sizeof(bd_vertex)),
                    gl->vertices, GL_STREAM_DRAW);
  gl->gl.DrawArrays(GL_TRIANGLES, 0, gl->vertex_count);
  gl->vertex_count = 0;
}

static void batch_begin(bd_menu_gl *gl, int mode, GLuint texture,
                        bd_menu_color color, float cx, float cy, float hx,
                        float hy, float radius) {
  batch_flush(gl);
  gl->gl.UseProgram(gl->program);
  gl->gl.Uniform2f(gl->uniform_viewport, (float)gl->width,
                   (float)gl->height);
  gl->gl.Uniform1i(gl->uniform_mode, mode);
  gl->gl.Uniform4f(gl->uniform_color, color.r, color.g, color.b, color.a);
  gl->gl.Uniform1i(gl->uniform_texture, 0);
  gl->gl.Uniform2f(gl->uniform_rect_center, cx, cy);
  gl->gl.Uniform2f(gl->uniform_rect_half, hx, hy);
  gl->gl.Uniform1f(gl->uniform_radius, radius);
  gl->gl.ActiveTexture(GL_TEXTURE0);
  gl->gl.BindTexture(GL_TEXTURE_2D, texture);
  gl->gl.BindBuffer(GL_ARRAY_BUFFER, gl->vbo);
  gl->gl.EnableVertexAttribArray(0);
  gl->gl.EnableVertexAttribArray(1);
  gl->gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(bd_vertex),
                             (const void *)0);
  gl->gl.VertexAttribPointer(
      1, 2, GL_FLOAT, GL_FALSE, sizeof(bd_vertex),
      (const void *)(uintptr_t)(2 * sizeof(float)));
}

static void batch_quad(bd_menu_gl *gl, float x0, float y0, float x1, float y1,
                       float u0, float v0, float u1, float v1) {
  if (gl->vertex_count + 6 > BD_MENU_MAX_VERTICES) {
    batch_flush(gl);
    if (gl->vertex_count + 6 > BD_MENU_MAX_VERTICES) {
      return;
    }
  }
  bd_vertex *v = &gl->vertices[gl->vertex_count];
  v[0] = (bd_vertex){x0, y0, u0, v0};
  v[1] = (bd_vertex){x1, y0, u1, v0};
  v[2] = (bd_vertex){x0, y1, u0, v1};
  v[3] = (bd_vertex){x1, y0, u1, v0};
  v[4] = (bd_vertex){x1, y1, u1, v1};
  v[5] = (bd_vertex){x0, y1, u0, v1};
  gl->vertex_count += 6;
}

static float sx(const bd_menu_gl *gl, float x) {
  return gl->origin_x + x * gl->scale;
}

static float sy(const bd_menu_gl *gl, float y) {
  return gl->origin_y + y * gl->scale;
}

static float sw(const bd_menu_gl *gl, float value) {
  return value * gl->scale;
}

static void fill_rect(bd_menu_gl *gl, float x, float y, float w, float h,
                      bd_menu_color color) {
  batch_begin(gl, 0, 0, color, 0, 0, 0, 0, 0);
  batch_quad(gl, x, y, x + w, y + h, 0, 0, 0, 0);
  batch_flush(gl);
}

static void round_rect(bd_menu_gl *gl, float x, float y, float w, float h,
                       float radius, bd_menu_color color) {
  float px = sx(gl, x);
  float py = sy(gl, y);
  float pw = sw(gl, w);
  float ph = sw(gl, h);
  float r = sw(gl, radius);
  float pad = 1.5f;
  batch_begin(gl, 2, 0, color, px + pw * 0.5f, py + ph * 0.5f, pw * 0.5f,
              ph * 0.5f, r);
  batch_quad(gl, px - pad, py - pad, px + pw + pad, py + ph + pad, 0, 0, 0, 0);
  batch_flush(gl);
}

static void draw_text(bd_menu_gl *gl, bd_menu_font *font, float x,
                      float baseline, bd_menu_color color, const char *text) {
  if (text == NULL || *text == '\0' || font->atlas == 0) {
    return;
  }
  batch_begin(gl, 1, font->atlas, color, 0, 0, 0, 0, 0);
  float pen = sx(gl, x);
  float base = sy(gl, baseline);
  const char *cursor = text;
  while (*cursor != '\0') {
    uint32_t codepoint = utf8_next(&cursor);
    if (codepoint == 0) {
      break;
    }
    const bd_menu_glyph *glyph = font_glyph(gl, font, codepoint);
    if (glyph == NULL) {
      continue;
    }
    if (glyph->width > 0 && glyph->height > 0) {
      float x0 = pen + sw(gl, (float)glyph->xoff);
      float y0 = base + sw(gl, (float)glyph->yoff);
      float x1 = x0 + sw(gl, (float)glyph->width);
      float y1 = y0 + sw(gl, (float)glyph->height);
      batch_quad(gl, x0, y0, x1, y1, glyph->u0, glyph->v0, glyph->u1,
                 glyph->v1);
    }
    pen += sw(gl, (float)glyph->advance);
  }
  batch_flush(gl);
}

static void draw_text_right(bd_menu_gl *gl, bd_menu_font *font, float right,
                            float baseline, bd_menu_color color,
                            const char *text) {
  draw_text(gl, font, right - (float)text_width(gl, font, text), baseline,
            color, text);
}

typedef struct bd_menu_hint {
  const char *key;
  const char *label;
} bd_menu_hint;

static const bd_menu_hint FooterHints[] = {
    {"A", "确认"},
    {"B", "返回"},
    {"←→", "调整"},
};

static int keycap_width(bd_menu_gl *gl, const char *key) {
  int width = text_width(gl, &gl->cap_font, key) + 12;
  return width < 22 ? 22 : width;
}

static int hint_item_width(bd_menu_gl *gl, const bd_menu_hint *item) {
  return keycap_width(gl, item->key) + 6 +
         text_width(gl, &gl->body_font, item->label);
}

static float draw_hint_keycap(bd_menu_gl *gl, float x, float y,
                              const char *key, bd_menu_color surface,
                              bd_menu_color ink) {
  int width = keycap_width(gl, key);
  bd_menu_color border = ink;
  border.a = 0.22f;
  bd_menu_color tint = ink;
  tint.a = 0.04f;
  round_rect(gl, x, y, (float)width, 22.0f, 4.0f, border);
  round_rect(gl, x + 1, y + 1, (float)width - 2.0f, 20.0f, 3.0f, surface);
  round_rect(gl, x + 1, y + 1, (float)width - 2.0f, 20.0f, 3.0f, tint);
  int label_width = text_width(gl, &gl->cap_font, key);
  draw_text(gl, &gl->cap_font, x + ((float)width - label_width) / 2.0f,
            y + 15.0f, ink, key);
  return (float)width;
}

static float draw_hint_item(bd_menu_gl *gl, float x, float y,
                            const bd_menu_hint *item, bd_menu_color surface,
                            bd_menu_color ink, bd_menu_color label_ink) {
  float key_width = draw_hint_keycap(gl, x, y, item->key, surface, ink);
  float label_x = x + key_width + 6.0f;
  draw_text(gl, &gl->body_font, label_x, y + 16.0f, label_ink, item->label);
  return label_x + (float)text_width(gl, &gl->body_font, item->label);
}

static void draw_background(bd_menu_gl *gl) {
  if (gl->background_valid && gl->background != 0) {
    batch_begin(gl, 3, gl->background, bd_menu_color_rgba(255, 255, 255, 255),
                0, 0, 0, 0, 0);
    batch_quad(gl, 0, 0, (float)gl->width, (float)gl->height, 0, 1, 1, 0);
    batch_flush(gl);
  } else {
    fill_rect(gl, 0, 0, (float)gl->width, (float)gl->height,
              bd_menu_color_rgba(0x1C, 0x19, 0x15, 255));
  }
}

/* --------------------------------------------------------------- lifecycle */

bd_menu_gl *bd_menu_gl_create(int width, int height, bd_menu_color accent,
                              const char *asset_dir) {
  if (width < 64 || height < 64 || width > 4096 || height > 4096) {
    return NULL;
  }
  bd_menu_gl *gl = (bd_menu_gl *)calloc(1, sizeof(*gl));
  if (gl == NULL) {
    return NULL;
  }
  if (!load_gl_api(&gl->gl)) {
    free(gl);
    return NULL;
  }
  gl->width = width;
  gl->height = height;
  gl->accent = accent;
  float scale_x = (float)width / (float)BD_MENU_DESIGN_WIDTH;
  float scale_y = (float)height / (float)BD_MENU_DESIGN_HEIGHT;
  gl->scale = scale_x < scale_y ? scale_x : scale_y;
  gl->origin_x = ((float)width - (float)BD_MENU_DESIGN_WIDTH * gl->scale) /
                 2.0f;
  gl->origin_y = ((float)height - (float)BD_MENU_DESIGN_HEIGHT * gl->scale) /
                 2.0f;

  save_state(gl);
  gl->gl.ActiveTexture(GL_TEXTURE0);
  gl->gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
  gl->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  gl->gl.Viewport(0, 0, width, height);
  gl->gl.Disable(GL_DEPTH_TEST);
  gl->gl.Disable(GL_CULL_FACE);
  gl->gl.Disable(GL_STENCIL_TEST);
  gl->gl.Disable(GL_SCISSOR_TEST);
  gl->gl.Enable(GL_BLEND);
  gl->gl.BlendEquation(GL_FUNC_ADD);
  gl->gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA,
                           GL_ONE_MINUS_SRC_ALPHA);

  if (!create_program(gl)) {
    restore_state(gl);
    free(gl);
    return NULL;
  }
  gl->gl.GenBuffers(1, &gl->vbo);
  gl->gl.GenTextures(1, &gl->background);

  char serif_path[1024];
  char sans_path[1024];
  snprintf(serif_path, sizeof(serif_path), "%s/fonts/NotoSerifSC-500-subset.ttf",
           asset_dir != NULL ? asset_dir : "");
  snprintf(sans_path, sizeof(sans_path), "%s/fonts/NotoSansSC-500-subset.ttf",
           asset_dir != NULL ? asset_dir : "");
  load_typeface(&gl->serif, serif_path);
  load_typeface(&gl->sans, sans_path);
  int title_size = (int)lroundf(30.0f * gl->scale);
  int body_size = (int)lroundf(20.0f * gl->scale);
  int cap_size = (int)lroundf(13.0f * gl->scale);
  if (title_size < 8) {
    title_size = 8;
  }
  if (body_size < 8) {
    body_size = 8;
  }
  if (cap_size < 8) {
    cap_size = 8;
  }
  bool title_ok =
      font_create(gl, &gl->title_font,
                  gl->serif.ok ? &gl->serif : &gl->sans, title_size);
  bool body_ok = font_create(gl, &gl->body_font, &gl->sans, body_size);
  bool cap_ok = font_create(gl, &gl->cap_font, &gl->sans, cap_size);
  if (!title_ok || !body_ok || !cap_ok) {
    restore_state(gl);
    if (gl->title_font.atlas != 0) {
      font_destroy(gl, &gl->title_font);
    }
    if (gl->body_font.atlas != 0) {
      font_destroy(gl, &gl->body_font);
    }
    if (gl->cap_font.atlas != 0) {
      font_destroy(gl, &gl->cap_font);
    }
    free(gl->serif.data);
    free(gl->sans.data);
    if (gl->vbo != 0) {
      gl->gl.DeleteBuffers(1, &gl->vbo);
    }
    if (gl->background != 0) {
      gl->gl.DeleteTextures(1, &gl->background);
    }
    gl->gl.DeleteProgram(gl->program);
    free(gl);
    return NULL;
  }
  return gl;
}

void bd_menu_gl_destroy(bd_menu_gl *gl) {
  if (gl == NULL) {
    return;
  }
  batch_flush(gl);
  restore_state(gl);
  font_destroy(gl, &gl->title_font);
  font_destroy(gl, &gl->body_font);
  font_destroy(gl, &gl->cap_font);
  if (gl->vbo != 0) {
    gl->gl.DeleteBuffers(1, &gl->vbo);
  }
  if (gl->background != 0) {
    gl->gl.DeleteTextures(1, &gl->background);
  }
  if (gl->program != 0) {
    gl->gl.DeleteProgram(gl->program);
  }
  free(gl->serif.data);
  free(gl->sans.data);
  free(gl);
}

bool bd_menu_gl_capture_background(bd_menu_gl *gl) {
  if (gl == NULL || gl->background == 0) {
    return false;
  }
  /*
   * Read back and upload instead of glCopyTexImage2D. The copy path is not
   * reliable on every GLES2/EGL configuration (and fails on multisampled or
   * single-buffered offscreen surfaces); this runs once per menu open.
   */
  size_t size = (size_t)gl->width * (size_t)gl->height * 4u;
  uint8_t *pixels = (uint8_t *)malloc(size);
  if (pixels == NULL) {
    return false;
  }
  gl->gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
  gl->gl.ReadPixels(0, 0, gl->width, gl->height, GL_RGBA, GL_UNSIGNED_BYTE,
                    pixels);
  gl->gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
  gl->gl.BindTexture(GL_TEXTURE_2D, gl->background);
  gl->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gl->width, gl->height, 0,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  free(pixels);
  gl->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl->background_valid = true;
  return true;
}

bool bd_menu_gl_read_pixels(bd_menu_gl *gl, bool top_down, uint8_t **out,
                            int *width, int *height) {
  if (gl == NULL || out == NULL) {
    return false;
  }
  size_t size = (size_t)gl->width * (size_t)gl->height * 4u;
  uint8_t *pixels = (uint8_t *)malloc(size);
  if (pixels == NULL) {
    return false;
  }
  gl->gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
  gl->gl.ReadPixels(0, 0, gl->width, gl->height, GL_RGBA, GL_UNSIGNED_BYTE,
                    pixels);
  gl->gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
  if (top_down) {
    size_t row = (size_t)gl->width * 4u;
    uint8_t *swap = (uint8_t *)malloc(row);
    if (swap == NULL) {
      free(pixels);
      return false;
    }
    for (int y = 0; y < gl->height / 2; ++y) {
      uint8_t *a = pixels + (size_t)y * row;
      uint8_t *b = pixels + (size_t)(gl->height - 1 - y) * row;
      memcpy(swap, a, row);
      memcpy(a, b, row);
      memcpy(b, swap, row);
    }
    free(swap);
  }
  *out = pixels;
  if (width != NULL) {
    *width = gl->width;
  }
  if (height != NULL) {
    *height = gl->height;
  }
  return true;
}

bool bd_menu_gl_dump_png(bd_menu_gl *gl, const char *path) {
  uint8_t *pixels = NULL;
  int width = 0;
  int height = 0;
  if (!bd_menu_gl_read_pixels(gl, true, &pixels, &width, &height)) {
    return false;
  }
  int ok = stbi_write_png(path, width, height, 4, pixels, width * 4);
  free(pixels);
  return ok != 0;
}

/* ------------------------------------------------------------------ layout */

typedef struct bd_menu_layout {
  float panel_x;
  float panel_y;
  float panel_w;
  float panel_h;
  float rows_y;
  float row_h;
  float row_gap;
  float footer_y;
} bd_menu_layout;

static bd_menu_layout layout_menu(void) {
  bd_menu_layout layout;
  layout.panel_w = 560.0f;
  layout.panel_h = 506.0f;
  layout.panel_x = ((float)BD_MENU_DESIGN_WIDTH - layout.panel_w) / 2.0f;
  layout.panel_y = ((float)BD_MENU_DESIGN_HEIGHT - layout.panel_h) / 2.0f;
  layout.row_h = 52.0f;
  layout.row_gap = 6.0f;
  layout.rows_y = layout.panel_y + 104.0f;
  layout.footer_y = layout.panel_y + layout.panel_h - 30.0f;
  return layout;
}

static const char *const ItemLabels[BD_MENU_ITEM_COUNT] = {
    "继续游戏", "返回桌面", "退出应用", "亮度", "音量", "性能模式"};

void bd_menu_gl_draw_menu(bd_menu_gl *gl, const bd_menu_model *model) {
  if (gl == NULL || model == NULL) {
    return;
  }
  bd_menu_layout layout = layout_menu();
  bd_menu_color surface = bd_menu_color_rgba(0x1C, 0x19, 0x15, 255);
  bd_menu_color surface2 = bd_menu_color_rgba(0x25, 0x21, 0x1B, 255);
  bd_menu_color surface3 = bd_menu_color_rgba(0x2F, 0x2A, 0x22, 255);
  bd_menu_color ink = bd_menu_color_rgba(0xF3, 0xED, 0xE2, 255);
  bd_menu_color ink2 = bd_menu_color_rgba(0xB9, 0xAF, 0xA0, 255);
  bd_menu_color ink3 = bd_menu_color_rgba(0x91, 0x88, 0x79, 255);
  bd_menu_color line = bd_menu_color_rgba(0xF3, 0xED, 0xE2, 31);
  bd_menu_color scrim = bd_menu_color_rgba(0, 0, 0, 158);

  draw_background(gl);
  fill_rect(gl, 0, 0, (float)gl->width, (float)gl->height, scrim);

  round_rect(gl, layout.panel_x - 1, layout.panel_y - 1, layout.panel_w + 2,
             layout.panel_h + 2, 21, line);
  round_rect(gl, layout.panel_x, layout.panel_y, layout.panel_w,
             layout.panel_h, 20, surface);

  draw_text(gl, &gl->title_font, layout.panel_x + 28, layout.panel_y + 58, ink,
            "菜单");
  if (model->status[0] != '\0') {
    draw_text(gl, &gl->body_font, layout.panel_x + 28, layout.panel_y + 86,
              model->accent, model->status);
  } else {
    draw_text(gl, &gl->body_font, layout.panel_x + 28, layout.panel_y + 86,
              ink3, "游戏已暂停");
  }

  for (int i = 0; i < BD_MENU_ITEM_COUNT; ++i) {
    float row_x = layout.panel_x + 16;
    float row_y = layout.rows_y + (float)i * (layout.row_h + layout.row_gap);
    float row_w = layout.panel_w - 32;
    bool focused = model->focus == i;
    if (focused) {
      round_rect(gl, row_x - 2, row_y - 2, row_w + 4, layout.row_h + 4, 14,
                 model->accent);
      round_rect(gl, row_x, row_y, row_w, layout.row_h, 12, surface2);
    }
    draw_text(gl, &gl->body_font, row_x + 18, row_y + 34,
              focused ? ink : ink2, ItemLabels[i]);

    float right = row_x + row_w - 18;
    float baseline = row_y + 34;
    char value[64];
    if (i == BD_MENU_ITEM_BRIGHTNESS || i == BD_MENU_ITEM_VOLUME) {
      int percent = i == BD_MENU_ITEM_BRIGHTNESS ? model->brightness
                                                 : model->volume;
      if (percent < 0) {
        /* No device, or the device did not confirm a value. */
        draw_text_right(gl, &gl->body_font, right, baseline, ink3, "不可用");
      } else {
        snprintf(value, sizeof(value), "%d%%", percent);
        draw_text_right(gl, &gl->body_font, right, baseline, ink2, value);
        float value_width = (float)text_width(gl, &gl->body_font, value);
        float track_w = 118.0f;
        float track_right = right - value_width - 16.0f;
        float track_left = track_right - track_w;
        float track_y = row_y + 23.0f;
        float track_h = 4.0f;
        round_rect(gl, track_left, track_y, track_w, track_h, 2, surface3);
        float fill_w = track_w * (float)percent / 100.0f;
        if (fill_w > 0) {
          round_rect(gl, track_left, track_y, fill_w, track_h, 2,
                     model->accent);
        }
        float knob = 12.0f;
        float knob_x = track_left + fill_w - knob / 2.0f;
        if (knob_x < track_left - knob / 2.0f) {
          knob_x = track_left - knob / 2.0f;
        }
        round_rect(gl, knob_x, track_y + track_h / 2.0f - knob / 2.0f, knob,
                   knob, knob / 2.0f, model->accent);
      }
    } else if (i == BD_MENU_ITEM_PERF) {
      if (model->perf_available && model->profile_count > 0) {
        const char *profile = model->profiles[model->profile_index];
        if (focused) {
          snprintf(value, sizeof(value), "< %s >", profile);
        } else {
          snprintf(value, sizeof(value), "%s", profile);
        }
        draw_text_right(gl, &gl->body_font, right, baseline,
                        focused ? ink : ink2, value);
      } else {
        draw_text_right(gl, &gl->body_font, right, baseline, ink3, "不可用");
      }
    }
  }

  int left_width = hint_item_width(gl, &FooterHints[0]) + 16 +
                   hint_item_width(gl, &FooterHints[1]);
  int right_width = hint_item_width(gl, &FooterHints[2]);
  int footer_width = left_width + 44 + right_width;
  float footer_x =
      layout.panel_x + layout.panel_w / 2.0f - (float)footer_width / 2.0f;
  float keycap_y = layout.footer_y - 16.0f;
  float left_end = draw_hint_item(gl, footer_x, keycap_y, &FooterHints[0],
                                  surface, ink, ink2);
  left_end = draw_hint_item(gl, left_end + 16.0f, keycap_y, &FooterHints[1],
                            surface, ink, ink2);
  bd_menu_color divider = ink;
  divider.a = 0.22f;
  fill_rect(gl, left_end + 24.0f, keycap_y + 2.0f, 1.0f, 18.0f, divider);
  draw_hint_item(gl, left_end + 44.0f, keycap_y, &FooterHints[2], surface,
                 ink, ink2);
}

void bd_menu_gl_draw_background(bd_menu_gl *gl) {
  if (gl == NULL) {
    return;
  }
  draw_background(gl);
}
