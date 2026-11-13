#include "gfx_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct bd_attrib_binding {
  GLuint index;
  const char *name;
} bd_attrib_binding;

static const char *const MAIN_VERTEX_SHADER =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "attribute vec4 a_color;\n"
    "attribute vec4 a_shape;\n"
    "attribute float a_radius;\n"
    "uniform vec2 u_viewport;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "varying vec2 v_local;\n"
    "varying vec4 v_shape;\n"
    "varying float v_radius;\n"
    "void main() {\n"
    "  vec2 ndc = vec2(a_pos.x / u_viewport.x * 2.0 - 1.0,\n"
    "                  1.0 - a_pos.y / u_viewport.y * 2.0);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "  v_color = a_color;\n"
    "  v_local = a_pos;\n"
    "  v_shape = a_shape;\n"
    "  v_radius = a_radius;\n"
    "}\n";

static const char *const MAIN_FRAGMENT_SHADER =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform sampler2D u_tex;\n"
    "uniform mediump int u_mode;\n"
    "uniform mediump int u_flip;\n"
    "uniform mediump float u_stroke;\n"
    "uniform vec4 u_grad0;\n"
    "uniform vec4 u_grad1;\n"
    "uniform vec4 u_grad2;\n"
    "uniform vec4 u_grad_rect;\n"
    "uniform mediump int u_grad_axis;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "varying vec2 v_local;\n"
    "varying vec4 v_shape;\n"
    "varying float v_radius;\n"
    "void main() {\n"
    "  vec4 color = v_color;\n"
    "  if (u_mode == 1) {\n"
    "    color.a *= texture2D(u_tex, v_uv).r;\n"
    "  } else if (u_mode == 2) {\n"
    "    vec2 uv = v_uv;\n"
    "    if (u_flip == 1) uv.y = 1.0 - uv.y;\n"
    "    color *= texture2D(u_tex, uv);\n"
    "  } else if (u_mode == 3) {\n"
    "    float t = u_grad_axis == 1\n"
    "        ? (v_local.x - u_grad_rect.x) / u_grad_rect.z\n"
    "        : (v_local.y - u_grad_rect.y) / u_grad_rect.w;\n"
    "    t = clamp(t, 0.0, 1.0);\n"
    "    color = t < 0.5 ? mix(u_grad0, u_grad1, t * 2.0)\n"
    "                    : mix(u_grad1, u_grad2, (t - 0.5) * 2.0);\n"
    "  }\n"
    "  float coverage = 1.0;\n"
    "  if (v_radius >= 0.0) {\n"
    "    vec2 q = abs(v_local - v_shape.xy) - (v_shape.zw - vec2(v_radius));\n"
    "    float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - v_radius;\n"
    "    float aa = max(BD_AA(d), 0.0001);\n"
    "    if (u_stroke > 0.0) {\n"
    "      coverage = 1.0 - smoothstep(u_stroke * 0.5 - aa,\n"
    "                                  u_stroke * 0.5 + aa,\n"
    "                                  abs(d + u_stroke * 0.5));\n"
    "    } else {\n"
    "      coverage = 1.0 - smoothstep(-aa, aa, d);\n"
    "    }\n"
    "  }\n"
    "  gl_FragColor = vec4(color.rgb, color.a * coverage);\n"
    "}\n";

static const char *const PASS_VERTEX_SHADER =
    "attribute vec2 a_pos;\n"
    "uniform vec4 u_uv_rect;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos * 2.0 - 1.0, 0.0, 1.0);\n"
    "  v_uv = mix(u_uv_rect.xy, u_uv_rect.zw, a_pos);\n"
    "}\n";

static const char *const COPY_FRAGMENT_SHADER =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(u_tex, v_uv);\n"
    "}\n";

static const char *const BLUR_FRAGMENT_SHADER =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_step;\n"
    "uniform vec2 u_uv_max;\n"
    "varying vec2 v_uv;\n"
    "vec4 tap(vec2 offset) {\n"
    "  return texture2D(u_tex, clamp(v_uv + offset, vec2(0.0), u_uv_max));\n"
    "}\n"
    "void main() {\n"
    "  vec4 c = tap(vec2(0.0)) * 0.2041;\n"
    "  c += (tap(u_step) + tap(-u_step)) * 0.1802;\n"
    "  c += (tap(u_step * 2.0) + tap(-u_step * 2.0)) * 0.1238;\n"
    "  c += (tap(u_step * 3.0) + tap(-u_step * 3.0)) * 0.0663;\n"
    "  c += (tap(u_step * 4.0) + tap(-u_step * 4.0)) * 0.0276;\n"
    "  gl_FragColor = c;\n"
    "}\n";

static const char *const COMPOSITE_VERTEX_SHADER =
    "attribute vec2 a_pos;\n"
    "uniform vec4 u_rect;\n"
    "uniform vec2 u_viewport;\n"
    "varying vec2 v_uv;\n"
    "varying vec2 v_local;\n"
    "void main() {\n"
    "  vec2 px = u_rect.xy + a_pos * u_rect.zw;\n"
    "  vec2 ndc = vec2(px.x / u_viewport.x * 2.0 - 1.0,\n"
    "                  1.0 - px.y / u_viewport.y * 2.0);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_uv = a_pos;\n"
    "  v_local = px;\n"
    "}\n";

