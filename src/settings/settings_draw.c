#include "settings_internal.h"

#include <stdio.h>
#include <string.h>

#include "brickdesk/theme.h"

#define SETTINGS_CATEGORY_COUNT BD_SETTINGS_CATEGORY_COUNT

static bd_color row_background(const bd_settings *settings, bool focused) {
  if (!focused) {
    return settings->palette.surface;
  }
  float amount = settings->style.dark ? 0.075f : 0.06f;
  bd_color mixed = bd_color_mix(settings->palette.surface,
                                settings->accent.accent, amount);
  mixed.a = 1.0f;
  return mixed;
}

static int text_baseline(const bd_font *font, int top, int height) {
  int line = bd_font_line_height(font);
  return top + (height - line) / 2 + bd_font_ascent(font);
}

static void draw_text_right(bd_gfx *gfx, const bd_font *font, int right,
                            int baseline, bd_color color, const char *text) {
  if (text == NULL || text[0] == '\0') {
    return;
  }
  int width = bd_font_measure_text(font, text);
  bd_gfx_draw_text(gfx, font, right - width, baseline, color, text);
}

static void draw_text_right_ellipsized(bd_gfx *gfx, const bd_font *font,
                                       int right, int baseline, int max_width,
                                       bd_color color, const char *text) {
  if (text == NULL || text[0] == '\0') {
    return;
  }
  int width = bd_font_measure_text(font, text);
  int x = right - (width < max_width ? width : max_width);
  bd_gfx_draw_text_ellipsized(gfx, font, x, baseline, max_width, color, text);
}

static void draw_chevron_left(bd_gfx *gfx, int cx, int cy, bd_color color) {
  for (int i = 0; i < 5; ++i) {
    bd_gfx_fill_rect(gfx, bd_rect_make(cx + 8 - i * 2, cy - 8 + i * 2, 3, 3),
                     color);
    bd_gfx_fill_rect(gfx, bd_rect_make(cx + 8 - i * 2, cy + 8 - i * 2, 3, 3),
                     color);
  }
}

static void draw_chevron_right(bd_gfx *gfx, int cx, int cy, bd_color color) {
  for (int i = 0; i < 4; ++i) {
    bd_gfx_fill_rect(gfx, bd_rect_make(cx + i * 2, cy - 6 + i * 2, 2, 3),
                     color);
    bd_gfx_fill_rect(gfx, bd_rect_make(cx + i * 2, cy + 6 - i * 2, 2, 3),
                     color);
  }
}

/* ------------------------------------------------------------- top bar */

static void draw_wifi(bd_gfx *gfx, int cx, int cy, bd_color color) {
  bd_gfx_fill_round_rect(gfx, bd_rect_make(cx - 2, cy + 5, 4, 4), 2, color);
  bd_gfx_push_clip(gfx, bd_rect_make(cx - 10, cy - 8, 20, 12));
  bd_gfx_stroke_round_rect(gfx, bd_rect_make(cx - 10, cy - 3, 20, 20), 10, 2,
                           color);
  bd_gfx_stroke_round_rect(gfx, bd_rect_make(cx - 6, cy + 1, 12, 12), 6, 2,
                           color);
  bd_gfx_pop_clip(gfx);
}

static void draw_battery(bd_gfx *gfx, int x, int cy, int percent,
                         bd_color color, bd_color fill) {
  int w = 25;
  int h = 12;
  int y = cy - h / 2;
  bd_gfx_stroke_round_rect(gfx, bd_rect_make(x, y, w, h), 2, 1, color);
  bd_gfx_fill_round_rect(gfx, bd_rect_make(x + w + 1, cy - 3, 2, 6), 1, color);
  int inner = (w - 4) * percent / 100;
  if (inner > 0) {
    bd_gfx_fill_round_rect(gfx, bd_rect_make(x + 2, y + 2, inner, h - 4), 1,
                           fill);
  }
}

static void draw_top_bar(const bd_settings *settings, bd_gfx *gfx) {
  const bd_settings_palette *palette = &settings->palette;
  draw_chevron_left(gfx, 34, 62, palette->ink2);
  bd_gfx_draw_text(gfx, settings->fonts.serif25, 64, 74, palette->ink, "设置");
  int title_width = bd_font_measure_text(settings->fonts.serif25, "设置");
  const char *category = BD_SETTINGS_PAGES[settings->category].title;
  if (settings->subpage == BD_SETTINGS_SUBPAGE_ACCENT) {
    category = "自定义";
  } else if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN) {
    category = "插件";
  }
  bd_gfx_draw_text(gfx, settings->fonts.value15, 64 + title_width + 18, 73,
                   palette->ink2, category);

  int right = settings->style.width - 28;
  if (settings->clock[0] != '\0') {
    draw_text_right(gfx, settings->fonts.value15, right, 73, palette->ink2,
                    settings->clock);
    right -= bd_font_measure_text(settings->fonts.value15, settings->clock) + 16;
  }
  if (settings->status.battery_percent >= 0) {
    right -= 28;
    draw_battery(gfx, right, 62, settings->status.battery_percent,
                 palette->ink2, palette->ink2);
    right -= 14;
  }
  if (settings->status.wifi) {
    draw_wifi(gfx, right - 9, 54, palette->ink2);
  }
}

