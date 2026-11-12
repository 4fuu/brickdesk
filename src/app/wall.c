#include "brickdesk/wall.h"

#include <stdlib.h>
#include <string.h>

#define BD_WALL_BUCKET_SIZE 256
#define BD_WALL_BUCKET_HASH 2048
#define BD_WALL_MAX_QUERY 1024

typedef struct bd_wall_bucket {
  int bx;
  int by;
  int *indices;
  int count;
  int capacity;
  struct bd_wall_bucket *next;
} bd_wall_bucket;

typedef struct bd_wall_entry {
  bd_wall_item item;
  char *title;
  char *subtitle;
  char *badge;
  char *image;
  uint32_t visit_stamp;
  bd_texture *texture;
} bd_wall_entry;

struct bd_wall {
  bd_gfx *gfx;
  bd_wall_style style;
  bd_wall_entry *entries;
  int count;
  bd_wall_bucket *buckets[BD_WALL_BUCKET_HASH];
  uint32_t stamp;
  int prefetch[BD_WALL_MAX_QUERY];
  int prefetch_count;
  int visible[BD_WALL_MAX_QUERY];
  int visible_count;
  int focus;
  int previous_focus;
  bool focus_dirty;
  bool needs_full;
  bool prepared;
  bd_rect draw_viewport;
  int draw_origin_x;
  int draw_origin_y;
  bool have_last;
  bd_rect last_viewport;
  int last_origin_x;
  int last_origin_y;
  bd_wall_stats stats;
};

static char *duplicate_string(const char *text) {
  if (text == NULL) {
    return NULL;
  }
  size_t length = strlen(text);
  char *copy = (char *)malloc(length + 1);
  if (copy != NULL) {
    memcpy(copy, text, length + 1);
  }
  return copy;
}

static unsigned int bucket_hash(int bx, int by) {
  unsigned int value = (unsigned int)(bx * 73856093) ^ (unsigned int)(by * 19349663);
  return value & (BD_WALL_BUCKET_HASH - 1);
}

static bd_wall_bucket *find_bucket(bd_wall *wall, int bx, int by) {
  unsigned int hash = bucket_hash(bx, by);
  for (bd_wall_bucket *bucket = wall->buckets[hash]; bucket != NULL;
       bucket = bucket->next) {
    if (bucket->bx == bx && bucket->by == by) {
      return bucket;
    }
  }
  return NULL;
}

static bd_wall_bucket *get_bucket(bd_wall *wall, int bx, int by) {
  bd_wall_bucket *bucket = find_bucket(wall, bx, by);
  if (bucket != NULL) {
    return bucket;
  }
  bucket = (bd_wall_bucket *)calloc(1, sizeof(*bucket));
  if (bucket == NULL) {
    return NULL;
  }
  bucket->bx = bx;
  bucket->by = by;
  unsigned int hash = bucket_hash(bx, by);
  bucket->next = wall->buckets[hash];
  wall->buckets[hash] = bucket;
  return bucket;
}

static bool bucket_add(bd_wall_bucket *bucket, int index) {
  if (bucket->count == bucket->capacity) {
    int capacity = bucket->capacity == 0 ? 8 : bucket->capacity * 2;
    int *indices = (int *)realloc(bucket->indices, (size_t)capacity * sizeof(int));
    if (indices == NULL) {
      return false;
    }
    bucket->indices = indices;
    bucket->capacity = capacity;
  }
  bucket->indices[bucket->count++] = index;
  return true;
}

static void clear_buckets(bd_wall *wall) {
  for (int i = 0; i < BD_WALL_BUCKET_HASH; ++i) {
    bd_wall_bucket *bucket = wall->buckets[i];
    while (bucket != NULL) {
      bd_wall_bucket *next = bucket->next;
      free(bucket->indices);
      free(bucket);
      bucket = next;
    }
    wall->buckets[i] = NULL;
  }
}

static void free_entries(bd_wall *wall) {
  if (wall->entries == NULL) {
    return;
  }
  for (int i = 0; i < wall->count; ++i) {
    free(wall->entries[i].title);
    free(wall->entries[i].subtitle);
    free(wall->entries[i].badge);
    free(wall->entries[i].image);
  }
  free(wall->entries);
  wall->entries = NULL;
  wall->count = 0;
}

static int floor_div(int value, int divisor) {
  int quotient = value / divisor;
  if (value < 0 && value % divisor != 0) {
    --quotient;
  }
  return quotient;
}

