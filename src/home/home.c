#include "home_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brickdesk/app.h"

#ifndef BRICKDESK_ASSET_DIR
#define BRICKDESK_ASSET_DIR "assets"
#endif

static const bd_color COVER_INK = {255.0f / 255.0f, 249.0f / 255.0f,
                                   241.0f / 255.0f, 1.0f};
static const bd_color COVER_INK_2 = {255.0f / 255.0f, 249.0f / 255.0f,
                                     241.0f / 255.0f, 163.0f / 255.0f};
static const bd_color FOCUS_RING = {255.0f / 255.0f, 250.0f / 255.0f,
                                    242.0f / 255.0f, 239.0f / 255.0f};

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

static void copy_text(char *dst, size_t size, const char *src) {
  if (dst == NULL || size == 0) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  snprintf(dst, size, "%s", src);
}

static int clamp_int(int value, int minimum, int maximum) {
  if (value < minimum) {
    return minimum;
  }
  if (value > maximum) {
    return maximum;
  }
  return value;
}

static void quick_scroll_to_selection(bd_home *home);

static int floor_div(int value, int divisor) {
  int quotient = value / divisor;
  if (value < 0 && value % divisor != 0) {
    --quotient;
  }
  return quotient;
}

static int max_pan(const bd_home *home) {
  int limit = home->wall_width - home->width;
  return limit > 0 ? limit : 0;
}

static int initial_pan_for_rect(const bd_home *home, bd_rect rect) {
  return clamp_int(rect.x - 96, 0, max_pan(home));
}

uint64_t bd_home_pan_end(const bd_home *home) {
  return home->pan_anim.active ? home->pan_anim.start_ms + BD_HOME_PAN_MS : 0;
}

uint64_t bd_home_launch_end(const bd_home *home) {
  if (home->launch == BD_HOME_LAUNCH_NONE) {
    return 0;
  }
  if (home->launch == BD_HOME_LAUNCH_OPEN && home->launch_complete) {
    return 0;
  }
  return home->launch_start + BD_HOME_LAUNCH_MS;
}

uint64_t bd_home_quick_end(const bd_home *home) {
  return home->quick_anim ? home->quick_start + BD_HOME_PANEL_MS : 0;
}

uint64_t bd_home_hint_end(const bd_home *home) {
  return home->hint_anim ? home->hint_start + BD_HOME_PANEL_MS : 0;
}

uint64_t bd_home_dialog_end(const bd_home *home) {
  return home->dialog_anim ? home->dialog_start + BD_HOME_DIALOG_MS : 0;
}

uint64_t bd_home_closing_end(const bd_home *home) {
  return home->closing_active ? home->closing_start + BD_HOME_CLOSE_MS : 0;
}

float bd_home_clampf(float value, float minimum, float maximum) {
  if (value < minimum) {
    return minimum;
  }
  if (value > maximum) {
    return maximum;
  }
  return value;
}

float bd_home_lerp(float a, float b, float t) {
  return a + (b - a) * t;
}

int bd_home_lerp_int(int a, int b, float t) {
  float value = (float)a + ((float)b - (float)a) * t;
  return (int)(value + (value >= 0.0f ? 0.5f : -0.5f));
}

static float bezier_axis(float t, float p1, float p2) {
  float inverse = 1.0f - t;
  return 3.0f * inverse * inverse * t * p1 +
         3.0f * inverse * t * t * p2 + t * t * t;
}

static float bezier(float x1, float y1, float x2, float y2, float x) {
  if (x <= 0.0f) {
    return 0.0f;
  }
  if (x >= 1.0f) {
    return 1.0f;
  }
  float low = 0.0f;
  float high = 1.0f;
  float t = x;
  for (int iteration = 0; iteration < 12; ++iteration) {
    float current = bezier_axis(t, x1, x2);
    if (fabsf(current - x) < 0.0001f) {
      break;
    }
    if (current < x) {
      low = t;
    } else {
      high = t;
    }
    t = (low + high) * 0.5f;
  }
  return bezier_axis(t, y1, y2);
}

float bd_home_ease_wall(float t) {
  return bezier(0.22f, 1.0f, 0.36f, 1.0f, bd_home_clampf(t, 0.0f, 1.0f));
}

float bd_home_ease_standard(float t) {
  return bezier(0.2f, 0.7f, 0.2f, 1.0f, bd_home_clampf(t, 0.0f, 1.0f));
}

float bd_home_ease_out(float t) {
  t = bd_home_clampf(t, 0.0f, 1.0f);
  return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

static void palette_set_dark(bd_home_palette *palette) {
  palette->bg = bd_color_rgb(18, 16, 13);
  palette->bg_deep = bd_color_rgb(11, 10, 8);
  palette->surface = bd_color_rgb(27, 24, 20);
  palette->surface2 = bd_color_rgb(36, 31, 25);
  palette->surface3 = bd_color_rgb(47, 42, 34);
  palette->ink = bd_color_rgb(243, 237, 226);
  palette->ink2 = bd_color_rgb(188, 178, 163);
  palette->ink3 = bd_color_rgb(143, 134, 120);
  palette->line = bd_color_rgba(243, 237, 226, 26);
  palette->line_strong = bd_color_rgba(243, 237, 226, 51);
  palette->chrome_strong = bd_color_rgba(18, 16, 13, 240);
  palette->chrome_base = bd_color_rgba(18, 16, 13, 190);
  palette->scrim = bd_color_rgba(0, 0, 0, 133);
  palette->tile_shade_a = bd_color_rgba(9, 8, 11, 112);
  palette->tile_shade_b = bd_color_rgba(9, 8, 11, 153);
}

void bd_home_palette_apply(bd_home *home) {
  palette_set_dark(&home->palette);
  home->palette.cover_ink = COVER_INK;
  home->palette.cover_ink2 = COVER_INK_2;
  home->palette.focus_ring = FOCUS_RING;
  home->accent = bd_accent_palette_from(home->style.accent_base, true);
}

/*
 * The bundled Noto SC subsets declare hhea ascent/descent of 1160/-288 units
 * per 1000 em.  stb_truetype scales by that 1.448 em span, so a "18 px" font
 * renders 18 / 1.448 = 12.4 px.  The design sizes are CSS pixels, so load the
 * nominal face at 1.448x and keep the layout constants in CSS pixels.
 */
#define BD_HOME_FONT_EM_SCALE 1.448f

static bd_font *load_font(bd_gfx *gfx, const char *name, int pixel_size) {
  char path[1024];
  int written = snprintf(path, sizeof(path), "%s/fonts/%s", BRICKDESK_ASSET_DIR,
                         name);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    return NULL;
  }
  int nominal = (int)((float)pixel_size * BD_HOME_FONT_EM_SCALE + 0.5f);
  return bd_font_create(gfx, path, nominal);
}

static bd_font *load_optional_full_font(bd_gfx *gfx) {
  static const char *const CANDIDATES[] = {
      "NotoSansSC-Full.ttf", "NotoSansSC-Full.otf",
      "NotoSansCJKsc-Regular.otf", "NotoSansSC-Regular.otf",
  };
  for (size_t i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); ++i) {
    char path[1024];
    int written = snprintf(path, sizeof(path), "%s/fonts/%s", BRICKDESK_ASSET_DIR,
                           CANDIDATES[i]);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
      continue;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
      continue;
    }
    fclose(file);
    bd_font *font = bd_font_create(gfx, path, 29);
    if (font != NULL) {
      return font;
    }
  }
  return NULL;
}

static void fonts_free(bd_home_fonts *fonts) {
  bd_font_destroy(fonts->title31);
  bd_font_destroy(fonts->title25);
  bd_font_destroy(fonts->title22);
  bd_font_destroy(fonts->title18);
  bd_font_destroy(fonts->tab18);
  bd_font_destroy(fonts->body16);
  bd_font_destroy(fonts->body15);
  bd_font_destroy(fonts->body14);
  bd_font_destroy(fonts->badge13);
  bd_font_destroy(fonts->serif25);
  bd_font_destroy(fonts->serif22);
  bd_font_destroy(fonts->serif17);
  bd_font_destroy(fonts->fallback);
  memset(fonts, 0, sizeof(*fonts));
}

static bool fonts_load(bd_home_fonts *fonts, bd_gfx *gfx) {
  memset(fonts, 0, sizeof(*fonts));
  fonts->title31 = load_font(gfx, "NotoSansSC-700-subset.ttf", 31);
  fonts->title25 = load_font(gfx, "NotoSansSC-700-subset.ttf", 25);
  fonts->title22 = load_font(gfx, "NotoSansSC-700-subset.ttf", 22);
  fonts->title18 = load_font(gfx, "NotoSansSC-700-subset.ttf", 18);
  fonts->tab18 = load_font(gfx, "NotoSansSC-700-subset.ttf", 18);
  fonts->body16 = load_font(gfx, "NotoSansSC-500-subset.ttf", 16);
  fonts->body15 = load_font(gfx, "NotoSansSC-500-subset.ttf", 15);
  fonts->body14 = load_font(gfx, "NotoSansSC-500-subset.ttf", 14);
  /* 13 px is the smallest UI face: badges, meta values and keycaps. */
  fonts->badge13 = load_font(gfx, "NotoSansSC-700-subset.ttf", 13);
  fonts->serif25 = load_font(gfx, "NotoSerifSC-500-subset.ttf", 25);
  fonts->serif22 = load_font(gfx, "NotoSerifSC-500-subset.ttf", 22);
  fonts->serif17 = load_font(gfx, "NotoSerifSC-500-subset.ttf", 17);
  fonts->fallback = load_optional_full_font(gfx);
  if (fonts->title31 == NULL || fonts->title25 == NULL ||
      fonts->title22 == NULL || fonts->title18 == NULL ||
      fonts->tab18 == NULL || fonts->body16 == NULL ||
      fonts->body15 == NULL || fonts->body14 == NULL ||
      fonts->badge13 == NULL || fonts->serif25 == NULL ||
      fonts->serif22 == NULL || fonts->serif17 == NULL) {
    fonts_free(fonts);
    return false;
  }
  bd_font *all[] = {fonts->title31, fonts->title25, fonts->title22,
                    fonts->title18, fonts->tab18,    fonts->body16,
                    fonts->body15,  fonts->body14,   fonts->badge13,
                    fonts->serif25, fonts->serif22,   fonts->serif17};
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
    bd_font_set_fallback(all[i], fonts->fallback);
  }
  return true;
}

