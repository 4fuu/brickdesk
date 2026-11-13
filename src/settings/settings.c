#define _POSIX_C_SOURCE 200809L

#include "settings_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "brickdesk/theme.h"
#include "brickdesk/sysinfo.h"
#include "brickdesk/version.h"

#ifndef BRICKDESK_ASSET_DIR
#define BRICKDESK_ASSET_DIR "assets"
#endif

/* --------------------------------------------------------------- palette */

static void palette_dark(bd_settings_palette *palette) {
  palette->bg = bd_color_rgb(16, 15, 11);
  palette->surface = bd_color_rgb(25, 24, 20);
  palette->surface2 = bd_color_rgb(35, 32, 27);
  palette->surface3 = bd_color_rgb(52, 48, 41);
  palette->ink = bd_color_rgb(243, 237, 226);
  palette->ink2 = bd_color_rgb(176, 167, 153);
  palette->ink3 = bd_color_rgb(140, 131, 118);
  palette->line = bd_color_rgba(243, 237, 226, 23);
  palette->line_strong = bd_color_rgba(243, 237, 226, 48);
  palette->hint = bd_color_rgba(16, 15, 11, 246);
  palette->scrim = bd_color_rgba(8, 7, 5, 150);
}

void bd_settings_palette_apply(bd_settings *settings) {
  palette_dark(&settings->palette);
}

void bd_settings_apply_theme(bd_settings *settings) {
  const char *accent = bd_config_get_string(settings->config, "theme.accent",
                                            "persimmon");
  const char *hex = bd_config_accent_hex(accent, true);
  bd_color base;
  if (hex == NULL || !bd_color_from_hex(hex, &base)) {
    base = bd_color_rgb(224, 122, 95);
  }
  settings->style.accent_base = base;
  settings->accent = bd_accent_palette_from(base, true);
  bd_settings_palette_apply(settings);
}

/* ----------------------------------------------------------------- fonts */

static bd_font *load_font(bd_gfx *gfx, const char *name, int pixel_size) {
  char path[1024];
  int written = snprintf(path, sizeof(path), "%s/fonts/%s", BRICKDESK_ASSET_DIR,
                         name);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    return NULL;
  }
  return bd_font_create(gfx, path, pixel_size);
}

static bd_font *load_optional_full_font(bd_gfx *gfx) {
  static const char *const CANDIDATES[] = {
      "NotoSansSC-Full.ttf", "NotoSansSC-Full.otf",
      "NotoSansCJKsc-Regular.otf", "NotoSansSC-Regular.otf",
  };
  for (size_t i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); ++i) {
    char path[1024];
    int written = snprintf(path, sizeof(path), "%s/fonts/%s", BRICKDESK_ASSET_DIR,
                           CANDIDATES[i]);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
      continue;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
      continue;
    }
    fclose(file);
    bd_font *font = bd_font_create(gfx, path, 20);
    if (font != NULL) {
      return font;
    }
  }
  return NULL;
}

static void fonts_free(bd_settings_fonts *fonts) {
  bd_font_destroy(fonts->serif34);
  bd_font_destroy(fonts->serif25);
  bd_font_destroy(fonts->title);
  bd_font_destroy(fonts->value);
  bd_font_destroy(fonts->body);
  bd_font_destroy(fonts->badge);
  bd_font_destroy(fonts->caption);
  bd_font_destroy(fonts->fallback);
  memset(fonts, 0, sizeof(*fonts));
}

static bool fonts_load(bd_settings_fonts *fonts, bd_gfx *gfx) {
  memset(fonts, 0, sizeof(*fonts));
  fonts->serif34 = load_font(gfx, "NotoSerifSC-500-subset.ttf", 34);
  fonts->serif25 = load_font(gfx, "NotoSerifSC-500-subset.ttf", 25);
  /* Body copy and values are 22 px so the 1024x768 panel stays legible. */
  fonts->title = load_font(gfx, "NotoSansSC-700-subset.ttf", 24);
  fonts->value = load_font(gfx, "NotoSansSC-500-subset.ttf", 22);
  fonts->body = load_font(gfx, "NotoSansSC-500-subset.ttf", 22);
  fonts->badge = load_font(gfx, "NotoSansSC-700-subset.ttf", 20);
  fonts->caption = load_font(gfx, "NotoSansSC-500-subset.ttf", 20);
  fonts->fallback = load_optional_full_font(gfx);
  if (fonts->serif34 == NULL || fonts->serif25 == NULL ||
      fonts->title == NULL || fonts->value == NULL ||
      fonts->body == NULL || fonts->badge == NULL ||
      fonts->caption == NULL) {
    fonts_free(fonts);
    return false;
  }
  bd_font *all[] = {fonts->serif34, fonts->serif25, fonts->title,
                    fonts->value, fonts->body, fonts->badge,
                    fonts->caption};
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
    bd_font_set_fallback(all[i], fonts->fallback);
  }
  return true;
}

/* --------------------------------------------------------------- labels */

static const char *const APPEARANCE_KEYS[] = {
    "theme.accent", "theme.ambient", "ui.animations", "ui.blur", NULL};
static const char *const APPEARANCE_SECTIONS[] = {"theme", "ui", NULL};
static const char *const APPEARANCE_SKIP[] = {
    "theme.mode", "ui.language", "ui.time_format", "ui.clock_seconds",
    "ui.cover_cache_mb", NULL};
static const char *const SOUND_KEYS[] = {"audio.volume", "audio.mute", NULL};
static const char *const SOUND_SECTIONS[] = {"audio", NULL};
static const char *const DISPLAY_KEYS[] = {"display.brightness", NULL};
static const char *const DISPLAY_SECTIONS[] = {"display", NULL};
static const char *const DISPLAY_SKIP[] = {"display.width", "display.height",
                                           "display.refresh_hz", NULL};
static const char *const LIGHTS_KEYS[] = {
    "led.enabled",
    "led.m.brightness", "led.m.color", "led.m.effect",
    "led.lr.brightness", "led.lr.color", "led.lr.effect",
    "led.f1.brightness", "led.f1.color", "led.f1.effect",
    "led.f2.color", "led.f2.effect",
    "led.rear.brightness", "led.rear.color", "led.rear.effect",
    "led.indicator_brightness", "led.ambient", "led.ambient_brightness",
    NULL};
static const char *const LIGHTS_SECTIONS[] = {
    "led", "led.m", "led.lr", "led.f1", "led.f2", "led.rear", NULL};
static const char *const BACKGROUND_KEYS[] = {"session.max_background",
                                              "session.memory_budget_mb", NULL};
static const char *const BACKGROUND_SECTIONS[] = {"session", NULL};
static const char *const BACKGROUND_SKIP[] = {"session.state_quota_mb",
                                              "session.quick_resume", NULL};

const bd_settings_page_def BD_SETTINGS_PAGES[BD_SETTINGS_CATEGORY_COUNT] = {
    {"外观", "主题、强调色与背景", "强调色会作用到顶栏、进度、开关与焦点行。",
     APPEARANCE_KEYS, APPEARANCE_SECTIONS, APPEARANCE_SKIP},
    {"声音", "音量与静音", "音量与静音通过系统接口生效。", SOUND_KEYS,
     SOUND_SECTIONS, NULL},
    {"显示", "屏幕背光", "亮度通过系统接口应用到背光。", DISPLAY_KEYS,
     DISPLAY_SECTIONS, DISPLAY_SKIP},
    {"灯光", "分区颜色、效果与指示",
     "顶部灯条、摇杆环、F1/F2 与扳机分区独立设置；F2 与 F1 共用亮度。",
     LIGHTS_KEYS, LIGHTS_SECTIONS, NULL},
    {"按键", "组合键与冲突", "只读查看；冲突由输入层在加载 hotkeys.ini 时检测。",
     NULL, NULL, NULL},
    {"性能", "全局档位与按应用覆盖",
     "档位由 perfd 应用；修改在下次启动应用时生效。", NULL, NULL, NULL},
    {"后台", "数量与内存预算", "超过上限或内存预算时会提示结束最久未用的应用。",
     BACKGROUND_KEYS, BACKGROUND_SECTIONS, BACKGROUND_SKIP},
    {"插件", "插件列表与设置", "A 打开插件声明式设置；B 返回。", NULL, NULL,
     NULL},
    {"系统", "版本、存储与关于", "设置会即时保存到 local.ini。", NULL, NULL,
     NULL},
};

static const bd_settings_label LABELS[] = {
    {"theme.accent", "强调色", "焦点、进度与开关共用一个颜色", NULL, 0},
    {"theme.ambient", "背景", "预烘焙封面底图，或纯色", NULL, 0},
    {"ui.animations", "界面动画", "焦点移动、墙平移与面板滑入", NULL, 0},
    {"ui.blur", "界面模糊", "顶栏与提示栏的窄条模糊；关闭后改为渐变", NULL, 0},
    {"audio.volume", "音量", "系统输出音量", NULL, 1},
    {"audio.mute", "静音", "关闭全部系统声音", NULL, 1},
    {"display.brightness", "亮度", "屏幕背光", NULL, 1},
    {"led.enabled", "启用灯光", "关闭后所有分区熄灭", NULL, 1},
    {"led.m.brightness", "顶栏亮度", "顶部灯条 / Logo 分区", NULL, 1},
    {"led.m.color", "顶栏颜色", "顶部灯条 / Logo 分区", NULL, 0},
    {"led.m.effect", "顶栏效果", "顶部灯条 / Logo 分区", NULL, 0},
    {"led.lr.brightness", "摇杆环亮度", "左右摇杆环分区", NULL, 1},
    {"led.lr.color", "摇杆环颜色", "左右摇杆环分区", NULL, 0},
    {"led.lr.effect", "摇杆环效果", "左右摇杆环分区", NULL, 0},
    {"led.f1.brightness", "F1/F2 亮度", "两个 FN 键共用亮度", NULL, 1},
    {"led.f1.color", "F1 颜色", "FN 1 键分区", NULL, 0},
    {"led.f1.effect", "F1 效果", "FN 1 键分区", NULL, 0},
    {"led.f2.color", "F2 颜色", "FN 2 键分区", NULL, 0},
    {"led.f2.effect", "F2 效果", "FN 2 键分区", NULL, 0},
    {"led.rear.brightness", "扳机亮度", "L/R 扳机分区", NULL, 1},
    {"led.rear.color", "扳机颜色", "L/R 扳机分区", NULL, 0},
    {"led.rear.effect", "扳机效果", "L/R 扳机分区", NULL, 0},
    {"led.indicator_brightness", "指示亮度", "低电量、充电与睡眠指示", NULL, 1},
    {"led.ambient", "氛围灯", "按前台应用推送的颜色驱动灯光", NULL, 1},
    {"led.ambient_brightness", "氛围灯亮度", "氛围灯的最大亮度", NULL, 1},
    {"session.max_background", "后台应用上限", "同时保留在后台的应用数量",
     " 个", 1},
    {"session.memory_budget_mb", "内存预算", "后台应用的内存软上限", " MB", 16},
};