static bool index_item(bd_wall *wall, int index) {
  const bd_rect rect = wall->entries[index].item.rect;
  int bx0 = floor_div(rect.x, BD_WALL_BUCKET_SIZE);
  int by0 = floor_div(rect.y, BD_WALL_BUCKET_SIZE);
  int bx1 = floor_div(rect.x + rect.w - 1, BD_WALL_BUCKET_SIZE);
  int by1 = floor_div(rect.y + rect.h - 1, BD_WALL_BUCKET_SIZE);
  for (int by = by0; by <= by1; ++by) {
    for (int bx = bx0; bx <= bx1; ++bx) {
      bd_wall_bucket *bucket = get_bucket(wall, bx, by);
      if (bucket == NULL || !bucket_add(bucket, index)) {
        return false;
      }
    }
  }
  return true;
}

bd_wall *bd_wall_create(bd_gfx *gfx, const bd_wall_style *style) {
  if (gfx == NULL || style == NULL || style->fonts == NULL) {
    return NULL;
  }
  bd_wall *wall = (bd_wall *)calloc(1, sizeof(*wall));
  if (wall == NULL) {
    return NULL;
  }
  wall->gfx = gfx;
  wall->style = *style;
  if (wall->style.radius <= 0) {
    wall->style.radius = 6;
  }
  if (wall->style.prefetch_pixels <= 0) {
    wall->style.prefetch_pixels = BD_WALL_BUCKET_SIZE;
  }
  wall->focus = -1;
  wall->previous_focus = -1;
  return wall;
}

void bd_wall_destroy(bd_wall *wall) {
  if (wall == NULL) {
    return;
  }
  free_entries(wall);
  clear_buckets(wall);
  free(wall);
}

bool bd_wall_set_items(bd_wall *wall, const bd_wall_item *items, int count) {
  if (wall == NULL || count < 0 || (count > 0 && items == NULL)) {
    return false;
  }
  free_entries(wall);
  clear_buckets(wall);

  if (count > 0) {
    wall->entries = (bd_wall_entry *)calloc((size_t)count, sizeof(bd_wall_entry));
    if (wall->entries == NULL) {
      return false;
    }
    wall->count = count;
    for (int i = 0; i < count; ++i) {
      bd_wall_entry *entry = &wall->entries[i];
      entry->item = items[i];
      entry->title = duplicate_string(items[i].title);
      entry->subtitle = duplicate_string(items[i].subtitle);
      entry->badge = duplicate_string(items[i].badge);
      entry->image = duplicate_string(items[i].image_path);
      if ((items[i].title != NULL && entry->title == NULL) ||
          (items[i].subtitle != NULL && entry->subtitle == NULL) ||
          (items[i].badge != NULL && entry->badge == NULL) ||
          (items[i].image_path != NULL && entry->image == NULL)) {
        free_entries(wall);
        return false;
      }
      entry->item.title = entry->title;
      entry->item.subtitle = entry->subtitle;
      entry->item.badge = entry->badge;
      entry->item.image_path = entry->image;
      if (!index_item(wall, i)) {
        free_entries(wall);
        clear_buckets(wall);
        return false;
      }
    }
  }

  wall->focus = count > 0 ? 0 : -1;
  wall->previous_focus = -1;
  wall->focus_dirty = true;
  wall->needs_full = true;
  wall->have_last = false;
  wall->prefetch_count = 0;
  wall->visible_count = 0;
  return true;
}

void bd_wall_set_style(bd_wall *wall, const bd_wall_style *style) {
  if (wall == NULL || style == NULL || style->fonts == NULL) {
    return;
  }
  wall->style = *style;
  if (wall->style.radius <= 0) {
    wall->style.radius = 6;
  }
  if (wall->style.prefetch_pixels <= 0) {
    wall->style.prefetch_pixels = BD_WALL_BUCKET_SIZE;
  }
  wall->needs_full = true;
}

void bd_wall_set_focus(bd_wall *wall, int index) {
  if (wall == NULL || wall->count == 0) {
    return;
  }
  if (index < 0) {
    index = 0;
  }
  if (index >= wall->count) {
    index = wall->count - 1;
  }
  if (index != wall->focus) {
    wall->focus = index;
    wall->focus_dirty = true;
  }
}

int bd_wall_focus(const bd_wall *wall) {
  return wall != NULL ? wall->focus : -1;
}

