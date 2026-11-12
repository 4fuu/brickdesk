#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

/*
 * Mode-A in-app menu: LD_PRELOAD hooks for the application's present calls.
 *
 * The library never starts a thread and never draws while the menu is closed.
 * The closed path is one monotonic clock read per present plus an inputd
 * socket read at most every 50 ms. The open path blocks the rendering thread
 * inside the hooked present, saves the application's GL state, draws with the
 * application's own GLES2 context, and restores that state before returning.
 */

#include "menu_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "brickdesk/app.h"
#include "brickdesk/config.h"

/* EGL constants, kept local so the shim has no EGL link-time dependency. */
#define BD_EGL_WIDTH 0x3057
#define BD_EGL_HEIGHT 0x3056

typedef unsigned (*bd_egl_swap_fn)(void *, void *);
typedef unsigned (*bd_egl_swap_damage_fn)(void *, void *, int32_t *,
                                          int32_t);
typedef unsigned (*bd_egl_query_surface_fn)(void *, void *, int32_t, int32_t *);
typedef void *(*bd_egl_get_current_context_fn)(void);
typedef void (*bd_sdl_gl_swap_fn)(void *);
typedef void (*bd_sdl_render_present_fn)(void *);
typedef void (*bd_sdl_get_drawable_fn)(void *, int *, int *);
typedef void *(*bd_sdl_get_current_window_fn)(void);
typedef void *(*bd_sdl_gl_get_current_context_fn)(void);

typedef enum present_kind {
  PRESENT_NONE = 0,
  PRESENT_SDL_GL,
  PRESENT_EGL,
  PRESENT_SDL_RENDER
} present_kind;

typedef struct bd_menu_present {
  present_kind kind;
  void *a;
  void *b;
} bd_menu_present;

typedef struct bd_menu_shim {
  bool disabled;

  pthread_mutex_t menu_lock;
  bool busy;
  bool open;
  bool pending_quit;
  unsigned open_count;

  bd_menu_input input;
  bd_menu_perf perf;
  bd_menu_system_ops system_ops;
  bd_menu_model model;
  bd_menu_gl *gl;
  uint64_t last_frame_seq;
  uint64_t last_poll_ms;

  char accent_hex[8];
  char asset_dir[1024];
  char capture_dir[BD_MENU_CAPTURE_DIR];
  unsigned capture_seq;
  char app_id[BD_APP_ID_MAX];

  bd_egl_swap_fn real_egl_swap;
  bd_egl_swap_damage_fn real_egl_swap_damage;
  bd_egl_query_surface_fn real_egl_query_surface;
  bd_egl_get_current_context_fn real_egl_get_current_context;
  bd_sdl_gl_swap_fn real_sdl_gl_swap;
  bd_sdl_render_present_fn real_sdl_render_present;
  bd_sdl_get_drawable_fn real_sdl_get_drawable;
  bd_sdl_get_current_window_fn real_sdl_get_current_window;
  bd_sdl_gl_get_current_context_fn real_sdl_gl_get_current_context;
} bd_menu_shim;

static bd_menu_shim g_shim;
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_ops_lock = PTHREAD_MUTEX_INITIALIZER;
static bd_menu_system_ops g_installed_ops;
static bool g_installed_ops_valid;
static int g_stub_brightness = 70;
static int g_stub_volume = 50;

static __thread bool tl_in_present;
static __thread bool tl_menu_owner;

static void debug_log(const char *message) {
  if (getenv("BRICKDESK_MENU_DEBUG") != NULL) {
    fprintf(stderr, "brickdesk-menu: %s\n", message);
  }
}

/* ----------------------------------------------------------- stub backends */

static int stub_brightness_get(void *user) {
  (void)user;
  return g_stub_brightness;
}

static bool stub_brightness_set(void *user, int percent) {
  (void)user;
  g_stub_brightness = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
  return true;
}

static int stub_volume_get(void *user) {
  (void)user;
  return g_stub_volume;
}

static bool stub_volume_set(void *user, int percent) {
  (void)user;
  g_stub_volume = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
  return true;
}

