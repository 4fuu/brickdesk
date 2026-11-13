#ifndef BRICKDESK_UI_H
#define BRICKDESK_UI_H

#include <stdbool.h>

#include "brickdesk/gfx.h"

/*
 * Shared UI font set (Noto Sans SC 500/700, Noto Serif SC 500, Inter 500).
 * Load once after the GL context; the wall and app chrome borrow the fonts.
 */
typedef struct bd_ui_fonts {
  bd_font *title;
  bd_font *body;
  bd_font *badge;
  bd_font *serif;
  bd_font *latin;
  /* Optional 20 px full Noto Sans SC loaded when it exists on disk. */
  bd_font *fallback_full;
} bd_ui_fonts;

bool bd_ui_fonts_load(bd_ui_fonts *fonts, bd_gfx *gfx);
void bd_ui_fonts_free(bd_ui_fonts *fonts);

/*
 * Shared status-bar icons.
 *
 * center_x/center_y are the centre of the complete icon, including the
 * battery terminal.  The widths below are the full advance the caller should
 * reserve, so two icons with the same gap look evenly spaced in every status
 * bar.  Callers position them at their own bar's optical centre instead of
 * forcing the home and settings bars to share an absolute y.
 *
 * Wi-Fi is a standard two-arc-plus-dot mark.  CONNECTED draws at the supplied
 * colour, DISCONNECTED dims that colour, and UNKNOWN dims it and adds a slash.
 * The icon is never hidden to express a state.
 *
 * A negative battery percent is unknown: the shell, terminal and slash are
 * drawn without a fill.  A known percentage fills the body proportionally;
 * the fill colour is caller-supplied so each bar keeps its own emphasis.
 */
#define BD_UI_WIFI_ICON_W 20
#define BD_UI_WIFI_ICON_H 16
#define BD_UI_BATTERY_ICON_W 29
#define BD_UI_BATTERY_ICON_H 13

typedef enum bd_ui_wifi_state {
  BD_UI_WIFI_CONNECTED = 0,
  BD_UI_WIFI_DISCONNECTED,
  BD_UI_WIFI_UNKNOWN
} bd_ui_wifi_state;

void bd_ui_draw_wifi(bd_gfx *gfx, int center_x, int center_y,
                     bd_ui_wifi_state state, bd_color color);
void bd_ui_draw_battery(bd_gfx *gfx, int center_x, int center_y, int percent,
                        bd_color outline, bd_color fill);

/* Shared switch geometry. x/y is the track's top-left; callers own colours,
 * labels and row focus so a switch does not look selected merely by being on. */
#define BD_UI_TOGGLE_W 44
#define BD_UI_TOGGLE_H 24
void bd_ui_draw_toggle(bd_gfx *gfx, int x, int y, bool on,
                       bd_color track, bd_color knob);

#endif /* BRICKDESK_UI_H */