static int query_wall(bd_wall *wall, bd_rect rect, int *out, int capacity,
                      int *bucket_count) {
  if (wall == NULL || out == NULL || capacity <= 0 || rect.w <= 0 || rect.h <= 0) {
    return 0;
  }
  ++wall->stamp;
  if (wall->stamp == 0) {
    for (int i = 0; i < wall->count; ++i) {
      wall->entries[i].visit_stamp = 0;
    }
    wall->stamp = 1;
  }

  int count = 0;
  int buckets = 0;
  int bx0 = floor_div(rect.x, BD_WALL_BUCKET_SIZE);
  int by0 = floor_div(rect.y, BD_WALL_BUCKET_SIZE);
  int bx1 = floor_div(rect.x + rect.w - 1, BD_WALL_BUCKET_SIZE);
  int by1 = floor_div(rect.y + rect.h - 1, BD_WALL_BUCKET_SIZE);
  for (int by = by0; by <= by1 && count < capacity; ++by) {
    for (int bx = bx0; bx <= bx1 && count < capacity; ++bx) {
      bd_wall_bucket *bucket = find_bucket(wall, bx, by);
      if (bucket == NULL) {
        continue;
      }
      ++buckets;
      for (int i = 0; i < bucket->count && count < capacity; ++i) {
        int index = bucket->indices[i];
        bd_wall_entry *entry = &wall->entries[index];
        if (entry->visit_stamp == wall->stamp) {
          continue;
        }
        entry->visit_stamp = wall->stamp;
        if (bd_rect_intersect(entry->item.rect, rect).w > 0) {
          out[count++] = index;
        }
      }
    }
  }
  if (bucket_count != NULL) {
    *bucket_count = buckets;
  }
  return count;
}

static int item_distance(const bd_wall *wall, const bd_wall_entry *entry) {
  int focus_x;
  int focus_y;
  if (wall->focus >= 0 && wall->focus < wall->count) {
    const bd_rect focus = wall->entries[wall->focus].item.rect;
    focus_x = focus.x + focus.w / 2;
    focus_y = focus.y + focus.h / 2;
  } else {
    focus_x = 0;
    focus_y = 0;
  }
  int x = entry->item.rect.x + entry->item.rect.w / 2;
  int y = entry->item.rect.y + entry->item.rect.h / 2;
  return (abs(x - focus_x) + abs(y - focus_y)) / 32;
}

static bool item_screen_rect(const bd_wall_entry *entry, int origin_x,
                             int origin_y, bd_rect *out) {
  if (entry == NULL) {
    return false;
  }
  *out = bd_rect_make(entry->item.rect.x + origin_x,
                      entry->item.rect.y + origin_y, entry->item.rect.w,
                      entry->item.rect.h);
  return out->w > 0 && out->h > 0;
}

static bd_color wall_veil(const bd_wall_style *style) {
  bd_color ink = bd_color_rgb(18, 16, 13);
  bd_color tinted = bd_color_mix(ink, style->accent.accent, 0.06f);
  tinted.a = 0.30f;
  return tinted;
}

static void draw_cover(const bd_wall *wall, bd_gfx *gfx, const bd_wall_entry *entry,
                       bd_rect screen) {
  if (entry->texture != NULL) {
    bd_gfx_draw_texture_round(gfx, entry->texture, screen,
                              bd_color_rgba(255, 255, 255, 255),
                              wall->style.radius);
    return;
  }
  bd_gfx_fill_round_rect(gfx, screen, wall->style.radius, entry->item.placeholder);
  bd_rect circle = bd_rect_make(screen.x + screen.w / 4, screen.y + screen.h / 4,
                                screen.w / 2, screen.h / 2);
  bd_color light = bd_color_mix(entry->item.placeholder,
                                bd_color_rgba(255, 248, 242, 255), 0.18f);
  light.a = 0.55f;
  bd_gfx_fill_round_rect(gfx, circle, circle.w / 2, light);
}

static void draw_shadow(const bd_wall *wall, bd_gfx *gfx, bd_rect screen) {
  int height = screen.h / 3;
  if (height > 48) {
    height = 48;
  }
  if (height <= 0) {
    return;
  }
  bd_rect shadow = bd_rect_make(screen.x, screen.y + screen.h - height, screen.w,
                                height);
  bd_gfx_fill_gradient(gfx, shadow, wall->style.radius,
                       bd_color_rgba(0, 0, 0, 0),
                       bd_color_rgba(0, 0, 0, 78), BD_GRADIENT_VERTICAL);
}