__attribute__((visibility("default"))) void bd_menu_system_ops_stub(
    bd_menu_system_ops *out) {
  if (out == NULL) {
    return;
  }
  memset(out, 0, sizeof(*out));
  out->brightness_get = stub_brightness_get;
  out->brightness_set = stub_brightness_set;
  out->volume_get = stub_volume_get;
  out->volume_set = stub_volume_set;
}

__attribute__((visibility("default"))) void bd_menu_set_system_ops(
    const bd_menu_system_ops *ops) {
  pthread_mutex_lock(&g_ops_lock);
  if (ops == NULL) {
    g_installed_ops_valid = false;
    memset(&g_installed_ops, 0, sizeof(g_installed_ops));
  } else {
    g_installed_ops = *ops;
    g_installed_ops_valid = true;
  }
  pthread_mutex_unlock(&g_ops_lock);
}

__attribute__((visibility("default"))) bool bd_menu_visible(void) {
  return __atomic_load_n(&g_shim.open, __ATOMIC_ACQUIRE);
}

__attribute__((visibility("default"))) unsigned bd_menu_open_count(void) {
  return __atomic_load_n(&g_shim.open_count, __ATOMIC_ACQUIRE);
}

/* ---------------------------------------------------------------- init */

static void *resolve_next(const char *name) {
  return dlsym(RTLD_NEXT, name);
}

static void shim_init_once(void) {
  pthread_mutex_init(&g_shim.menu_lock, NULL);
  bd_menu_input_init(&g_shim.input);
  bd_menu_perf_init(&g_shim.perf);
  snprintf(g_shim.accent_hex, sizeof(g_shim.accent_hex), "%s", "#e07a5f");
  snprintf(g_shim.asset_dir, sizeof(g_shim.asset_dir), "%s", bd_menu_asset_dir());

  const char *disabled = getenv("BRICKDESK_MENU_DISABLE");
  if (disabled != NULL && disabled[0] != '\0' && strcmp(disabled, "0") != 0) {
    g_shim.disabled = true;
  }
  const char *mode = getenv("BRICKDESK_MENU_MODE");
  if (mode != NULL && mode[0] != '\0' && strcmp(mode, "overlay") != 0) {
    /* handoff, embedded and none are not served by this shim. */
    g_shim.disabled = true;
  }
  if (g_shim.disabled) {
    return;
  }

  const char *capture = getenv("BRICKDESK_MENU_CAPTURE");
  if (capture != NULL) {
    snprintf(g_shim.capture_dir, sizeof(g_shim.capture_dir), "%s", capture);
  }
  const char *app_id = getenv("BRICKDESK_APP_ID");
  if (app_id != NULL) {
    snprintf(g_shim.app_id, sizeof(g_shim.app_id), "%s", app_id);
  }

  bd_config_options options;
  memset(&options, 0, sizeof(options));
  options.config_dir = NULL;
  options.migrate = false;
  char error[256] = {0};
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  if (config != NULL) {
    const char *accent = bd_config_get_string(config, "theme.accent", "persimmon");
    const char *hex = bd_config_accent_hex(accent, true);
    if (hex != NULL && hex[0] == '#' && strlen(hex) == 7) {
      snprintf(g_shim.accent_hex, sizeof(g_shim.accent_hex), "%s", hex);
    }
    long brightness = bd_config_get_int(config, "display.brightness", 70);
    if (brightness >= 0 && brightness <= 100) {
      g_stub_brightness = (int)brightness;
    }
    bd_config_free(config);
  }

  g_shim.real_egl_swap = (bd_egl_swap_fn)resolve_next("eglSwapBuffers");
  g_shim.real_egl_swap_damage =
      (bd_egl_swap_damage_fn)resolve_next("eglSwapBuffersWithDamageKHR");
  g_shim.real_egl_query_surface =
      (bd_egl_query_surface_fn)resolve_next("eglQuerySurface");
  g_shim.real_egl_get_current_context =
      (bd_egl_get_current_context_fn)resolve_next("eglGetCurrentContext");
  g_shim.real_sdl_gl_swap =
      (bd_sdl_gl_swap_fn)resolve_next("SDL_GL_SwapWindow");
  g_shim.real_sdl_render_present =
      (bd_sdl_render_present_fn)resolve_next("SDL_RenderPresent");
  g_shim.real_sdl_get_drawable =
      (bd_sdl_get_drawable_fn)resolve_next("SDL_GL_GetDrawableSize");
  g_shim.real_sdl_get_current_window =
      (bd_sdl_get_current_window_fn)resolve_next("SDL_GL_GetCurrentWindow");
  g_shim.real_sdl_gl_get_current_context =
      (bd_sdl_gl_get_current_context_fn)resolve_next(
          "SDL_GL_GetCurrentContext");
  if (getenv("BRICKDESK_MENU_DEBUG") != NULL) {
    char message[256];
    snprintf(message, sizeof(message),
             "init disabled=%d sdl_swap=%p egl_swap=%p input=%s",
             g_shim.disabled ? 1 : 0, (void *)g_shim.real_sdl_gl_swap,
             (void *)g_shim.real_egl_swap,
             getenv("BRICKDESK_INPUT_SOCKET"));
    debug_log(message);
  }
}

