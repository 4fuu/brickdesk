#include "brickdesk/platform.h"

#include <SDL.h>
#include <GLES2/gl2.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct bd_platform {
  SDL_Window *window;
  SDL_GLContext context;
  int width;
  int height;
  bd_gl_info gl;
  int wake_fd;
  int wake_write_fd;
};

static bd_key map_scancode(SDL_Scancode scancode) {
  switch (scancode) {
    case SDL_SCANCODE_LEFT:
      return BD_KEY_LEFT;
    case SDL_SCANCODE_RIGHT:
      return BD_KEY_RIGHT;
    case SDL_SCANCODE_UP:
      return BD_KEY_UP;
    case SDL_SCANCODE_DOWN:
      return BD_KEY_DOWN;
    case SDL_SCANCODE_Z:
      return BD_KEY_A;
    case SDL_SCANCODE_X:
      return BD_KEY_B;
    case SDL_SCANCODE_C:
      return BD_KEY_X;
    case SDL_SCANCODE_V:
      return BD_KEY_Y;
    case SDL_SCANCODE_Q:
      return BD_KEY_L1;
    case SDL_SCANCODE_E:
      return BD_KEY_R1;
    case SDL_SCANCODE_R:
      return BD_KEY_L2;
    case SDL_SCANCODE_F:
      return BD_KEY_R2;
    case SDL_SCANCODE_H:
      return BD_KEY_HOME;
    case SDL_SCANCODE_P:
      return BD_KEY_POWER;
    case SDL_SCANCODE_LEFTBRACKET:
      return BD_KEY_VOLUME_DOWN;
    case SDL_SCANCODE_RIGHTBRACKET:
      return BD_KEY_VOLUME_UP;
    case SDL_SCANCODE_TAB:
      return BD_KEY_SELECT;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
      return BD_KEY_START;
    case SDL_SCANCODE_ESCAPE:
      return BD_KEY_MENU;
    default:
      return BD_KEY_UNKNOWN;
  }
}

static const char *gl_string(GLenum name) {
  const GLubyte *value = glGetString(name);
  return value != NULL ? (const char *)value : "";
}

static int wake_watch(void *user, SDL_Event *event) {
  (void)event;
  bd_platform *platform = (bd_platform *)user;
  if (platform != NULL && platform->wake_write_fd >= 0) {
    unsigned char byte = 1;
    ssize_t ignored = write(platform->wake_write_fd, &byte, 1);
    (void)ignored;
  }
  return 1;
}

static void drain_wake(bd_platform *platform) {
  if (platform == NULL || platform->wake_fd < 0) {
    return;
  }
  unsigned char buffer[64];
  while (read(platform->wake_fd, buffer, sizeof(buffer)) > 0) {
  }
}

static void map_event(const SDL_Event *sdl_event, bd_event *event) {
  event->type = BD_EVENT_NONE;
  event->key = BD_KEY_UNKNOWN;
  event->pressed = false;

  switch (sdl_event->type) {
    case SDL_QUIT:
      event->type = BD_EVENT_QUIT;
      break;
    case SDL_WINDOWEVENT:
      switch (sdl_event->window.event) {
        case SDL_WINDOWEVENT_CLOSE:
          event->type = BD_EVENT_QUIT;
          break;
        case SDL_WINDOWEVENT_EXPOSED:
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_SHOWN:
        case SDL_WINDOWEVENT_RESTORED:
          event->type = BD_EVENT_REDRAW;
          break;
        default:
          break;
      }
      break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
      if (sdl_event->type == SDL_KEYDOWN && sdl_event->key.repeat != 0) {
        break;
      }
      bd_key key = map_scancode(sdl_event->key.keysym.scancode);
      if (key != BD_KEY_UNKNOWN) {
        event->type = BD_EVENT_KEY;
        event->key = key;
        event->pressed = sdl_event->type == SDL_KEYDOWN;
      }
      break;
    }
    default:
      break;
  }
}

