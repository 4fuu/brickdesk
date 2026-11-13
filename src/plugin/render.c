#define _POSIX_C_SOURCE 200809L

#include "brickdesk/plugin.h"

#include <stdio.h>
#include <string.h>

#include "host_internal.h"

#define BD_PLUGIN_TILE_RADIUS 6
#define BD_PLUGIN_ROW_GAP 8

typedef struct ui_colors {
  bd_color surface;
  bd_color text;
  bd_color muted;
  bd_color line;
  bd_color accent;
  bd_color soft;
} ui_colors;

typedef struct ui_draw_context {
  bd_gfx *gfx;
  const bd_plugin_ui *ui;
  const bd_plugin_render_style *style;
  const bd_ui_fonts *fonts;
  ui_colors colors;
  int focused_node;
  int depth;
} ui_draw_context;

static ui_colors make_colors(const bd_plugin_render_style *style) {
  ui_colors colors;
  if (style != NULL && style->dark) {
    colors.surface = bd_color_rgb(26, 23, 19);
    colors.text = bd_color_rgb(243, 237, 226);
    colors.muted = bd_color_rgb(175, 165, 150);
    colors.line = bd_color_rgba(243, 237, 226, 46);
    colors.soft = bd_color_rgba(243, 237, 226, 20);
  } else {
    colors.surface = bd_color_rgb(247, 243, 234);
    colors.text = bd_color_rgb(30, 26, 21);
    colors.muted = bd_color_rgb(92, 85, 74);
    colors.line = bd_color_rgba(30, 26, 21, 38);
    colors.soft = bd_color_rgba(30, 26, 21, 16);
  }
  colors.accent = style != NULL ? style->accent.accent : bd_color_rgb(224, 108, 74);
  if (style != NULL) {
    colors.soft = style->accent.soft;
  }
  return colors;
}

static const bd_font *font_for_style(const ui_draw_context *context,
                                     const char *style) {
  const bd_ui_fonts *fonts = context->fonts;
  if (fonts == NULL) {
    return NULL;
  }
  if (strcmp(style, "title") == 0) {
    return fonts->title;
  }
  if (strcmp(style, "caption") == 0) {
    return fonts->badge;
  }
  if (strcmp(style, "mono") == 0) {
    return fonts->latin;
  }
  return fonts->body;
}

static int count_lines(const char *text) {
  int lines = 1;
  for (const char *cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor == '\n' && lines < 4) {
      ++lines;
    }
  }
  return lines;
}

static int measure_node(const bd_plugin_ui *ui, int index, int width,
                        int depth) {
  if (ui == NULL || index < 0 || index >= ui->node_count || depth > 12) {
    return 0;
  }
  const bd_plugin_ui_node *node = &ui->nodes[index];
  const bd_ui_fonts *fonts = NULL;
  (void)fonts;
  (void)width;
  switch (node->type) {
    case BD_PLUGIN_UI_TEXT:
      return 22 * count_lines(node->text) + 2;
    case BD_PLUGIN_UI_ICON:
      return 28;
    case BD_PLUGIN_UI_IMAGE:
      return node->image_height > 0 ? node->image_height : 72;
    case BD_PLUGIN_UI_PROGRESS:
      return node->text[0] != '\0' ? 26 : 10;
    case BD_PLUGIN_UI_TOGGLE:
    case BD_PLUGIN_UI_SELECT:
      return 30;
    case BD_PLUGIN_UI_SLIDER:
      return 44;
    case BD_PLUGIN_UI_BUTTON:
      return 34;
    case BD_PLUGIN_UI_SPACER:
      return 0;
    case BD_PLUGIN_UI_ROW: {
      int tallest = 0;
      for (int i = 0; i < node->child_count; ++i) {
        int height = measure_node(ui, node->children[i], width, depth + 1);
        if (height > tallest) {
          tallest = height;
        }
      }
      return tallest + node->padding * 2;
    }
    case BD_PLUGIN_UI_COLUMN: {
      int total = node->padding * 2;
      for (int i = 0; i < node->child_count; ++i) {
        total += measure_node(ui, node->children[i], width, depth + 1);
        if (i + 1 < node->child_count) {
          total += node->gap;
        }
      }
      return total;
    }
    default:
      return 0;
  }
}

