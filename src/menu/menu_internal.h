#ifndef BRICKDESK_MENU_INTERNAL_H
#define BRICKDESK_MENU_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "brickdesk/menu.h"

#define BD_MENU_DESIGN_WIDTH 1024
#define BD_MENU_DESIGN_HEIGHT 768
#define BD_MENU_MAX_PROFILES 16
#define BD_MENU_PROFILE_NAME 48
#define BD_MENU_INPUT_LINE 4096
#define BD_MENU_PERF_LINE 8192
#define BD_MENU_CAPTURE_DIR 512

typedef struct bd_menu_color {
  float r;
  float g;
  float b;
  float a;
} bd_menu_color;

bd_menu_color bd_menu_color_rgba(int r, int g, int b, int a);
bd_menu_color bd_menu_color_hex(const char *hex, bd_menu_color fallback);

typedef enum bd_menu_item {
  BD_MENU_ITEM_RESUME = 0,
  BD_MENU_ITEM_BACKGROUND,
  BD_MENU_ITEM_QUIT,
  BD_MENU_ITEM_BRIGHTNESS,
  BD_MENU_ITEM_VOLUME,
  BD_MENU_ITEM_PERF,
  BD_MENU_ITEM_COUNT
} bd_menu_item;

typedef struct bd_menu_model {
  int focus;
  bd_menu_color accent;
  int brightness;
  int volume;
  char profiles[BD_MENU_MAX_PROFILES][BD_MENU_PROFILE_NAME];
  int profile_count;
  int profile_index;
  bool perf_available;
  char status[96];
} bd_menu_model;

/* --------------------------------------------------------------- renderer */

typedef struct bd_menu_gl bd_menu_gl;

bd_menu_gl *bd_menu_gl_create(int width, int height, bd_menu_color accent,
                              const char *asset_dir);
/*
 * draw_* assumes the renderer saved the application's state in create() and
 * leaves its own state current until destroy(), which restores every value
 * the renderer touched.
 */
void bd_menu_gl_draw_menu(bd_menu_gl *gl, const bd_menu_model *model);
void bd_menu_gl_draw_background(bd_menu_gl *gl);
bool bd_menu_gl_capture_background(bd_menu_gl *gl);
void bd_menu_gl_destroy(bd_menu_gl *gl);

/* Reads the default framebuffer. `top_down` flips the GL row order. */
bool bd_menu_gl_read_pixels(bd_menu_gl *gl, bool top_down, uint8_t **out,
                            int *width, int *height);
bool bd_menu_gl_dump_png(bd_menu_gl *gl, const char *path);

/* ------------------------------------------------------------- input client */

typedef struct bd_menu_input {
  int fd;
  char buffer[BD_MENU_INPUT_LINE];
  size_t length;
  uint64_t last_connect_ms;
  bool grab_active;
  bool grab_pending;
} bd_menu_input;

void bd_menu_input_init(bd_menu_input *input);
void bd_menu_input_close(bd_menu_input *input);
/* Connects and subscribes when needed. Returns false when inputd is absent. */
bool bd_menu_input_ensure(bd_menu_input *input, uint64_t now_ms);
/*
 * Returns 1 for one complete line, 0 on timeout/partial data, -1 when the
 * connection is gone. `timeout_ms` < 0 blocks.
 */
int bd_menu_input_read(bd_menu_input *input, int timeout_ms, char *line,
                       size_t line_size);
bool bd_menu_input_send(bd_menu_input *input, const char *line);
/* Asks for the evdev grab and waits for the daemon acknowledgement. */
bool bd_menu_input_grab(bd_menu_input *input, int timeout_ms);
void bd_menu_input_release_all(bd_menu_input *input);
void bd_menu_input_ungrab(bd_menu_input *input);

/* -------------------------------------------------------------- perf client */

typedef struct bd_menu_perf {
  int fd;
  int next_id;
  char buffer[BD_MENU_PERF_LINE];
  size_t length;
} bd_menu_perf;

void bd_menu_perf_init(bd_menu_perf *perf);
void bd_menu_perf_close(bd_menu_perf *perf);
/* Connects, then fills profiles/current. Returns false when unavailable. */
bool bd_menu_perf_open(bd_menu_perf *perf, const char *app_id,
                       bd_menu_model *model);
bool bd_menu_perf_apply(bd_menu_perf *perf, const char *app_id,
                        const char *profile);

/* ----------------------------------------------------------- session client */

/* Sends one {"type":"menu","action":...} line. False when no socket is set. */
bool bd_menu_session_send(const char *action);

/* --------------------------------------------------------------- utilities */

bool bd_menu_json_string(const char *json, const char *field, char *out,
                         size_t out_size);
bool bd_menu_json_bool(const char *json, const char *field, bool fallback);
uint64_t bd_menu_now_ms(void);
const char *bd_menu_asset_dir(void);

#endif /* BRICKDESK_MENU_INTERNAL_H */
