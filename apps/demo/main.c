#define _POSIX_C_SOURCE 200809L
#define JSMN_STATIC
#include "jsmn.h"

#include "brickdesk/app.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>

#define DEMO_DEFAULT_WIDTH 512
#define DEMO_DEFAULT_HEIGHT 384
#define DEMO_LINE_MAX 8192

static volatile sig_atomic_t g_stop;

typedef struct demo {
  SDL_Window *window;
  SDL_GLContext context;
  GLuint program;
  GLuint vertex_buffer;
  GLint time_uniform;
  bool display_ready;
  bool headless;
  bool paused;
  bool quit;
  uint64_t frame;
  int width;
  int height;
  bool fixed_time;
  float fixed_time_seconds;
  int socket_fd;
  char input[DEMO_LINE_MAX];
  size_t input_length;
  uint64_t frame_seq;
  int last_slot;
} demo;

static void on_signal(int signal_number) {
  (void)signal_number;
  g_stop = 1;
}

static uint64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void sleep_ms(int milliseconds) {
  struct timespec delay;
  delay.tv_sec = milliseconds / 1000;
  delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
  nanosleep(&delay, NULL);
}

static bool send_line(int fd, const char *line) {
  if (fd < 0) {
    return false;
  }
  char buffer[DEMO_LINE_MAX + 2];
  size_t length = strlen(line);
  if (length + 1 >= sizeof(buffer)) {
    return false;
  }
  memcpy(buffer, line, length);
  buffer[length] = '\n';
  size_t sent = 0;
  while (sent < length + 1) {
    ssize_t amount = send(fd, buffer + sent, length + 1 - sent, MSG_NOSIGNAL);
    if (amount < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    sent += (size_t)amount;
  }
  return true;
}

static void reply_ok(int fd, int id) {
  if (id < 0) {
    return;
  }
  char line[128];
  snprintf(line, sizeof(line), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"ok\":true}}",
           id);
  send_line(fd, line);
}

static void reply_error(int fd, int id, int code, const char *message) {
  if (id < 0) {
    return;
  }
  char line[512];
  snprintf(line, sizeof(line),
           "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
           id, code, message);
  send_line(fd, line);
}

static int token_int(const char *json, const jsmntok_t *token) {
  if (token->type != JSMN_PRIMITIVE) {
    return -1;
  }
  char buffer[32];
  size_t length = (size_t)(token->end - token->start);
  if (length == 0 || length >= sizeof(buffer)) {
    return -1;
  }
  memcpy(buffer, json + token->start, length);
  buffer[length] = '\0';
  char *end = NULL;
  long value = strtol(buffer, &end, 10);
  if (end == buffer || *end != '\0' || value < 0 || value > 2147483647L) {
    return -1;
  }
  return (int)value;
}

static bool token_string(const char *json, const jsmntok_t *token, char *out,
                         size_t out_size) {
  if (token->type != JSMN_STRING) {
    return false;
  }
  size_t length = (size_t)(token->end - token->start);
  if (length == 0 || length >= out_size) {
    return false;
  }
  memcpy(out, json + token->start, length);
  out[length] = '\0';
  return true;
}

static int json_skip(const jsmntok_t *tokens, int index) {
  int end = index + 1;
  if (tokens[index].type == JSMN_OBJECT) {
    for (int pair = 0; pair < tokens[index].size; ++pair) {
      end = json_skip(tokens, end);
      end = json_skip(tokens, end);
    }
  } else if (tokens[index].type == JSMN_ARRAY) {
    for (int element = 0; element < tokens[index].size; ++element) {
      end = json_skip(tokens, end);
    }
  }
  return end;
}

static int json_field(const jsmntok_t *tokens, int object, const char *json,
                      const char *key) {
  if (tokens[object].type != JSMN_OBJECT) {
    return -1;
  }
  int child = object + 1;
  for (int pair = 0; pair < tokens[object].size; ++pair) {
    int value = json_skip(tokens, child);
    if (tokens[child].type == JSMN_STRING) {
      size_t length = (size_t)(tokens[child].end - tokens[child].start);
      if (strlen(key) == length &&
          memcmp(json + tokens[child].start, key, length) == 0) {
        return value;
      }
    }
    child = json_skip(tokens, value);
  }
  return -1;
}

static void init_gl(demo *app) {
  static const char *vertex_source =
      "attribute vec2 a_pos;\n"
      "varying vec2 v_pos;\n"
      "void main() { v_pos = a_pos; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
  static const char *fragment_source =
      "precision mediump float;\n"
      "varying vec2 v_pos;\n"
      "uniform float u_time;\n"
      "void main() {\n"
      "  float wave = 0.5 + 0.5 * sin(u_time + v_pos.x * 3.0);\n"
      "  gl_FragColor = vec4(0.35 + 0.25 * v_pos.x, 0.30 + 0.35 * wave,\n"
      "                      0.45 + 0.20 * v_pos.y, 1.0);\n"
      "}\n";
  const GLfloat vertices[] = {-1.0f, -1.0f, 1.0f, -1.0f,
                              -1.0f, 1.0f,  1.0f, 1.0f};
  GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
  GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
  const char *vertex_text = vertex_source;
  const char *fragment_text = fragment_source;
  glShaderSource(vertex, 1, &vertex_text, NULL);
  glCompileShader(vertex);
  glShaderSource(fragment, 1, &fragment_text, NULL);
  glCompileShader(fragment);
  app->program = glCreateProgram();
  glAttachShader(app->program, vertex);
  glAttachShader(app->program, fragment);
  glBindAttribLocation(app->program, 0, "a_pos");
  glLinkProgram(app->program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);

  GLint linked = 0;
  glGetProgramiv(app->program, GL_LINK_STATUS, &linked);
  if (!linked) {
    glDeleteProgram(app->program);
    app->program = 0;
    return;
  }
  app->time_uniform = glGetUniformLocation(app->program, "u_time");
  glGenBuffers(1, &app->vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, app->vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
}

static void release_display(demo *app) {
  if (app->program != 0) {
    glDeleteProgram(app->program);
    app->program = 0;
  }
  if (app->vertex_buffer != 0) {
    glDeleteBuffers(1, &app->vertex_buffer);
    app->vertex_buffer = 0;
  }
  if (app->context != NULL) {
    SDL_GL_DeleteContext(app->context);
    app->context = NULL;
  }
  if (app->window != NULL) {
    SDL_DestroyWindow(app->window);
    app->window = NULL;
  }
  app->display_ready = false;
}

static bool acquire_display(demo *app) {
  if (app->display_ready) {
    return true;
  }
  if (app->headless) {
    return false;
  }
  if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
    app->headless = true;
    return false;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  app->window = SDL_CreateWindow("brickdesk demo", SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED, app->width,
                                 app->height, SDL_WINDOW_OPENGL);
  if (app->window == NULL) {
    app->headless = true;
    return false;
  }
  app->context = SDL_GL_CreateContext(app->window);
  if (app->context == NULL) {
    SDL_DestroyWindow(app->window);
    app->window = NULL;
    app->headless = true;
    return false;
  }
  init_gl(app);
  app->display_ready = true;
  return true;
}

static void render_frame(demo *app) {
  if (!app->display_ready) {
    return;
  }
  glViewport(0, 0, app->width, app->height);
  glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  if (app->program != 0) {
    glUseProgram(app->program);
    float time_seconds =
        app->fixed_time
            ? app->fixed_time_seconds
            : (float)(monotonic_ms() % 10000) / 1000.0f;
    glUniform1f(app->time_uniform, time_seconds);
    glBindBuffer(GL_ARRAY_BUFFER, app->vertex_buffer);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  glFinish();
}

static void fill_pattern(demo *app, uint8_t *pixels) {
  for (int y = 0; y < app->height; ++y) {
    for (int x = 0; x < app->width; ++x) {
      size_t offset = ((size_t)y * (size_t)app->width + (size_t)x) * 4u;
      pixels[offset + 0] = (uint8_t)(x * 255 / (app->width - 1));
      pixels[offset + 1] = (uint8_t)(y * 255 / (app->height - 1));
      pixels[offset + 2] = (uint8_t)((app->frame_seq * 37u) & 0xffu);
      pixels[offset + 3] = 255;
    }
  }
}

static void fill_stable_pattern(demo *app, uint8_t *pixels) {
  /*
   * A flat colour is intentional: host screenshot baselines compare exact
   * pixels, and gradients would encode the host compiler's float rounding.
   */
  for (int y = 0; y < app->height; ++y) {
    for (int x = 0; x < app->width; ++x) {
      size_t offset = ((size_t)y * (size_t)app->width + (size_t)x) * 4u;
      pixels[offset + 0] = 58;
      pixels[offset + 1] = 52;
      pixels[offset + 2] = 72;
      pixels[offset + 3] = 255;
    }
  }
}

static bool write_last_frame(demo *app) {
  const char *path = getenv("BRICKDESK_LAST_FRAME");
  if (path == NULL || path[0] == '\0') {
    return false;
  }
  size_t pixel_size = (size_t)app->width * (size_t)app->height * 4u;
  if (pixel_size == 0) {
    return false;
  }
  uint8_t *pixels = (uint8_t *)malloc(pixel_size);
  if (pixels == NULL) {
    return false;
  }
  bool captured = false;
  /*
   * Host screenshot scenarios ask for a CPU pattern so the handoff tile does
   * not depend on the GL driver's transcendental-math rounding.
   */
  bool stable_frame = getenv("BRICKDESK_DEMO_STABLE_FRAME") != NULL;
  if (!stable_frame && app->display_ready && app->context != NULL) {
    render_frame(app);
    glReadPixels(0, 0, app->width, app->height, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixels);
    captured = true;
  }
  if (!captured) {
    if (stable_frame) {
      fill_stable_pattern(app, pixels);
    } else {
      fill_pattern(app, pixels);
    }
  }

  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    free(pixels);
    return false;
  }
  size_t total = sizeof(bd_app_last_frame_header) + pixel_size;
  if (ftruncate(fd, (off_t)total) != 0) {
    close(fd);
    free(pixels);
    return false;
  }
  void *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    close(fd);
    free(pixels);
    return false;
  }
  bd_app_last_frame_header header;
  memset(&header, 0, sizeof(header));
  header.magic = BD_APP_LAST_FRAME_MAGIC;
  header.version = BD_APP_LAST_FRAME_VERSION;
  header.header_size = (uint16_t)sizeof(bd_app_last_frame_header);
  header.width = (uint32_t)app->width;
  header.height = (uint32_t)app->height;
  header.format = BD_APP_PIXEL_RGBA8888;
  header.stride = (uint32_t)app->width * 4u;
  header.data_size = pixel_size;
  header.timestamp_ms = monotonic_ms();
  header.flags = BD_APP_LAST_FRAME_FLAG_WRITING;
  memcpy(map, &header, sizeof(header));
  __atomic_thread_fence(__ATOMIC_RELEASE);
  memcpy((uint8_t *)map + sizeof(header), pixels, pixel_size);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  header.flags = 0;
  header.frame_seq = ++app->frame_seq;
  memcpy(map, &header, sizeof(header));
  msync(map, total, MS_SYNC);
  munmap(map, total);
  close(fd);
  free(pixels);
  return true;
}

static char *state_path(char *buffer, size_t size) {
  const char *direct = getenv("BRICKDESK_STATE_FILE");
  if (direct != NULL && direct[0] != '\0') {
    snprintf(buffer, size, "%s", direct);
    return buffer;
  }
  const char *directory = getenv("BRICKDESK_STATE_DIR");
  if (directory == NULL || directory[0] == '\0') {
    return NULL;
  }
  snprintf(buffer, size, "%s/state.txt", directory);
  return buffer;
}

static bool save_state(const demo *app, int slot) {
  char path[BD_APP_PATH_MAX];
  if (state_path(path, sizeof(path)) == NULL) {
    return false;
  }
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  fprintf(file, "slot=%d\nframes=%llu\nsaved_at_ms=%llu\n", slot,
          (unsigned long long)app->frame_seq,
          (unsigned long long)monotonic_ms());
  bool ok = fclose(file) == 0;
  return ok;
}

static void handle_request(demo *app, const char *json, const jsmntok_t *tokens,
                           int root, int id, const char *method) {
  if (strcmp(method, "app.pause") == 0) {
    if (app->display_ready) {
      write_last_frame(app);
      release_display(app);
    }
    app->paused = true;
    reply_ok(app->socket_fd, id);
  } else if (strcmp(method, "app.resume") == 0) {
    acquire_display(app);
    app->paused = false;
    reply_ok(app->socket_fd, id);
  } else if (strcmp(method, "app.display_release") == 0) {
    if (app->display_ready) {
      write_last_frame(app);
      release_display(app);
    }
    reply_ok(app->socket_fd, id);
  } else if (strcmp(method, "app.display_acquire") == 0) {
    acquire_display(app);
    reply_ok(app->socket_fd, id);
  } else if (strcmp(method, "state.save") == 0) {
    int slot = 0;
    int params = json_field(tokens, root, json, "params");
    if (params >= 0) {
      int slot_token = json_field(tokens, params, json, "slot");
      if (slot_token >= 0) {
        int value = token_int(json, &tokens[slot_token]);
        if (value >= 0) {
          slot = value;
        }
      }
    }
    bool ok = save_state(app, slot);
    app->last_slot = slot;
    char line[192];
    snprintf(line, sizeof(line),
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"ok\":%s,\"bytes\":%d}}",
             id, ok ? "true" : "false", ok ? 64 : 0);
    send_line(app->socket_fd, line);
  } else if (strcmp(method, "state.load") == 0) {
    char path[BD_APP_PATH_MAX];
    bool ok = state_path(path, sizeof(path)) != NULL &&
              access(path, R_OK) == 0;
    char line[256];
    snprintf(line, sizeof(line),
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"ok\":%s}}", id,
             ok ? "true" : "false");
    send_line(app->socket_fd, line);
  } else if (strcmp(method, "app.quit") == 0) {
    save_state(app, app->last_slot);
    reply_ok(app->socket_fd, id);
    app->quit = true;
  } else if (strcmp(method, "app.ping") == 0) {
    reply_ok(app->socket_fd, id);
  } else if (strcmp(method, "app.screenshot") == 0) {
    write_last_frame(app);
    reply_ok(app->socket_fd, id);
  } else {
    reply_error(app->socket_fd, id, -32601, "method not found");
  }
}

static void handle_line(demo *app, const char *line) {
  if (line[0] == '\0') {
    return;
  }
  jsmn_parser parser;
  jsmntok_t tokens[256];
  jsmn_init(&parser);
  int count = jsmn_parse(&parser, line, strlen(line), tokens,
                         sizeof(tokens) / sizeof(tokens[0]));
  if (count < 1 || tokens[0].type != JSMN_OBJECT) {
    return;
  }
  int method_token = json_field(tokens, 0, line, "method");
  int id_token = json_field(tokens, 0, line, "id");
  if (method_token < 0 || id_token < 0) {
    return;
  }
  char method[64];
  if (!token_string(line, &tokens[method_token], method, sizeof(method))) {
    return;
  }
  int id = token_int(line, &tokens[id_token]);
  handle_request(app, line, tokens, 0, id, method);
}

static void read_socket(demo *app) {
  char buffer[2048];
  for (;;) {
    ssize_t bytes = recv(app->socket_fd, buffer, sizeof(buffer), 0);
    if (bytes < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return;
      }
      app->socket_fd = -1;
      return;
    }
    if (bytes == 0) {
      app->socket_fd = -1;
      return;
    }
    if (app->input_length + (size_t)bytes >= sizeof(app->input)) {
      app->input_length = 0;
      continue;
    }
    memcpy(app->input + app->input_length, buffer, (size_t)bytes);
    app->input_length += (size_t)bytes;
    char *newline = NULL;
    while ((newline = memchr(app->input, '\n', app->input_length)) != NULL) {
      size_t length = (size_t)(newline - app->input);
      app->input[length] = '\0';
      handle_line(app, app->input);
      memmove(app->input, app->input + length + 1,
              app->input_length - length - 1);
      app->input_length -= length + 1;
    }
  }
}