static void entry_free(bd_home_entry *entry) {
  free(entry->id);
  free(entry->title);
  free(entry->subtitle);
  free(entry->image);
  memset(entry, 0, sizeof(*entry));
}

static void entries_free(bd_home *home) {
  for (int i = 0; i < home->count; ++i) {
    bd_home_entry *entry = &home->entries[i];
    if (entry->custom_draw != NULL && entry->custom_visible) {
      entry->custom_draw(entry->custom_user, home, home->gfx,
                         bd_home_screen_rect(home, i), false);
    }
    entry_free(entry);
  }
  free(home->entries);
  home->entries = NULL;
  home->count = 0;
  home->custom_count = 0;
  free(home->col_head);
  home->col_head = NULL;
  home->cols = 0;
  free(home->nodes);
  home->nodes = NULL;
  home->node_count = 0;
}

static bool ensure_columns(uint8_t **occ, int *capacity, int needed) {
  if (needed <= *capacity) {
    return true;
  }
  int next = *capacity > 0 ? *capacity : 16;
  while (next < needed) {
    next *= 2;
  }
  uint8_t *grown = (uint8_t *)realloc(*occ, (size_t)next);
  if (grown == NULL) {
    return false;
  }
  memset(grown + *capacity, 0, (size_t)(next - *capacity));
  *occ = grown;
  *capacity = next;
  return true;
}

static bool layout_items(bd_home *home) {
  uint8_t *occ = NULL;
  int capacity = 0;
  int max_col = -1;
  int first_free = 0;

  for (int i = 0; i < home->count; ++i) {
    bd_home_entry *entry = &home->entries[i];
    int w = entry->w;
    int h = entry->h;
    int spot_col = -1;
    int spot_row = 0;
    for (int c = first_free; c <= max_col && spot_col < 0; ++c) {
      for (int r = 0; r <= BD_HOME_ROWS - h; ++r) {
        uint8_t mask = (uint8_t)(((1u << h) - 1u) << r);
        bool fits = true;
        for (int x = c; x < c + w; ++x) {
          if ((occ[x] & mask) != 0) {
            fits = false;
            break;
          }
        }
        if (fits) {
          spot_col = c;
          spot_row = r;
          break;
        }
      }
    }
    if (spot_col < 0) {
      spot_col = max_col + 1;
      spot_row = 0;
    }
    if (!ensure_columns(&occ, &capacity, spot_col + w)) {
      free(occ);
      return false;
    }
    uint8_t mask = (uint8_t)(((1u << h) - 1u) << spot_row);
    for (int x = spot_col; x < spot_col + w; ++x) {
      occ[x] |= mask;
    }
    if (spot_col + w - 1 > max_col) {
      max_col = spot_col + w - 1;
    }
    while (first_free <= max_col && occ[first_free] == 0x0Fu) {
      ++first_free;
    }

    entry->col = spot_col;
    entry->row = spot_row;
    entry->rect = bd_rect_make(
        spot_col * BD_HOME_PITCH, spot_row * BD_HOME_PITCH,
        w * BD_HOME_CELL + (w - 1) * BD_HOME_GAP,
        h * BD_HOME_CELL + (h - 1) * BD_HOME_GAP);
  }

  home->cols = max_col + 1;
  home->wall_width =
      home->cols > 0 ? home->cols * BD_HOME_PITCH - BD_HOME_GAP : 0;
  int free_cells = 0;
  for (int c = 0; c < home->cols; ++c) {
    uint8_t mask = occ[c];
    int used = 0;
    for (int bit = 0; bit < BD_HOME_ROWS; ++bit) {
      if ((mask & (1u << bit)) != 0) {
        ++used;
      }
    }
    free_cells += BD_HOME_ROWS - used;
  }
  home->free_cells = free_cells;
  free(occ);
  return true;
}

static bool build_index(bd_home *home) {
  if (home->cols <= 0 || home->count <= 0) {
    return true;
  }
  home->col_head = (int *)malloc((size_t)home->cols * sizeof(int));
  home->nodes = (bd_home_index_node *)malloc(
      (size_t)home->count * 3u * sizeof(bd_home_index_node));
  if (home->col_head == NULL || home->nodes == NULL) {
    return false;
  }
  for (int c = 0; c < home->cols; ++c) {
    home->col_head[c] = -1;
  }
  home->node_count = 0;
  for (int i = 0; i < home->count; ++i) {
    bd_home_entry *entry = &home->entries[i];
    for (int c = entry->col; c < entry->col + entry->w; ++c) {
      if (c < 0 || c >= home->cols) {
        continue;
      }
      bd_home_index_node *node = &home->nodes[home->node_count++];
      node->item = i;
      node->next = home->col_head[c];
      home->col_head[c] = (int)(home->node_count - 1);
    }
  }
  return true;
}

int bd_home_query_indexed(bd_home *home, bd_rect rect, int *out, int capacity,
                          int *visited_out, int *buckets_out) {
  int visited = 0;
  int buckets = 0;
  if (home->count == 0 || home->cols == 0 || rect.w <= 0 || rect.h <= 0 ||
      out == NULL || capacity <= 0) {
    if (visited_out != NULL) {
      *visited_out = 0;
    }
    if (buckets_out != NULL) {
      *buckets_out = 0;
    }
    return 0;
  }
  ++home->stamp;
  if (home->stamp == 0) {
    for (int i = 0; i < home->count; ++i) {
      home->entries[i].stamp = 0;
    }
    home->stamp = 1;
  }
  int first = floor_div(rect.x, BD_HOME_PITCH);
  int last = floor_div(rect.x + rect.w - 1, BD_HOME_PITCH);
  if (first < 0) {
    first = 0;
  }
  if (last >= home->cols) {
    last = home->cols - 1;
  }
  int count = 0;
  for (int c = first; c <= last && count < capacity; ++c) {
    ++buckets;
    for (int node = home->col_head[c]; node >= 0 && count < capacity;
         node = home->nodes[node].next) {
      int index = home->nodes[node].item;
      bd_home_entry *entry = &home->entries[index];
      if (entry->stamp == home->stamp) {
        continue;
      }
      entry->stamp = home->stamp;
      ++visited;
      if (bd_rect_intersect(entry->rect, rect).w > 0) {
        out[count++] = index;
      }
    }
  }
  if (visited_out != NULL) {
    *visited_out = visited;
  }
  if (buckets_out != NULL) {
    *buckets_out = buckets;
  }
  return count;
}

void bd_home_damage_rect(bd_home *home, bd_rect rect) {
  bd_gfx_damage_rect(home->gfx, rect);
}

void bd_home_damage_item(bd_home *home, int index) {
  if (index < 0 || index >= home->count) {
    return;
  }
  bd_rect rect = bd_home_screen_rect(home, index);
  rect.x -= 2;
  rect.y -= 2;
  rect.w += 4;
  rect.h += 4;
  bd_home_damage_rect(home, rect);
}

void bd_home_damage_all(bd_home *home) {
  bd_gfx_damage_all(home->gfx);
}

bd_rect bd_home_screen_rect(const bd_home *home, int index) {
  if (index < 0 || index >= home->count) {
    return bd_rect_make(0, 0, 0, 0);
  }
  const bd_rect rect = home->entries[index].rect;
  return bd_rect_make(rect.x - home->pan, rect.y, rect.w, rect.h);
}

bool bd_home_item_visible(const bd_home *home, int index) {
  bd_rect rect = bd_home_screen_rect(home, index);
  return rect.x < home->width && rect.x + rect.w > 0 && rect.y < home->height &&
         rect.y + rect.h > 0;
}

bool bd_home_item_matches_tab(const bd_home *home, int index) {
  if (index < 0 || index >= home->count) {
    return false;
  }
  const bd_home_entry *entry = &home->entries[index];
  switch (home->tab) {
    case BD_HOME_TAB_GAMES:
      return (entry->item.flags & BD_HOME_ITEM_GAME) != 0;
    case BD_HOME_TAB_APPS:
      return (entry->item.flags & BD_HOME_ITEM_GAME) == 0;
    case BD_HOME_TAB_FAVORITES:
      return (entry->item.flags & BD_HOME_ITEM_FAVORITE) != 0;
    case BD_HOME_TAB_ALL:
    default:
      return true;
  }
}

static int find_item(const bd_home *home, const char *id) {
  if (id == NULL) {
    return -1;
  }
  for (int i = 0; i < home->count; ++i) {
    if (home->entries[i].id != NULL && strcmp(home->entries[i].id, id) == 0) {
      return i;
    }
  }
  return -1;
}

int bd_home_running_index(const bd_home *home, const char *id) {
  if (id == NULL) {
    return -1;
  }
  for (int i = 0; i < home->running_count; ++i) {
    if (strcmp(home->running[i].id, id) == 0) {
      return i;
    }
  }
  return -1;
}

static void running_remove_at(bd_home *home, int index) {
  if (index < 0 || index >= home->running_count) {
    return;
  }
  bd_home_running *entry = &home->running[index];
  if (entry->frame != NULL) {
    bd_gfx_texture_destroy(home->gfx, entry->frame);
  }
  for (int i = index; i + 1 < home->running_count; ++i) {
    home->running[i] = home->running[i + 1];
  }
  --home->running_count;
  memset(&home->running[home->running_count], 0, sizeof(bd_home_running));
}

static void update_running_flags(bd_home *home) {
  for (int i = 0; i < home->count; ++i) {
    home->entries[i].running = false;
  }
  for (int i = 0; i < home->running_count; ++i) {
    int index = home->running[i].item_index;
    if (index >= 0 && index < home->count) {
      home->entries[index].running = true;
    }
  }
}

