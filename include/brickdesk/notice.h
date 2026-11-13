#ifndef BRICKDESK_NOTICE_H
#define BRICKDESK_NOTICE_H

/*
 * Transient notices and modal confirmations drawn over the desktop.
 *
 * The layer is owned by the shell, which feeds it time and logical input and
 * draws it after the home page or the settings page.  Other modules only call
 * bd_shell_notice() / bd_shell_confirm(); they never need the internals.
 *
 * Notices are drawn only while the desktop owns the screen.  A notice raised
 * while an app is foreground stays queued and appears when the desktop comes
 * back; it does not expire behind the app.
 */

#include <stdbool.h>
#include <stdint.h>

#include "brickdesk/gfx.h"

#define BD_NOTICE_TEXT_MAX 512
#define BD_NOTICE_TITLE_MAX 128
#define BD_NOTICE_QUEUE_MAX 8
#define BD_NOTICE_DEFAULT_TIMEOUT_MS 3000u

typedef enum bd_notice_level {
  BD_NOTICE_INFO = 0,
  BD_NOTICE_SUCCESS,
  BD_NOTICE_WARNING,
  BD_NOTICE_ERROR
} bd_notice_level;

typedef enum bd_notice_action {
  BD_NOTICE_ACTION_NONE = 0,
  BD_NOTICE_ACTION_LEFT,
  BD_NOTICE_ACTION_RIGHT,
  BD_NOTICE_ACTION_ACCEPT,
  BD_NOTICE_ACTION_CANCEL
} bd_notice_action;

typedef void (*bd_notice_confirm_fn)(void *user, bool confirmed);

typedef struct bd_notice bd_notice;

/* Requires a live renderer: fonts are loaded here. Returns NULL on failure. */
bd_notice *bd_notice_create(bd_gfx *gfx);
void bd_notice_destroy(bd_notice *notice);

/*
 * Queues a transient notice.  timeout_ms == 0 uses the default (3 s).
 * Returns false for an empty/oversized text or when the queue is full.
 */
bool bd_notice_show(bd_notice *notice, bd_notice_level level,
                    const char *text, uint32_t timeout_ms, uint64_t now_ms);

/*
 * Opens a modal confirmation.  Only one confirmation is active at a time; a
 * second call returns false until the first one is answered.  on_done may be
 * NULL.  timeout_ms == 0 means no automatic dismissal.
 */
bool bd_notice_confirm(bd_notice *notice, const char *title, const char *text,
                       uint32_t timeout_ms, bd_notice_confirm_fn on_done,
                       void *user, uint64_t now_ms);

bool bd_notice_confirm_open(const bd_notice *notice);
/* True while a toast is visible or queued, or a confirmation is open. */
bool bd_notice_active(const bd_notice *notice);
/* Absolute monotonic deadline that needs a redraw/update, or 0 for none. */
uint64_t bd_notice_next_deadline(const bd_notice *notice);

/* Advances timeouts and promotes the next queued toast. Returns true when the
 * visible content changed and the caller should present a frame. */
bool bd_notice_update(bd_notice *notice, uint64_t now_ms);
/* Draws the active toast and/or confirmation. Queues its own damage. */
void bd_notice_draw(bd_notice *notice, bd_gfx *gfx, uint64_t now_ms);

/* Routes one logical action. Returns true when the notice layer consumed it. */
bool bd_notice_input(bd_notice *notice, bd_notice_action action);

/* Drops every toast and closes an open confirmation as cancelled. */
void bd_notice_clear(bd_notice *notice);

#endif /* BRICKDESK_NOTICE_H */
