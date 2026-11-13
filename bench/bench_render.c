#define _POSIX_C_SOURCE 200809L

#include <GLES2/gl2.h>

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "brickdesk/desktop.h"
#include "brickdesk/gfx.h"
#include "brickdesk/platform.h"
#include "brickdesk/plugin.h"
#include "brickdesk/theme.h"
#include "brickdesk/ui.h"
#include "brickdesk/wall.h"
#include "ini.h"

#ifndef BRICKDESK_ASSET_DIR
#define BRICKDESK_ASSET_DIR "assets"
#endif

#ifndef BRICKDESK_BACKEND_NAME
#define BRICKDESK_BACKEND_NAME "host"
#endif

#define BENCH_WIDTH 1024
#define BENCH_HEIGHT 768
#define BENCH_WALL_ITEMS 5000
#define BENCH_WALL_COLUMNS 71
#define BENCH_WALL_CELL 168
#define BENCH_WALL_GAP 6
#define BENCH_WALL_PITCH (BENCH_WALL_CELL + BENCH_WALL_GAP)
#define BENCH_MAX_SAMPLES 4000
#define BENCH_COVER_CACHE_BYTES (96u * 1024u * 1024u)
#define BENCH_TILE_SIZE 168
#define BENCH_TILE_X 696
#define BENCH_TILE_Y 530
#define BENCH_MAX_BUDGET_FAILURES 16
#define BENCH_BUDGET_TEXT 192

typedef enum bench_scenario {
  BENCH_IDLE = 0,
  BENCH_FOCUS,
  BENCH_PAN,
  BENCH_BLUR,
  BENCH_WALL5000,
  BENCH_PLUGIN15,
  BENCH_SETTINGS,
  BENCH_SCENARIO_COUNT
} bench_scenario;

static const char *const SCENARIO_NAMES[BENCH_SCENARIO_COUNT] = {
    "idle", "focus", "pan", "blur", "wall5000", "plugin15", "settings",
};

typedef struct bench_options {
  int frames;
  int idle_ms;
  const char *scenario;
  const char *json_path;
  const char *budget_path;
} bench_options;

typedef struct bench_world {
  bd_platform *platform;
  bd_gfx *gfx;
  bd_ui_fonts fonts;
  bool fonts_loaded;
  bd_wall *wall;
  bd_desktop *desktop;
  bd_texture *tile;
  uint8_t *tile_pixels;
  bd_plugin_ui settings;
  bool settings_parsed;
  bd_color bg;
  bd_color ink;
} bench_world;

typedef struct bench_result {
  const char *name;
  int frames;
  int presented;
  double samples[BENCH_MAX_SAMPLES];
  double draw_total;
  int draw_max;
  int64_t damage_total;
  int64_t damage_max;
  double blur_total;
  int blur_max;
  int visited_max;
  int visible_max;
  int total_items;
  int idle_ms;
  double idle_cpu_ms;
  long rss_peak_kb;
  long rss_steady_kb;
  bool peak_reset;
  size_t cache_bytes;
  size_t cache_budget_bytes;
  size_t atlas_bytes;
  size_t font_bytes;
  size_t fbo_bytes;
  double avg_ms;
  double p50_ms;
  double p95_ms;
  double max_ms;
} bench_result;

typedef struct bench_budgets {
  long long idle_frames_max;
  double idle_cpu_ms_max;
  long long focus_repainted_pixels_max;
  long long wall5000_visited_items_max;
  long long rss_peak_kb_max;
  long long texture_cache_slack_bytes;
  bool seen_idle_frames;
  bool seen_idle_cpu;
  bool seen_focus_pixels;
  bool seen_wall_items;
  bool seen_rss;
} bench_budgets;

typedef struct bench_budget_report {
  bool enabled;
  bool ok;
  int failures;
  char messages[BENCH_MAX_BUDGET_FAILURES][BENCH_BUDGET_TEXT];
} bench_budget_report;

static double now_ms(void) {
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (double)time.tv_sec * 1000.0 + (double)time.tv_nsec / 1000000.0;
}

static int compare_double(const void *a, const void *b) {
  double left = *(const double *)a;
  double right = *(const double *)b;
  return left < right ? -1 : (left > right ? 1 : 0);
}

static long read_status_kb(const char *key) {
  FILE *file = fopen("/proc/self/status", "r");
  if (file == NULL) {
    return -1;
  }
  char line[256];
  size_t length = strlen(key);
  long value = -1;
  while (fgets(line, sizeof(line), file) != NULL) {
    if (strncmp(line, key, length) == 0) {
      value = strtol(line + length, NULL, 10);
      break;
    }
  }
  fclose(file);
  return value;
}

static bool reset_peak_rss(void) {
  FILE *file = fopen("/proc/self/clear_refs", "w");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs("5\n", file) >= 0;
  if (fclose(file) != 0) {
    ok = false;
  }
  return ok;
}

static void begin_measurement(bench_result *result) {
  result->peak_reset = reset_peak_rss();
  long peak = read_status_kb("VmHWM:");
  if (peak <= 0) {
    peak = read_status_kb("VmRSS:");
  }
  result->rss_peak_kb = peak > 0 ? peak : 0;
  result->rss_steady_kb = result->rss_peak_kb;
}

static void sample_rss(bench_result *result) {
  long rss = read_status_kb("VmRSS:");
  if (rss > result->rss_peak_kb) {
    result->rss_peak_kb = rss;
  }
}