static const char *const COMPOSITE_FRAGMENT_SHADER =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_center;\n"
    "uniform vec2 u_half;\n"
    "uniform float u_radius;\n"
    "uniform vec4 u_rect;\n"
    "uniform vec2 u_uv_scale;\n"
    "uniform vec4 u_overlay0;\n"
    "uniform vec4 u_overlay1;\n"
    "uniform mediump int u_axis;\n"
    "uniform float u_feather;\n"
    "uniform mediump int u_feather_edge;\n"
    "varying vec2 v_uv;\n"
    "varying vec2 v_local;\n"
    "void main() {\n"
    "  vec4 blurred = texture2D(u_tex, vec2(v_uv.x * u_uv_scale.x,\n"
    "                                       (1.0 - v_uv.y) * u_uv_scale.y));\n"
    "  float t = u_axis == 1 ? v_uv.x : v_uv.y;\n"
    "  vec4 overlay = mix(u_overlay0, u_overlay1, t);\n"
    "  vec3 rgb = mix(blurred.rgb, overlay.rgb, overlay.a);\n"
    "  float coverage = 1.0;\n"
    "  if (u_radius >= 0.0) {\n"
    "    vec2 q = abs(v_local - u_center) - (u_half - vec2(u_radius));\n"
    "    float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - u_radius;\n"
    "    float aa = max(BD_AA(d), 0.0001);\n"
    "    coverage = 1.0 - smoothstep(-aa, aa, d);\n"
    "  }\n"
    "  float fade = 1.0;\n"
    "  if (u_feather > 0.0) {\n"
    "    float distance = u_feather_edge == 1 ? v_uv.y * u_rect.w\n"
    "                                        : (1.0 - v_uv.y) * u_rect.w;\n"
    "    fade = smoothstep(0.0, 1.0, clamp(distance / u_feather, 0.0, 1.0));\n"
    "  }\n"
    "  gl_FragColor = vec4(rgb, coverage * fade);\n"
    "}\n";

static const GLfloat PASS_QUAD[] = {
    0.0f, 0.0f,
    1.0f, 0.0f,
    0.0f, 1.0f,
    1.0f, 1.0f,
};

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
                           const char *fragment_source,
                           const bd_attrib_binding *attributes, int attribute_count) {
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
  for (int i = 0; i < attribute_count; ++i) {
    glBindAttribLocation(program, attributes[i].index, attributes[i].name);
  }
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

static char *shader_with_aa(const char *body, bool derivatives) {
  const char *prefix = derivatives
                           ? "#extension GL_OES_standard_derivatives : enable\n"
                             "#define BD_AA(d) fwidth(d)\n"
                           : "#define BD_AA(d) 0.75\n";
  size_t prefix_length = strlen(prefix);
  size_t body_length = strlen(body);
  char *source = (char *)malloc(prefix_length + body_length + 1);
  if (source == NULL) {
    return NULL;
  }
  memcpy(source, prefix, prefix_length);
  memcpy(source + prefix_length, body, body_length + 1);
  return source;
}

static bool has_standard_derivatives(void) {
  const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
  return extensions != NULL && strstr(extensions, "GL_OES_standard_derivatives") != NULL;
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

bd_color bd_color_mix(bd_color a, bd_color b, float t) {
  bd_color color;
  color.r = a.r + (b.r - a.r) * t;
  color.g = a.g + (b.g - a.g) * t;
  color.b = a.b + (b.b - a.b) * t;
  color.a = a.a + (b.a - a.a) * t;
  return color;
}

bd_rect bd_rect_make(int x, int y, int w, int h) {
  bd_rect rect;
  rect.x = x;
  rect.y = y;
  rect.w = w;
  rect.h = h;
  return rect;
}

bd_rect bd_rect_intersect(bd_rect a, bd_rect b) {
  int x0 = a.x > b.x ? a.x : b.x;
  int y0 = a.y > b.y ? a.y : b.y;
  int ax1 = a.x + a.w;
  int bx1 = b.x + b.w;
  int ay1 = a.y + a.h;
  int by1 = b.y + b.h;
  int x1 = ax1 < bx1 ? ax1 : bx1;
  int y1 = ay1 < by1 ? ay1 : by1;
  if (x1 <= x0 || y1 <= y0) {
    return bd_rect_make(0, 0, 0, 0);
  }
  return bd_rect_make(x0, y0, x1 - x0, y1 - y0);
}

int bd_rect_area(bd_rect rect) {
  if (rect.w <= 0 || rect.h <= 0) {
    return 0;
  }
  return rect.w * rect.h;
}

GLuint bd_gfx_create_gl_texture(bd_gfx *gfx, int width, int height,
                                bd_texture_filter filter, GLenum internal_format,
                                GLenum format, GLenum type, const void *pixels) {
  if (gfx == NULL || width <= 0 || height <= 0) {
    return 0;
  }
  GLuint texture = 0;
  glGenTextures(1, &texture);
  if (texture == 0) {
    bd_gfx_set_error(gfx, "glGenTextures failed");
    return 0;
  }
  glBindTexture(GL_TEXTURE_2D, texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal_format, width, height, 0, format,
               type, pixels);
  GLint gl_filter = filter == BD_TEXTURE_LINEAR ? GL_LINEAR : GL_NEAREST;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    char message[64];
    snprintf(message, sizeof(message), "glTexImage2D failed (0x%04x)", (unsigned)error);
    bd_gfx_set_error(gfx, message);
    glDeleteTextures(1, &texture);
    return 0;
  }
  return texture;
}

bd_texture *bd_gfx_texture_create_internal(bd_gfx *gfx, int width, int height,
                                           bd_texture_filter filter,
                                           GLenum internal_format, GLenum format,
                                           GLenum type, const void *pixels,
                                           bool cache_managed, bool opaque) {
  GLuint id = bd_gfx_create_gl_texture(gfx, width, height, filter, internal_format,
                                       format, type, pixels);
  if (id == 0) {
    return NULL;
  }
  bd_texture *texture = (bd_texture *)calloc(1, sizeof(*texture));
  if (texture == NULL) {
    glDeleteTextures(1, &id);
    bd_gfx_set_error(gfx, "out of memory for texture");
    return NULL;
  }
  texture->id = id;
  texture->width = width;
  texture->height = height;
  texture->opaque = opaque;
  texture->cache_managed = cache_managed;
  return texture;
}

static bool create_scene_target(bd_gfx *gfx) {
  gfx->scene_texture = bd_gfx_create_gl_texture(
      gfx, gfx->width, gfx->height, BD_TEXTURE_LINEAR, GL_RGBA, GL_RGBA,
      GL_UNSIGNED_BYTE, NULL);
  if (gfx->scene_texture == 0) {
    return false;
  }
  glGenFramebuffers(1, &gfx->scene_fbo);
  if (gfx->scene_fbo == 0) {
    bd_gfx_set_error(gfx, "glGenFramebuffers failed");
    return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, gfx->scene_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         gfx->scene_texture, 0);
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    bd_gfx_set_error(gfx, "scene framebuffer is incomplete");
    return false;
  }
  return true;
}