static void draw_text_node(ui_draw_context *context, const bd_plugin_ui_node *node,
                           bd_rect rect) {
  const bd_font *font = font_for_style(context, node->style);
  if (font == NULL) {
    return;
  }
  bd_color color = strcmp(node->style, "caption") == 0 ? context->colors.muted
                                                       : context->colors.text;
  int baseline = rect.y + bd_font_ascent(font);
  const char *line = node->text;
  int remaining = rect.h;
  for (;;) {
    const char *newline = strchr(line, '\n');
    size_t length = newline != NULL ? (size_t)(newline - line) : strlen(line);
    char buffer[128];
    if (length >= sizeof(buffer)) {
      length = sizeof(buffer) - 1;
    }
    memcpy(buffer, line, length);
    buffer[length] = '\0';
    bd_gfx_draw_text_ellipsized(context->gfx, font, rect.x, baseline, rect.w,
                                color, buffer);
    baseline += bd_font_line_height(font);
    remaining -= bd_font_line_height(font);
    if (newline == NULL || remaining <= bd_font_ascent(font)) {
      break;
    }
    line = newline + 1;
  }
}

static void draw_icon_node(ui_draw_context *context, const bd_plugin_ui_node *node,
                           bd_rect rect) {
  int size = rect.h < 28 ? rect.h : 28;
  bd_rect icon = bd_rect_make(rect.x, rect.y, size, size);
  bd_gfx_fill_round_rect(context->gfx, icon, 7, context->colors.soft);
  if (node->icon[0] != '\0' && context->fonts != NULL) {
    char first[8];
    size_t length = 1;
    unsigned char c = (unsigned char)node->icon[0];
    if ((c & 0xe0u) == 0xc0u) {
      length = 2;
    } else if ((c & 0xf0u) == 0xe0u) {
      length = 3;
    } else if ((c & 0xf8u) == 0xf0u) {
      length = 4;
    }
    if (length >= sizeof(first)) {
      length = 1;
    }
    memcpy(first, node->icon, length);
    first[length] = '\0';
    if (first[0] >= 'a' && first[0] <= 'z') {
      first[0] = (char)(first[0] - 'a' + 'A');
    }
    int text_width = bd_font_measure_text(context->fonts->badge, first);
    bd_gfx_draw_text(context->gfx, context->fonts->badge,
                     icon.x + (size - text_width) / 2,
                     icon.y + size / 2 + bd_font_ascent(context->fonts->badge) / 2,
                     context->colors.accent, first);
  }
}

static void draw_image_node(ui_draw_context *context,
                            const bd_plugin_ui_node *node, bd_rect rect) {
  bd_texture *texture = NULL;
  if (node->image[0] == '/') {
    bd_image_state state = bd_gfx_image_texture(
        context->gfx, node->image, rect.w, rect.h, &texture);
    if (state == BD_IMAGE_READY && texture != NULL) {
      bd_gfx_draw_texture_cover(context->gfx, texture, rect,
                                bd_color_rgba(255, 255, 255, 255),
                                node->radius);
      return;
    }
  }
  bd_gfx_fill_round_rect(context->gfx, rect, node->radius,
                         context->colors.soft);
}

static void draw_progress_node(ui_draw_context *context,
                               const bd_plugin_ui_node *node, bd_rect rect) {
  int top = rect.y;
  if (node->text[0] != '\0' && context->fonts != NULL) {
    bd_gfx_draw_text_ellipsized(context->gfx, context->fonts->badge, rect.x,
                                rect.y + 13, rect.w, context->colors.muted,
                                node->text);
    top += 16;
  }
  int height = 6;
  if (top + height > rect.y + rect.h) {
    top = rect.y + rect.h - height;
  }
  bd_rect track = bd_rect_make(rect.x, top, rect.w, height);
  bd_gfx_fill_round_rect(context->gfx, track, 3, context->colors.soft);
  float ratio = node->max_value > 0.0f ? node->value / node->max_value : 0.0f;
  if (ratio < 0.0f) {
    ratio = 0.0f;
  }
  if (ratio > 1.0f) {
    ratio = 1.0f;
  }
  int fill = (int)(ratio * (float)rect.w);
  if (fill > 0) {
    bd_gfx_fill_round_rect(
        context->gfx, bd_rect_make(track.x, track.y, fill, height), 3,
        context->colors.accent);
  }
}