static void connect_session(demo *app) {
  const char *path = getenv("BRICKDESK_SESSION_SOCKET");
  if (path == NULL || path[0] == '\0') {
    return;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return;
  }
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(address.sun_path)) {
    close(fd);
    return;
  }
  snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
  bool connected = false;
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
      connected = true;
      break;
    }
    if (errno != ENOENT && errno != ECONNREFUSED) {
      break;
    }
    sleep_ms(20);
  }
  if (!connected) {
    close(fd);
    return;
  }
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  app->socket_fd = fd;
  char hello[512];
  snprintf(hello, sizeof(hello),
           "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"app.hello\","
           "\"params\":{\"pid\":%ld,\"protocol\":1,\"capabilities\":["
           "\"pause\",\"resume\",\"save_state\",\"load_state\","
           "\"screenshot\",\"display_release\"]}}",
           (long)getpid());
  send_line(fd, hello);
}

static void pump_session(demo *app, int timeout_ms) {
  if (app->socket_fd < 0) {
    sleep_ms(timeout_ms);
    return;
  }
  struct pollfd fd;
  fd.fd = app->socket_fd;
  fd.events = POLLIN;
  fd.revents = 0;
  int result = poll(&fd, 1, timeout_ms);
  if (result > 0 && (fd.revents & (POLLIN | POLLHUP))) {
    read_socket(app);
  } else if (result == 0) {
    return;
  }
}

