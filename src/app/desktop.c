#include "brickdesk/desktop.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brickdesk/ui.h"
#include "brickdesk/wall.h"

#ifndef BRICKDESK_ASSET_DIR
#define BRICKDESK_ASSET_DIR "assets"
#endif

#define BD_WALL_CELL 168
#define BD_WALL_GAP 6
#define BD_WALL_PITCH (BD_WALL_CELL + BD_WALL_GAP)
#define BD_WALL_ORIGIN_X 0
#define BD_WALL_ORIGIN_Y 8
#define BD_TOP_BAR_HEIGHT 76
#define BD_HINT_WIDTH 640
#define BD_HINT_HEIGHT 56
#define BD_HINT_Y 700

struct bd_desktop {
  bd_gfx *gfx;
  bd_ui_fonts fonts;
  bd_wall *wall;
  bd_wall_style wall_style;
  bd_accent_palette accent;
  bd_color bg;
  bd_color bg_deep;
  bd_color ink;
  bd_color ink2;
  bd_color ink3;
  bd_color line;
  bd_texture *ambient;
  float ambient_veil;
  bool ambient_baked;
  bool dark;
  bd_wall_item items[14];
  char image_paths[14][512];
  int item_count;
};

static float blob(float x, float y, float cx, float cy, float rx, float ry) {
  float dx = (x - cx) / rx;
  float dy = (y - cy) / ry;
  float value = 1.0f - (dx * dx + dy * dy);
  return value > 0.0f ? value * value : 0.0f;
}

static bd_texture *create_ambient(bd_desktop *desktop) {
  const int width = 256;
  const int height = 192;
  uint8_t *pixels = (uint8_t *)malloc((size_t)width * height * 4u);
  if (pixels == NULL) {
    return NULL;
  }
  for (int y = 0; y < height; ++y) {
    float ty = (float)y / (float)(height - 1);
    bd_color base = bd_color_mix(desktop->bg, desktop->bg_deep, ty * 0.85f);
    for (int x = 0; x < width; ++x) {
      float b1 = blob((float)x, (float)y, 56.0f, 40.0f, 96.0f, 84.0f);
      float b2 = blob((float)x, (float)y, 214.0f, 96.0f, 120.0f, 110.0f);
      float b3 = blob((float)x, (float)y, 120.0f, 196.0f, 150.0f, 120.0f);
      float amount = b1 * 0.26f + b2 * 0.20f + b3 * 0.16f;
      bd_color color = bd_color_mix(base, desktop->accent.soft, amount);
      uint8_t *pixel = pixels + ((size_t)y * width + (size_t)x) * 4u;
      pixel[0] = (uint8_t)(color.r * 255.0f + 0.5f);
      pixel[1] = (uint8_t)(color.g * 255.0f + 0.5f);
      pixel[2] = (uint8_t)(color.b * 255.0f + 0.5f);
      pixel[3] = 255;
    }
  }
  bd_texture *texture = bd_gfx_texture_create(
      desktop->gfx, width, height, BD_TEXTURE_LINEAR, pixels);
  free(pixels);
  return texture;
}

static bool apply_theme(bd_desktop *desktop, const bd_desktop_theme *theme) {
  if (desktop == NULL || theme == NULL) {
    return false;
  }
  desktop->dark = theme->dark;
  desktop->ambient_baked = theme->ambient_baked;
  if (theme->dark) {
    desktop->bg = bd_color_rgb(18, 16, 13);
    desktop->bg_deep = bd_color_rgb(11, 10, 8);
    desktop->ink = bd_color_rgb(243, 237, 226);
    desktop->ink2 = bd_color_rgb(185, 175, 160);
    desktop->ink3 = bd_color_rgb(145, 136, 121);
    desktop->line = bd_color_rgba(243, 237, 226, 31);
    desktop->ambient_veil = 0.86f;
  } else {
    desktop->bg = bd_color_rgb(242, 237, 227);
    desktop->bg_deep = bd_color_rgb(231, 224, 211);
    desktop->ink = bd_color_rgb(30, 26, 21);
    desktop->ink2 = bd_color_rgb(92, 85, 74);
    desktop->ink3 = bd_color_rgb(111, 103, 91);
    desktop->line = bd_color_rgba(30, 26, 21, 31);
    desktop->ambient_veil = 0.90f;
  }
  desktop->accent = bd_accent_palette_from(theme->accent, theme->dark);

  if (desktop->ambient != NULL) {
    bd_gfx_texture_destroy(desktop->gfx, desktop->ambient);
    desktop->ambient = NULL;
  }
  if (theme->ambient_baked) {
    desktop->ambient = create_ambient(desktop);
    if (desktop->ambient == NULL) {
      return false;
    }
  }
  if (desktop->wall != NULL) {
    desktop->wall_style.accent = desktop->accent;
    bd_wall_set_style(desktop->wall, &desktop->wall_style);
  }
  bd_gfx_damage_all(desktop->gfx);
  return true;
}