static void draw_toggle_node(ui_draw_context *context,
                             const bd_plugin_ui_node *node, bd_rect rect,
                             int index) {
  if (context->fonts != NULL) {
    bd_gfx_draw_text_ellipsized(context->gfx, context->fonts->body, rect.x,
                                rect.y + 20, rect.w - BD_UI_TOGGLE_W - 16,
                                context->colors.text,
                                node->text);
  }
  bd_ui_draw_toggle(context->gfx, rect.x + rect.w - BD_UI_TOGGLE_W - 4,
                     rect.y + (rect.h - BD_UI_TOGGLE_H) / 2, node->boolean,
                     node->boolean ? context->colors.accent
                                   : context->colors.soft,
                     context->colors.text);
  if (index == context->focused_node) {
    bd_gfx_stroke_round_rect(context->gfx, bd_rect_make(rect.x - 2, rect.y - 1,
                                                        rect.w + 4, rect.h + 2),
                             5, 2, context->colors.accent);
  }
}

static void draw_slider_node(ui_draw_context *context,
                             const bd_plugin_ui_node *node, bd_rect rect,
                             int index) {
  char value[32];
  snprintf(value, sizeof(value), "%.3g", (double)node->value);
  if (context->fonts != NULL) {
    bd_gfx_draw_text_ellipsized(context->gfx, context->fonts->body, rect.x,
                                rect.y + 18, rect.w - 64, context->colors.text,
                                node->text);
    int value_width = bd_font_measure_text(context->fonts->badge, value);
    bd_gfx_draw_text(context->gfx, context->fonts->badge,
                     rect.x + rect.w - value_width, rect.y + 18,
                     context->colors.muted, value);
  }
  bd_rect track = bd_rect_make(rect.x, rect.y + 30, rect.w, 4);
  bd_gfx_fill_round_rect(context->gfx, track, 2, context->colors.soft);
  float ratio = 0.0f;
  if (node->max_value > node->min_value) {
    ratio = (node->value - node->min_value) /
            (node->max_value - node->min_value);
  }
  if (ratio < 0.0f) {
    ratio = 0.0f;
  }
  if (ratio > 1.0f) {
    ratio = 1.0f;
  }
  int knob_x = rect.x + (int)(ratio * (float)(rect.w - 12));
  bd_gfx_fill_round_rect(context->gfx, bd_rect_make(track.x, track.y, knob_x - track.x + 6, 4),
                         2, context->colors.accent);
  bd_gfx_fill_round_rect(context->gfx, bd_rect_make(knob_x, rect.y + 26, 12, 12),
                         6, context->colors.accent);
  if (index == context->focused_node) {
    bd_gfx_stroke_round_rect(context->gfx, bd_rect_make(rect.x - 2, rect.y - 1,
                                                        rect.w + 4, rect.h + 2),
                             5, 2, context->colors.accent);
  }
}

static void draw_select_node(ui_draw_context *context,
                             const bd_plugin_ui_node *node, bd_rect rect,
                             int index) {
  if (context->fonts != NULL) {
    bd_gfx_draw_text_ellipsized(context->gfx, context->fonts->body, rect.x,
                                rect.y + 20, rect.w / 2, context->colors.text,
                                node->text);
    int value_width = bd_font_measure_text(context->fonts->badge,
                                          node->value_text);
    bd_gfx_draw_text_ellipsized(context->gfx, context->fonts->badge,
                                rect.x + rect.w - value_width - 12, rect.y + 20,
                                value_width + 12, context->colors.muted,
                                node->value_text);
  }
  if (index == context->focused_node) {
    bd_gfx_stroke_round_rect(context->gfx, bd_rect_make(rect.x - 2, rect.y - 1,
                                                        rect.w + 4, rect.h + 2),
                             5, 2, context->colors.accent);
  }
}

static void draw_button_node(ui_draw_context *context,
                             const bd_plugin_ui_node *node, bd_rect rect,
                             int index) {
  bd_gfx_fill_round_rect(context->gfx, rect, 7, context->colors.soft);
  if (context->fonts != NULL) {
    int width = bd_font_measure_text(context->fonts->body, node->text);
    bd_gfx_draw_text(context->gfx, context->fonts->body,
                     rect.x + (rect.w - width) / 2,
                     rect.y + rect.h / 2 + bd_font_ascent(context->fonts->body) / 2,
                     context->colors.text, node->text);
  }
  if (index == context->focused_node) {
    bd_gfx_stroke_round_rect(context->gfx, rect, 7, 2, context->colors.accent);
  }
}