static void draw_badge_pill(const bd_wall *wall, bd_gfx *gfx,
                            const bd_wall_entry *entry, bd_rect screen) {
  if (entry->item.badge == NULL || entry->item.badge[0] == '\0') {
    return;
  }
  int width = bd_font_measure_text(wall->style.fonts->badge, entry->item.badge) + 12;
  if (width > screen.w - 16) {
    width = screen.w - 16;
  }
  bd_rect badge = bd_rect_make(screen.x + 8, screen.y + 8, width, 20);
  bd_gfx_fill_round_rect(gfx, badge, 4, bd_color_rgba(0, 0, 0, 150));
}

static void draw_badge_text(const bd_wall *wall, bd_gfx *gfx,
                            const bd_wall_entry *entry, bd_rect screen) {
  if (entry->item.badge == NULL || entry->item.badge[0] == '\0') {
    return;
  }
  int width = bd_font_measure_text(wall->style.fonts->badge, entry->item.badge) + 12;
  if (width > screen.w - 16) {
    width = screen.w - 16;
  }
  bd_rect badge = bd_rect_make(screen.x + 8, screen.y + 8, width, 20);
  int baseline = badge.y + 15;
  bd_gfx_draw_text_ellipsized(gfx, wall->style.fonts->badge, badge.x + 6, baseline,
                              width - 12, bd_color_rgba(255, 248, 242, 220),
                              entry->item.badge);
}

static void text_layout(const bd_wall *wall, const bd_wall_entry *entry,
                        bd_rect screen, int *title_baseline, int *subtitle_baseline,
                        const bd_font **title_font) {
  bool hero = screen.w >= 280 && screen.h >= 280;
  bool progress = entry->item.progress >= 0.0f;
  *subtitle_baseline = screen.y + screen.h - (progress ? 24 : 16);
  *title_baseline = *subtitle_baseline - (hero ? 36 : 22);
  *title_font = hero ? wall->style.fonts->serif : wall->style.fonts->title;
}

static void draw_title(const bd_wall *wall, bd_gfx *gfx, const bd_wall_entry *entry,
                       bd_rect screen) {
  int title_baseline = 0;
  int subtitle_baseline = 0;
  const bd_font *font = NULL;
  text_layout(wall, entry, screen, &title_baseline, &subtitle_baseline, &font);
  if (entry->item.title != NULL) {
    bd_gfx_draw_text_ellipsized(gfx, font, screen.x + 8, title_baseline,
                                screen.w - 16, bd_color_rgba(255, 250, 245, 255),
                                entry->item.title);
  }
}

static void draw_subtitle(const bd_wall *wall, bd_gfx *gfx,
                          const bd_wall_entry *entry, bd_rect screen) {
  int title_baseline = 0;
  int subtitle_baseline = 0;
  const bd_font *font = NULL;
  text_layout(wall, entry, screen, &title_baseline, &subtitle_baseline, &font);
  (void)title_baseline;
  (void)font;
  if (entry->item.subtitle != NULL) {
    bd_gfx_draw_text_ellipsized(gfx, wall->style.fonts->body, screen.x + 8,
                                subtitle_baseline, screen.w - 16,
                                bd_color_rgba(255, 248, 242, 185),
                                entry->item.subtitle);
  }
}

static void draw_progress(const bd_wall *wall, bd_gfx *gfx,
                          const bd_wall_entry *entry, bd_rect screen) {
  if (entry->item.progress < 0.0f) {
    return;
  }
  float progress = entry->item.progress;
  if (progress > 1.0f) {
    progress = 1.0f;
  }
  int x = screen.x + 8;
  int width = screen.w - 16;
  if (width <= 8) {
    return;
  }
  bd_rect track = bd_rect_make(x, screen.y + screen.h - 7, width, 3);
  bd_gfx_fill_round_rect(gfx, track, 2, bd_color_rgba(255, 255, 255, 70));
  int fill = (int)((float)width * progress);
  if (fill > 0) {
    bd_rect value = bd_rect_make(x, track.y, fill, 3);
    bd_gfx_fill_round_rect(gfx, value, 2, wall->style.accent.accent);
  }
}

