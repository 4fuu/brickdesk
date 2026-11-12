#ifndef BRICKDESK_WALL_H
#define BRICKDESK_WALL_H

#include <stdbool.h>
#include <stddef.h>

#include "brickdesk/gfx.h"
#include "brickdesk/theme.h"
#include "brickdesk/ui.h"

/*
 * Infinite cover wall. Items live in wall coordinates and may span several
 * square grid cells. The wall indexes them in 2D buckets so drawing touches
 * only buckets intersecting the viewport plus a prefetch ring, never the
 * whole manifest. Image requests are driven by that same set.
 */
typedef struct bd_wall bd_wall;

typedef struct bd_wall_item {
  bd_rect rect;             /* wall coordinates */
  const char *title;
  const char *subtitle;
  const char *badge;        /* small top-left status/platform label */
  const char *image_path;   /* NULL keeps the placeholder colour */
  bd_color placeholder;
  float progress;           /* < 0 hides the progress bar */
} bd_wall_item;

typedef struct bd_wall_style {
  const bd_ui_fonts *fonts;
  bd_accent_palette accent;
  int radius;               /* cover corner radius, default 6 */
  int prefetch_pixels;      /* ring around the viewport, default one cell */
} bd_wall_style;

typedef struct bd_wall_stats {
  int total_items;
  int visited_items;
  int visible_items;
  int drawn_items;
  int buckets_visited;
  int focus_index;
  int pending_images;
  int resident_textures;
  size_t resident_texture_bytes;
} bd_wall_stats;

bd_wall *bd_wall_create(bd_gfx *gfx, const bd_wall_style *style);
void bd_wall_destroy(bd_wall *wall);
/* Copies item strings; returns false on allocation failure. */
bool bd_wall_set_items(bd_wall *wall, const bd_wall_item *items, int count);
void bd_wall_set_style(bd_wall *wall, const bd_wall_style *style);
void bd_wall_set_focus(bd_wall *wall, int index);
bool bd_wall_focus_move(bd_wall *wall, int dx, int dy);
int bd_wall_focus(const bd_wall *wall);
/*
 * begin_frame queries the viewport + prefetch ring and queues per-tile damage
 * (focus/image changes, pans). Call it before drawing the background so all
 * damage is known for the first quad of the frame. draw then renders the
 * prepared visible list; it calls begin_frame itself when not prepared.
 */
void bd_wall_begin_frame(bd_wall *wall, bd_gfx *gfx, bd_rect viewport,
                         int origin_x, int origin_y);
void bd_wall_draw(bd_wall *wall, bd_gfx *gfx);
bool bd_wall_has_pending_images(const bd_wall *wall);
bd_wall_stats bd_wall_get_stats(const bd_wall *wall);

#endif /* BRICKDESK_WALL_H */
