#include "home_internal.h"

#include "brickdesk/ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const TAB_NAMES[BD_HOME_TAB_COUNT] = {"全部", "游戏", "应用",
                                                          "收藏"};

static bd_color alpha_color(bd_color color, float alpha) {
  color.a *= alpha;
  return color;
}

static bd_color with_alpha(bd_color color, float alpha) {
  color.a = alpha;
  return color;
}

static int round_int(float value) {
  return (int)(value + 0.5f);
}

static const bd_font *title_font_for(const bd_home *home, int w, int h) {
  if (w >= 3 && h >= 2) {
    return home->fonts.title31;
  }
  if (w >= 2 && h >= 2) {
    return home->fonts.title25;
  }
  if (w == 1 && h == 1) {
    return home->fonts.title18;
  }
  return home->fonts.title22;
}

static int title_pixel_size(int w, int h) {
  if (w >= 3 && h >= 2) {
    return 31;
  }
  if (w >= 2 && h >= 2) {
    return 25;
  }
  if (w == 1 && h == 1) {
    return 18;
  }
  if (w >= 3 && h == 1) {
    return 23;
  }
  return 22;
}

static const bd_font *subtitle_font_for(const bd_home *home, int w, int h) {
  if ((w >= 2 && h >= 2)) {
    return home->fonts.body15;
  }
  return home->fonts.body14;
}

static int line_height_for(int pixel_size, float multiplier) {
  return round_int((float)pixel_size * multiplier);
}

static int baseline_for(const bd_font *font, int top, int line_height) {
  int metrics = bd_font_line_height(font);
  return top + (line_height - metrics) / 2 + bd_font_ascent(font);
}

static void text_shadowed(bd_gfx *gfx, const bd_font *font, int x,
                          int baseline, int max_width, bd_color color,
                          const char *text, bool ellipsized) {
  if (text == NULL || text[0] == '\0' || color.a <= 0.0f) {
    return;
  }
  bd_color shadow = color;
  shadow.r = 0.0f;
  shadow.g = 0.0f;
  shadow.b = 0.0f;
  shadow.a = color.a * 0.42f;
  if (ellipsized) {
    bd_gfx_draw_text_ellipsized(gfx, font, x, baseline + 1, max_width, shadow,
                                text);
    bd_gfx_draw_text_ellipsized(gfx, font, x, baseline, max_width, color, text);
  } else {
    bd_gfx_draw_text(gfx, font, x, baseline + 1, shadow, text);
    bd_gfx_draw_text(gfx, font, x, baseline, color, text);
  }
}

static int utf8_length(unsigned char first) {
  if ((first & 0x80u) == 0) {
    return 1;
  }
  if ((first & 0xE0u) == 0xC0u) {
    return 2;
  }
  if ((first & 0xF0u) == 0xE0u) {
    return 3;
  }
  if ((first & 0xF8u) == 0xF0u) {
    return 4;
  }
  return 1;
}

static int fit_prefix(const bd_font *font, const char *text, int max_width) {
  size_t length = strlen(text);
  size_t best = 0;
  size_t last_space = 0;
  size_t cursor = 0;
  char buffer[BD_HOME_TITLE_MAX];
  while (cursor < length) {
    int step = utf8_length((unsigned char)text[cursor]);
    if (cursor + (size_t)step > length) {
      step = (int)(length - cursor);
    }
    cursor += (size_t)step;
    size_t copy = cursor < sizeof(buffer) - 1 ? cursor : sizeof(buffer) - 1;
    memcpy(buffer, text, copy);
    buffer[copy] = '\0';
    if (bd_font_measure_text(font, buffer) <= max_width) {
      best = cursor;
      if (text[cursor - 1] == ' ') {
        last_space = cursor;
      }
    } else {
      break;
    }
  }
  if (last_space > 0 && last_space < best) {
    return (int)last_space;
  }
  return (int)best;
}

static int line_count(const bd_font *font, const char *text, int max_width,
                      int max_lines) {
  if (text == NULL || text[0] == '\0') {
    return 0;
  }
  if (max_lines <= 1 || bd_font_measure_text(font, text) <= max_width) {
    return 1;
  }
  int prefix = fit_prefix(font, text, max_width);
  if (prefix <= 0) {
    return 1;
  }
  const char *rest = text + prefix;
  while (*rest == ' ') {
    ++rest;
  }
  return *rest != '\0' ? 2 : 1;
}

static void draw_wrapped(bd_gfx *gfx, const bd_font *font, int x, int top,
                         int max_width, int line_height, bd_color color,
                         const char *text, int lines) {
  if (text == NULL || lines <= 0) {
    return;
  }
  if (lines == 1) {
    text_shadowed(gfx, font, x, baseline_for(font, top, line_height), max_width,
                  color, text, true);
    return;
  }
  int prefix = fit_prefix(font, text, max_width);
  if (prefix <= 0) {
    text_shadowed(gfx, font, x, baseline_for(font, top, line_height), max_width,
                  color, text, true);
    return;
  }
  char first[BD_HOME_TITLE_MAX];
  size_t copy = (size_t)prefix < sizeof(first) - 1 ? (size_t)prefix
                                                   : sizeof(first) - 1;
  memcpy(first, text, copy);
  first[copy] = '\0';
  while (copy > 0 && first[copy - 1] == ' ') {
    first[--copy] = '\0';
  }
  text_shadowed(gfx, font, x, baseline_for(font, top, line_height), max_width,
                color, first, false);
  const char *rest = text + prefix;
  while (*rest == ' ') {
    ++rest;
  }
  text_shadowed(gfx, font, x, baseline_for(font, top + line_height, line_height),
                max_width, color, rest, true);
}

static void draw_copy_block(const bd_home *home, bd_gfx *gfx,
                            const bd_home_entry *entry, bd_rect rect,
                            float alpha) {
  const bd_home_palette *palette = &home->palette;
  if (entry->title == NULL || entry->title[0] == '\0') {
    return;
  }
  int title_px = title_pixel_size(entry->w, entry->h);
  const bd_font *title_font = title_font_for(home, entry->w, entry->h);
  int max_width = rect.w - 28;
  if (max_width < 24) {
    return;
  }
  int title_lines = line_count(title_font, entry->title, max_width, 2);
  int title_lh = line_height_for(title_px, 1.18f);
  bool show_subtitle =
      entry->subtitle != NULL && entry->subtitle[0] != '\0' &&
      !(entry->w == 1 && entry->h == 1);
  const bd_font *sub_font = subtitle_font_for(home, entry->w, entry->h);
  int sub_px = entry->w >= 2 && entry->h >= 2 ? 15 : 14;
  int sub_lh = line_height_for(sub_px, 1.32f);
  int sub_lines = 0;
  if (show_subtitle) {
    int sub_max_lines = (entry->h >= 2) ? 2 : 1;
    sub_lines = line_count(sub_font, entry->subtitle, max_width, sub_max_lines);
  }
  int bottom = rect.y + rect.h - 14;
  int sub_top = bottom - sub_lines * sub_lh;
  int title_top = sub_top - (sub_lines > 0 ? 3 : 0) - title_lines * title_lh;
  draw_wrapped(gfx, title_font, rect.x + 14, title_top, max_width, title_lh,
               alpha_color(palette->cover_ink, alpha), entry->title, title_lines);
  if (sub_lines > 0) {
    bd_color sub_color = palette->cover_ink2;
    if (entry->running) {
      /* The last frame is arbitrary; lift the subtitle over the scrim. */
      sub_color = palette->cover_ink;
      sub_color.a = 0.84f;
    }
    draw_wrapped(gfx, sub_font, rect.x + 14, sub_top, max_width, sub_lh,
                 alpha_color(sub_color, alpha), entry->subtitle, sub_lines);
  }
}

static int badge_width(const bd_home *home, const char *text, bool dot) {
  return 14 + (dot ? 10 : 0) + bd_font_measure_text(home->fonts.badge13, text);
}

