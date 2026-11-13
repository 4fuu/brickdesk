#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "brickdesk/config.h"
#include "brickdesk/gfx.h"
#include "brickdesk/home.h"
#include "brickdesk/platform.h"
#include "brickdesk/theme.h"
#include "brickdesk/ui.h"
#include "cui_dither.h"
#include "home_fixture.h"
#include "home_internal.h"

/*
 * Prototype-only CUI dither samples for the quick-panel background-memory row.
 *
 * The wall, top bar, quick-panel header, now-playing card, type and palette are
 * drawn by the real bd_home/bd_gfx stack.  The run/memory section below the
 * now-playing card is replaced only for the samples so a taller dither bar can
 * reserve its own vertical space; see README.md for the exact boundary.
 *
 * This file is not part of the product build and no product file is modified.
 */

#define SAMPLE_WIDTH 1024
#define SAMPLE_HEIGHT 768
#define SAMPLE_PANEL_W 402
#define SAMPLE_RUN_COUNT 3
#define SAMPLE_IDLE_FRAMES 120

enum {
  SAMPLE_BAR_INSET_X = 18,
  SAMPLE_BAR_Y = 196,
  SAMPLE_RUN_HEAD_Y = 188,
  SAMPLE_ROWS_START = 204,
  SAMPLE_ROW_H = 50,
  SAMPLE_ROW_BOX = 48,
  SAMPLE_ROWS_GAP = 14,
  SAMPLE_FOOT_GAP = 10,
  SAMPLE_CONTROL_H = 48,
  SAMPLE_CONTROL_COUNT = 4,
  SAMPLE_ROW_RIGHT_PAD = 18,
  SAMPLE_VALUE_COL_W = 40,
  SAMPLE_VALUE_GAP = 12,
  SAMPLE_TRACK_W = 92
};

static const char *const RUN_IDS[SAMPLE_RUN_COUNT] = {"demo-03", "demo-10",
                                                       "music"};
static const int RUN_COMMON_MB[SAMPLE_RUN_COUNT] = {210, 210, 86};
static const int RUN_ZERO_MB[SAMPLE_RUN_COUNT] = {0, 0, 0};
static const int RUN_FULL_MB[SAMPLE_RUN_COUNT] = {1024, 0, 0};
static const int RUN_UNKNOWN_MB[SAMPLE_RUN_COUNT] = {210, -1, 86};

typedef enum sample_value {
  SAMPLE_VALUE_ZERO = 0,
  SAMPLE_VALUE_COMMON,
  SAMPLE_VALUE_FULL,
  SAMPLE_VALUE_UNKNOWN
} sample_value;

typedef struct sample_spec {
  const char *name;
  const char *value_label;
  int cell;
  int bar_h;
  bool dither;
  sample_value value;
  const int *memory_mb;
  float density;
} sample_spec;

typedef struct sample_layout {
  bd_rect panel;
  bd_rect bar;
  int rows_start;  /* panel-relative */
  int controls_y;  /* panel-relative */
} sample_layout;

typedef struct bake_info {
  int tex_width;
  int tex_height;
  int columns;
  int rows;
  int lit_cells;
  int total_cells;
  float density;
} bake_info;

typedef struct idle_stats {
  int frames;
  int presents;
  int max_draw_calls;
  int damaged_frames;
  int bakes;
} idle_stats;

typedef struct sample_ctx {
  bd_platform *platform;
  bd_gfx *gfx;
  bd_home *home;
  bd_texture *dots;
  bake_info bake;
  int bakes;
  int uploads;
  const char *out_dir;
  FILE *manifest;
  uint64_t now_ms;
  int shot_count;
  int failures;
} sample_ctx;

static int g_brightness = 70;
static int g_volume = 45;

static bd_color alpha_color(bd_color color, float alpha) {
  color.a *= alpha;
  return color;
}

static bool quick_read(void *user, bd_home_quick_control control,
                       int *percent) {
  (void)user;
  if (percent == NULL) {
    return false;
  }
  *percent =
      control == BD_HOME_QUICK_BRIGHTNESS ? g_brightness : g_volume;
  return true;
}

static bool quick_adjust(void *user, bd_home_quick_control control, int delta,
                         int *percent) {
  (void)user;
  if (percent == NULL) {
    return false;
  }
  int value =
      (control == BD_HOME_QUICK_BRIGHTNESS ? g_brightness : g_volume) + delta;
  if (value < 0) {
    value = 0;
  }
  if (value > 100) {
    value = 100;
  }
  if (control == BD_HOME_QUICK_BRIGHTNESS) {
    g_brightness = value;
  } else {
    g_volume = value;
  }
  *percent = value;
  return true;
}

