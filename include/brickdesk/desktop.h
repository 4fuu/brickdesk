#ifndef BRICKDESK_DESKTOP_H
#define BRICKDESK_DESKTOP_H

#include <stdbool.h>

#include "brickdesk/gfx.h"
#include "brickdesk/input.h"

/*
 * M1 placeholder desktop.
 *
 * Draws the 1024x768 shell (header, six tiles, focus ring, footer) so the
 * rendering and input-replay pipelines have a real, deterministic surface.
 * The real cover wall replaces this in M2; the public shape stays small.
 */

typedef struct bd_desktop bd_desktop;

bd_desktop *bd_desktop_create(void);
void bd_desktop_destroy(bd_desktop *desktop);

void bd_desktop_draw(bd_desktop *desktop, bd_gfx *gfx);

/* Returns true when the visible state changed and a redraw is needed. */
bool bd_desktop_handle_key(bd_desktop *desktop, bd_key key, bool pressed);

int bd_desktop_focus(const bd_desktop *desktop);

#endif /* BRICKDESK_DESKTOP_H */