static void shim_init(void) {
  pthread_once(&g_init_once, shim_init_once);
}

/* ------------------------------------------------------------ present path */

static void present_real(const bd_menu_present *present) {
  bool previous = tl_in_present;
  tl_in_present = true;
  switch (present->kind) {
    case PRESENT_SDL_GL:
      if (g_shim.real_sdl_gl_swap != NULL) {
        g_shim.real_sdl_gl_swap(present->a);
      }
      break;
    case PRESENT_EGL:
      if (g_shim.real_egl_swap != NULL) {
        g_shim.real_egl_swap(present->a, present->b);
      } else if (g_shim.real_egl_swap_damage != NULL) {
        g_shim.real_egl_swap_damage(present->a, present->b, NULL, 0);
      }
      break;
    case PRESENT_SDL_RENDER:
      if (g_shim.real_sdl_render_present != NULL) {
        g_shim.real_sdl_render_present(present->a);
      }
      break;
    default:
      break;
  }
  tl_in_present = previous;
}

static void present_size(const bd_menu_present *present, int *width,
                         int *height) {
  int w = 0;
  int h = 0;
  if (present->kind == PRESENT_SDL_GL && g_shim.real_sdl_get_drawable != NULL) {
    g_shim.real_sdl_get_drawable(present->a, &w, &h);
  } else if (present->kind == PRESENT_EGL &&
             g_shim.real_egl_query_surface != NULL) {
    int32_t value = 0;
    if (g_shim.real_egl_query_surface(present->a, present->b, BD_EGL_WIDTH,
                                      &value)) {
      w = value;
    }
    value = 0;
    if (g_shim.real_egl_query_surface(present->a, present->b, BD_EGL_HEIGHT,
                                      &value)) {
      h = value;
    }
  } else if (present->kind == PRESENT_SDL_RENDER &&
             g_shim.real_sdl_get_current_window != NULL &&
             g_shim.real_sdl_get_drawable != NULL) {
    void *window = g_shim.real_sdl_get_current_window();
    if (window != NULL) {
      g_shim.real_sdl_get_drawable(window, &w, &h);
    }
  }
  if (w < 1 || h < 1 || w > 4096 || h > 4096) {
    w = BD_MENU_DESIGN_WIDTH;
    h = BD_MENU_DESIGN_HEIGHT;
  }
  *width = w;
  *height = h;
}

static bool is_menu_open_action(const char *line) {
  char type[32];
  char action[64];
  char phase[16];
  if (!bd_menu_json_string(line, "type", type, sizeof(type)) ||
      strcmp(type, "action") != 0) {
    return false;
  }
  if (!bd_menu_json_string(line, "action", action, sizeof(action)) ||
      strcmp(action, "menu.open") != 0) {
    return false;
  }
  if (bd_menu_json_string(line, "phase", phase, sizeof(phase)) &&
      strcmp(phase, "down") != 0) {
    return false;
  }
  return true;
}