static void draw_focus_content(const bd_wall *wall, bd_gfx *gfx,
                               const bd_wall_entry *entry, bd_rect screen) {
  draw_cover(wall, gfx, entry, screen);
  draw_shadow(wall, gfx, screen);
  draw_badge_pill(wall, gfx, entry, screen);
  draw_badge_text(wall, gfx, entry, screen);
  draw_title(wall, gfx, entry, screen);
  draw_subtitle(wall, gfx, entry, screen);
  draw_progress(wall, gfx, entry, screen);
  bd_gfx_stroke_round_rect(gfx, screen, wall->style.radius, 2,
                           wall->style.accent.focus_ring);
}

static void queue_screen_damage(bd_gfx *gfx, bd_rect rect, bd_rect viewport) {
  bd_rect clipped = bd_rect_intersect(rect, viewport);
  if (clipped.w <= 0 || clipped.h <= 0) {
    return;
  }
  clipped.x -= 4;
  clipped.y -= 4;
  clipped.w += 8;
  clipped.h += 8;
  bd_gfx_damage_rect(gfx, clipped);
}

void bd_wall_begin_frame(bd_wall *wall, bd_gfx *gfx, bd_rect viewport,
                         int origin_x, int origin_y) {
  if (wall == NULL || gfx == NULL) {
    return;
  }
  wall->prepared = true;
  wall->draw_viewport = viewport;
  wall->draw_origin_x = origin_x;
  wall->draw_origin_y = origin_y;
  wall->stats.total_items = wall->count;
  wall->stats.visited_items = 0;
  wall->stats.visible_items = 0;
  wall->stats.drawn_items = 0;
  wall->stats.buckets_visited = 0;
  wall->stats.focus_index = wall->focus;
  if (wall->count == 0) {
    return;
  }

  bool moved = !wall->have_last || wall->last_origin_x != origin_x ||
               wall->last_origin_y != origin_y ||
               wall->last_viewport.x != viewport.x ||
               wall->last_viewport.y != viewport.y ||
               wall->last_viewport.w != viewport.w ||
               wall->last_viewport.h != viewport.h;
  if (wall->needs_full || moved) {
    bd_gfx_damage_rect(gfx, viewport);
    wall->needs_full = false;
  }

  bd_rect prefetch = bd_rect_make(viewport.x - origin_x - wall->style.prefetch_pixels,
                                  viewport.y - origin_y - wall->style.prefetch_pixels,
                                  viewport.w + wall->style.prefetch_pixels * 2,
                                  viewport.h + wall->style.prefetch_pixels * 2);
  int bucket_count = 0;
  wall->prefetch_count =
      query_wall(wall, prefetch, wall->prefetch, BD_WALL_MAX_QUERY, &bucket_count);

  wall->visible_count = 0;
  for (int i = 0; i < wall->prefetch_count; ++i) {
    bd_wall_entry *entry = &wall->entries[wall->prefetch[i]];
    bd_rect screen = bd_rect_make(0, 0, 0, 0);
    item_screen_rect(entry, origin_x, origin_y, &screen);
    if (bd_rect_intersect(screen, viewport).w > 0 &&
        bd_rect_intersect(screen, viewport).h > 0) {
      wall->visible[wall->visible_count++] = wall->prefetch[i];
    }
  }

  bd_gfx_image_begin_set(gfx);
  for (int i = 0; i < wall->prefetch_count; ++i) {
    int index = wall->prefetch[i];
    bd_wall_entry *entry = &wall->entries[index];
    if (entry->image == NULL || entry->image[0] == '\0') {
      entry->texture = NULL;
      continue;
    }
    int target_width = entry->item.rect.w < 384 ? entry->item.rect.w : 384;
    int target_height = entry->item.rect.h < 384 ? entry->item.rect.h : 384;
    if (target_width < 32) {
      target_width = 32;
    }
    if (target_height < 32) {
      target_height = 32;
    }
    bd_texture *texture = NULL;
    bd_image_state state = bd_gfx_image_request(
        gfx, entry->image, target_width, target_height, item_distance(wall, entry),
        &texture);
    bd_texture *ready = state == BD_IMAGE_READY ? texture : NULL;
    bd_rect screen = bd_rect_make(0, 0, 0, 0);
    item_screen_rect(entry, origin_x, origin_y, &screen);
    bd_rect visible_rect = bd_rect_intersect(screen, viewport);
    bool visible = visible_rect.w > 0 && visible_rect.h > 0;
    if (visible && ready != entry->texture) {
      queue_screen_damage(gfx, screen, viewport);
    }
    entry->texture = ready;
  }
  bd_gfx_image_end_set(gfx);

  if (wall->focus_dirty) {
    if (wall->previous_focus >= 0 && wall->previous_focus < wall->count) {
      bd_rect screen = bd_rect_make(0, 0, 0, 0);
      if (item_screen_rect(&wall->entries[wall->previous_focus], origin_x,
                           origin_y, &screen)) {
        queue_screen_damage(gfx, screen, viewport);
      }
    }
    if (wall->focus >= 0 && wall->focus < wall->count) {
      bd_rect screen = bd_rect_make(0, 0, 0, 0);
      if (item_screen_rect(&wall->entries[wall->focus], origin_x, origin_y,
                           &screen)) {
        queue_screen_damage(gfx, screen, viewport);
      }
    }
    wall->previous_focus = wall->focus;
    wall->focus_dirty = false;
  }

  wall->last_viewport = viewport;
  wall->last_origin_x = origin_x;
  wall->last_origin_y = origin_y;
  wall->have_last = true;

  wall->stats.visited_items = wall->prefetch_count;
  wall->stats.visible_items = wall->visible_count;
  wall->stats.drawn_items = wall->visible_count;
  wall->stats.buckets_visited = bucket_count;
  wall->stats.focus_index = wall->focus;
}