/* --------------------------------------------------------- category list */

static void draw_icon_monitor(bd_gfx *gfx, int x, int cy, bd_color color) {
  bd_gfx_stroke_round_rect(gfx, bd_rect_make(x, cy - 8, 16, 12), 2, 2, color);
  bd_gfx_fill_rect(gfx, bd_rect_make(x + 7, cy + 4, 2, 3), color);
  bd_gfx_fill_rect(gfx, bd_rect_make(x + 4, cy + 7, 10, 2), color);
}

static void draw_icon_volume(bd_gfx *gfx, int x, int cy, bd_color color) {
  bd_gfx_fill_round_rect(gfx, bd_rect_make(x + 1, cy - 3, 2, 6), 1, color);
  bd_gfx_fill_round_rect(gfx, bd_rect_make(x + 6, cy - 6, 2, 12), 1, color);
  bd_gfx_fill_round_rect(gfx, bd_rect_make(x + 11, cy - 9, 2, 18), 1, color);
}

static void draw_icon_sun(bd_gfx *gfx, int x, int cy, bd_color color) {
  bd_gfx_stroke_round_rect(gfx, bd_rect_make(x + 4, cy - 5, 10, 10), 5, 2,
                           color);
  bd_gfx_fill_rect(gfx, bd_rect_make(x + 8, cy - 9, 2, 2), color);
  bd_gfx_fill_rect(gfx, bd_rect_make(x + 8, cy + 7, 2, 2), color);
  bd_gfx_fill_rect(gfx, bd_rect_make(x, cy - 1, 2, 2), color);
  bd_gfx_fill_rect(gfx, bd_rect_make(x + 16, cy - 1, 2, 2), color);
  bd_gfx_fill_rect(gfx, bd_rect_make(x + 2, cy - 7, 2, 2), color);
  bd_gfx_fill_rect(gfx, bd_rect_make(x + 14, cy - 7, 2, 2), color);
  bd_gfx_fill_rect(gfx, bd_rect_make(x + 2, cy + 5, 2, 2), color);
  bd_gfx_fill_rect(gfx, bd_rect_make(x + 14, cy + 5, 2, 2), color);
}

static void draw_icon_gamepad(bd_gfx *gfx, int x, int cy, bd_color color) {
  bd_gfx_stroke_round_rect(gfx, bd_rect_make(x + 1, cy - 6, 16, 12), 4, 2,
                           color);
  bd_gfx_fill_round_rect(gfx, bd_rect_make(x + 4, cy - 1, 5, 2), 1, color);
  bd_gfx_fill_rect(gfx, bd_rect_make(x + 5, cy - 3, 2, 6), color);
  bd_gfx_fill_round_rect(gfx, bd_rect_make(x + 11, cy - 2, 3, 3), 2, color);
}

static void draw_icon_gauge(bd_gfx *gfx, int x, int cy, bd_color color) {
  bd_gfx_push_clip(gfx, bd_rect_make(x, cy - 2, 18, 12));
  bd_gfx_stroke_round_rect(gfx, bd_rect_make(x + 3, cy - 7, 12, 12), 6, 2,
                           color);
  bd_gfx_pop_clip(gfx);
  for (int i = 0; i < 4; ++i) {
    bd_gfx_fill_rect(gfx, bd_rect_make(x + 9 + i * 2, cy - 1 - i * 2, 2, 2),
                     color);
  }
  bd_gfx_fill_round_rect(gfx, bd_rect_make(x + 7, cy - 2, 3, 3), 2, color);
}

static void draw_icon_layers(bd_gfx *gfx, int x, int cy, bd_color color) {
  bd_gfx_stroke_round_rect(gfx, bd_rect_make(x + 3, cy - 8, 12, 9), 2, 2,
                           color);
  bd_gfx_stroke_round_rect(gfx, bd_rect_make(x + 1, cy - 1, 12, 9), 2, 2,
                           color);
}