static void draw_node(ui_draw_context *context, int index, bd_rect rect);

static void draw_column(ui_draw_context *context, const bd_plugin_ui_node *node,
                        bd_rect rect) {
  int x = rect.x + node->padding;
  int width = rect.w - node->padding * 2;
  if (width < 0) {
    width = 0;
  }
  int y = rect.y + node->padding;
  for (int i = 0; i < node->child_count; ++i) {
    int child = node->children[i];
    int height = measure_node(context->ui, child, width, context->depth + 1);
    if (context->ui->nodes[child].type == BD_PLUGIN_UI_SPACER) {
      height = 0;
    }
    draw_node(context, child,
              bd_rect_make(x, y, width, height));
    y += height + node->gap;
  }
}

static void draw_row(ui_draw_context *context, const bd_plugin_ui_node *node,
                     bd_rect rect) {
  int inner_width = rect.w - node->padding * 2;
  if (inner_width < 0) {
    inner_width = 0;
  }
  int count = node->child_count > 0 ? node->child_count : 1;
  int gap_total = node->gap * (count - 1);
  int fixed_width = 0;
  int flexible = 0;
  for (int i = 0; i < node->child_count; ++i) {
    const bd_plugin_ui_node *child =
        &context->ui->nodes[node->children[i]];
    if (child->type == BD_PLUGIN_UI_ICON) {
      fixed_width += 28;
    } else {
      ++flexible;
    }
  }
  int available = inner_width - gap_total - fixed_width;
  if (available < 0) {
    available = 0;
  }
  int flexible_width = flexible > 0 ? available / flexible : 0;
  int x = rect.x + node->padding;
  for (int i = 0; i < node->child_count; ++i) {
    int child = node->children[i];
    const bd_plugin_ui_node *child_node = &context->ui->nodes[child];
    int width = child_node->type == BD_PLUGIN_UI_ICON ? 28 : flexible_width;
    if (child_node->type == BD_PLUGIN_UI_SPACER) {
      width = flexible_width;
    }
    int height = measure_node(context->ui, child, width, context->depth + 1);
    int offset = (rect.h - node->padding * 2 - height) / 2;
    if (offset < 0) {
      offset = 0;
    }
    draw_node(context, child,
              bd_rect_make(x, rect.y + node->padding + offset, width, height));
    x += width + node->gap;
  }
}

static void draw_node(ui_draw_context *context, int index, bd_rect rect) {
  if (context->ui == NULL || index < 0 || index >= context->ui->node_count ||
      context->depth > 12 || rect.w <= 0 || rect.h <= 0) {
    return;
  }
  ++context->depth;
  const bd_plugin_ui_node *node = &context->ui->nodes[index];
  switch (node->type) {
    case BD_PLUGIN_UI_COLUMN:
      draw_column(context, node, rect);
      break;
    case BD_PLUGIN_UI_ROW:
      draw_row(context, node, rect);
      break;
    case BD_PLUGIN_UI_TEXT:
      draw_text_node(context, node, rect);
      break;
    case BD_PLUGIN_UI_ICON:
      draw_icon_node(context, node, rect);
      break;
    case BD_PLUGIN_UI_IMAGE:
      draw_image_node(context, node, rect);
      break;
    case BD_PLUGIN_UI_PROGRESS:
      draw_progress_node(context, node, rect);
      break;
    case BD_PLUGIN_UI_TOGGLE:
      draw_toggle_node(context, node, rect, index);
      break;
    case BD_PLUGIN_UI_SLIDER:
      draw_slider_node(context, node, rect, index);
      break;
    case BD_PLUGIN_UI_SELECT:
      draw_select_node(context, node, rect, index);
      break;
    case BD_PLUGIN_UI_BUTTON:
      draw_button_node(context, node, rect, index);
      break;
    case BD_PLUGIN_UI_SPACER:
      break;
    default:
      break;
  }
  --context->depth;
}

void bd_plugin_ui_draw(const bd_plugin_ui *ui, bd_gfx *gfx, bd_rect rect,
                       const bd_plugin_render_style *style, int focused_node,
                       int scroll_y) {
  if (ui == NULL || gfx == NULL || ui->root < 0 || rect.w <= 0 || rect.h <= 0) {
    return;
  }
  ui_draw_context context;
  memset(&context, 0, sizeof(context));
  context.gfx = gfx;
  context.ui = ui;
  context.style = style;
  context.fonts = style != NULL ? style->fonts : NULL;
  context.colors = make_colors(style);
  context.focused_node = focused_node;
  bd_gfx_push_clip(gfx, rect);
  draw_node(&context, ui->root,
            bd_rect_make(rect.x, rect.y - scroll_y, rect.w, rect.h));
  bd_gfx_pop_clip(gfx);
}