static const bd_settings_label *find_label(const char *key) {
  for (size_t i = 0; i < sizeof(LABELS) / sizeof(LABELS[0]); ++i) {
    if (strcmp(LABELS[i].key, key) == 0) {
      return &LABELS[i];
    }
  }
  return NULL;
}

static void copy_text(char *dst, size_t size, const char *src) {
  if (dst == NULL || size == 0) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  snprintf(dst, size, "%s", src);
}

static const char *fallback_label(char *buffer, size_t size, const char *key) {
  const char *dot = strrchr(key, '.');
  const char *name = dot != NULL ? dot + 1 : key;
  copy_text(buffer, size, name);
  return buffer;
}

static const char *accent_label(const char *accent) {
  static const struct {
    const char *key;
    const char *label;
  } NAMES[] = {
      {"persimmon", "柿红"}, {"coral", "珊瑚红"}, {"teal", "青"},
      {"blue", "蓝"},         {"green", "绿"},     {"violet", "紫"},
  };
  for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); ++i) {
    if (strcmp(NAMES[i].key, accent) == 0) {
      return NAMES[i].label;
    }
  }
  return "自定义";
}

static const char *enum_label(const char *key, const char *value) {
  if (strcmp(key, "theme.ambient") == 0) {
    if (strcmp(value, "baked") == 0) {
      return "预烘焙";
    }
    if (strcmp(value, "solid") == 0) {
      return "纯色";
    }
  } else if (strcmp(key, "session.quick_resume") == 0) {
    if (strcmp(value, "auto") == 0) {
      return "自动";
    }
    if (strcmp(value, "always") == 0) {
      return "总是";
    }
    if (strcmp(value, "never") == 0) {
      return "从不";
    }
  }
  return value;
}

/* ----------------------------------------------------------- row helpers */

static bool row_visible(const bd_settings *settings, int index) {
  if (index < 0 || index >= settings->row_count) {
    return false;
  }
  bd_rect rect = bd_settings_row_rect(settings, index);
  bd_rect table = bd_settings_table_rect(settings);
  return rect.y + rect.h > table.y && rect.y < table.y + table.h;
}

int bd_settings_visible_rows(const bd_settings *settings) {
  int count = 0;
  for (int i = 0; i < settings->row_count; ++i) {
    if (row_visible(settings, i)) {
      ++count;
    }
  }
  return count;
}

static int row_height(const bd_settings_row *row) {
  return row->description[0] != '\0' ? BD_SETTINGS_ROW_TALL
                                     : BD_SETTINGS_ROW_MIN;
}

int bd_settings_row_at_point(const bd_settings *settings, int y) {
  for (int i = 0; i < settings->row_count; ++i) {
    bd_rect rect = bd_settings_row_rect(settings, i);
    if (y >= rect.y && y < rect.y + rect.h) {
      return i;
    }
  }
  return -1;
}

static int plugin_tree_height(const bd_settings *settings) {
  if (settings->plugin_index < 0 ||
      settings->plugin_index >= settings->plugin_count) {
    return 0;
  }
  const bd_plugin_ui *ui =
      bd_plugin_ui_settings(settings->plugins[settings->plugin_index]);
  return bd_plugin_ui_measure(ui, BD_SETTINGS_TABLE_W);
}

static int max_scroll(const bd_settings *settings) {
  if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN) {
    int content = plugin_tree_height(settings);
    bd_rect table = bd_settings_table_rect(settings);
    int limit = content + 2 - table.h;
    if (limit > 0) {
      limit += BD_SETTINGS_SCROLL_PAD;
    }
    return limit > 0 ? limit : 0;
  }
  int total = 0;
  for (int i = 0; i < settings->row_count; ++i) {
    total += row_height(&settings->rows[i]);
  }
  bd_rect table = bd_settings_table_rect(settings);
  int limit = total - table.h;
  return limit > 0 ? limit : 0;
}

int bd_settings_max_scroll(const bd_settings *settings) {
  return max_scroll(settings);
}

static void clamp_scroll(bd_settings *settings) {
  int limit = max_scroll(settings);
  if (settings->scroll > limit) {
    settings->scroll = limit;
  }
  if (settings->scroll < 0) {
    settings->scroll = 0;
  }
}

/* ------------------------------------------------------------ row build */

static bool row_has_key(const bd_settings *settings, const char *key) {
  for (int i = 0; i < settings->row_count; ++i) {
    if (strcmp(settings->rows[i].key, key) == 0) {
      return true;
    }
  }
  return false;
}

static bool key_in_list(const char *const *list, const char *key) {
  if (list == NULL) {
    return false;
  }
  for (size_t i = 0; list[i] != NULL; ++i) {
    if (strcmp(list[i], key) == 0) {
      return true;
    }
  }
  return false;
}

static bd_settings_row *append_row(bd_settings *settings) {
  if (settings->row_count >= BD_SETTINGS_MAX_ROWS) {
    return NULL;
  }
  bd_settings_row *row = &settings->rows[settings->row_count++];
  memset(row, 0, sizeof(*row));
  row->step = 1;
  return row;
}

static bool add_config_row(bd_settings *settings, const char *key) {
  if (row_has_key(settings, key)) {
    return true;
  }
  bd_config_value value;
  if (!bd_config_get(settings->config, key, &value)) {
    return false;
  }
  bd_config_schema_info info;
  if (!bd_config_describe(settings->config, key, &info)) {
    return false;
  }
  bd_settings_row *row = append_row(settings);
  if (row == NULL) {
    return false;
  }
  copy_text(row->key, sizeof(row->key), key);
  const bd_settings_label *label = find_label(key);
  if (label != NULL) {
    copy_text(row->label, sizeof(row->label), label->label);
    copy_text(row->description, sizeof(row->description), label->description);
    copy_text(row->unit, sizeof(row->unit), label->unit);
    row->step = label->step;
  } else {
    char buffer[96];
    copy_text(row->label, sizeof(row->label),
              fallback_label(buffer, sizeof(buffer), key));
    row->step = 0;
  }
  row->type = info.type;
  row->min_value = info.min_value;
  row->max_value = info.max_value;
  row->enum_values = info.enum_values;
  row->enum_count = (int)info.enum_count;
  switch (info.type) {
    case BD_CONFIG_BOOL:
      row->kind = BD_SETTINGS_ROW_TOGGLE;
      break;
    case BD_CONFIG_INT:
    case BD_CONFIG_DURATION:
      row->kind = BD_SETTINGS_ROW_SLIDER;
      break;
    case BD_CONFIG_ENUM:
      row->kind = BD_SETTINGS_ROW_ENUM;
      break;
    case BD_CONFIG_ACCENT:
      row->kind = BD_SETTINGS_ROW_ACCENT;
      break;
    default:
      row->kind = BD_SETTINGS_ROW_INFO;
      break;
  }
  if (row->kind == BD_SETTINGS_ROW_SLIDER && row->step <= 0) {
    long span = row->max_value - row->min_value;
    row->step = span >= 200 ? 16 : 1;
  }
  return true;
}

static void add_info_row(bd_settings *settings, const char *key,
                         const char *label, const char *description,
                         const char *value) {
  bd_settings_row *row = append_row(settings);
  if (row == NULL) {
    return;
  }
  row->kind = BD_SETTINGS_ROW_INFO;
  copy_text(row->key, sizeof(row->key), key);
  copy_text(row->label, sizeof(row->label), label);
  copy_text(row->description, sizeof(row->description), description);
  copy_text(row->value, sizeof(row->value), value);
}

static bool build_config_page(bd_settings *settings,
                              const bd_settings_page_def *page) {
  if (page->preferred != NULL) {
    for (size_t i = 0; page->preferred[i] != NULL; ++i) {
      add_config_row(settings, page->preferred[i]);
    }
  }
  if (page->sections != NULL) {
    size_t count = bd_config_key_count(settings->config);
    for (size_t i = 0; i < count; ++i) {
      const char *key = bd_config_key(settings->config, i);
      if (key == NULL || row_has_key(settings, key) ||
          key_in_list(page->skip, key)) {
        continue;
      }
      bool matches = false;
      for (size_t s = 0; page->sections[s] != NULL; ++s) {
        size_t length = strlen(page->sections[s]);
        if (strncmp(key, page->sections[s], length) == 0 &&
            key[length] == '.') {
          matches = true;
          break;
        }
      }
      if (matches) {
        add_config_row(settings, key);
      }
    }
  }
  return true;
}

static const char *hotkey_trigger_label(const bd_hotkey *hotkey, char *buffer,
                                        size_t size) {
  if (hotkey->hold_ms > 0) {
    snprintf(buffer, size, "长按 %d ms", hotkey->hold_ms);
    return buffer;
  }
  if (hotkey->repeat) {
    snprintf(buffer, size, "按住重复 %d ms", hotkey->repeat_ms);
    return buffer;
  }
  if (hotkey->step_count > 1) {
    snprintf(buffer, size, "序列 %d 步", hotkey->step_count);
    return buffer;
  }
  if (hotkey->step_count == 1 && hotkey->steps[0].key_count > 1) {
    snprintf(buffer, size, "组合键 %d 键", hotkey->steps[0].key_count);
    return buffer;
  }
  if (hotkey->step_count == 1 && hotkey->steps[0].double_tap) {
    return "双击";
  }
  return "单击";
}

