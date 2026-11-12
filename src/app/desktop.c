#include "brickdesk/desktop.h"

#include <stddef.h>
#include <stdlib.h>

struct bd_desktop {
  int focus;
};

enum {
  BD_TILE_COUNT = 6,
  BD_TILE_COLUMNS = 3,
  BD_MARGIN = 48,
  BD_GAP = 32,
  BD_HEADER_HEIGHT = 72,
  BD_FOOTER_HEIGHT = 72
};

static const bd_color COLOR_BACKGROUND = {14.0f / 255.0f, 22.0f / 255.0f, 32.0f / 255.0f, 1.0f};
static const bd_color COLOR_HEADER = {27.0f / 255.0f, 39.0f / 255.0f, 53.0f / 255.0f, 1.0f};
static const bd_color COLOR_FOOTER = {22.0f / 255.0f, 34.0f / 255.0f, 46.0f / 255.0f, 1.0f};
static const bd_color COLOR_CARD = {36.0f / 255.0f, 50.0f / 255.0f, 68.0f / 255.0f, 1.0f};
static const bd_color COLOR_ACCENT = {224.0f / 255.0f, 168.0f / 255.0f, 78.0f / 255.0f, 1.0f};
static const bd_color COLOR_FOCUS = {242.0f / 255.0f, 193.0f / 255.0f, 78.0f / 255.0f, 1.0f};
static const bd_color COLOR_TEXT = {150.0f / 255.0f, 166.0f / 255.0f, 184.0f / 255.0f, 1.0f};
static const bd_color COLOR_SUBTEXT = {94.0f / 255.0f, 111.0f / 255.0f, 130.0f / 255.0f, 1.0f};

static const bd_color TILE_COLORS[BD_TILE_COUNT] = {
    {60.0f / 255.0f, 110.0f / 255.0f, 143.0f / 255.0f, 1.0f},
    {143.0f / 255.0f, 93.0f / 255.0f, 60.0f / 255.0f, 1.0f},
    {79.0f / 255.0f, 143.0f / 255.0f, 93.0f / 255.0f, 1.0f},
    {122.0f / 255.0f, 79.0f / 255.0f, 143.0f / 255.0f, 1.0f},
    {143.0f / 255.0f, 122.0f / 255.0f, 60.0f / 255.0f, 1.0f},
    {60.0f / 255.0f, 143.0f / 255.0f, 138.0f / 255.0f, 1.0f},
};

bd_desktop *bd_desktop_create(void) {
  bd_desktop *desktop = (bd_desktop *)calloc(1, sizeof(*desktop));
  if (desktop != NULL) {
    desktop->focus = 0;
  }
  return desktop;
}

void bd_desktop_destroy(bd_desktop *desktop) {
  free(desktop);
}

int bd_desktop_focus(const bd_desktop *desktop) {
  return desktop != NULL ? desktop->focus : -1;
}

bool bd_desktop_handle_key(bd_desktop *desktop, bd_key key, bool pressed) {
  if (desktop == NULL || !pressed) {
    return false;
  }

  int next = desktop->focus;
  switch (key) {
    case BD_KEY_LEFT:
      next -= 1;
      break;
    case BD_KEY_RIGHT:
      next += 1;
      break;
    case BD_KEY_UP:
      next -= BD_TILE_COLUMNS;
      break;
    case BD_KEY_DOWN:
      next += BD_TILE_COLUMNS;
      break;
    default:
      return false;
  }

  if (next < 0) {
    next = 0;
  }
  if (next >= BD_TILE_COUNT) {
    next = BD_TILE_COUNT - 1;
  }
  if (next == desktop->focus) {
    return false;
  }

  desktop->focus = next;
  return true;
}