int bd_plugin_ui_measure(const bd_plugin_ui *ui, int width) {
  if (ui == NULL || ui->root < 0 || width <= 0) {
    return 0;
  }
  return measure_node(ui, ui->root, width, 0);
}

static bool layout_find(const bd_plugin_ui *ui, int index, bd_rect rect,
                        int target, bd_rect *out, int depth) {
  if (ui == NULL || index < 0 || index >= ui->node_count || depth > 12 ||
      rect.w <= 0 || rect.h <= 0) {
    return false;
  }
  if (index == target) {
    *out = rect;
    return true;
  }
  const bd_plugin_ui_node *node = &ui->nodes[index];
  if (node->type == BD_PLUGIN_UI_COLUMN) {
    int x = rect.x + node->padding;
    int width = rect.w - node->padding * 2;
    if (width < 0) {
      width = 0;
    }
    int y = rect.y + node->padding;
    for (int i = 0; i < node->child_count; ++i) {
      int child = node->children[i];
      int height = measure_node(ui, child, width, depth + 1);
      if (ui->nodes[child].type == BD_PLUGIN_UI_SPACER) {
        height = 0;
      }
      if (layout_find(ui, child, bd_rect_make(x, y, width, height), target,
                      out, depth + 1)) {
        return true;
      }
      y += height + node->gap;
    }
  } else if (node->type == BD_PLUGIN_UI_ROW) {
    int inner_width = rect.w - node->padding * 2;
    if (inner_width < 0) {
      inner_width = 0;
    }
    int count = node->child_count > 0 ? node->child_count : 1;
    int gap_total = node->gap * (count - 1);
    int fixed_width = 0;
    int flexible = 0;
    for (int i = 0; i < node->child_count; ++i) {
      const bd_plugin_ui_node *child = &ui->nodes[node->children[i]];
      if (child->type == BD_PLUGIN_UI_ICON) {
        fixed_width += 28;
      } else {
        ++flexible;
      }
    }
    int available = inner_width - gap_total - fixed_width;
    if (available < 0) {
      available = 0;
    }
    int flexible_width = flexible > 0 ? available / flexible : 0;
    int x = rect.x + node->padding;
    for (int i = 0; i < node->child_count; ++i) {
      int child = node->children[i];
      const bd_plugin_ui_node *child_node = &ui->nodes[child];
      int width = child_node->type == BD_PLUGIN_UI_ICON ? 28 : flexible_width;
      if (child_node->type == BD_PLUGIN_UI_SPACER) {
        width = flexible_width;
      }
      int height = measure_node(ui, child, width, depth + 1);
      int offset = (rect.h - node->padding * 2 - height) / 2;
      if (offset < 0) {
        offset = 0;
      }
      if (layout_find(ui, child,
                      bd_rect_make(x, rect.y + node->padding + offset, width,
                                   height),
                      target, out, depth + 1)) {
        return true;
      }
      x += width + node->gap;
    }
  }
  return false;
}

bool bd_plugin_ui_node_rect(const bd_plugin_ui *ui, int node, int width,
                            bd_rect *out) {
  if (ui == NULL || out == NULL || ui->root < 0 || width <= 0 || node < 0 ||
      node >= ui->node_count) {
    return false;
  }
  int height = measure_node(ui, ui->root, width, 0);
  return layout_find(ui, ui->root, bd_rect_make(0, 0, width, height), node,
                     out, 0);
}

static const char *state_label(bd_plugin_state state) {
  switch (state) {
    case BD_PLUGIN_DISCOVERED:
    case BD_PLUGIN_STOPPED:
      return "插件未运行";
    case BD_PLUGIN_STARTING:
    case BD_PLUGIN_HANDSHAKING:
      return "插件启动中";
    case BD_PLUGIN_BACKOFF:
      return "插件重启中";
    case BD_PLUGIN_STOPPING:
      return "插件停止中";
    case BD_PLUGIN_QUARANTINED:
      return "插件不可用";
    case BD_PLUGIN_RUNNING:
      return "等待插件 UI";
    default:
      return "插件";
  }
}