static bool build_hotkey_page(bd_settings *settings) {
  if (!settings->hotkeys_loaded) {
    add_info_row(settings, "!hotkeys", "按键不可用",
                 "hotkeys.ini 加载失败，请检查文件", "");
    return true;
  }
  if (settings->hotkeys.conflict_count > 0) {
    const bd_hotkey_conflict *first = &settings->hotkeys.conflicts[0];
    char description[224];
    const bd_hotkey *a =
        first->first >= 0 && (size_t)first->first < settings->hotkeys.count
            ? &settings->hotkeys.items[first->first]
            : NULL;
    const bd_hotkey *b =
        first->second >= 0 && (size_t)first->second < settings->hotkeys.count
            ? &settings->hotkeys.items[first->second]
            : NULL;
    if (first->kind == BD_HOTKEY_CONFLICT_DUPLICATE && a != NULL) {
      snprintf(description, sizeof(description),
               "重复绑定：%.64s；后一个绑定被忽略", a->combo);
    } else if (first->kind == BD_HOTKEY_CONFLICT_PREFIX && a != NULL &&
               b != NULL) {
      snprintf(description, sizeof(description),
               "%.64s 与 %.64s 是前缀关系；按优先级解析", a->combo, b->combo);
    } else if (a != NULL && b != NULL) {
      snprintf(description, sizeof(description),
               "%.64s 与 %.64s 首键重叠；只会有一个先触发", a->combo, b->combo);
    } else {
      snprintf(description, sizeof(description), "输入层报告了按键冲突");
    }
    size_t extra = settings->hotkeys.conflict_count - 1;
    if (extra > 0) {
      size_t used = strlen(description);
      snprintf(description + used, sizeof(description) - used,
               "；另有 %zu 条，冲突绑定已标出", extra);
    }
    bd_settings_row *row = append_row(settings);
    if (row != NULL) {
      row->kind = BD_SETTINGS_ROW_CONFLICT;
      copy_text(row->key, sizeof(row->key), "!conflict.0");
      copy_text(row->label, sizeof(row->label), "按键冲突");
      copy_text(row->description, sizeof(row->description), description);
      snprintf(row->value, sizeof(row->value), "%zu 条",
               settings->hotkeys.conflict_count);
      row->warning = true;
      row->bullet_slot = true;
    }
  }
  for (size_t i = 0; i < settings->hotkeys.count; ++i) {
    const bd_hotkey *hotkey = &settings->hotkeys.items[i];
    bd_settings_row *row = append_row(settings);
    if (row == NULL) {
      break;
    }
    row->kind = BD_SETTINGS_ROW_INFO;
    snprintf(row->key, sizeof(row->key), "!hotkey.%s", hotkey->id);
    copy_text(row->label, sizeof(row->label), hotkey->combo);
    char trigger[48];
    snprintf(row->description, sizeof(row->description), "%s · %s",
             hotkey->action,
             hotkey_trigger_label(hotkey, trigger, sizeof(trigger)));
    copy_text(row->value, sizeof(row->value), hotkey->id);
    row->index = (int)i;
    row->bullet_slot = true;
    for (size_t c = 0; c < settings->hotkeys.conflict_count; ++c) {
      const bd_hotkey_conflict *conflict = &settings->hotkeys.conflicts[c];
      if (conflict->first == (int)i || conflict->second == (int)i) {
        row->warning = true;
        break;
      }
    }
  }
  if (settings->row_count == 0) {
    add_info_row(settings, "!hotkeys", "没有热键绑定",
                 "hotkeys.ini 中没有可用的 [hotkey.<id>] 段", "");
  }
  return true;
}

static bool build_perf_page(bd_settings *settings) {
  if (settings->profile_count > 0) {
    bd_settings_row *row = append_row(settings);
    if (row != NULL) {
      row->kind = BD_SETTINGS_ROW_ENUM;
      copy_text(row->key, sizeof(row->key), "perf.global_profile");
      copy_text(row->label, sizeof(row->label), "全局性能档位");
      copy_text(row->description, sizeof(row->description),
                "桌面与未单独设置的应用使用");
      row->type = BD_CONFIG_STRING;
      row->enum_values = settings->profile_names;
      row->enum_count = settings->profile_count;
    }
  } else {
    add_info_row(settings, "!perf", "没有性能档位",
                 "perf.ini 中没有可用的 [profile.<name>] 段", "");
  }
  int app_rows = 0;
  size_t count = bd_config_key_count(settings->config);
  for (size_t i = 0; i < count; ++i) {
    const char *key = bd_config_key(settings->config, i);
    static const char PREFIX[] = "app.";
    static const char SUFFIX[] = ".profile";
    size_t key_length = strlen(key);
    size_t suffix_length = sizeof(SUFFIX) - 1;
    if (strncmp(key, PREFIX, sizeof(PREFIX) - 1) != 0 ||
        key_length <= (sizeof(PREFIX) - 1) + suffix_length ||
        strcmp(key + key_length - suffix_length, SUFFIX) != 0) {
      continue;
    }
    char app[BD_PERF_NAME_MAX + 1];
    size_t name_length = key_length - (sizeof(PREFIX) - 1) - suffix_length;
    if (name_length > BD_PERF_NAME_MAX) {
      continue;
    }
    memcpy(app, key + sizeof(PREFIX) - 1, name_length);
    app[name_length] = '\0';
    bd_settings_row *row = append_row(settings);
    if (row == NULL) {
      break;
    }
    row->kind = settings->profile_count > 0 ? BD_SETTINGS_ROW_ENUM
                                            : BD_SETTINGS_ROW_INFO;
    copy_text(row->key, sizeof(row->key), key);
    copy_text(row->label, sizeof(row->label), app);
    copy_text(row->description, sizeof(row->description),
              "启动时使用的性能档位");
    row->type = BD_CONFIG_STRING;
    row->enum_values = settings->profile_names;
    row->enum_count = settings->profile_count;
    ++app_rows;
  }
  if (app_rows == 0) {
    add_info_row(settings, "!perf.apps", "没有按应用覆盖",
                 "在 perf.ini 的 [app.<id>] 中增加 profile", "");
  }
  return true;
}

static const char *plugin_state_text(bd_plugin *plugin) {
  switch (bd_plugin_state_of(plugin)) {
    case BD_PLUGIN_RUNNING:
      return "运行中";
    case BD_PLUGIN_STARTING:
    case BD_PLUGIN_HANDSHAKING:
      return "启动中";
    case BD_PLUGIN_BACKOFF:
      return "重启中";
    case BD_PLUGIN_QUARANTINED:
      return "不可用";
    case BD_PLUGIN_STOPPING:
      return "停止中";
    default:
      return "未运行";
  }
}

static bool build_plugin_page(bd_settings *settings) {
  if (settings->plugin_count == 0) {
    add_info_row(settings, "!plugins", "没有发现插件",
                 "把插件放到 plugins/ 目录后重新扫描", "");
    return true;
  }
  for (int i = 0; i < settings->plugin_count; ++i) {
    bd_plugin *plugin = settings->plugins[i];
    const bd_plugin_manifest *manifest = bd_plugin_manifest_of(plugin);
    const char *name = manifest != NULL && manifest->name[0] != '\0'
                           ? manifest->name
                           : bd_plugin_id(plugin);
    const char *version = manifest != NULL ? manifest->version : "";
    bd_settings_row *row = append_row(settings);
    if (row == NULL) {
      break;
    }
    row->kind = BD_SETTINGS_ROW_PLUGIN;
    snprintf(row->key, sizeof(row->key), "!plugin.%s", bd_plugin_id(plugin));
    copy_text(row->label, sizeof(row->label), name);
    snprintf(row->description, sizeof(row->description), "v%s · %s", version,
             plugin_state_text(plugin));
    copy_text(row->value, sizeof(row->value),
              manifest != NULL &&
                      (manifest->capabilities & BD_PLUGIN_CAP_SETTINGS) != 0
                  ? "设置"
                  : "无设置");
    row->index = i;
  }
  return true;
}

static void format_storage(const bd_settings *settings, char *buffer,
                           size_t size) {
  if (settings->storage.total_mb <= 0) {
    snprintf(buffer, size, "不可用");
    return;
  }
  double total = (double)settings->storage.total_mb / 1024.0;
  double free_gb = (double)settings->storage.free_mb / 1024.0;
  if (free_gb < 0.0) {
    free_gb = 0.0;
  }
  snprintf(buffer, size, "可用 %.1f GB / %.1f GB", free_gb, total);
}

static bool build_system_page(bd_settings *settings) {
  char version[96];
  snprintf(version, sizeof(version), "brickdesk %s (%s)", BRICKDESK_VERSION,
           BRICKDESK_BACKEND_NAME);
  add_info_row(settings, "!version", "版本", "当前构建", version);
  char storage[96];
  format_storage(settings, storage, sizeof(storage));
  add_info_row(settings, "!storage", "存储", "配置与存档所在分区", storage);
  int cpu_c = -999;
  int gpu_c = -999;
  if (bd_sys_temperature(NULL, &cpu_c, &gpu_c)) {
    if (cpu_c != -999) {
      char text[32];
      snprintf(text, sizeof(text), "%d °C", cpu_c);
      add_info_row(settings, "!temp.cpu", "CPU 温度", "热区传感器读数", text);
    }
    if (gpu_c != -999) {
      char text[32];
      snprintf(text, sizeof(text), "%d °C", gpu_c);
      add_info_row(settings, "!temp.gpu", "GPU 温度", "热区传感器读数", text);
    }
  }
  if (settings->battery_history != NULL) {
    char remaining[48];
    int minutes = bd_battery_history_remaining_minutes(
        settings->battery_history, (int64_t)time(NULL));
    if (minutes < 0) {
      snprintf(remaining, sizeof(remaining), "估算中");
    } else if (minutes >= 60) {
      snprintf(remaining, sizeof(remaining), "%d 小时 %d 分", minutes / 60,
               minutes % 60);
    } else {
      snprintf(remaining, sizeof(remaining), "%d 分钟", minutes);
    }
    add_info_row(settings, "!battery.remaining", "预计剩余",
                 "按最近一小时放电速度估算，仅供参考", remaining);
    bd_settings_row *history = append_row(settings);
    if (history != NULL) {
      history->kind = BD_SETTINGS_ROW_HISTORY;
      copy_text(history->key, sizeof(history->key), "!battery.history");
      copy_text(history->label, sizeof(history->label), "电量历史");
      copy_text(history->description, sizeof(history->description),
                "最近 24 小时 · 每 5 分钟采样");
    }
  }
  add_info_row(settings, "!about", "关于 brickdesk",
               "为 TrimUI Brick Pro 设计的桌面 · 1024×768 · 仅手柄操作", "");
  bd_settings_row *row = append_row(settings);
  if (row != NULL) {
    row->kind = BD_SETTINGS_ROW_RESET;
    copy_text(row->key, sizeof(row->key), "!reset");
    copy_text(row->label, sizeof(row->label), "恢复默认设置");
    copy_text(row->description, sizeof(row->description),
              "清除本地修改，立即恢复默认值");
    copy_text(row->value, sizeof(row->value), "重置");
  }
  return true;
}

