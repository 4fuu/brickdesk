#define _POSIX_C_SOURCE 200809L

#include <GLES2/gl2.h>

#include <stdbool.h>
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

#define BENCH_WIDTH 1024
#define BENCH_HEIGHT 768
#define WALL_ITEMS 5000
#define WALL_COLUMNS 71
#define WALL_CELL 168
#define WALL_GAP 6
#define WALL_PITCH (WALL_CELL + WALL_GAP)
#define MAX_SAMPLES 1000

typedef enum bench_scenario {
  BENCH_FULL = 0,
  BENCH_FOCUS,
  BENCH_PAN,
  BENCH_BLUR,
  BENCH_SCENARIO_COUNT
} bench_scenario;

static const char *const SCENARIO_NAMES[BENCH_SCENARIO_COUNT] = {
    "full", "focus", "pan", "blur",
};

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

static void draw_chrome(bd_gfx *gfx, bd_color bg, bd_color ink) {
  bd_color top = bg;
  top.a = 0.72f;
  bd_color bottom = bg;
  bottom.a = 0.14f;
  bd_gfx_blur_region(gfx, bd_rect_make(0, 0, BENCH_WIDTH, 76), 0, top, bottom,
                     BD_GRADIENT_VERTICAL);
  bd_gfx_fill_rect(gfx, bd_rect_make(0, 75, BENCH_WIDTH, 1),
                   bd_color_rgba(30, 26, 21, 30));

  bd_color hint_top = ink;
  hint_top.a = 0.60f;
  bd_color hint_bottom = ink;
  hint_bottom.a = 0.82f;
  bd_gfx_blur_region(gfx,
                     bd_rect_make((BENCH_WIDTH - 640) / 2, 700, 640, 56), 16,
                     hint_top, hint_bottom, BD_GRADIENT_VERTICAL);
}