static void draw_quick_icon(bd_gfx *gfx, int icon, int x, int y, bd_color color,
                            bd_ui_wifi_state wifi_state) {
  switch (icon) {
    case 0: /* brightness */
      bd_gfx_fill_round_rect(gfx, bd_rect_make(x + 6, y + 6, 8, 8), 4, color);
      bd_gfx_fill_rect(gfx, bd_rect_make(x + 9, y, 2, 3), color);
      bd_gfx_fill_rect(gfx, bd_rect_make(x + 9, y + 17, 2, 3), color);
      bd_gfx_fill_rect(gfx, bd_rect_make(x, y + 9, 3, 2), color);
      bd_gfx_fill_rect(gfx, bd_rect_make(x + 17, y + 9, 3, 2), color);
      break;
    case 1: /* volume */
      bd_gfx_fill_round_rect(gfx, bd_rect_make(x + 2, y + 6, 6, 8), 1, color);
      bd_gfx_fill_rect(gfx, bd_rect_make(x + 9, y + 4, 2, 12), color);
      bd_gfx_fill_rect(gfx, bd_rect_make(x + 13, y + 7, 2, 6), color);
      break;
    case 2: /* wifi */
      bd_ui_draw_wifi(gfx, x + BD_UI_WIFI_ICON_W / 2,
                      y + BD_UI_WIFI_ICON_H / 2 + 2, wifi_state, color);
      break;
    case 3: /* bluetooth */
      bd_gfx_fill_rect(gfx, bd_rect_make(x + 9, y + 2, 2, 16), color);
      bd_gfx_fill_rect(gfx, bd_rect_make(x + 6, y + 5, 6, 2), color);
      bd_gfx_fill_rect(gfx, bd_rect_make(x + 6, y + 13, 6, 2), color);
      bd_gfx_fill_rect(gfx, bd_rect_make(x + 12, y + 7, 2, 2), color);
      bd_gfx_fill_rect(gfx, bd_rect_make(x + 12, y + 11, 2, 2), color);
      break;
    default:
      break;
  }
}

static void draw_slider(bd_home *home, bd_gfx *gfx, int x, int y, int value,
                        float alpha) {
  int width = SAMPLE_TRACK_W;
  bd_gfx_fill_round_rect(gfx, bd_rect_make(x, y, width, 3), 1,
                         alpha_color(home->palette.line_strong, alpha));
  int fill = width * value / 100;
  if (fill > 0) {
    bd_gfx_fill_round_rect(gfx, bd_rect_make(x, y, fill, 3), 1,
                           alpha_color(home->accent.accent, alpha));
  }
  bd_rect knob = bd_rect_make(x + fill - 4, y - 3, 9, 9);
  bd_gfx_fill_round_rect(gfx, knob, 4,
                         alpha_color(home->palette.surface, alpha));
  bd_gfx_stroke_round_rect(gfx, knob, 4, 1,
                           alpha_color(home->accent.accent, alpha));
}

static void draw_quick_row_background(bd_home *home, bd_gfx *gfx, bd_rect row,
                                      bool focused) {
  if (!focused) {
    return;
  }
  bd_gfx_fill_round_rect(gfx, row, 5, home->palette.surface2);
  bd_gfx_stroke_round_rect(gfx, row, 5, 2, home->accent.accent);
}

static bd_ui_wifi_state wifi_state(const bd_home *home) {
  if (!home->status.wifi_known) {
    return BD_UI_WIFI_UNKNOWN;
  }
  return home->status.wifi ? BD_UI_WIFI_CONNECTED : BD_UI_WIFI_DISCONNECTED;
}

