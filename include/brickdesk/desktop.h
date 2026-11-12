#ifndef BRICKDESK_DESKTOP_H
#define BRICKDESK_DESKTOP_H

#include <stdbool.h>

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
  bool dark;
  bool ambient_baked;
  bd_color accent;
} bd_desktop_theme;

/* Requires a current GLES2 context and a live renderer. */
bd_desktop *bd_desktop_create(bd_gfx *gfx, const bd_desktop_theme *theme);
void bd_desktop_destroy(bd_desktop *desktop);

void bd_desktop_draw(bd_desktop *desktop, bd_gfx *gfx);

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