static void draw_icon_plugin(bd_gfx *gfx, int x, int cy, bd_color color) {
  bd_gfx_stroke_round_rect(gfx, bd_rect_make(x + 2, cy - 4, 14, 12), 1, 2,
                           color);
  bd_gfx_fill_rect(gfx, bd_rect_make(x + 6, cy - 8, 2, 5), color);
  bd_gfx_fill_rect(gfx, bd_rect_make(x + 11, cy - 8, 2, 5), color);
  bd_gfx_fill_rect(gfx, bd_rect_make(x + 8, cy + 1, 2, 4), color);
}

static void draw_icon_info(bd_gfx *gfx, int x, int cy, bd_color color) {
  bd_gfx_stroke_round_rect(gfx, bd_rect_make(x + 1, cy - 8, 16, 16), 8, 2,
                           color);
  bd_gfx_fill_round_rect(gfx, bd_rect_make(x + 8, cy - 4, 2, 2), 1, color);
  bd_gfx_fill_rect(gfx, bd_rect_make(x + 8, cy - 1, 2, 5), color);
}

static void draw_category_icon(bd_gfx *gfx, int category, int x, int cy,
                               bd_color color) {
  switch (category) {
    case BD_SETTINGS_CATEGORY_APPEARANCE:
      draw_icon_monitor(gfx, x, cy, color);
      break;
    case BD_SETTINGS_CATEGORY_SOUND:
      draw_icon_volume(gfx, x, cy, color);
      break;
    case BD_SETTINGS_CATEGORY_DISPLAY:
      draw_icon_sun(gfx, x, cy, color);
      break;
    case BD_SETTINGS_CATEGORY_KEYS:
      draw_icon_gamepad(gfx, x, cy, color);
      break;
    case BD_SETTINGS_CATEGORY_PERFORMANCE:
      draw_icon_gauge(gfx, x, cy, color);
      break;
    case BD_SETTINGS_CATEGORY_BACKGROUND:
      draw_icon_layers(gfx, x, cy, color);
      break;
    case BD_SETTINGS_CATEGORY_PLUGINS:
      draw_icon_plugin(gfx, x, cy, color);
      break;
    default:
      draw_icon_info(gfx, x, cy, color);
      break;
  }
}

static void draw_categories(const bd_settings *settings, bd_gfx *gfx) {
  const bd_settings_palette *palette = &settings->palette;
  for (int i = 0; i < SETTINGS_CATEGORY_COUNT; ++i) {
    int top = BD_SETTINGS_CATEGORY_TOP + i * BD_SETTINGS_CATEGORY_PITCH;
    int cy = top + BD_SETTINGS_CATEGORY_PITCH / 2;
    bool selected = i == settings->category &&
                    settings->subpage == BD_SETTINGS_SUBPAGE_NONE &&
                    !settings->confirm_open;
    bd_color color = selected ? settings->accent.accent : palette->ink3;
    if (selected) {
      bd_gfx_fill_round_rect(
          gfx, bd_rect_make(BD_SETTINGS_CATEGORY_X + 3, cy - 11, 3, 22), 1,
          settings->accent.accent);
    }
    draw_category_icon(gfx, i, BD_SETTINGS_CATEGORY_X + 24, cy, color);
    bd_gfx_draw_text(gfx, settings->fonts.title16,
                     BD_SETTINGS_CATEGORY_X + 56, cy + 6,
                     selected ? settings->accent.accent : palette->ink2,
                     BD_SETTINGS_PAGES[i].title);
  }
}

/* ---------------------------------------------------------------- table */

static void draw_page_header(const bd_settings *settings, bd_gfx *gfx) {
  const bd_settings_palette *palette = &settings->palette;
  const char *title = BD_SETTINGS_PAGES[settings->category].title;
  const char *subtitle = BD_SETTINGS_PAGES[settings->category].subtitle;
  int title_baseline = 128;
  if (settings->subpage == BD_SETTINGS_SUBPAGE_ACCENT) {
    title = "自定义强调色";
    subtitle = "调整红、绿、蓝三个通道，立即应用";
  } else if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN) {
    if (settings->plugin_index >= 0 &&
        settings->plugin_index < settings->plugin_count) {
      const bd_plugin_manifest *manifest =
          bd_plugin_manifest_of(settings->plugins[settings->plugin_index]);
      title = manifest != NULL ? manifest->name : "插件设置";
    }
    subtitle = "插件设置 · 由插件声明";
  }
  bd_gfx_draw_text(gfx, settings->fonts.serif34, BD_SETTINGS_TABLE_X,
                   title_baseline, palette->ink, title);
  bd_gfx_draw_text(gfx, settings->fonts.body14, BD_SETTINGS_TABLE_X, 156,
                   palette->ink2, subtitle);
  if (settings->subpage == BD_SETTINGS_SUBPAGE_ACCENT) {
    int size = 58;
    int x = BD_SETTINGS_TABLE_X + BD_SETTINGS_TABLE_W - size;
    int y = 88;
    bd_gfx_fill_round_rect(gfx, bd_rect_make(x, y, size, size), 5,
                           settings->accent.accent);
    bd_gfx_stroke_round_rect(gfx, bd_rect_make(x, y, size, size), 5, 1,
                             palette->line_strong);
    const char *accent =
        bd_config_get_string(settings->config, "theme.accent", "persimmon");
    const char *hex = bd_config_accent_hex(accent, settings->style.dark);
    if (hex != NULL) {
      draw_text_right(gfx, settings->fonts.body14, x + size, 166,
                      palette->ink2, hex);
    }
  }
}