static void write_last_frame(bd_menu_gl *gl) {
  const char *path = getenv("BRICKDESK_LAST_FRAME");
  if (path == NULL || path[0] == '\0') {
    return;
  }
  uint8_t *pixels = NULL;
  int width = 0;
  int height = 0;
  if (!bd_menu_gl_read_pixels(gl, false, &pixels, &width, &height)) {
    return;
  }
  char error[128] = {0};
  if (!bd_app_last_frame_write(path, (uint32_t)width, (uint32_t)height,
                               BD_APP_PIXEL_RGBA8888, pixels,
                               (size_t)width * 4u, &g_shim.last_frame_seq, error,
                               sizeof(error))) {
    debug_log(error);
  }
  free(pixels);
}

static void capture_menu_frame(bd_menu_gl *gl) {
  if (g_shim.capture_dir[0] == '\0') {
    return;
  }
  char path[BD_MENU_CAPTURE_DIR + 64];
  snprintf(path, sizeof(path), "%s/menu-%04u.png", g_shim.capture_dir,
           ++g_shim.capture_seq);
  if (!bd_menu_gl_dump_png(gl, path)) {
    debug_log("menu capture failed");
  }
}

static void redraw_menu(const bd_menu_present *present) {
  if (g_shim.gl == NULL) {
    return;
  }
  bd_menu_gl_draw_menu(g_shim.gl, &g_shim.model);
  capture_menu_frame(g_shim.gl);
  present_real(present);
}

static void load_system_ops(void) {
  pthread_mutex_lock(&g_ops_lock);
  if (g_installed_ops_valid) {
    g_shim.system_ops = g_installed_ops;
  } else {
    bd_menu_system_ops_stub(&g_shim.system_ops);
  }
  pthread_mutex_unlock(&g_ops_lock);
}

static void adjust_brightness(int delta) {
  int value = g_shim.model.brightness;
  if (g_shim.system_ops.brightness_get != NULL) {
    int current = g_shim.system_ops.brightness_get(g_shim.system_ops.user);
    if (current >= 0) {
      value = current;
    }
  }
  value += delta;
  if (value < 0) {
    value = 0;
  }
  if (value > 100) {
    value = 100;
  }
  if (g_shim.system_ops.brightness_set != NULL) {
    g_shim.system_ops.brightness_set(g_shim.system_ops.user, value);
  }
  g_shim.model.brightness = value;
}

static void adjust_volume(int delta) {
  int value = g_shim.model.volume;
  if (g_shim.system_ops.volume_get != NULL) {
    int current = g_shim.system_ops.volume_get(g_shim.system_ops.user);
    if (current >= 0) {
      value = current;
    }
  }
  value += delta;
  if (value < 0) {
    value = 0;
  }
  if (value > 100) {
    value = 100;
  }
  if (g_shim.system_ops.volume_set != NULL) {
    g_shim.system_ops.volume_set(g_shim.system_ops.user, value);
  }
  g_shim.model.volume = value;
}

static void cycle_profile(int delta) {
  if (!g_shim.model.perf_available || g_shim.model.profile_count <= 0) {
    return;
  }
  int previous = g_shim.model.profile_index;
  int next = previous + delta;
  if (next < 0) {
    next = g_shim.model.profile_count - 1;
  }
  if (next >= g_shim.model.profile_count) {
    next = 0;
  }
  if (!bd_menu_perf_apply(&g_shim.perf, g_shim.app_id,
                          g_shim.model.profiles[next])) {
    snprintf(g_shim.model.status, sizeof(g_shim.model.status), "%s",
             "切换失败");
    g_shim.model.profile_index = previous;
    return;
  }
  g_shim.model.profile_index = next;
  g_shim.model.status[0] = '\0';
}