static sample_layout compute_layout(bd_home *home,
                                    const sample_spec *spec) {
  sample_layout layout;
  memset(&layout, 0, sizeof(layout));
  layout.panel = bd_home_quick_panel_rect(home);
  /*
   * 366 is divisible by 2 and 3 but not 4.  The 4 px sample is 364 px wide so
   * every cell stays an exact 4 px block; it keeps the same optical inset and
   * is 1 px narrower at each end.  The product width stays 366 in the
   * current-state and 2/3 px samples.
   */
  int bar_inset = spec->cell == 4 ? 19 : SAMPLE_BAR_INSET_X;
  int bar_w = spec->cell == 4 ? 364 : layout.panel.w - 36;
  layout.bar = bd_rect_make(layout.panel.x + bar_inset,
                            layout.panel.y + SAMPLE_BAR_Y, bar_w, spec->bar_h);
  if (spec->dither) {
    layout.rows_start = SAMPLE_ROWS_START + spec->bar_h;
    layout.controls_y = layout.rows_start +
                        SAMPLE_RUN_COUNT * SAMPLE_ROW_H + SAMPLE_ROWS_GAP +
                        SAMPLE_FOOT_GAP;
  } else {
    layout.rows_start = SAMPLE_ROWS_START;
    layout.controls_y = layout.rows_start + SAMPLE_RUN_COUNT * SAMPLE_ROW_H +
                        SAMPLE_ROWS_GAP + SAMPLE_FOOT_GAP;
  }
  return layout;
}

static int running_memory(const bd_home *home) {
  int memory = 0;
  for (int i = 0; i < home->running_count; ++i) {
    if (home->running[i].memory_mb < 0) {
      return -1;
    }
    memory += home->running[i].memory_mb;
  }
  return memory;
}

