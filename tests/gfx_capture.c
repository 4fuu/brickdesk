#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brickdesk/gfx.h"
#include "brickdesk/keyboard.h"
#include "brickdesk/platform.h"
#include "brickdesk/theme.h"
#include "brickdesk/ui.h"
#include "brickdesk/wall.h"

#ifndef BRICKDESK_ASSET_DIR
#define BRICKDESK_ASSET_DIR "assets"
#endif
#ifndef BRICKDESK_TEST_ASSET_DIR
#define BRICKDESK_TEST_ASSET_DIR "tests/assets"
#endif

#define CAPTURE_WIDTH 1024
#define CAPTURE_HEIGHT 768

static const bd_color BG = {242.0f / 255.0f, 237.0f / 255.0f, 227.0f / 255.0f, 1.0f};
static const bd_color BG_DEEP = {231.0f / 255.0f, 224.0f / 255.0f, 211.0f / 255.0f, 1.0f};
static const bd_color INK = {30.0f / 255.0f, 26.0f / 255.0f, 21.0f / 255.0f, 1.0f};
static const bd_color INK_2 = {92.0f / 255.0f, 85.0f / 255.0f, 74.0f / 255.0f, 1.0f};
static const bd_color ACCENT = {174.0f / 255.0f, 67.0f / 255.0f, 43.0f / 255.0f, 1.0f};

static bd_texture *make_test_texture(bd_gfx *gfx) {
  uint8_t pixels[128 * 128 * 4];
  for (int y = 0; y < 128; ++y) {
    for (int x = 0; x < 128; ++x) {
      uint8_t *pixel = pixels + ((size_t)y * 128 + (size_t)x) * 4u;
      bool checker = ((x / 16) + (y / 16)) % 2 == 0;
      pixel[0] = checker ? 232 : 42;
      pixel[1] = (uint8_t)(x * 2);
      pixel[2] = (uint8_t)(y * 2);
      pixel[3] = 255;
    }
  }
  return bd_gfx_texture_create(gfx, 128, 128, BD_TEXTURE_LINEAR, pixels);
}

static void page_background(bd_gfx *gfx) {
  bd_gfx_fill_rect(gfx, bd_rect_make(0, 0, CAPTURE_WIDTH, CAPTURE_HEIGHT), BG);
  bd_gfx_fill_gradient3(gfx, bd_rect_make(0, 0, CAPTURE_WIDTH, CAPTURE_HEIGHT), 0,
                        BG, BG_DEEP, BG, BD_GRADIENT_VERTICAL);
}

static void section_label(bd_gfx *gfx, const bd_ui_fonts *fonts, int x, int y,
                          const char *text) {
  bd_gfx_fill_round_rect(gfx, bd_rect_make(x, y - 12, 18, 3), 1, ACCENT);
  bd_gfx_draw_text(gfx, fonts->body, x + 28, y, INK_2, text);
}

