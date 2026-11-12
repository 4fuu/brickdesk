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

#endif /* BRICKDESK_UI_H */