static void draw_focus_ring(bd_gfx *gfx, int x, int y, int width, int height) {
  const int thickness = 4;
  bd_gfx_fill_rect(gfx, bd_rect_make(x - thickness, y - thickness,
                                     width + thickness * 2, thickness),
                   COLOR_FOCUS);
  bd_gfx_fill_rect(gfx, bd_rect_make(x - thickness, y + height,
                                     width + thickness * 2, thickness),
                   COLOR_FOCUS);
  bd_gfx_fill_rect(gfx, bd_rect_make(x - thickness, y, thickness, height),
                   COLOR_FOCUS);
  bd_gfx_fill_rect(gfx, bd_rect_make(x + width, y, thickness, height),
                   COLOR_FOCUS);
}

void bd_desktop_draw(bd_desktop *desktop, bd_gfx *gfx) {
  if (desktop == NULL || gfx == NULL) {
    return;
  }

  const int width = bd_gfx_width(gfx);
  const int height = bd_gfx_height(gfx);
  if (width <= 0 || height <= 0) {
    return;
  }

  bd_gfx_clear(gfx, COLOR_BACKGROUND);

  /* Header: a wordmark block and a status block, no text yet (M2). */
  bd_gfx_fill_rect(gfx, bd_rect_make(0, 0, width, BD_HEADER_HEIGHT), COLOR_HEADER);
  bd_gfx_fill_rect(gfx, bd_rect_make(BD_MARGIN, 28, 96, 16), COLOR_ACCENT);
  bd_gfx_fill_rect(gfx, bd_rect_make(BD_MARGIN + 112, 32, 120, 8), COLOR_SUBTEXT);
  bd_gfx_fill_rect(gfx, bd_rect_make(width - BD_MARGIN - 120, 30, 120, 12),
                   COLOR_SUBTEXT);

  const int footer_y = height - BD_FOOTER_HEIGHT - 48;
  bd_gfx_fill_rect(gfx, bd_rect_make(BD_MARGIN, footer_y,
                                     width - BD_MARGIN * 2, BD_FOOTER_HEIGHT),
                   COLOR_FOOTER);
  bd_gfx_fill_rect(gfx, bd_rect_make(BD_MARGIN + 24, footer_y + 20, 220, 8),
                   COLOR_TEXT);
  bd_gfx_fill_rect(gfx, bd_rect_make(BD_MARGIN + 24, footer_y + 38, 140, 6),
                   COLOR_SUBTEXT);
  for (int dot = 0; dot < 3; ++dot) {
    bd_gfx_fill_rect(gfx,
                     bd_rect_make(width - BD_MARGIN - 24 - (dot + 1) * 24,
                                  footer_y + 30, 12, 12),
                     dot == 0 ? COLOR_ACCENT : COLOR_SUBTEXT);
  }

  const int grid_top = BD_HEADER_HEIGHT + 40;
  const int grid_bottom = footer_y - 32;
  const int tile_width = (width - BD_MARGIN * 2 - BD_GAP * (BD_TILE_COLUMNS - 1)) /
                         BD_TILE_COLUMNS;
  const int tile_height = (grid_bottom - grid_top - BD_GAP) / 2;
  if (tile_width <= 32 || tile_height <= 64) {
    return;
  }

  for (int index = 0; index < BD_TILE_COUNT; ++index) {
    const int column = index % BD_TILE_COLUMNS;
    const int row = index / BD_TILE_COLUMNS;
    const int x = BD_MARGIN + column * (tile_width + BD_GAP);
    const int y = grid_top + row * (tile_height + BD_GAP);

    bd_gfx_fill_rect(gfx, bd_rect_make(x, y, tile_width, tile_height), COLOR_CARD);
    bd_gfx_fill_rect(gfx, bd_rect_make(x + 16, y + 16, tile_width - 32,
                                       tile_height - 60),
                     TILE_COLORS[index]);
    bd_gfx_fill_rect(gfx,
                     bd_rect_make(x + 16, y + tile_height - 44,
                                  (tile_width - 32) * 62 / 100, 10),
                     COLOR_TEXT);
    bd_gfx_fill_rect(gfx,
                     bd_rect_make(x + 16, y + tile_height - 26,
                                  (tile_width - 32) * 38 / 100, 6),
                     COLOR_SUBTEXT);

    if (index == desktop->focus) {
      draw_focus_ring(gfx, x, y, tile_width, tile_height);
    }
  }
}
