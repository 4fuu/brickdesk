#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

/*
 * Host probe for the menu shim's GL state save/restore contract.
 *
 * The app draws a deterministic frame, records the GL state and pixels, binds
 * an offscreen FBO, and then swaps until the shim has opened and closed the
 * menu (driven by tests/menu_test.c through brickdesk-inputd). It then checks
 * that every value the shim touches is back and that redrawing with the
 * restored state produces the same pixels.
 */

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>

#define WIDTH 320
#define HEIGHT 240
#define ATTRIBS 4

typedef struct attrib_state {
  GLint enabled;
  GLint buffer;
  GLint size;
  GLint type;
  GLint normalized;
  GLint stride;
  GLvoid *pointer;
} attrib_state;

typedef struct gl_snapshot {
  GLint viewport[4];
  GLint scissor[4];
  GLint framebuffer;
  GLint program;
  GLint array_buffer;
  GLint element_buffer;
  GLint active_texture;
  GLint texture0;
  GLint texture1;
  GLint unpack_alignment;
  GLint pack_alignment;
  GLint blend_src_rgb;
  GLint blend_dst_rgb;
  GLint blend_equation_rgb;
  GLfloat clear_color[4];
  GLboolean scissor_test;
  GLboolean blend;
  GLboolean depth_test;
  GLboolean cull_face;
  GLboolean stencil_test;
  attrib_state attribs[ATTRIBS];
} gl_snapshot;

static int g_failures;

static void fail(const char *message) {
  printf("menu_state: FAIL: %s\n", message);
  ++g_failures;
}

static void snapshot(gl_snapshot *state) {
  memset(state, 0, sizeof(*state));
  glGetIntegerv(GL_VIEWPORT, state->viewport);
  glGetIntegerv(GL_SCISSOR_BOX, state->scissor);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &state->framebuffer);
  glGetIntegerv(GL_CURRENT_PROGRAM, &state->program);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &state->array_buffer);
  glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &state->element_buffer);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &state->active_texture);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &state->texture0);
  glActiveTexture(GL_TEXTURE1);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &state->texture1);
  glActiveTexture((GLenum)state->active_texture);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &state->unpack_alignment);
  glGetIntegerv(GL_PACK_ALIGNMENT, &state->pack_alignment);
  glGetIntegerv(GL_BLEND_SRC_RGB, &state->blend_src_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &state->blend_dst_rgb);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &state->blend_equation_rgb);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, state->clear_color);
  state->scissor_test = glIsEnabled(GL_SCISSOR_TEST);
  state->blend = glIsEnabled(GL_BLEND);
  state->depth_test = glIsEnabled(GL_DEPTH_TEST);
  state->cull_face = glIsEnabled(GL_CULL_FACE);
  state->stencil_test = glIsEnabled(GL_STENCIL_TEST);
  for (int i = 0; i < ATTRIBS; ++i) {
    glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED,
                        &state->attribs[i].enabled);
    glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                        &state->attribs[i].buffer);
    glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_SIZE,
                        &state->attribs[i].size);
    glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_TYPE,
                        &state->attribs[i].type);
    glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                        &state->attribs[i].normalized);
    glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_STRIDE,
                        &state->attribs[i].stride);
    glGetVertexAttribPointerv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                              &state->attribs[i].pointer);
  }
}