static bd_rect cell_rect(int column, int row, int columns, int rows) {
  return bd_rect_make(BD_WALL_ORIGIN_X + column * BD_WALL_PITCH,
                      BD_WALL_ORIGIN_Y + row * BD_WALL_PITCH,
                      columns * BD_WALL_CELL + (columns - 1) * BD_WALL_GAP,
                      rows * BD_WALL_CELL + (rows - 1) * BD_WALL_GAP);
}

static void set_item(bd_desktop *desktop, int index, int column, int row,
                     int columns, int rows, const char *title,
                     const char *subtitle, const char *badge, int cover,
                     bd_color color, float progress) {
  bd_wall_item *item = &desktop->items[index];
  memset(item, 0, sizeof(*item));
  item->rect = cell_rect(column, row, columns, rows);
  item->title = title;
  item->subtitle = subtitle;
  item->badge = badge;
  snprintf(desktop->image_paths[index], sizeof(desktop->image_paths[index]),
           "%s/covers/cover-%02d.png", BRICKDESK_ASSET_DIR, cover);
  item->image_path = desktop->image_paths[index];
  item->placeholder = color;
  item->progress = progress;
}

static void build_items(bd_desktop *desktop) {
  desktop->item_count = 0;
  int i = 0;
  set_item(desktop, i++, 0, 0, 3, 2, "小镇四季", "第 3 年夏季 · 已玩 41 小时", "GBA", 1,
           bd_color_rgb(109, 122, 99), -1.0f);
  set_item(desktop, i++, 3, 0, 2, 2, "星海拾荒者", "第 7 扇区 · 已玩 6 小时", "PICO-8",
           2, bd_color_rgb(76, 92, 112), -1.0f);
  set_item(desktop, i++, 5, 0, 1, 1, "终端", "本机 Shell", "APP", 3,
           bd_color_rgb(44, 46, 44), -1.0f);
  set_item(desktop, i++, 5, 1, 1, 1, "设置", "显示与声音", "APP", 4,
           bd_color_rgb(111, 115, 123), -1.0f);
  set_item(desktop, i++, 0, 2, 1, 2, "音乐", "夜航 · 林间电台", "LIVE", 5,
           bd_color_rgb(123, 90, 140), 0.38f);
  set_item(desktop, i++, 1, 2, 1, 1, "阅读", "《三体》第 14 章", "APP", 6,
           bd_color_rgb(93, 124, 90), -1.0f);
  set_item(desktop, i++, 2, 2, 1, 1, "文件", "SD 卡已用 41 GB", "APP", 7,
           bd_color_rgb(76, 92, 112), -1.0f);
  set_item(desktop, i++, 3, 2, 1, 1, "相册", "截图与照片 86 张", "APP", 8,
           bd_color_rgb(176, 138, 78), -1.0f);
  set_item(desktop, i++, 4, 2, 1, 1, "无线传输", "浏览器传文件", "APP", 1,
           bd_color_rgb(63, 133, 135), -1.0f);
  set_item(desktop, i++, 5, 2, 1, 2, "系统", "屏幕、网络与按键", "SYS", 2,
           bd_color_rgb(111, 115, 123), -1.0f);
  set_item(desktop, i++, 1, 3, 1, 1, "雾港", "第二章「灯塔」", "PS", 3,
           bd_color_rgb(81, 96, 108), -1.0f);
  set_item(desktop, i++, 2, 3, 1, 1, "深井", "到达地下 12 层", "SFC", 4,
           bd_color_rgb(75, 94, 102), -1.0f);
  set_item(desktop, i++, 3, 3, 1, 1, "赤色公路", "最佳圈速 1:42.08", "PS", 5,
           bd_color_rgb(162, 90, 76), -1.0f);
  set_item(desktop, i++, 4, 3, 1, 1, "收藏夹", "12 个收藏应用", "APP", 6,
           bd_color_rgb(123, 90, 140), -1.0f);
  desktop->item_count = i;
}