static void menu_activate(const bd_menu_present *present) {
  switch ((bd_menu_item)g_shim.model.focus) {
    case BD_MENU_ITEM_RESUME:
      __atomic_store_n(&g_shim.open, false, __ATOMIC_RELEASE);
      break;
    case BD_MENU_ITEM_BACKGROUND:
      if (bd_menu_session_send("background")) {
        debug_log("menu action: background");
        __atomic_store_n(&g_shim.open, false, __ATOMIC_RELEASE);
      } else {
        debug_log("menu action: background failed");
        snprintf(g_shim.model.status, sizeof(g_shim.model.status), "%s",
                 "桌面不可用");
        redraw_menu(present);
      }
      break;
    case BD_MENU_ITEM_QUIT:
      if (bd_menu_session_send("quit")) {
        debug_log("menu action: quit");
        __atomic_store_n(&g_shim.open, false, __ATOMIC_RELEASE);
      } else {
        debug_log("menu action: quit fallback");
        g_shim.pending_quit = true;
        __atomic_store_n(&g_shim.open, false, __ATOMIC_RELEASE);
      }
      break;
    default:
      break;
  }
}

static void menu_key(const bd_menu_present *present, const char *key,
                     bool pressed) {
  if (!pressed) {
    return;
  }
  g_shim.model.status[0] = '\0';
  if (strcmp(key, "UP") == 0) {
    g_shim.model.focus =
        (g_shim.model.focus + BD_MENU_ITEM_COUNT - 1) % BD_MENU_ITEM_COUNT;
    redraw_menu(present);
    return;
  }
  if (strcmp(key, "DOWN") == 0) {
    g_shim.model.focus =
        (g_shim.model.focus + 1) % BD_MENU_ITEM_COUNT;
    redraw_menu(present);
    return;
  }
  if (strcmp(key, "LEFT") == 0 || strcmp(key, "RIGHT") == 0) {
    int delta = strcmp(key, "LEFT") == 0 ? -5 : 5;
    if (g_shim.model.focus == BD_MENU_ITEM_BRIGHTNESS) {
      adjust_brightness(delta);
      redraw_menu(present);
    } else if (g_shim.model.focus == BD_MENU_ITEM_VOLUME) {
      adjust_volume(delta);
      redraw_menu(present);
    } else if (g_shim.model.focus == BD_MENU_ITEM_PERF) {
      cycle_profile(strcmp(key, "LEFT") == 0 ? -1 : 1);
      redraw_menu(present);
    }
    return;
  }
  if (strcmp(key, "A") == 0) {
    menu_activate(present);
    return;
  }
  if (strcmp(key, "B") == 0 || strcmp(key, "MENU") == 0) {
    __atomic_store_n(&g_shim.open, false, __ATOMIC_RELEASE);
  }
}

static void menu_handle_line(const bd_menu_present *present, const char *line) {
  char type[32];
  if (!bd_menu_json_string(line, "type", type, sizeof(type))) {
    return;
  }
  if (strcmp(type, "key") == 0) {
    char key[32];
    if (!bd_menu_json_string(line, "key", key, sizeof(key))) {
      return;
    }
    menu_key(present, key, bd_menu_json_bool(line, "pressed", false));
    return;
  }
  if (strcmp(type, "action") == 0) {
    char action[64];
    char phase[16];
    if (!bd_menu_json_string(line, "action", action, sizeof(action))) {
      return;
    }
    if (strcmp(action, "menu.open") == 0) {
      if (!bd_menu_json_string(line, "phase", phase, sizeof(phase)) ||
          strcmp(phase, "down") == 0) {
        __atomic_store_n(&g_shim.open, false, __ATOMIC_RELEASE);
      }
    }
    return;
  }
  /* grab, release_all, hello and pong need no model change. */
}

static void close_menu(const bd_menu_present *present) {
  if (g_shim.gl != NULL) {
    bd_menu_gl_draw_background(g_shim.gl);
    present_real(present);
    bd_menu_gl_destroy(g_shim.gl);
    g_shim.gl = NULL;
  }
  bd_menu_input_release_all(&g_shim.input);
  bd_menu_input_ungrab(&g_shim.input);
  tl_menu_owner = false;
  __atomic_store_n(&g_shim.busy, false, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&g_shim.menu_lock);
  if (g_shim.pending_quit) {
    g_shim.pending_quit = false;
    kill(getpid(), SIGTERM);
  }
}