static void render_primitives(bd_gfx *gfx, const bd_ui_fonts *fonts,
                              bd_texture *procedural, bd_texture *png,
                              bd_texture *jpg) {
  bd_gfx_damage_all(gfx);
  bd_gfx_begin_frame(gfx);
  page_background(gfx);

  bd_gfx_draw_text(gfx, fonts->serif, 40, 66, INK, "渲染图元");
  bd_gfx_draw_text(gfx, fonts->latin, 40, 94, INK_2,
                   "Rounded rects · gradients · scrim · clip · textures");

  section_label(gfx, fonts, 40, 126, "圆角填充 / rounded fill");
  static const bd_color FILLS[4] = {
      {109.0f / 255.0f, 122.0f / 255.0f, 99.0f / 255.0f, 1.0f},
      {76.0f / 255.0f, 92.0f / 255.0f, 112.0f / 255.0f, 1.0f},
      {123.0f / 255.0f, 90.0f / 255.0f, 140.0f / 255.0f, 1.0f},
      {176.0f / 255.0f, 138.0f / 255.0f, 78.0f / 255.0f, 1.0f},
  };
  static const int RADII[4] = {0, 6, 18, 32};
  for (int i = 0; i < 4; ++i) {
    bd_rect rect = bd_rect_make(40 + i * 170, 146, 150, 100);
    bd_gfx_fill_round_rect(gfx, rect, RADII[i], FILLS[i]);
    bd_gfx_draw_text(gfx, fonts->badge, rect.x + 10, rect.y + 20,
                     bd_color_rgba(255, 248, 242, 220), "radius");
  }

  section_label(gfx, fonts, 40, 280, "圆角描边 / stroke");
  static const int THICKNESS[4] = {2, 4, 8, 16};
  for (int i = 0; i < 4; ++i) {
    bd_rect rect = bd_rect_make(40 + i * 170, 300, 150, 92);
    bd_gfx_fill_round_rect(gfx, rect, 18, bd_color_rgba(255, 255, 255, 140));
    bd_gfx_stroke_round_rect(gfx, rect, 18, THICKNESS[i], ACCENT);
  }

  section_label(gfx, fonts, 40, 430, "渐变与遮罩 / gradient & scrim");
  bd_gfx_fill_gradient(gfx, bd_rect_make(40, 450, 220, 120), 0,
                       bd_color_rgb(76, 92, 112), bd_color_rgb(196, 160, 120),
                       BD_GRADIENT_VERTICAL);
  bd_gfx_fill_gradient(gfx, bd_rect_make(280, 450, 220, 120), 0,
                       bd_color_rgb(93, 124, 90), bd_color_rgb(176, 138, 78),
                       BD_GRADIENT_HORIZONTAL);
  bd_gfx_fill_gradient3(gfx, bd_rect_make(520, 450, 220, 120), 28,
                        bd_color_rgb(174, 67, 43), bd_color_rgb(242, 237, 227),
                        bd_color_rgb(44, 46, 44), BD_GRADIENT_VERTICAL);
  bd_gfx_fill_gradient(gfx, bd_rect_make(760, 450, 220, 120), 0,
                       bd_color_rgb(63, 133, 135), bd_color_rgb(123, 90, 140),
                       BD_GRADIENT_VERTICAL);
  bd_gfx_fill_rect(gfx, bd_rect_make(760, 450, 220, 120),
                   bd_color_rgba(18, 16, 13, 115));
  bd_gfx_draw_text(gfx, fonts->body, 774, 500,
                   bd_color_rgba(255, 248, 242, 255), "scrim 45%");

  section_label(gfx, fonts, 40, 610, "纹理圆角裁剪 / rounded textures");
  bd_gfx_draw_texture(gfx, procedural, bd_rect_make(40, 630, 128, 128),
                      bd_color_rgba(255, 255, 255, 255));
  bd_gfx_draw_texture_round(gfx, procedural, bd_rect_make(190, 630, 128, 128),
                            bd_color_rgba(255, 255, 255, 255), 28);
  bd_gfx_draw_texture_round(gfx, png, bd_rect_make(340, 630, 128, 128),
                            bd_color_rgba(255, 255, 255, 255), 28);
  bd_gfx_draw_texture_round(gfx, jpg, bd_rect_make(490, 630, 128, 128),
                            bd_color_rgba(255, 255, 255, 255), 28);
  bd_gfx_draw_text(gfx, fonts->body, 650, 680, INK_2,
                   "sRGB · PNG/JPEG async · LRU");

  bd_gfx_push_clip(gfx, bd_rect_make(900, 630, 84, 84));
  bd_gfx_fill_gradient(gfx, bd_rect_make(880, 610, 130, 130), 0,
                       bd_color_rgb(174, 67, 43), bd_color_rgb(76, 92, 112),
                       BD_GRADIENT_HORIZONTAL);
  bd_gfx_pop_clip(gfx);
  bd_gfx_stroke_round_rect(gfx, bd_rect_make(900, 630, 84, 84), 6, 1, ACCENT);

  bd_gfx_end_frame(gfx);
}