static void draw_backdrop(bd_desktop *desktop, bd_gfx *gfx) {
  int width = bd_gfx_width(gfx);
  int height = bd_gfx_height(gfx);
  if (desktop->ambient != NULL) {
    bd_gfx_draw_texture(gfx, desktop->ambient, bd_rect_make(0, 0, width, height),
                        bd_color_rgba(255, 255, 255, 255));
    bd_color veil = desktop->bg;
    veil.a = desktop->ambient_veil;
    bd_gfx_fill_rect(gfx, bd_rect_make(0, 0, width, height), veil);
  } else {
    bd_gfx_fill_rect(gfx, bd_rect_make(0, 0, width, height), desktop->bg);
  }
}

static void draw_top_bar(bd_desktop *desktop, bd_gfx *gfx) {
  int width = bd_gfx_width(gfx);
  bd_rect bar = bd_rect_make(0, 0, width, BD_TOP_BAR_HEIGHT);
  bd_color top = desktop->bg;
  top.a = 0.72f;
  bd_color bottom = desktop->bg;
  bottom.a = 0.16f;
  bd_gfx_blur_region_feathered(gfx, bar, 0, top, bottom, BD_GRADIENT_VERTICAL,
                                  BD_BLUR_EDGE_BOTTOM, 24);
  bd_gfx_fill_rect(gfx, bd_rect_make(0, BD_TOP_BAR_HEIGHT - 1, width, 1), desktop->line);

  bd_gfx_fill_round_rect(gfx, bd_rect_make(32, 29, 14, 14), 4,
                         desktop->accent.accent);
  bd_gfx_fill_round_rect(gfx, bd_rect_make(39, 36, 7, 7), 2, desktop->bg);
  bd_gfx_draw_text(gfx, desktop->fonts.latin, 56, 43, desktop->ink2, "brickdesk");

  static const char *const TABS[] = {"全部", "游戏", "应用", "收藏"};
  int x = 240;
  for (int i = 0; i < 4; ++i) {
    bd_color color = i == 0 ? desktop->ink : desktop->ink3;
    bd_gfx_draw_text(gfx, desktop->fonts.title, x, 45, color, TABS[i]);
    if (i == 0) {
      bd_gfx_fill_round_rect(gfx, bd_rect_make(x, 58, 22, 2), 1,
                             desktop->accent.accent);
    }
    x += bd_font_measure_text(desktop->fonts.title, TABS[i]) + 34;
  }

  bd_gfx_draw_text(gfx, desktop->fonts.body, width - 108, 43, desktop->ink2, "20:41");
  bd_gfx_stroke_round_rect(gfx, bd_rect_make(width - 48, 31, 26, 14), 4, 1,
                           desktop->ink3);
  bd_gfx_fill_rect(gfx, bd_rect_make(width - 45, 34, 17, 8), desktop->accent.accent);
}

static void draw_hint_bar(bd_desktop *desktop, bd_gfx *gfx) {
  int width = bd_gfx_width(gfx);
  int x = (width - BD_HINT_WIDTH) / 2;
  bd_rect bar = bd_rect_make(x, BD_HINT_Y, BD_HINT_WIDTH, BD_HINT_HEIGHT);
  bd_color top = desktop->ink;
  top.a = 0.62f;
  bd_color bottom = desktop->ink;
  bottom.a = 0.82f;
  bd_gfx_blur_region_feathered(gfx, bar, 16, top, bottom, BD_GRADIENT_VERTICAL,
                                  BD_BLUR_EDGE_TOP, 14);

  static const char *const KEYS[] = {"A", "B", "M"};
  static const char *const LABELS[] = {"打开", "返回", "菜单"};
  int key_x = x + 28;
  for (int i = 0; i < 3; ++i) {
    bd_rect key = bd_rect_make(key_x, BD_HINT_Y + 16, 24, 24);
    bd_color hint_text = desktop->bg;
    bd_color key_fill = hint_text;
    key_fill.a = 0.14f;
    bd_color key_line = hint_text;
    key_line.a = 0.28f;
    bd_gfx_fill_round_rect(gfx, key, 6, key_fill);
    bd_gfx_stroke_round_rect(gfx, key, 6, 1, key_line);
    int key_width = bd_font_measure_text(desktop->fonts.body, KEYS[i]);
    bd_gfx_draw_text(gfx, desktop->fonts.body, key.x + (24 - key_width) / 2,
                     key.y + 17, hint_text, KEYS[i]);
    bd_gfx_draw_text(gfx, desktop->fonts.body, key.x + 32, key.y + 17, hint_text,
                     LABELS[i]);
    key_x += 24 + 32 + bd_font_measure_text(desktop->fonts.body, LABELS[i]) + 36;
  }
}