static bool open_menu(const bd_menu_present *present) {
  debug_log("opening overlay");
  /*
   * Drawing with no current context would touch whatever surface happens to
   * be current (or fail half-way). Leave the frame alone in that case.
   */
  bool context_checked = false;
  bool context_current = false;
  if (g_shim.real_sdl_gl_get_current_context != NULL) {
    context_checked = true;
    if (g_shim.real_sdl_gl_get_current_context() != NULL) {
      context_current = true;
    }
  }
  if (!context_current && g_shim.real_egl_get_current_context != NULL) {
    context_checked = true;
    if (g_shim.real_egl_get_current_context() != NULL) {
      context_current = true;
    }
  }
  if (context_checked && !context_current) {
    debug_log("no current GL context; overlay stays closed");
    return false;
  }
  if (!bd_menu_input_grab(&g_shim.input, 200)) {
    debug_log("inputd grab failed; overlay stays closed");
    return false;
  }
  int width = 0;
  int height = 0;
  present_size(present, &width, &height);
  bd_menu_color accent =
      bd_menu_color_hex(g_shim.accent_hex, bd_menu_color_rgba(0xE0, 0x7A, 0x5F,
                                                              255));
  bd_menu_gl *gl = bd_menu_gl_create(width, height, accent, g_shim.asset_dir);
  if (gl == NULL) {
    debug_log("GLES2 menu renderer unavailable");
    bd_menu_input_release_all(&g_shim.input);
    bd_menu_input_ungrab(&g_shim.input);
    return false;
  }

  tl_menu_owner = true;
  g_shim.gl = gl;
  __atomic_store_n(&g_shim.open, true, __ATOMIC_RELEASE);
  __atomic_add_fetch(&g_shim.open_count, 1, __ATOMIC_RELEASE);

  memset(&g_shim.model, 0, sizeof(g_shim.model));
  g_shim.model.focus = BD_MENU_ITEM_RESUME;
  g_shim.model.accent = accent;
  load_system_ops();
  g_shim.model.brightness =
      g_shim.system_ops.brightness_get != NULL
          ? g_shim.system_ops.brightness_get(g_shim.system_ops.user)
          : 70;
  g_shim.model.volume = g_shim.system_ops.volume_get != NULL
                            ? g_shim.system_ops.volume_get(g_shim.system_ops.user)
                            : 50;
  if (g_shim.model.brightness < 0) {
    g_shim.model.brightness = 70;
  }
  if (g_shim.model.volume < 0) {
    g_shim.model.volume = 50;
  }
  bd_menu_perf_open(&g_shim.perf, g_shim.app_id, &g_shim.model);

  write_last_frame(gl);
  bd_menu_gl_capture_background(gl);
  redraw_menu(present);

  while (__atomic_load_n(&g_shim.open, __ATOMIC_ACQUIRE)) {
    char line[BD_MENU_INPUT_LINE];
    int result = bd_menu_input_read(&g_shim.input, -1, line, sizeof(line));
    if (result < 0) {
      /* inputd went away; never keep the application frozen without input. */
      __atomic_store_n(&g_shim.open, false, __ATOMIC_RELEASE);
      break;
    }
    if (result == 0) {
      continue;
    }
    menu_handle_line(present, line);
  }

  close_menu(present);
  return true;
}

static bool poll_open_request(const bd_menu_present *present) {
  uint64_t now = bd_menu_now_ms();
  if (g_shim.last_poll_ms != 0 && now - g_shim.last_poll_ms < 50) {
    return false;
  }
  g_shim.last_poll_ms = now;
  if (!bd_menu_input_ensure(&g_shim.input, now)) {
    return false;
  }
  for (;;) {
    char line[BD_MENU_INPUT_LINE];
    int result = bd_menu_input_read(&g_shim.input, 0, line, sizeof(line));
    if (result <= 0) {
      break;
    }
    if (is_menu_open_action(line)) {
      debug_log("menu.open action");
      return open_menu(present);
    }
  }
  return false;
}