static void draw_table_frame(const bd_settings *settings, bd_gfx *gfx) {
  bd_rect table = bd_settings_table_rect(settings);
  bd_gfx_fill_round_rect(gfx, table, BD_SETTINGS_TABLE_RADIUS,
                         settings->palette.surface);
  bd_gfx_stroke_round_rect(gfx, table, BD_SETTINGS_TABLE_RADIUS, 1,
                           settings->palette.line_strong);
}

static void draw_swatch(bd_gfx *gfx, int cx, int cy, int radius, bd_color color,
                        bool selected, bd_color ring, bd_color gap) {
  if (selected) {
    bd_gfx_fill_round_rect(gfx, bd_rect_make(cx - radius - 4, cy - radius - 4,
                                             (radius + 4) * 2,
                                             (radius + 4) * 2),
                           radius + 4, gap);
    bd_gfx_stroke_round_rect(gfx, bd_rect_make(cx - radius - 3, cy - radius - 3,
                                               (radius + 3) * 2,
                                               (radius + 3) * 2),
                             radius + 3, 2, ring);
  }
  bd_gfx_fill_round_rect(
      gfx, bd_rect_make(cx - radius, cy - radius, radius * 2, radius * 2),
      radius, color);
}

static bd_color knob_color(void) {
  return bd_color_rgb(247, 241, 230);
}

static void draw_toggle(const bd_settings *settings, bd_gfx *gfx,
                        const bd_settings_row *row, bd_rect rect, int right) {
  bool value = bd_config_get_bool(settings->config, row->key, false);
  int pill_w = 44;
  int pill_h = 24;
  int x = right - pill_w;
  int y = rect.y + (rect.h - pill_h) / 2;
  bd_color track = value ? settings->accent.accent : settings->palette.surface3;
  bd_gfx_fill_round_rect(gfx, bd_rect_make(x, y, pill_w, pill_h), 4, track);
  int knob_x = value ? x + pill_w - 21 : x + 3;
  bd_color knob = value ? knob_color() : settings->palette.ink2;
  bd_gfx_fill_round_rect(gfx, bd_rect_make(knob_x, y + 3, 18, 18), 3, knob);
  char label[8];
  snprintf(label, sizeof(label), "%s", value ? "开" : "关");
  int width = bd_font_measure_text(settings->fonts.caption13, label);
  bd_gfx_draw_text(gfx, settings->fonts.caption13, x - width - 12,
                   text_baseline(settings->fonts.caption13, rect.y, rect.h),
                   settings->palette.ink2, label);
}

static void draw_slider(const bd_settings *settings, bd_gfx *gfx,
                        const bd_settings_row *row, bd_rect rect, int right) {
  char text[48];
  bd_settings_row_value(settings, row, text, sizeof(text));
  long value = bd_settings_row_slider_value(settings, row);
  int value_right = right;
  draw_text_right(gfx, settings->fonts.value15, value_right,
                  text_baseline(settings->fonts.value15, rect.y, rect.h),
                  settings->palette.ink2, text);
  int track_w = 96;
  int track_x = right - 64 - 12 - track_w;
  int track_y = rect.y + rect.h / 2 - 1;
  bd_gfx_fill_round_rect(gfx, bd_rect_make(track_x, track_y, track_w, 3), 1,
                         settings->palette.surface3);
  long span = row->max_value - row->min_value;
  float ratio = span > 0 ? (float)(value - row->min_value) / (float)span : 0.0f;
  if (ratio < 0.0f) {
    ratio = 0.0f;
  }
  if (ratio > 1.0f) {
    ratio = 1.0f;
  }
  int fill = (int)(ratio * (float)track_w);
  if (fill > 0) {
    bd_gfx_fill_round_rect(gfx, bd_rect_make(track_x, track_y, fill, 3), 1,
                           settings->accent.accent);
  }
  int knob_x = track_x + (int)(ratio * (float)(track_w - 12)) + 6;
  bd_gfx_fill_round_rect(gfx, bd_rect_make(knob_x - 6, track_y - 5, 12, 12), 6,
                         knob_color());
  bd_gfx_stroke_round_rect(gfx, bd_rect_make(knob_x - 6, track_y - 5, 12, 12),
                           6, 2, settings->accent.accent);
}

