#include "brickdesk/ui.h"

#include <stdio.h>

#ifndef BRICKDESK_ASSET_DIR
#define BRICKDESK_ASSET_DIR "assets"
#endif

static bd_font *load_font(bd_gfx *gfx, const char *name, int pixel_size) {
  char path[1024];
  int written = snprintf(path, sizeof(path), "%s/fonts/%s", BRICKDESK_ASSET_DIR, name);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    return NULL;
  }
  return bd_font_create(gfx, path, pixel_size);
}

bool bd_ui_fonts_load(bd_ui_fonts *fonts, bd_gfx *gfx) {
  if (fonts == NULL || gfx == NULL) {
    return false;
  }
  fonts->title = load_font(gfx, "NotoSansSC-700-subset.ttf", 20);
  fonts->body = load_font(gfx, "NotoSansSC-500-subset.ttf", 16);
  fonts->badge = load_font(gfx, "NotoSansSC-500-subset.ttf", 12);
  fonts->serif = load_font(gfx, "NotoSerifSC-500-subset.ttf", 34);
  fonts->latin = load_font(gfx, "Inter-500-subset.ttf", 16);
  if (fonts->title == NULL || fonts->body == NULL || fonts->badge == NULL ||
      fonts->serif == NULL || fonts->latin == NULL) {
    bd_ui_fonts_free(fonts);
    return false;
  }
  bd_font_set_fallback(fonts->latin, fonts->body);
  return true;
}

void bd_ui_fonts_free(bd_ui_fonts *fonts) {
  if (fonts == NULL) {
    return;
  }
  bd_font_destroy(fonts->title);
  bd_font_destroy(fonts->body);
  bd_font_destroy(fonts->badge);
  bd_font_destroy(fonts->serif);
  bd_font_destroy(fonts->latin);
  fonts->title = NULL;
  fonts->body = NULL;
  fonts->badge = NULL;
  fonts->serif = NULL;
  fonts->latin = NULL;
}
