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

static bd_color dim_color(bd_color color, float factor) {
  color.a *= factor;
  return color;
}

static void draw_line(bd_gfx *gfx, int x0, int y0, int x1, int y1,
                      int thickness, bd_color color) {
  int dx = x1 - x0;
  int dy = y1 - y0;
  int steps = (dx >= 0 ? dx : -dx) > (dy >= 0 ? dy : -dy)
                  ? (dx >= 0 ? dx : -dx)
                  : (dy >= 0 ? dy : -dy);
  if (steps <= 0) {
    bd_gfx_fill_rect(gfx, bd_rect_make(x0, y0, thickness, thickness), color);
    return;
  }
  for (int step = 0; step <= steps; ++step) {
    int x = x0 + dx * step / steps;
    int y = y0 + dy * step / steps;
    bd_gfx_fill_rect(gfx, bd_rect_make(x, y, thickness, thickness), color);
  }
}

void bd_ui_draw_wifi(bd_gfx *gfx, int center_x, int center_y,
                     bd_ui_wifi_state state, bd_color color) {
  if (gfx == NULL) {
    return;
  }
  bd_color ink = state == BD_UI_WIFI_CONNECTED ? color
                                               : dim_color(color, 0.55f);

  /*
   * Two upper-half rings meet the dot with a two-pixel gap.  The clip ends at
   * center_y + 2 so the bowl of each ring is cut away instead of drawing a
   * complete circle.  The outer ring spans the full 20 px width; the dot and
   * the 16 px height place the optical centre on center_y.
   */
  bd_gfx_push_clip(gfx, bd_rect_make(center_x - 11, center_y - 9, 22, 12));
  bd_gfx_stroke_round_rect(
      gfx, bd_rect_make(center_x - 10, center_y - 8, 20, 20), 10, 2, ink);
  bd_gfx_pop_clip(gfx);
  bd_gfx_push_clip(gfx, bd_rect_make(center_x - 7, center_y - 5, 14, 8));
  bd_gfx_stroke_round_rect(
      gfx, bd_rect_make(center_x - 6, center_y - 4, 12, 12), 6, 2, ink);
  bd_gfx_pop_clip(gfx);
  bd_gfx_fill_round_rect(gfx,
                         bd_rect_make(center_x - 2, center_y + 4, 4, 4), 2,
                         ink);

  if (state == BD_UI_WIFI_UNKNOWN) {
    draw_line(gfx, center_x + 7, center_y - 7, center_x - 7, center_y + 6, 2,
              ink);
  }
}

void bd_ui_draw_battery(bd_gfx *gfx, int center_x, int center_y, int percent,
                        bd_color outline, bd_color fill) {
  if (gfx == NULL) {
    return;
  }
  int left = center_x - (BD_UI_BATTERY_ICON_W - 1) / 2;
  int top = center_y - BD_UI_BATTERY_ICON_H / 2;
  bd_rect body = bd_rect_make(left, top, 26, BD_UI_BATTERY_ICON_H);
  bd_gfx_stroke_round_rect(gfx, body, 2, 1, outline);
  /* The terminal is part of the 29 px advance, not extra overhang. */
  bd_gfx_fill_round_rect(
      gfx, bd_rect_make(left + BD_UI_BATTERY_ICON_W - 2, center_y - 2, 2, 5), 1,
      outline);

  if (percent < 0) {
    draw_line(gfx, left + 4, center_y + 4, left + 24, center_y - 4, 1,
              outline);
    return;
  }
  if (percent > 100) {
    percent = 100;
  }
  int inner = 22 * percent / 100;
  if (inner > 0) {
    bd_gfx_fill_round_rect(
        gfx, bd_rect_make(left + 2, top + 2, inner, BD_UI_BATTERY_ICON_H - 4),
        1, fill);
  }
}