static void draw_accent_row(const bd_settings *settings, bd_gfx *gfx,
                            const bd_settings_row *row, bd_rect rect, int right,
                            bool focused) {
  char text[48];
  bd_settings_row_value(settings, row, text, sizeof(text));
  int value_width = bd_font_measure_text(settings->fonts.value15, text);
  draw_text_right(gfx, settings->fonts.value15, right,
                  text_baseline(settings->fonts.value15, rect.y, rect.h),
                  settings->palette.ink2, text);
  const char *current =
      bd_config_get_string(settings->config, "theme.accent", "persimmon");
  bool custom = true;
  for (int i = 0; i < row->enum_count; ++i) {
    if (strcmp(row->enum_values[i], current) == 0) {
      custom = false;
      break;
    }
  }
  int count = row->enum_count + (custom ? 1 : 0);
  int radius = 9;
  int gap = 6;
  int total = count * radius * 2 + (count - 1) * gap;
  int x = right - value_width - 20 - total + radius;
  int cy = rect.y + rect.h / 2;
  bd_color row_bg = row_background(settings, focused);
  if (custom) {
    bd_color color;
    const char *hex = bd_config_accent_hex(current, settings->style.dark);
    if (hex == NULL || !bd_color_from_hex(hex, &color)) {
      color = settings->accent.accent;
    }
    draw_swatch(gfx, x, cy, radius, color, true, settings->palette.ink,
                row_bg);
    x += radius * 2 + gap;
  }
  for (int i = 0; i < row->enum_count; ++i) {
    bd_color color;
    const char *hex =
        bd_config_accent_hex(row->enum_values[i], settings->style.dark);
    if (hex == NULL || !bd_color_from_hex(hex, &color)) {
      color = settings->palette.surface3;
    }
    bool selected = strcmp(row->enum_values[i], current) == 0;
    draw_swatch(gfx, x, cy, radius, color, selected, settings->palette.ink,
                row_bg);
    x += radius * 2 + gap;
  }
}

static void draw_plain_value(const bd_settings *settings, bd_gfx *gfx,
                             const bd_settings_row *row, bd_rect rect,
                             int right) {
  char text[96];
  bd_settings_row_value(settings, row, text, sizeof(text));
  bd_color color = settings->palette.ink2;
  if (row->kind == BD_SETTINGS_ROW_CONFLICT ||
      row->kind == BD_SETTINGS_ROW_RESET) {
    color = settings->accent.accent;
  }
  if (row->kind == BD_SETTINGS_ROW_PLUGIN ||
      row->kind == BD_SETTINGS_ROW_ACTION) {
    draw_text_right_ellipsized(gfx, settings->fonts.value15, right - 14,
                               text_baseline(settings->fonts.value15, rect.y,
                                             rect.h),
                               right - 20, color, text);
    draw_chevron_right(gfx, right - 8, rect.y + rect.h / 2, color);
    return;
  }
  draw_text_right_ellipsized(gfx, settings->fonts.value15, right,
                             text_baseline(settings->fonts.value15, rect.y,
                                           rect.h),
                             right - (rect.x + 220), color, text);
}