static void capture_memory(bench_result *result, const bench_world *world) {
  bd_gfx_memory_stats memory = bd_gfx_memory_get_stats(world->gfx);
  result->cache_bytes = memory.texture_cache_bytes;
  result->cache_budget_bytes = memory.texture_cache_budget_bytes;
  result->atlas_bytes = memory.glyph_atlas_bytes;
  result->font_bytes = memory.font_data_bytes;
  result->fbo_bytes = memory.fbo_bytes;
  result->rss_steady_kb = read_status_kb("VmRSS:");
  sample_rss(result);
  if (result->rss_steady_kb > result->rss_peak_kb) {
    result->rss_peak_kb = result->rss_steady_kb;
  }
}

static void metric_frame(bench_result *result, const bd_gfx_stats *stats,
                         double elapsed_ms) {
  if (result->frames >= BENCH_MAX_SAMPLES) {
    return;
  }
  result->samples[result->frames++] = elapsed_ms;
  if (stats->presented) {
    ++result->presented;
  }
  result->draw_total += (double)stats->draw_calls;
  if (stats->draw_calls > result->draw_max) {
    result->draw_max = stats->draw_calls;
  }
  result->damage_total += stats->damage_pixels;
  if (stats->damage_pixels > result->damage_max) {
    result->damage_max = stats->damage_pixels;
  }
  result->blur_total += (double)stats->blur_passes;
  if (stats->blur_passes > result->blur_max) {
    result->blur_max = stats->blur_passes;
  }
}

static void finalize_result(bench_result *result) {
  if (result->frames <= 0) {
    result->avg_ms = 0.0;
    result->p50_ms = 0.0;
    result->p95_ms = 0.0;
    result->max_ms = 0.0;
    return;
  }
  double total = 0.0;
  double *sorted = (double *)malloc((size_t)result->frames * sizeof(double));
  for (int i = 0; i < result->frames; ++i) {
    total += result->samples[i];
    if (sorted != NULL) {
      sorted[i] = result->samples[i];
    }
  }
  result->avg_ms = total / (double)result->frames;
  if (sorted != NULL) {
    qsort(sorted, (size_t)result->frames, sizeof(double), compare_double);
    result->p50_ms = sorted[(result->frames - 1) / 2];
    result->p95_ms = sorted[(int)((double)(result->frames - 1) * 0.95)];
    result->max_ms = sorted[result->frames - 1];
    free(sorted);
  } else {
    result->p50_ms = result->avg_ms;
    result->p95_ms = result->avg_ms;
    result->max_ms = result->avg_ms;
  }
}

static void draw_chrome(bench_world *world) {
  bd_color top = world->bg;
  top.a = 0.72f;
  bd_color bottom = world->bg;
  bottom.a = 0.14f;
  bd_gfx_blur_region_feathered(world->gfx, bd_rect_make(0, 0, BENCH_WIDTH, 76), 0,
                               top, bottom, BD_GRADIENT_VERTICAL,
                               BD_BLUR_EDGE_BOTTOM, 20);
  bd_gfx_fill_rect(world->gfx, bd_rect_make(0, 75, BENCH_WIDTH, 1),
                   bd_color_rgba(30, 26, 21, 30));

  bd_color hint_top = world->ink;
  hint_top.a = 0.60f;
  bd_color hint_bottom = world->ink;
  hint_bottom.a = 0.82f;
  bd_gfx_blur_region_feathered(
      world->gfx, bd_rect_make((BENCH_WIDTH - 640) / 2, 700, 640, 56), 16,
      hint_top, hint_bottom, BD_GRADIENT_VERTICAL, BD_BLUR_EDGE_TOP, 12);
}