static void draw_dither_section(sample_ctx *ctx, const sample_spec *spec) {
  bd_home *home = ctx->home;
  bd_gfx *gfx = ctx->gfx;
  const bd_home_palette *palette = &home->palette;
  sample_layout layout = compute_layout(home, spec);

  /*
   * The real quick panel already drew the header and now-playing card.  Cover
   * only the run section and rebuild it with the sample row start; the fill is
   * the same opaque panel surface, so there is no seam.
   */
  int cut_y = layout.panel.y + 164;
  bd_gfx_fill_rect(gfx,
                   bd_rect_make(layout.panel.x, cut_y, layout.panel.w,
                                layout.panel.h - (cut_y - layout.panel.y)),
                   palette->surface);
  bd_gfx_fill_rect(gfx,
                   bd_rect_make(layout.panel.x, cut_y, 1,
                                layout.panel.h - (cut_y - layout.panel.y)),
                   palette->line_strong);
  bd_gfx_push_clip(gfx, layout.panel);

  int run_head = layout.panel.y + SAMPLE_RUN_HEAD_Y;
  bd_gfx_draw_text(gfx, home->fonts.body14, layout.panel.x + 18, run_head,
                   palette->ink3, "正在运行");
  char count[32];
  int max_background =
      home->status.max_background > 0 ? home->status.max_background : 3;
  snprintf(count, sizeof(count), "%d / %d", home->running_count,
           max_background);
  bd_gfx_draw_text(gfx, home->fonts.body14,
                   layout.panel.x + layout.panel.w - 18 -
                       bd_font_measure_text(home->fonts.body14, count),
                   run_head, palette->ink3, count);

  bd_gfx_fill_round_rect(gfx, layout.bar, layout.bar.h / 2,
                         palette->line);
  if (ctx->dots != NULL) {
    int draw_w = ctx->bake.tex_width * spec->cell;
    bd_gfx_push_clip(gfx, layout.bar);
    bd_gfx_draw_texture_round(
        gfx, ctx->dots,
        bd_rect_make(layout.bar.x, layout.bar.y, draw_w, layout.bar.h),
        bd_color_rgba(255, 255, 255, 255), layout.bar.h / 2);
    bd_gfx_pop_clip(gfx);
  }

  int rows_start = layout.panel.y + layout.rows_start;
  for (int slot = 0; slot < home->running_count; ++slot) {
    const bd_home_running *run = &home->running[slot];
    bd_rect row = bd_rect_make(layout.panel.x + 16,
                               rows_start + slot * SAMPLE_ROW_H,
                               layout.panel.w - 32, SAMPLE_ROW_BOX);
    bool focused = home->quick_sel == slot;
    draw_quick_row_background(home, gfx, row, focused);
    bd_rect thumb = bd_rect_make(row.x + 8, row.y + 8, 32, 32);
    if (run->frame != NULL) {
      bd_gfx_draw_texture_cover(gfx, run->frame, thumb,
                                bd_color_rgba(255, 255, 255, 255), 5);
    } else if (run->item_index >= 0 && run->item_index < home->count &&
               home->entries[run->item_index].texture != NULL) {
      bd_gfx_draw_texture_cover(gfx,
                                home->entries[run->item_index].texture, thumb,
                                bd_color_rgba(255, 255, 255, 255), 5);
    } else {
      bd_gfx_fill_round_rect(gfx, thumb, 5, palette->surface3);
    }
    const char *name = run->item_index >= 0 && run->item_index < home->count
                           ? home->entries[run->item_index].title
                           : run->id;
    bd_gfx_draw_text_ellipsized(
        gfx, home->fonts.body16, row.x + 48, row.y + 28, row.w - 180,
        focused ? palette->ink : palette->ink2, name);
    char meta[64];
    if (run->memory_mb < 0) {
      snprintf(meta, sizeof(meta), "%s · 内存未知",
               run->paused ? "已暂停" : "运行中");
    } else {
      snprintf(meta, sizeof(meta), "%s · %d MB",
               run->paused ? "已暂停" : "运行中", run->memory_mb);
    }
    bd_gfx_draw_text(gfx, home->fonts.badge13,
                     row.x + row.w - SAMPLE_ROW_RIGHT_PAD -
                         bd_font_measure_text(home->fonts.badge13, meta),
                     row.y + 28, focused ? palette->ink2 : palette->ink3, meta);
  }

  int memory = running_memory(home);
  int foot_baseline = layout.panel.y + layout.controls_y - SAMPLE_FOOT_GAP;
  char memory_text[64];
  if (memory < 0) {
    snprintf(memory_text, sizeof(memory_text), "后台内存 未知");
  } else {
    snprintf(memory_text, sizeof(memory_text), "后台内存 %d / 1024 MB", memory);
  }
  bd_gfx_draw_text(gfx, home->fonts.badge13, layout.panel.x + 18,
                   foot_baseline, palette->ink3, memory_text);

  int list_y = layout.panel.y + layout.controls_y;
  for (int i = 0; i < SAMPLE_CONTROL_COUNT; ++i) {
    bd_rect row = bd_rect_make(layout.panel.x + 16,
                               list_y + i * SAMPLE_CONTROL_H,
                               layout.panel.w - 32, SAMPLE_CONTROL_H);
    bool focused = home->quick_sel == home->running_count + i;
    draw_quick_row_background(home, gfx, row, focused);
    bd_color icon_color = focused ? home->accent.accent : palette->ink3;
    draw_quick_icon(gfx, i, row.x + 10, row.y + 14, icon_color,
                    wifi_state(home));
    static const char *const LABELS[4] = {"亮度", "音量", "Wi-Fi", "蓝牙"};
    bd_gfx_draw_text(gfx, home->fonts.body16, row.x + 40, row.y + 29,
                     focused ? palette->ink : palette->ink2, LABELS[i]);
    if (i == 0 || i == 1) {
      int value = i == 0 ? home->brightness : home->volume;
      int value_right = row.x + row.w - SAMPLE_ROW_RIGHT_PAD;
      if (value < 0) {
        const char *unknown = "未知";
        bd_gfx_draw_text(gfx, home->fonts.body14,
                         value_right -
                             bd_font_measure_text(home->fonts.body14, unknown),
                         row.y + 29, palette->ink3, unknown);
      } else {
        int value_left = value_right - SAMPLE_VALUE_COL_W;
        draw_slider(home, gfx,
                    value_left - SAMPLE_VALUE_GAP - SAMPLE_TRACK_W,
                    row.y + 23, value, 1.0f);
        char number[16];
        snprintf(number, sizeof(number), "%d", value);
        bd_gfx_draw_text(gfx, home->fonts.body14,
                         value_right -
                             bd_font_measure_text(home->fonts.body14, number),
                         row.y + 29, palette->ink2, number);
      }
    } else if (i == 2) {
      const char *value = !home->status.wifi_known
                              ? "未知"
                              : (home->status.wifi ? "已连接" : "未连接");
      bd_gfx_draw_text(gfx, home->fonts.body14,
                       row.x + row.w - SAMPLE_ROW_RIGHT_PAD -
                           bd_font_measure_text(home->fonts.body14, value),
                       row.y + 29, palette->ink2, value);
    } else if (i == 3) {
      const char *value = "不可用";
      bd_gfx_draw_text(gfx, home->fonts.body14,
                       row.x + row.w - SAMPLE_ROW_RIGHT_PAD -
                           bd_font_measure_text(home->fonts.body14, value),
                       row.y + 29, palette->ink3, value);
    }
  }

  bd_gfx_pop_clip(gfx);
}