static void draw_row(const bd_settings *settings, bd_gfx *gfx, int index) {
  if (index < 0 || index >= settings->row_count) {
    return;
  }
  const bd_settings_row *row = &settings->rows[index];
  bd_rect table = bd_settings_table_rect(settings);
  bd_rect rect = bd_settings_row_rect(settings, index);
  if (rect.y + rect.h <= table.y || rect.y >= table.y + table.h) {
    return;
  }
  const bd_settings_palette *palette = &settings->palette;
  bool focused = index == settings->focus &&
                 settings->subpage != BD_SETTINGS_SUBPAGE_PLUGIN &&
                 !settings->confirm_open;
  bd_gfx_push_clip(gfx, rect);
  bd_gfx_fill_rect(gfx, rect, row_background(settings, focused));
  bd_color border = settings->palette.line_strong;
  bd_gfx_fill_rect(gfx, bd_rect_make(table.x, rect.y, 1, rect.h), border);
  bd_gfx_fill_rect(gfx, bd_rect_make(table.x + table.w - 1, rect.y, 1, rect.h),
                   border);
  if (index == 0) {
    bd_gfx_fill_rect(gfx, bd_rect_make(table.x, rect.y, table.w, 1), border);
  }
  if (index == settings->row_count - 1) {
    bd_gfx_fill_rect(
        gfx, bd_rect_make(table.x, rect.y + rect.h - 1, table.w, 1), border);
  }
  int separator_y = rect.y + rect.h - 1;
  if (separator_y > table.y && separator_y < table.y + table.h - 1) {
    bd_gfx_fill_rect(gfx, bd_rect_make(table.x + 1, separator_y, table.w - 2, 1),
                     palette->line);
  }
  int text_x = table.x + BD_SETTINGS_ROW_PAD;
  bool warning = row->warning;
  if (row->bullet_slot) {
    if (warning) {
      bd_gfx_fill_round_rect(gfx, bd_rect_make(text_x, rect.y + 25, 5, 5), 3,
                             settings->accent.accent);
    }
    text_x += 16;
  }
  bool has_description = row->description[0] != '\0';
  int title_top = has_description ? rect.y + 14 : rect.y + (rect.h - 24) / 2;
  bd_color title_color =
      row->kind == BD_SETTINGS_ROW_CONFLICT ? settings->accent.accent
                                            : palette->ink;
  bd_gfx_draw_text_ellipsized(
      gfx, settings->fonts.title16, text_x, title_top + 16, table.x + table.w -
                                                                    text_x - 200,
      title_color, row->label);
  if (has_description) {
    bd_gfx_draw_text_ellipsized(
        gfx, settings->fonts.body14, text_x, rect.y + 50,
        table.x + table.w - text_x - 190, palette->ink2, row->description);
  }
  int right = table.x + table.w - 20;
  switch (row->kind) {
    case BD_SETTINGS_ROW_TOGGLE:
      draw_toggle(settings, gfx, row, rect, right);
      break;
    case BD_SETTINGS_ROW_SLIDER:
      draw_slider(settings, gfx, row, rect, right);
      break;
    case BD_SETTINGS_ROW_ACCENT:
      draw_accent_row(settings, gfx, row, rect, right, focused);
      break;
    default:
      draw_plain_value(settings, gfx, row, rect, right);
      break;
  }
  if (focused) {
    bd_gfx_stroke_round_rect(
        gfx, bd_rect_make(rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 1), 2,
        2, settings->accent.accent);
  }
  bd_gfx_pop_clip(gfx);
}

static void draw_scrollbar(const bd_settings *settings, bd_gfx *gfx) {
  int total = 0;
  for (int i = 0; i < settings->row_count; ++i) {
    total += BD_SETTINGS_ROW_MIN;
    if (settings->rows[i].description[0] != '\0') {
      total += BD_SETTINGS_ROW_TALL - BD_SETTINGS_ROW_MIN;
    }
  }
  bd_rect table = bd_settings_table_rect(settings);
  int limit = total - table.h;
  if (limit <= 0) {
    return;
  }
  int track_h = table.h - 16;
  int thumb_h = track_h * table.h / total;
  if (thumb_h < 28) {
    thumb_h = 28;
  }
  int thumb_y = table.y + 8;
  if (limit > 0) {
    thumb_y += (track_h - thumb_h) * settings->scroll / limit;
  }
  bd_gfx_fill_round_rect(
      gfx, bd_rect_make(table.x + table.w - 6, table.y + 8, 2, track_h), 1,
      settings->palette.line);
  bd_gfx_fill_round_rect(
      gfx, bd_rect_make(table.x + table.w - 6, thumb_y, 2, thumb_h), 1,
      settings->palette.ink3);
}

static void draw_plugin_table(const bd_settings *settings, bd_gfx *gfx) {
  bd_rect table = bd_settings_table_rect(settings);
  if (settings->plugin_index < 0 ||
      settings->plugin_index >= settings->plugin_count) {
    return;
  }
  bd_plugin *plugin = settings->plugins[settings->plugin_index];
  const bd_plugin_ui *ui = bd_plugin_ui_settings(plugin);
  if (ui == NULL || ui->root < 0) {
    const bd_settings_palette *palette = &settings->palette;
    bd_gfx_draw_text(gfx, settings->fonts.title16, table.x + 24, table.y + 48,
                     palette->ink, "插件尚未提供设置界面");
    bd_gfx_draw_text(gfx, settings->fonts.body14, table.x + 24, table.y + 76,
                     palette->ink2, "等待插件发送 plugin.ui.set");
    return;
  }
  bd_plugin_render_style style;
  memset(&style, 0, sizeof(style));
  style.fonts = &settings->plugin_fonts;
  style.accent = settings->accent;
  style.dark = settings->style.dark;
  int node = settings->plugin_node_count > 0
                 ? settings->plugin_nodes[settings->plugin_node_focus]
                 : -1;
  bd_plugin_ui_draw(ui, gfx, table, &style, node, 0);
}