static bool build_wall_items(bd_wall_item **out_items,
                             char (**out_titles)[40],
                             char (**out_subtitles)[40],
                             char paths[8][512]) {
  bd_wall_item *items =
      (bd_wall_item *)calloc(BENCH_WALL_ITEMS, sizeof(*items));
  char (*titles)[40] = (char (*)[40])calloc(BENCH_WALL_ITEMS, sizeof(*titles));
  char (*subtitles)[40] =
      (char (*)[40])calloc(BENCH_WALL_ITEMS, sizeof(*subtitles));
  if (items == NULL || titles == NULL || subtitles == NULL) {
    free(items);
    free(titles);
    free(subtitles);
    return false;
  }
  for (int i = 0; i < 8; ++i) {
    snprintf(paths[i], sizeof(paths[i]), "%s/covers/cover-%02d.png",
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
  for (int i = 0; i < BENCH_WALL_ITEMS; ++i) {
    int column = i % BENCH_WALL_COLUMNS;
    int row = i / BENCH_WALL_COLUMNS;
    snprintf(titles[i], sizeof(titles[i]), "应用 %04d", i);
    snprintf(subtitles[i], sizeof(subtitles[i]), "第 %d 行 · 示例条目", row + 1);
    items[i].rect = bd_rect_make(column * BENCH_WALL_PITCH,
                                 row * BENCH_WALL_PITCH, BENCH_WALL_CELL,
                                 BENCH_WALL_CELL);
    items[i].title = titles[i];
    items[i].subtitle = subtitles[i];
    items[i].badge = (i % 3 == 0) ? "GAME" : "APP";
    items[i].image_path = paths[i % 8];
    items[i].placeholder = COLORS[i % 8];
    items[i].progress = -1.0f;
  }
  *out_items = items;
  *out_titles = titles;
  *out_subtitles = subtitles;
  return true;
}

static bool ensure_custom_world(bench_world *world) {
  if (world->wall != NULL) {
    return true;
  }
  if (!bd_ui_fonts_load(&world->fonts, world->gfx)) {
    fprintf(stderr, "bench: failed to load fonts\n");
    return false;
  }
  world->fonts_loaded = true;

  bd_wall_style style;
  memset(&style, 0, sizeof(style));
  style.fonts = &world->fonts;
  style.accent = bd_accent_palette_from(bd_color_rgb(174, 67, 43), false);
  style.radius = 6;
  style.prefetch_pixels = BENCH_WALL_PITCH;
  world->wall = bd_wall_create(world->gfx, &style);
  if (world->wall == NULL) {
    return false;
  }

  bd_wall_item *items = NULL;
  char (*titles)[40] = NULL;
  char (*subtitles)[40] = NULL;
  char paths[8][512];
  if (!build_wall_items(&items, &titles, &subtitles, paths) ||
      !bd_wall_set_items(world->wall, items, BENCH_WALL_ITEMS)) {
    fprintf(stderr, "bench: failed to index the 5000-item wall\n");
    free(items);
    free(titles);
    free(subtitles);
    return false;
  }
  free(items);
  free(titles);
  free(subtitles);
  return true;
}

static bool ensure_desktop_world(bench_world *world) {
  if (world->desktop != NULL) {
    return true;
  }
  bd_desktop_theme theme;
  memset(&theme, 0, sizeof(theme));
  theme.ambient_baked = true;
  theme.accent = bd_color_rgb(224, 122, 95);
  world->desktop = bd_desktop_create(world->gfx, &theme);
  if (world->desktop == NULL) {
    fprintf(stderr, "bench: failed to create the desktop\n");
    return false;
  }
  return true;
}

static bool ensure_tile(bench_world *world) {
  if (world->tile != NULL) {
    return true;
  }
  world->tile_pixels = (uint8_t *)calloc(
      (size_t)BENCH_TILE_SIZE * (size_t)BENCH_TILE_SIZE, 4u);
  if (world->tile_pixels == NULL) {
    return false;
  }
  world->tile = bd_gfx_texture_create(world->gfx, BENCH_TILE_SIZE,
                                      BENCH_TILE_SIZE, BD_TEXTURE_LINEAR,
                                      world->tile_pixels);
  return world->tile != NULL;
}

static void fill_tile(bench_world *world, int frame) {
  for (int y = 0; y < BENCH_TILE_SIZE; ++y) {
    for (int x = 0; x < BENCH_TILE_SIZE; ++x) {
      uint8_t *pixel =
          world->tile_pixels +
          ((size_t)y * (size_t)BENCH_TILE_SIZE + (size_t)x) * 4u;
      pixel[0] = (uint8_t)(32 + ((x * 3 + frame * 9) & 127));
      pixel[1] = (uint8_t)(48 + ((y * 2 + frame * 5) & 127));
      pixel[2] = (uint8_t)(72 + (((x + y) / 2 + frame * 3) & 127));
      pixel[3] = 255;
    }
  }
}

static bool ensure_settings(bench_world *world) {
  if (world->settings_parsed) {
    return true;
  }
  static const char SETTINGS_JSON[] =
      "{\"type\":\"column\",\"padding\":20,\"gap\":14,\"children\":["
      "{\"type\":\"text\",\"text\":\"你好插件设置\",\"style\":\"title\"},"
      "{\"type\":\"text\",\"text\":\"这些控件由宿主渲染，插件只发送声明。\","
      "\"style\":\"caption\"},"
      "{\"type\":\"toggle\",\"id\":\"show_meta\",\"label\":\"显示副标题\","
      "\"value\":true},"
      "{\"type\":\"slider\",\"id\":\"pace\",\"label\":\"节奏\","
      "\"value\":3,\"min\":1,\"max\":5,\"step\":1},"
      "{\"type\":\"select\",\"id\":\"mode\",\"label\":\"模式\","
      "\"value\":\"calm\",\"options\":[\"calm\",\"bright\",\"night\"]},"
      "{\"type\":\"button\",\"id\":\"refresh\",\"label\":\"刷新\","
      "\"action\":\"refresh\"},"
      "{\"type\":\"text\",\"text\":\"控件由宿主渲染，事件回传插件。\","
      "\"style\":\"caption\"}]}";
  char error[256] = {0};
  if (!bd_plugin_ui_parse(SETTINGS_JSON, strlen(SETTINGS_JSON),
                          &world->settings, error, sizeof(error))) {
    fprintf(stderr, "bench: settings tree is invalid: %s\n", error);
    return false;
  }
  world->settings_parsed = true;
  return true;
}

static bd_gfx_stats draw_custom_frame(bench_world *world, int origin_x,
                                      int origin_y) {
  bd_gfx_begin_frame(world->gfx);
  bd_wall_begin_frame(world->wall, world->gfx,
                      bd_rect_make(0, 0, BENCH_WIDTH, BENCH_HEIGHT), origin_x,
                      origin_y);
  bd_gfx_fill_rect(world->gfx,
                   bd_rect_make(0, 0, BENCH_WIDTH, BENCH_HEIGHT), world->bg);
  bd_wall_draw(world->wall, world->gfx);
  draw_chrome(world);
  bd_gfx_end_frame(world->gfx);
  bd_gfx_stats stats = bd_gfx_get_stats(world->gfx);
  if (stats.presented) {
    bd_platform_present(world->platform);
  }
  glFinish();
  return stats;
}

static bd_gfx_stats draw_desktop_frame(bench_world *world, bool settings,
                                       bool tile) {
  bd_gfx_begin_frame(world->gfx);
  bd_desktop_draw(world->desktop, world->gfx);
  if (settings) {
    bd_desktop_draw_plugin_settings(world->desktop, world->gfx,
                                    &world->settings);
  }
  if (tile && world->tile != NULL) {
    bd_gfx_draw_texture_round(world->gfx, world->tile,
                              bd_rect_make(BENCH_TILE_X, BENCH_TILE_Y,
                                           BENCH_TILE_SIZE, BENCH_TILE_SIZE),
                              bd_color_rgba(255, 255, 255, 255), 6);
  }
  bd_gfx_end_frame(world->gfx);
  bd_gfx_stats stats = bd_gfx_get_stats(world->gfx);
  if (stats.presented) {
    bd_platform_present(world->platform);
  }
  glFinish();
  return stats;
}

static void wait_for_event(bench_world *world, int timeout_ms) {
  bd_event event;
  if (bd_platform_wait_event(world->platform, &event, timeout_ms) < 0) {
    return;
  }
  (void)world;
}

static void warmup_custom(bench_world *world) {
  const int focus_base = BENCH_WALL_COLUMNS * 2;
  bd_wall_set_focus(world->wall, focus_base + 1);
  for (int i = 0; i < 8; ++i) {
    draw_custom_frame(world, 0, 0);
  }
  for (int tries = 0; tries < 300 && bd_wall_has_pending_images(world->wall);
       ++tries) {
    draw_custom_frame(world, 0, 0);
    wait_for_event(world, 5);
  }
  for (int i = 0; i < 8; ++i) {
    draw_custom_frame(world, 0, 0);
  }
}

static void warmup_desktop(bench_world *world) {
  int quiet = 0;
  for (int tries = 0; tries < 400 && quiet < 2; ++tries) {
    bd_gfx_stats stats = draw_desktop_frame(world, false, false);
    if (!bd_desktop_has_pending_images(world->desktop) && !stats.presented) {
      ++quiet;
    } else {
      quiet = 0;
    }
    wait_for_event(world, 5);
  }
}

static bool run_idle(bench_world *world, int idle_ms, bench_result *result) {
  if (!ensure_desktop_world(world)) {
    return false;
  }
  warmup_desktop(world);
  begin_measurement(result);
  result->idle_ms = idle_ms;
  result->rss_steady_kb = result->rss_peak_kb;

  double cpu_start = (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
  uint32_t start = bd_platform_ticks_ms();
  bool dirty = false;
  for (;;) {
    uint32_t elapsed = bd_platform_ticks_ms() - start;
    if (elapsed >= (uint32_t)idle_ms) {
      break;
    }
    int remaining = idle_ms - (int)elapsed;
    bd_event event;
    int waiting = bd_platform_wait_event(world->platform, &event, remaining);
    if (waiting < 0) {
      return false;
    }
    if (waiting == 0) {
      break;
    }
    if (event.type == BD_EVENT_QUIT) {
      return false;
    }
    if (event.type == BD_EVENT_REDRAW) {
      dirty = true;
    }
    if (dirty) {
      bd_gfx_stats stats = draw_desktop_frame(world, false, false);
      if (stats.presented) {
        ++result->presented;
      }
      dirty = false;
    }
  }
  result->idle_cpu_ms =
      (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC - cpu_start;
  capture_memory(result, world);
  return true;
}

static bool run_custom(bench_world *world, bench_scenario scenario, int frames,
                       bench_result *result) {
  if (!ensure_custom_world(world)) {
    return false;
  }
  warmup_custom(world);
  begin_measurement(result);

  const int focus_base = BENCH_WALL_COLUMNS * 2;
  int origin_x = 0;
  int origin_y = 0;
  for (int frame = 0; frame < frames; ++frame) {
    switch (scenario) {
      case BENCH_FOCUS: {
        bd_wall_set_focus(world->wall, focus_base + frame % 2);
        break;
      }
      case BENCH_PAN:
        origin_x =
            -(int)(((frame + 1) * 12) % (BENCH_WALL_COLUMNS * BENCH_WALL_PITCH));
        origin_y =
            -(int)(((frame + 1) * 7) % (BENCH_WALL_COLUMNS * BENCH_WALL_PITCH));
        break;
      case BENCH_BLUR:
        bd_gfx_damage_rect(world->gfx,
                           bd_rect_make(0, 0, BENCH_WIDTH, 76));
        bd_gfx_damage_rect(
            world->gfx,
            bd_rect_make((BENCH_WIDTH - 640) / 2, 700, 640, 56));
        break;
      case BENCH_WALL5000:
        origin_x = 0;
        origin_y =
            -(int)((frame % BENCH_WALL_COLUMNS) * BENCH_WALL_PITCH);
        bd_gfx_damage_all(world->gfx);
        break;
      default:
        break;
    }

    double start = now_ms();
    bd_gfx_stats stats = draw_custom_frame(world, origin_x, origin_y);
    double elapsed = now_ms() - start;
    metric_frame(result, &stats, elapsed);
    bd_wall_stats wall_stats = bd_wall_get_stats(world->wall);
    if (wall_stats.visited_items > result->visited_max) {
      result->visited_max = wall_stats.visited_items;
    }
    if (wall_stats.visible_items > result->visible_max) {
      result->visible_max = wall_stats.visible_items;
    }
    result->total_items = wall_stats.total_items;
    sample_rss(result);
  }
  capture_memory(result, world);
  return true;
}

static bool run_plugin(bench_world *world, int frames, bench_result *result) {
  if (!ensure_desktop_world(world) || !ensure_tile(world)) {
    return false;
  }
  warmup_desktop(world);
  for (int i = 0; i < 8; ++i) {
    fill_tile(world, i);
    bd_gfx_texture_update(world->gfx, world->tile, BENCH_TILE_SIZE,
                          BENCH_TILE_SIZE, world->tile_pixels);
    bd_gfx_damage_rect(world->gfx,
                       bd_rect_make(BENCH_TILE_X, BENCH_TILE_Y, BENCH_TILE_SIZE,
                                    BENCH_TILE_SIZE));
    draw_desktop_frame(world, false, true);
  }
  begin_measurement(result);

  for (int frame = 0; frame < frames; ++frame) {
    double start = now_ms();
    fill_tile(world, frame);
    bd_gfx_texture_update(world->gfx, world->tile, BENCH_TILE_SIZE,
                          BENCH_TILE_SIZE, world->tile_pixels);
    bd_gfx_damage_rect(world->gfx,
                       bd_rect_make(BENCH_TILE_X, BENCH_TILE_Y, BENCH_TILE_SIZE,
                                    BENCH_TILE_SIZE));
    bd_gfx_stats stats = draw_desktop_frame(world, false, true);
    double elapsed = now_ms() - start;
    metric_frame(result, &stats, elapsed);
    sample_rss(result);
  }
  capture_memory(result, world);
  return true;
}

static bool run_settings(bench_world *world, int frames, bench_result *result) {
  if (!ensure_desktop_world(world) || !ensure_settings(world)) {
    return false;
  }
  warmup_desktop(world);
  for (int i = 0; i < 8; ++i) {
    bd_gfx_damage_all(world->gfx);
    draw_desktop_frame(world, true, false);
  }
  begin_measurement(result);

  for (int frame = 0; frame < frames; ++frame) {
    bd_gfx_damage_all(world->gfx);
    double start = now_ms();
    bd_gfx_stats stats = draw_desktop_frame(world, true, false);
    double elapsed = now_ms() - start;
    metric_frame(result, &stats, elapsed);
    sample_rss(result);
  }
  capture_memory(result, world);
  return true;
}

static bool parse_long_value(const char *value, long long *out) {
  if (value == NULL || value[0] == '\0') {
    return false;
  }
  errno = 0;
  char *end = NULL;
  long long parsed = strtoll(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0') {
    return false;
  }
  *out = parsed;
  return true;
}

static bool parse_double_value(const char *value, double *out) {
  if (value == NULL || value[0] == '\0') {
    return false;
  }
  errno = 0;
  char *end = NULL;
  double parsed = strtod(value, &end);
  if (errno != 0 || end == value || *end != '\0' || parsed < 0.0) {
    return false;
  }
  *out = parsed;
  return true;
}

static int budget_ini_handler(void *user, const char *section,
                              const char *name, const char *value,
                              int lineno) {
  (void)lineno;
  bench_budgets *budgets = (bench_budgets *)user;
  if (strcmp(section, "budgets") != 0) {
    return 1;
  }
  if (strcmp(name, "idle_frames_max") == 0) {
    budgets->seen_idle_frames =
        parse_long_value(value, &budgets->idle_frames_max);
    return budgets->seen_idle_frames ? 1 : 0;
  }
  if (strcmp(name, "idle_cpu_ms_max") == 0) {
    budgets->seen_idle_cpu = parse_double_value(value, &budgets->idle_cpu_ms_max);
    return budgets->seen_idle_cpu ? 1 : 0;
  }
  if (strcmp(name, "focus_repainted_pixels_max") == 0) {
    budgets->seen_focus_pixels =
        parse_long_value(value, &budgets->focus_repainted_pixels_max);
    return budgets->seen_focus_pixels ? 1 : 0;
  }
  if (strcmp(name, "wall5000_visited_items_max") == 0) {
    budgets->seen_wall_items =
        parse_long_value(value, &budgets->wall5000_visited_items_max);
    return budgets->seen_wall_items ? 1 : 0;
  }
  if (strcmp(name, "rss_peak_kb_max") == 0) {
    budgets->seen_rss = parse_long_value(value, &budgets->rss_peak_kb_max);
    return budgets->seen_rss ? 1 : 0;
  }
  if (strcmp(name, "texture_cache_slack_bytes") == 0) {
    return parse_long_value(value, &budgets->texture_cache_slack_bytes) ? 1 : 0;
  }
  return 0;
}

static bool load_budgets(const char *path, bench_budgets *budgets) {
  memset(budgets, 0, sizeof(*budgets));
  int result = ini_parse(path, budget_ini_handler, budgets);
  if (result < 0) {
    fprintf(stderr, "bench: cannot read budget file %s\n", path);
    return false;
  }
  if (result > 0) {
    fprintf(stderr, "bench: budget file %s has an error on line %d\n", path,
            result);
    return false;
  }
  if (!budgets->seen_idle_frames || !budgets->seen_idle_cpu ||
      !budgets->seen_focus_pixels || !budgets->seen_wall_items ||
      !budgets->seen_rss) {
    fprintf(stderr,
            "bench: budget file %s must set idle_frames_max, idle_cpu_ms_max, "
            "focus_repainted_pixels_max, wall5000_visited_items_max and "
            "rss_peak_kb_max\n",
            path);
    return false;
  }
  return true;
}

static void budget_failure(bench_budget_report *report, const char *format, ...) {
  if (report->failures >= BENCH_MAX_BUDGET_FAILURES) {
    report->ok = false;
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(report->messages[report->failures], BENCH_BUDGET_TEXT, format,
            args);
  va_end(args);
  ++report->failures;
  report->ok = false;
}

static const bench_result *find_result(const bench_result *results,
                                       int count, bench_scenario scenario) {
  for (int i = 0; i < count; ++i) {
    if (strcmp(results[i].name, SCENARIO_NAMES[scenario]) == 0) {
      return &results[i];
    }
  }
  return NULL;
}

static void check_budgets(const bench_budgets *budgets,
                          const bench_result *results, int count,
                          bench_budget_report *report) {
  memset(report, 0, sizeof(*report));
  report->enabled = true;
  report->ok = true;

  for (int i = 0; i < count; ++i) {
    const bench_result *result = &results[i];
    if (result->cache_budget_bytes > 0 &&
        (long long)result->cache_bytes >
            (long long)result->cache_budget_bytes +
                budgets->texture_cache_slack_bytes) {
      budget_failure(report,
                     "%s texture cache %zu B exceeds budget %zu B by %zu B",
                     result->name, result->cache_bytes,
                     result->cache_budget_bytes,
                     result->cache_bytes - result->cache_budget_bytes);
    }
  }

  const bench_result *idle = find_result(results, count, BENCH_IDLE);
  if (idle != NULL) {
    if ((long long)idle->presented > budgets->idle_frames_max) {
      budget_failure(report, "idle presented %d frame(s), limit %lld",
                     idle->presented, budgets->idle_frames_max);
    }
    if (idle->idle_cpu_ms > budgets->idle_cpu_ms_max) {
      budget_failure(report, "idle CPU %.3f ms, limit %.3f ms",
                     idle->idle_cpu_ms, budgets->idle_cpu_ms_max);
    }
    if ((long long)idle->rss_peak_kb > budgets->rss_peak_kb_max) {
      budget_failure(report, "idle RSS peak %ld KiB, limit %lld KiB",
                     idle->rss_peak_kb, budgets->rss_peak_kb_max);
    }
  }

  const bench_result *focus = find_result(results, count, BENCH_FOCUS);
  if (focus != NULL &&
      (long long)focus->damage_max > budgets->focus_repainted_pixels_max) {
    budget_failure(report,
                   "focus repainted %lld px in one frame, limit %lld px",
                   (long long)focus->damage_max,
                   budgets->focus_repainted_pixels_max);
  }

  const bench_result *wall = find_result(results, count, BENCH_WALL5000);
  if (wall != NULL &&
      (long long)wall->visited_max > budgets->wall5000_visited_items_max) {
    budget_failure(report, "wall5000 visited %d items, limit %lld",
                   wall->visited_max, budgets->wall5000_visited_items_max);
  }
}

static const char *scenario_name(bench_scenario scenario) {
  return scenario >= 0 && scenario < BENCH_SCENARIO_COUNT
             ? SCENARIO_NAMES[scenario]
             : "unknown";
}

static void print_human(const bench_world *world, const bench_result *results,
                        int count, const bench_budget_report *report) {
  bd_gl_info info;
  bd_platform_gl_info(world->platform, &info);
  printf("bench: schema=brickdesk-bench-v1 backend=%s renderer=%s\n",
         BRICKDESK_BACKEND_NAME, info.renderer != NULL ? info.renderer : "");
  for (int i = 0; i < count; ++i) {
    const bench_result *result = &results[i];
    double draw_avg = result->frames > 0
                          ? result->draw_total / (double)result->frames
                          : 0.0;
    double damage_avg = result->frames > 0
                            ? (double)result->damage_total /
                                  (double)result->frames
                            : 0.0;
    double blur_avg = result->frames > 0
                          ? result->blur_total / (double)result->frames
                          : 0.0;
    printf("bench: scenario=%s frames=%d presented=%d avg_ms=%.3f "
           "p50_ms=%.3f p95_ms=%.3f max_ms=%.3f draws_avg=%.1f "
           "draws_max=%d damage_px_avg=%.0f damage_px_max=%lld "
           "blur_avg=%.1f blur_max=%d",
           result->name, result->frames, result->presented, result->avg_ms,
           result->p50_ms, result->p95_ms, result->max_ms, draw_avg,
           result->draw_max, damage_avg, (long long)result->damage_max,
           blur_avg, result->blur_max);
    if (result->idle_ms > 0) {
      printf(" idle_ms=%d idle_cpu_ms=%.3f", result->idle_ms,
             result->idle_cpu_ms);
    }
    if (result->total_items > 0) {
      printf(" visited_max=%d/%d visible_max=%d", result->visited_max,
             result->total_items, result->visible_max);
    }
    printf(" rss_peak_kb=%ld rss_steady_kb=%ld cache_kb=%zu/%zu "
           "atlas_kb=%zu font_kb=%zu fbo_kb=%zu\n",
           result->rss_peak_kb, result->rss_steady_kb,
           result->cache_bytes / 1024u, result->cache_budget_bytes / 1024u,
           result->atlas_bytes / 1024u, result->font_bytes / 1024u,
           result->fbo_bytes / 1024u);
  }
  if (report->enabled) {
    printf("budget: ok=%d failures=%d\n", report->ok ? 1 : 0,
           report->failures);
    for (int i = 0; i < report->failures; ++i) {
      fprintf(stderr, "budget: FAIL %s\n", report->messages[i]);
    }
  }
  printf("bench: note=host timings use llvmpipe and glFinish; "
         "device is the frame-time reference\n");
}

static void json_string(FILE *out, const char *text) {
  fputc('"', out);
  if (text != NULL) {
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0'; ++cursor) {
      switch (*cursor) {
        case '"':
          fputs("\\\"", out);
          break;
        case '\\':
          fputs("\\\\", out);
          break;
        case '\n':
          fputs("\\n", out);
          break;
        case '\r':
          fputs("\\r", out);
          break;
        case '\t':
          fputs("\\t", out);
          break;
        default:
          if (*cursor < 0x20u) {
            fprintf(out, "\\u%04x", (unsigned)*cursor);
          } else {
            fputc((int)*cursor, out);
          }
          break;
      }
    }
  }
  fputc('"', out);
}

static bool write_json(const char *path, const bench_world *world,
                       const bench_options *options,
                       const bench_result *results, int count,
                       const bench_budget_report *report) {
  FILE *out = fopen(path, "w");
  if (out == NULL) {
    fprintf(stderr, "bench: cannot write %s\n", path);
    return false;
  }
  bd_gl_info info;
  bd_platform_gl_info(world->platform, &info);
  fprintf(out, "{\n");
  fprintf(out, "  \"schema\": \"brickdesk-bench-v1\",\n");
  fprintf(out, "  \"schema_version\": 1,\n");
  fprintf(out, "  \"backend\": \"%s\",\n", BRICKDESK_BACKEND_NAME);
  fprintf(out, "  \"width\": %d,\n", BENCH_WIDTH);
  fprintf(out, "  \"height\": %d,\n", BENCH_HEIGHT);
  fprintf(out, "  \"gl_renderer\": ");
  json_string(out, info.renderer != NULL ? info.renderer : "");
  fprintf(out, ",\n");
  fprintf(out, "  \"frames_requested\": %d,\n", options->frames);
  fprintf(out, "  \"idle_ms_requested\": %d,\n", options->idle_ms);
  fprintf(out, "  \"budget\": {\"enabled\": %s, \"ok\": %s, \"failures\": [",
          report->enabled ? "true" : "false", report->ok ? "true" : "false");
  for (int i = 0; i < report->failures; ++i) {
    if (i > 0) {
      fputs(", ", out);
    }
    json_string(out, report->messages[i]);
  }
  fprintf(out, "]},\n");
  fprintf(out, "  \"scenarios\": [\n");
  for (int i = 0; i < count; ++i) {
    const bench_result *result = &results[i];
    double draw_avg = result->frames > 0
                          ? result->draw_total / (double)result->frames
                          : 0.0;
    double damage_avg = result->frames > 0
                            ? (double)result->damage_total /
                                  (double)result->frames
                            : 0.0;
    double blur_avg = result->frames > 0
                          ? result->blur_total / (double)result->frames
                          : 0.0;
    fprintf(out, "    {\n");
    fprintf(out, "      \"name\": ");
    json_string(out, result->name);
    fprintf(out, ",\n");
    fprintf(out, "      \"frames\": %d,\n", result->frames);
    fprintf(out, "      \"presented\": %d,\n", result->presented);
    fprintf(out,
            "      \"frame_ms\": {\"avg\": %.3f, \"p50\": %.3f, \"p95\": %.3f, "
            "\"max\": %.3f},\n",
            result->avg_ms, result->p50_ms, result->p95_ms, result->max_ms);
    fprintf(out,
            "      \"draw_calls\": {\"avg\": %.2f, \"max\": %d, \"total\": "
            "%.0f},\n",
            draw_avg, result->draw_max, result->draw_total);
    fprintf(out,
            "      \"repainted_pixels\": {\"avg\": %.0f, \"max\": %lld, "
            "\"total\": %lld},\n",
            damage_avg, (long long)result->damage_max,
            (long long)result->damage_total);
    fprintf(out,
            "      \"blur_passes\": {\"avg\": %.2f, \"max\": %d, \"total\": "
            "%.0f},\n",
            blur_avg, result->blur_max, result->blur_total);
    fprintf(out,
            "      \"wall\": {\"visited_items_max\": %d, "
            "\"visible_items_max\": %d, \"total_items\": %d},\n",
            result->visited_max, result->visible_max, result->total_items);
    fprintf(out, "      \"idle\": {\"duration_ms\": %d, \"cpu_ms\": %.3f},\n",
            result->idle_ms, result->idle_cpu_ms);
    fprintf(out,
            "      \"memory\": {\"rss_peak_kb\": %ld, \"rss_steady_kb\": %ld, "
            "\"peak_reset\": %s, \"texture_cache_bytes\": %zu, "
            "\"texture_cache_budget_bytes\": %zu, \"glyph_atlas_bytes\": %zu, "
            "\"font_data_bytes\": %zu, \"fbo_bytes\": %zu}\n",
            result->rss_peak_kb, result->rss_steady_kb,
            result->peak_reset ? "true" : "false", result->cache_bytes,
            result->cache_budget_bytes, result->atlas_bytes,
            result->font_bytes, result->fbo_bytes);
    fprintf(out, "    }%s\n", i + 1 < count ? "," : "");
  }
  fprintf(out, "  ]\n}\n");
  if (fclose(out) != 0) {
    fprintf(stderr, "bench: failed to close %s\n", path);
    return false;
  }
  return true;
}

static void print_usage(FILE *stream) {
  fprintf(stream,
          "usage: bench_render [--frames N] [--scenario NAME] [--idle-ms N]\n"
          "                    [--json PATH] [--budget PATH]\n"
          "\n"
          "scenarios: idle, focus, pan, blur, wall5000, plugin15, settings, "
          "all\n"
          "defaults: --frames 120 --scenario all --idle-ms 1000\n");
}

int main(int argc, char **argv) {
  bench_options options;
  options.frames = 120;
  options.idle_ms = 1000;
  options.scenario = "all";
  options.json_path = NULL;
  options.budget_path = NULL;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      options.frames = atoi(argv[++i]);
      if (options.frames < 1) {
        options.frames = 1;
      }
      if (options.frames > BENCH_MAX_SAMPLES) {
        options.frames = BENCH_MAX_SAMPLES;
      }
    } else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
      options.scenario = argv[++i];
    } else if (strcmp(argv[i], "--idle-ms") == 0 && i + 1 < argc) {
      options.idle_ms = atoi(argv[++i]);
      if (options.idle_ms < 1) {
        options.idle_ms = 1;
      }
      if (options.idle_ms > 600000) {
        options.idle_ms = 600000;
      }
    } else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
      options.json_path = argv[++i];
    } else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc) {
      options.budget_path = argv[++i];
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_usage(stdout);
      return 0;
    } else {
      fprintf(stderr, "bench_render: unknown option %s\n", argv[i]);
      print_usage(stderr);
      return 2;
    }
  }

  bench_budgets budgets;
  if (options.budget_path != NULL &&
      !load_budgets(options.budget_path, &budgets)) {
    return 2;
  }

  bench_world world;
  memset(&world, 0, sizeof(world));
  bd_platform_config platform_config;
  memset(&platform_config, 0, sizeof(platform_config));
  platform_config.title = "brickdesk bench";
  platform_config.width = BENCH_WIDTH;
  platform_config.height = BENCH_HEIGHT;
  platform_config.hidden = true;
  world.platform = bd_platform_create(&platform_config);
  if (world.platform == NULL) {
    return 1;
  }
  world.gfx = bd_gfx_create(BENCH_WIDTH, BENCH_HEIGHT);
  if (world.gfx == NULL) {
    bd_platform_destroy(world.platform);
    return 1;
  }
  bd_gfx_image_cache_set_budget(world.gfx, BENCH_COVER_CACHE_BYTES);
  world.bg = bd_color_rgb(242, 237, 227);
  world.ink = bd_color_rgb(30, 26, 21);

  bench_result *results = (bench_result *)calloc(
      BENCH_SCENARIO_COUNT, sizeof(*results));
  if (results == NULL) {
    bd_gfx_destroy(world.gfx);
    bd_platform_destroy(world.platform);
    return 1;
  }

  int result_count = 0;
  bool success = true;
  for (int scenario = 0; scenario < BENCH_SCENARIO_COUNT && success;
       ++scenario) {
    const char *name = scenario_name((bench_scenario)scenario);
    bool selected = strcmp(options.scenario, "all") == 0 ||
                    strcmp(options.scenario, name) == 0 ||
                    (scenario == BENCH_WALL5000 &&
                     strcmp(options.scenario, "full") == 0);
    if (!selected) {
      continue;
    }
    bench_result *result = &results[result_count++];
    memset(result, 0, sizeof(*result));
    result->name = name;
    if (scenario == BENCH_IDLE) {
      success = run_idle(&world, options.idle_ms, result);
    } else if (scenario == BENCH_PLUGIN15) {
      success = run_plugin(&world, options.frames, result);
    } else if (scenario == BENCH_SETTINGS) {
      success = run_settings(&world, options.frames, result);
    } else {
      success = run_custom(&world, (bench_scenario)scenario, options.frames,
                           result);
    }
    if (success) {
      finalize_result(result);
    }
  }

  if (!success) {
    fprintf(stderr, "bench: scenario setup or rendering failed\n");
  }

  bench_budget_report report;
  memset(&report, 0, sizeof(report));
  report.ok = true;
  if (success && options.budget_path != NULL) {
    check_budgets(&budgets, results, result_count, &report);
  }

  if (success) {
    print_human(&world, results, result_count, &report);
    if (options.json_path != NULL &&
        !write_json(options.json_path, &world, &options, results, result_count,
                    &report)) {
      success = false;
    }
    if (options.budget_path != NULL && !report.ok) {
      success = false;
    }
  }

  if (world.tile != NULL) {
    bd_gfx_texture_destroy(world.gfx, world.tile);
  }
  free(world.tile_pixels);
  if (world.wall != NULL) {
    bd_wall_destroy(world.wall);
  }
  if (world.fonts_loaded) {
    bd_ui_fonts_free(&world.fonts);
  }
  if (world.desktop != NULL) {
    bd_desktop_destroy(world.desktop);
  }
  free(results);
  bd_gfx_destroy(world.gfx);
  bd_platform_destroy(world.platform);
  return success ? 0 : 3;
}