static void draw_scene(sample_ctx *ctx, const sample_spec *spec,
                       bool force_damage) {
  bd_gfx_begin_frame(ctx->gfx);
  bd_home_update(ctx->home, ctx->now_ms);
  if (force_damage) {
    bd_gfx_damage_all(ctx->gfx);
  }
  if (bd_gfx_has_damage(ctx->gfx)) {
    bd_home_draw(ctx->home, ctx->gfx);
    if (spec->dither) {
      draw_dither_section(ctx, spec);
    }
  }
  bd_gfx_end_frame(ctx->gfx);
  if (bd_gfx_get_stats(ctx->gfx).presented) {
    bd_platform_present(ctx->platform);
  }
}

static void settle(sample_ctx *ctx, const sample_spec *spec) {
  int quiet = 0;
  for (int tries = 0; tries < 400 && quiet < 2; ++tries) {
    ctx->now_ms += 20;
    draw_scene(ctx, spec, true);
    uint64_t until = 0;
    bool active = bd_home_needs_frame_until(ctx->home, &until) ||
                  bd_home_has_pending_images(ctx->home);
    quiet = active ? 0 : quiet + 1;
    bd_event event;
    bd_platform_wait_event(ctx->platform, &event, 2);
  }
}

static bool bake_bar(sample_ctx *ctx, const sample_spec *spec,
                     const sample_layout *layout) {
  bd_color color =
      spec->value == SAMPLE_VALUE_UNKNOWN ? ctx->home->palette.ink3
                                          : ctx->home->accent.accent;
  cui_dither_bitmap bake;
  if (!cui_dither_bake(&bake, layout->bar.w, layout->bar.h, spec->cell,
                       spec->density, color)) {
    fprintf(stderr, "cui_samples: bake failed for %s\n", spec->name);
    return false;
  }

  bool ok;
  if (ctx->dots == NULL) {
    ctx->dots = bd_gfx_texture_create(ctx->gfx, bake.tex_width, bake.tex_height,
                                      BD_TEXTURE_NEAREST, bake.pixels);
    ok = ctx->dots != NULL;
  } else {
    ok = bd_gfx_texture_update(ctx->gfx, ctx->dots, bake.tex_width,
                               bake.tex_height, bake.pixels);
  }
  if (!ok) {
    fprintf(stderr, "cui_samples: texture upload failed for %s: %s\n",
            spec->name, bd_gfx_last_error(ctx->gfx));
    cui_dither_free(&bake);
    return false;
  }
  ++ctx->bakes;
  ++ctx->uploads;

  ctx->bake.tex_width = bake.tex_width;
  ctx->bake.tex_height = bake.tex_height;
  ctx->bake.columns = bake.columns;
  ctx->bake.rows = bake.rows;
  ctx->bake.lit_cells = bake.lit_cells;
  ctx->bake.total_cells = bake.total_cells;
  ctx->bake.density = bake.density;
  cui_dither_free(&bake);
  return true;
}

static bool verify_idle(sample_ctx *ctx, idle_stats *out) {
  memset(out, 0, sizeof(*out));
  int bakes_before = ctx->bakes;
  uint64_t until = 0;
  if (bd_home_needs_frame_until(ctx->home, &until) ||
      bd_home_has_pending_images(ctx->home)) {
    fprintf(stderr, "cui_samples: scene still active before idle proof\n");
    return false;
  }
  for (int i = 0; i < SAMPLE_IDLE_FRAMES; ++i) {
    ctx->now_ms += 16;
    bd_home_update(ctx->home, ctx->now_ms);
    bd_gfx_begin_frame(ctx->gfx);
    if (bd_gfx_has_damage(ctx->gfx)) {
      ++out->damaged_frames;
    }
    bd_gfx_end_frame(ctx->gfx);
    bd_gfx_stats stats = bd_gfx_get_stats(ctx->gfx);
    if (stats.presented) {
      ++out->presents;
    }
    if (stats.draw_calls > out->max_draw_calls) {
      out->max_draw_calls = stats.draw_calls;
    }
    ++out->frames;
  }
  out->bakes = ctx->bakes - bakes_before;
  return out->presents == 0 && out->max_draw_calls == 0 &&
         out->damaged_frames == 0 && out->bakes == 0;
}