static bd_texture *load_last_frame_texture(bd_home *home, const char *path) {
  if (path == NULL || path[0] == '\0') {
    return NULL;
  }
  bd_app_last_frame frame;
  char error[128];
  if (!bd_app_last_frame_open(path, &frame, error, sizeof(error))) {
    return NULL;
  }
  const bd_app_last_frame_header *header = &frame.header;
  size_t packed =
      bd_app_last_frame_pixel_size(header->format, header->width, header->height);
  if (packed == 0 || header->width > 2048 || header->height > 2048) {
    bd_app_last_frame_close(&frame);
    return NULL;
  }
  uint8_t *source = (uint8_t *)malloc(packed);
  if (source == NULL) {
    bd_app_last_frame_close(&frame);
    return NULL;
  }
  bd_app_last_frame_header copied;
  bool ok = bd_app_last_frame_copy(&frame, source, packed, &copied, error,
                                   sizeof(error));
  bd_app_last_frame_close(&frame);
  if (!ok) {
    free(source);
    return NULL;
  }

  uint32_t width = copied.width;
  uint32_t height = copied.height;
  size_t bpp = copied.format == BD_APP_PIXEL_RGB565 ? 2u : 4u;
  uint8_t *rgba = (uint8_t *)malloc((size_t)width * height * 4u);
  if (rgba == NULL) {
    free(source);
    return NULL;
  }
  for (uint32_t y = 0; y < height; ++y) {
    /* The shared-memory frame stores GL's bottom-first rows. */
    const uint8_t *row = source + (size_t)(height - 1u - y) * width * bpp;
    uint8_t *out = rgba + (size_t)y * width * 4u;
    if (copied.format == BD_APP_PIXEL_RGB565) {
      for (uint32_t x = 0; x < width; ++x) {
        uint16_t value = (uint16_t)(row[x * 2u] | (row[x * 2u + 1u] << 8));
        uint8_t r = (uint8_t)((value >> 11) & 0x1Fu);
        uint8_t g = (uint8_t)((value >> 5) & 0x3Fu);
        uint8_t b = (uint8_t)(value & 0x1Fu);
        out[x * 4u + 0u] = (uint8_t)((r * 255u + 15u) / 31u);
        out[x * 4u + 1u] = (uint8_t)((g * 255u + 31u) / 63u);
        out[x * 4u + 2u] = (uint8_t)((b * 255u + 15u) / 31u);
        out[x * 4u + 3u] = 255;
      }
    } else {
      for (uint32_t x = 0; x < width; ++x) {
        out[x * 4u + 0u] = row[x * 4u + 0u];
        out[x * 4u + 1u] = row[x * 4u + 1u];
        out[x * 4u + 2u] = row[x * 4u + 2u];
        out[x * 4u + 3u] = 255;
      }
    }
  }
  free(source);
  bd_texture *texture =
      bd_gfx_texture_create(home->gfx, (int)width, (int)height,
                            BD_TEXTURE_LINEAR, rgba);
  free(rgba);
  return texture;
}


static void damage_quick_panel(bd_home *home) {
  bd_rect panel = bd_home_quick_panel_rect(home);
  panel.x -= 4;
  panel.w += 8;
  panel.y = 0;
  panel.h = home->height;
  bd_home_damage_rect(home, panel);
}

bd_home *bd_home_create(bd_gfx *gfx, const bd_home_style *style) {
  if (gfx == NULL || style == NULL) {
    return NULL;
  }
  bd_home *home = (bd_home *)calloc(1, sizeof(*home));
  if (home == NULL) {
    return NULL;
  }
  home->gfx = gfx;
  home->style = *style;
  home->width = style->width > 0 ? style->width : bd_gfx_width(gfx);
  home->height = style->height > 0 ? style->height : bd_gfx_height(gfx);
  home->focus = -1;
  home->pan = 0;
  home->animations = true;
  home->custom_count = 0;
  home->surface_visible = true;
  home->quick_last_x = home->width;
  home->brightness = -1;
  home->volume = -1;
  home->bluetooth = false;
  home->status.wifi_known = false;
  home->status.wifi = false;
  home->status.battery_percent = -1;
  home->status.max_background = 3;
  copy_text(home->clock, sizeof(home->clock), "22:26");
  home->status.clock = home->clock;

  bd_home_palette_apply(home);
  if (!fonts_load(&home->fonts, gfx)) {
    free(home);
    return NULL;
  }

  bd_home_live live;
  memset(&live, 0, sizeof(live));
  live.title = "夜航";
  live.artist = "林间电台";
  live.previous_line = "灯塔在身后慢慢变小";
  live.current_line = "风把航线吹成一条细线";
  live.next_line = "我们往没有名字的海去";
  live.progress = 0.38f;
  live.elapsed_seconds = 87;
  live.duration_seconds = 228;
  live.playing = true;
  bd_home_set_live(home, &live);
  return home;
}

void bd_home_destroy(bd_home *home) {
  if (home == NULL) {
    return;
  }
  for (int i = 0; i < home->running_count; ++i) {
    if (home->running[i].frame != NULL) {
      bd_gfx_texture_destroy(home->gfx, home->running[i].frame);
    }
  }
  if (home->memory_dither != NULL) {
    bd_gfx_texture_destroy(home->gfx, home->memory_dither);
    home->memory_dither = NULL;
  }
  entries_free(home);
  fonts_free(&home->fonts);
  free(home);
}

bool bd_home_set_items(bd_home *home, const bd_home_item *items, int count) {
  if (home == NULL || count < 0 || (count > 0 && items == NULL)) {
    return false;
  }
  char old_focus[BD_HOME_ID_MAX];
  old_focus[0] = '\0';
  if (home->focus >= 0 && home->focus < home->count &&
      home->entries[home->focus].id != NULL) {
    copy_text(old_focus, sizeof(old_focus), home->entries[home->focus].id);
  }
  char old_quick_id[BD_HOME_ID_MAX];
  old_quick_id[0] = '\0';
  bool had_quick_running =
      home->quick_open && home->quick_sel >= 0 &&
      home->quick_sel < home->running_count;
  if (had_quick_running) {
    copy_text(old_quick_id, sizeof(old_quick_id),
              home->running[home->quick_sel].id);
  }
  bool had_quick_control =
      home->quick_open && home->quick_sel >= home->running_count &&
      home->quick_sel < home->running_count + BD_HOME_QUICK_CONTROL_COUNT;
  int old_quick_control =
      had_quick_control ? home->quick_sel - home->running_count : -1;

  entries_free(home);
  home->focus = -1;
  home->nav_row = 0;
  home->pan = 0;
  home->pan_anim.active = false;
  home->launch = BD_HOME_LAUNCH_NONE;
  home->launch_complete = false;
  home->launch_painted = false;
  home->launch_app_painted = false;
  home->closing_active = false;
  home->dialog_open = false;
  home->dialog_visible = false;
  home->dialog_anim = false;
  home->dialog_target = false;
  home->dialog_kind = BD_HOME_DIALOG_NONE;
  home->dialog_result = BD_HOME_DIALOG_RESULT_NONE;
  home->dialog_pending_id[0] = '\0';
  home->dialog_victim_id[0] = '\0';

  if (count == 0) {
    while (home->running_count > 0) {
      running_remove_at(home, 0);
    }
    update_running_flags(home);
    home->quick_sel = 0;
    home->quick_scroll = 0;
    home->wall_width = 0;
    home->free_cells = 0;
    home->stats.total_items = 0;
    home->stats.wall_width = 0;
    home->stats.free_cells = 0;
    home->stats.focus_index = -1;
    bd_home_damage_all(home);
    return true;
  }

  home->entries = (bd_home_entry *)calloc((size_t)count, sizeof(bd_home_entry));
  if (home->entries == NULL) {
    return false;
  }
  home->count = count;
  bool failed = false;
  for (int i = 0; i < count; ++i) {
    const bd_home_item *source = &items[i];
    if (source->id == NULL || source->id[0] == '\0') {
      failed = true;
      break;
    }
    for (int previous = 0; previous < i; ++previous) {
      if (home->entries[previous].id != NULL &&
          strcmp(home->entries[previous].id, source->id) == 0) {
        failed = true;
        break;
      }
    }
    if (failed) {
      break;
    }
    bd_home_entry *entry = &home->entries[i];
    entry->item = *source;
    entry->id = duplicate_string(source->id);
    entry->title = duplicate_string(source->title != NULL ? source->title : "");
    entry->subtitle = duplicate_string(source->subtitle);
    entry->image = duplicate_string(source->image_path);
    if (entry->id == NULL || entry->title == NULL ||
        (source->subtitle != NULL && entry->subtitle == NULL) ||
        (source->image_path != NULL && entry->image == NULL)) {
      failed = true;
      break;
    }
    entry->item.id = entry->id;
    entry->item.title = entry->title;
    entry->item.subtitle = entry->subtitle;
    entry->item.image_path = entry->image;
    if ((source->flags & BD_HOME_ITEM_LIVE) != 0) {
      entry->w = 2;
      entry->h = 2;
    } else {
      entry->w = source->span_w > 0 ? source->span_w : 1;
      entry->h = source->span_h > 0 ? source->span_h : 1;
    }
    if (entry->w < 1) {
      entry->w = 1;
    }
    if (entry->w > 3) {
      entry->w = 3;
    }
    if (entry->h < 1) {
      entry->h = 1;
    }
    if (entry->h > 2) {
      entry->h = 2;
    }
    entry->item.span_w = entry->w;
    entry->item.span_h = entry->h;
    entry->item.badge_number =
        source->badge_number > 0 ? source->badge_number : i + 1;
  }
  home->count = count;
  if (failed || !layout_items(home) || !build_index(home)) {
    entries_free(home);
    home->stats.total_items = 0;
    home->stats.wall_width = 0;
    home->stats.free_cells = 0;
    home->wall_width = 0;
    home->free_cells = 0;
    return false;
  }

  int focus = 0;
  if (old_focus[0] != '\0') {
    int found = find_item(home, old_focus);
    if (found >= 0) {
      focus = found;
    }
  }
  if (!bd_home_item_matches_tab(home, focus)) {
    focus = -1;
    for (int i = 0; i < count; ++i) {
      if (bd_home_item_matches_tab(home, i)) {
        focus = i;
        break;
      }
    }
  }
  home->focus = focus;
  if (focus >= 0) {
    home->nav_row = home->entries[focus].row;
    home->pan = initial_pan_for_rect(home, home->entries[focus].rect);
  } else {
    home->nav_row = 0;
    home->pan = 0;
  }

  for (int i = 0; i < home->running_count;) {
    int index = find_item(home, home->running[i].id);
    if (index < 0) {
      running_remove_at(home, i);
    } else {
      home->running[i].item_index = index;
      ++i;
    }
  }
  update_running_flags(home);

  home->stats.total_items = count;
  home->stats.wall_width = home->wall_width;
  home->stats.free_cells = home->free_cells;
  home->stats.focus_index = focus;
  home->stats.visited_items = 0;
  home->stats.visible_items = 0;
  home->stats.drawn_items = 0;
  home->stats.buckets_visited = 0;
  int max_selection = home->running_count + BD_HOME_QUICK_CONTROL_COUNT - 1;
  if (had_quick_running) {
    int index = bd_home_running_index(home, old_quick_id);
    if (index >= 0) {
      home->quick_sel = index;
    }
  } else if (had_quick_control) {
    home->quick_sel = home->running_count + old_quick_control;
  }
  home->quick_sel = clamp_int(home->quick_sel, 0, max_selection);
  quick_scroll_to_selection(home);
  bd_home_damage_all(home);
  return true;
}