static void draw_empty(const bd_settings *settings, bd_gfx *gfx) {
  bd_rect table = bd_settings_table_rect(settings);
  bd_gfx_draw_text(gfx, settings->fonts.body14, table.x + 22, table.y + 42,
                   settings->palette.ink3, "没有可显示的设置");
}

static void draw_rows(const bd_settings *settings, bd_gfx *gfx) {
  bd_rect table = bd_settings_table_rect(settings);
  bd_gfx_push_clip(gfx, table);
  if (settings->row_count == 0) {
    draw_empty(settings, gfx);
  } else {
    for (int i = 0; i < settings->row_count; ++i) {
      bd_rect rect = bd_settings_row_rect(settings, i);
      if (rect.y + rect.h <= table.y || rect.y >= table.y + table.h) {
        continue;
      }
      draw_row(settings, gfx, i);
    }
  }
  draw_scrollbar(settings, gfx);
  bd_gfx_pop_clip(gfx);
}

/* --------------------------------------------------------------- hint */

static const char *page_hint(const bd_settings *settings) {
  if (settings->error[0] != '\0') {
    return settings->error;
  }
  if (settings->subpage == BD_SETTINGS_SUBPAGE_ACCENT) {
    return "←→ 调整通道 · B 返回外观";
  }
  if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN) {
    return "↑↓ 移动 · A 激活控件 · B 返回插件列表";
  }
  return BD_SETTINGS_PAGES[settings->category].hint;
}

void bd_settings_draw_hint(bd_settings *settings, bd_gfx *gfx) {
  if (!settings->hint_visible) {
    return;
  }
  const bd_settings_palette *palette = &settings->palette;
  int top = settings->style.height - BD_SETTINGS_HINT_HEIGHT;
  bd_color transparent = palette->hint;
  transparent.a = 0.0f;
  bd_gfx_fill_gradient3(gfx, bd_rect_make(0, top, settings->style.width,
                                          BD_SETTINGS_HINT_HEIGHT),
                        0, transparent, palette->hint, palette->hint,
                        BD_GRADIENT_VERTICAL);
  bd_gfx_fill_rect(gfx, bd_rect_make(0, top, settings->style.width, 1),
                   palette->line_strong);
  int baseline = top + 25;
  bd_gfx_draw_text(gfx, settings->fonts.caption13, 34, baseline, palette->ink2,
                   "B 返回 · ←→ 调整 · A 选择");
  bd_gfx_draw_text(gfx, settings->fonts.caption13, 34, top + 45,
                   palette->ink3, "L1/R1 切换分类");
  bd_gfx_fill_round_rect(gfx, bd_rect_make(280, top + 30, 4, 4), 2,
                         settings->accent.accent);
  bd_gfx_draw_text_ellipsized(gfx, settings->fonts.caption13, 294, top + 35,
                              settings->style.width - 328, palette->ink3,
                              page_hint(settings));
}

static void draw_error_line(const bd_settings *settings, bd_gfx *gfx) {
  if (settings->error[0] == '\0' || settings->hint_visible) {
    return;
  }
  int baseline = settings->style.height - 20;
  bd_gfx_fill_round_rect(gfx, bd_rect_make(BD_SETTINGS_TABLE_X, baseline - 5,
                                           4, 4),
                         2, settings->accent.accent);
  bd_gfx_draw_text_ellipsized(gfx, settings->fonts.caption13,
                              BD_SETTINGS_TABLE_X + 12, baseline,
                              BD_SETTINGS_TABLE_W - 12,
                              settings->accent.accent, settings->error);
}

static void draw_hint_layer(const bd_settings *settings, bd_gfx *gfx) {
  if (settings->hint_visible) {
    bd_settings_draw_hint((bd_settings *)settings, gfx);
  } else {
    int top = settings->style.height - BD_SETTINGS_HINT_HEIGHT;
    bd_gfx_fill_rect(gfx, bd_rect_make(0, top, settings->style.width,
                                       BD_SETTINGS_HINT_HEIGHT),
                     settings->palette.bg);
    draw_error_line(settings, gfx);
  }
}

/* ------------------------------------------------------------- confirm */

static void draw_button(bd_gfx *gfx, const bd_settings *settings, bd_rect rect,
                        const char *label, bool primary, bool danger) {
  const bd_settings_palette *palette = &settings->palette;
  if (primary) {
    bd_gfx_fill_round_rect(gfx, rect, 4, settings->accent.accent);
  } else {
    bd_gfx_fill_round_rect(gfx, rect, 4, palette->surface2);
    bd_gfx_stroke_round_rect(gfx, rect, 4, 1, palette->line_strong);
  }
  bd_color color = primary ? settings->accent.on_accent
                          : (danger ? settings->accent.accent : palette->ink);
  int width = bd_font_measure_text(settings->fonts.title16, label);
  bd_gfx_draw_text(gfx, settings->fonts.title16,
                   rect.x + (rect.w - width) / 2,
                   rect.y + rect.h / 2 + bd_font_ascent(settings->fonts.title16) / 2 -
                       2,
                   color, label);
}

