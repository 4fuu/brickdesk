#ifndef BRICKDESK_DESKTOP_H
#define BRICKDESK_DESKTOP_H

#include <stdbool.h>
#include <stddef.h>

#include "brickdesk/app.h"
#include "brickdesk/gfx.h"
#include "brickdesk/input.h"
#include "brickdesk/plugin.h"
#include "brickdesk/theme.h"

/*
 * M2 desktop shell: an infinite cover wall with floating, blurred top and
 * bottom bars. The wall owns focus and per-tile damage; the desktop owns the
 * chrome and the runtime accent palette.
 */

typedef struct bd_desktop bd_desktop;
typedef struct bd_plugin bd_plugin;

typedef struct bd_desktop_theme {
  bool ambient_baked;
  bd_color accent;
} bd_desktop_theme;

/* Requires a current GLES2 context and a live renderer. */
bd_desktop *bd_desktop_create(bd_gfx *gfx, const bd_desktop_theme *theme);
void bd_desktop_destroy(bd_desktop *desktop);

void bd_desktop_draw(bd_desktop *desktop, bd_gfx *gfx);

/*
 * Replaces the wall's app tiles with `apps`. The first apps occupy the demo
 * wall's existing slots so a short list stays mixed with the placeholder
 * tiles; an empty list restores the pure demo wall. The desktop copies what it
 * needs, so `apps` may be released after the call.
 */
bool bd_desktop_set_apps(bd_desktop *desktop, const bd_app *const *apps,
                         size_t count);
/* App id of the focused tile, or NULL when the focused tile is a demo tile. */
const char *bd_desktop_focused_app_id(const bd_desktop *desktop);
/*
 * Updates the running marker and, when available, loads the session's
 * last-frame file into the tile. `last_frame_path` may be NULL.
 */
void bd_desktop_set_app_running(bd_desktop *desktop, const char *app_id,
                                bool running, const char *last_frame_path);

/* Hint bar is hidden until the ui.hint action arrives, and hidden on release. */
void bd_desktop_set_hint_visible(bd_desktop *desktop, bool visible);
bool bd_desktop_hint_visible(const bd_desktop *desktop);

/*
 * Shared action path for inputd and the in-process hotkey state machine.
 * Returns true when the visible desktop changed.
 */
bool bd_desktop_handle_action(bd_desktop *desktop, const char *action,
                              bool pressed);

/* Returns true when the visible state changed and a redraw is needed. */
bool bd_desktop_handle_key(bd_desktop *desktop, bd_key key, bool pressed);

int bd_desktop_focus(const bd_desktop *desktop);
/* Restores focus (for session restore and deterministic tests). */
void bd_desktop_set_focus(bd_desktop *desktop, int index);

/* Theme accent is runtime data; focus rings and progress bars derive from it. */
void bd_desktop_set_theme(bd_desktop *desktop, const bd_desktop_theme *theme);
void bd_desktop_set_accent(bd_desktop *desktop, bd_color accent);
bd_accent_palette bd_desktop_accent(const bd_desktop *desktop);

/* True while cover images are decoding; the event loop should wake soon. */
bool bd_desktop_has_pending_images(const bd_desktop *desktop);

/*
 * Replaces the reserved demo tile with a plugin-rendered tile. The desktop
 * owns the tile's visibility and rendering; the caller owns the plugin host
 * and must destroy the desktop (which frees the tile texture) before the host.
 */
void bd_desktop_set_plugin_tile(bd_desktop *desktop, bd_plugin *plugin);

/*
 * Draws a plugin's declarative settings tree in a centered panel over the
 * desktop. Must be called inside a gfx frame; the caller draws the desktop
 * first. Returns false when there is no settings tree.
 */
bool bd_desktop_draw_plugin_settings(bd_desktop *desktop, bd_gfx *gfx,
                                     const bd_plugin_ui *settings);

#endif /* BRICKDESK_DESKTOP_H */