static void draw_badge(const bd_home *home, bd_gfx *gfx, int x, int y,
                       const char *text, bool dot, float background_alpha,
                       bd_color text_color, float alpha) {
  if (text == NULL || text[0] == '\0') {
    return;
  }
  int width = badge_width(home, text, dot);
  bd_rect badge = bd_rect_make(x, y, width, 20);
  bd_color background = bd_color_rgba(10, 9, 12, (uint8_t)(background_alpha * 255.0f));
  bd_gfx_fill_round_rect(gfx, badge, 4, alpha_color(background, alpha));
  int text_x = x + 7;
  if (dot) {
    bd_gfx_fill_round_rect(gfx, bd_rect_make(text_x, y + 7, 5, 5), 2,
                           alpha_color(home->accent.accent, alpha));
    text_x += 10;
  }
  bd_gfx_draw_text(gfx, home->fonts.badge13, text_x, y + 15,
                   alpha_color(text_color, alpha), text);
}

static void format_number(char *out, size_t size, int value) {
  if (value >= 0 && value <= 99) {
    snprintf(out, size, "%02d", value);
  } else {
    snprintf(out, size, "%d", value);
  }
}

static void format_time(char *out, size_t size, int seconds) {
  if (seconds < 0) {
    seconds = 0;
  }
  snprintf(out, size, "%02d:%02d", seconds / 60, seconds % 60);
}

static const bd_texture *running_frame_texture(const bd_home *home, int index) {
  const bd_home_entry *entry = &home->entries[index];
  if (!entry->running) {
    return NULL;
  }
  int running = bd_home_running_index(home, entry->id);
  if (running < 0) {
    return NULL;
  }
  return home->running[running].frame;
}

static void draw_live_body(const bd_home *home, bd_gfx *gfx, bd_rect rect,
                           int badge_bottom, float alpha) {
  const bd_home_palette *palette = &home->palette;
  const bd_home_live *live = &home->live;
  int x = rect.x + 16;
  int width = rect.w - 32;
  /* Start below the top-left live badge. */
  int title_top = badge_bottom + 4;
  int title_lh = line_height_for(25, 1.18f);
  text_shadowed(gfx, home->fonts.title25, x,
                baseline_for(home->fonts.title25, title_top, title_lh), width,
                alpha_color(palette->cover_ink, alpha), live->title, true);
  int artist_top = title_top + title_lh + 3;
  text_shadowed(gfx, home->fonts.body14, x,
                baseline_for(home->fonts.body14, artist_top, 18), width,
                alpha_color(palette->cover_ink2, alpha), live->artist, true);

  int prev_lh = line_height_for(17, 1.34f);
  int now_lh = line_height_for(22, 1.34f);
  int next_lh = prev_lh;
  int block_bottom = rect.y + rect.h - 58;
  int next_top = block_bottom - next_lh;
  int now_top = next_top - 3 - now_lh;
  int prev_top = now_top - 3 - prev_lh;
  bd_color dim = with_alpha(palette->cover_ink, 0.34f * alpha);
  bd_color bright = with_alpha(palette->cover_ink, 0.96f * alpha);
  text_shadowed(gfx, home->fonts.serif17, x,
                baseline_for(home->fonts.serif17, prev_top, prev_lh), width, dim,
                live->previous_line, true);
  const char *current = live->playing ? live->current_line : "已暂停";
  text_shadowed(gfx, home->fonts.serif22, x,
                baseline_for(home->fonts.serif22, now_top, now_lh), width,
                bright, current, true);
  text_shadowed(gfx, home->fonts.serif17, x,
                baseline_for(home->fonts.serif17, next_top, next_lh), width, dim,
                live->next_line, true);

  int bar_y = rect.y + rect.h - 49;
  int bar_width = rect.w - 32;
  bd_gfx_fill_round_rect(gfx, bd_rect_make(x, bar_y, bar_width, 3), 2,
                         with_alpha(bd_color_rgb(255, 255, 255), 0.22f * alpha));
  int fill = (int)((float)bar_width * live->progress);
  if (fill > 0) {
    bd_gfx_fill_round_rect(gfx, bd_rect_make(x, bar_y, fill, 3), 2,
                           alpha_color(home->accent.accent, alpha));
  }

  char elapsed[16];
  char duration[16];
  format_time(elapsed, sizeof(elapsed), live->elapsed_seconds);
  format_time(duration, sizeof(duration), live->duration_seconds);
  int time_baseline = rect.y + rect.h - 22;
  bd_color time_color = with_alpha(palette->cover_ink, 0.56f * alpha);
  bd_gfx_draw_text(gfx, home->fonts.badge13, x, time_baseline, time_color,
                   elapsed);
  bd_gfx_draw_text(gfx, home->fonts.badge13,
                   x + bd_font_measure_text(home->fonts.badge13, elapsed) + 10,
                   time_baseline, time_color, duration);

  int button = 26;
  bd_rect play = bd_rect_make(rect.x + rect.w - 16 - button,
                              rect.y + rect.h - 14 - button, button, button);
  bd_gfx_stroke_round_rect(gfx, play, button / 2, 1,
                           with_alpha(bd_color_rgb(255, 255, 255), 0.32f * alpha));
  bd_color icon = with_alpha(palette->cover_ink, 0.86f * alpha);
  if (live->playing) {
    bd_gfx_fill_rect(gfx, bd_rect_make(play.x + 9, play.y + 9, 2, 8), icon);
    bd_gfx_fill_rect(gfx, bd_rect_make(play.x + 15, play.y + 9, 2, 8), icon);
  } else {
    bd_gfx_fill_round_rect(gfx, bd_rect_make(play.x + 10, play.y + 9, 6, 8), 1,
                           icon);
  }
}

