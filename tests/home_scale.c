#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "brickdesk/config.h"
#include "brickdesk/gfx.h"
#include "brickdesk/home.h"
#include "brickdesk/platform.h"
#include "brickdesk/theme.h"

#define TEST_WIDTH 1024
#define TEST_HEIGHT 768
#define BIG_COUNT 5000

static double now_ms(void) {
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (double)time.tv_sec * 1000.0 + (double)time.tv_nsec / 1000000.0;
}

static void draw_frame(bd_platform *platform, bd_gfx *gfx, bd_home *home,
                       uint64_t now, bool full_damage) {
  bd_gfx_begin_frame(gfx);
  bd_home_update(home, now);
  if (full_damage) {
    bd_home_damage(home);
  }
  bd_home_draw(home, gfx);
  bd_gfx_end_frame(gfx);
  if (bd_gfx_get_stats(gfx).presented) {
    bd_platform_present(platform);
  }
}

static void settle(bd_platform *platform, bd_gfx *gfx, bd_home *home,
                   uint64_t now) {
  int quiet = 0;
  for (int tries = 0; tries < 500 && quiet < 2; ++tries) {
    draw_frame(platform, gfx, home, now, true);
    if (bd_home_has_pending_images(home)) {
      quiet = 0;
    } else {
      ++quiet;
    }
    bd_event event;
    bd_platform_wait_event(platform, &event, 5);
  }
}


static void span_for_index(int index, int *w, int *h) {
  switch (index % 24) {
    case 0:
      *w = 3;
      *h = 2;
      break;
    case 3:
    case 7:
    case 12:
    case 17:
    case 21:
      *w = 2;
      *h = 2;
      break;
    case 5:
    case 15:
      *w = 1;
      *h = 2;
      break;
    case 9:
    case 14:
    case 19:
      *w = 2;
      *h = 1;
      break;
    default:
      *w = 1;
      *h = 1;
      break;
  }
}

static bool build_big_items(bd_home_item **out_items, char (**out_ids)[32],
                            char (**out_titles)[32]) {
  bd_home_item *items = (bd_home_item *)calloc(BIG_COUNT, sizeof(*items));
  char (*ids)[32] = (char (*)[32])calloc(BIG_COUNT, sizeof(*ids));
  char (*titles)[32] = (char (*)[32])calloc(BIG_COUNT, sizeof(*titles));
  static char paths[8][512];
  if (items == NULL || ids == NULL || titles == NULL) {
    free(items);
    free(ids);
    free(titles);
    return false;
  }
  for (int i = 0; i < 8; ++i) {
    snprintf(paths[i], sizeof(paths[i]), "%s/covers/cover-%02d.png",
             BRICKDESK_ASSET_DIR, i + 1);
  }
  for (int i = 0; i < BIG_COUNT; ++i) {
    int w;
    int h;
    span_for_index(i, &w, &h);
    snprintf(ids[i], sizeof(ids[i]), "app-%04d", i);
    snprintf(titles[i], sizeof(titles[i]), "应用 %04d", i);
    items[i].id = ids[i];
    items[i].title = titles[i];
    items[i].subtitle = (i % 2 == 0) ? "演示条目 · 本地" : "游戏 · 已玩 3 小时";
    items[i].image_path = paths[i % 8];
    items[i].placeholder = bd_color_rgb(60 + (i % 8) * 12, 54, 48);
    items[i].span_w = w;
    items[i].span_h = h;
    items[i].flags = (i % 3 == 0 ? BD_HOME_ITEM_GAME : 0) |
                     (i % 7 == 0 ? BD_HOME_ITEM_FAVORITE : 0);
  }
  *out_items = items;
  *out_ids = ids;
  *out_titles = titles;
  return true;
}