static void render_text(bd_gfx *gfx, const bd_ui_fonts *fonts) {
  bd_gfx_damage_all(gfx);
  bd_gfx_begin_frame(gfx);
  page_background(gfx);

  bd_gfx_draw_text(gfx, fonts->serif, 40, 70, INK, "文字与回退");
  bd_gfx_draw_text(gfx, fonts->latin, 40, 98, INK_2,
                   "Type & fallback · Chinese + Latin + ellipsis");

  section_label(gfx, fonts, 40, 145, "拉丁主字体 + 中文回退 / Inter + Noto Sans SC");
  bd_gfx_draw_text(gfx, fonts->latin, 40, 190, INK,
                   "BrickDesk 书架 · 夜航 No. 03 · 已播放 38%");
  bd_gfx_draw_text(gfx, fonts->latin, 40, 222, INK_2,
                   "The quick brown fox 跳过了懒狗。");
  bd_gfx_draw_text(gfx, fonts->body, 40, 254, INK_2,
                   "缺字占位：\xF0\xA0\x80\x80");

  section_label(gfx, fonts, 40, 280, "标题 / title");
  bd_gfx_draw_text(gfx, fonts->title, 40, 325, INK, "小镇四季 · 第 3 年夏季");
  bd_gfx_draw_text(gfx, fonts->serif, 40, 388, INK, "纸面书架与封面墙");

  section_label(gfx, fonts, 40, 450, "按宽度截断 / ellipsis");
  bd_gfx_draw_text_ellipsized(
      gfx, fonts->title, 40, 492, 250,
      bd_color_rgb(174, 67, 43),
      "这是一个很长很长的标题，需要在宽度不足时截断并显示省略号");
  bd_gfx_draw_text_ellipsized(
      gfx, fonts->body, 40, 526, 200, bd_color_rgb(76, 92, 112),
      "A long subtitle that is truncated with an ellipsis at 320 pixels");
  bd_gfx_draw_text_ellipsized(gfx, fonts->title, 40, 560, 260, INK,
                              "短标题不会被截断");

  section_label(gfx, fonts, 40, 625, "字号 / sizes");
  bd_gfx_draw_text(gfx, fonts->badge, 40, 660, INK_2,
                   "BADGE 13 · 副标题 18 · 标题 20 · 衬线 34");
  bd_gfx_draw_text(gfx, fonts->body, 40, 692, INK_2,
                   "brickdesk 0.1.0 · TrimUI Brick Pro · 1024×768");

  bd_gfx_end_frame(gfx);
  bd_gfx_stats stats = bd_gfx_get_stats(gfx);
  printf("text: mixed_width=%d ellipsis_width=%d serif_width=%d missing=%d\n",
         bd_font_measure_text(fonts->latin,
                              "BrickDesk 书架 · 夜航 No. 03 · 已播放 38%"),
         bd_font_measure_text(fonts->title,
                              "这是一个很长很长的标题，需要在宽度不足时截断并显示省略号"),
         bd_font_measure_text(fonts->serif, "纸面书架与封面墙"),
         stats.missing_glyphs);
}

static bd_wall *create_wall(bd_gfx *gfx, const bd_ui_fonts *fonts) {
  bd_wall_style style;
  memset(&style, 0, sizeof(style));
  style.fonts = fonts;
  style.accent = bd_accent_palette_from(ACCENT, false);
  style.radius = 6;
  style.prefetch_pixels = 174;
  bd_wall *wall = bd_wall_create(gfx, &style);
  if (wall == NULL) {
    return NULL;
  }

  bd_wall_item items[14];
  memset(items, 0, sizeof(items));
  static char paths[8][512];
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
  struct Entry {
    int column, row, columns, rows;
    const char *title;
    const char *subtitle;
    const char *badge;
    float progress;
  };
  static const struct Entry ENTRIES[14] = {
      {0, 0, 3, 2, "小镇四季", "第 3 年夏季 · 已玩 41 小时", "GBA", -1.0f},
      {3, 0, 2, 2, "星海拾荒者", "第 7 扇区 · 已玩 6 小时", "PICO-8", -1.0f},
      {5, 0, 1, 1, "终端", "本机 Shell", "APP", -1.0f},
      {5, 1, 1, 1, "设置", "显示与声音", "APP", -1.0f},
      {0, 2, 1, 2, "音乐", "夜航 · 林间电台", "LIVE", 0.38f},
      {1, 2, 1, 1, "阅读", "《三体》第 14 章", "APP", -1.0f},
      {2, 2, 1, 1, "文件", "SD 卡已用 41 GB", "APP", -1.0f},
      {3, 2, 1, 1, "相册", "截图与照片 86 张", "APP", -1.0f},
      {4, 2, 1, 1, "无线传输", "浏览器传文件", "APP", -1.0f},
      {5, 2, 1, 2, "系统", "屏幕、网络与按键", "SYS", -1.0f},
      {1, 3, 1, 1, "雾港", "第二章「灯塔」", "PS", -1.0f},
      {2, 3, 1, 1, "深井", "到达地下 12 层", "SFC", -1.0f},
      {3, 3, 1, 1, "赤色公路", "最佳圈速 1:42.08", "PS", -1.0f},
      {4, 3, 1, 1, "收藏夹", "12 个收藏应用", "APP", -1.0f},
  };
  for (int i = 0; i < 14; ++i) {
    bd_rect rect = bd_rect_make(24 + ENTRIES[i].column * 174,
                                12 + ENTRIES[i].row * 174,
                                ENTRIES[i].columns * 168 +
                                    (ENTRIES[i].columns - 1) * 6,
                                ENTRIES[i].rows * 168 + (ENTRIES[i].rows - 1) * 6);
    items[i].rect = rect;
    items[i].title = ENTRIES[i].title;
    items[i].subtitle = ENTRIES[i].subtitle;
    items[i].badge = ENTRIES[i].badge;
    items[i].image_path = paths[i % 8];
    items[i].placeholder = COLORS[i % 8];
    items[i].progress = ENTRIES[i].progress;
  }
  if (!bd_wall_set_items(wall, items, 14)) {
    bd_wall_destroy(wall);
    return NULL;
  }
  bd_wall_set_focus(wall, 4);
  return wall;
}