int bd_home_item_count(const bd_home *home) {
  return home != NULL ? home->count : 0;
}

bool bd_home_item_rect(const bd_home *home, int index, bd_rect *out) {
  if (home == NULL || out == NULL || index < 0 || index >= home->count) {
    return false;
  }
  *out = home->entries[index].rect;
  return true;
}

bool bd_home_set_custom_draw(bd_home *home, const char *id,
                             bd_home_custom_draw_fn draw, void *user) {
  if (home == NULL || id == NULL) {
    return false;
  }
  int index = find_item(home, id);
  if (index < 0 || index >= home->count) {
    return false;
  }
  bd_home_entry *entry = &home->entries[index];
  if (draw == NULL) {
    entry->custom_draw = NULL;
    entry->custom_user = NULL;
    entry->custom_visible = false;
    for (int i = 0; i < home->custom_count; ++i) {
      if (home->custom_items[i] == index) {
        for (int j = i; j + 1 < home->custom_count; ++j) {
          home->custom_items[j] = home->custom_items[j + 1];
        }
        --home->custom_count;
        break;
      }
    }
    return true;
  }
  entry->custom_draw = draw;
  entry->custom_user = user;
  bool present = false;
  for (int i = 0; i < home->custom_count; ++i) {
    if (home->custom_items[i] == index) {
      present = true;
      break;
    }
  }
  if (!present && home->custom_count < BD_HOME_MAX_CUSTOM) {
    home->custom_items[home->custom_count++] = index;
  }
  return true;
}

void bd_home_set_surface_visible(bd_home *home, bool visible) {
  if (home == NULL) {
    return;
  }
  home->surface_visible = visible;
  for (int i = 0; i < home->custom_count; ++i) {
    int index = home->custom_items[i];
    if (index < 0 || index >= home->count) {
      continue;
    }
    bd_home_entry *entry = &home->entries[index];
    if (entry->custom_draw == NULL) {
      continue;
    }
    bool now_visible = visible && bd_home_item_visible(home, index);
    if (now_visible != entry->custom_visible) {
      entry->custom_visible = now_visible;
      entry->custom_draw(entry->custom_user, home, home->gfx,
                         bd_home_screen_rect(home, index), now_visible);
    }
  }
}

bool bd_home_surface_visible(const bd_home *home) {
  return home != NULL && home->surface_visible;
}

int bd_home_focus(const bd_home *home) {
  return home != NULL ? home->focus : -1;
}

const char *bd_home_selected_id(const bd_home *home) {
  if (home == NULL || home->closing_active || home->launch != BD_HOME_LAUNCH_NONE ||
      home->dialog_open || home->dialog_visible) {
    return NULL;
  }
  if (home->quick_open) {
    if (home->quick_sel < 0 || home->quick_sel >= home->running_count) {
      return NULL;
    }
    return home->running[home->quick_sel].id;
  }
  if (home->quick_anim) {
    return NULL;
  }
  if (home->focus < 0 || home->focus >= home->count) {
    return NULL;
  }
  return home->entries[home->focus].id;
}

int bd_home_tab(const bd_home *home) {
  return home != NULL ? home->tab : BD_HOME_TAB_ALL;
}

static void ensure_visible(bd_home *home) {
  if (home->focus < 0 || home->focus >= home->count) {
    return;
  }
  const bd_rect rect = home->entries[home->focus].rect;
  int x = home->pan;
  if (rect.x < x + 64) {
    x = rect.x - 64;
  }
  if (rect.x + rect.w > x + home->width - 64) {
    x = rect.x + rect.w - home->width + 64;
  }
  x = clamp_int(x, 0, max_pan(home));
  if (x != home->pan) {
    home->pan_anim.active = true;
    home->pan_anim.start_ms = home->now_ms;
    home->pan_anim.from = home->pan;
    home->pan_anim.to = x;
    bd_home_damage_all(home);
  }
}

int bd_home_find_item(const bd_home *home, const char *id) {
  if (home == NULL || id == NULL) {
    return -1;
  }
  for (int i = 0; i < home->count; ++i) {
    if (home->entries[i].id != NULL && strcmp(home->entries[i].id, id) == 0) {
      return i;
    }
  }
  return -1;
}

void bd_home_set_focus(bd_home *home, int index) {
  if (home == NULL || home->count == 0) {
    return;
  }
  index = clamp_int(index, 0, home->count - 1);
  if (index == home->focus) {
    return;
  }
  int previous = home->focus;
  home->focus = index;
  home->nav_row = home->entries[index].row;
  int target_pan = initial_pan_for_rect(home, home->entries[index].rect);
  if (target_pan != home->pan) {
    home->pan = target_pan;
    home->pan_anim.active = false;
    bd_home_damage_all(home);
  } else {
    bd_home_damage_item(home, previous);
    bd_home_damage_item(home, index);
  }
}

static bool focus_move(bd_home *home, int dx, int dy) {
  if (home->count == 0 || home->focus < 0 || home->focus >= home->count ||
      (dx == 0 && dy == 0)) {
    return false;
  }
  if (dx != 0) {
    dy = 0;
    dx = dx < 0 ? -1 : 1;
  } else {
    dy = dy < 0 ? -1 : 1;
  }
  const bd_home_entry *focus = &home->entries[home->focus];
  const bd_rect f = focus->rect;
  bd_rect region;
  const int reach = 2048;
  if (dx > 0) {
    region = bd_rect_make(f.x + f.w, 0, reach, BD_HOME_WALL_HEIGHT);
  } else if (dx < 0) {
    int start = f.x - reach;
    if (start < 0) {
      start = 0;
    }
    region = bd_rect_make(start, 0, f.x - start, BD_HOME_WALL_HEIGHT);
  } else if (dy > 0) {
    int start = f.x - reach;
    if (start < 0) {
      start = 0;
    }
    region = bd_rect_make(start, f.y + f.h, reach * 2, reach);
  } else {
    int start = f.x - reach;
    if (start < 0) {
      start = 0;
    }
    int begin = f.y - reach;
    if (begin < 0) {
      begin = 0;
    }
    region = bd_rect_make(start, begin, reach * 2, f.y - begin);
  }
  if (region.w <= 0 || region.h <= 0) {
    return false;
  }
  int candidates[BD_HOME_MAX_QUERY];
  int count = bd_home_query_indexed(home, region, candidates, BD_HOME_MAX_QUERY, NULL,
                               NULL);
  bool horizontal = dx != 0;
  bool have_band = false;
  if (horizontal) {
    for (int i = 0; i < count; ++i) {
      const bd_home_entry *entry = &home->entries[candidates[i]];
      if (entry->row <= home->nav_row &&
          home->nav_row < entry->row + entry->h) {
        have_band = true;
        break;
      }
    }
  }

  int best = -1;
  int best_score = 0;
  for (int i = 0; i < count; ++i) {
    int index = candidates[i];
    if (index == home->focus || !bd_home_item_matches_tab(home, index)) {
      continue;
    }
    const bd_home_entry *candidate = &home->entries[index];
    const bd_rect c = candidate->rect;
    if (dx > 0 && c.x < f.x + f.w) {
      continue;
    }
    if (dx < 0 && c.x + c.w > f.x) {
      continue;
    }
    if (dy > 0 && c.y < f.y + f.h) {
      continue;
    }
    if (dy < 0 && c.y + c.h > f.y) {
      continue;
    }
    if (horizontal && have_band &&
        !(candidate->row <= home->nav_row &&
          home->nav_row < candidate->row + candidate->h)) {
      continue;
    }
    int primary;
    if (dx > 0) {
      primary = c.x - (f.x + f.w);
    } else if (dx < 0) {
      primary = f.x - (c.x + c.w);
    } else if (dy > 0) {
      primary = c.y - (f.y + f.h);
    } else {
      primary = f.y - (c.y + c.h);
    }
    int overlap;
    int cross;
    if (horizontal) {
      int top = f.y > c.y ? f.y : c.y;
      int bottom = (f.y + f.h) < (c.y + c.h) ? (f.y + f.h) : (c.y + c.h);
      overlap = (bottom - top) / BD_HOME_PITCH;
      int center = (c.y + c.h / 2) / BD_HOME_PITCH;
      cross = center - home->nav_row;
      if (cross < 0) {
        cross = -cross;
      }
    } else {
      int left = f.x > c.x ? f.x : c.x;
      int right = (f.x + f.w) < (c.x + c.w) ? (f.x + f.w) : (c.x + c.w);
      overlap = (right - left) / BD_HOME_PITCH;
      int focus_center = (f.x + f.w / 2) / BD_HOME_PITCH;
      int center = (c.x + c.w / 2) / BD_HOME_PITCH;
      cross = center - focus_center;
      if (cross < 0) {
        cross = -cross;
      }
    }
    int miss = overlap > 0 ? 0 : 90 - overlap * 22;
    int score = primary * 10 + cross * 4 + miss;
    if (best < 0 || score < best_score) {
      best = index;
      best_score = score;
    }
  }
  if (best < 0 || best == home->focus) {
    return false;
  }
  int previous = home->focus;
  home->focus = best;
  const bd_home_entry *target = &home->entries[best];
  if (horizontal) {
    home->nav_row = clamp_int(home->nav_row, target->row,
                              target->row + target->h - 1);
  } else if (dy > 0) {
    home->nav_row = target->row;
  } else {
    home->nav_row = target->row + target->h - 1;
  }
  bd_home_damage_item(home, previous);
  bd_home_damage_item(home, best);
  ensure_visible(home);
  return true;
}