static bool build_accent_page(bd_settings *settings) {
  static const char *const LABELS_RGB[3] = {"红", "绿", "蓝"};
  const int values[3] = {settings->custom_r, settings->custom_g,
                         settings->custom_b};
  static const char *const KEYS[3] = {"!accent.r", "!accent.g", "!accent.b"};
  for (int i = 0; i < 3; ++i) {
    bd_settings_row *row = append_row(settings);
    if (row == NULL) {
      break;
    }
    row->kind = BD_SETTINGS_ROW_SLIDER;
    copy_text(row->key, sizeof(row->key), KEYS[i]);
    copy_text(row->label, sizeof(row->label), LABELS_RGB[i]);
    row->min_value = 0;
    row->max_value = 255;
    row->step = 1;
    row->index = values[i];
  }
  return true;
}

bool bd_settings_rebuild_rows(bd_settings *settings) {
  settings->row_count = 0;
  memset(settings->rows, 0, sizeof(settings->rows));
  if (settings->subpage == BD_SETTINGS_SUBPAGE_ACCENT) {
    return build_accent_page(settings);
  }
  if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN) {
    return true; /* the plugin renderer owns the table */
  }
  switch (settings->category) {
    case BD_SETTINGS_CATEGORY_APPEARANCE:
    case BD_SETTINGS_CATEGORY_SOUND:
    case BD_SETTINGS_CATEGORY_DISPLAY:
    case BD_SETTINGS_CATEGORY_LIGHTS:
    case BD_SETTINGS_CATEGORY_BACKGROUND:
      return build_config_page(settings, &BD_SETTINGS_PAGES[settings->category]);
    case BD_SETTINGS_CATEGORY_KEYS:
      return build_hotkey_page(settings);
    case BD_SETTINGS_CATEGORY_PERFORMANCE:
      return build_perf_page(settings);
    case BD_SETTINGS_CATEGORY_PLUGINS:
      return build_plugin_page(settings);
    case BD_SETTINGS_CATEGORY_SYSTEM:
      return build_system_page(settings);
    default:
      return false;
  }
}

/* ---------------------------------------------------------- data reload */

static void load_hotkeys(bd_settings *settings) {
  memset(&settings->hotkeys, 0, sizeof(settings->hotkeys));
  settings->hotkeys_loaded =
      bd_hotkey_table_load(&settings->hotkeys, settings->config);
  settings->conflict_count =
      settings->hotkeys_loaded ? (int)settings->hotkeys.conflict_count : 0;
}

static void load_perf(bd_settings *settings, bool reload) {
  settings->profile_count = 0;
  settings->profile_names[0] = NULL;
  settings->perf_loaded = false;
  if (reload) {
    char error[128] = {0};
    if (bd_perf_policy_load(&settings->perf, bd_config_dir(settings->config),
                            error, sizeof(error))) {
      settings->perf_loaded = true;
    }
  }
  if (!settings->perf_loaded) {
    return;
  }
  for (size_t i = 0; i < settings->perf.profile_count &&
                     settings->profile_count < BD_PERF_MAX_PROFILES;
       ++i) {
    settings->profile_names[settings->profile_count++] =
        settings->perf.profiles[i].name;
  }
  settings->profile_names[settings->profile_count] = NULL;
}

static int compare_plugins(const void *left, const void *right) {
  bd_plugin *const *a = (bd_plugin *const *)left;
  bd_plugin *const *b = (bd_plugin *const *)right;
  return strcmp(bd_plugin_id(*a), bd_plugin_id(*b));
}

static void load_plugins(bd_settings *settings) {
  settings->plugin_count = 0;
  if (settings->host == NULL) {
    return;
  }
  size_t count = bd_plugin_host_count(settings->host);
  for (size_t i = 0; i < count && settings->plugin_count < BD_SETTINGS_MAX_PLUGINS;
       ++i) {
    bd_plugin *plugin = bd_plugin_host_at(settings->host, i);
    if (plugin != NULL) {
      settings->plugins[settings->plugin_count++] = plugin;
    }
  }
  if (settings->plugin_count > 1) {
    qsort(settings->plugins, (size_t)settings->plugin_count,
          sizeof(settings->plugins[0]), compare_plugins);
  }
}

bool bd_settings_rebuild(bd_settings *settings) {
  load_hotkeys(settings);
  load_perf(settings, settings->category == BD_SETTINGS_CATEGORY_PERFORMANCE ||
                          settings->perf_loaded);
  load_plugins(settings);
  bool ok = bd_settings_rebuild_rows(settings);
  clamp_scroll(settings);
  return ok;
}

/* --------------------------------------------------------------- damage */

void bd_settings_mark_row(bd_settings *settings, int index) {
  if (index < 0 || index >= BD_SETTINGS_MAX_ROWS) {
    return;
  }
  settings->dirty_rows[index / 64] |= (uint64_t)1 << (index % 64);
}

void bd_settings_mark_all(bd_settings *settings) {
  settings->full_dirty = true;
  settings->table_dirty = true;
  settings->hint_dirty = true;
}

void bd_settings_damage_row(bd_settings *settings, int index) {
  if (index < 0 || index >= settings->row_count) {
    return;
  }
  bd_rect rect = bd_settings_row_rect(settings, index);
  rect.x -= 2;
  rect.y -= 2;
  rect.w += 4;
  rect.h += 4;
  bd_settings_mark_row(settings, index);
  bd_gfx_damage_rect(settings->gfx, rect);
}

void bd_settings_damage(bd_settings *settings) {
  if (settings == NULL) {
    return;
  }
  bd_settings_mark_all(settings);
  bd_gfx_damage_all(settings->gfx);
}

/* --------------------------------------------------------------- values */

static void set_error(bd_settings *settings, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(settings->error, sizeof(settings->error), fmt, args);
  va_end(args);
  settings->hint_dirty = true;
  int top = settings->style.height - BD_SETTINGS_HINT_HEIGHT;
  bd_gfx_damage_rect(settings->gfx,
                     bd_rect_make(0, top, settings->style.width,
                                  BD_SETTINGS_HINT_HEIGHT));
}

static void clear_error(bd_settings *settings) {
  settings->error[0] = '\0';
}

static bool apply_system_value(bd_settings *settings, const char *key,
                               const char *value) {
  if (strcmp(key, "display.brightness") == 0) {
    int percent = atoi(value);
    if (settings->system.set_brightness == NULL) {
      set_error(settings, "背光接口不可用");
      return false;
    }
    if (!settings->system.set_brightness(settings->system.user, percent)) {
      set_error(settings, "背光接口拒绝了亮度 %d", percent);
      return false;
    }
  } else if (strcmp(key, "audio.volume") == 0) {
    int percent = atoi(value);
    if (settings->system.set_volume == NULL) {
      set_error(settings, "音频接口不可用");
      return false;
    }
    if (!settings->system.set_volume(settings->system.user, percent)) {
      set_error(settings, "音频接口拒绝了音量 %d", percent);
      return false;
    }
  } else if (strcmp(key, "audio.mute") == 0) {
    bool muted = strcmp(value, "true") == 0;
    if (settings->system.set_muted == NULL) {
      set_error(settings, "音频接口不可用");
      return false;
    }
    if (!settings->system.set_muted(settings->system.user, muted)) {
      set_error(settings, "音频接口拒绝了静音切换");
      return false;
    }
  }
  return true;
}

static bool read_system_value(const bd_settings *settings, const char *key,
                              bd_config_value *value) {
  if (strcmp(key, "display.brightness") == 0 &&
      settings->system.get_brightness != NULL) {
    int percent = 0;
    if (!settings->system.get_brightness(settings->system.user, &percent)) {
      return false;
    }
    memset(value, 0, sizeof(*value));
    value->type = BD_CONFIG_INT;
    value->integer = percent;
    return true;
  }
  if (strcmp(key, "audio.volume") == 0 &&
      settings->system.get_volume != NULL) {
    int percent = 0;
    if (!settings->system.get_volume(settings->system.user, &percent)) {
      return false;
    }
    memset(value, 0, sizeof(*value));
    value->type = BD_CONFIG_INT;
    value->integer = percent;
    return true;
  }
  if (strcmp(key, "audio.mute") == 0 && settings->system.get_muted != NULL) {
    bool muted = false;
    if (!settings->system.get_muted(settings->system.user, &muted)) {
      return false;
    }
    memset(value, 0, sizeof(*value));
    value->type = BD_CONFIG_BOOL;
    value->boolean = muted;
    return true;
  }
  return bd_config_get(settings->config, key, value);
}

static bool has_system_reader(const bd_settings *settings, const char *key) {
  if (strcmp(key, "display.brightness") == 0) {
    return settings->system.get_brightness != NULL;
  }
  if (strcmp(key, "audio.volume") == 0) {
    return settings->system.get_volume != NULL;
  }
  if (strcmp(key, "audio.mute") == 0) {
    return settings->system.get_muted != NULL;
  }
  return false;
}

static bool config_set(bd_settings *settings, const char *key,
                       const char *value) {
  if (!apply_system_value(settings, key, value)) {
    return false;
  }
  bool show_error = settings->error[0] != '\0';
  char error[128] = {0};
  if (!bd_config_set(settings->config, key, value, error, sizeof(error))) {
    set_error(settings, "%s", error);
    return false;
  }
  if (!bd_config_save(settings->config, error, sizeof(error))) {
    set_error(settings, "已应用但保存失败：%s", error);
    show_error = true;
  } else {
    clear_error(settings);
  }
  if (show_error) {
    settings->hint_dirty = true;
    int top = settings->style.height - BD_SETTINGS_HINT_HEIGHT;
    bd_gfx_damage_rect(settings->gfx,
                       bd_rect_make(0, top, settings->style.width,
                                    BD_SETTINGS_HINT_HEIGHT));
  }
  return true;
}