static void draw_tile(bd_home *home, bd_gfx *gfx, int index, bool focused) {
  bd_home_entry *entry = &home->entries[index];
  const bd_home_palette *palette = &home->palette;
  bool matches_tab = bd_home_item_matches_tab(home, index);
  bool show_focus = focused && matches_tab;
  bd_rect rect = bd_home_screen_rect(home, index);
  float alpha = 1.0f;
  bool closing = home->closing_active && home->closing_item == index;
  if (closing) {
    float t = (float)(home->now_ms - home->closing_start) / (float)BD_HOME_CLOSE_MS;
    t = bd_home_clampf(t, 0.0f, 1.0f);
    int inset = round_int((float)rect.w * 0.05f * t);
    rect.x += inset;
    rect.y += inset;
    rect.w -= inset * 2;
    rect.h -= inset * 2;
    alpha = 1.0f - t;
    if (rect.w <= 0 || rect.h <= 0) {
      return;
    }
  }
  if (alpha <= 0.0f || rect.x >= home->width || rect.x + rect.w <= 0) {
    return;
  }

  if (entry->custom_draw != NULL) {
    bd_gfx_fill_round_rect(gfx, rect, BD_HOME_GAP,
                           alpha_color(entry->item.placeholder, alpha));
    if (closing) {
      bd_gfx_push_clip(gfx, rect);
    }
    entry->custom_visible = true;
    entry->custom_draw(entry->custom_user, home, gfx, rect, true);
    if (closing) {
      bd_gfx_pop_clip(gfx);
    }
    if (!matches_tab) {
      bd_gfx_fill_round_rect(gfx, rect, BD_HOME_GAP,
                             with_alpha(bd_color_rgb(8, 7, 9), 0.46f * alpha));
    }
    if (show_focus && !closing) {
      bd_gfx_stroke_round_rect(gfx, rect, BD_HOME_GAP, 2, palette->focus_ring);
    }
    return;
  }

  const bd_texture *frame = running_frame_texture(home, index);
  const bd_texture *texture = frame != NULL ? frame : entry->texture;
  if (texture != NULL) {
    bd_gfx_draw_texture_cover(gfx, (bd_texture *)texture, rect,
                              bd_color_rgba(255, 255, 255, (uint8_t)(alpha * 255.0f)),
                              BD_HOME_GAP);
  } else {
    bd_color top = bd_color_mix(entry->item.placeholder,
                                bd_color_rgb(255, 248, 242), 0.16f);
    bd_gfx_fill_gradient(gfx, rect, BD_HOME_GAP, alpha_color(top, alpha),
                         alpha_color(entry->item.placeholder, alpha),
                         BD_GRADIENT_VERTICAL);
    bd_rect circle = bd_rect_make(rect.x + rect.w / 4, rect.y + rect.h / 4,
                                  rect.w / 2, rect.h / 2);
    bd_gfx_fill_round_rect(gfx, circle, circle.w / 2,
                           with_alpha(bd_color_rgb(255, 248, 242), 0.12f * alpha));
  }

  if (closing) {
    bd_gfx_push_clip(gfx, rect);
  }
  if (!show_focus) {
    bd_gfx_fill_gradient(gfx, rect, BD_HOME_GAP,
                         alpha_color(palette->tile_shade_a, alpha),
                         alpha_color(palette->tile_shade_b, alpha),
                         BD_GRADIENT_VERTICAL);
    if (!matches_tab) {
      bd_gfx_fill_round_rect(gfx, rect, BD_HOME_GAP,
                             with_alpha(bd_color_rgb(8, 7, 9), 0.46f * alpha));
    }
  }

  int badge_top = rect.y + (entry->row == 0 ? 80 : 12);
  char number[16];
  format_number(number, sizeof(number), entry->item.badge_number);
  char badge_text[64];
  if (entry->running) {
    /*
     * A running tile shows the app's last frame.  Keep only the top-right
     * "running" badge; the old top-left number duplicated it.
     */
    snprintf(badge_text, sizeof(badge_text), "运行中 · %s", number);
    int width = badge_width(home, badge_text, true);
    draw_badge(home, gfx, rect.x + rect.w - 12 - width, badge_top, badge_text,
               true, 0.62f, palette->cover_ink, alpha);
  } else {
    bool badge_dot = false;
    if ((entry->item.flags & BD_HOME_ITEM_LIVE) != 0) {
      badge_dot = true;
      snprintf(badge_text, sizeof(badge_text), "%s · %s",
               home->live.playing ? "正在播放" : "已暂停", number);
    } else {
      snprintf(badge_text, sizeof(badge_text), "%s", number);
    }
    draw_badge(home, gfx, rect.x + 12, badge_top, badge_text, badge_dot, 0.44f,
               with_alpha(palette->cover_ink, 0.82f), alpha);
  }

  if (entry->running) {
    /*
     * Last frames are arbitrary application pixels; a cheap bottom-up scrim
     * keeps the bottom-left title and subtitle readable on bright frames.
     */
    int scrim_height = rect.h / 2;
    bd_color clear = with_alpha(bd_color_rgb(8, 7, 9), 0.0f);
    bd_color dark = with_alpha(bd_color_rgb(8, 7, 9), 0.70f * alpha);
    bd_gfx_fill_gradient(
        gfx,
        bd_rect_make(rect.x, rect.y + rect.h - scrim_height, rect.w,
                     scrim_height),
        0, clear, dark, BD_GRADIENT_VERTICAL);
  }

  bool live_body = (entry->item.flags & BD_HOME_ITEM_LIVE) != 0 && show_focus &&
                   !entry->running;
  if (live_body) {
    draw_live_body(home, gfx, rect, badge_top + 20, alpha);
  } else {
    draw_copy_block(home, gfx, entry, rect, alpha);
  }

  if (closing) {
    bd_gfx_pop_clip(gfx);
  }
  if (show_focus && !closing) {
    bd_gfx_stroke_round_rect(gfx, rect, BD_HOME_GAP, 2, palette->focus_ring);
  }
}

static int image_target_size(const bd_home_entry *entry) {
  int max_dimension = entry->rect.w > entry->rect.h ? entry->rect.w : entry->rect.h;
  if (max_dimension <= 256) {
    return 256;
  }
  if (max_dimension <= 384) {
    return 384;
  }
  return 512;
}

static void draw_wall(bd_home *home, bd_gfx *gfx) {
  bd_rect world = bd_rect_make(home->pan - BD_HOME_PREFETCH, 0,
                               home->width + BD_HOME_PREFETCH * 2,
                               home->height);
  int prefetch[BD_HOME_MAX_QUERY];
  int visited = 0;
  int buckets = 0;
  int prefetch_count =
      bd_home_query_indexed(home, world, prefetch, BD_HOME_MAX_QUERY,
                             &visited, &buckets);
  int visible[BD_HOME_MAX_QUERY];
  int visible_count = 0;

  bd_gfx_image_begin_set(gfx);
  for (int i = 0; i < prefetch_count; ++i) {
    int index = prefetch[i];
    bd_home_entry *entry = &home->entries[index];
    if (entry->image == NULL || entry->image[0] == '\0') {
      entry->texture = NULL;
    } else {
      int size = image_target_size(entry);
      bd_texture *texture = NULL;
      int focus_x = 0;
      int focus_y = 0;
      if (home->focus >= 0 && home->focus < home->count) {
        const bd_rect focus = home->entries[home->focus].rect;
        focus_x = focus.x + focus.w / 2;
        focus_y = focus.y + focus.h / 2;
      }
      int center_x = entry->rect.x + entry->rect.w / 2;
      int center_y = entry->rect.y + entry->rect.h / 2;
      int distance = (abs(center_x - focus_x) + abs(center_y - focus_y)) / 32;
      bd_image_state state = bd_gfx_image_request(
          gfx, entry->image, size, size, distance, &texture);
      bd_texture *ready = state == BD_IMAGE_READY ? texture : NULL;
      if (ready != entry->texture && bd_home_item_visible(home, index)) {
        bd_home_damage_item(home, index);
      }
      entry->texture = ready;
    }
    if (bd_home_item_visible(home, index) && visible_count < BD_HOME_MAX_QUERY) {
      visible[visible_count++] = index;
    }
  }
  bd_gfx_image_end_set(gfx);

  for (int i = 0; i < home->custom_count; ++i) {
    int index = home->custom_items[i];
    if (index < 0 || index >= home->count) {
      continue;
    }
    bd_home_entry *entry = &home->entries[index];
    if (entry->custom_draw != NULL && entry->custom_visible &&
        !bd_home_item_visible(home, index)) {
      entry->custom_visible = false;
      entry->custom_draw(entry->custom_user, home, gfx,
                         bd_home_screen_rect(home, index), false);
    }
  }

  if (home->style.ambient_solid) {
    bd_gfx_fill_rect(gfx, bd_rect_make(0, 0, home->width, home->height),
                     home->palette.bg_deep);
  } else {
    bd_gfx_fill_gradient(gfx, bd_rect_make(0, 0, home->width, home->height), 0,
                         home->palette.bg_deep, home->palette.bg,
                         BD_GRADIENT_VERTICAL);
  }

  int focus_index = home->focus;
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 0; i < visible_count; ++i) {
      int index = visible[i];
      bool focused = index == focus_index;
      if ((pass == 0 && focused) || (pass == 1 && !focused)) {
        continue;
      }
      draw_tile(home, gfx, index, focused);
    }
  }

  bd_color clear = bd_color_rgba(0, 0, 0, 0);
  bd_color edge = bd_color_rgba(0, 0, 0, 133);
  bd_gfx_fill_gradient(gfx, bd_rect_make(0, 0, 116, home->height), 0, edge,
                       clear, BD_GRADIENT_HORIZONTAL);
  bd_gfx_fill_gradient(gfx, bd_rect_make(home->width - 116, 0, 116, home->height),
                       0, clear, edge, BD_GRADIENT_HORIZONTAL);
  bd_gfx_fill_gradient(gfx, bd_rect_make(0, home->height - 48, home->width, 48),
                       0, clear, bd_color_rgba(0, 0, 0, 66), BD_GRADIENT_VERTICAL);

  home->stats.visited_items = visited;
  home->stats.visible_items = visible_count;
  home->stats.drawn_items = visible_count;
  home->stats.buckets_visited = buckets;
  home->stats.focus_index = focus_index;
  home->stats.total_items = home->count;
}