void bd_home_set_tab(bd_home *home, int tab) {
  if (home == NULL || tab < 0 || tab >= BD_HOME_TAB_COUNT || tab == home->tab) {
    return;
  }
  home->tab = tab;
  if (home->count > 0 && !bd_home_item_matches_tab(home, home->focus)) {
    int old_center_x = 0;
    int old_center_y = 0;
    if (home->focus >= 0 && home->focus < home->count) {
      const bd_rect old = home->entries[home->focus].rect;
      old_center_x = old.x + old.w / 2;
      old_center_y = old.y + old.h / 2;
    }
    int best = -1;
    int best_distance = 0;
    for (int i = 0; i < home->count; ++i) {
      if (!bd_home_item_matches_tab(home, i)) {
        continue;
      }
      const bd_rect rect = home->entries[i].rect;
      int distance = abs(rect.x + rect.w / 2 - old_center_x) +
                     abs(rect.y + rect.h / 2 - old_center_y);
      if (best < 0 || distance < best_distance) {
        best = i;
        best_distance = distance;
      }
    }
    if (best >= 0) {
      home->focus = best;
      const bd_home_entry *entry = &home->entries[best];
      home->nav_row = clamp_int(home->nav_row, entry->row, entry->row + entry->h - 1);
      ensure_visible(home);
    } else {
      home->focus = -1;
      home->nav_row = 0;
    }
  }
  bd_home_damage_all(home);
}

void bd_home_set_ambient_solid(bd_home *home, bool solid) {
  if (home == NULL || home->style.ambient_solid == solid) {
    return;
  }
  home->style.ambient_solid = solid;
  if (home->launch == BD_HOME_LAUNCH_OPEN) {
    home->launch_painted = false;
  }
  bd_home_damage_all(home);
}

bool bd_home_ambient_solid(const bd_home *home) {
  return home != NULL && home->style.ambient_solid;
}

void bd_home_set_accent(bd_home *home, bd_color accent_base) {
  if (home == NULL) {
    return;
  }
  home->style.accent_base = accent_base;
  bd_home_palette_apply(home);
  home->memory_dither_valid = false;
  if (home->launch == BD_HOME_LAUNCH_OPEN) {
    home->launch_painted = false;
  }
  bd_home_damage_all(home);
}

bd_color bd_home_accent_base(const bd_home *home) {
  return home != NULL ? home->style.accent_base : bd_color_rgb(224, 122, 95);
}

static void refresh_quick_values(bd_home *home) {
  int value = -1;
  home->brightness =
      home->quick_backend.read != NULL &&
              home->quick_backend.read(home->quick_backend.user,
                                       BD_HOME_QUICK_BRIGHTNESS, &value)
          ? clamp_int(value, 0, 100)
          : -1;
  value = -1;
  home->volume =
      home->quick_backend.read != NULL &&
              home->quick_backend.read(home->quick_backend.user,
                                       BD_HOME_QUICK_VOLUME, &value)
          ? clamp_int(value, 0, 100)
          : -1;
}

void bd_home_set_quick_backend(bd_home *home,
                               const bd_home_quick_backend *backend) {
  if (home == NULL) {
    return;
  }
  if (backend != NULL) {
    home->quick_backend = *backend;
  } else {
    memset(&home->quick_backend, 0, sizeof(home->quick_backend));
  }
  refresh_quick_values(home);
  damage_quick_panel(home);
}

void bd_home_refresh_quick(bd_home *home) {
  if (home == NULL) {
    return;
  }
  int old_brightness = home->brightness;
  int old_volume = home->volume;
  refresh_quick_values(home);
  if (old_brightness != home->brightness || old_volume != home->volume) {
    damage_quick_panel(home);
  }
}

bool bd_home_set_favorite(bd_home *home, const char *id, bool favorite) {
  if (home == NULL || id == NULL) {
    return false;
  }
  int index = bd_home_find_item(home, id);
  if (index < 0) {
    return false;
  }
  uint32_t flags = home->entries[index].item.flags;
  bool was = (flags & BD_HOME_ITEM_FAVORITE) != 0;
  if (was == favorite) {
    return true;
  }
  if (favorite) {
    home->entries[index].item.flags |= BD_HOME_ITEM_FAVORITE;
  } else {
    home->entries[index].item.flags &= ~BD_HOME_ITEM_FAVORITE;
  }
  bd_home_damage_all(home);
  return true;
}

bool bd_home_favorite(const bd_home *home, const char *id) {
  if (home == NULL || id == NULL) {
    return false;
  }
  int index = bd_home_find_item(home, id);
  return index >= 0 &&
         (home->entries[index].item.flags & BD_HOME_ITEM_FAVORITE) != 0;
}

void bd_home_set_blur_enabled(bd_home *home, bool enabled) {
  if (home == NULL || home->style.blur_enabled == enabled) {
    return;
  }
  home->style.blur_enabled = enabled;
  if (home->launch == BD_HOME_LAUNCH_OPEN) {
    home->launch_painted = false;
  }
  bd_home_damage_rect(home, bd_rect_make(0, 0, home->width, BD_HOME_TOP_STRIP));
  if (home->hint_visible || home->hint_anim) {
    bd_home_damage_rect(home, bd_rect_make(0, home->height - BD_HOME_HINT_STRIP,
                                           home->width, BD_HOME_HINT_STRIP));
  }
}

void bd_home_set_status(bd_home *home, const bd_home_status *status) {
  if (home == NULL || status == NULL) {
    return;
  }
  copy_text(home->clock, sizeof(home->clock),
            status->clock != NULL ? status->clock : "22:26");
  home->status.clock = home->clock;
  home->status.wifi_known = status->wifi_known;
  home->status.wifi = status->wifi_known ? status->wifi : false;
  home->status.battery_percent =
      status->battery_percent < 0 ? -1 : clamp_int(status->battery_percent, 0, 100);
  home->status.max_background =
      status->max_background > 0 ? status->max_background : 3;
  if (home->launch == BD_HOME_LAUNCH_OPEN) {
    home->launch_painted = false;
  }
  bd_home_damage_rect(home, bd_rect_make(0, 0, home->width, BD_HOME_TOP_STRIP));
}

void bd_home_set_live(bd_home *home, const bd_home_live *live) {
  if (home == NULL || live == NULL) {
    return;
  }
  copy_text(home->live_title, sizeof(home->live_title),
            live->title != NULL ? live->title : "夜航");
  copy_text(home->live_artist, sizeof(home->live_artist),
            live->artist != NULL ? live->artist : "林间电台");
  copy_text(home->live_previous, sizeof(home->live_previous),
            live->previous_line != NULL ? live->previous_line : "");
  copy_text(home->live_current, sizeof(home->live_current),
            live->current_line != NULL ? live->current_line : "");
  copy_text(home->live_next, sizeof(home->live_next),
            live->next_line != NULL ? live->next_line : "");
  home->live = *live;
  home->live.title = home->live_title;
  home->live.artist = home->live_artist;
  home->live.previous_line = home->live_previous;
  home->live.current_line = home->live_current;
  home->live.next_line = home->live_next;
  home->live.progress = bd_home_clampf(live->progress, 0.0f, 1.0f);
  if (home->live.elapsed_seconds < 0) {
    home->live.elapsed_seconds = 0;
  }
  if (home->live.duration_seconds <= 0) {
    home->live.duration_seconds = 1;
  }
  for (int i = 0; i < home->count; ++i) {
    if ((home->entries[i].item.flags & BD_HOME_ITEM_LIVE) != 0) {
      bd_home_damage_item(home, i);
    }
  }
}

bool bd_home_set_running(bd_home *home, const char *id,
                         const char *last_frame_path, bool paused,
                         int memory_mb) {
  if (home == NULL || id == NULL || id[0] == '\0') {
    return false;
  }
  int item = find_item(home, id);
  if (item < 0) {
    return false;
  }
  int existing = bd_home_running_index(home, id);
  bool panel_open = home->quick_open || home->quick_anim;
  int old_running_count = home->running_count;
  int old_sel = home->quick_sel;
  char selected_id[BD_HOME_ID_MAX];
  selected_id[0] = '\0';
  bool selected_running =
      panel_open && old_sel >= 0 && old_sel < old_running_count;
  if (selected_running) {
    copy_text(selected_id, sizeof(selected_id), home->running[old_sel].id);
  }
  bd_home_running updated;
  memset(&updated, 0, sizeof(updated));
  if (existing >= 0) {
    updated = home->running[existing];
  } else {
    if (home->running_count >= BD_HOME_MAX_RUNNING) {
      return false;
    }
  }
  copy_text(updated.id, sizeof(updated.id), id);
  updated.item_index = item;
  updated.paused = paused;
  updated.memory_mb = memory_mb < 0 ? -1 : memory_mb;
  if (existing < 0) {
    updated.since_ms = home->now_ms;
  }
  bool reload = false;
  if (last_frame_path != NULL && last_frame_path[0] != '\0') {
    if (strcmp(updated.frame_path, last_frame_path) != 0) {
      copy_text(updated.frame_path, sizeof(updated.frame_path), last_frame_path);
      reload = true;
    }
  }
  if (reload) {
    if (updated.frame != NULL) {
      bd_gfx_texture_destroy(home->gfx, updated.frame);
      updated.frame = NULL;
    }
    updated.frame = load_last_frame_texture(home, updated.frame_path);
  }
  if (existing >= 0) {
    /* Move the record without destroying its frame texture. */
    for (int i = existing; i + 1 < home->running_count; ++i) {
      home->running[i] = home->running[i + 1];
    }
    --home->running_count;
    memset(&home->running[home->running_count], 0, sizeof(bd_home_running));
  }
  int slot = home->running_count++;
  home->running[slot] = updated;
  update_running_flags(home);
  if (panel_open) {
    int max_selection = home->running_count + BD_HOME_QUICK_CONTROL_COUNT - 1;
    if (selected_running) {
      int index = bd_home_running_index(home, selected_id);
      if (index >= 0) {
        home->quick_sel = index;
      }
    } else if (home->running_count > old_running_count &&
               old_sel >= old_running_count) {
      /* A new app above the controls pushes the same control down one row. */
      home->quick_sel = old_sel + (home->running_count - old_running_count);
    }
    home->quick_sel = clamp_int(home->quick_sel, 0, max_selection);
    quick_scroll_to_selection(home);
  }
  home->memory_dither_valid = false;
  bd_home_damage_item(home, item);
  if (home->quick_open || home->quick_anim) {
    damage_quick_panel(home);
  }
  return true;
}