bool bd_plugin_tile_begin_frame(bd_plugin *plugin, bd_gfx *gfx) {
  if (plugin == NULL || gfx == NULL) {
    return false;
  }
  bool changed = false;
  if (!plugin->render_initialized || plugin->render_state != plugin->state ||
      plugin->render_ui_generation != plugin->ui_generation) {
    plugin->render_initialized = true;
    plugin->render_state = plugin->state;
    plugin->render_ui_generation = plugin->ui_generation;
    changed = true;
  }
  bd_plugin_frame frame;
  if (bd_plugin_tile_consume(plugin, &frame)) {
    bool updated = false;
    if (plugin->texture != NULL) {
      updated = bd_gfx_texture_update(gfx, plugin->texture, frame.width,
                                      frame.height, frame.rgba);
      if (!updated) {
        bd_gfx_texture_destroy(gfx, plugin->texture);
        plugin->texture = NULL;
      }
    }
    if (plugin->texture == NULL) {
      plugin->texture = bd_gfx_texture_create(gfx, frame.width, frame.height,
                                              BD_TEXTURE_LINEAR, frame.rgba);
      updated = plugin->texture != NULL;
    }
    if (updated) {
      plugin->texture_w = frame.width;
      plugin->texture_h = frame.height;
      plugin->texture_seq = frame.seq;
      changed = true;
    }
  }
  return changed;
}

void bd_plugin_tile_draw(bd_plugin *plugin, bd_gfx *gfx, bd_rect rect,
                         const bd_plugin_render_style *style) {
  if (plugin == NULL || gfx == NULL || rect.w <= 0 || rect.h <= 0) {
    return;
  }
  ui_colors colors = make_colors(style);
  bd_gfx_fill_round_rect(gfx, rect, BD_PLUGIN_TILE_RADIUS, colors.surface);
  if (plugin->texture != NULL) {
    bd_gfx_draw_texture_round(gfx, plugin->texture, rect,
                              bd_color_rgba(255, 255, 255, 255),
                              BD_PLUGIN_TILE_RADIUS);
    bd_color scrim = style != NULL && style->dark
                         ? bd_color_rgba(10, 8, 6, 96)
                         : bd_color_rgba(255, 252, 246, 78);
    bd_gfx_fill_round_rect(gfx, rect, BD_PLUGIN_TILE_RADIUS, scrim);
  }
  if (plugin->has_tile_ui && plugin->state == BD_PLUGIN_RUNNING) {
    bd_plugin_ui_draw(&plugin->ui_tile, gfx, rect, style, -1, 0);
  } else {
    const bd_ui_fonts *fonts = style != NULL ? style->fonts : NULL;
    int icon_size = 30;
    bd_rect icon = bd_rect_make(rect.x + 14, rect.y + 14, icon_size, icon_size);
    bd_gfx_fill_round_rect(gfx, icon, 8, colors.soft);
    if (fonts != NULL) {
      int width = bd_font_measure_text(fonts->badge, "P");
      bd_gfx_draw_text(gfx, fonts->badge, icon.x + (icon_size - width) / 2,
                       icon.y + icon_size / 2 + bd_font_ascent(fonts->badge) / 2,
                       colors.accent, "P");
      bd_gfx_draw_text_ellipsized(gfx, fonts->title, rect.x + 14, rect.y + 68,
                                  rect.w - 28, colors.text,
                                  plugin->manifest.name);
      bd_gfx_draw_text_ellipsized(gfx, fonts->badge, rect.x + 14, rect.y + 90,
                                  rect.w - 28, colors.muted,
                                  state_label(plugin->state));
    }
  }
  bd_gfx_stroke_round_rect(gfx, rect, BD_PLUGIN_TILE_RADIUS, 1, colors.line);
}

void bd_plugin_tile_release(bd_plugin *plugin, bd_gfx *gfx) {
  if (plugin == NULL) {
    return;
  }
  if (plugin->texture != NULL && gfx != NULL) {
    bd_gfx_texture_destroy(gfx, plugin->texture);
  }
  plugin->texture = NULL;
  plugin->texture_w = 0;
  plugin->texture_h = 0;
  plugin->texture_seq = 0;
  plugin->render_initialized = false;
  plugin->render_ui_generation = -1;
}