static bool wait_wall_images(bd_wall *wall, bd_gfx *gfx, bd_platform *platform) {
  for (int tries = 0; tries < 300 && bd_wall_has_pending_images(wall); ++tries) {
    bd_gfx_begin_frame(gfx);
    bd_wall_begin_frame(wall, gfx,
                        bd_rect_make(0, 0, CAPTURE_WIDTH, CAPTURE_HEIGHT), 0, 0);
    bd_gfx_fill_rect(gfx, bd_rect_make(0, 0, CAPTURE_WIDTH, CAPTURE_HEIGHT), BG);
    bd_wall_draw(wall, gfx);
    bd_gfx_end_frame(gfx);
    bd_event event;
    bd_platform_wait_event(platform, &event, 5);
  }
  bd_image_cache_stats stats = bd_gfx_image_cache_get_stats(gfx);
  return stats.pending == 0 && stats.failed == 0;
}

static void draw_capture_top_bar(bd_gfx *gfx, const bd_ui_fonts *fonts) {
  bd_color top = BG;
  top.a = 0.72f;
  bd_color bottom = BG;
  bottom.a = 0.14f;
  bd_gfx_blur_region_feathered(gfx, bd_rect_make(0, 0, CAPTURE_WIDTH, 76), 0, top,
                               bottom, BD_GRADIENT_VERTICAL,
                               BD_BLUR_EDGE_BOTTOM, 20);
  bd_gfx_fill_rect(gfx, bd_rect_make(0, 75, CAPTURE_WIDTH, 1),
                   bd_color_rgba(30, 26, 21, 30));
  bd_gfx_fill_round_rect(gfx, bd_rect_make(32, 29, 14, 14), 4, ACCENT);
  bd_gfx_draw_text(gfx, fonts->latin, 56, 43, INK_2, "cover wall");
  bd_gfx_draw_text(gfx, fonts->title, 240, 45, INK, "封面墙");
  bd_gfx_draw_text(gfx, fonts->body, 330, 43, INK_2, "1×1 · 1×2 · 2×2 · 3×2");
  bd_gfx_draw_text(gfx, fonts->body, CAPTURE_WIDTH - 92, 43, INK_2, "20:41");
}

static void draw_capture_hint(bd_gfx *gfx, const bd_ui_fonts *fonts) {
  int x = (CAPTURE_WIDTH - 640) / 2;
  bd_color top = INK;
  top.a = 0.62f;
  bd_color bottom = INK;
  bottom.a = 0.82f;
  bd_gfx_blur_region_feathered(gfx, bd_rect_make(x, 700, 640, 56), 16, top,
                               bottom, BD_GRADIENT_VERTICAL, BD_BLUR_EDGE_TOP,
                               12);
  bd_gfx_draw_text(gfx, fonts->body, x + 28, 736,
                   bd_color_rgba(255, 248, 242, 230), "A 打开    B 返回    M 菜单");
}

