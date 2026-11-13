#ifndef BRICKDESK_HOME_H
#define BRICKDESK_HOME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "brickdesk/gfx.h"

/*
 * v3 home page.
 *
 * The home draws the infinite cover wall (four fixed rows, 188 px cells,
 * 5 px gutters, right-growing first-fit layout), the floating top bar, the
 * hold-to-show hint bar, the quick panel, the end-process dialog and the
 * launch/return transitions.  It owns no session or input state: callers set
 * items, running apps and status, feed logical actions, advance time and ask
 * whether another frame is needed.  It draws into the shared bd_gfx renderer
 * and questions damage itself; the event loop only has to call update() and
 * draw() and honour needs_frame_until().
 *
 * Time is supplied by the caller as monotonic milliseconds.  Start functions
 * use the last value passed to bd_home_update(), so call update() once before
 * starting an animation.
 */

typedef struct bd_home bd_home;

enum {
  BD_HOME_TAB_ALL = 0,
  BD_HOME_TAB_GAMES = 1,
  BD_HOME_TAB_APPS = 2,
  BD_HOME_TAB_FAVORITES = 3,
  BD_HOME_TAB_COUNT = 4
};

enum {
  BD_HOME_ITEM_LIVE = 1u << 0,
  BD_HOME_ITEM_GAME = 1u << 1,
  BD_HOME_ITEM_FAVORITE = 1u << 2
};

typedef struct bd_home_item {
  const char *id;            /* required, unique; running state is keyed by it */
  const char *title;         /* bottom-left bold label */
  const char *subtitle;      /* light grey second line; NULL hides it */
  const char *image_path;    /* cover image; NULL draws the placeholder */
  bd_color placeholder;      /* used before the cover decodes / without one */
  int span_w;                /* 1..3, 0 means 1 */
  int span_h;                /* 1..2, 0 means 1 */
  uint32_t flags;            /* BD_HOME_ITEM_* */
  int badge_number;          /* top-left number, 0 means index + 1 */
} bd_home_item;

typedef struct bd_home_style {
  bd_color accent_base;      /* required; bd_config_accent_hex() output */
  bool dark;
  bool blur_enabled;         /* false: plain gradients for the floating bars */
  int width;                 /* 0 uses the renderer width */
  int height;                /* 0 uses the renderer height */
} bd_home_style;

typedef struct bd_home_status {
  const char *clock;         /* "22:26"; NULL keeps the default */
  bool wifi_known;           /* false: wireless state is unknown */
  bool wifi;                 /* valid only when wifi_known */
  int battery_percent;       /* 0..100, or -1 when unknown */
  int max_background;        /* quick panel "N / M"; 0 means 3 */
} bd_home_status;

typedef struct bd_home_live {
  const char *title;
  const char *artist;
  const char *previous_line;
  const char *current_line;
  const char *next_line;
  float progress;            /* 0..1 */
  int elapsed_seconds;
  int duration_seconds;
  bool playing;
} bd_home_live;

#define BD_HOME_QUERY_MAX 512

typedef struct bd_home_stats {
  int total_items;
  int visited_items;         /* distinct indexed items examined for the view */
  int visible_items;         /* items intersecting the viewport */
  int drawn_items;
  int buckets_visited;       /* wall columns examined */
  int focus_index;
  int running_count;
  int pending_images;
  int resident_textures;
  size_t resident_texture_bytes;
  int wall_width;
  int free_cells;            /* empty grid cells at the right end */
  int pan_x;                 /* current wall offset in pixels */
  bool panning;              /* a 220 ms pan is in flight */
} bd_home_stats;

typedef enum bd_home_action {
  BD_HOME_ACTION_LEFT = 0,
  BD_HOME_ACTION_RIGHT,
  BD_HOME_ACTION_UP,
  BD_HOME_ACTION_DOWN,
  BD_HOME_ACTION_ACCEPT,
  BD_HOME_ACTION_CANCEL,
  BD_HOME_ACTION_END,        /* X: end the focused/running app */
  BD_HOME_ACTION_QUICK,      /* START: toggle the quick panel */
  BD_HOME_ACTION_TAB_PREV,   /* L1 */
  BD_HOME_ACTION_TAB_NEXT,   /* R1 */
  BD_HOME_ACTION_RECENT      /* HOME: focus the most recent background app */
} bd_home_action;

typedef enum bd_home_dialog_kind {
  BD_HOME_DIALOG_NONE = 0,
  BD_HOME_DIALOG_END_APP,     /* end the running app on the focused tile */
  BD_HOME_DIALOG_BACKGROUND_FULL /* end the LRU app and launch another */
} bd_home_dialog_kind;

/*
 * A dialog result is reported once, after the user confirms or cancels, so the
 * event loop can act on the session (end a process or evict the LRU entry).
 * Take it after bd_home_input() or bd_home_take_dialog_result().
 */
typedef enum bd_home_dialog_result {
  BD_HOME_DIALOG_RESULT_NONE = 0,
  BD_HOME_DIALOG_RESULT_CANCEL,
  BD_HOME_DIALOG_RESULT_CONFIRM
} bd_home_dialog_result;

bd_home *bd_home_create(bd_gfx *gfx, const bd_home_style *style);
void bd_home_destroy(bd_home *home);

/* Copies strings; returns false on allocation failure or a missing/duplicate id. */
bool bd_home_set_items(bd_home *home, const bd_home_item *items, int count);
int bd_home_item_count(const bd_home *home);
/* Wall coordinates of one item; false when index is out of range. */
bool bd_home_item_rect(const bd_home *home, int index, bd_rect *out);