bd_desktop *bd_desktop_create(bd_gfx *gfx, const bd_desktop_theme *theme) {
  if (gfx == NULL || theme == NULL) {
    return NULL;
  }
  bd_desktop *desktop = (bd_desktop *)calloc(1, sizeof(*desktop));
  if (desktop == NULL) {
    return NULL;
  }
  desktop->gfx = gfx;
  if (!apply_theme(desktop, theme)) {
    fprintf(stderr, "desktop: failed to create ambient texture\n");
    free(desktop);
    return NULL;
  }
  if (!bd_ui_fonts_load(&desktop->fonts, gfx)) {
    fprintf(stderr, "desktop: failed to load UI fonts under %s/fonts\n",
            BRICKDESK_ASSET_DIR);
    bd_gfx_texture_destroy(gfx, desktop->ambient);
    free(desktop);
    return NULL;
  }

  desktop->wall_style.fonts = &desktop->fonts;
  desktop->wall_style.accent = desktop->accent;
  desktop->wall_style.radius = 6;
  desktop->wall_style.prefetch_pixels = BD_WALL_PITCH;
  desktop->wall = bd_wall_create(gfx, &desktop->wall_style);
  if (desktop->wall == NULL) {
    bd_ui_fonts_free(&desktop->fonts);
    bd_gfx_texture_destroy(gfx, desktop->ambient);
    free(desktop);
    return NULL;
  }

  build_items(desktop);
  if (!bd_wall_set_items(desktop->wall, desktop->items, desktop->item_count)) {
    fprintf(stderr, "desktop: failed to index wall items\n");
    bd_wall_destroy(desktop->wall);
    bd_ui_fonts_free(&desktop->fonts);
    bd_gfx_texture_destroy(gfx, desktop->ambient);
    free(desktop);
    return NULL;
  }
  bd_wall_set_focus(desktop->wall, 0);
  return desktop;
}

void bd_desktop_destroy(bd_desktop *desktop) {
  if (desktop == NULL) {
    return;
  }
  bd_wall_destroy(desktop->wall);
  bd_ui_fonts_free(&desktop->fonts);
  bd_gfx_texture_destroy(desktop->gfx, desktop->ambient);
  free(desktop);
}

void bd_desktop_draw(bd_desktop *desktop, bd_gfx *gfx) {
  if (desktop == NULL || gfx == NULL) {
    return;
  }
  bd_rect viewport = bd_rect_make(0, 0, bd_gfx_width(gfx), bd_gfx_height(gfx));
  bd_wall_begin_frame(desktop->wall, gfx, viewport, BD_WALL_ORIGIN_X,
                      BD_WALL_ORIGIN_Y);
  draw_backdrop(desktop, gfx);
  bd_wall_draw(desktop->wall, gfx);
  draw_top_bar(desktop, gfx);
  draw_hint_bar(desktop, gfx);
}

bool bd_desktop_handle_key(bd_desktop *desktop, bd_key key, bool pressed) {
  if (desktop == NULL || !pressed) {
    return false;
  }
  switch (key) {
    case BD_KEY_LEFT:
      return bd_wall_focus_move(desktop->wall, -1, 0);
    case BD_KEY_RIGHT:
      return bd_wall_focus_move(desktop->wall, 1, 0);
    case BD_KEY_UP:
      return bd_wall_focus_move(desktop->wall, 0, -1);
    case BD_KEY_DOWN:
      return bd_wall_focus_move(desktop->wall, 0, 1);
    default:
      return false;
  }
}

int bd_desktop_focus(const bd_desktop *desktop) {
  return desktop != NULL ? bd_wall_focus(desktop->wall) : -1;
}

void bd_desktop_set_focus(bd_desktop *desktop, int index) {
  if (desktop != NULL) {
    bd_wall_set_focus(desktop->wall, index);
  }
}

void bd_desktop_set_theme(bd_desktop *desktop, const bd_desktop_theme *theme) {
  if (desktop == NULL || theme == NULL) {
    return;
  }
  if (!apply_theme(desktop, theme)) {
    fprintf(stderr, "desktop: failed to rebuild ambient texture\n");
  }
}

void bd_desktop_set_accent(bd_desktop *desktop, bd_color accent) {
  if (desktop == NULL) {
    return;
  }
  bd_desktop_theme theme;
  theme.dark = desktop->dark;
  theme.ambient_baked = desktop->ambient_baked;
  theme.accent = accent;
  bd_desktop_set_theme(desktop, &theme);
}

bd_accent_palette bd_desktop_accent(const bd_desktop *desktop) {
  bd_accent_palette empty;
  memset(&empty, 0, sizeof(empty));
  return desktop != NULL ? desktop->accent : empty;
}

bool bd_desktop_has_pending_images(const bd_desktop *desktop) {
  return desktop != NULL && bd_wall_has_pending_images(desktop->wall);
}
