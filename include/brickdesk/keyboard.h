#ifndef BRICKDESK_KEYBOARD_H
#define BRICKDESK_KEYBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "brickdesk/gfx.h"
#include "brickdesk/input.h"
#include "brickdesk/ui.h"

/*
 * Gamepad-operated on-screen keyboard.
 *
 * The caller owns the instance and routes key edges while is_open() is true.
 * D-pad moves, A types, B deletes, X switches to symbols/letters and Y accepts.
 * The bottom row also has explicit cancel and done keys.  Password fields draw
 * bullets instead of the buffer.
 */

#define BD_KEYBOARD_TEXT_MAX 128

typedef void (*bd_keyboard_done_fn)(void *user, bool accepted,
                                    const char *text);

typedef struct bd_keyboard bd_keyboard;

bd_keyboard *bd_keyboard_create(void);
void bd_keyboard_destroy(bd_keyboard *keyboard);

bool bd_keyboard_open(bd_keyboard *keyboard, const char *title,
                      const char *initial, size_t max_length, bool mask,
                      bd_keyboard_done_fn on_done, void *user);
void bd_keyboard_close(bd_keyboard *keyboard);
bool bd_keyboard_is_open(const bd_keyboard *keyboard);
const char *bd_keyboard_text(const bd_keyboard *keyboard);

/* Returns true when the edge was consumed. */
bool bd_keyboard_input(bd_keyboard *keyboard, bd_key key, bool pressed);

void bd_keyboard_draw(bd_keyboard *keyboard, bd_gfx *gfx,
                      const bd_ui_fonts *fonts, bd_color accent,
                      uint64_t now_ms);

#endif /* BRICKDESK_KEYBOARD_H */