static bool build_wall_items(bd_wall_item **out_items, char (**out_titles)[40],
                             char (**out_subtitles)[40], char paths[8][512]) {
  bd_wall_item *items = (bd_wall_item *)calloc(WALL_ITEMS, sizeof(*items));
  char (*titles)[40] = (char (*)[40])calloc(WALL_ITEMS, sizeof(*titles));
  char (*subtitles)[40] = (char (*)[40])calloc(WALL_ITEMS, sizeof(*subtitles));
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
  for (int i = 0; i < WALL_ITEMS; ++i) {
    int column = i % WALL_COLUMNS;
    int row = i / WALL_COLUMNS;
    snprintf(titles[i], sizeof(titles[i]), "应用 %04d", i);
    snprintf(subtitles[i], sizeof(subtitles[i]), "第 %d 行 · 示例条目", row + 1);
    items[i].rect = bd_rect_make(column * WALL_PITCH, row * WALL_PITCH, WALL_CELL,
                                 WALL_CELL);
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

static void run_scenario(bench_scenario scenario, int frames, bd_platform *platform,
                         bd_gfx *gfx, bd_wall *wall, bd_color bg, bd_color ink) {
  double samples[MAX_SAMPLES];
  if (frames > MAX_SAMPLES) {
    frames = MAX_SAMPLES;
  }
  int origin_x = 0;
  int origin_y = 0;
  const int focus_base = WALL_COLUMNS * 2;
  int focus = focus_base;

  for (int warmup = 0; warmup < 8; ++warmup) {
    if (scenario == BENCH_FOCUS) {
      focus = focus_base + warmup % 2;
      bd_wall_set_focus(wall, focus);
    }
    bd_gfx_begin_frame(gfx);
    if (scenario == BENCH_FULL) {
      bd_gfx_damage_all(gfx);
    } else if (scenario == BENCH_BLUR) {
      bd_gfx_damage_rect(gfx, bd_rect_make(0, 0, BENCH_WIDTH, 76));
      bd_gfx_damage_rect(gfx, bd_rect_make((BENCH_WIDTH - 640) / 2, 700, 640, 56));
    }
    bd_wall_begin_frame(wall, gfx,
                        bd_rect_make(0, 0, BENCH_WIDTH, BENCH_HEIGHT), origin_x,
                        origin_y);
    bd_gfx_fill_rect(gfx, bd_rect_make(0, 0, BENCH_WIDTH, BENCH_HEIGHT), bg);
    bd_wall_draw(wall, gfx);
    draw_chrome(gfx, bg, ink);
    bd_gfx_end_frame(gfx);
    if (bd_gfx_get_stats(gfx).presented) {
      bd_platform_present(platform);
    }
    glFinish();
    bd_event event;
    bd_platform_wait_event(platform, &event, 2);
  }

  double total = 0.0;
  for (int frame = 0; frame < frames; ++frame) {
    switch (scenario) {
      case BENCH_FULL:
        bd_gfx_damage_all(gfx);
        break;
      case BENCH_FOCUS:
        focus = focus_base + frame % 2;
        bd_wall_set_focus(wall, focus);
        break;
      case BENCH_PAN:
        origin_x = -(int)((frame * 12) % (WALL_COLUMNS * WALL_PITCH));
        origin_y = -(int)((frame * 7) % (WALL_COLUMNS * WALL_PITCH));
        break;
      case BENCH_BLUR:
        bd_gfx_damage_rect(gfx, bd_rect_make(0, 0, BENCH_WIDTH, 76));
        bd_gfx_damage_rect(gfx, bd_rect_make((BENCH_WIDTH - 640) / 2, 700, 640, 56));
        break;
      default:
        break;
    }

    double start = now_ms();
    bd_gfx_begin_frame(gfx);
    bd_wall_begin_frame(wall, gfx,
                        bd_rect_make(0, 0, BENCH_WIDTH, BENCH_HEIGHT), origin_x,
                        origin_y);
    bd_gfx_fill_rect(gfx, bd_rect_make(0, 0, BENCH_WIDTH, BENCH_HEIGHT), bg);
    bd_wall_draw(wall, gfx);
    draw_chrome(gfx, bg, ink);
    bd_gfx_end_frame(gfx);
    if (bd_gfx_get_stats(gfx).presented) {
      bd_platform_present(platform);
    }
    glFinish();
    double elapsed = now_ms() - start;
    samples[frame] = elapsed;
    total += elapsed;
  }

  double sorted[MAX_SAMPLES];
  memcpy(sorted, samples, (size_t)frames * sizeof(double));
  qsort(sorted, (size_t)frames, sizeof(double), compare_double);
  double average = total / (double)frames;
  double p95 = sorted[(int)((double)(frames - 1) * 0.95)];
  bd_gfx_stats stats = bd_gfx_get_stats(gfx);
  bd_wall_stats wall_stats = bd_wall_get_stats(wall);
  printf("bench: scenario=%s frames=%d avg_ms=%.3f p95_ms=%.3f fps=%.1f "
         "damage_px=%lld draws=%d blur=%d visited=%d/%d visible=%d textures=%d "
         "resident_kb=%zu\n",
         SCENARIO_NAMES[scenario], frames, average, p95,
         average > 0.0 ? 1000.0 / average : 0.0, (long long)stats.damage_pixels,
         stats.draw_calls, stats.blur_passes, wall_stats.visited_items,
         wall_stats.total_items, wall_stats.visible_items,
         wall_stats.resident_textures, wall_stats.resident_texture_bytes / 1024u);
}

int main(int argc, char **argv) {
  int frames = 120;
  const char *scenario_name = "all";
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      frames = atoi(argv[++i]);
      if (frames < 1) {
        frames = 1;
      }
    } else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
      scenario_name = argv[++i];
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      printf("usage: bench_render [--frames N] [--scenario full|focus|pan|blur|all]\n");
      return 0;
    } else {
      fprintf(stderr, "bench_render: unknown option %s\n", argv[i]);
      return 2;
    }
  }

  bd_platform_config config;
  config.title = "brickdesk bench";
  config.width = BENCH_WIDTH;
  config.height = BENCH_HEIGHT;
  config.hidden = true;
  bd_platform *platform = bd_platform_create(&config);
  if (platform == NULL) {
    return 1;
  }
  bd_gfx *gfx = bd_gfx_create(BENCH_WIDTH, BENCH_HEIGHT);
  if (gfx == NULL) {
    bd_platform_destroy(platform);
    return 1;
  }
  bd_ui_fonts fonts;
  if (!bd_ui_fonts_load(&fonts, gfx)) {
    fprintf(stderr, "bench_render: failed to load fonts\n");
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }

  bd_wall_item *items = NULL;
  char (*titles)[40] = NULL;
  char (*subtitles)[40] = NULL;
  char paths[8][512];
  if (!build_wall_items(&items, &titles, &subtitles, paths)) {
    bd_ui_fonts_free(&fonts);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }

  bd_wall_style style;
  memset(&style, 0, sizeof(style));
  style.fonts = &fonts;
  style.accent = bd_accent_palette_from(bd_color_rgb(174, 67, 43), false);
  style.radius = 6;
  style.prefetch_pixels = WALL_PITCH;
  bd_wall *wall = bd_wall_create(gfx, &style);
  if (wall == NULL || !bd_wall_set_items(wall, items, WALL_ITEMS)) {
    fprintf(stderr, "bench_render: failed to create wall\n");
    bd_wall_destroy(wall);
    free(items);
    free(titles);
    free(subtitles);
    bd_ui_fonts_free(&fonts);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }
  free(items);
  free(titles);
  free(subtitles);

  bd_color bg = bd_color_rgb(242, 237, 227);
  bd_color ink = bd_color_rgb(30, 26, 21);
  for (int scenario = 0; scenario < BENCH_SCENARIO_COUNT; ++scenario) {
    if (strcmp(scenario_name, "all") != 0 &&
        strcmp(scenario_name, SCENARIO_NAMES[scenario]) != 0) {
      continue;
    }
    run_scenario((bench_scenario)scenario, frames, platform, gfx, wall, bg, ink);
  }

  printf("bench: note=host llvmpipe timings include glFinish; use device for real budgets\n");
  bd_wall_destroy(wall);
  bd_ui_fonts_free(&fonts);
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  return 0;
}