static bool row_is_theme(const bd_settings_row *row) {
  return strcmp(row->key, "theme.accent") == 0 ||
         strcmp(row->key, "theme.ambient") == 0;
}

static bool set_config_value(bd_settings *settings, bd_settings_row *row,
                             const char *value) {
  if (!config_set(settings, row->key, value)) {
    bd_settings_mark_all(settings);
    bd_gfx_damage_all(settings->gfx);
    return false;
  }
  if (row_is_theme(row)) {
    bd_settings_apply_theme(settings);
    bd_settings_mark_all(settings);
    bd_gfx_damage_all(settings->gfx);
    return true;
  }
  int index = (int)(row - settings->rows);
  bd_settings_damage_row(settings, index);
  return true;
}

static int slider_value(const bd_settings *settings, const bd_settings_row *row) {
  if (row->key[0] == '!' && strncmp(row->key, "!accent.", 8) == 0) {
    return row->index;
  }
  return (int)bd_config_get_int(settings->config, row->key,
                                row->min_value);
}

long bd_settings_row_slider_value(const bd_settings *settings,
                                  const bd_settings_row *row) {
  return slider_value(settings, row);
}

void bd_settings_row_value(const bd_settings *settings,
                           const bd_settings_row *row, char *buffer,
                           size_t size) {
  if (buffer == NULL || size == 0) {
    return;
  }
  buffer[0] = '\0';
  if (row == NULL) {
    return;
  }
  switch (row->kind) {
    case BD_SETTINGS_ROW_TOGGLE: {
      bd_config_value value;
      if (has_system_reader(settings, row->key)) {
        if (!read_system_value(settings, row->key, &value)) {
          snprintf(buffer, size, "不可用");
          break;
        }
        snprintf(buffer, size, "%s", value.boolean ? "开" : "关");
        break;
      }
      bool enabled = bd_config_get_bool(settings->config, row->key, false);
      snprintf(buffer, size, "%s", enabled ? "开" : "关");
      break;
    }
    case BD_SETTINGS_ROW_SLIDER: {
      if (row->key[0] == '!' && strncmp(row->key, "!accent.", 8) == 0) {
        snprintf(buffer, size, "%d", row->index);
        break;
      }
      bd_config_value value;
      if (!read_system_value(settings, row->key, &value)) {
        snprintf(buffer, size, "不可用");
        break;
      }
      if (value.type == BD_CONFIG_DURATION) {
        long ms = value.integer;
        if (ms >= 60000 && ms % 60000 == 0) {
          snprintf(buffer, size, "%ld 分钟", ms / 60000);
        } else if (ms >= 1000 && ms % 1000 == 0) {
          snprintf(buffer, size, "%ld 秒", ms / 1000);
        } else {
          snprintf(buffer, size, "%ld ms", ms);
        }
      } else {
        snprintf(buffer, size, "%ld%s", value.integer, row->unit);
      }
      break;
    }
    case BD_SETTINGS_ROW_ENUM: {
      const char *text =
          bd_config_get_string(settings->config, row->key, "—");
      snprintf(buffer, size, "%s", enum_label(row->key, text));
      break;
    }
    case BD_SETTINGS_ROW_ACCENT: {
      const char *text =
          bd_config_get_string(settings->config, row->key, "persimmon");
      snprintf(buffer, size, "%s", accent_label(text));
      break;
    }
    default:
      snprintf(buffer, size, "%s", row->value);
      break;
  }
}

static bool adjust_slider(bd_settings *settings, bd_settings_row *row,
                          int direction) {
  long value = bd_settings_row_slider_value(settings, row);
  if (has_system_reader(settings, row->key)) {
    bd_config_value current;
    if (!read_system_value(settings, row->key, &current)) {
      set_error(settings, "%s不可用", row->label);
      return false;
    }
    value = current.integer;
  }
  long step = row->step > 0 ? row->step : 1;
  value += direction > 0 ? step : -step;
  if (value < row->min_value) {
    value = row->min_value;
  }
  if (value > row->max_value) {
    value = row->max_value;
  }
  if (value == bd_settings_row_slider_value(settings, row)) {
    return false;
  }
  if (row->key[0] == '!') {
    int channel = row->key[8] == 'r' ? 0 : (row->key[8] == 'g' ? 1 : 2);
    int old = row->index;
    row->index = (int)value;
    int r = channel == 0 ? (int)value : settings->custom_r;
    int g = channel == 1 ? (int)value : settings->custom_g;
    int b = channel == 2 ? (int)value : settings->custom_b;
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", r, g, b);
    if (!config_set(settings, "theme.accent", hex)) {
      row->index = old;
      bd_settings_mark_all(settings);
      bd_gfx_damage_all(settings->gfx);
      return false;
    }
    settings->custom_r = r;
    settings->custom_g = g;
    settings->custom_b = b;
    bd_settings_apply_theme(settings);
    bd_settings_mark_all(settings);
    bd_gfx_damage_all(settings->gfx);
    return true;
  }
  char text[32];
  snprintf(text, sizeof(text), "%ld", value);
  return set_config_value(settings, row, text);
}

static bool adjust_perf_profile(bd_settings *settings, bd_settings_row *row,
                               int direction) {
  if (row->enum_values == NULL || row->enum_count <= 0) {
    return false;
  }
  if (settings->perf_apply.apply_global_profile == NULL) {
    set_error(settings, "perf 服务不可用");
    return false;
  }
  const char *current =
      bd_config_get_string(settings->config, row->key, NULL);
  int index = -1;
  for (int i = 0; i < row->enum_count; ++i) {
    if (current != NULL && strcmp(row->enum_values[i], current) == 0) {
      index = i;
      break;
    }
  }
  int next;
  if (index < 0) {
    next = direction > 0 ? 0 : row->enum_count - 1;
  } else {
    next = (index + direction + row->enum_count) % row->enum_count;
  }
  const char *profile = row->enum_values[next];
  char error[128] = {0};
  if (!settings->perf_apply.apply_global_profile(
          settings->perf_apply.user, profile, error, sizeof(error))) {
    set_error(settings, "%s",
              error[0] != '\0' ? error : "perf 未确认应用该档位");
    return false;
  }
  return set_config_value(settings, row, profile);
}

static bool adjust_enum(bd_settings *settings, bd_settings_row *row,
                        int direction) {
  if (strcmp(row->key, "perf.global_profile") == 0) {
    return adjust_perf_profile(settings, row, direction);
  }
  if (row->enum_values == NULL || row->enum_count <= 0) {
    return false;
  }
  const char *current = row->key[0] == '!'
                            ? NULL
                            : bd_config_get_string(settings->config, row->key,
                                                   NULL);
  int index = -1;
  for (int i = 0; i < row->enum_count; ++i) {
    if (current != NULL && strcmp(row->enum_values[i], current) == 0) {
      index = i;
      break;
    }
  }
  int next;
  if (index < 0) {
    next = direction > 0 ? 0 : row->enum_count - 1;
  } else {
    next = (index + direction + row->enum_count) % row->enum_count;
  }
  return set_config_value(settings, row, row->enum_values[next]);
}

static bool adjust_accent(bd_settings *settings, bd_settings_row *row,
                          int direction) {
  const char *current = bd_config_get_string(settings->config, "theme.accent",
                                             "persimmon");
  int index = -1;
  for (int i = 0; i < row->enum_count; ++i) {
    if (strcmp(row->enum_values[i], current) == 0) {
      index = i;
      break;
    }
  }
  int next;
  if (index < 0) {
    next = direction > 0 ? 0 : row->enum_count - 1;
  } else {
    next = (index + direction + row->enum_count) % row->enum_count;
  }
  return set_config_value(settings, row, row->enum_values[next]);
}

static bool adjust_toggle(bd_settings *settings, bd_settings_row *row) {
  bool value = bd_config_get_bool(settings->config, row->key, false);
  if (has_system_reader(settings, row->key)) {
    bd_config_value current;
    if (!read_system_value(settings, row->key, &current)) {
      set_error(settings, "%s不可用", row->label);
      return false;
    }
    value = current.boolean;
  }
  return set_config_value(settings, row, value ? "false" : "true");
}

/* -------------------------------------------------------------- subpages */

static void close_plugin_settings(bd_settings *settings) {
  if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN &&
      settings->plugin_index >= 0 &&
      settings->plugin_index < settings->plugin_count) {
    bd_plugin_set_settings_open(settings->plugins[settings->plugin_index], false);
  }
}

static void load_custom_rgb(bd_settings *settings) {
  const char *accent = bd_config_get_string(settings->config, "theme.accent",
                                            "persimmon");
  const char *hex = bd_config_accent_hex(accent, true);
  bd_color color;
  if (hex == NULL || !bd_color_from_hex(hex, &color)) {
    color = settings->style.accent_base;
  }
  settings->custom_r = (int)(color.r * 255.0f + 0.5f);
  settings->custom_g = (int)(color.g * 255.0f + 0.5f);
  settings->custom_b = (int)(color.b * 255.0f + 0.5f);
}

static void open_accent_subpage(bd_settings *settings) {
  load_custom_rgb(settings);
  settings->subpage = BD_SETTINGS_SUBPAGE_ACCENT;
  settings->focus = 0;
  settings->scroll = 0;
  bd_settings_rebuild_rows(settings);
  bd_settings_mark_all(settings);
  bd_gfx_damage_all(settings->gfx);
}

static int collect_plugin_nodes(const bd_plugin_ui *ui, int node, int depth,
                                int *out, int count, int capacity) {
  if (node < 0 || node >= ui->node_count || depth > BD_PLUGIN_UI_MAX_DEPTH) {
    return count;
  }
  const bd_plugin_ui_node *item = &ui->nodes[node];
  if (item->type == BD_PLUGIN_UI_TOGGLE || item->type == BD_PLUGIN_UI_SLIDER ||
      item->type == BD_PLUGIN_UI_SELECT || item->type == BD_PLUGIN_UI_BUTTON) {
    if (count < capacity) {
      out[count++] = node;
    }
  }
  for (int i = 0; i < item->child_count; ++i) {
    count = collect_plugin_nodes(ui, item->children[i], depth + 1, out, count,
                                 capacity);
  }
  return count;
}