static bool capture_png(sample_ctx *ctx, const char *name) {
  char path[1024];
  int written = snprintf(path, sizeof(path), "%s/%s.png", ctx->out_dir, name);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    fprintf(stderr, "cui_samples: output path too long for %s\n", name);
    return false;
  }
  if (!bd_gfx_save_png(ctx->gfx, path)) {
    fprintf(stderr, "cui_samples: save %s failed: %s\n", path,
            bd_gfx_last_error(ctx->gfx));
    return false;
  }
  printf("capture: %s\n", path);
  return true;
}

static bool run_shot(sample_ctx *ctx, const sample_spec *spec) {
  for (int i = 0; i < SAMPLE_RUN_COUNT; ++i) {
    if (!bd_home_set_running_memory(ctx->home, RUN_IDS[i], spec->memory_mb[i])) {
      fprintf(stderr, "cui_samples: setting memory for %s failed\n",
              RUN_IDS[i]);
      return false;
    }
  }
  sample_layout layout = compute_layout(ctx->home, spec);
  memset(&ctx->bake, 0, sizeof(ctx->bake));
  if (spec->dither) {
    if (!bake_bar(ctx, spec, &layout)) {
      return false;
    }
  }

  settle(ctx, spec);
  layout = compute_layout(ctx->home, spec);
  if (!capture_png(ctx, spec->name)) {
    return false;
  }

  idle_stats idle;
  bool idle_ok = verify_idle(ctx, &idle);
  if (!idle_ok) {
    fprintf(stderr,
            "cui_samples: %s is not static (presents=%d draws=%d damage=%d "
            "bakes=%d)\n",
            spec->name, idle.presents, idle.max_draw_calls,
            idle.damaged_frames, idle.bakes);
    ++ctx->failures;
  }

  ++ctx->shot_count;
  int manifest_memory = running_memory(ctx->home);
  float manifest_density = spec->dither
                               ? spec->density
                               : (manifest_memory > 0
                                      ? (float)(manifest_memory > 1024
                                                    ? 1024
                                                    : manifest_memory) /
                                            1024.0f
                                      : 0.0f);
  fprintf(ctx->manifest,
          "%s\t%s.png\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t"
          "%d\t%.4f\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n",
          spec->name, spec->name, spec->dither ? "dither" : "current",
          spec->value_label, spec->cell, layout.bar.x, layout.bar.y,
          layout.bar.w, layout.bar.h, ctx->bake.tex_width,
          ctx->bake.tex_height, ctx->bake.columns, ctx->bake.rows,
          ctx->bake.lit_cells, ctx->bake.total_cells,
          manifest_density, manifest_memory, layout.rows_start,
          layout.controls_y,
          spec->dither ? 1 : 0, spec->dither ? 1 : 0, idle.frames,
          idle.presents, idle.max_draw_calls, idle.damaged_frames, idle.bakes);
  fflush(ctx->manifest);
  return true;
}

static bool self_check(const char *out_dir) {
  bool seen[64] = {false};
  bool permutation = true;
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      float threshold = cui_bayer8(x, y);
      int index = (int)(threshold * 64.0f);
      if (index < 0 || index >= 64 || seen[index]) {
        permutation = false;
      } else {
        seen[index] = true;
      }
    }
  }

  bool subset = true;
  cui_dither_bitmap low;
  cui_dither_bitmap high;
  bool baked = cui_dither_bake(&low, 8, 8, 1, 0.25f,
                               bd_color_rgba(255, 255, 255, 255)) &&
               cui_dither_bake(&high, 8, 8, 1, 0.75f,
                               bd_color_rgba(255, 255, 255, 255));
  if (baked) {
    for (int i = 0; i < 64; ++i) {
      const uint8_t *lo = low.pixels + (size_t)i * 4u;
      const uint8_t *hi = high.pixels + (size_t)i * 4u;
      if (lo[3] != 0 && hi[3] == 0) {
        subset = false;
      }
    }
  } else {
    subset = false;
  }
  cui_dither_free(&low);
  cui_dither_free(&high);

  cui_dither_bitmap over;
  cui_dither_bitmap under;
  bool clamp = cui_dither_bake(&over, 8, 8, 1, 1.5f,
                               bd_color_rgba(255, 255, 255, 255)) &&
               cui_dither_bake(&under, 8, 8, 1, -1.0f,
                               bd_color_rgba(255, 255, 255, 255));
  if (clamp) {
    clamp = over.lit_cells == 64 && under.lit_cells == 0 &&
            over.density == 1.0f && under.density == 0.0f;
  }
  cui_dither_free(&over);
  cui_dither_free(&under);

  char path[1024];
  int written = snprintf(path, sizeof(path), "%s/selfcheck.txt", out_dir);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    return false;
  }
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  fprintf(file, "bayer_permutation=%s\n", permutation ? "ok" : "fail");
  fprintf(file, "bayer_subset=%s\n", subset ? "ok" : "fail");
  fprintf(file, "density_clamp=%s\n", clamp ? "ok" : "fail");
  fclose(file);
  printf("self-check: bayer_permutation=%s bayer_subset=%s density_clamp=%s\n",
         permutation ? "ok" : "fail", subset ? "ok" : "fail",
         clamp ? "ok" : "fail");
  return permutation && subset && clamp;
}