bool bd_home_set_running_memory(bd_home *home, const char *id, int memory_mb) {
  if (home == NULL || id == NULL) {
    return false;
  }
  int index = bd_home_running_index(home, id);
  if (index < 0) {
    return false;
  }
  int value = memory_mb < 0 ? -1 : memory_mb;
  if (home->running[index].memory_mb == value) {
    return true;
  }
  home->running[index].memory_mb = value;
  home->memory_dither_valid = false;
  if (home->quick_open || home->quick_anim) {
    damage_quick_panel(home);
  }
  if (home->dialog_open || home->dialog_anim) {
    bd_home_damage_all(home);
  }
  return true;
}

int bd_home_running_memory(const bd_home *home, const char *id) {
  if (home == NULL || id == NULL) {
    return -1;
  }
  int index = bd_home_running_index(home, id);
  return index >= 0 ? home->running[index].memory_mb : -1;
}

void bd_home_clear_running(bd_home *home, const char *id) {
  if (home == NULL || id == NULL) {
    return;
  }
  int index = bd_home_running_index(home, id);
  if (index < 0) {
    return;
  }
  int item = home->running[index].item_index;
  bool panel_open = home->quick_open || home->quick_anim;
  int old_sel = home->quick_sel;
  running_remove_at(home, index);
  update_running_flags(home);
  home->memory_dither_valid = false;
  bd_home_damage_item(home, item);
  if (panel_open) {
    if (index < old_sel) {
      --home->quick_sel;
    }
    int max_selection = home->running_count + BD_HOME_QUICK_CONTROL_COUNT - 1;
    home->quick_sel = clamp_int(home->quick_sel, 0, max_selection);
    quick_scroll_to_selection(home);
    damage_quick_panel(home);
  }
  if (home->closing_active && home->closing_item == item) {
    home->closing_active = false;
  }
}

int bd_home_running_count(const bd_home *home) {
  return home != NULL ? home->running_count : 0;
}

int bd_home_query_items(const bd_home *home, bd_rect world_rect, int *out,
                        int capacity) {
  if (home == NULL) {
    return 0;
  }
  bd_home *mutable_home = (bd_home *)home;
  return bd_home_query_indexed(mutable_home, world_rect, out, capacity, NULL,
                               NULL);
}

bool bd_home_has_pending_images(const bd_home *home) {
  if (home == NULL) {
    return false;
  }
  bd_image_cache_stats stats = bd_gfx_image_cache_get_stats(home->gfx);
  return stats.pending > 0;
}

bd_home_stats bd_home_get_stats(const bd_home *home) {
  bd_home_stats stats;
  memset(&stats, 0, sizeof(stats));
  if (home == NULL) {
    return stats;
  }
  stats = home->stats;
  stats.total_items = home->count;
  stats.wall_width = home->wall_width;
  stats.free_cells = home->free_cells;
  stats.focus_index = home->focus;
  stats.running_count = home->running_count;
  stats.pan_x = home->pan;
  stats.panning = home->pan_anim.active;
  bd_image_cache_stats images = bd_gfx_image_cache_get_stats(home->gfx);
  stats.pending_images = images.pending;
  stats.resident_textures = images.textures;
  stats.resident_texture_bytes = images.resident_bytes;
  return stats;
}

static void center_on_item(bd_home *home, int index) {
  if (index < 0 || index >= home->count) {
    return;
  }
  home->focus = index;
  home->nav_row = home->entries[index].row;
  home->pan = initial_pan_for_rect(home, home->entries[index].rect);
  home->pan_anim.active = false;
  bd_home_damage_all(home);
}

static void close_overlays(bd_home *home) {
  bool changed = false;
  if (home->quick_open || home->quick_anim) {
    home->quick_open = false;
    home->quick_anim = false;
    home->quick_target = false;
    home->quick_last_x = home->width;
    changed = true;
  }
  if (home->dialog_open || home->dialog_visible || home->dialog_anim) {
    home->dialog_open = false;
    home->dialog_visible = false;
    home->dialog_anim = false;
    home->dialog_target = false;
    home->dialog_kind = BD_HOME_DIALOG_NONE;
    home->dialog_result = BD_HOME_DIALOG_RESULT_NONE;
    home->dialog_pending_id[0] = '\0';
    home->dialog_victim_id[0] = '\0';
    changed = true;
  }
  if (home->hint_visible || home->hint_anim) {
    home->hint_visible = false;
    home->hint_anim = false;
    home->hint_target = false;
    changed = true;
  }
  if (changed) {
    bd_home_damage_all(home);
  }
}

bool bd_home_start_launch(bd_home *home, const char *id) {
  if (home == NULL || home->launch != BD_HOME_LAUNCH_NONE) {
    return false;
  }
  int index = find_item(home, id);
  if (index < 0) {
    return false;
  }
  close_overlays(home);
  center_on_item(home, index);
  home->launch = BD_HOME_LAUNCH_OPEN;
  home->launch_item = index;
  home->launch_start = home->now_ms;
  home->launch_from = bd_home_screen_rect(home, index);
  home->launch_complete = false;
  home->launch_painted = false;
  home->launch_app_painted = false;
  bd_home_damage_all(home);
  return true;
}

const char *bd_home_launch_target(const bd_home *home) {
  if (home == NULL || home->launch != BD_HOME_LAUNCH_OPEN) {
    return NULL;
  }
  if (home->launch_item < 0 || home->launch_item >= home->count) {
    return NULL;
  }
  return home->entries[home->launch_item].id;
}

bool bd_home_launch_complete(const bd_home *home) {
  return home != NULL && home->launch == BD_HOME_LAUNCH_OPEN &&
         home->launch_complete;
}

bool bd_home_start_return(bd_home *home, const char *id) {
  if (home == NULL) {
    return false;
  }
  if (home->launch == BD_HOME_LAUNCH_OPEN && !home->launch_complete) {
    return false;
  }
  int index = find_item(home, id);
  if (index < 0) {
    return false;
  }
  close_overlays(home);
  center_on_item(home, index);
  home->launch = BD_HOME_LAUNCH_RETURN;
  home->launch_item = index;
  home->launch_start = home->now_ms;
  home->launch_complete = false;
  home->launch_painted = false;
  home->launch_app_painted = false;
  bd_home_damage_all(home);
  return true;
}

float bd_home_quick_progress(const bd_home *home) {
  float target = home->quick_target ? 1.0f : 0.0f;
  if (!home->quick_anim) {
    return target;
  }
  float t = (float)(home->now_ms - home->quick_start) / (float)BD_HOME_PANEL_MS;
  return bd_home_lerp(home->quick_from, target, bd_home_ease_standard(t));
}

float bd_home_hint_progress(const bd_home *home) {
  float target = home->hint_target ? 1.0f : 0.0f;
  if (!home->hint_anim) {
    return target;
  }
  float t = (float)(home->now_ms - home->hint_start) / (float)BD_HOME_PANEL_MS;
  return bd_home_lerp(home->hint_from, target, bd_home_ease_standard(t));
}

float bd_home_dialog_progress(const bd_home *home) {
  float target = home->dialog_target ? 1.0f : 0.0f;
  if (!home->dialog_anim) {
    return target;
  }
  float t = (float)(home->now_ms - home->dialog_start) / (float)BD_HOME_DIALOG_MS;
  return bd_home_lerp(home->dialog_from, target, bd_home_ease_out(t));
}

bd_rect bd_home_quick_panel_rect(const bd_home *home) {
  float progress = bd_home_quick_progress(home);
  int closed_x = home->width;
  int open_x = home->width - 402;
  int x = bd_home_lerp_int(closed_x, open_x, progress);
  /* Flush to the right edge, full height, square outer corners. */
  return bd_rect_make(x, 0, 402, home->height);
}

bd_home_quick_layout bd_home_quick_layout_get(const bd_home *home) {
  bd_home_quick_layout layout;
  memset(&layout, 0, sizeof(layout));
  if (home == NULL) {
    return layout;
  }
  layout.rows_start = BD_HOME_QUICK_ROWS_START;
  int natural_rows = home->running_count > 0
                         ? home->running_count * BD_HOME_QUICK_ROW_H
                         : 30;
  int natural_controls = layout.rows_start + natural_rows +
                         BD_HOME_QUICK_ROWS_GAP + BD_HOME_QUICK_FOOT_GAP;
  int controls_h = BD_HOME_QUICK_CONTROL_COUNT * BD_HOME_QUICK_CONTROL_H;
  int bottom = home->height - BD_HOME_QUICK_BOTTOM_PAD;
  /* Default quotas keep their natural flow; a full list pins the controls
   * inside the panel and turns the run section into a bounded viewport. */
  layout.controls_y = natural_controls;
  if (layout.controls_y + controls_h > bottom) {
    layout.controls_y = bottom - controls_h;
  }
  if (layout.controls_y < 0) {
    layout.controls_y = 0;
  }
  layout.foot_baseline = layout.controls_y - BD_HOME_QUICK_FOOT_GAP;
  int available =
      layout.foot_baseline - BD_HOME_QUICK_ROWS_GAP - layout.rows_start;
  layout.visible_rows =
      available > 0 ? available / BD_HOME_QUICK_ROW_H : 0;
  if (home->running_count > 0 && layout.visible_rows < 1) {
    layout.visible_rows = 1;
  }
  if (layout.visible_rows > home->running_count) {
    layout.visible_rows = home->running_count;
  }
  layout.max_scroll = home->running_count > layout.visible_rows
                          ? home->running_count - layout.visible_rows
                          : 0;
  layout.scroll = clamp_int(home->quick_scroll, 0, layout.max_scroll);
  return layout;
}

static void quick_scroll_to_selection(bd_home *home) {
  bd_home_quick_layout layout = bd_home_quick_layout_get(home);
  if (home->quick_sel >= 0 && home->quick_sel < home->running_count) {
    int visible = layout.visible_rows > 0 ? layout.visible_rows : 1;
    if (home->quick_sel < layout.scroll) {
      home->quick_scroll = home->quick_sel;
    } else if (home->quick_sel >= layout.scroll + visible) {
      home->quick_scroll = home->quick_sel - visible + 1;
    }
  }
  layout = bd_home_quick_layout_get(home);
  home->quick_scroll = layout.scroll;
}