static bool create_blur_target(bd_gfx *gfx) {
  gfx->blur_width = (gfx->width + BD_GFX_BLUR_DIVISOR - 1) / BD_GFX_BLUR_DIVISOR;
  gfx->blur_height = (gfx->height + BD_GFX_BLUR_DIVISOR - 1) / BD_GFX_BLUR_DIVISOR;
  if (gfx->blur_width < 1) {
    gfx->blur_width = 1;
  }
  if (gfx->blur_height < 1) {
    gfx->blur_height = 1;
  }
  for (int i = 0; i < 2; ++i) {
    gfx->blur_texture[i] = bd_gfx_create_gl_texture(
        gfx, gfx->blur_width, gfx->blur_height, BD_TEXTURE_LINEAR, GL_RGBA,
        GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if (gfx->blur_texture[i] == 0) {
      return false;
    }
  }
  glGenFramebuffers(1, &gfx->blur_fbo);
  if (gfx->blur_fbo == 0) {
    bd_gfx_set_error(gfx, "glGenFramebuffers failed");
    return false;
  }
  for (int i = 0; i < 2; ++i) {
    glBindFramebuffer(GL_FRAMEBUFFER, gfx->blur_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           gfx->blur_texture[i], 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      bd_gfx_set_error(gfx, "blur framebuffer is incomplete");
      return false;
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return true;
}

static bool create_buffers(bd_gfx *gfx) {
  glGenBuffers(1, &gfx->vbo);
  glBindBuffer(GL_ARRAY_BUFFER, gfx->vbo);
  glBufferData(GL_ARRAY_BUFFER,
               (GLsizeiptr)(BD_GFX_MAX_QUADS * 4 * (int)sizeof(bd_vertex)), NULL,
               GL_STREAM_DRAW);

  glGenBuffers(1, &gfx->ibo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gfx->ibo);
  size_t index_count = (size_t)BD_GFX_MAX_QUADS * 6u;
  uint16_t *indices = (uint16_t *)malloc(index_count * sizeof(uint16_t));
  if (indices == NULL) {
    bd_gfx_set_error(gfx, "out of memory for index buffer");
    return false;
  }
  size_t cursor = 0;
  for (int quad = 0; quad < BD_GFX_MAX_QUADS; ++quad) {
    uint16_t base = (uint16_t)(quad * 4);
    indices[cursor++] = (uint16_t)(base + 0);
    indices[cursor++] = (uint16_t)(base + 1);
    indices[cursor++] = (uint16_t)(base + 2);
    indices[cursor++] = (uint16_t)(base + 2);
    indices[cursor++] = (uint16_t)(base + 1);
    indices[cursor++] = (uint16_t)(base + 3);
  }
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(index_count * sizeof(uint16_t)),
               indices, GL_STATIC_DRAW);
  free(indices);

  glGenBuffers(1, &gfx->pass_quad_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, gfx->pass_quad_vbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(PASS_QUAD), PASS_QUAD,
               GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

  static const uint8_t white_pixel[4] = {255, 255, 255, 255};
  gfx->white_texture = bd_gfx_create_gl_texture(
      gfx, 1, 1, BD_TEXTURE_NEAREST, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE,
      white_pixel);
  return gfx->white_texture != 0;
}

static bool create_programs(bd_gfx *gfx) {
  static const bd_attrib_binding main_attributes[] = {
      {0, "a_pos"}, {1, "a_uv"}, {2, "a_color"}, {3, "a_shape"}, {4, "a_radius"},
  };
  static const bd_attrib_binding pass_attributes[] = {
      {0, "a_pos"},
  };

  bool derivatives = has_standard_derivatives();
  char *main_fragment = shader_with_aa(MAIN_FRAGMENT_SHADER, derivatives);
  char *composite_fragment = shader_with_aa(COMPOSITE_FRAGMENT_SHADER, derivatives);
  if (main_fragment == NULL || composite_fragment == NULL) {
    free(main_fragment);
    free(composite_fragment);
    bd_gfx_set_error(gfx, "out of memory for shader sources");
    return false;
  }

  gfx->main_program = link_program(gfx, MAIN_VERTEX_SHADER, main_fragment,
                                   main_attributes, 5);
  gfx->copy_program = link_program(gfx, PASS_VERTEX_SHADER, COPY_FRAGMENT_SHADER,
                                   pass_attributes, 1);
  gfx->blur_program = link_program(gfx, PASS_VERTEX_SHADER, BLUR_FRAGMENT_SHADER,
                                   pass_attributes, 1);
  gfx->composite_program = link_program(gfx, COMPOSITE_VERTEX_SHADER,
                                        composite_fragment, pass_attributes, 1);
  free(main_fragment);
  free(composite_fragment);

  if (gfx->main_program == 0 || gfx->copy_program == 0 || gfx->blur_program == 0 ||
      gfx->composite_program == 0) {
    return false;
  }

  gfx->main_viewport = glGetUniformLocation(gfx->main_program, "u_viewport");
  gfx->main_tex = glGetUniformLocation(gfx->main_program, "u_tex");
  gfx->main_mode = glGetUniformLocation(gfx->main_program, "u_mode");
  gfx->main_flip = glGetUniformLocation(gfx->main_program, "u_flip");
  gfx->main_stroke = glGetUniformLocation(gfx->main_program, "u_stroke");
  gfx->main_grad0 = glGetUniformLocation(gfx->main_program, "u_grad0");
  gfx->main_grad1 = glGetUniformLocation(gfx->main_program, "u_grad1");
  gfx->main_grad2 = glGetUniformLocation(gfx->main_program, "u_grad2");
  gfx->main_grad_rect = glGetUniformLocation(gfx->main_program, "u_grad_rect");
  gfx->main_grad_axis = glGetUniformLocation(gfx->main_program, "u_grad_axis");

  gfx->copy_uv_rect = glGetUniformLocation(gfx->copy_program, "u_uv_rect");
  gfx->copy_tex = glGetUniformLocation(gfx->copy_program, "u_tex");
  gfx->blur_tex = glGetUniformLocation(gfx->blur_program, "u_tex");
  gfx->blur_step = glGetUniformLocation(gfx->blur_program, "u_step");
  gfx->blur_uv_rect = glGetUniformLocation(gfx->blur_program, "u_uv_rect");
  gfx->blur_uv_max = glGetUniformLocation(gfx->blur_program, "u_uv_max");
  if (gfx->blur_uv_rect < 0 || gfx->blur_uv_max < 0) {
    bd_gfx_set_error(gfx, "missing blur uniforms");
    return false;
  }

  gfx->composite_tex = glGetUniformLocation(gfx->composite_program, "u_tex");
  gfx->composite_rect = glGetUniformLocation(gfx->composite_program, "u_rect");
  gfx->composite_viewport =
      glGetUniformLocation(gfx->composite_program, "u_viewport");
  gfx->composite_center =
      glGetUniformLocation(gfx->composite_program, "u_center");
  gfx->composite_half = glGetUniformLocation(gfx->composite_program, "u_half");
  gfx->composite_radius =
      glGetUniformLocation(gfx->composite_program, "u_radius");
  gfx->composite_uv_scale =
      glGetUniformLocation(gfx->composite_program, "u_uv_scale");
  gfx->composite_overlay0 =
      glGetUniformLocation(gfx->composite_program, "u_overlay0");
  gfx->composite_overlay1 =
      glGetUniformLocation(gfx->composite_program, "u_overlay1");
  gfx->composite_axis = glGetUniformLocation(gfx->composite_program, "u_axis");
  gfx->composite_feather =
      glGetUniformLocation(gfx->composite_program, "u_feather");
  gfx->composite_feather_edge =
      glGetUniformLocation(gfx->composite_program, "u_feather_edge");

  if (gfx->main_viewport < 0 || gfx->main_tex < 0 || gfx->main_mode < 0 ||
      gfx->main_flip < 0 || gfx->main_stroke < 0 || gfx->main_grad0 < 0 ||
      gfx->main_grad1 < 0 || gfx->main_grad2 < 0 || gfx->main_grad_rect < 0 ||
      gfx->main_grad_axis < 0 || gfx->copy_uv_rect < 0 || gfx->copy_tex < 0 ||
      gfx->blur_tex < 0 || gfx->blur_step < 0 || gfx->composite_tex < 0 ||
      gfx->composite_rect < 0 || gfx->composite_viewport < 0 ||
      gfx->composite_center < 0 || gfx->composite_half < 0 ||
      gfx->composite_radius < 0 || gfx->composite_uv_scale < 0 ||
      gfx->composite_overlay0 < 0 || gfx->composite_overlay1 < 0 ||
      gfx->composite_axis < 0 || gfx->composite_feather < 0 ||
      gfx->composite_feather_edge < 0) {
    bd_gfx_set_error(gfx, "missing shader uniforms");
    return false;
  }
  return true;
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
  gfx->image_budget = 16u * 1024u * 1024u;
  gfx->image_next_id = 1;

  gfx->vertices = (bd_vertex *)calloc((size_t)BD_GFX_MAX_QUADS * 4u,
                                      sizeof(bd_vertex));
  gfx->batches = (bd_batch *)calloc(BD_GFX_MAX_BATCHES, sizeof(bd_batch));
  if (gfx->vertices == NULL || gfx->batches == NULL) {
    bd_gfx_set_error(gfx, "out of memory for renderer buffers");
    bd_gfx_destroy(gfx);
    return NULL;
  }

  gfx->damage_all = true;
  glDisable(GL_DITHER);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  if (!create_programs(gfx) || !create_buffers(gfx) || !create_scene_target(gfx) ||
      !create_blur_target(gfx) || !bd_gfx_image_init(gfx)) {
    fprintf(stderr, "gfx: %s\n", gfx->error);
    bd_gfx_destroy(gfx);
    return NULL;
  }

  glUseProgram(0);
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
  bd_gfx_image_shutdown(gfx);
  if (gfx->vbo != 0) {
    glDeleteBuffers(1, &gfx->vbo);
  }
  if (gfx->ibo != 0) {
    glDeleteBuffers(1, &gfx->ibo);
  }
  if (gfx->pass_quad_vbo != 0) {
    glDeleteBuffers(1, &gfx->pass_quad_vbo);
  }
  if (gfx->white_texture != 0) {
    glDeleteTextures(1, &gfx->white_texture);
  }
  if (gfx->scene_texture != 0) {
    glDeleteTextures(1, &gfx->scene_texture);
  }
  if (gfx->scene_fbo != 0) {
    glDeleteFramebuffers(1, &gfx->scene_fbo);
  }
  if (gfx->blur_texture[0] != 0) {
    glDeleteTextures(1, &gfx->blur_texture[0]);
  }
  if (gfx->blur_texture[1] != 0) {
    glDeleteTextures(1, &gfx->blur_texture[1]);
  }
  if (gfx->blur_fbo != 0) {
    glDeleteFramebuffers(1, &gfx->blur_fbo);
  }
  if (gfx->main_program != 0) {
    glDeleteProgram(gfx->main_program);
  }
  if (gfx->copy_program != 0) {
    glDeleteProgram(gfx->copy_program);
  }
  if (gfx->blur_program != 0) {
    glDeleteProgram(gfx->blur_program);
  }
  if (gfx->composite_program != 0) {
    glDeleteProgram(gfx->composite_program);
  }
  bd_font_file *font_file = gfx->font_files;
  while (font_file != NULL) {
    bd_font_file *next = font_file->next;
    free(font_file->path);
    free(font_file->data);
    free(font_file);
    font_file = next;
  }
  free(gfx->vertices);
  free(gfx->batches);
  free(gfx);
}

int bd_gfx_width(const bd_gfx *gfx) {
  return gfx != NULL ? gfx->width : 0;
}

int bd_gfx_height(const bd_gfx *gfx) {
  return gfx != NULL ? gfx->height : 0;
}

bd_gfx_stats bd_gfx_get_stats(const bd_gfx *gfx) {
  bd_gfx_stats empty;
  memset(&empty, 0, sizeof(empty));
  return gfx != NULL ? gfx->stats : empty;
}

bd_gfx_memory_stats bd_gfx_memory_get_stats(const bd_gfx *gfx) {
  bd_gfx_memory_stats stats;
  memset(&stats, 0, sizeof(stats));
  if (gfx == NULL) {
    return stats;
  }
  bd_image_cache_stats images = bd_gfx_image_cache_get_stats(gfx);
  stats.texture_cache_bytes = images.resident_bytes;
  stats.texture_cache_budget_bytes = images.budget_bytes;
  stats.glyph_atlas_bytes = gfx->glyph_atlas_bytes;
  stats.font_data_bytes = gfx->font_data_bytes;
  stats.fbo_bytes = (size_t)gfx->width * (size_t)gfx->height * 4u +
                    2u * (size_t)gfx->blur_width * (size_t)gfx->blur_height * 4u;
  return stats;
}

void bd_gfx_damage_all(bd_gfx *gfx) {
  bd_gfx_batch_damage_all(gfx);
}

void bd_gfx_damage_rect(bd_gfx *gfx, bd_rect rect) {
  bd_gfx_batch_damage_rect(gfx, rect);
}

bool bd_gfx_has_damage(const bd_gfx *gfx) {
  return bd_gfx_batch_has_damage(gfx);
}

int bd_gfx_damage_rects(const bd_gfx *gfx, bd_rect *out, int capacity) {
  return bd_gfx_batch_damage_rects(gfx, out, capacity);
}

void bd_gfx_begin_frame(bd_gfx *gfx) {
  if (gfx == NULL || gfx->in_frame) {
    return;
  }
  bd_gfx_batch_begin_frame(gfx);
  bd_gfx_image_poll(gfx);

  glBindFramebuffer(GL_FRAMEBUFFER, gfx->scene_fbo);
  glViewport(0, 0, gfx->width, gfx->height);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DITHER);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

static void present_scene(bd_gfx *gfx) {
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, gfx->width, gfx->height);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glUseProgram(gfx->copy_program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gfx->scene_texture);
  glUniform1i(gfx->copy_tex, 0);
  glUniform4f(gfx->copy_uv_rect, 0.0f, 0.0f, 1.0f, 1.0f);
  glBindBuffer(GL_ARRAY_BUFFER, gfx->pass_quad_vbo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  ++gfx->stats.present_draw_calls;
  gfx->stats.presented = true;
}

void bd_gfx_end_frame(bd_gfx *gfx) {
  if (gfx == NULL || !gfx->in_frame) {
    return;
  }
  bd_gfx_batch_finalize_damage(gfx);
  bool has_output = gfx->damage_final_count > 0;

  if (has_output) {
    bd_gfx_batch_flush(gfx);
    present_scene(gfx);
    gfx->blur_active_count = gfx->blur_frame_count;
    for (int i = 0; i < gfx->blur_frame_count; ++i) {
      gfx->blur_active[i] = gfx->blur_frame[i];
    }
  } else {
    bd_gfx_batch_reset(gfx);
  }

  bd_gfx_image_end_frame(gfx);
  bd_gfx_batch_after_frame(gfx);
}

static int clamp_radius(int radius, bd_rect rect) {
  int maximum = rect.w < rect.h ? rect.w : rect.h;
  maximum /= 2;
  if (radius <= 0) {
    return 0;
  }
  if (radius > maximum) {
    radius = maximum;
  }
  return radius;
}

static bd_rect clamp_blur_rect(const bd_gfx *gfx, bd_rect rect) {
  return bd_rect_intersect(rect, bd_rect_make(0, 0, gfx->width, gfx->height));
}

void bd_gfx_clear(bd_gfx *gfx, bd_color color) {
  if (gfx == NULL) {
    return;
  }
  bd_gfx_fill_rect(gfx, bd_rect_make(0, 0, gfx->width, gfx->height), color);
}

void bd_gfx_fill_rect(bd_gfx *gfx, bd_rect rect, bd_color color) {
  if (gfx == NULL || !gfx->in_frame || rect.w <= 0 || rect.h <= 0) {
    return;
  }
  bd_color colors[4] = {color, color, color, color};
  bd_gfx_batch_add_quad(gfx, gfx->white_texture, BD_GFX_MODE_SOLID, 0, 0.0f, rect,
                        -1, 0.0f, 0.0f, 1.0f, 1.0f, colors);
}

void bd_gfx_fill_round_rect(bd_gfx *gfx, bd_rect rect, int radius, bd_color color) {
  if (gfx == NULL || rect.w <= 0 || rect.h <= 0) {
    return;
  }
  radius = clamp_radius(radius, rect);
  if (radius <= 0) {
    bd_gfx_fill_rect(gfx, rect, color);
    return;
  }
  bd_color colors[4] = {color, color, color, color};
  bd_gfx_batch_add_quad(gfx, gfx->white_texture, BD_GFX_MODE_SOLID, 0, 0.0f, rect,
                        radius, 0.0f, 0.0f, 1.0f, 1.0f, colors);
}

void bd_gfx_stroke_round_rect(bd_gfx *gfx, bd_rect rect, int radius, int thickness,
                              bd_color color) {
  if (gfx == NULL || rect.w <= 1 || rect.h <= 1 || thickness <= 0) {
    return;
  }
  radius = clamp_radius(radius, rect);
  int maximum = (rect.w < rect.h ? rect.w : rect.h) / 2;
  if (thickness > maximum) {
    thickness = maximum;
  }
  bd_color colors[4] = {color, color, color, color};
  bd_gfx_batch_add_quad(gfx, gfx->white_texture, BD_GFX_MODE_SOLID, 0,
                        (float)thickness, rect, radius, 0.0f, 0.0f, 1.0f, 1.0f,
                        colors);
}

void bd_gfx_fill_gradient(bd_gfx *gfx, bd_rect rect, int radius, bd_color start,
                          bd_color end, bd_gradient_axis axis) {
  if (gfx == NULL || rect.w <= 0 || rect.h <= 0) {
    return;
  }
  radius = clamp_radius(radius, rect);
  bd_color colors[4];
  if (axis == BD_GRADIENT_HORIZONTAL) {
    colors[0] = start;
    colors[1] = end;
    colors[2] = start;
    colors[3] = end;
  } else {
    colors[0] = start;
    colors[1] = start;
    colors[2] = end;
    colors[3] = end;
  }
  bd_gfx_batch_add_quad(gfx, gfx->white_texture, BD_GFX_MODE_SOLID, 0, 0.0f, rect,
                        radius > 0 ? radius : -1, 0.0f, 0.0f, 1.0f, 1.0f, colors);
}

void bd_gfx_fill_gradient3(bd_gfx *gfx, bd_rect rect, int radius, bd_color start,
                           bd_color middle, bd_color end, bd_gradient_axis axis) {
  if (gfx == NULL || rect.w <= 0 || rect.h <= 0) {
    return;
  }
  radius = clamp_radius(radius, rect);
  bd_gfx_batch_add_gradient3(gfx, rect, radius > 0 ? radius : -1, start, middle,
                             end, axis);
}

void bd_gfx_push_clip(bd_gfx *gfx, bd_rect rect) {
  bd_gfx_batch_push_clip(gfx, rect);
}

void bd_gfx_pop_clip(bd_gfx *gfx) {
  bd_gfx_batch_pop_clip(gfx);
}

static void bind_unit_quad(bd_gfx *gfx) {
  glBindBuffer(GL_ARRAY_BUFFER, gfx->pass_quad_vbo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
}

static void draw_unit_quad(bd_gfx *gfx) {
  bind_unit_quad(gfx);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  ++gfx->stats.draw_calls;
}

void bd_gfx_blur_region(bd_gfx *gfx, bd_rect rect, int radius, bd_color overlay_start,
                        bd_color overlay_end, bd_gradient_axis axis) {
  bd_gfx_blur_region_feathered(gfx, rect, radius, overlay_start, overlay_end, axis,
                               BD_BLUR_EDGE_NONE, 0);
}

void bd_gfx_blur_region_feathered(bd_gfx *gfx, bd_rect rect, int radius,
                                  bd_color overlay_start, bd_color overlay_end,
                                  bd_gradient_axis axis, bd_blur_edge edge,
                                  int feather) {
  if (gfx == NULL || !gfx->in_frame) {
    return;
  }
  rect = clamp_blur_rect(gfx, rect);
  if (rect.w <= 0 || rect.h <= 0) {
    return;
  }

  if (gfx->blur_frame_count < BD_GFX_MAX_BLUR_REGIONS) {
    gfx->blur_frame[gfx->blur_frame_count++] = rect;
  }

  bd_gfx_batch_flush(gfx);
  bd_gfx_batch_finalize_damage(gfx);
  if (gfx->damage_final_count == 0) {
    return;
  }

  bd_rect clip = bd_gfx_batch_current_clip(gfx);
  bd_rect clipped = bd_rect_intersect(rect, clip);
  if (clipped.w <= 0 || clipped.h <= 0) {
    return;
  }

  bool intersects = false;
  for (int i = 0; i < gfx->damage_final_count; ++i) {
    bd_rect hit = bd_rect_intersect(gfx->damage_final[i], clipped);
    if (hit.w > 0 && hit.h > 0) {
      intersects = true;
      break;
    }
  }
  if (!intersects) {
    return;
  }

  int bw = (rect.w + BD_GFX_BLUR_DIVISOR - 1) / BD_GFX_BLUR_DIVISOR;
  int bh = (rect.h + BD_GFX_BLUR_DIVISOR - 1) / BD_GFX_BLUR_DIVISOR;
  if (bw < 1) {
    bw = 1;
  }
  if (bh < 1) {
    bh = 1;
  }
  int kernel = radius > 0 ? radius : 4;
  if (kernel > 8) {
    kernel = 8;
  }
  float scale_x = (float)bw / (float)gfx->blur_width;
  float scale_y = (float)bh / (float)gfx->blur_height;

  /* Downsample the scene region into blur texture A. */
  glBindFramebuffer(GL_FRAMEBUFFER, gfx->blur_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         gfx->blur_texture[0], 0);
  glViewport(0, 0, bw, bh);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glUseProgram(gfx->copy_program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gfx->scene_texture);
  glUniform1i(gfx->copy_tex, 0);
  float u0 = (float)rect.x / (float)gfx->width;
  float v0 = 1.0f - (float)(rect.y + rect.h) / (float)gfx->height;
  float u1 = (float)(rect.x + rect.w) / (float)gfx->width;
  float v1 = 1.0f - (float)rect.y / (float)gfx->height;
  glUniform4f(gfx->copy_uv_rect, u0, v0, u1, v1);
  draw_unit_quad(gfx);
  gfx->stats.blur_passes += 1;

  /* Horizontal and vertical 9-tap Gaussian passes. */
  glUseProgram(gfx->blur_program);
  glUniform1i(gfx->blur_tex, 0);
  for (int pass = 0; pass < 2; ++pass) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           gfx->blur_texture[pass == 0 ? 1 : 0], 0);
    glViewport(0, 0, bw, bh);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gfx->blur_texture[pass == 0 ? 0 : 1]);
    glUniform4f(gfx->blur_uv_rect, 0.0f, 0.0f, scale_x, scale_y);
    glUniform2f(gfx->blur_uv_max, scale_x, scale_y);
    if (pass == 0) {
      glUniform2f(gfx->blur_step,
                  kernel * 0.25f / (float)gfx->blur_width, 0.0f);
    } else {
      glUniform2f(gfx->blur_step, 0.0f,
                  kernel * 0.25f / (float)gfx->blur_height);
    }
    draw_unit_quad(gfx);
    gfx->stats.blur_passes += 1;
  }

  /* Composite the blurred A back into the scene, masked and tinted. */
  glBindFramebuffer(GL_FRAMEBUFFER, gfx->scene_fbo);
  glViewport(0, 0, gfx->width, gfx->height);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_SCISSOR_TEST);
  glUseProgram(gfx->composite_program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gfx->blur_texture[0]);
  glUniform1i(gfx->composite_tex, 0);
  glUniform4f(gfx->composite_rect, (float)rect.x, (float)rect.y, (float)rect.w,
              (float)rect.h);
  glUniform2f(gfx->composite_viewport, (float)gfx->width, (float)gfx->height);
  glUniform2f(gfx->composite_center, (float)rect.x + (float)rect.w * 0.5f,
              (float)rect.y + (float)rect.h * 0.5f);
  glUniform2f(gfx->composite_half, (float)rect.w * 0.5f, (float)rect.h * 0.5f);
  int rounded = clamp_radius(radius, rect);
  glUniform1f(gfx->composite_radius, rounded > 0 ? (float)rounded : -1.0f);
  glUniform2f(gfx->composite_uv_scale, scale_x, scale_y);
  glUniform4f(gfx->composite_overlay0, overlay_start.r, overlay_start.g,
              overlay_start.b, overlay_start.a);
  glUniform4f(gfx->composite_overlay1, overlay_end.r, overlay_end.g, overlay_end.b,
              overlay_end.a);
  glUniform1i(gfx->composite_axis, axis == BD_GRADIENT_HORIZONTAL ? 1 : 0);
  glUniform1f(gfx->composite_feather, feather > 0 ? (float)feather : 0.0f);
  glUniform1i(gfx->composite_feather_edge, (int)edge);
  gfx->stats.blur_passes += 1;

  for (int i = 0; i < gfx->damage_final_count; ++i) {
    bd_rect scissor = bd_rect_intersect(gfx->damage_final[i], clipped);
    if (scissor.w <= 0 || scissor.h <= 0) {
      continue;
    }
    glScissor(scissor.x, gfx->height - (scissor.y + scissor.h), scissor.w,
              scissor.h);
    draw_unit_quad(gfx);
    gfx->stats.blur_passes += 1;
  }

  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glUseProgram(0);
  glBindFramebuffer(GL_FRAMEBUFFER, gfx->scene_fbo);
  glViewport(0, 0, gfx->width, gfx->height);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

bd_texture *bd_gfx_texture_create(bd_gfx *gfx, int width, int height,
                                  bd_texture_filter filter,
                                  const uint8_t *rgba_pixels) {
  if (gfx == NULL || rgba_pixels == NULL || width <= 0 || height <= 0) {
    return NULL;
  }
  return bd_gfx_texture_create_internal(gfx, width, height, filter, GL_RGBA,
                                        GL_RGBA, GL_UNSIGNED_BYTE, rgba_pixels,
                                        false, false);
}

bool bd_gfx_texture_update(bd_gfx *gfx, bd_texture *texture, int width,
                           int height, const uint8_t *rgba_pixels) {
  if (gfx == NULL || texture == NULL || texture->id == 0 ||
      texture->cache_managed || rgba_pixels == NULL || width <= 0 ||
      height <= 0) {
    return false;
  }
  glBindTexture(GL_TEXTURE_2D, texture->id);
  if (width == texture->width && height == texture->height) {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA,
                    GL_UNSIGNED_BYTE, rgba_pixels);
  } else {
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)GL_RGBA, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba_pixels);
    texture->width = width;
    texture->height = height;
  }
  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    bd_gfx_set_error(gfx, "texture update failed");
    return false;
  }
  return true;
}