static bool write_gl_info(sample_ctx *ctx) {
  bd_gl_info info;
  memset(&info, 0, sizeof(info));
  bd_platform_gl_info(ctx->platform, &info);
  char path[1024];
  int written = snprintf(path, sizeof(path), "%s/gl.txt", ctx->out_dir);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    return false;
  }
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  fprintf(file, "vendor=%s\n", info.vendor != NULL ? info.vendor : "");
  fprintf(file, "renderer=%s\n", info.renderer != NULL ? info.renderer : "");
  fprintf(file, "version=%s\n", info.version != NULL ? info.version : "");
  fprintf(file, "shading_language=%s\n",
          info.shading_language != NULL ? info.shading_language : "");
  fclose(file);
  printf("gl: %s / %s / %s\n", info.vendor != NULL ? info.vendor : "?",
         info.renderer != NULL ? info.renderer : "?",
         info.version != NULL ? info.version : "?");
  printf("glsl: %s\n",
         info.shading_language != NULL ? info.shading_language : "?");
  return true;
}

static bool create_home(sample_ctx *ctx) {
  bd_color accent;
  const char *hex = bd_config_accent_hex("persimmon", true);
  if (hex == NULL || !bd_color_from_hex(hex, &accent)) {
    accent = bd_color_rgb(224, 122, 95);
  }
  bd_home_style style;
  memset(&style, 0, sizeof(style));
  style.accent_base = accent;
  style.blur_enabled = true;
  style.width = SAMPLE_WIDTH;
  style.height = SAMPLE_HEIGHT;
  ctx->home = bd_home_create(ctx->gfx, &style);
  if (ctx->home == NULL) {
    fprintf(stderr, "cui_samples: bd_home_create failed\n");
    return false;
  }

  bd_home_item items[HOME_DEMO_COUNT];
  int count = 0;
  home_demo_items(items, &count);
  if (!bd_home_set_items(ctx->home, items, count)) {
    fprintf(stderr, "cui_samples: bd_home_set_items failed\n");
    return false;
  }
  bd_home_status status;
  memset(&status, 0, sizeof(status));
  status.clock = "22:26";
  status.wifi_known = true;
  status.wifi = true;
  status.battery_percent = 82;
  status.max_background = 3;
  bd_home_set_status(ctx->home, &status);

  bd_home_quick_backend backend;
  memset(&backend, 0, sizeof(backend));
  backend.read = quick_read;
  backend.adjust = quick_adjust;
  bd_home_set_quick_backend(ctx->home, &backend);
  bd_home_refresh_quick(ctx->home);

  for (int i = 0; i < SAMPLE_RUN_COUNT; ++i) {
    if (!bd_home_set_running(ctx->home, RUN_IDS[i], NULL, false,
                             RUN_COMMON_MB[i])) {
      fprintf(stderr, "cui_samples: bd_home_set_running %s failed\n",
              RUN_IDS[i]);
      return false;
    }
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: cui_samples OUT_DIR\n");
    return 2;
  }
  const char *out_dir = argv[1];
  if (mkdir(out_dir, 0777) != 0) {
    /* Reusing an existing output directory is fine. */
  }

  bd_platform_config config;
  memset(&config, 0, sizeof(config));
  config.title = "brickdesk CUI samples";
  config.width = SAMPLE_WIDTH;
  config.height = SAMPLE_HEIGHT;
  config.hidden = true;
  sample_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.out_dir = out_dir;
  ctx.platform = bd_platform_create(&config);
  if (ctx.platform == NULL) {
    return 1;
  }
  ctx.gfx = bd_gfx_create(SAMPLE_WIDTH, SAMPLE_HEIGHT);
  if (ctx.gfx == NULL) {
    bd_platform_destroy(ctx.platform);
    return 1;
  }
  bd_gfx_image_cache_set_budget(ctx.gfx, 96u * 1024u * 1024u);

  bool ok = write_gl_info(&ctx);
  ok = self_check(out_dir) && ok;
  char manifest_path[1024];
  int written = snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.tsv",
                         out_dir);
  if (written > 0 && (size_t)written < sizeof(manifest_path)) {
    ctx.manifest = fopen(manifest_path, "w");
  }
  if (ctx.manifest == NULL) {
    fprintf(stderr, "cui_samples: cannot open manifest\n");
    return 1;
  }
  fprintf(ctx.manifest,
          "name\tfile\tmode\tvalue\tcell\tbar_x\tbar_y\tbar_w\tbar_h\t"
          "tex_w\ttex_h\tcols\trows\tlit\ttotal\tdensity\tmemory_mb\t"
          "rows_start\tcontrols_y\tbakes\tuploads\tidle_frames\t"
          "idle_presents\tidle_draw_calls\tidle_damage_rects\tidle_bakes\n");

  ok = create_home(&ctx) && ok;
  if (ok) {
    ctx.now_ms = 10000;
    bd_home_update(ctx.home, ctx.now_ms);
    if (!bd_home_open_quick(ctx.home)) {
      fprintf(stderr, "cui_samples: bd_home_open_quick failed\n");
      ok = false;
    } else {
      /* Finish the 200 ms slide before the first layout is recorded. */
      ctx.now_ms += 220;
      bd_home_update(ctx.home, ctx.now_ms);
    }
  }

  static const sample_spec CURRENT = {
      .name = "00_current_506",
      .value_label = "common",
      .cell = 0,
      .bar_h = 2,
      .dither = false,
      .value = SAMPLE_VALUE_COMMON,
      .memory_mb = RUN_COMMON_MB,
      .density = 506.0f / 1024.0f,
  };
  static const sample_spec CURRENT_UNKNOWN = {
      .name = "00b_current_unknown",
      .value_label = "unknown",
      .cell = 0,
      .bar_h = 2,
      .dither = false,
      .value = SAMPLE_VALUE_UNKNOWN,
      .memory_mb = RUN_UNKNOWN_MB,
      .density = 0.0f,
  };
  if (ok) {
    ok = run_shot(&ctx, &CURRENT) && ok;
  }
  if (ok) {
    ok = run_shot(&ctx, &CURRENT_UNKNOWN) && ok;
  }

  static const int CELLS[3] = {2, 3, 4};
  static const int BAR_HEIGHTS[3] = {10, 12, 16};
  static const char *const CELL_NAMES[3] = {"cell2", "cell3", "cell4"};
  for (int c = 0; c < 3 && ok; ++c) {
    static const sample_value VALUES[4] = {
        SAMPLE_VALUE_ZERO, SAMPLE_VALUE_COMMON, SAMPLE_VALUE_FULL,
        SAMPLE_VALUE_UNKNOWN};
    static const char *const VALUE_NAMES[4] = {"zero", "506", "full",
                                               "unknown"};
    static const int *const MEMORY[4] = {RUN_ZERO_MB, RUN_COMMON_MB,
                                         RUN_FULL_MB, RUN_UNKNOWN_MB};
    static const float DENSITY[4] = {0.0f, 506.0f / 1024.0f, 1.0f, 0.25f};
    for (int v = 0; v < 4; ++v) {
      char name[64];
      snprintf(name, sizeof(name), "%02d_%s_%s", 1 + c * 4 + v,
               CELL_NAMES[c], VALUE_NAMES[v]);
      sample_spec spec;
      memset(&spec, 0, sizeof(spec));
      spec.name = name;
      spec.value_label = VALUE_NAMES[v];
      spec.cell = CELLS[c];
      spec.bar_h = BAR_HEIGHTS[c];
      spec.dither = true;
      spec.value = VALUES[v];
      spec.memory_mb = MEMORY[v];
      spec.density = DENSITY[v];
      ok = run_shot(&ctx, &spec) && ok;
    }
  }

  fclose(ctx.manifest);
  if (ctx.dots != NULL) {
    bd_gfx_texture_destroy(ctx.gfx, ctx.dots);
  }
  bd_home_destroy(ctx.home);
  bd_gfx_destroy(ctx.gfx);
  bd_platform_destroy(ctx.platform);

  printf("shots: %d, failures: %d\n", ctx.shot_count, ctx.failures);
  return (ok && ctx.failures == 0 && ctx.shot_count == 14) ? 0 : 1;
}