bool bd_home_open_quick(bd_home *home) {
  if (home == NULL || home->quick_open) {
    return false;
  }
  float current = bd_home_quick_progress(home);
  home->quick_from = current;
  home->quick_open = true;
  home->quick_target = true;
  home->quick_anim = true;
  home->quick_start = home->now_ms;
  home->quick_sel = home->running_count > 0 ? 0 : home->running_count;
  home->quick_scroll = 0;
  if (current <= 0.0f) {
    bd_home_damage_all(home);
  } else {
    damage_quick_panel(home);
  }
  return true;
}

bool bd_home_close_quick(bd_home *home) {
  if (home == NULL || (!home->quick_open && !home->quick_anim)) {
    return false;
  }
  home->quick_from = bd_home_quick_progress(home);
  home->quick_open = false;
  home->quick_target = false;
  home->quick_anim = true;
  home->quick_start = home->now_ms;
  damage_quick_panel(home);
  return true;
}

bool bd_home_quick_open(const bd_home *home) {
  return home != NULL && home->quick_open;
}

void bd_home_set_hint_visible(bd_home *home, bool visible) {
  if (home == NULL) {
    return;
  }
  if (home->launch != BD_HOME_LAUNCH_NONE) {
    visible = false;
  }
  if (visible == home->hint_target && !home->hint_anim) {
    return;
  }
  if (visible == home->hint_target) {
    return;
  }
  home->hint_from = bd_home_hint_progress(home);
  home->hint_target = visible;
  home->hint_visible = visible;
  home->hint_anim = true;
  home->hint_start = home->now_ms;
  bd_home_damage_rect(home, bd_rect_make(0, home->height - BD_HOME_HINT_STRIP,
                                         home->width, BD_HOME_HINT_STRIP));
}

bool bd_home_hint_visible(const bd_home *home) {
  return home != NULL && home->hint_visible;
}

bool bd_home_open_end_dialog(bd_home *home, const char *id) {
  if (home == NULL || id == NULL) {
    return false;
  }
  int item = find_item(home, id);
  if (item < 0 || bd_home_running_index(home, id) < 0) {
    return false;
  }
  home->hint_visible = false;
  home->hint_anim = false;
  home->hint_target = false;
  home->dialog_item = item;
  home->dialog_focus = 0;
  home->dialog_kind = BD_HOME_DIALOG_END_APP;
  home->dialog_result = BD_HOME_DIALOG_RESULT_NONE;
  home->dialog_max_background = 0;
  home->dialog_pending_id[0] = '\0';
  home->dialog_victim_id[0] = '\0';
  home->dialog_error[0] = '\0';
  home->dialog_from = home->dialog_visible ? bd_home_dialog_progress(home) : 0.0f;
  home->dialog_open = true;
  home->dialog_visible = true;
  home->dialog_target = true;
  home->dialog_anim = true;
  home->dialog_start = home->now_ms;
  bd_home_damage_all(home);
  return true;
}

bool bd_home_open_background_full(bd_home *home, const char *pending_id,
                                  const char *victim_id, int max_background) {
  if (home == NULL || pending_id == NULL || victim_id == NULL ||
      pending_id[0] == '\0' || victim_id[0] == '\0') {
    return false;
  }
  if (find_item(home, pending_id) < 0) {
    return false;
  }
  home->hint_visible = false;
  home->hint_anim = false;
  home->hint_target = false;
  home->dialog_item = find_item(home, victim_id);
  home->dialog_focus = 0;
  home->dialog_kind = BD_HOME_DIALOG_BACKGROUND_FULL;
  home->dialog_result = BD_HOME_DIALOG_RESULT_NONE;
  home->dialog_max_background = max_background > 0 ? max_background : 3;
  copy_text(home->dialog_pending_id, sizeof(home->dialog_pending_id),
            pending_id);
  copy_text(home->dialog_victim_id, sizeof(home->dialog_victim_id), victim_id);
  home->dialog_error[0] = '\0';
  home->dialog_from = home->dialog_visible ? bd_home_dialog_progress(home) : 0.0f;
  home->dialog_open = true;
  home->dialog_visible = true;
  home->dialog_target = true;
  home->dialog_anim = true;
  home->dialog_start = home->now_ms;
  bd_home_damage_all(home);
  return true;
}

void bd_home_set_dialog_error(bd_home *home, const char *message) {
  if (home == NULL || !home->dialog_open) {
    return;
  }
  copy_text(home->dialog_error, sizeof(home->dialog_error),
            message != NULL ? message : "");
  bd_home_damage_all(home);
}

const char *bd_home_dialog_error(const bd_home *home) {
  return home != NULL ? home->dialog_error : "";
}

void bd_home_cancel_dialog(bd_home *home) {
  if (home == NULL || !home->dialog_open) {
    return;
  }
  home->dialog_result = BD_HOME_DIALOG_RESULT_CANCEL;
  home->dialog_error[0] = '\0';
  home->dialog_from = bd_home_dialog_progress(home);
  home->dialog_open = false;
  home->dialog_target = false;
  home->dialog_anim = true;
  home->dialog_start = home->now_ms;
  bd_home_damage_all(home);
}

bool bd_home_confirm_dialog(bd_home *home) {
  if (home == NULL || !home->dialog_open) {
    return false;
  }
  if (home->dialog_focus == 0) {
    bd_home_cancel_dialog(home);
    return false;
  }
  int kind = home->dialog_kind;
  int item = home->dialog_item;
  if (kind == BD_HOME_DIALOG_END_APP && (item < 0 || item >= home->count)) {
    bd_home_cancel_dialog(home);
    return false;
  }
  if (kind == BD_HOME_DIALOG_BACKGROUND_FULL &&
      home->dialog_pending_id[0] == '\0') {
    bd_home_cancel_dialog(home);
    return false;
  }
  home->dialog_result = BD_HOME_DIALOG_RESULT_CONFIRM;
  home->dialog_error[0] = '\0';
  home->dialog_from = bd_home_dialog_progress(home);
  home->dialog_open = false;
  home->dialog_target = false;
  home->dialog_anim = true;
  home->dialog_start = home->now_ms;
  if (kind == BD_HOME_DIALOG_END_APP) {
    home->closing_active = true;
    home->closing_item = item;
    home->closing_start = home->now_ms;
  }
  bd_home_damage_all(home);
  return kind == BD_HOME_DIALOG_END_APP || kind == BD_HOME_DIALOG_BACKGROUND_FULL;
}

bool bd_home_dialog_open(const bd_home *home) {
  return home != NULL && home->dialog_open;
}

bd_home_dialog_result bd_home_take_dialog_result(bd_home *home) {
  if (home == NULL) {
    return BD_HOME_DIALOG_RESULT_NONE;
  }
  bd_home_dialog_result result = (bd_home_dialog_result)home->dialog_result;
  home->dialog_result = BD_HOME_DIALOG_RESULT_NONE;
  return result;
}

static bool quick_adjust(bd_home *home, int delta) {
  if (home->quick_sel < home->running_count) {
    return false;
  }
  int option = home->quick_sel - home->running_count;
  if (option != 0 && option != 1) {
    return false;
  }
  if (home->quick_backend.adjust == NULL) {
    return false;
  }
  bd_home_quick_control control = option == 0 ? BD_HOME_QUICK_BRIGHTNESS
                                              : BD_HOME_QUICK_VOLUME;
  int value = -1;
  if (!home->quick_backend.adjust(home->quick_backend.user, control,
                                  delta * 10, &value)) {
    return false;
  }
  value = clamp_int(value, 0, 100);
  if (option == 0) {
    home->brightness = value;
  } else {
    home->volume = value;
  }
  damage_quick_panel(home);
  return true;
}

static bool quick_toggle(bd_home *home, bd_home_quick_control control) {
  if (home->quick_backend.toggle == NULL) {
    return false;
  }
  bool enabled = control == BD_HOME_QUICK_WIFI
                     ? home->status.wifi_known && home->status.wifi
                     : false;
  if (!home->quick_backend.toggle(home->quick_backend.user, control,
                                  &enabled)) {
    return false;
  }
  if (control == BD_HOME_QUICK_WIFI) {
    home->status.wifi_known = true;
    home->status.wifi = enabled;
  }
  damage_quick_panel(home);
  return true;
}

static bool quick_option_toggle(bd_home *home) {
  if (home->quick_sel < home->running_count) {
    return false;
  }
  int option = home->quick_sel - home->running_count;
  switch (option) {
    case 0:
    case 1:
      return quick_adjust(home, 1);
    case 2:
      return quick_toggle(home, BD_HOME_QUICK_WIFI);
    case 3:
      return quick_toggle(home, BD_HOME_QUICK_BLUETOOTH);
    default:
      return false;
  }
}