static void compare_snapshot(const gl_snapshot *before,
                             const gl_snapshot *after) {
  if (memcmp(before->viewport, after->viewport, sizeof(before->viewport)) != 0) {
    fail("GL_VIEWPORT changed");
  }
  if (memcmp(before->scissor, after->scissor, sizeof(before->scissor)) != 0) {
    fail("GL_SCISSOR_BOX changed");
  }
  if (before->framebuffer != after->framebuffer) {
    fail("GL_FRAMEBUFFER_BINDING changed");
  }
  if (before->program != after->program) {
    fail("GL_CURRENT_PROGRAM changed");
  }
  if (before->array_buffer != after->array_buffer) {
    fail("GL_ARRAY_BUFFER_BINDING changed");
  }
  if (before->element_buffer != after->element_buffer) {
    fail("GL_ELEMENT_ARRAY_BUFFER_BINDING changed");
  }
  if (before->active_texture != after->active_texture) {
    fail("GL_ACTIVE_TEXTURE changed");
  }
  if (before->texture0 != after->texture0) {
    fail("GL_TEXTURE_BINDING_2D unit 0 changed");
  }
  if (before->texture1 != after->texture1) {
    fail("GL_TEXTURE_BINDING_2D unit 1 changed");
  }
  if (before->unpack_alignment != after->unpack_alignment) {
    fail("GL_UNPACK_ALIGNMENT changed");
  }
  if (before->pack_alignment != after->pack_alignment) {
    fail("GL_PACK_ALIGNMENT changed");
  }
  if (before->blend_src_rgb != after->blend_src_rgb ||
      before->blend_dst_rgb != after->blend_dst_rgb ||
      before->blend_equation_rgb != after->blend_equation_rgb) {
    fail("blend state changed");
  }
  if (memcmp(before->clear_color, after->clear_color,
             sizeof(before->clear_color)) != 0) {
    fail("GL_COLOR_CLEAR_VALUE changed");
  }
  if (before->scissor_test != after->scissor_test) {
    fail("GL_SCISSOR_TEST changed");
  }
  if (before->blend != after->blend) {
    fail("GL_BLEND changed");
  }
  if (before->depth_test != after->depth_test) {
    fail("GL_DEPTH_TEST changed");
  }
  if (before->cull_face != after->cull_face) {
    fail("GL_CULL_FACE changed");
  }
  if (before->stencil_test != after->stencil_test) {
    fail("GL_STENCIL_TEST changed");
  }
  for (int i = 0; i < ATTRIBS; ++i) {
    char message[96];
    if (before->attribs[i].enabled != after->attribs[i].enabled ||
        before->attribs[i].buffer != after->attribs[i].buffer ||
        before->attribs[i].size != after->attribs[i].size ||
        before->attribs[i].type != after->attribs[i].type ||
        before->attribs[i].normalized != after->attribs[i].normalized ||
        before->attribs[i].stride != after->attribs[i].stride ||
        before->attribs[i].pointer != after->attribs[i].pointer) {
      snprintf(message, sizeof(message), "vertex attrib %d changed", i);
      fail(message);
    }
  }
}

static GLuint compile(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    fprintf(stderr, "menu_state: shader: %s\n", log);
  }
  return shader;
}

static GLuint make_program(void) {
  static const char *vertex =
      "attribute vec2 a_pos;\n"
      "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
  static const char *fragment =
      "precision mediump float;\n"
      "uniform vec4 u_color;\n"
      "void main() { gl_FragColor = u_color; }\n";
  GLuint vs = compile(GL_VERTEX_SHADER, vertex);
  GLuint fs = compile(GL_FRAGMENT_SHADER, fragment);
  GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glBindAttribLocation(program, 0, "a_pos");
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetProgramInfoLog(program, sizeof(log), NULL, log);
    fprintf(stderr, "menu_state: program: %s\n", log);
  }
  return program;
}

static GLuint make_texture(GLubyte value) {
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  GLubyte pixel[4] = {value, value, value, 255};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               pixel);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  return texture;
}

static unsigned menu_open_count(void) {
  static unsigned (*fn)(void) = NULL;
  if (fn == NULL) {
    fn = (unsigned (*)(void))dlsym(RTLD_DEFAULT, "bd_menu_open_count");
  }
  return fn != NULL ? fn() : 0;
}

static bool menu_visible(void) {
  static bool (*fn)(void) = NULL;
  if (fn == NULL) {
    fn = (bool (*)(void))dlsym(RTLD_DEFAULT, "bd_menu_visible");
  }
  return fn != NULL ? fn() : false;
}

