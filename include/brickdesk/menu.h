#ifndef BRICKDESK_MENU_H
#define BRICKDESK_MENU_H

/*
 * The generic in-app menu (M1-09).
 *
 * The overlay is delivered as libbrickdesk-menu.so through LD_PRELOAD. It
 * hooks the application's frame present calls, draws a GLES2 menu over the
 * application's back buffer, and blocks the rendering thread while the menu
 * is open. See docs/menu-shim.md for the mode-A/mode-C decision table.
 *
 * Brightness and volume deliberately sit behind bd_menu_system_ops: M1 ships
 * an in-process stub because the real backends (backlight and ALSA) are M4.
 * An embedded application or a later backend can install a different table
 * with bd_menu_set_system_ops(); the shim reads it on every menu open, so a
 * backend can be registered before or after the library is preloaded.
 */

#include <stdbool.h>

/* 0..100 values, or a negative value when the backend cannot report one. */
typedef struct bd_menu_system_ops {
  void *user;
  int (*brightness_get)(void *user);
  bool (*brightness_set)(void *user, int percent);
  int (*volume_get)(void *user);
  bool (*volume_set)(void *user, int percent);
} bd_menu_system_ops;

/*
 * Fills `out` with the in-memory stub: values start at the configuration's
 * display.brightness and 50 for volume, and writes only update the stub.
 */
void bd_menu_system_ops_stub(bd_menu_system_ops *out);

/* Installs a backend; NULL restores the stub. The table is copied by value. */
void bd_menu_set_system_ops(const bd_menu_system_ops *ops);

/* True while the overlay is visible. Safe to call from any thread. */
bool bd_menu_visible(void);

/* Number of completed menu opens in this process. Used by host tests. */
unsigned bd_menu_open_count(void);

#endif /* BRICKDESK_MENU_H */