static bd_ui_wifi_state home_wifi_state(const bd_home *home) {
  if (!home->status.wifi_known) {
    return BD_UI_WIFI_UNKNOWN;
  }
  return home->status.wifi ? BD_UI_WIFI_CONNECTED : BD_UI_WIFI_DISCONNECTED;
}

static void draw_top_bar(bd_home *home, bd_gfx *gfx) {
  const bd_home_palette *palette = &home->palette;
  bd_rect strip = bd_rect_make(0, 0, home->width, BD_HOME_TOP_STRIP);
  if (home->style.blur_enabled) {
    bd_gfx_blur_region_feathered(gfx, strip, 0, palette->chrome_strong,
                                 palette->chrome_base, BD_GRADIENT_VERTICAL,
                                 BD_BLUR_EDGE_BOTTOM, 36);
  } else {
    bd_color fade = palette->chrome_base;
    fade.a = 0.0f;
    bd_gfx_fill_gradient3(gfx, strip, 0, palette->chrome_strong,
                          palette->chrome_base, fade, BD_GRADIENT_VERTICAL);
  }

  /*
   * The left brand mark and wordmark are gone.  Categories, Wi-Fi, battery and
   * clock stay; there are no search/gear/avatar buttons.
   */
  int x = 24;
  for (int i = 0; i < BD_HOME_TAB_COUNT; ++i) {
    bd_color color = i == home->tab ? palette->ink : palette->ink3;
    bd_gfx_draw_text(gfx, home->fonts.tab18, x, 41, color, TAB_NAMES[i]);
    if (i == home->tab) {
      bd_gfx_fill_round_rect(gfx, bd_rect_make(x, 54, 17, 2), 1,
                             home->accent.accent);
    }
    x += bd_font_measure_text(home->fonts.tab18, TAB_NAMES[i]) + 27;
  }

  int clock_right = home->width - 24;
  int clock_width = bd_font_measure_text(home->fonts.body16, home->clock);
  if (clock_width < 44) {
    clock_width = 44;
  }
  int clock_x = clock_right - clock_width;
  bd_gfx_draw_text(gfx, home->fonts.body16, clock_x, 41, palette->ink2,
                   home->clock);
  int battery_left = clock_x - 12 - BD_UI_BATTERY_ICON_W;
  bd_ui_draw_battery(gfx, battery_left + (BD_UI_BATTERY_ICON_W - 1) / 2, 35,
                     home->status.battery_percent, palette->ink3,
                     palette->ink2);
  int wifi_left = battery_left - 12 - BD_UI_WIFI_ICON_W;
  bd_ui_draw_wifi(gfx, wifi_left + (BD_UI_WIFI_ICON_W - 1) / 2, 35,
                  home_wifi_state(home), palette->ink2);

  if (home->running_count > 0) {
    char chip[64];
    snprintf(chip, sizeof(chip), "%d 运行中", home->running_count);
    int text_width = bd_font_measure_text(home->fonts.badge13, chip);
    int chip_width = text_width + 20;
    int chip_x = wifi_left - 20 - chip_width;
    if (chip_x < x + 16) {
      chip_x = x + 16;
    }
    bd_rect rect = bd_rect_make(chip_x, 21, chip_width, 26);
    bd_gfx_fill_round_rect(gfx, rect, 5,
                           with_alpha(home->accent.accent, 0.09f));
    bd_gfx_stroke_round_rect(gfx, rect, 5, 1, palette->line);
    bd_gfx_draw_text(gfx, home->fonts.badge13, chip_x + 10, 39, palette->ink2,
                     chip);
  }
}

typedef struct hint_item {
  const char *key;
  const char *label;
} hint_item;

static int keycap_width(const bd_home *home, const char *key) {
  int width = bd_font_measure_text(home->fonts.badge13, key) + 12;
  return width < 22 ? 22 : width;
}

static int draw_keycap(const bd_home *home, bd_gfx *gfx, int x, int y,
                       const char *key, float alpha) {
  int width = keycap_width(home, key);
  bd_rect rect = bd_rect_make(x, y, width, 22);
  bd_gfx_fill_round_rect(gfx, rect, 4,
                         with_alpha(home->palette.ink, 0.04f * alpha));
  bd_gfx_stroke_round_rect(gfx, rect, 4, 1,
                           with_alpha(home->palette.ink, 0.22f * alpha));
  bd_gfx_draw_text(gfx, home->fonts.badge13, x + (width -
                     bd_font_measure_text(home->fonts.badge13, key)) / 2,
                   y + 15, with_alpha(home->palette.ink2, alpha), key);
  return width;
}

static int draw_hint_group(const bd_home *home, bd_gfx *gfx,
                           const hint_item *items, int count, int x, int y,
                           float alpha) {
  for (int i = 0; i < count; ++i) {
    int key_width = draw_keycap(home, gfx, x, y, items[i].key, alpha);
    int label_x = x + key_width + 6;
    bd_gfx_draw_text(gfx, home->fonts.body14, label_x, y + 16,
                     with_alpha(home->palette.ink2, alpha), items[i].label);
    x = label_x + bd_font_measure_text(home->fonts.body14, items[i].label) + 16;
  }
  return x;
}

static void hint_items(bd_home *home, hint_item *left, int left_capacity,
                       int *left_count_out, hint_item *right,
                       int right_capacity, int *right_count_out) {
  int left_count = 0;
  int right_count = 0;
#define ADD_LEFT(k, v)                                                             \
  do {                                                                             \
    if (left_count < left_capacity) {                                              \
      left[left_count].key = (k);                                                  \
      left[left_count].label = (v);                                                \
      ++left_count;                                                                \
    }                                                                              \
  } while (0)
#define ADD_RIGHT(k, v)                                                            \
  do {                                                                             \
    if (right_count < right_capacity) {                                            \
      right[right_count].key = (k);                                                \
      right[right_count].label = (v);                                              \
      ++right_count;                                                               \
    }                                                                              \
  } while (0)

  if (home->dialog_visible) {
    ADD_LEFT("←→", "选择");
    ADD_RIGHT("A", "确定");
    ADD_RIGHT("B", "取消");
  } else if (home->quick_open || home->quick_anim) {
    if (home->quick_sel < home->running_count) {
      ADD_LEFT("↑↓", "后台程序");
      ADD_RIGHT("A", "恢复");
      ADD_RIGHT("X", "结束");
      ADD_RIGHT("B", "关闭");
    } else {
      ADD_LEFT("↑↓", "移动");
      int option = home->quick_sel - home->running_count;
      if (option == 0 || option == 1) {
        ADD_LEFT("←→", "调整");
      }
      /* Wi-Fi is a sampled status row, not a local switch. */
      if (option != 2) {
        ADD_RIGHT("A", "切换");
      }
      ADD_RIGHT("B", "关闭");
    }
  } else {
    ADD_LEFT("MENU", "菜单");
    ADD_LEFT("START", "快捷设置");
    if (home->running_count > 0) {
      ADD_LEFT("HOME", "后台");
    }
    ADD_RIGHT("L1", "分类");
    ADD_RIGHT("R1", "分类");
    const bd_home_entry *entry =
        home->focus >= 0 && home->focus < home->count
            ? &home->entries[home->focus]
            : NULL;
    if (entry != NULL && entry->running) {
      ADD_RIGHT("A", "恢复");
      ADD_RIGHT("X", "结束");
      ADD_RIGHT("HOME", "最近");
    } else {
      if (entry != NULL && (entry->item.flags & BD_HOME_ITEM_LIVE) != 0) {
        ADD_RIGHT("X", home->live.playing ? "暂停" : "播放");
        ADD_RIGHT("Y", "下一首");
      }
      if (entry != NULL) {
        ADD_RIGHT("A", "打开");
      }
    }
  }
#undef ADD_LEFT
#undef ADD_RIGHT
  if (left_count_out != NULL) {
    *left_count_out = left_count;
  }
  if (right_count_out != NULL) {
    *right_count_out = right_count;
  }
}

