#include "brickdesk/ui.h"

#include <stdio.h>
#include <string.h>

#ifndef BRICKDESK_ASSET_DIR
#define BRICKDESK_ASSET_DIR "assets"
#endif

static bool build_font_path(char *path, size_t path_size, const char *name) {
  int written = snprintf(path, path_size, "%s/fonts/%s", BRICKDESK_ASSET_DIR, name);
  return written > 0 && (size_t)written < path_size;
}

static bd_font *load_font(bd_gfx *gfx, const char *name, int pixel_size) {
  char path[1024];
  if (!build_font_path(path, sizeof(path), name)) {
    return NULL;
  }
  return bd_font_create(gfx, path, pixel_size);
}

static bool file_exists(const char *path) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return false;
  }
  fclose(file);
  return true;
}

static bd_font *load_optional_full_fallback(bd_gfx *gfx) {
  static const char *const CANDIDATES[] = {
      "NotoSansSC-Full.ttf", "NotoSansSC-Full.otf",
      "NotoSansCJKsc-Regular.otf", "NotoSansSC-Regular.otf",
  };
  for (size_t i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); ++i) {
    char path[1024];
    if (!build_font_path(path, sizeof(path), CANDIDATES[i]) || !file_exists(path)) {
      continue;
    }
    bd_font *font = bd_font_create(gfx, path, 20);
    if (font != NULL) {
      return font;
    }
  }
  return NULL;
}

bool bd_ui_fonts_load(bd_ui_fonts *fonts, bd_gfx *gfx) {
  if (fonts == NULL || gfx == NULL) {
    return false;
  }
  memset(fonts, 0, sizeof(*fonts));
  fonts->title = load_font(gfx, "NotoSansSC-700-subset.ttf", 20);
  fonts->body = load_font(gfx, "NotoSansSC-500-subset.ttf", 18);
  fonts->badge = load_font(gfx, "NotoSansSC-500-subset.ttf", 13);
  fonts->serif = load_font(gfx, "NotoSerifSC-500-subset.ttf", 34);
  fonts->latin = load_font(gfx, "Inter-500-subset.ttf", 16);
  if (fonts->title == NULL || fonts->body == NULL || fonts->badge == NULL ||
      fonts->serif == NULL || fonts->latin == NULL) {
    bd_ui_fonts_free(fonts);
    return false;
  }
  fonts->fallback_full = load_optional_full_fallback(gfx);
  bd_font_set_fallback(fonts->latin, fonts->body);
  bd_font_set_fallback(fonts->body, fonts->fallback_full);
  bd_font_set_fallback(fonts->title, fonts->fallback_full);
  bd_font_set_fallback(fonts->badge, fonts->fallback_full);
  bd_font_set_fallback(fonts->serif, fonts->fallback_full);
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
  bd_font_destroy(fonts->fallback_full);
  fonts->title = NULL;
  fonts->body = NULL;
  fonts->badge = NULL;
  fonts->serif = NULL;
  fonts->latin = NULL;
  fonts->fallback_full = NULL;
}
