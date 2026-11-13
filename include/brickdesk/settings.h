#ifndef BRICKDESK_SETTINGS_H
#define BRICKDESK_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "brickdesk/config.h"
#include "brickdesk/gfx.h"
#include "brickdesk/input.h"
#include "brickdesk/plugin.h"

/*
 * v3 settings page.
 *
 * The page owns its focus, scroll, sub-pages and confirm dialog.  The event
 * loop only creates it, feeds logical actions (and hold edges for sliders),
 * advances a monotonic clock, draws it and honours needs_frame_until().
 *
 * Editable rows are generated from the configuration schema: bools become
 * toggles, ints and durations become sliders with the schema's min/max,
 * enums become select rows and accents become swatches.  Every change goes
 * through bd_config_set() and is persisted immediately with bd_config_save();
 * the accent change rebuilds the page palette and repaints the page.
 *
 * Brightness, volume and mute are applied through bd_settings_system, so the
 * device backends that arrive in M4 can replace the stub without touching the
 * page.  Configuration remains the persistence layer.
 *
 * The bottom hint strip is only visible while the caller holds Select:
 * bd_settings_set_hint_visible(true) on press, false on release.
 */

typedef struct bd_settings bd_settings;

typedef enum bd_settings_category {
  BD_SETTINGS_CATEGORY_APPEARANCE = 0,
  BD_SETTINGS_CATEGORY_SOUND,
  BD_SETTINGS_CATEGORY_DISPLAY,
  BD_SETTINGS_CATEGORY_KEYS,
  BD_SETTINGS_CATEGORY_PERFORMANCE,
  BD_SETTINGS_CATEGORY_BACKGROUND,
  BD_SETTINGS_CATEGORY_PLUGINS,
  BD_SETTINGS_CATEGORY_SYSTEM,
  BD_SETTINGS_CATEGORY_COUNT
} bd_settings_category;

typedef enum bd_settings_action {
  BD_SETTINGS_ACTION_UP = 0,
  BD_SETTINGS_ACTION_DOWN,
  BD_SETTINGS_ACTION_LEFT,
  BD_SETTINGS_ACTION_RIGHT,
  BD_SETTINGS_ACTION_ACCEPT,
  BD_SETTINGS_ACTION_CANCEL,
  BD_SETTINGS_ACTION_CATEGORY_PREV, /* L1 */
  BD_SETTINGS_ACTION_CATEGORY_NEXT  /* R1 */
} bd_settings_action;

typedef struct bd_settings_style {
  bd_color accent_base; /* bd_config_accent_hex() output */
  int width;            /* 0 uses the renderer width */
  int height;           /* 0 uses the renderer height */
} bd_settings_style;

typedef struct bd_settings_status {
  /* Top-bar status.  The page intentionally has no project logo; the back
   * arrow, title and category are settings chrome, and the right side keeps
   * the Wi-Fi, battery and clock readouts. */
  const char *clock;       /* "22:26"; NULL or "" hides the clock */
  bool wifi;
  int battery_percent;     /* 0..100; negative hides the battery */
} bd_settings_status;

typedef struct bd_settings_storage {
  long total_mb;
  long free_mb;
} bd_settings_storage;

/*
 * Small system seam for backlight and audio.  Configuration keeps the value;
 * these hooks only touch the hardware.  A missing hook is treated as success
 * so the page still works on hosts without a device backend.
 */
typedef struct bd_settings_system {
  void *user;
  bool (*set_brightness)(void *user, int percent);
  bool (*set_volume)(void *user, int percent);
  bool (*set_muted)(void *user, bool muted);
} bd_settings_system;

/* Stub backend used by the tests and by M3; device backends arrive in M4. */
typedef struct bd_settings_system_stub {
  int brightness;
  int volume;
  bool muted;
  int brightness_calls;
  int volume_calls;
  int mute_calls;
} bd_settings_system_stub;

void bd_settings_system_stub_init(bd_settings_system_stub *stub,
                                  bd_settings_system *out);

typedef struct bd_settings_stats {
  int rows;       /* rows on the current page, including synthetic rows */
  int hotkeys;    /* accepted bindings in the loaded hotkeys.ini */
  int conflicts;  /* conflicts reported by the input layer */
  int plugins;    /* plugins discovered by the host */
  int plugin_nodes; /* focusable nodes in the open plugin settings tree */
  int scroll;       /* current row or plugin-tree scroll offset */
  bool focus_visible; /* focused row/node is inside the table viewport */
  bool subpage;   /* a sub-page or dialog is on top of the category page */
  bool hint_visible;
  int focus;
  int category;
} bd_settings_stats;

bd_settings *bd_settings_create(bd_gfx *gfx, bd_config *config,
                                const bd_settings_style *style);
void bd_settings_destroy(bd_settings *settings);

/* Optional integrations.  The page works without any of them. */
void bd_settings_set_system(bd_settings *settings,
                            const bd_settings_system *system);
void bd_settings_set_plugins(bd_settings *settings, bd_plugin_host *host);
void bd_settings_set_status(bd_settings *settings,
                            const bd_settings_status *status);
void bd_settings_set_storage(bd_settings *settings,
                             const bd_settings_storage *storage);

/* Opens a category page, or closes it.  open() damages the whole page. */
bool bd_settings_open(bd_settings *settings, int category);
void bd_settings_close(bd_settings *settings);
bool bd_settings_is_open(const bd_settings *settings);

/* Re-reads values changed outside the page (config reload, plugin state). */
void bd_settings_refresh(bd_settings *settings);

/*
 * Feeds one action edge.  Returns true when focus, value, page or dialog
 * state changed.  LEFT/RIGHT change values; ACCEPT toggles/opens; CANCEL goes
 * back one level and closes the page at the root.
 */
bool bd_settings_input(bd_settings *settings, bd_settings_action action);

/*
 * Hold tracking for sliders.  Call on the press and release edges of LEFT or
 * RIGHT; the first press steps once, then update() repeats after
 * input.repeat_delay_ms and every input.repeat_interval_ms.  Passing the
 * action to input() as well would step twice.
 */
void bd_settings_hold(bd_settings *settings, bd_settings_action action,
                      bool pressed);

void bd_settings_update(bd_settings *settings, uint64_t now_ms);
void bd_settings_draw(bd_settings *settings, bd_gfx *gfx);
/* Forces a full repaint (open, theme change, first frame). */
void bd_settings_damage(bd_settings *settings);
/*
 * True while a held slider still has a repeat pending.  until_ms receives
 * the next deadline.  The page has no other animations.
 */
bool bd_settings_needs_frame_until(const bd_settings *settings,
                                   uint64_t *until_ms);

/* Select-hold hint strip. */
void bd_settings_set_hint_visible(bd_settings *settings, bool visible);
bool bd_settings_hint_visible(const bd_settings *settings);

int bd_settings_category_index(const bd_settings *settings);
int bd_settings_focus(const bd_settings *settings);
int bd_settings_row_count(const bd_settings *settings);
/* Stable identity of a row, e.g. "theme.accent" or "!hotkey.menu". */
const char *bd_settings_row_key(const bd_settings *settings, int index);
bool bd_settings_subpage(const bd_settings *settings);
bool bd_settings_confirm_open(const bd_settings *settings);
const char *bd_settings_last_error(const bd_settings *settings);
/* Current accent used for the focus ring and controls (after mode derivation). */
bd_color bd_settings_accent(const bd_settings *settings);
bd_settings_stats bd_settings_get_stats(const bd_settings *settings);

#endif /* BRICKDESK_SETTINGS_H */