static void draw_hint_bar(bd_home *home, bd_gfx *gfx) {
  float progress = bd_home_hint_progress(home);
  if (progress <= 0.0f) {
    return;
  }
  const bd_home_palette *palette = &home->palette;
  bd_rect strip = bd_rect_make(0, home->height - BD_HOME_HINT_STRIP, home->width,
                              BD_HOME_HINT_STRIP);
  bd_color fade = palette->chrome_base;
  fade.a = 0.0f;
  if (home->style.blur_enabled) {
    bd_gfx_blur_region_feathered(gfx, strip, 0, fade, palette->chrome_strong,
                                 BD_GRADIENT_VERTICAL, BD_BLUR_EDGE_TOP, 30);
  } else {
    bd_gfx_fill_gradient3(gfx, strip, 0, fade, palette->chrome_base,
                          palette->chrome_strong, BD_GRADIENT_VERTICAL);
  }

  hint_item left[8];
  hint_item right[8];
  int left_count = 0;
  int right_count = 0;
  hint_items(home, left, 8, &left_count, right, 8, &right_count);

  int top = home->height - BD_HOME_HINT_BAR + round_int((1.0f - progress) * 8.0f);
  int x = 24;
  x = draw_hint_group(home, gfx, left, left_count, x, top + 18, progress);
  bd_gfx_fill_rect(gfx, bd_rect_make(x + 8, top + 20, 1, 18),
                   with_alpha(palette->line_strong, progress));
  x = draw_hint_group(home, gfx, right, right_count, x + 27, top + 18, progress);
  (void)x;
  const char *note = "松开收起";
  int note_width = bd_font_measure_text(home->fonts.badge13, note);
  bd_gfx_draw_text(gfx, home->fonts.badge13, home->width - 24 - note_width,
                   top + 34, with_alpha(palette->ink3, progress), note);
}

static const bd_texture *live_cover_texture(const bd_home *home) {
  for (int i = 0; i < home->count; ++i) {
    if ((home->entries[i].item.flags & BD_HOME_ITEM_LIVE) != 0) {
      const bd_texture *frame = running_frame_texture(home, i);
      return frame != NULL ? frame : home->entries[i].texture;
    }
  }
  return NULL;
}