static bool try_menu(const bd_menu_present *present) {
  if (g_shim.disabled || present == NULL || tl_menu_owner) {
    return false;
  }
  if (__atomic_load_n(&g_shim.open, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&g_shim.busy, __ATOMIC_ACQUIRE)) {
    /* The menu thread owns the back buffer; wait until it is done. */
    pthread_mutex_lock(&g_shim.menu_lock);
    pthread_mutex_unlock(&g_shim.menu_lock);
    return false;
  }
  bool expected = false;
  if (!__atomic_compare_exchange_n(&g_shim.busy, &expected, true, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    pthread_mutex_lock(&g_shim.menu_lock);
    pthread_mutex_unlock(&g_shim.menu_lock);
    return false;
  }
  pthread_mutex_lock(&g_shim.menu_lock);
  bool opened = false;
  if (!__atomic_load_n(&g_shim.open, __ATOMIC_ACQUIRE)) {
    opened = poll_open_request(present);
  }
  if (!opened) {
    __atomic_store_n(&g_shim.busy, false, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_shim.menu_lock);
  }
  return opened;
}

/* ------------------------------------------------------------------ hooks */

__attribute__((visibility("default"))) void SDL_GL_SwapWindow(void *window) {
  shim_init();
  if (g_shim.disabled || g_shim.real_sdl_gl_swap == NULL || tl_in_present) {
    if (g_shim.real_sdl_gl_swap != NULL) {
      g_shim.real_sdl_gl_swap(window);
    }
    return;
  }
  bd_menu_present present;
  present.kind = PRESENT_SDL_GL;
  present.a = window;
  present.b = NULL;
  if (try_menu(&present)) {
    return;
  }
  present_real(&present);
}

__attribute__((visibility("default"))) unsigned
eglSwapBuffers(void *display, void *surface) {
  shim_init();
  if (g_shim.disabled || tl_in_present ||
      (g_shim.real_egl_swap == NULL && g_shim.real_egl_swap_damage == NULL)) {
    if (g_shim.real_egl_swap != NULL) {
      return g_shim.real_egl_swap(display, surface);
    }
    if (g_shim.real_egl_swap_damage != NULL) {
      return g_shim.real_egl_swap_damage(display, surface, NULL, 0);
    }
    return 0;
  }
  bd_menu_present present;
  present.kind = PRESENT_EGL;
  present.a = display;
  present.b = surface;
  if (try_menu(&present)) {
    return 1;
  }
  present_real(&present);
  return 1;
}

__attribute__((visibility("default"))) unsigned
eglSwapBuffersWithDamageKHR(void *display, void *surface, int32_t *rects,
                            int32_t count) {
  shim_init();
  if (g_shim.disabled || tl_in_present) {
    if (g_shim.real_egl_swap_damage != NULL) {
      return g_shim.real_egl_swap_damage(display, surface, rects, count);
    }
    if (g_shim.real_egl_swap != NULL) {
      return g_shim.real_egl_swap(display, surface);
    }
    return 0;
  }
  bd_menu_present present;
  present.kind = PRESENT_EGL;
  present.a = display;
  present.b = surface;
  if (try_menu(&present)) {
    return 1;
  }
  /*
   * Keep the application's damage rectangles. present_real() uses the plain
   * swap for menu frames, which must cover the whole overlay.
   */
  bool previous = tl_in_present;
  tl_in_present = true;
  unsigned result = 1;
  if (g_shim.real_egl_swap_damage != NULL) {
    result = g_shim.real_egl_swap_damage(display, surface, rects, count);
  } else if (g_shim.real_egl_swap != NULL) {
    result = g_shim.real_egl_swap(display, surface);
  } else {
    result = 0;
  }
  tl_in_present = previous;
  return result;
}

__attribute__((visibility("default"))) void
SDL_RenderPresent(void *renderer) {
  shim_init();
  if (g_shim.disabled || g_shim.real_sdl_render_present == NULL ||
      tl_in_present) {
    if (g_shim.real_sdl_render_present != NULL) {
      g_shim.real_sdl_render_present(renderer);
    }
    return;
  }
  /* Software renderers have no GL context to draw into. */
  if (g_shim.real_sdl_gl_get_current_context == NULL ||
      g_shim.real_sdl_gl_get_current_context() == NULL) {
    g_shim.real_sdl_render_present(renderer);
    return;
  }
  bd_menu_present present;
  present.kind = PRESENT_SDL_RENDER;
  present.a = renderer;
  present.b = NULL;
  if (try_menu(&present)) {
    return;
  }
  present_real(&present);
}