bd_platform *bd_platform_create(const bd_platform_config *config) {
  if (config == NULL || config->width <= 0 || config->height <= 0) {
    fprintf(stderr, "platform: invalid window configuration\n");
    return NULL;
  }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
    fprintf(stderr, "platform: SDL_Init failed: %s\n", SDL_GetError());
    return NULL;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

  Uint32 flags = SDL_WINDOW_OPENGL;
  if (config->hidden) {
    flags |= SDL_WINDOW_HIDDEN;
  }

  bd_platform *platform = (bd_platform *)calloc(1, sizeof(*platform));
  if (platform == NULL) {
    SDL_Quit();
    return NULL;
  }
  platform->wake_fd = -1;
  platform->wake_write_fd = -1;
  int wake_pipe[2];
  if (pipe(wake_pipe) == 0) {
    for (int i = 0; i < 2; ++i) {
      int flags = fcntl(wake_pipe[i], F_GETFL, 0);
      if (flags >= 0) {
        fcntl(wake_pipe[i], F_SETFL, flags | O_NONBLOCK);
      }
      int fd_flags = fcntl(wake_pipe[i], F_GETFD, 0);
      if (fd_flags >= 0) {
        fcntl(wake_pipe[i], F_SETFD, fd_flags | FD_CLOEXEC);
      }
    }
    platform->wake_fd = wake_pipe[0];
    platform->wake_write_fd = wake_pipe[1];
    SDL_AddEventWatch(wake_watch, platform);
  }

  platform->window = SDL_CreateWindow(config->title != NULL ? config->title : "brickdesk",
                                      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                      config->width, config->height, flags);
  if (platform->window == NULL) {
    fprintf(stderr, "platform: SDL_CreateWindow failed: %s\n", SDL_GetError());
    free(platform);
    SDL_Quit();
    return NULL;
  }

  platform->context = SDL_GL_CreateContext(platform->window);
  if (platform->context == NULL) {
    fprintf(stderr, "platform: SDL_GL_CreateContext failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(platform->window);
    free(platform);
    SDL_Quit();
    return NULL;
  }

  if (SDL_GL_MakeCurrent(platform->window, platform->context) != 0) {
    fprintf(stderr, "platform: SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
    SDL_GL_DeleteContext(platform->context);
    SDL_DestroyWindow(platform->window);
    free(platform);
    SDL_Quit();
    return NULL;
  }

  const char *version = gl_string(GL_VERSION);
  if (version == NULL || strncmp(version, "OpenGL ES", 9) != 0) {
    fprintf(stderr, "platform: expected a GLES2 context, got '%s'\n",
            version != NULL ? version : "(unknown)");
    SDL_GL_DeleteContext(platform->context);
    SDL_DestroyWindow(platform->window);
    free(platform);
    SDL_Quit();
    return NULL;
  }

  /* Vsync is best-effort: the offscreen driver may not implement it. */
  SDL_GL_SetSwapInterval(1);

  int window_width = 0;
  int window_height = 0;
  SDL_GetWindowSize(platform->window, &window_width, &window_height);

  int drawable_width = 0;
  int drawable_height = 0;
  SDL_GL_GetDrawableSize(platform->window, &drawable_width, &drawable_height);
  if (drawable_width <= 0 || drawable_height <= 0) {
    drawable_width = window_width;
    drawable_height = window_height;
  }
  if (drawable_width != window_width || drawable_height != window_height) {
    fprintf(stderr, "platform: window %dx%d, drawable %dx%d\n", window_width,
            window_height, drawable_width, drawable_height);
  }
  platform->width = drawable_width;
  platform->height = drawable_height;

  platform->gl.vendor = gl_string(GL_VENDOR);
  platform->gl.renderer = gl_string(GL_RENDERER);
  platform->gl.version = gl_string(GL_VERSION);
  platform->gl.shading_language = gl_string(GL_SHADING_LANGUAGE_VERSION);
  platform->gl.extensions = gl_string(GL_EXTENSIONS);

  return platform;
}

void bd_platform_destroy(bd_platform *platform) {
  if (platform == NULL) {
    return;
  }
  if (platform->wake_fd >= 0) {
    SDL_DelEventWatch(wake_watch, platform);
  }
  if (platform->context != NULL) {
    SDL_GL_DeleteContext(platform->context);
  }
  if (platform->window != NULL) {
    SDL_DestroyWindow(platform->window);
  }
  if (platform->wake_fd >= 0) {
    close(platform->wake_fd);
  }
  if (platform->wake_write_fd >= 0) {
    close(platform->wake_write_fd);
  }
  free(platform);
  SDL_Quit();
}

int bd_platform_width(const bd_platform *platform) {
  return platform != NULL ? platform->width : 0;
}

int bd_platform_height(const bd_platform *platform) {
  return platform != NULL ? platform->height : 0;
}

void bd_platform_gl_info(const bd_platform *platform, bd_gl_info *out) {
  if (out == NULL) {
    return;
  }
  if (platform == NULL) {
    memset(out, 0, sizeof(*out));
    return;
  }
  *out = platform->gl;
}

void bd_gl_print_info(const bd_gl_info *info, bool verbose) {
  if (info == NULL) {
    return;
  }
  printf("GL vendor: %s\n", info->vendor != NULL ? info->vendor : "(unknown)");
  printf("GL renderer: %s\n", info->renderer != NULL ? info->renderer : "(unknown)");
  printf("GL version: %s\n", info->version != NULL ? info->version : "(unknown)");

  if (!verbose) {
    return;
  }

  printf("GLSL version: %s\n",
         info->shading_language != NULL ? info->shading_language : "(unknown)");

  const char *cursor = info->extensions;
  int count = 0;
  while (cursor != NULL && *cursor != '\0') {
    while (*cursor == ' ') {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }
    ++count;
    while (*cursor != '\0' && *cursor != ' ') {
      ++cursor;
    }
  }

  printf("GL extensions (%d):\n", count);
  cursor = info->extensions;
  while (cursor != NULL && *cursor != '\0') {
    while (*cursor == ' ') {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }
    const char *start = cursor;
    while (*cursor != '\0' && *cursor != ' ') {
      ++cursor;
    }
    printf("  %.*s\n", (int)(cursor - start), start);
  }
}

uint32_t bd_platform_ticks_ms(void) {
  return (uint32_t)SDL_GetTicks();
}

/*
 * SDL's offscreen video driver spins inside SDL_WaitEventTimeout instead of
 * blocking, which makes an idle host process burn most of a core. Device
 * drivers (fbdev/mali) block normally, so only pay for the fallback when the
 * offscreen driver is selected.
 */
static bool video_driver_busy_waits(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *driver = SDL_GetCurrentVideoDriver();
    cached = driver != NULL && strcmp(driver, "offscreen") == 0;
  }
  return cached != 0;
}

static int wait_sdl_event(SDL_Event *sdl_event, int timeout_ms) {
  if (!video_driver_busy_waits()) {
    return SDL_WaitEventTimeout(sdl_event, timeout_ms);
  }

  if (timeout_ms >= 0) {
    if (timeout_ms > 0) {
      SDL_Delay((Uint32)timeout_ms);
    }
    return SDL_PollEvent(sdl_event);
  }
  for (;;) {
    if (SDL_PollEvent(sdl_event)) {
      return 1;
    }
    SDL_Delay(16);
  }
}

int bd_platform_wait_event(bd_platform *platform, bd_event *event, int timeout_ms) {
  if (platform == NULL || event == NULL) {
    return -1;
  }

  SDL_Event sdl_event;
  int result = wait_sdl_event(&sdl_event, timeout_ms);
  drain_wake(platform);
  if (result <= 0) {
    event->type = BD_EVENT_NONE;
    event->key = BD_KEY_UNKNOWN;
    event->pressed = false;
    return result;
  }

  map_event(&sdl_event, event);
  return 1;
}

int bd_platform_event_fd(const bd_platform *platform) {
  return platform != NULL ? platform->wake_fd : -1;
}

int bd_platform_pump_event(bd_platform *platform, bd_event *event) {
  if (platform == NULL || event == NULL) {
    return -1;
  }
  drain_wake(platform);

  SDL_PumpEvents();
  SDL_Event sdl_event;
  int result = SDL_PeepEvents(&sdl_event, 1, SDL_GETEVENT, SDL_FIRSTEVENT,
                              SDL_LASTEVENT);
  if (result < 0) {
    return -1;
  }
  if (result == 0) {
    event->type = BD_EVENT_NONE;
    event->key = BD_KEY_UNKNOWN;
    event->pressed = false;
    return 0;
  }
  map_event(&sdl_event, event);
  return 1;
}

void bd_platform_present(bd_platform *platform) {
  if (platform == NULL || platform->window == NULL) {
    return;
  }
  SDL_GL_SwapWindow(platform->window);
}