static void rebuild_plugin_nodes(bd_settings *settings) {
  settings->plugin_node_count = 0;
  settings->plugin_node_focus = 0;
  settings->scroll = 0;
  if (settings->plugin_index < 0 ||
      settings->plugin_index >= settings->plugin_count) {
    return;
  }
  bd_plugin *plugin = settings->plugins[settings->plugin_index];
  const bd_plugin_ui *ui = bd_plugin_ui_settings(plugin);
  if (ui == NULL || ui->root < 0) {
    return;
  }
  settings->plugin_node_count =
      collect_plugin_nodes(ui, ui->root, 0, settings->plugin_nodes,
                           settings->plugin_node_count,
                           BD_SETTINGS_MAX_PLUGIN_NODES);
}

static void open_plugin_subpage(bd_settings *settings, int index) {
  if (index < 0 || index >= settings->plugin_count) {
    return;
  }
  settings->subpage = BD_SETTINGS_SUBPAGE_PLUGIN;
  settings->plugin_index = index;
  settings->focus = 0;
  settings->scroll = 0;
  bd_plugin *plugin = settings->plugins[index];
  bd_plugin_activate(plugin);
  bd_plugin_set_settings_open(plugin, true);
  rebuild_plugin_nodes(settings);
  bd_settings_mark_all(settings);
  bd_gfx_damage_all(settings->gfx);
}

static void leave_subpage(bd_settings *settings) {
  if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN) {
    close_plugin_settings(settings);
  }
  settings->subpage = BD_SETTINGS_SUBPAGE_NONE;
  settings->focus = 0;
  settings->scroll = 0;
  bd_settings_rebuild_rows(settings);
  for (int i = 0; i < settings->row_count; ++i) {
    if (strcmp(settings->rows[i].key, "theme.accent") == 0) {
      settings->focus = i;
      break;
    }
  }
  bd_settings_mark_all(settings);
  bd_gfx_damage_all(settings->gfx);
}

/* ----------------------------------------------------------------- reset */

static bool reset_value_text(const bd_settings *settings, const char *key,
                             char *out, size_t out_size) {
  bd_config_value value;
  if (!bd_config_get(settings->config, key, &value)) {
    return false;
  }
  if (value.type == BD_CONFIG_BOOL) {
    snprintf(out, out_size, "%s", value.boolean ? "true" : "false");
    return true;
  }
  if (value.type == BD_CONFIG_INT || value.type == BD_CONFIG_DURATION) {
    snprintf(out, out_size, "%ld", value.integer);
    return true;
  }
  if (value.string != NULL) {
    snprintf(out, out_size, "%s", value.string);
    return true;
  }
  return false;
}

static bool reset_service_key(bd_settings *settings, const char *key,
                              const char *default_value) {
  bool system_key = strcmp(key, "display.brightness") == 0 ||
                    strcmp(key, "audio.volume") == 0 ||
                    strcmp(key, "audio.mute") == 0;
  bool perf_key = strcmp(key, "perf.global_profile") == 0;
  char old_value[96] = {0};
  reset_value_text(settings, key, old_value, sizeof(old_value));

  if (system_key) {
    if (!apply_system_value(settings, key, default_value)) {
      return false;
    }
  } else if (perf_key) {
    if (settings->perf_apply.apply_global_profile == NULL) {
      set_error(settings, "perf 服务不可用");
      return false;
    }
    char error[128] = {0};
    if (!settings->perf_apply.apply_global_profile(
            settings->perf_apply.user, default_value, error, sizeof(error))) {
      set_error(settings, "%s",
                error[0] != '\0' ? error : "perf 未确认默认档位");
      return false;
    }
  }

  char error[128] = {0};
  if (!bd_config_set(settings->config, key, default_value, error,
                     sizeof(error))) {
    set_error(settings, "%s", error);
    if (system_key && old_value[0] != '\0') {
      (void)apply_system_value(settings, key, old_value);
    } else if (perf_key && old_value[0] != '\0' &&
               settings->perf_apply.apply_global_profile != NULL) {
      char ignored[96] = {0};
      (void)settings->perf_apply.apply_global_profile(
          settings->perf_apply.user, old_value, ignored, sizeof(ignored));
    }
    return false;
  }
  return true;
}

static void reset_defaults(bd_settings *settings) {
  static const char *const DYNAMIC_PREFIXES[] = {
      "hotkey.", "profile.", "app.", "core.", "device.",
  };
  clear_error(settings);
  int failed = 0;
  char failure[192] = {0};
  size_t count = bd_config_key_count(settings->config);
  for (size_t i = 0; i < count; ++i) {
    const char *key = bd_config_key(settings->config, i);
    if (key == NULL) {
      continue;
    }
    bool dynamic = false;
    for (size_t p = 0;
         p < sizeof(DYNAMIC_PREFIXES) / sizeof(DYNAMIC_PREFIXES[0]); ++p) {
      size_t length = strlen(DYNAMIC_PREFIXES[p]);
      if (strncmp(key, DYNAMIC_PREFIXES[p], length) == 0) {
        dynamic = true;
        break;
      }
    }
    if (dynamic) {
      continue;
    }
    bd_config_schema_info info;
    if (!bd_config_describe(settings->config, key, &info) ||
        info.default_value == NULL) {
      continue;
    }
    bool service_key = strcmp(key, "display.brightness") == 0 ||
                       strcmp(key, "audio.volume") == 0 ||
                       strcmp(key, "audio.mute") == 0 ||
                       strcmp(key, "perf.global_profile") == 0;
    bool ok = false;
    if (service_key) {
      ok = reset_service_key(settings, key, info.default_value);
      if (!ok && failure[0] == '\0') {
        copy_text(failure, sizeof(failure), settings->error);
      }
    } else {
      char error[128] = {0};
      ok = bd_config_set(settings->config, key, info.default_value, error,
                         sizeof(error));
      if (!ok && failure[0] == '\0') {
        copy_text(failure, sizeof(failure), error);
      }
    }
    if (!ok) {
      ++failed;
    }
  }
  char save_error[128] = {0};
  bool saved = bd_config_save(settings->config, save_error, sizeof(save_error));
  bd_settings_apply_theme(settings);
  settings->confirm_open = false;
  settings->focus = 0;
  settings->scroll = 0;
  bd_settings_rebuild(settings);
  bd_settings_mark_all(settings);
  bd_gfx_damage_all(settings->gfx);

  if (failed > 0) {
    size_t used = strlen(failure);
    if (failed > 1) {
      snprintf(failure + used, sizeof(failure) - used, "（另有 %d 项）",
               failed - 1);
      used = strlen(failure);
    }
    if (!saved) {
      snprintf(failure + used, sizeof(failure) - used, "；保存失败：%s",
               save_error);
    }
    set_error(settings, "恢复默认值部分失败：%s", failure);
  } else if (!saved) {
    set_error(settings, "恢复默认值后保存失败：%s", save_error);
  }
}

/* ------------------------------------------------------------- creation */

bd_settings *bd_settings_create(bd_gfx *gfx, bd_config *config,
                                const bd_settings_style *style) {
  if (gfx == NULL || config == NULL) {
    return NULL;
  }
  bd_settings *settings = (bd_settings *)calloc(1, sizeof(*settings));
  if (settings == NULL) {
    return NULL;
  }
  settings->gfx = gfx;
  settings->config = config;
  settings->style.accent_base =
      style != NULL ? style->accent_base : bd_color_rgb(224, 122, 95);
  settings->style.width =
      style != NULL && style->width > 0 ? style->width : bd_gfx_width(gfx);
  settings->style.height =
      style != NULL && style->height > 0 ? style->height : bd_gfx_height(gfx);
  settings->status.battery_percent = -1;
  settings->plugin_index = -1;
  if (!fonts_load(&settings->fonts, gfx)) {
    free(settings);
    return NULL;
  }
  settings->plugin_fonts.title = settings->fonts.title;
  settings->plugin_fonts.body = settings->fonts.body;
  settings->plugin_fonts.badge = settings->fonts.badge;
  settings->plugin_fonts.serif = settings->fonts.serif34;
  settings->plugin_fonts.latin = settings->fonts.body;
  settings->plugin_fonts.fallback_full = settings->fonts.fallback;
  bd_settings_apply_theme(settings);
  bd_settings_rebuild(settings);
  settings->full_dirty = true;
  return settings;
}

void bd_settings_destroy(bd_settings *settings) {
  if (settings == NULL) {
    return;
  }
  close_plugin_settings(settings);
  fonts_free(&settings->fonts);
  free(settings);
}

/* -------------------------------------------------------------- setters */

void bd_settings_set_system(bd_settings *settings,
                            const bd_settings_system *system) {
  if (settings == NULL) {
    return;
  }
  if (system != NULL) {
    settings->system = *system;
  } else {
    memset(&settings->system, 0, sizeof(settings->system));
  }
}

bool bd_settings_sync_system(bd_settings *settings, char *error,
                             size_t error_size) {
  if (settings == NULL) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "设置页不可用");
    }
    return false;
  }
  static const char *const KEYS[] = {"display.brightness", "audio.volume",
                                     "audio.mute"};
  bool ok = true;
  for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); ++i) {
    bd_config_value value;
    if (!bd_config_get(settings->config, KEYS[i], &value)) {
      continue;
    }
    char text[32];
    if (value.type == BD_CONFIG_BOOL) {
      snprintf(text, sizeof(text), "%s", value.boolean ? "true" : "false");
    } else {
      snprintf(text, sizeof(text), "%ld", value.integer);
    }
    if (!apply_system_value(settings, KEYS[i], text)) {
      if (ok && error != NULL && error_size > 0) {
        snprintf(error, error_size, "%s", settings->error);
      }
      ok = false;
    }
  }
  if (ok) {
    clear_error(settings);
  }
  return ok;
}

