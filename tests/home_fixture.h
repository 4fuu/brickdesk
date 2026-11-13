#ifndef BRICKDESK_TESTS_HOME_FIXTURE_H
#define BRICKDESK_TESTS_HOME_FIXTURE_H

#include <stdio.h>
#include <string.h>

#include "brickdesk/home.h"

#ifndef BRICKDESK_ASSET_DIR
#define BRICKDESK_ASSET_DIR "assets"
#endif

#define HOME_DEMO_COUNT 60

/*
 * The demo catalogue mirrors the v3 prototype's 60-item home: the featured
 * eight keep their explicit spans and the rest use the prototype's 24-item
 * deterministic span cycle.  The ordered list makes first-fit pack densely,
 * so the screen has a v3-like mixture of 1x1, 1x2, 2x1, 2x2 and 3x2 tiles.
 */
static void home_demo_items(bd_home_item *items, int *count) {
  static const char *const NAMES[HOME_DEMO_COUNT] = {
      "阅读", "终端", "小镇四季", "时钟", "文件", "音乐", "相册", "设置",
      "雾港", "星海拾荒者", "无线传输", "深井", "赤色公路", "计算器",
      "播客", "天气", "相机", "录音机", "笔记", "地图", "书店", "画廊",
      "代码", "电台", "胶片", "潮汐", "远山", "夜行", "灯塔", "荒原",
      "迷城", "雨巷", "车站", "邮局", "花园", "森林", "海岛", "矿脉",
      "轨道", "风筝", "手帐", "词典", "沙漏", "尺规", "星图", "日历",
      "备忘录", "算盘", "黑胶", "磁带", "广播", "唱片", "剧场",
      "图书馆", "档案馆", "气象站", "天文台", "观测站", "实验室", "工作台"};
  static const int FEATURED_SPAN[8][2] = {
      {2, 1}, {1, 1}, {3, 2}, {1, 1}, {2, 1}, {2, 2}, {2, 2}, {2, 2}};
  static const char *const FEATURED_SUB[8] = {
      "《三体》读到第 14 章", "本机 Shell", "GBA · 第 3 年夏季",
      "工作日 · 06:42", "SD 卡 41.2 GB / 128 GB", "夜航 · 林间电台",
      "截图与照片 · 86 张", "显示、声音、网络与按键"};
  static const bool GAME[52] = {
      1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0,
      0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
  static char ids[HOME_DEMO_COUNT][16];
  static char subs[HOME_DEMO_COUNT][64];
  static char paths[8][512];
  for (int i = 0; i < 8; ++i) {
    snprintf(paths[i], sizeof(paths[i]), "%s/covers/home-%02d.jpg",
             BRICKDESK_ASSET_DIR, i + 1);
  }

  for (int i = 0; i < HOME_DEMO_COUNT; ++i) {
    bd_home_item *item = &items[i];
    memset(item, 0, sizeof(*item));
    snprintf(ids[i], sizeof(ids[i]), i == 5 ? "music" : "demo-%02d", i + 1);
    item->id = ids[i];
    item->title = NAMES[i];
    static const int FEATURED_COVER[8] = {0, 4, 1, 4, 7, 2, 3, 4};
    item->image_path = paths[i < 8 ? FEATURED_COVER[i] : (i * 5 + 3) % 8];
    item->placeholder = bd_color_rgb(58 + (i % 8) * 12, 52 + (i % 5) * 6, 48);
    if (i < 8) {
      snprintf(subs[i], sizeof(subs[i]), "%s", FEATURED_SUB[i]);
      item->span_w = FEATURED_SPAN[i][0];
      item->span_h = FEATURED_SPAN[i][1];
      item->flags = BD_HOME_ITEM_FAVORITE;
      if (i == 2) {
        item->flags |= BD_HOME_ITEM_GAME;
      }
      if (i == 5) {
        item->flags |= BD_HOME_ITEM_LIVE;
      }
    } else {
      int name = i - 8;
      int game = name < 52 ? GAME[name] : 0;
      if (game) {
        snprintf(subs[i], sizeof(subs[i]), "GBA · 已玩 %d 小时",
                 3 + (name * 7) % 40);
        item->flags |= BD_HOME_ITEM_GAME;
      } else if (name % 4 == 0) {
        snprintf(subs[i], sizeof(subs[i]), "本地应用 · 工具");
      } else if (name % 4 == 1) {
        snprintf(subs[i], sizeof(subs[i]), "媒体 · 本地");
      } else if (name % 4 == 2) {
        snprintf(subs[i], sizeof(subs[i]), "系统 · 本地");
      } else {
        snprintf(subs[i], sizeof(subs[i]), "游戏 · 已玩 %d 小时",
                 2 + (name * 3) % 30);
      }
      if (i % 9 == 0) {
        item->flags |= BD_HOME_ITEM_FAVORITE;
      }
      int cycle = i % 24;
      if (cycle == 0) {
        item->span_w = 3;
        item->span_h = 2;
      } else if (cycle == 3 || cycle == 7 || cycle == 12 || cycle == 17 ||
                 cycle == 21) {
        item->span_w = 2;
        item->span_h = 2;
      } else if (cycle == 5 || cycle == 15) {
        item->span_w = 1;
        item->span_h = 2;
      } else if (cycle == 9 || cycle == 14 || cycle == 19) {
        item->span_w = 2;
        item->span_h = 1;
      } else {
        item->span_w = 1;
        item->span_h = 1;
      }
    }
    item->subtitle = subs[i];
  }
  *count = HOME_DEMO_COUNT;
}

#endif /* BRICKDESK_TESTS_HOME_FIXTURE_H */