int main(void) {
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  signal(SIGPIPE, SIG_IGN);
  setvbuf(stdout, NULL, _IONBF, 0);

  demo app;
  memset(&app, 0, sizeof(app));
  app.socket_fd = -1;
  app.width = DEMO_DEFAULT_WIDTH;
  app.height = DEMO_DEFAULT_HEIGHT;
  const char *width_env = getenv("BRICKDESK_DEMO_WIDTH");
  const char *height_env = getenv("BRICKDESK_DEMO_HEIGHT");
  if (width_env != NULL) {
    int value = atoi(width_env);
    if (value >= 64 && value <= 4096) {
      app.width = value;
    }
  }
  if (height_env != NULL) {
    int value = atoi(height_env);
    if (value >= 64 && value <= 4096) {
      app.height = value;
    }
  }
  const char *fixed_time = getenv("BRICKDESK_DEMO_FIXED_TIME_MS");
  if (fixed_time != NULL) {
    app.fixed_time = true;
    app.fixed_time_seconds = (float)atol(fixed_time) / 1000.0f;
  }

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    app.headless = true;
  } else {
    acquire_display(&app);
    if (!app.display_ready) {
      app.headless = true;
    }
  }
  connect_session(&app);

  uint64_t last_render = 0;
  while (!g_stop && !app.quit) {
    pump_session(&app, 16);
    if (app.paused) {
      continue;
    }
    uint64_t now = monotonic_ms();
    if (now - last_render >= 33) {
      render_frame(&app);
      if (app.display_ready) {
        SDL_GL_SwapWindow(app.window);
      }
      ++app.frame;
      last_render = now;
    }
  }

  save_state(&app, app.last_slot);
  release_display(&app);
  if (app.socket_fd >= 0) {
    close(app.socket_fd);
  }
  SDL_Quit();
  return 0;
}