static void render_tiles(bd_gfx *gfx, const bd_ui_fonts *fonts, bd_wall *wall,
                         bd_platform *platform) {
  if (!wait_wall_images(wall, gfx, platform)) {
    fprintf(stderr, "gfx_capture: wall images did not finish\n");
  }
  bd_gfx_damage_all(gfx);
  bd_gfx_begin_frame(gfx);
  bd_wall_begin_frame(wall, gfx,
                      bd_rect_make(0, 0, CAPTURE_WIDTH, CAPTURE_HEIGHT), 0, 0);
  page_background(gfx);
  bd_wall_draw(wall, gfx);
  draw_capture_top_bar(gfx, fonts);
  draw_capture_hint(gfx, fonts);
  bd_gfx_end_frame(gfx);
}

static void render_blur(bd_gfx *gfx, const bd_ui_fonts *fonts) {
  bd_gfx_damage_all(gfx);
  bd_gfx_begin_frame(gfx);
  page_background(gfx);
  bd_gfx_fill_round_rect(gfx, bd_rect_make(-80, 120, 440, 440), 220,
                         bd_color_rgb(174, 67, 43));
  bd_gfx_fill_round_rect(gfx, bd_rect_make(600, 80, 520, 520), 260,
                         bd_color_rgb(76, 92, 112));
  bd_gfx_fill_round_rect(gfx, bd_rect_make(300, 380, 500, 500), 250,
                         bd_color_rgb(93, 124, 90));
  bd_gfx_fill_gradient(gfx, bd_rect_make(0, 560, CAPTURE_WIDTH, 208), 0,
                       bd_color_rgba(242, 237, 227, 0),
                       bd_color_rgb(242, 237, 227), BD_GRADIENT_VERTICAL);

  bd_color top = BG;
  top.a = 0.66f;
  bd_color bottom = BG;
  bottom.a = 0.08f;
  bd_gfx_blur_region_feathered(gfx, bd_rect_make(0, 0, CAPTURE_WIDTH, 118), 0,
                               top, bottom, BD_GRADIENT_VERTICAL,
                               BD_BLUR_EDGE_BOTTOM, 24);
  bd_gfx_draw_text(gfx, fonts->serif, 40, 70, INK, "浮层模糊");
  bd_gfx_draw_text(gfx, fonts->latin, 220, 66, INK_2,
                   "1/4 downsample · 2 × 9-tap Gaussian · gradient overlay");

  bd_color hint_top = INK;
  hint_top.a = 0.58f;
  bd_color hint_bottom = INK;
  hint_bottom.a = 0.84f;
  bd_gfx_blur_region_feathered(
      gfx, bd_rect_make((CAPTURE_WIDTH - 640) / 2, 640, 640, 64), 18, hint_top,
      hint_bottom, BD_GRADIENT_VERTICAL, BD_BLUR_EDGE_TOP, 14);
  bd_gfx_draw_text(gfx, fonts->body, (CAPTURE_WIDTH - 640) / 2 + 28, 678,
                   bd_color_rgba(255, 248, 242, 235),
                   "A 打开    B 返回    M 菜单");
  bd_gfx_end_frame(gfx);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: gfx_capture OUT_DIR\n");
    return 2;
  }
  const char *out_dir = argv[1];

  bd_platform_config config;
  config.title = "brickdesk gfx capture";
  config.width = CAPTURE_WIDTH;
  config.height = CAPTURE_HEIGHT;
  config.hidden = true;
  bd_platform *platform = bd_platform_create(&config);
  if (platform == NULL) {
    return 1;
  }
  bd_gfx *gfx = bd_gfx_create(CAPTURE_WIDTH, CAPTURE_HEIGHT);
  if (gfx == NULL) {
    bd_platform_destroy(platform);
    return 1;
  }
  bd_ui_fonts fonts;
  if (!bd_ui_fonts_load(&fonts, gfx)) {
    fprintf(stderr, "gfx_capture: font load failed\n");
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }

  bd_texture *procedural = make_test_texture(gfx);
  bd_texture *png = NULL;
  bd_texture *jpg = NULL;
  char png_path[512];
  char jpg_path[512];
  snprintf(png_path, sizeof(png_path), "%s/covers/cover-01.png", BRICKDESK_ASSET_DIR);
  snprintf(jpg_path, sizeof(jpg_path), "%s/test-cover.jpg", BRICKDESK_TEST_ASSET_DIR);
  for (int tries = 0; tries < 300; ++tries) {
    bd_image_state png_state =
        bd_gfx_image_texture(gfx, png_path, 128, 128, &png);
    bd_image_state jpg_state =
        bd_gfx_image_texture(gfx, jpg_path, 128, 128, &jpg);
    if (png_state == BD_IMAGE_READY && jpg_state == BD_IMAGE_READY) {
      break;
    }
    if (png_state == BD_IMAGE_FAILED || jpg_state == BD_IMAGE_FAILED) {
      fprintf(stderr, "gfx_capture: image decode failed\n");
      break;
    }
    bd_gfx_begin_frame(gfx);
    bd_gfx_end_frame(gfx);
    bd_event event;
    bd_platform_wait_event(platform, &event, 5);
  }
  if (procedural == NULL || png == NULL || jpg == NULL) {
    fprintf(stderr, "gfx_capture: texture setup failed\n");
    bd_gfx_texture_destroy(gfx, procedural);
    bd_ui_fonts_free(&fonts);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }

  bd_wall *wall = create_wall(gfx, &fonts);
  if (wall == NULL) {
    fprintf(stderr, "gfx_capture: wall creation failed\n");
    bd_gfx_texture_destroy(gfx, procedural);
    bd_ui_fonts_free(&fonts);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }

  char path[1024];
  render_primitives(gfx, &fonts, procedural, png, jpg);
  snprintf(path, sizeof(path), "%s/primitives.png", out_dir);
  if (!bd_gfx_save_png(gfx, path)) {
    fprintf(stderr, "gfx_capture: save %s failed: %s\n", path, bd_gfx_last_error(gfx));
    return 1;
  }

  render_text(gfx, &fonts);
  snprintf(path, sizeof(path), "%s/text.png", out_dir);
  if (!bd_gfx_save_png(gfx, path)) {
    fprintf(stderr, "gfx_capture: save %s failed: %s\n", path, bd_gfx_last_error(gfx));
    return 1;
  }

  render_tiles(gfx, &fonts, wall, platform);
  snprintf(path, sizeof(path), "%s/tiles.png", out_dir);
  if (!bd_gfx_save_png(gfx, path)) {
    fprintf(stderr, "gfx_capture: save %s failed: %s\n", path, bd_gfx_last_error(gfx));
    return 1;
  }

  bd_keyboard *keyboard = bd_keyboard_create();
  if (keyboard != NULL &&
      bd_keyboard_open(keyboard, "Wi-Fi 密码", "hunter2", 64, true, NULL,
                       NULL)) {
    bd_gfx_damage_all(gfx);
    bd_gfx_begin_frame(gfx);
    page_background(gfx);
    bd_gfx_draw_text(gfx, fonts.serif, 40, 66, INK, "屏幕键盘");
    bd_keyboard_draw(keyboard, gfx, &fonts, ACCENT, 0);
    bd_gfx_end_frame(gfx);
    snprintf(path, sizeof(path), "%s/keyboard.png", out_dir);
    if (!bd_gfx_save_png(gfx, path)) {
      fprintf(stderr, "gfx_capture: save %s failed: %s\n", path,
              bd_gfx_last_error(gfx));
      return 1;
    }
  }
  bd_keyboard_destroy(keyboard);

  render_blur(gfx, &fonts);
  snprintf(path, sizeof(path), "%s/blur.png", out_dir);
  if (!bd_gfx_save_png(gfx, path)) {
    fprintf(stderr, "gfx_capture: save %s failed: %s\n", path, bd_gfx_last_error(gfx));
    return 1;
  }

  bd_gfx_stats stats = bd_gfx_get_stats(gfx);
  bd_wall_stats wall_stats = bd_wall_get_stats(wall);
  bd_image_cache_stats images = bd_gfx_image_cache_get_stats(gfx);
  printf("gfx_capture: draws=%d verts=%d damage=%lld blur=%d wall_visited=%d/%d "
         "textures=%d resident_kb=%zu\n",
         stats.draw_calls, stats.vertices, (long long)stats.damage_pixels,
         stats.blur_passes, wall_stats.visited_items, wall_stats.total_items,
         images.textures, images.resident_bytes / 1024u);

  bd_wall_destroy(wall);
  bd_gfx_texture_destroy(gfx, procedural);
  bd_ui_fonts_free(&fonts);
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  return 0;
}