static bool reference_layout(const bd_home_item *items, int count,
                             bd_rect *out) {
  uint8_t *occ = (uint8_t *)calloc((size_t)count + 4u, 1);
  if (occ == NULL) {
    return false;
  }
  int max_col = -1;
  for (int i = 0; i < count; ++i) {
    int w = items[i].span_w > 0 ? items[i].span_w : 1;
    int h = items[i].span_h > 0 ? items[i].span_h : 1;
    if ((items[i].flags & BD_HOME_ITEM_LIVE) != 0) {
      w = 2;
      h = 2;
    }
    if (w < 1) w = 1;
    if (w > 3) w = 3;
    if (h < 1) h = 1;
    if (h > 4) h = 4;
    int spot_col = -1;
    int spot_row = 0;
    for (int c = 0; c <= max_col && spot_col < 0; ++c) {
      for (int r = 0; r <= 4 - h; ++r) {
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
    uint8_t mask = (uint8_t)(((1u << h) - 1u) << spot_row);
    for (int x = spot_col; x < spot_col + w; ++x) {
      occ[x] |= mask;
    }
    if (spot_col + w - 1 > max_col) {
      max_col = spot_col + w - 1;
    }
    out[i] = bd_rect_make(spot_col * 193, spot_row * 193,
                          w * 188 + (w - 1) * 5, h * 188 + (h - 1) * 5);
  }
  free(occ);
  return true;
}

static bool build_small_items(bd_home_item *items, char paths[8][512]) {
  for (int i = 0; i < 8; ++i) {
    snprintf(paths[i], sizeof(paths[i]), "%s/covers/cover-%02d.png",
             BRICKDESK_ASSET_DIR, i + 1);
    memset(&items[i], 0, sizeof(items[i]));
    items[i].id = "small";
    items[i].title = "小格";
    items[i].subtitle = "导航测试";
    items[i].image_path = paths[i];
    items[i].placeholder = bd_color_rgb(70, 64, 56);
    items[i].span_w = 1;
    items[i].span_h = 1;
  }
  static char ids[8][16];
  for (int i = 0; i < 8; ++i) {
    snprintf(ids[i], sizeof(ids[i]), "small-%d", i);
    items[i].id = ids[i];
  }
  return true;
}

int main(void) {
  bd_platform_config config;
  config.title = "brickdesk home scale";
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
  bd_gfx_image_cache_set_budget(gfx, 96u * 1024u * 1024u);

  bd_color accent;
  const char *hex = bd_config_accent_hex("persimmon", true);
  if (hex == NULL || !bd_color_from_hex(hex, &accent)) {
    accent = bd_color_rgb(224, 122, 95);
  }
  bd_home_style style;
  memset(&style, 0, sizeof(style));
  style.accent_base = accent;
  style.blur_enabled = true;
  style.width = TEST_WIDTH;
  style.height = TEST_HEIGHT;
  bd_home *home = bd_home_create(gfx, &style);
  if (home == NULL) {
    fprintf(stderr, "home_scale: create failed\n");
    return 1;
  }

  bd_home_item *items = NULL;
  char (*ids)[32] = NULL;
  char (*titles)[32] = NULL;
  if (!build_big_items(&items, &ids, &titles)) {
    return 1;
  }
  double layout_start = now_ms();
  bool ok = bd_home_set_items(home, items, BIG_COUNT);
  double layout_ms = now_ms() - layout_start;
  if (!ok) {
    fprintf(stderr, "home_scale: 5000-item set failed\n");
    return 1;
  }
  bd_home_stats stats = bd_home_get_stats(home);
  int wall_width = stats.wall_width;
  int free_cells = stats.free_cells;

  bd_rect *rects = (bd_rect *)calloc(BIG_COUNT, sizeof(bd_rect));
  if (rects == NULL) {
    return 1;
  }
  for (int i = 0; i < BIG_COUNT; ++i) {
    if (!bd_home_item_rect(home, i, &rects[i])) {
      fprintf(stderr, "home_scale: missing rect %d\n", i);
      return 1;
    }
  }

  /* Rebuilding the same ordered list must produce the same coordinates. */
  if (!bd_home_set_items(home, items, BIG_COUNT)) {
    fprintf(stderr, "home_scale: rebuild failed\n");
    return 1;
  }
  for (int i = 0; i < BIG_COUNT; ++i) {
    bd_rect again;
    bd_home_item_rect(home, i, &again);
    if (again.x != rects[i].x || again.y != rects[i].y ||
        again.w != rects[i].w || again.h != rects[i].h) {
      fprintf(stderr, "home_scale: layout is not deterministic at %d\n", i);
      return 1;
    }
  }

  bd_rect *reference = (bd_rect *)calloc(BIG_COUNT, sizeof(bd_rect));
  if (reference == NULL || !reference_layout(items, BIG_COUNT, reference)) {
    return 1;
  }
  for (int i = 0; i < BIG_COUNT; ++i) {
    if (reference[i].x != rects[i].x || reference[i].y != rects[i].y ||
        reference[i].w != rects[i].w || reference[i].h != rects[i].h) {
      fprintf(stderr, "home_scale: layout differs from first-fit at %d\n", i);
      return 1;
    }
  }
  free(reference);

  /* Traverse the whole manifest with the viewport + one screen each side. */
  int query_max = 0;
  long long query_total = 0;
  int *found = (int *)calloc(BD_HOME_QUERY_MAX, sizeof(int));
  if (found == NULL) {
    return 1;
  }
  for (int i = 0; i < BIG_COUNT; ++i) {
    bd_rect view = bd_rect_make(rects[i].x - 1024, 0, rects[i].w + 2048,
                                TEST_HEIGHT);
    int count = bd_home_query_items(home, view, found, BD_HOME_QUERY_MAX);
    if (count > query_max) {
      query_max = count;
    }
    query_total += count;
    bool self = false;
    for (int j = 0; j < count; ++j) {
      if (found[j] == i) {
        self = true;
        break;
      }
    }
    if (!self) {
      fprintf(stderr, "home_scale: index missed item %d\n", i);
      return 1;
    }
  }

  uint64_t now = 1000;
  int focus_visited = 0;
  int focus_visible = 0;
  const int probes[] = {0, 1000, 2500, 4000, 4999};
  for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); ++p) {
    bd_home_update(home, now);
    bd_home_set_focus(home, probes[p]);
    now += 300;
    settle(platform, gfx, home, now);
    bd_home_stats probe = bd_home_get_stats(home);
    if (probe.visited_items > focus_visited) {
      focus_visited = probe.visited_items;
    }
    if (probe.visible_items > focus_visible) {
      focus_visible = probe.visible_items;
    }
  }

  /* Walk focus through the wall and check row memory stays in range. */
  int moves = 0;
  bd_home_update(home, now);
  bd_home_set_focus(home, 0);
  now += 300;
  for (int step = 0; step < 240; ++step) {
    bd_home_action action;
    switch ((step / 4) % 4) {
      case 0:
        action = BD_HOME_ACTION_RIGHT;
        break;
      case 1:
        action = BD_HOME_ACTION_DOWN;
        break;
      case 2:
        action = BD_HOME_ACTION_RIGHT;
        break;
      default:
        action = BD_HOME_ACTION_UP;
        break;
    }
    if (bd_home_input(home, action)) {
      ++moves;
    }
    now += 250;
    bd_home_update(home, now);
    int focus = bd_home_focus(home);
    if (focus < 0 || focus >= BIG_COUNT) {
      fprintf(stderr, "home_scale: focus out of range\n");
      return 1;
    }
  }
  if (moves < 100) {
    fprintf(stderr, "home_scale: focus walk made only %d moves\n", moves);
    return 1;
  }
  settle(platform, gfx, home, now);

  /* Focus-only damage on a settled 4x2 wall. */
  bd_home *small = bd_home_create(gfx, &style);
  if (small == NULL) {
    return 1;
  }
  bd_home_item small_items[8];
  char small_paths[8][512];
  build_small_items(small_items, small_paths);
  if (!bd_home_set_items(small, small_items, 8)) {
    return 1;
  }
  bd_home_update(small, now);
  bd_home_set_focus(small, 5);
  now += 300;
  settle(platform, gfx, small, now);
  bd_gfx_damage_all(gfx);
  draw_frame(platform, gfx, small, now, true);
  bd_home_set_focus(small, 6);
  bd_home_update(small, now);
  draw_frame(platform, gfx, small, now, false);
  bd_image partial;
  if (!bd_gfx_read_image(gfx, &partial)) {
    return 1;
  }
  bd_gfx_stats partial_stats = bd_gfx_get_stats(gfx);
  bd_rect damage[8];
  int damage_count = bd_gfx_damage_rects(gfx, damage, 8);
  bd_gfx_damage_all(gfx);
  draw_frame(platform, gfx, small, now, true);
  bd_image full;
  if (!bd_gfx_read_image(gfx, &full)) {
    return 1;
  }
  bool identical =
      partial.width == full.width && partial.height == full.height &&
      memcmp(partial.pixels, full.pixels,
             (size_t)full.width * full.height * 4u) == 0;
  if (!identical) {
    int min_x = full.width;
    int min_y = full.height;
    int max_x = -1;
    int max_y = -1;
    long long diff = 0;
    for (int y = 0; y < full.height; ++y) {
      for (int x = 0; x < full.width; ++x) {
        size_t offset = ((size_t)y * (size_t)full.width + (size_t)x) * 4u;
        if (memcmp(partial.pixels + offset, full.pixels + offset, 4u) != 0) {
          ++diff;
          if (x < min_x) min_x = x;
          if (y < min_y) min_y = y;
          if (x > max_x) max_x = x;
          if (y > max_y) max_y = y;
        }
      }
    }
    fprintf(stderr, "home_scale: partial focus redraw differs from full redraw "
                    "(%lld px, bbox %d,%d..%d,%d, dims %dx%d vs %dx%d)\n",
            diff, min_x, min_y, max_x, max_y, partial.width, partial.height,
            full.width, full.height);
    bd_image_free(&partial);
    bd_image_free(&full);
    return 1;
  }
  bd_image_free(&partial);
  bd_image_free(&full);
  if (partial_stats.damage_pixels > 80000) {
    fprintf(stderr, "home_scale: focus move repainted %lld px\n",
            (long long)partial_stats.damage_pixels);
    return 1;
  }

  /* Blur-disabled fallback uses plain gradients only. */
  bd_home_set_blur_enabled(small, false);
  bd_home_damage(small);
  draw_frame(platform, gfx, small, now, true);
  bd_gfx_stats no_blur = bd_gfx_get_stats(gfx);
  bd_home_set_blur_enabled(small, true);
  bd_home_damage(small);
  draw_frame(platform, gfx, small, now, true);
  bd_gfx_stats with_blur = bd_gfx_get_stats(gfx);
  if (no_blur.blur_passes != 0 || with_blur.blur_passes <= 0) {
    fprintf(stderr, "home_scale: blur fallback failed (%d then %d passes)\n",
            no_blur.blur_passes, with_blur.blur_passes);
    return 1;
  }

  /* Incremental launch painting must match a full redraw at the same time. */
  bd_home_update(small, now);
  if (!bd_home_start_launch(small, "small-0")) {
    fprintf(stderr, "home_scale: small launch failed\n");
    return 1;
  }
  draw_frame(platform, gfx, small, now, true);
  now += 90;
  bd_home_update(small, now);
  draw_frame(platform, gfx, small, now, false);
  bd_image incremental;
  if (!bd_gfx_read_image(gfx, &incremental)) {
    return 1;
  }
  bd_home_damage(small);
  draw_frame(platform, gfx, small, now, true);
  bd_image redrawn;
  if (!bd_gfx_read_image(gfx, &redrawn)) {
    return 1;
  }
  int launch_diff = 0;
  int launch_diff_max = 0;
  for (int y = 0; y < incremental.height; ++y) {
    for (int x = 0; x < incremental.width; ++x) {
      size_t offset = ((size_t)y * (size_t)incremental.width + (size_t)x) * 4u;
      int delta = 0;
      for (int channel = 0; channel < 3; ++channel) {
        int value = abs((int)incremental.pixels[offset + channel] -
                        (int)redrawn.pixels[offset + channel]);
        if (value > delta) {
          delta = value;
        }
      }
      if (delta > 2) {
        ++launch_diff;
      }
      if (delta > launch_diff_max) {
        launch_diff_max = delta;
      }
    }
  }
  printf("scale: launch_incremental_diff_px=%d max=%d\n", launch_diff,
         launch_diff_max);
  if (launch_diff > (incremental.width * incremental.height) / 100 ||
      launch_diff_max > 64) {
    fprintf(stderr, "home_scale: incremental launch differs from full redraw\n");
    bd_image_free(&incremental);
    bd_image_free(&redrawn);
    return 1;
  }
  bd_image_free(&incremental);
  bd_image_free(&redrawn);

  /* Pan frame cost on a real focus move that crosses the safe edge. */
  bd_home_update(home, now);
  bd_home_set_focus(home, 0);
  now += 300;
  settle(platform, gfx, home, now);
  bool panning = false;
  for (int step = 0; step < 24 && !panning; ++step) {
    bd_home_input(home, BD_HOME_ACTION_RIGHT);
    now += 16;
    bd_home_update(home, now);
    panning = bd_home_get_stats(home).panning;
    if (!panning) {
      now += 300;
      bd_home_update(home, now);
    }
  }
  if (!panning) {
    fprintf(stderr, "home_scale: could not start a wall pan\n");
    return 1;
  }
  double pan_total = 0.0;
  double pan_max = 0.0;
  for (int frame = 0; frame < 14; ++frame) {
    now += 16;
    double start = now_ms();
    draw_frame(platform, gfx, home, now, false);
    double elapsed = now_ms() - start;
    pan_total += elapsed;
    if (elapsed > pan_max) {
      pan_max = elapsed;
    }
  }
  double pan_avg = pan_total / 14.0;
  now += 300;
  settle(platform, gfx, home, now);

  /* Launch frame cost. */
  int focus = bd_home_focus(home);
  char launch_id[32];
  snprintf(launch_id, sizeof(launch_id), "app-%04d", focus);
  bd_home_update(home, now);
  if (!bd_home_start_launch(home, launch_id)) {
    fprintf(stderr, "home_scale: launch start failed\n");
    return 1;
  }
  double launch_total = 0.0;
  double launch_max = 0.0;
  for (int frame = 0; frame < 20; ++frame) {
    now += 21;
    double start = now_ms();
    draw_frame(platform, gfx, home, now, false);
    double elapsed = now_ms() - start;
    launch_total += elapsed;
    if (elapsed > launch_max) {
      launch_max = elapsed;
    }
  }
  double launch_avg = launch_total / 20.0;

  bd_image_cache_stats images = bd_gfx_image_cache_get_stats(gfx);
  printf("scale: items=%d wall_w=%d free=%d layout_ms=%.3f query_max=%d "
         "query_avg=%.2f focus_visited_max=%d focus_visible_max=%d moves=%d\n",
         BIG_COUNT, wall_width, free_cells, layout_ms, query_max,
         (double)query_total / (double)BIG_COUNT, focus_visited, focus_visible,
         moves);
  printf("scale: focus_damage_px=%lld rects=%d identical=1\n",
         (long long)partial_stats.damage_pixels, damage_count);
  printf("scale: pan_avg_ms=%.3f pan_max_ms=%.3f launch_avg_ms=%.3f "
         "launch_max_ms=%.3f\n",
         pan_avg, pan_max, launch_avg, launch_max);
  printf("scale: cache_textures=%d resident_kb=%zu budget_kb=%zu pending=%d\n",
         images.textures, images.resident_bytes / 1024u,
         images.budget_bytes / 1024u, images.pending);

  if (query_max > 96) {
    fprintf(stderr, "home_scale: traversal touched %d items\n", query_max);
    return 1;
  }
  if (focus_visited > 96) {
    fprintf(stderr, "home_scale: draw visited %d items\n", focus_visited);
    return 1;
  }
  if (images.resident_bytes > images.budget_bytes) {
    fprintf(stderr, "home_scale: texture cache exceeded its budget\n");
    return 1;
  }

  bd_home_destroy(small);
  bd_home_destroy(home);
  free(found);
  free(rects);
  free(items);
  free(ids);
  free(titles);
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  return 0;
}
