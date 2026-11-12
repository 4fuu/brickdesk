#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "brickdesk/gfx.h"
#include "brickdesk/platform.h"
#include "brickdesk/theme.h"
#include "brickdesk/ui.h"
#include "brickdesk/wall.h"

#ifndef BRICKDESK_ASSET_DIR
#define BRICKDESK_ASSET_DIR "assets"
#endif

#define TEST_WIDTH 1024
#define TEST_HEIGHT 768
#define CELL 168
#define GAP 6
#define PITCH (CELL + GAP)
#define BIG_COUNT 5000

static double now_ms(void) {
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (double)time.tv_sec * 1000.0 + (double)time.tv_nsec / 1000000.0;
}

static bool build_items(int count, int columns, bd_wall_item **out_items,
                        char (**out_titles)[40], char (**out_paths)[8][512]) {
  bd_wall_item *items = (bd_wall_item *)calloc((size_t)count, sizeof(*items));
  char (*titles)[40] = (char (*)[40])calloc((size_t)count, sizeof(*titles));
  char (*paths)[8][512] = (char (*)[8][512])calloc(1, sizeof(*paths));
  if (items == NULL || titles == NULL || paths == NULL) {
    free(items);
    free(titles);
    free(paths);
    return false;
  }
  for (int i = 0; i < 8; ++i) {
    snprintf((*paths)[i], sizeof((*paths)[i]), "%s/covers/cover-%02d.png",
             BRICKDESK_ASSET_DIR, i + 1);
  }
  static const bd_color COLORS[8] = {
      {109.0f / 255.0f, 122.0f / 255.0f, 99.0f / 255.0f, 1.0f},
      {76.0f / 255.0f, 92.0f / 255.0f, 112.0f / 255.0f, 1.0f},
      {44.0f / 255.0f, 46.0f / 255.0f, 44.0f / 255.0f, 1.0f},
      {111.0f / 255.0f, 115.0f / 255.0f, 123.0f / 255.0f, 1.0f},
      {123.0f / 255.0f, 90.0f / 255.0f, 140.0f / 255.0f, 1.0f},
      {93.0f / 255.0f, 124.0f / 255.0f, 90.0f / 255.0f, 1.0f},
      {176.0f / 255.0f, 138.0f / 255.0f, 78.0f / 255.0f, 1.0f},
      {63.0f / 255.0f, 133.0f / 255.0f, 135.0f / 255.0f, 1.0f},
  };
  for (int i = 0; i < count; ++i) {
    int column = i % columns;
    int row = i / columns;
    snprintf(titles[i], sizeof(titles[i]), "应用 %04d", i);
    items[i].rect = bd_rect_make(column * PITCH, row * PITCH, CELL, CELL);
    items[i].title = titles[i];
    items[i].subtitle = "可绘制条目";
    items[i].badge = i % 3 == 0 ? "GAME" : "APP";
    items[i].image_path = (*paths)[i % 8];
    items[i].placeholder = COLORS[i % 8];
    items[i].progress = -1.0f;
  }
  *out_items = items;
  *out_titles = titles;
  *out_paths = paths;
  return true;
}

static void draw_wall(bd_platform *platform, bd_gfx *gfx, bd_wall *wall,
                      int origin_x, int origin_y) {
  bd_gfx_damage_all(gfx);
  bd_gfx_begin_frame(gfx);
  bd_wall_begin_frame(wall, gfx, bd_rect_make(0, 0, TEST_WIDTH, TEST_HEIGHT),
                      origin_x, origin_y);
  bd_gfx_fill_rect(gfx, bd_rect_make(0, 0, TEST_WIDTH, TEST_HEIGHT),
                   bd_color_rgb(242, 237, 227));
  bd_wall_draw(wall, gfx);
  bd_gfx_end_frame(gfx);
  if (bd_gfx_get_stats(gfx).presented) {
    bd_platform_present(platform);
  }
}