bool bd_home_input(bd_home *home, bd_home_action action) {
  if (home == NULL || home->closing_active) {
    return false;
  }
  if (home->launch != BD_HOME_LAUNCH_NONE) {
    return false;
  }
  if (home->dialog_visible) {
    if (home->dialog_anim && !home->dialog_open) {
      return false;
    }
    switch (action) {
      case BD_HOME_ACTION_LEFT:
      case BD_HOME_ACTION_RIGHT:
        home->dialog_focus = home->dialog_focus == 0 ? 1 : 0;
        bd_home_damage_all(home);
        return true;
      case BD_HOME_ACTION_ACCEPT:
        bd_home_confirm_dialog(home);
        return true;
      case BD_HOME_ACTION_CANCEL:
        bd_home_cancel_dialog(home);
        return true;
      default:
        return false;
    }
  }
  if (home->quick_open || home->quick_anim) {
    if (home->quick_anim && !home->quick_open) {
      return false;
    }
    int rows = home->running_count + 4;
    switch (action) {
      case BD_HOME_ACTION_UP:
        if (home->quick_sel > 0) {
          --home->quick_sel;
          quick_scroll_to_selection(home);
          damage_quick_panel(home);
        }
        return true;
      case BD_HOME_ACTION_DOWN:
        if (home->quick_sel + 1 < rows) {
          ++home->quick_sel;
          quick_scroll_to_selection(home);
          damage_quick_panel(home);
        }
        return true;
      case BD_HOME_ACTION_LEFT:
        return quick_adjust(home, -1);
      case BD_HOME_ACTION_RIGHT:
        return quick_adjust(home, 1);
      case BD_HOME_ACTION_ACCEPT:
        if (home->quick_sel < home->running_count) {
          const char *id = home->running[home->quick_sel].id;
          return bd_home_start_launch(home, id);
        }
        quick_option_toggle(home);
        return true;
      case BD_HOME_ACTION_END:
        if (home->quick_sel < home->running_count) {
          return bd_home_open_end_dialog(home,
                                         home->running[home->quick_sel].id);
        }
        return false;
      case BD_HOME_ACTION_CANCEL:
      case BD_HOME_ACTION_QUICK:
        return bd_home_close_quick(home);
      default:
        return false;
    }
  }

  switch (action) {
    case BD_HOME_ACTION_LEFT:
      return focus_move(home, -1, 0);
    case BD_HOME_ACTION_RIGHT:
      return focus_move(home, 1, 0);
    case BD_HOME_ACTION_UP:
      return focus_move(home, 0, -1);
    case BD_HOME_ACTION_DOWN:
      return focus_move(home, 0, 1);
    case BD_HOME_ACTION_ACCEPT: {
      if (home->focus < 0 || home->focus >= home->count) {
        return false;
      }
      return bd_home_start_launch(home, home->entries[home->focus].id);
    }
    case BD_HOME_ACTION_END: {
      if (home->focus < 0 || home->focus >= home->count) {
        return false;
      }
      const bd_home_entry *entry = &home->entries[home->focus];
      if (entry->running) {
        return bd_home_open_end_dialog(home, entry->id);
      }
      if ((entry->item.flags & BD_HOME_ITEM_LIVE) != 0) {
        home->live.playing = !home->live.playing;
        bd_home_damage_item(home, home->focus);
        if (home->hint_visible || home->hint_anim) {
          bd_home_damage_rect(
              home, bd_rect_make(0, home->height - BD_HOME_HINT_STRIP,
                                 home->width, BD_HOME_HINT_STRIP));
        }
        return true;
      }
      return false;
    }
    case BD_HOME_ACTION_QUICK:
      return bd_home_open_quick(home);
    case BD_HOME_ACTION_TAB_PREV:
      bd_home_set_tab(home, (home->tab + BD_HOME_TAB_COUNT - 1) % BD_HOME_TAB_COUNT);
      return true;
    case BD_HOME_ACTION_TAB_NEXT:
      bd_home_set_tab(home, (home->tab + 1) % BD_HOME_TAB_COUNT);
      return true;
    case BD_HOME_ACTION_RECENT: {
      if (home->running_count == 0) {
        return false;
      }
      int item = home->running[home->running_count - 1].item_index;
      if (item < 0 || item >= home->count) {
        return false;
      }
      center_on_item(home, item);
      return true;
    }
    case BD_HOME_ACTION_CANCEL:
    default:
      return false;
  }
}

void bd_home_set_animations(bd_home *home, bool enabled) {
  if (home == NULL || home->animations == enabled) {
    return;
  }
  home->animations = enabled;
  if (!enabled) {
    bd_home_damage_all(home);
  }
}

void bd_home_update(bd_home *home, uint64_t now_ms) {
  if (home == NULL) {
    return;
  }
  home->now_ms = now_ms;

  if (!home->animations) {
    if (home->pan_anim.active) {
      home->pan = home->pan_anim.to;
      home->pan_anim.active = false;
      bd_home_damage_all(home);
    }
    if (home->quick_anim) {
      home->quick_start = now_ms >= BD_HOME_PANEL_MS
                              ? now_ms - BD_HOME_PANEL_MS
                              : 0;
    }
    if (home->hint_anim) {
      home->hint_start =
          now_ms >= BD_HOME_PANEL_MS ? now_ms - BD_HOME_PANEL_MS : 0;
    }
    if (home->dialog_anim) {
      home->dialog_start =
          now_ms >= BD_HOME_DIALOG_MS ? now_ms - BD_HOME_DIALOG_MS : 0;
    }
    if (home->launch != BD_HOME_LAUNCH_NONE) {
      home->launch_start =
          now_ms >= BD_HOME_LAUNCH_MS ? now_ms - BD_HOME_LAUNCH_MS : 0;
    }
  }

  if (home->pan_anim.active) {
    float t = (float)(now_ms - home->pan_anim.start_ms) / (float)BD_HOME_PAN_MS;
    if (t >= 1.0f) {
      home->pan = home->pan_anim.to;
      home->pan_anim.active = false;
      bd_home_damage_all(home);
    } else {
      home->pan =
          bd_home_lerp_int(home->pan_anim.from, home->pan_anim.to,
                           bd_home_ease_wall(t));
      bd_home_damage_all(home);
    }
  }

  if (home->launch == BD_HOME_LAUNCH_OPEN) {
    if (!home->launch_complete) {
      uint64_t elapsed = now_ms >= home->launch_start
                             ? now_ms - home->launch_start
                             : 0;
      if (elapsed >= BD_HOME_LAUNCH_MS) {
        home->launch_complete = true;
        bd_home_damage_all(home);
      } else if (!home->launch_app_painted && elapsed >= 300) {
        home->launch_app_painted = true;
        bd_home_damage_all(home);
      } else {
        bd_rect card = bd_home_launch_rect(home);
        card.x -= 4;
        card.y -= 4;
        card.w += 8;
        card.h += 8;
        bd_home_damage_rect(home, card);
      }
    }
  } else if (home->launch == BD_HOME_LAUNCH_RETURN) {
    if (now_ms >= home->launch_start + BD_HOME_LAUNCH_MS) {
      home->launch = BD_HOME_LAUNCH_NONE;
      home->launch_complete = false;
      bd_home_damage_all(home);
    } else {
      bd_home_damage_all(home);
    }
  }

  if (home->quick_anim) {
    bd_rect panel = bd_home_quick_panel_rect(home);
    if (panel.x != home->quick_last_x) {
      int left = panel.x < home->quick_last_x ? panel.x : home->quick_last_x;
      bd_home_damage_rect(home, bd_rect_make(left - 2, 0, home->width - left + 2,
                                             home->height));
    }
    if (now_ms >= home->quick_start + BD_HOME_PANEL_MS) {
      home->quick_anim = false;
      home->quick_last_x = panel.x;
      if (!home->quick_open) {
        bd_home_damage_all(home);
      } else {
        damage_quick_panel(home);
      }
    }
  }

  if (home->hint_anim) {
    bd_home_damage_rect(home, bd_rect_make(0, home->height - BD_HOME_HINT_STRIP,
                                           home->width, BD_HOME_HINT_STRIP));
    if (now_ms >= home->hint_start + BD_HOME_PANEL_MS) {
      home->hint_anim = false;
      home->hint_visible = home->hint_target;
    }
  }

  if (home->dialog_anim) {
    bd_home_damage_all(home);
    if (now_ms >= home->dialog_start + BD_HOME_DIALOG_MS) {
      home->dialog_anim = false;
      home->dialog_visible = home->dialog_target;
      home->dialog_open = home->dialog_target;
      bd_home_damage_all(home);
    }
  }

  if (home->closing_active) {
    bd_home_damage_item(home, home->closing_item);
    if (now_ms >= home->closing_start + BD_HOME_CLOSE_MS) {
      int item = home->closing_item;
      home->closing_active = false;
      if (item >= 0 && item < home->count) {
        char id[BD_HOME_ID_MAX];
        copy_text(id, sizeof(id), home->entries[item].id);
        bd_home_clear_running(home, id);
      }
      bd_home_damage_all(home);
    }
  }
}

bool bd_home_needs_frame_until(const bd_home *home, uint64_t *until_ms) {
  if (home == NULL) {
    if (until_ms != NULL) {
      *until_ms = 0;
    }
    return false;
  }
  bool active = false;
  uint64_t until = home->now_ms;
  uint64_t ends[6];
  ends[0] = bd_home_pan_end(home);
  ends[1] = bd_home_launch_end(home);
  ends[2] = bd_home_quick_end(home);
  ends[3] = bd_home_hint_end(home);
  ends[4] = bd_home_dialog_end(home);
  ends[5] = bd_home_closing_end(home);
  for (size_t i = 0; i < sizeof(ends) / sizeof(ends[0]); ++i) {
    if (ends[i] > 0) {
      active = true;
      if (ends[i] > until) {
        until = ends[i];
      }
    }
  }
  if (bd_home_has_pending_images(home)) {
    active = true;
    uint64_t decode = home->now_ms + 16;
    if (decode > until) {
      until = decode;
    }
  }
  if (until < home->now_ms) {
    until = home->now_ms;
  }
  if (until_ms != NULL) {
    *until_ms = until;
  }
  return active;
}

void bd_home_draw(bd_home *home, bd_gfx *gfx) {
  if (home == NULL || gfx == NULL) {
    return;
  }
  if (home->quick_anim) {
    bd_rect panel = bd_home_quick_panel_rect(home);
    home->quick_last_x = panel.x;
  }
  bd_home_draw_all(home, gfx);
}

void bd_home_damage(bd_home *home) {
  if (home != NULL) {
    if (home->launch == BD_HOME_LAUNCH_OPEN) {
      home->launch_painted = false;
    }
    bd_home_damage_all(home);
  }
}

bd_rect bd_home_launch_rect(const bd_home *home) {
  bd_rect full = bd_rect_make(0, 0, home->width, home->height);
  if (home->launch == BD_HOME_LAUNCH_NONE) {
    return full;
  }
  float elapsed = (float)(home->now_ms - home->launch_start);
  float t = bd_home_clampf(elapsed / (float)BD_HOME_LAUNCH_MS, 0.0f, 1.0f);
  float eased = bd_home_ease_wall(t);
  bd_rect tile = bd_home_screen_rect(home, home->launch_item);
  if (home->launch == BD_HOME_LAUNCH_OPEN) {
    return bd_rect_make(
        bd_home_lerp_int(home->launch_from.x, full.x, eased),
        bd_home_lerp_int(home->launch_from.y, full.y, eased),
        bd_home_lerp_int(home->launch_from.w, full.w, eased),
        bd_home_lerp_int(home->launch_from.h, full.h, eased));
  }
  return bd_rect_make(
      bd_home_lerp_int(full.x, tile.x, eased),
      bd_home_lerp_int(full.y, tile.y, eased),
      bd_home_lerp_int(full.w, tile.w, eased),
      bd_home_lerp_int(full.h, tile.h, eased));
}