void bd_settings_set_perf(bd_settings *settings,
                          const bd_settings_perf *perf) {
  if (settings == NULL) {
    return;
  }
  if (perf != NULL) {
    settings->perf_apply = *perf;
  } else {
    memset(&settings->perf_apply, 0, sizeof(settings->perf_apply));
  }
}

void bd_settings_set_plugins(bd_settings *settings, bd_plugin_host *host) {
  if (settings == NULL) {
    return;
  }
  bool was_plugin_page = settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN;
  close_plugin_settings(settings);
  if (was_plugin_page) {
    settings->subpage = BD_SETTINGS_SUBPAGE_NONE;
    settings->plugin_index = -1;
  }
  settings->host = host;
  load_plugins(settings);
  if (was_plugin_page ||
      (settings->category == BD_SETTINGS_CATEGORY_PLUGINS &&
       settings->subpage == BD_SETTINGS_SUBPAGE_NONE)) {
    bd_settings_rebuild_rows(settings);
  }
  clamp_scroll(settings);
  bd_settings_mark_all(settings);
  bd_gfx_damage_all(settings->gfx);
}

void bd_settings_set_status(bd_settings *settings,
                            const bd_settings_status *status) {
  if (settings == NULL) {
    return;
  }
  if (status != NULL) {
  settings->status = *status;
  if (status->clock != settings->clock) {
      copy_text(settings->clock, sizeof(settings->clock), status->clock);
    }
  } else {
    memset(&settings->status, 0, sizeof(settings->status));
    settings->status.battery_percent = -1;
    settings->clock[0] = '\0';
  }
  bd_settings_mark_all(settings);
  bd_gfx_damage_all(settings->gfx);
}

void bd_settings_set_battery_history(bd_settings *settings,
                                     const bd_battery_history *history) {
  if (settings == NULL) {
    return;
  }
  settings->battery_history = history;
  if (settings->category == BD_SETTINGS_CATEGORY_SYSTEM) {
    bd_settings_rebuild_rows(settings);
    bd_settings_mark_all(settings);
    bd_gfx_damage_all(settings->gfx);
  }
}

void bd_settings_set_storage(bd_settings *settings,
                             const bd_settings_storage *storage) {
  if (settings == NULL) {
    return;
  }
  if (storage != NULL) {
    settings->storage = *storage;
  } else {
    memset(&settings->storage, 0, sizeof(settings->storage));
  }
  if (settings->category == BD_SETTINGS_CATEGORY_SYSTEM &&
      settings->subpage == BD_SETTINGS_SUBPAGE_NONE) {
    int focus = settings->focus;
    bd_settings_rebuild_rows(settings);
    settings->focus = focus < settings->row_count ? focus : 0;
  }
  bd_settings_mark_all(settings);
  bd_gfx_damage_all(settings->gfx);
}

/* ------------------------------------------------------------ open/close */

void bd_settings_refresh(bd_settings *settings) {
  if (settings == NULL) {
    return;
  }
  bd_settings_apply_theme(settings);
  if (settings->subpage == BD_SETTINGS_SUBPAGE_ACCENT) {
    load_custom_rgb(settings);
  }
  bd_settings_rebuild(settings);
  if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN) {
    rebuild_plugin_nodes(settings);
  }
  bd_settings_mark_all(settings);
  bd_gfx_damage_all(settings->gfx);
}

bool bd_settings_open(bd_settings *settings, int category) {
  if (settings == NULL || category < 0 ||
      category >= BD_SETTINGS_CATEGORY_COUNT) {
    return false;
  }
  if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN) {
    close_plugin_settings(settings);
  }
  bool was_open = settings->open;
  settings->open = true;
  settings->category = category;
  settings->subpage = BD_SETTINGS_SUBPAGE_NONE;
  settings->focus = 0;
  settings->scroll = 0;
  settings->confirm_open = false;
  settings->held = false;
  settings->next_repeat_ms = 0;
  clear_error(settings);
  bd_settings_rebuild(settings);
  if (!was_open) {
    settings->full_dirty = true;
  }
  bd_settings_mark_all(settings);
  bd_gfx_damage_all(settings->gfx);
  return true;
}

void bd_settings_close(bd_settings *settings) {
  if (settings == NULL || !settings->open) {
    return;
  }
  close_plugin_settings(settings);
  settings->open = false;
  settings->subpage = BD_SETTINGS_SUBPAGE_NONE;
  settings->confirm_open = false;
  settings->held = false;
  settings->next_repeat_ms = 0;
  bd_settings_mark_all(settings);
  bd_gfx_damage_all(settings->gfx);
}

bool bd_settings_is_open(const bd_settings *settings) {
  return settings != NULL && settings->open;
}

/* -------------------------------------------------------------- helpers */

void bd_settings_set_hint_visible(bd_settings *settings, bool visible) {
  if (settings == NULL || settings->hint_visible == visible) {
    return;
  }
  settings->hint_visible = visible;
  settings->hint_dirty = true;
  int top = settings->style.height - BD_SETTINGS_HINT_HEIGHT;
  bd_gfx_damage_rect(settings->gfx,
                     bd_rect_make(0, top, settings->style.width,
                                  BD_SETTINGS_HINT_HEIGHT));
}

bool bd_settings_hint_visible(const bd_settings *settings) {
  return settings != NULL && settings->hint_visible;
}

int bd_settings_category_index(const bd_settings *settings) {
  return settings != NULL ? settings->category : 0;
}

int bd_settings_focus(const bd_settings *settings) {
  if (settings == NULL) {
    return 0;
  }
  if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN) {
    return settings->plugin_node_focus;
  }
  return settings->focus;
}

int bd_settings_row_count(const bd_settings *settings) {
  return settings != NULL ? settings->row_count : 0;
}

const char *bd_settings_row_key(const bd_settings *settings, int index) {
  if (settings == NULL || index < 0 || index >= settings->row_count) {
    return NULL;
  }
  return settings->rows[index].key;
}

bool bd_settings_subpage(const bd_settings *settings) {
  return settings != NULL &&
         (settings->subpage != BD_SETTINGS_SUBPAGE_NONE ||
          settings->confirm_open);
}

bool bd_settings_confirm_open(const bd_settings *settings) {
  return settings != NULL && settings->confirm_open;
}

const char *bd_settings_last_error(const bd_settings *settings) {
  return settings != NULL ? settings->error : "";
}

bd_color bd_settings_accent(const bd_settings *settings) {
  if (settings == NULL) {
    return bd_color_rgb(224, 122, 95);
  }
  return settings->accent.accent;
}

bd_settings_stats bd_settings_get_stats(const bd_settings *settings) {
  bd_settings_stats stats;
  memset(&stats, 0, sizeof(stats));
  if (settings == NULL) {
    return stats;
  }
  stats.rows = settings->row_count;
  stats.hotkeys = (int)settings->hotkeys.count;
  stats.conflicts = settings->conflict_count;
  stats.plugins = settings->plugin_count;
  stats.plugin_nodes = settings->plugin_node_count;
  stats.scroll = settings->scroll;
  if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN) {
    if (settings->plugin_index >= 0 &&
        settings->plugin_index < settings->plugin_count &&
        settings->plugin_node_count > 0) {
      const bd_plugin_ui *ui =
          bd_plugin_ui_settings(settings->plugins[settings->plugin_index]);
      bd_rect node_rect;
      bd_rect table = bd_settings_table_rect(settings);
      int node = settings->plugin_nodes[settings->plugin_node_focus];
      if (ui != NULL &&
          bd_plugin_ui_node_rect(ui, node, table.w, &node_rect)) {
        int top = table.y + node_rect.y - settings->scroll;
        stats.focus_visible =
            top >= table.y && top + node_rect.h <= table.y + table.h;
      }
    }
  } else if (settings->focus >= 0 && settings->focus < settings->row_count) {
    bd_rect table = bd_settings_table_rect(settings);
    bd_rect row = bd_settings_row_rect(settings, settings->focus);
    stats.focus_visible =
        row.y >= table.y && row.y + row.h <= table.y + table.h;
  }
  stats.subpage = bd_settings_subpage(settings);
  stats.hint_visible = settings->hint_visible;
  stats.focus = bd_settings_focus(settings);
  stats.category = settings->category;
  return stats;
}

/* --------------------------------------------------------------- input */

static bool focus_move(bd_settings *settings, int direction) {
  if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN) {
    if (settings->plugin_node_count == 0) {
      return false;
    }
    int next = settings->plugin_node_focus + direction;
    if (next < 0) {
      next = 0;
    }
    if (next >= settings->plugin_node_count) {
      next = settings->plugin_node_count - 1;
    }
    if (next == settings->plugin_node_focus) {
      return false;
    }
    settings->plugin_node_focus = next;
    bd_rect table = bd_settings_table_rect(settings);
    if (settings->plugin_index >= 0 &&
        settings->plugin_index < settings->plugin_count) {
      const bd_plugin_ui *ui =
          bd_plugin_ui_settings(settings->plugins[settings->plugin_index]);
      bd_rect node_rect;
      int node = settings->plugin_nodes[next];
      if (ui != NULL &&
          bd_plugin_ui_node_rect(ui, node, table.w, &node_rect)) {
        int top = table.y + node_rect.y - settings->scroll;
        int bottom = top + node_rect.h;
        int bottom_limit = table.y + table.h - BD_SETTINGS_SCROLL_PAD;
        if (top < table.y) {
          settings->scroll -= table.y - top;
        } else if (bottom > bottom_limit) {
          settings->scroll += bottom - bottom_limit;
        }
        clamp_scroll(settings);
      }
    }
    settings->table_dirty = true;
    bd_gfx_damage_rect(settings->gfx, table);
    return true;
  }
  if (settings->row_count == 0) {
    return false;
  }
  int next = settings->focus + direction;
  if (next < 0) {
    next = 0;
  }
  if (next >= settings->row_count) {
    next = settings->row_count - 1;
  }
  if (next == settings->focus) {
    return false;
  }
  int old = settings->focus;
  settings->focus = next;
  int old_scroll = settings->scroll;
  bd_rect table = bd_settings_table_rect(settings);
  bd_rect row = bd_settings_row_rect(settings, next);
  if (row.y < table.y) {
    settings->scroll -= table.y - row.y;
  } else if (row.y + row.h > table.y + table.h) {
    settings->scroll += row.y + row.h - (table.y + table.h);
  }
  clamp_scroll(settings);
  if (settings->scroll != old_scroll) {
    bd_settings_mark_all(settings);
    bd_gfx_damage_rect(settings->gfx, table);
  } else {
    bd_settings_damage_row(settings, old);
    bd_settings_damage_row(settings, next);
  }
  return true;
}