void bd_wall_draw(bd_wall *wall, bd_gfx *gfx) {
  if (wall == NULL || gfx == NULL) {
    return;
  }
  if (!wall->prepared) {
    bd_rect viewport = wall->have_last
                           ? wall->last_viewport
                           : bd_rect_make(0, 0, bd_gfx_width(gfx), bd_gfx_height(gfx));
    bd_wall_begin_frame(wall, gfx, viewport, wall->last_origin_x,
                        wall->last_origin_y);
  }
  wall->prepared = false;
  if (wall->count == 0) {
    return;
  }
  bd_rect viewport = wall->draw_viewport;
  int origin_x = wall->draw_origin_x;
  int origin_y = wall->draw_origin_y;

  bd_gfx_push_clip(gfx, viewport);

  for (int i = 0; i < wall->visible_count; ++i) {
    bd_wall_entry *entry = &wall->entries[wall->visible[i]];
    bd_rect screen = bd_rect_make(0, 0, 0, 0);
    item_screen_rect(entry, origin_x, origin_y, &screen);
    draw_cover(wall, gfx, entry, screen);
  }
  for (int i = 0; i < wall->visible_count; ++i) {
    bd_wall_entry *entry = &wall->entries[wall->visible[i]];
    bd_rect screen = bd_rect_make(0, 0, 0, 0);
    item_screen_rect(entry, origin_x, origin_y, &screen);
    draw_shadow(wall, gfx, screen);
  }
  for (int i = 0; i < wall->visible_count; ++i) {
    bd_wall_entry *entry = &wall->entries[wall->visible[i]];
    bd_rect screen = bd_rect_make(0, 0, 0, 0);
    item_screen_rect(entry, origin_x, origin_y, &screen);
    draw_badge_pill(wall, gfx, entry, screen);
  }
  for (int i = 0; i < wall->visible_count; ++i) {
    bd_wall_entry *entry = &wall->entries[wall->visible[i]];
    bd_rect screen = bd_rect_make(0, 0, 0, 0);
    item_screen_rect(entry, origin_x, origin_y, &screen);
    draw_badge_text(wall, gfx, entry, screen);
  }
  for (int i = 0; i < wall->visible_count; ++i) {
    bd_wall_entry *entry = &wall->entries[wall->visible[i]];
    bd_rect screen = bd_rect_make(0, 0, 0, 0);
    item_screen_rect(entry, origin_x, origin_y, &screen);
    draw_title(wall, gfx, entry, screen);
  }
  for (int i = 0; i < wall->visible_count; ++i) {
    bd_wall_entry *entry = &wall->entries[wall->visible[i]];
    bd_rect screen = bd_rect_make(0, 0, 0, 0);
    item_screen_rect(entry, origin_x, origin_y, &screen);
    draw_subtitle(wall, gfx, entry, screen);
  }

  bd_gfx_fill_rect(gfx, viewport, wall_veil(&wall->style));

  bd_color clear = bd_color_rgba(0, 0, 0, 0);
  bd_color dark = bd_color_rgba(0, 0, 0, 72);
  bd_rect left = bd_rect_make(viewport.x, viewport.y, 56, viewport.h);
  bd_rect right = bd_rect_make(viewport.x + viewport.w - 56, viewport.y, 56,
                               viewport.h);
  bd_gfx_fill_gradient(gfx, left, 0, dark, clear, BD_GRADIENT_HORIZONTAL);
  bd_gfx_fill_gradient(gfx, right, 0, clear, dark, BD_GRADIENT_HORIZONTAL);

  if (wall->focus >= 0 && wall->focus < wall->count) {
    bd_wall_entry *entry = &wall->entries[wall->focus];
    bd_rect screen = bd_rect_make(0, 0, 0, 0);
    bd_rect focus_visible;
    if (item_screen_rect(entry, origin_x, origin_y, &screen) &&
        (focus_visible = bd_rect_intersect(screen, viewport)).w > 0 &&
        focus_visible.h > 0) {
      draw_focus_content(wall, gfx, entry, screen);
    }
  }

  bd_gfx_pop_clip(gfx);
}