int main(void) {
  bd_platform_config config;
  config.title = "brickdesk wall scale";
  config.width = TEST_WIDTH;
  config.height = TEST_HEIGHT;
  config.hidden = true;
  bd_platform *platform = bd_platform_create(&config);
  if (platform == NULL) {
    return 1;
  }
  bd_gfx *gfx = bd_gfx_create(TEST_WIDTH, TEST_HEIGHT);
  if (gfx == NULL) {
    bd_platform_destroy(platform);
    return 1;
  }
  bd_ui_fonts fonts;
  if (!bd_ui_fonts_load(&fonts, gfx)) {
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }

  bd_wall_style style;
  memset(&style, 0, sizeof(style));
  style.fonts = &fonts;
  style.accent = bd_accent_palette_from(bd_color_rgb(174, 67, 43), false);
  style.radius = 6;
  style.prefetch_pixels = PITCH;
  bd_wall *wall = bd_wall_create(gfx, &style);
  if (wall == NULL) {
    bd_ui_fonts_free(&fonts);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }

  bd_wall_item *small_items = NULL;
  char (*small_titles)[40] = NULL;
  char (*small_paths)[8][512] = NULL;
  if (!build_items(100, 10, &small_items, &small_titles, &small_paths) ||
      !bd_wall_set_items(wall, small_items, 100)) {
    fprintf(stderr, "scale: small wall setup failed\n");
    return 1;
  }
  draw_wall(platform, gfx, wall, 0, 0);
  bd_wall_stats small = bd_wall_get_stats(wall);
  free(small_items);
  free(small_titles);
  free(small_paths);

  bd_wall_item *items = NULL;
  char (*titles)[40] = NULL;
  char (*paths)[8][512] = NULL;
  if (!build_items(BIG_COUNT, 71, &items, &titles, &paths) ||
      !bd_wall_set_items(wall, items, BIG_COUNT)) {
    fprintf(stderr, "scale: big wall setup failed\n");
    return 1;
  }
  free(items);
  free(titles);
  free(paths);

  draw_wall(platform, gfx, wall, 0, 0);
  bd_wall_stats big = bd_wall_get_stats(wall);

  /* Rapid pans must cancel queued work outside the new prefetch ring. */
  for (int frame = 0; frame < 6; ++frame) {
    draw_wall(platform, gfx, wall, -frame * 900, -frame * 420);
  }
  bd_image_cache_stats rapid = bd_gfx_image_cache_get_stats(gfx);
  printf("scale: rapid_pan_pending=%d textures=%d\n", rapid.pending, rapid.textures);
  if (rapid.pending > 64) {
    fprintf(stderr, "scale: image queue was not cancelled on rapid pans (%d)\n",
            rapid.pending);
    return 1;
  }

  for (int tries = 0; tries < 300 && bd_wall_has_pending_images(wall); ++tries) {
    draw_wall(platform, gfx, wall, 0, 0);
    bd_event event;
    bd_platform_wait_event(platform, &event, 5);
  }

  double start = now_ms();
  int frames = 10;
  for (int frame = 0; frame < frames; ++frame) {
    draw_wall(platform, gfx, wall, -frame * 17, -frame * 11);
  }
  double average_ms = (now_ms() - start) / (double)frames;
  bd_wall_stats panned = bd_wall_get_stats(wall);
  bd_image_cache_stats images = bd_gfx_image_cache_get_stats(gfx);

  printf("scale: small_items=%d visited=%d visible=%d buckets=%d\n",
         small.total_items, small.visited_items, small.visible_items,
         small.buckets_visited);
  printf("scale: big_items=%d visited=%d visible=%d buckets=%d pan_avg_ms=%.3f\n",
         big.total_items, big.visited_items, big.visible_items,
         big.buckets_visited, average_ms);
  printf("scale: panned_visited=%d panned_visible=%d textures=%d resident_kb=%zu "
         "budget_kb=%zu\n",
         panned.visited_items, panned.visible_items, images.textures,
         images.resident_bytes / 1024u, images.budget_bytes / 1024u);

  if (big.visited_items > big.total_items / 20) {
    fprintf(stderr, "scale: traversal touched too many items (%d/%d)\n",
            big.visited_items, big.total_items);
    return 1;
  }
  if (big.visited_items > small.visited_items + 8) {
    fprintf(stderr, "scale: visited count grew with manifest size (%d vs %d)\n",
            big.visited_items, small.visited_items);
    return 1;
  }
  if (panned.visited_items > 200) {
    fprintf(stderr, "scale: panned traversal is not viewport-bound (%d)\n",
            panned.visited_items);
    return 1;
  }
  if (images.resident_bytes > images.budget_bytes) {
    fprintf(stderr, "scale: texture cache exceeded its budget\n");
    return 1;
  }
  if (images.textures > 16) {
    fprintf(stderr, "scale: texture count grew with manifest size (%d)\n",
            images.textures);
    return 1;
  }

  /* Exercise the LRU budget: a tiny cache must stay under its cap. */
  bd_gfx_image_cache_set_budget(gfx, 256u * 1024u);
  draw_wall(platform, gfx, wall, 0, 0);
  bd_image_cache_stats capped = bd_gfx_image_cache_get_stats(gfx);
  printf("scale: capped_resident_kb=%zu budget_kb=%zu textures=%d\n",
         capped.resident_bytes / 1024u, capped.budget_bytes / 1024u,
         capped.textures);
  if (capped.resident_bytes > capped.budget_bytes) {
    fprintf(stderr, "scale: LRU did not respect the configured budget\n");
    return 1;
  }

  bd_wall_destroy(wall);
  bd_ui_fonts_free(&fonts);
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  return 0;
}