void bd_gfx_texture_destroy(bd_gfx *gfx, bd_texture *texture) {
  (void)gfx;
  if (texture == NULL || texture->cache_managed) {
    return;
  }
  if (texture->id != 0) {
    glDeleteTextures(1, &texture->id);
  }
  free(texture);
}

void bd_gfx_draw_texture(bd_gfx *gfx, bd_texture *texture, bd_rect dst,
                         bd_color tint) {
  bd_gfx_draw_texture_round(gfx, texture, dst, tint, 0);
}

void bd_gfx_draw_texture_round(bd_gfx *gfx, bd_texture *texture, bd_rect dst,
                               bd_color tint, int radius) {
  if (gfx == NULL || texture == NULL || texture->id == 0 || !gfx->in_frame ||
      dst.w <= 0 || dst.h <= 0) {
    return;
  }
  radius = clamp_radius(radius, dst);
  bd_color colors[4] = {tint, tint, tint, tint};
  bd_gfx_batch_add_quad(gfx, texture->id, BD_GFX_MODE_RGBA, 0, 0.0f, dst,
                        radius > 0 ? radius : -1, 0.0f, 0.0f, 1.0f, 1.0f, colors);
}

const char *bd_gfx_last_error(const bd_gfx *gfx) {
  if (gfx == NULL) {
    return "";
  }
  return gfx->error;
}
