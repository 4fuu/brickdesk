#ifndef BRICKDESK_PLATFORM_H
#define BRICKDESK_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#include "brickdesk/input.h"

#define BD_DEFAULT_WIDTH 1024
#define BD_DEFAULT_HEIGHT 768

typedef struct bd_platform bd_platform;

typedef struct bd_platform_config {
  const char *title;
  int width;
  int height;
  bool hidden;
} bd_platform_config;

typedef enum bd_event_type {
  BD_EVENT_NONE = 0,
  BD_EVENT_QUIT,
  BD_EVENT_REDRAW,
  BD_EVENT_KEY
} bd_event_type;

typedef struct bd_event {
  bd_event_type type;
  bd_key key;
  bool pressed;
} bd_event;

/* Driver information, valid while the platform (GL context) is alive. */
typedef struct bd_gl_info {
  const char *vendor;
  const char *renderer;
  const char *version;
  const char *shading_language;
  const char *extensions;
} bd_gl_info;

/*
 * Creates an SDL2 window and a GLES2 context. On the host this works headless
 * with SDL_VIDEODRIVER=offscreen. Returns NULL on failure after printing the
 * reason to stderr.
 */
bd_platform *bd_platform_create(const bd_platform_config *config);
void bd_platform_destroy(bd_platform *platform);

int bd_platform_width(const bd_platform *platform);
int bd_platform_height(const bd_platform *platform);

void bd_platform_gl_info(const bd_platform *platform, bd_gl_info *out);
/* Prints vendor/renderer/version always; extensions only when verbose. */
void bd_gl_print_info(const bd_gl_info *info, bool verbose);

/* Milliseconds since SDL start. Wraps after ~49 days; compare as int32_t. */
uint32_t bd_platform_ticks_ms(void);

/*
 * Waits up to timeout_ms for an event (-1 blocks indefinitely). Returns 1 when
 * an event was written, 0 on timeout, -1 on error. Events that do not matter
 * to the desktop are returned as BD_EVENT_NONE.
 */
int bd_platform_wait_event(bd_platform *platform, bd_event *event, int timeout_ms);

void bd_platform_present(bd_platform *platform);

/*
 * Read end of a self-pipe that becomes readable when SDL has queued an event.
 * The desktop loop polls it together with the service sockets so it can block
 * once for all sources. Returns -1 when the platform could not create it.
 */
int bd_platform_event_fd(const bd_platform *platform);
/* Drains the self-pipe and maps one queued SDL event: 1, 0 when empty, -1. */
int bd_platform_pump_event(bd_platform *platform, bd_event *event);

#endif /* BRICKDESK_PLATFORM_H */