static void draw_quick_icon(bd_gfx *gfx, int icon, int x, int y,
                            bd_color color, bd_ui_wifi_state wifi_state) {
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

enum {
  /* Row content shares the header's 18 px optical inset. */
  QUICK_ROW_RIGHT_PAD = 18,
  /* Fixed value column keeps "100" clear of the slider knob. */
  QUICK_VALUE_COL_W = 40,
  QUICK_VALUE_GAP = 12,
  QUICK_TRACK_W = 92
};

static void draw_slider(bd_home *home, bd_gfx *gfx, int x, int y, int value,
                        float alpha) {
  int width = QUICK_TRACK_W;
  bd_gfx_fill_round_rect(gfx, bd_rect_make(x, y, width, 3), 1,
                         alpha_color(home->palette.line_strong, alpha));
  int fill = width * value / 100;
  if (fill > 0) {
    bd_gfx_fill_round_rect(gfx, bd_rect_make(x, y, fill, 3), 1,
                           alpha_color(home->accent.accent, alpha));
  }
  bd_rect knob = bd_rect_make(x + fill - 4, y - 3, 9, 9);
  bd_gfx_fill_round_rect(gfx, knob, 4, alpha_color(home->palette.surface, alpha));
  bd_gfx_stroke_round_rect(gfx, knob, 4, 1,
                           alpha_color(home->accent.accent, alpha));
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

static void draw_quick_row_background(bd_home *home, bd_gfx *gfx, bd_rect row,
                                      bool focused) {
  if (!focused) {
    return;
  }
  bd_gfx_fill_round_rect(gfx, row, 5, home->palette.surface2);
  bd_gfx_stroke_round_rect(gfx, row, 5, 2, home->accent.accent);
}

static void draw_quick_scrollbar(const bd_home *home, bd_gfx *gfx,
                                 bd_rect panel,
                                 const bd_home_quick_layout *layout) {
  if (layout->max_scroll <= 0 || layout->visible_rows <= 0 ||
      home->running_count <= 0) {
    return;
  }
  int x = panel.x + panel.w - 10;
  int y = panel.y + layout->rows_start + 2;
  int track_h = layout->visible_rows * BD_HOME_QUICK_ROW_H - 4;
  int thumb_h = track_h * layout->visible_rows / home->running_count;
  if (thumb_h < 24) {
    thumb_h = 24;
  }
  if (thumb_h > track_h) {
    thumb_h = track_h;
  }
  int travel = track_h - thumb_h;
  int thumb_y =
      y + travel * layout->scroll / layout->max_scroll;
  bd_gfx_fill_round_rect(gfx, bd_rect_make(x, y, BD_HOME_QUICK_SCROLLBAR_W,
                                           track_h),
                         1, home->palette.line);
  bd_gfx_fill_round_rect(
      gfx,
      bd_rect_make(x, thumb_y, BD_HOME_QUICK_SCROLLBAR_W, thumb_h), 1,
      home->palette.ink3);
}

/*
 * Ordered 8x8 Bayer threshold, adapted from CUI's pattern meter
 * (commit 86d9781bbc3c35c264ec46287ad89c1d86a4c4de, MIT).  One texel is one
 * dither cell; the texture is drawn with nearest filtering at an integer
 * scale, so cells stay sharp at 1x.
 */
static float memory_bayer8(int x, int y) {
  int qx = x & 7;
  int qy = y & 7;
  int a = qx ^ qy;
  int m = ((a & 1) << 5) | ((qy & 1) << 4) | ((a & 2) << 2) |
          ((qy & 2) << 1) | ((a & 4) >> 1) | ((qy & 4) >> 2);
  return ((float)m + 0.5f) / 64.0f;
}

static uint32_t memory_color_rgb(bd_color color) {
  int r = (int)(color.r * 255.0f + 0.5f);
  int g = (int)(color.g * 255.0f + 0.5f);
  int b = (int)(color.b * 255.0f + 0.5f);
  if (r < 0) r = 0;
  if (r > 255) r = 255;
  if (g < 0) g = 0;
  if (g > 255) g = 255;
  if (b < 0) b = 0;
  if (b > 255) b = 255;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static bool ensure_memory_dither(bd_home *home, bd_gfx *gfx, int memory,
                                 int width, int height) {
  if (home == NULL || gfx == NULL || memory <= 0 || width <= 0 || height <= 0) {
    return false;
  }
  int columns = (width + BD_HOME_QUICK_MEM_CELL - 1) / BD_HOME_QUICK_MEM_CELL;
  int rows = (height + BD_HOME_QUICK_MEM_CELL - 1) / BD_HOME_QUICK_MEM_CELL;
  if (columns <= 0 || rows <= 0 || columns > 1024 || rows > 64) {
    return false;
  }
  uint32_t rgb = memory_color_rgb(home->accent.accent);
  if (home->memory_dither_valid && home->memory_dither != NULL &&
      home->memory_dither_value == memory && home->memory_dither_rgb == rgb &&
      home->memory_dither_width == columns &&
      home->memory_dither_height == rows) {
    return true;
  }
  size_t count = (size_t)columns * (size_t)rows;
  uint8_t *pixels = (uint8_t *)calloc(count, 4u);
  if (pixels == NULL) {
    return false;
  }
  float density = (float)(memory > 1024 ? 1024 : memory) / 1024.0f;
  uint8_t r = (uint8_t)((rgb >> 16) & 0xffu);
  uint8_t g = (uint8_t)((rgb >> 8) & 0xffu);
  uint8_t b = (uint8_t)(rgb & 0xffu);
  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < columns; ++x) {
      if (memory_bayer8(x, y) >= density) {
        continue;
      }
      uint8_t *pixel = pixels + ((size_t)y * (size_t)columns + (size_t)x) * 4u;
      pixel[0] = r;
      pixel[1] = g;
      pixel[2] = b;
      pixel[3] = 255;
    }
  }
  bool ok = false;
  if (home->memory_dither == NULL) {
    home->memory_dither = bd_gfx_texture_create(gfx, columns, rows,
                                                BD_TEXTURE_NEAREST, pixels);
    ok = home->memory_dither != NULL;
  } else {
    ok = bd_gfx_texture_update(gfx, home->memory_dither, columns, rows, pixels);
    if (!ok) {
      bd_gfx_texture_destroy(gfx, home->memory_dither);
      home->memory_dither = bd_gfx_texture_create(gfx, columns, rows,
                                                  BD_TEXTURE_NEAREST, pixels);
      ok = home->memory_dither != NULL;
    }
  }
  free(pixels);
  if (!ok) {
    home->memory_dither_valid = false;
    return false;
  }
  home->memory_dither_width = columns;
  home->memory_dither_height = rows;
  home->memory_dither_value = memory;
  home->memory_dither_rgb = rgb;
  home->memory_dither_valid = true;
  return true;
}

static void draw_memory_strip(bd_home *home, bd_gfx *gfx, bd_rect bar,
                              int memory) {
  if (home == NULL || gfx == NULL) {
    return;
  }
  bd_gfx_fill_round_rect(gfx, bar, bar.h / 2, home->palette.line);
  if (memory <= 0 || !ensure_memory_dither(home, gfx, memory, bar.w, bar.h)) {
    return;
  }
  int draw_w = home->memory_dither_width * BD_HOME_QUICK_MEM_CELL;
  bd_gfx_push_clip(gfx, bar);
  bd_gfx_draw_texture_round(
      gfx, home->memory_dither,
      bd_rect_make(bar.x, bar.y, draw_w, bar.h),
      bd_color_rgba(255, 255, 255, 255), bar.h / 2);
  bd_gfx_pop_clip(gfx);
}

static void draw_quick(bd_home *home, bd_gfx *gfx) {
  float progress = bd_home_quick_progress(home);
  if (progress <= 0.0f) {
    return;
  }
  const bd_home_palette *palette = &home->palette;
  bd_rect scrim = bd_rect_make(0, 0, home->width, home->height);
  bd_gfx_fill_rect(gfx, scrim, palette->scrim);

  bd_rect panel = bd_home_quick_panel_rect(home);
  if (panel.x >= home->width) {
    return;
  }
  /* Steam-style edge panel: full height, flush to the side, square outline. */
  bd_gfx_fill_rect(gfx, panel, palette->surface);
  bd_gfx_fill_rect(gfx, bd_rect_make(panel.x, panel.y, 1, panel.h),
                   palette->line_strong);
  bd_gfx_push_clip(gfx, panel);

  bd_gfx_draw_text(gfx, home->fonts.serif25, panel.x + 18, panel.y + 42,
                   palette->ink, "快捷设置");
  char battery[32];
  if (home->status.battery_percent < 0) {
    snprintf(battery, sizeof(battery), "电量 未知");
  } else {
    snprintf(battery, sizeof(battery), "电量 %d%%", home->status.battery_percent);
  }
  bd_gfx_draw_text(gfx, home->fonts.badge13,
                   panel.x + panel.w - 18 -
                       bd_font_measure_text(home->fonts.badge13, battery),
                   panel.y + 42, palette->ink3, battery);
  bd_gfx_fill_rect(gfx, bd_rect_make(panel.x + 16, panel.y + 70, panel.w - 32, 1),
                   palette->line);

  bd_rect now = bd_rect_make(panel.x + 16, panel.y + 82, panel.w - 32, 78);
  bd_gfx_fill_round_rect(gfx, now, 5, palette->surface2);
  bd_gfx_stroke_round_rect(gfx, now, 5, 1, palette->line);
  bd_rect cover = bd_rect_make(now.x + 10, now.y + 10, 58, 58);
  const bd_texture *live_cover = live_cover_texture(home);
  if (live_cover != NULL) {
    bd_gfx_draw_texture_cover(gfx, (bd_texture *)live_cover, cover,
                              bd_color_rgba(255, 255, 255, 255), 5);
  } else {
    bd_gfx_fill_round_rect(gfx, cover, 5, palette->surface3);
  }
  int text_x = now.x + 79;
  bd_gfx_draw_text(gfx, home->fonts.badge13, text_x, now.y + 20,
                   home->accent.accent, "正在播放");
  bd_gfx_draw_text(gfx, home->fonts.title18, text_x, now.y + 43, palette->ink,
                   home->live.title);
  bd_gfx_draw_text(gfx, home->fonts.badge13, text_x, now.y + 61, palette->ink3,
                   home->live.artist);
  int now_bar_width = now.w - 79 - 10 - 44;
  bd_gfx_fill_round_rect(gfx, bd_rect_make(text_x, now.y + 69, now_bar_width, 3),
                         2, palette->line_strong);
  int now_fill = (int)((float)now_bar_width * home->live.progress);
  if (now_fill > 0) {
    bd_gfx_fill_round_rect(gfx,
                           bd_rect_make(text_x, now.y + 69, now_fill, 3), 2,
                           home->accent.accent);
  }
  bd_rect play = bd_rect_make(now.x + now.w - 40, now.y + 24, 30, 30);
  bd_gfx_stroke_round_rect(gfx, play, 15, 1, palette->line_strong);
  bd_color play_icon = palette->ink2;
  if (home->live.playing) {
    bd_gfx_fill_rect(gfx, bd_rect_make(play.x + 11, play.y + 11, 2, 8),
                     play_icon);
    bd_gfx_fill_rect(gfx, bd_rect_make(play.x + 17, play.y + 11, 2, 8),
                     play_icon);
  } else {
    bd_gfx_fill_round_rect(gfx, bd_rect_make(play.x + 12, play.y + 11, 6, 8), 1,
                           play_icon);
  }

  int run_head = panel.y + 188;
  bd_gfx_draw_text(gfx, home->fonts.body14, panel.x + 18, run_head,
                   palette->ink3, "正在运行");
  char count[32];
  int max_background =
      home->status.max_background > 0 ? home->status.max_background : 3;
  snprintf(count, sizeof(count), "%d / %d", home->running_count,
           max_background);
  bd_gfx_draw_text(gfx, home->fonts.body14,
                   panel.x + panel.w - 18 -
                       bd_font_measure_text(home->fonts.body14, count),
                   run_head, palette->ink3, count);
  int memory = running_memory(home);
  bd_rect mem_bar = bd_rect_make(panel.x + 18,
                                 panel.y + BD_HOME_QUICK_MEM_BAR_Y,
                                 panel.w - 36, BD_HOME_QUICK_MEM_BAR_H);
  draw_memory_strip(home, gfx, mem_bar, memory);

  bd_home_quick_layout layout = bd_home_quick_layout_get(home);
  int rows_start = panel.y + layout.rows_start;
  if (home->running_count == 0) {
    bd_gfx_draw_text(gfx, home->fonts.body14, panel.x + 18, rows_start + 18,
                     palette->ink3, "没有后台程序。启动应用后会出现在这里。");
  } else {
    for (int slot = 0; slot < layout.visible_rows; ++slot) {
      int i = layout.scroll + slot;
      if (i >= home->running_count) {
        break;
      }
      const bd_home_running *run = &home->running[i];
      bd_rect row =
          bd_rect_make(panel.x + 16, rows_start + slot * BD_HOME_QUICK_ROW_H,
                       panel.w - 32, BD_HOME_QUICK_ROW_BOX);
      draw_quick_row_background(home, gfx, row, home->quick_sel == i);
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
      bd_gfx_draw_text_ellipsized(gfx, home->fonts.body16, row.x + 48,
                                  row.y + 28, row.w - 180,
                                  home->quick_sel == i ? palette->ink
                                                       : palette->ink2,
                                  name);
      char meta[64];
      if (run->memory_mb < 0) {
        snprintf(meta, sizeof(meta), "%s · 内存未知",
                 run->paused ? "已暂停" : "运行中");
      } else {
        snprintf(meta, sizeof(meta), "%s · %d MB",
                 run->paused ? "已暂停" : "运行中", run->memory_mb);
      }
      bd_gfx_draw_text(gfx, home->fonts.badge13,
                       row.x + row.w - QUICK_ROW_RIGHT_PAD -
                           bd_font_measure_text(home->fonts.badge13, meta),
                       row.y + 28,
                       home->quick_sel == i ? palette->ink2 : palette->ink3,
                       meta);
    }
    draw_quick_scrollbar(home, gfx, panel, &layout);
  }
  int foot_baseline = panel.y + layout.foot_baseline;
  char memory_text[64];
  if (memory < 0) {
    snprintf(memory_text, sizeof(memory_text), "后台内存 未知");
  } else {
    snprintf(memory_text, sizeof(memory_text), "后台内存 %d / 1024 MB", memory);
  }
  bd_gfx_draw_text(gfx, home->fonts.badge13, panel.x + 18, foot_baseline,
                   palette->ink3, memory_text);

  int list_y = panel.y + layout.controls_y;
  for (int i = 0; i < BD_HOME_QUICK_CONTROL_COUNT; ++i) {
    bd_rect row = bd_rect_make(panel.x + 16,
                               list_y + i * BD_HOME_QUICK_CONTROL_H,
                               panel.w - 32, BD_HOME_QUICK_CONTROL_H);
    bool focused = home->quick_sel == home->running_count + i;
    draw_quick_row_background(home, gfx, row, focused);
    bd_color icon_color = focused ? home->accent.accent : palette->ink3;
    draw_quick_icon(gfx, i, row.x + 10, row.y + 14, icon_color,
                    home_wifi_state(home));
    static const char *const LABELS[4] = {"亮度", "音量", "Wi-Fi", "蓝牙"};
    bd_gfx_draw_text(gfx, home->fonts.body16, row.x + 40, row.y + 29,
                     focused ? palette->ink : palette->ink2, LABELS[i]);
    if (i == 0 || i == 1) {
      int value = i == 0 ? home->brightness : home->volume;
      int value_right = row.x + row.w - QUICK_ROW_RIGHT_PAD;
      if (value < 0) {
        const char *unknown = "未知";
        bd_gfx_draw_text(gfx, home->fonts.body14,
                         value_right -
                             bd_font_measure_text(home->fonts.body14, unknown),
                         row.y + 29, palette->ink3, unknown);
      } else {
        int value_left = value_right - QUICK_VALUE_COL_W;
        draw_slider(home, gfx,
                    value_left - QUICK_VALUE_GAP - QUICK_TRACK_W, row.y + 23,
                    value, 1.0f);
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
                       row.x + row.w - QUICK_ROW_RIGHT_PAD -
                           bd_font_measure_text(home->fonts.body14, value),
                       row.y + 29, palette->ink2, value);
    } else if (i == 3) {
      const char *value = "不可用";
      bd_gfx_draw_text(gfx, home->fonts.body14,
                       row.x + row.w - QUICK_ROW_RIGHT_PAD -
                           bd_font_measure_text(home->fonts.body14, value),
                       row.y + 29, palette->ink3, value);
    }
  }

  bd_gfx_pop_clip(gfx);
}

static void draw_dialog_button(bd_home *home, bd_gfx *gfx, bd_rect rect,
                               const char *label, bool primary, bool focused,
                               float alpha) {
  bd_color fill = primary ? home->accent.accent : home->palette.surface2;
  bd_color text = primary ? home->accent.on_accent : home->palette.ink;
  bd_gfx_fill_round_rect(gfx, rect, 9, alpha_color(fill, alpha));
  if (!primary) {
    bd_gfx_stroke_round_rect(gfx, rect, 9, 1,
                             alpha_color(home->palette.line_strong, alpha));
  }
  bd_gfx_draw_text(gfx, home->fonts.body15,
                   rect.x + (rect.w - bd_font_measure_text(home->fonts.body15,
                                                            label)) / 2,
                   rect.y + 28, alpha_color(text, alpha), label);
  if (focused) {
    bd_rect outline = bd_rect_make(rect.x - 3, rect.y - 3, rect.w + 6, rect.h + 6);
    bd_gfx_stroke_round_rect(gfx, outline, 11, 2,
                             alpha_color(home->palette.ink, alpha));
  }
}

static const char *dialog_item_title(const bd_home *home, int index,
                                     const char *fallback) {
  if (index >= 0 && index < home->count && home->entries[index].title != NULL) {
    return home->entries[index].title;
  }
  return fallback;
}

static int dialog_item_by_id(const bd_home *home, const char *id) {
  if (id == NULL || id[0] == '\0') {
    return -1;
  }
  for (int i = 0; i < home->count; ++i) {
    if (home->entries[i].id != NULL && strcmp(home->entries[i].id, id) == 0) {
      return i;
    }
  }
  return -1;
}

static void draw_dialog(bd_home *home, bd_gfx *gfx) {
  float progress = bd_home_dialog_progress(home);
  if (progress <= 0.0f || !home->dialog_visible) {
    return;
  }
  const bd_home_palette *palette = &home->palette;
  bool background_full = home->dialog_kind == BD_HOME_DIALOG_BACKGROUND_FULL;
  bd_color scrim = palette->scrim;
  scrim.a *= progress;
  bd_gfx_fill_rect(gfx, bd_rect_make(0, 0, home->width, home->height), scrim);

  int card_width = 500;
  int card_height = background_full ? 260 : 230;
  if (background_full && home->dialog_error[0] != '\0') {
    card_height += 32;
  }
  int card_x = (home->width - card_width) / 2;
  int card_y = (home->height - card_height) / 2 + round_int((1.0f - progress) * 10.0f);
  bd_rect card = bd_rect_make(card_x, card_y, card_width, card_height);
  bd_gfx_fill_round_rect(gfx, card, 16, alpha_color(palette->surface, progress));
  bd_gfx_stroke_round_rect(gfx, card, 16, 1, alpha_color(palette->line, progress));

  int item = home->dialog_item;
  const char *name = dialog_item_title(home, item, "程序");
  bd_rect thumb = bd_rect_make(card.x + 24, card.y + 24, 52, 52);
  const bd_texture *frame =
      item >= 0 && item < home->count ? running_frame_texture(home, item) : NULL;
  if (frame == NULL && item >= 0 && item < home->count) {
    frame = home->entries[item].texture;
  }
  if (frame != NULL) {
    bd_gfx_draw_texture_cover(gfx, (bd_texture *)frame, thumb,
                              bd_color_rgba(255, 255, 255,
                                            (uint8_t)(progress * 255.0f)),
                              7);
  } else {
    bd_gfx_fill_round_rect(gfx, thumb, 7,
                           alpha_color(palette->surface3, progress));
  }

  char title[128];
  if (background_full) {
    snprintf(title, sizeof(title), "后台已满");
  } else {
    snprintf(title, sizeof(title), "结束 %s？", name);
  }
  bd_gfx_draw_text_ellipsized(gfx, home->fonts.serif25, card.x + 89,
                              card.y + 50, card.w - 113,
                              alpha_color(palette->ink, progress), title);

  int memory = -1;
  if (item >= 0 && item < home->count) {
    int running = bd_home_running_index(home, home->entries[item].id);
    if (running >= 0) {
      memory = home->running[running].memory_mb;
    }
  }

  if (background_full) {
    int pending = dialog_item_by_id(home, home->dialog_pending_id);
    const char *pending_name =
        dialog_item_title(home, pending, home->dialog_pending_id);
    char body[128];
    snprintf(body, sizeof(body), "最多同时保留 %d 个后台程序。",
             home->dialog_max_background);
    bd_gfx_draw_text_ellipsized(gfx, home->fonts.body15, card.x + 24,
                                card.y + 100, card.w - 48,
                                alpha_color(palette->ink2, progress), body);
    char choice[512];
    snprintf(choice, sizeof(choice), "结束「%s」并启动「%s」？", name,
             pending_name);
    bd_gfx_draw_text_ellipsized(gfx, home->fonts.body15, card.x + 24,
                                card.y + 126, card.w - 48,
                                alpha_color(palette->ink, progress), choice);
    char meta[128];
    if (memory < 0) {
      snprintf(meta, sizeof(meta), "最久未使用 · 内存未知");
    } else {
      snprintf(meta, sizeof(meta), "最久未使用 · %d MB", memory);
    }
    bd_gfx_draw_text_ellipsized(gfx, home->fonts.badge13, card.x + 24,
                                card.y + 156, card.w - 48,
                                alpha_color(palette->ink3, progress), meta);
    if (home->dialog_error[0] != '\0') {
      bd_gfx_draw_text_ellipsized(gfx, home->fonts.body14, card.x + 24,
                                  card.y + 183, card.w - 48,
                                  alpha_color(home->accent.accent, progress),
                                  home->dialog_error);
    }
  } else {
    char body[256];
    snprintf(body, sizeof(body), "未保存的进度会丢失，%s会被移出后台列表。",
             name);
    bd_gfx_draw_text_ellipsized(gfx, home->fonts.body15, card.x + 24,
                                card.y + 100, card.w - 48,
                                alpha_color(palette->ink2, progress), body);
    char meta[128];
    if (memory < 0) {
      snprintf(meta, sizeof(meta), "已暂停 · 内存未知 · 返回桌面后可再次打开");
    } else {
      snprintf(meta, sizeof(meta), "已暂停 · %d MB · 返回桌面后可再次打开",
               memory);
    }
    bd_gfx_draw_text_ellipsized(gfx, home->fonts.badge13, card.x + 24,
                                card.y + 128, card.w - 48,
                                alpha_color(palette->ink3, progress), meta);
  }

  bd_rect end =
      bd_rect_make(card.x + card.w - 24 - 108, card.y + card.h - 24 - 44, 108, 44);
  bd_rect cancel = bd_rect_make(end.x - 9 - 108, end.y, 108, 44);
  draw_dialog_button(home, gfx, cancel, "取消", false,
                     home->dialog_focus == 0, progress);
  draw_dialog_button(home, gfx, end,
                     background_full ? "结束并启动" : "结束进程", true,
                     home->dialog_focus == 1, progress);
}

static float smoothstep01(float value) {
  value = bd_home_clampf(value, 0.0f, 1.0f);
  return value * value * (3.0f - 2.0f * value);
}

static void draw_app_view(bd_home *home, bd_gfx *gfx, int item, float alpha) {
  bd_rect full = bd_rect_make(0, 0, home->width, home->height);
  bd_gfx_fill_rect(gfx, full, with_alpha(bd_color_rgb(0, 0, 0), alpha));
  if (item < 0 || item >= home->count) {
    return;
  }
  const bd_texture *frame = running_frame_texture(home, item);
  if (frame == NULL) {
    frame = home->entries[item].texture;
  }
  if (frame != NULL) {
    bd_gfx_draw_texture_cover(gfx, (bd_texture *)frame, full,
                              bd_color_rgba(255, 255, 255,
                                            (uint8_t)(alpha * 255.0f)),
                              0);
  }
}

static void draw_launch(bd_home *home, bd_gfx *gfx) {
  if (home->launch == BD_HOME_LAUNCH_NONE) {
    return;
  }
  int item = home->launch_item;
  if (item < 0 || item >= home->count) {
    return;
  }
  float elapsed = (float)(home->now_ms - home->launch_start);
  bd_color scrim = with_alpha(bd_color_rgb(0, 0, 0), 0.46f);
  bd_rect full = bd_rect_make(0, 0, home->width, home->height);

  if (home->launch == BD_HOME_LAUNCH_RETURN) {
    float t = bd_home_clampf(elapsed / (float)BD_HOME_LAUNCH_MS, 0.0f, 1.0f);
    float eased = bd_home_ease_wall(t);
    float scrim_alpha = 0.46f * (1.0f - bd_home_clampf(elapsed / 180.0f, 0.0f, 1.0f));
    scrim.a = scrim_alpha;
    bd_gfx_fill_rect(gfx, full, scrim);
    bd_rect card = bd_home_launch_rect(home);
    int radius = round_int(BD_HOME_GAP * eased);
    const bd_texture *frame = running_frame_texture(home, item);
    if (frame == NULL) {
      frame = home->entries[item].texture;
    }
    if (frame != NULL) {
      bd_gfx_draw_texture_cover(gfx, (bd_texture *)frame, card,
                                bd_color_rgba(255, 255, 255, 255), radius);
    } else {
      bd_gfx_fill_round_rect(gfx, card, radius,
                             home->entries[item].item.placeholder);
    }
    if (t > 0.7f) {
      float ring_alpha = (t - 0.7f) / 0.3f;
      bd_gfx_stroke_round_rect(gfx, card, radius, 2,
                               with_alpha(home->palette.focus_ring, ring_alpha));
    }
    return;
  }

  bool complete = home->launch_complete;
  if (complete) {
    draw_app_view(home, gfx, item, 1.0f);
    return;
  }
  float t = bd_home_clampf(elapsed / (float)BD_HOME_LAUNCH_MS, 0.0f, 1.0f);
  float eased = bd_home_ease_wall(t);
  float app_alpha = smoothstep01((elapsed - 300.0f) / 120.0f);
  if (app_alpha > 0.0f) {
    /* Opaque underneath so the fading card never reveals the wall. */
    draw_app_view(home, gfx, item, 1.0f);
  }
  float card_alpha = 1.0f - app_alpha;
  if (card_alpha > 0.0f) {
    bd_rect card = bd_home_launch_rect(home);
    int radius = round_int(BD_HOME_GAP * (1.0f - eased));
    const bd_texture *frame = running_frame_texture(home, item);
    if (frame == NULL) {
      frame = home->entries[item].texture;
    }
    if (frame != NULL) {
      bd_gfx_draw_texture_cover(
          gfx, (bd_texture *)frame, card,
          bd_color_rgba(255, 255, 255, (uint8_t)(card_alpha * 255.0f)), radius);
    } else {
      bd_gfx_fill_round_rect(gfx, card, radius,
                             alpha_color(home->entries[item].item.placeholder,
                                         card_alpha));
    }
    float ring_alpha = 1.0f - eased * 1.6f;
    if (ring_alpha > 0.0f) {
      bd_gfx_stroke_round_rect(
          gfx, card, radius, 2,
          with_alpha(home->palette.focus_ring, ring_alpha * card_alpha));
    }
  }
}

void bd_home_draw_all(bd_home *home, bd_gfx *gfx) {
  if (home->launch == BD_HOME_LAUNCH_OPEN) {
    if (home->launch_complete) {
      draw_app_view(home, gfx, home->launch_item, 1.0f);
      return;
    }
    if (!home->launch_painted) {
      draw_wall(home, gfx);
      draw_top_bar(home, gfx);
      bd_gfx_fill_rect(gfx, bd_rect_make(0, 0, home->width, home->height),
                       with_alpha(bd_color_rgb(0, 0, 0), 0.46f));
      home->launch_painted = true;
    }
    draw_launch(home, gfx);
    return;
  }
  draw_wall(home, gfx);
  draw_top_bar(home, gfx);
  if (home->quick_open || home->quick_anim) {
    draw_quick(home, gfx);
  }
  if (home->hint_visible || home->hint_anim) {
    draw_hint_bar(home, gfx);
  }
  if (home->launch != BD_HOME_LAUNCH_NONE) {
    draw_launch(home, gfx);
  }
  if (home->dialog_visible) {
    draw_dialog(home, gfx);
  }
}