static bool switch_category(bd_settings *settings, int direction) {
  if (settings->subpage != BD_SETTINGS_SUBPAGE_NONE ||
      settings->confirm_open) {
    return false;
  }
  int next = (settings->category + direction + BD_SETTINGS_CATEGORY_COUNT) %
             BD_SETTINGS_CATEGORY_COUNT;
  return bd_settings_open(settings, next);
}

static bool activate_row(bd_settings *settings, bd_settings_row *row) {
  switch (row->kind) {
    case BD_SETTINGS_ROW_TOGGLE:
      return adjust_toggle(settings, row);
    case BD_SETTINGS_ROW_ACCENT:
      open_accent_subpage(settings);
      return true;
    case BD_SETTINGS_ROW_ENUM:
      return adjust_enum(settings, row, 1);
    case BD_SETTINGS_ROW_PLUGIN:
      open_plugin_subpage(settings, row->index);
      return true;
    case BD_SETTINGS_ROW_RESET:
      settings->confirm_open = true;
      settings->confirm_focus = 0;
      bd_settings_mark_all(settings);
      bd_gfx_damage_all(settings->gfx);
      return true;
    case BD_SETTINGS_ROW_CONFLICT:
    case BD_SETTINGS_ROW_INFO:
    case BD_SETTINGS_ROW_SLIDER:
    default:
      return false;
  }
}

static bool plugin_activate(bd_settings *settings) {
  if (settings->plugin_index < 0 ||
      settings->plugin_index >= settings->plugin_count ||
      settings->plugin_node_count == 0) {
    return false;
  }
  bd_plugin *plugin = settings->plugins[settings->plugin_index];
  const bd_plugin_ui *ui = bd_plugin_ui_settings(plugin);
  if (ui == NULL) {
    return false;
  }
  int node = settings->plugin_nodes[settings->plugin_node_focus];
  if (node < 0 || node >= ui->node_count) {
    return false;
  }
  const char *id = ui->nodes[node].id;
  if (id[0] == '\0') {
    return false;
  }
  return bd_plugin_activate_node(plugin, id, "activate");
}

bool bd_settings_input(bd_settings *settings, bd_settings_action action) {
  if (settings == NULL || !settings->open) {
    return false;
  }
  if (settings->confirm_open) {
    if (action == BD_SETTINGS_ACTION_LEFT ||
        action == BD_SETTINGS_ACTION_RIGHT) {
      settings->confirm_focus = settings->confirm_focus == 0 ? 1 : 0;
      bd_settings_mark_all(settings);
      bd_gfx_damage_all(settings->gfx);
      return true;
    }
    if (action == BD_SETTINGS_ACTION_ACCEPT) {
      if (settings->confirm_focus == 1) {
        reset_defaults(settings);
      } else {
        settings->confirm_open = false;
        bd_settings_mark_all(settings);
        bd_gfx_damage_all(settings->gfx);
      }
      return true;
    }
    if (action == BD_SETTINGS_ACTION_CANCEL) {
      settings->confirm_open = false;
      bd_settings_mark_all(settings);
      bd_gfx_damage_all(settings->gfx);
      return true;
    }
    return false;
  }
  switch (action) {
    case BD_SETTINGS_ACTION_UP:
      return focus_move(settings, -1);
    case BD_SETTINGS_ACTION_DOWN:
      return focus_move(settings, 1);
    case BD_SETTINGS_ACTION_LEFT:
    case BD_SETTINGS_ACTION_RIGHT:
      if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN) {
        return false;
      }
      if (settings->subpage == BD_SETTINGS_SUBPAGE_NONE &&
          (settings->focus < 0 || settings->focus >= settings->row_count)) {
        return false;
      }
      if (settings->subpage == BD_SETTINGS_SUBPAGE_NONE) {
        bd_settings_row *row = &settings->rows[settings->focus];
        int direction = action == BD_SETTINGS_ACTION_RIGHT ? 1 : -1;
        switch (row->kind) {
          case BD_SETTINGS_ROW_TOGGLE:
            return adjust_toggle(settings, row);
          case BD_SETTINGS_ROW_SLIDER:
            return adjust_slider(settings, row, direction);
          case BD_SETTINGS_ROW_ENUM:
            return adjust_enum(settings, row, direction);
          case BD_SETTINGS_ROW_ACCENT:
            return adjust_accent(settings, row, direction);
          default:
            return false;
        }
      }
      {
        bd_settings_row *row = &settings->rows[settings->focus];
        if (row->kind == BD_SETTINGS_ROW_SLIDER) {
          return adjust_slider(settings, row,
                               action == BD_SETTINGS_ACTION_RIGHT ? 1 : -1);
        }
      }
      return false;
    case BD_SETTINGS_ACTION_ACCEPT:
      if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN) {
        return plugin_activate(settings);
      }
      if (settings->subpage == BD_SETTINGS_SUBPAGE_ACCENT) {
        return false;
      }
      if (settings->focus >= 0 && settings->focus < settings->row_count) {
        return activate_row(settings, &settings->rows[settings->focus]);
      }
      return false;
    case BD_SETTINGS_ACTION_CANCEL:
      if (settings->subpage != BD_SETTINGS_SUBPAGE_NONE) {
        leave_subpage(settings);
        return true;
      }
      bd_settings_close(settings);
      return true;
    case BD_SETTINGS_ACTION_CATEGORY_PREV:
      return switch_category(settings, -1);
    case BD_SETTINGS_ACTION_CATEGORY_NEXT:
      return switch_category(settings, 1);
    default:
      return false;
  }
}

void bd_settings_hold(bd_settings *settings, bd_settings_action action,
                      bool pressed) {
  if (settings == NULL || !settings->open) {
    return;
  }
  if (!pressed) {
    if (settings->held && settings->held_action == action) {
      settings->held = false;
      settings->next_repeat_ms = 0;
    }
    return;
  }
  if (action != BD_SETTINGS_ACTION_LEFT &&
      action != BD_SETTINGS_ACTION_RIGHT) {
    return;
  }
  if (settings->held && settings->held_action == action) {
    return;
  }
  settings->held = false;
  settings->next_repeat_ms = 0;
  if (!bd_settings_input(settings, action)) {
    return;
  }
  bool repeatable = false;
  if (settings->subpage == BD_SETTINGS_SUBPAGE_ACCENT) {
    repeatable = settings->row_count > 0 &&
                 settings->rows[settings->focus].kind == BD_SETTINGS_ROW_SLIDER;
  } else if (settings->subpage == BD_SETTINGS_SUBPAGE_NONE &&
             !settings->confirm_open && settings->focus >= 0 &&
             settings->focus < settings->row_count) {
    repeatable =
        settings->rows[settings->focus].kind == BD_SETTINGS_ROW_SLIDER;
  }
  if (!repeatable) {
    return;
  }
  int delay = (int)bd_config_get_int(settings->config, "input.repeat_delay_ms",
                                     400);
  settings->held = true;
  settings->held_action = action;
  settings->next_repeat_ms = settings->now_ms + (uint64_t)delay;
}

void bd_settings_update(bd_settings *settings, uint64_t now_ms) {
  if (settings == NULL) {
    return;
  }
  settings->now_ms = now_ms;
  settings->has_time = true;
  if (!settings->held || settings->next_repeat_ms == 0) {
    return;
  }
  int interval = (int)bd_config_get_int(settings->config,
                                        "input.repeat_interval_ms", 80);
  if (interval < 10) {
    interval = 10;
  }
  int fired = 0;
  while (now_ms >= settings->next_repeat_ms && fired < 8) {
    if (!bd_settings_input(settings, settings->held_action)) {
      settings->held = false;
      settings->next_repeat_ms = 0;
      return;
    }
    settings->next_repeat_ms += (uint64_t)interval;
    ++fired;
  }
  if (settings->held && now_ms >= settings->next_repeat_ms) {
    settings->next_repeat_ms = now_ms + (uint64_t)interval;
  }
}

bool bd_settings_needs_frame_until(const bd_settings *settings,
                                   uint64_t *until_ms) {
  if (settings == NULL || !settings->held || settings->next_repeat_ms == 0) {
    return false;
  }
  if (until_ms != NULL) {
    *until_ms = settings->next_repeat_ms;
  }
  return true;
}

/* -------------------------------------------------------------- geometry */

bd_rect bd_settings_table_rect(const bd_settings *settings) {
  int bottom = BD_SETTINGS_TABLE_BOTTOM;
  int limit = settings->style.height - 108;
  if (bottom > limit) {
    bottom = limit;
  }
  int max_height = bottom - BD_SETTINGS_TABLE_Y;
  if (max_height < 60) {
    max_height = 60;
  }
  int content = 0;
  for (int i = 0; i < settings->row_count; ++i) {
    content += row_height(&settings->rows[i]);
  }
  int height = max_height;
  if (settings->subpage == BD_SETTINGS_SUBPAGE_PLUGIN) {
    int measured = plugin_tree_height(settings);
    if (measured > 0) {
      height = measured + 2;
      if (height > max_height) {
        height = max_height;
      }
      if (height < 120) {
        height = 120;
      }
    }
  } else if (settings->row_count > 0 && content < max_height) {
    height = content;
  }
  return bd_rect_make(BD_SETTINGS_TABLE_X, BD_SETTINGS_TABLE_Y,
                      BD_SETTINGS_TABLE_W, height);
}

bd_rect bd_settings_row_rect(const bd_settings *settings, int index) {
  bd_rect table = bd_settings_table_rect(settings);
  int y = table.y - settings->scroll;
  for (int i = 0; i < index && i < settings->row_count; ++i) {
    y += row_height(&settings->rows[i]);
  }
  int height = index >= 0 && index < settings->row_count
                   ? row_height(&settings->rows[index])
                   : BD_SETTINGS_ROW_MIN;
  return bd_rect_make(table.x, y, table.w, height);
}