void bd_settings_draw_confirm(bd_settings *settings, bd_gfx *gfx) {
  if (!settings->confirm_open) {
    return;
  }
  const bd_settings_palette *palette = &settings->palette;
  bd_gfx_fill_rect(gfx,
                   bd_rect_make(0, 0, settings->style.width,
                                settings->style.height),
                   palette->scrim);
  int w = 460;
  int h = 212;
  int x = (settings->style.width - w) / 2;
  int y = (settings->style.height - h) / 2;
  bd_gfx_fill_round_rect(gfx, bd_rect_make(x, y, w, h), 5, palette->surface);
  bd_gfx_stroke_round_rect(gfx, bd_rect_make(x, y, w, h), 5, 1,
                           palette->line_strong);
  bd_gfx_draw_text(gfx, settings->fonts.serif25, x + 24, y + 52, palette->ink,
                   "恢复默认设置？");
  bd_gfx_draw_text(gfx, settings->fonts.body14, x + 24, y + 84, palette->ink2,
                   "所有本地修改将被默认值覆盖，");
  bd_gfx_draw_text(gfx, settings->fonts.body14, x + 24, y + 106, palette->ink2,
                   "并立即写入 local.ini。");
  bd_gfx_draw_text(gfx, settings->fonts.caption13, x + 24, y + 136,
                   palette->ink3, "热键与按应用性能覆盖不会删除。");
  int button_w = 120;
  int button_h = 36;
  int button_y = y + h - 24 - button_h;
  bd_rect cancel = bd_rect_make(x + w - 24 - button_w * 2 - 12, button_y,
                                button_w, button_h);
  bd_rect reset = bd_rect_make(x + w - 24 - button_w, button_y, button_w,
                               button_h);
  draw_button(gfx, settings, cancel, "取消", settings->confirm_focus == 0,
              false);
  draw_button(gfx, settings, reset, "恢复", settings->confirm_focus == 1,
              true);
}

/* ---------------------------------------------------------------- draw */

static void draw_background(bd_settings *settings, bd_gfx *gfx) {
  bd_gfx_fill_rect(gfx,
                   bd_rect_make(0, 0, settings->style.width,
                                settings->style.height),
                   settings->palette.bg);
}

void bd_settings_draw_all(bd_settings *settings, bd_gfx *gfx) {
  draw_background(settings, gfx);
  draw_top_bar(settings, gfx);
  draw_categories(settings, gfx);
  draw_page_header(settings, gfx);
  draw_table_frame(settings, gfx);
  if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN) {
    bd_gfx_push_clip(gfx, bd_settings_table_rect(settings));
    draw_plugin_table(settings, gfx);
    bd_gfx_pop_clip(gfx);
  } else {
    draw_rows(settings, gfx);
  }
  draw_hint_layer(settings, gfx);
  if (settings->confirm_open) {
    bd_settings_draw_confirm(settings, gfx);
  }
}

void bd_settings_draw(bd_settings *settings, bd_gfx *gfx) {
  if (settings == NULL || gfx == NULL || !settings->open) {
    return;
  }
  if (settings->full_dirty) {
    bd_settings_draw_all(settings, gfx);
  } else if (settings->table_dirty) {
    draw_table_frame(settings, gfx);
    if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN) {
      bd_gfx_push_clip(gfx, bd_settings_table_rect(settings));
      draw_plugin_table(settings, gfx);
      bd_gfx_pop_clip(gfx);
    } else {
      draw_rows(settings, gfx);
    }
  } else {
    for (int i = 0; i < BD_SETTINGS_MAX_ROWS; ++i) {
      if ((settings->dirty_rows[i / 64] & ((uint64_t)1 << (i % 64))) == 0) {
        continue;
      }
      bd_rect table = bd_settings_table_rect(settings);
      bd_rect rect = bd_settings_row_rect(settings, i);
      if (i < settings->row_count && rect.y + rect.h > table.y &&
          rect.y < table.y + table.h) {
        draw_row(settings, gfx, i);
      }
    }
  }
  if (settings->hint_dirty && !settings->full_dirty) {
    draw_hint_layer(settings, gfx);
  }
  if (settings->confirm_open && !settings->full_dirty) {
    bd_settings_draw_confirm(settings, gfx);
  }
  settings->full_dirty = false;
  settings->table_dirty = false;
  settings->hint_dirty = false;
  settings->dirty_rows[0] = 0;
  settings->dirty_rows[1] = 0;
}