static void draw_probe_frame(void) {
  /* Uses the state the application already set; the shim must restore it. */
  static GLint color_uniform = -1;
  if (color_uniform < 0) {
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    color_uniform = glGetUniformLocation((GLuint)program, "u_color");
  }
  glClear(GL_COLOR_BUFFER_BIT);
  glUniform4f(color_uniform, 0.8f, 0.25f, 0.1f, 1.0f);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "menu_state: SDL_Init: %s\n", SDL_GetError());
    return 2;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_Window *window = SDL_CreateWindow("menu state", SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED, WIDTH, HEIGHT,
                                        SDL_WINDOW_OPENGL);
  if (window == NULL) {
    fprintf(stderr, "menu_state: window: %s\n", SDL_GetError());
    return 2;
  }
  SDL_GLContext context = SDL_GL_CreateContext(window);
  if (context == NULL) {
    fprintf(stderr, "menu_state: context: %s\n", SDL_GetError());
    return 2;
  }

  GLuint program = make_program();
  GLuint vbo = 0;
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  static const GLfloat vertices[] = {-0.8f, -0.7f, 0.9f, -0.4f,
                                     -0.2f, 0.9f};
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
  GLuint index_buffer = 0;
  glGenBuffers(1, &index_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer);
  static const GLushort indices[] = {0, 1, 2};
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
               GL_STATIC_DRAW);

  GLuint texture0 = make_texture(40);
  GLuint texture1 = make_texture(200);

  GLuint fbo = 0;
  GLuint fbo_texture = 0;
  glGenTextures(1, &fbo_texture);
  glBindTexture(GL_TEXTURE_2D, fbo_texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, NULL);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         fbo_texture, 0);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, WIDTH, HEIGHT);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.2f, 0.3f, 0.4f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glScissor(16, 24, 200, 140);
  glEnable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE);
  glBlendEquation(GL_FUNC_ADD);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glClearColor(0.2f, 0.3f, 0.4f, 1.0f);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);

  glUseProgram(program);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  for (int i = 0; i < 3; ++i) {
    glEnableVertexAttribArray((GLuint)i);
    glVertexAttribPointer((GLuint)i, 2, GL_FLOAT, GL_FALSE, 0,
                          (const void *)(uintptr_t)(i * 0));
  }
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, texture1);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture0);
  glActiveTexture(GL_TEXTURE1);

  GLubyte before_pixels[WIDTH * HEIGHT * 4];
  GLubyte after_pixels[WIDTH * HEIGHT * 4];
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, WIDTH, HEIGHT);
  draw_probe_frame();
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, before_pixels);

  /* Leave the FBO bound so the shim must save and restore it. */
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  gl_snapshot before;
  snapshot(&before);

  bool observed_open = false;
  bool observed_closed = false;
  for (int frame = 0; frame < 1200; ++frame) {
    SDL_GL_SwapWindow(window);
    unsigned count = menu_open_count();
    if (count > 0) {
      observed_open = true;
    }
    if (observed_open && !menu_visible()) {
      observed_closed = true;
      break;
    }
    SDL_Delay(16);
  }
  if (!observed_open) {
    fail("menu never opened (inputd replay missing?)");
  }
  if (!observed_closed) {
    fail("menu never closed");
  }

  gl_snapshot after;
  snapshot(&after);
  compare_snapshot(&before, &after);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  draw_probe_frame();
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, after_pixels);
  if (memcmp(before_pixels, after_pixels, sizeof(before_pixels)) != 0) {
    fail("pixels after the menu differ from before");
  }

  if (g_failures == 0) {
    printf("menu_state: OK\n");
  }
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &fbo_texture);
  glDeleteTextures(1, &texture0);
  glDeleteTextures(1, &texture1);
  glDeleteBuffers(1, &index_buffer);
  glDeleteBuffers(1, &vbo);
  glDeleteProgram(program);
  SDL_GL_DeleteContext(context);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return g_failures == 0 ? 0 : 1;
}