bool bd_wall_has_pending_images(const bd_wall *wall) {
  if (wall == NULL) {
    return false;
  }
  bd_image_cache_stats stats = bd_gfx_image_cache_get_stats(wall->gfx);
  return stats.pending > 0;
}

bd_wall_stats bd_wall_get_stats(const bd_wall *wall) {
  bd_wall_stats stats;
  memset(&stats, 0, sizeof(stats));
  if (wall == NULL) {
    return stats;
  }
  stats = wall->stats;
  bd_image_cache_stats images = bd_gfx_image_cache_get_stats(wall->gfx);
  stats.pending_images = images.pending;
  stats.resident_textures = images.textures;
  stats.resident_texture_bytes = images.resident_bytes;
  return stats;
}

bool bd_wall_focus_move(bd_wall *wall, int dx, int dy) {
  if (wall == NULL || wall->count == 0 || (dx == 0 && dy == 0)) {
    return false;
  }
  if (dx != 0) {
    dy = 0;
    dx = dx < 0 ? -1 : 1;
  } else {
    dy = dy < 0 ? -1 : 1;
  }

  bd_rect focus = wall->entries[wall->focus].item.rect;
  int reach = 1600;
  bd_rect region;
  if (dx > 0) {
    region = bd_rect_make(focus.x + focus.w + 1, focus.y - focus.h * 2, reach,
                          focus.h * 5);
  } else if (dx < 0) {
    region = bd_rect_make(focus.x - reach, focus.y - focus.h * 2, reach,
                          focus.h * 5);
  } else if (dy > 0) {
    region = bd_rect_make(focus.x - focus.w * 2, focus.y + focus.h + 1,
                          focus.w * 5, reach);
  } else {
    region = bd_rect_make(focus.x - focus.w * 2, focus.y - reach, focus.w * 5,
                          reach);
  }

  int candidates[BD_WALL_MAX_QUERY];
  int count = query_wall(wall, region, candidates, BD_WALL_MAX_QUERY, NULL);
  int focus_cx = focus.x + focus.w / 2;
  int focus_cy = focus.y + focus.h / 2;
  int best = -1;
  int best_score = 0;
  for (int i = 0; i < count; ++i) {
    bd_wall_entry *entry = &wall->entries[candidates[i]];
    int cx = entry->item.rect.x + entry->item.rect.w / 2;
    int cy = entry->item.rect.y + entry->item.rect.h / 2;
    int primary = 0;
    int secondary = 0;
    if (dx > 0) {
      primary = cx - focus_cx;
      secondary = abs(cy - focus_cy);
    } else if (dx < 0) {
      primary = focus_cx - cx;
      secondary = abs(cy - focus_cy);
    } else if (dy > 0) {
      primary = cy - focus_cy;
      secondary = abs(cx - focus_cx);
    } else {
      primary = focus_cy - cy;
      secondary = abs(cx - focus_cx);
    }
    if (primary <= 0) {
      continue;
    }
    int limit = (dx != 0 ? focus.h : focus.w) / 2 +
                (dx != 0 ? entry->item.rect.h : entry->item.rect.w) / 2;
    if (secondary > limit) {
      continue;
    }
    int score = primary * 2 + secondary * 3;
    if (best < 0 || score < best_score) {
      best = candidates[i];
      best_score = score;
    }
  }
  if (best >= 0 && best != wall->focus) {
    wall->focus = best;
    wall->focus_dirty = true;
    return true;
  }
  return false;
}