/*
 * Optional custom draw entry for a home item.  The callback is invoked with
 * visible=true only while the tile intersects the viewport; with
 * visible=false when it leaves the viewport or the home surface is hidden, so
 * a plugin-backed tile can start on demand and stop drawing off-screen.  The
 * callback owns the tile's content; the home still owns its focus ring and
 * damage.  It must be called after bd_home_set_items().
 */
typedef void (*bd_home_custom_draw_fn)(void *user, bd_home *home, bd_gfx *gfx,
                                       bd_rect rect, bool visible);
bool bd_home_set_custom_draw(bd_home *home, const char *id,
                             bd_home_custom_draw_fn draw, void *user);
/* Tells custom tiles whether the home surface itself is on screen. */
void bd_home_set_surface_visible(bd_home *home, bool visible);

int bd_home_focus(const bd_home *home);
void bd_home_set_focus(bd_home *home, int index);
/*
 * Item id A would act on right now: the selected running row while the quick
 * panel is open, otherwise the focused wall tile.  Returns NULL for quick
 * panel control rows and while the dialog or a transition owns input.
 */
const char *bd_home_selected_id(const bd_home *home);
int bd_home_tab(const bd_home *home);
void bd_home_set_tab(bd_home *home, int tab);

void bd_home_set_theme(bd_home *home, bool dark, bd_color accent_base);
void bd_home_set_blur_enabled(bd_home *home, bool enabled);
void bd_home_set_status(bd_home *home, const bd_home_status *status);
void bd_home_set_live(bd_home *home, const bd_home_live *live);

/*
 * Marks an app as running and loads its last frame through the M1-06
 * bd_app_last_frame API.  last_frame_path may be NULL or unreadable; the tile
 * then keeps its cover and still shows the running badge.  memory_mb is a
 * sampled process-tree RSS in MiB, or -1 when the sample is unavailable.  A
 * value of -1 is rendered as "unknown" and is never used for eviction.
 * Re-setting an existing id updates it and moves it to the most-recent end of
 * the quick list.
 */
bool bd_home_set_running(bd_home *home, const char *id,
                         const char *last_frame_path,
                         bool paused, int memory_mb);
/* Updates the memory reading without changing quick-list order. */
bool bd_home_set_running_memory(bd_home *home, const char *id, int memory_mb);
/* Sampled process-tree RSS in MiB, or -1 when unknown / not running. */
int bd_home_running_memory(const bd_home *home, const char *id);
void bd_home_clear_running(bd_home *home, const char *id);
int bd_home_running_count(const bd_home *home);

/* Returns true when the action changed focus/panel/dialog/animation state. */
bool bd_home_input(bd_home *home, bd_home_action action);

void bd_home_update(bd_home *home, uint64_t now_ms);
void bd_home_draw(bd_home *home, bd_gfx *gfx);
/* Forces a full-screen repaint (theme change, first frame, resize). */
void bd_home_damage(bd_home *home);

/*
 * True while an animation or image decode is in flight.  until_ms receives the
 * latest animation deadline (or now + 16 ms while images decode), so a loop can
 * block instead of polling at 60 Hz.  Returns false once settled.
 */
bool bd_home_needs_frame_until(const bd_home *home, uint64_t *until_ms);
bool bd_home_has_pending_images(const bd_home *home);
bd_home_stats bd_home_get_stats(const bd_home *home);

/* Queries the column index for a wall-space rectangle; returns item count. */
int bd_home_query_items(const bd_home *home, bd_rect world_rect, int *out,
                        int capacity);

/* Launch: the tile grows to full screen over 420 ms, then fades to the app
 * view.  launch_complete() tells the loop to hand the display to the app. */
bool bd_home_start_launch(bd_home *home, const char *id);
const char *bd_home_launch_target(const bd_home *home);
bool bd_home_launch_complete(const bd_home *home);

/* Return: focus/pan to the tile immediately and shrink the last frame into it
 * over 420 ms. */
bool bd_home_start_return(bd_home *home, const char *id);

/* Quick panel: slides 200 ms; the running list is always rebuilt from state. */
bool bd_home_open_quick(bd_home *home);
bool bd_home_close_quick(bd_home *home);
bool bd_home_quick_open(const bd_home *home);

/* Hint bar: animate in/out over 200 ms; input wiring stays outside. */
void bd_home_set_hint_visible(bd_home *home, bool visible);
bool bd_home_hint_visible(const bd_home *home);

/* End-process dialog: 180 ms; confirm starts the 180 ms tile close animation. */
bool bd_home_open_end_dialog(bd_home *home, const char *id);
/*
 * "Background full" choice: the session refused a launch because the
 * max_background slots are taken.  Shows the LRU app and the app that would
 * start; confirm means "end the LRU app and start", cancel leaves both alone.
 * The loop remains responsible for terminating the process and retrying.
 */
bool bd_home_open_background_full(bd_home *home, const char *pending_id,
                                  const char *victim_id, int max_background);
/* Keeps the current dialog open and shows a non-fatal action error. */
void bd_home_set_dialog_error(bd_home *home, const char *message);
const char *bd_home_dialog_error(const bd_home *home);
void bd_home_cancel_dialog(bd_home *home);
bool bd_home_confirm_dialog(bd_home *home);
bool bd_home_dialog_open(const bd_home *home);
bd_home_dialog_result bd_home_take_dialog_result(bd_home *home);

#endif /* BRICKDESK_HOME_H */
