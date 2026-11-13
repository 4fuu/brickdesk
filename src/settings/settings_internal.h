#ifndef BRICKDESK_SETTINGS_INTERNAL_H
#define BRICKDESK_SETTINGS_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "brickdesk/config.h"
#include "brickdesk/gfx.h"
#include "brickdesk/input.h"
#include "brickdesk/perf.h"
#include "brickdesk/plugin.h"
#include "brickdesk/settings.h"

#define BD_SETTINGS_TOP_BAR 68
#define BD_SETTINGS_TABLE_X 248
#define BD_SETTINGS_TABLE_Y 170
#define BD_SETTINGS_TABLE_W 748
#define BD_SETTINGS_TABLE_BOTTOM 660
#define BD_SETTINGS_TABLE_RADIUS 0
#define BD_SETTINGS_ROW_PAD 22
#define BD_SETTINGS_HINT_HEIGHT 58
#define BD_SETTINGS_CATEGORY_X 28
#define BD_SETTINGS_CATEGORY_TOP 92
#define BD_SETTINGS_CATEGORY_PITCH 43
#define BD_SETTINGS_MAX_ROWS 96
#define BD_SETTINGS_MAX_SCOPES 24
#define BD_SETTINGS_MAX_PLUGINS BD_PLUGIN_MAX_PLUGINS
#define BD_SETTINGS_MAX_PLUGIN_NODES 32
#define BD_SETTINGS_ROW_MIN 62
#define BD_SETTINGS_ROW_TALL 74
#define BD_SETTINGS_SCROLL_PAD 12

typedef struct bd_settings_palette {
  bd_color bg;
  bd_color surface;
  bd_color surface2;
  bd_color surface3;
  bd_color ink;
  bd_color ink2;
  bd_color ink3;
  bd_color line;
  bd_color line_strong;
  bd_color hint;
  bd_color scrim;
} bd_settings_palette;

typedef struct bd_settings_fonts {
  bd_font *serif34;
  bd_font *serif25;
  bd_font *title;
  bd_font *value;
  bd_font *body;
  bd_font *badge;
  bd_font *caption;
  bd_font *fallback;
} bd_settings_fonts;

typedef enum bd_settings_row_kind {
  BD_SETTINGS_ROW_TOGGLE = 0,
  BD_SETTINGS_ROW_SLIDER,
  BD_SETTINGS_ROW_ENUM,
  BD_SETTINGS_ROW_ACCENT,
  BD_SETTINGS_ROW_ACTION,
  BD_SETTINGS_ROW_INFO,
  BD_SETTINGS_ROW_CONFLICT,
  BD_SETTINGS_ROW_PLUGIN,
  BD_SETTINGS_ROW_HISTORY,
  BD_SETTINGS_ROW_RESET
} bd_settings_row_kind;

typedef struct bd_settings_row {
  bd_settings_row_kind kind;
  char key[160];
  char label[96];
  char description[224];
  char value[96];
  char unit[8];
  bd_config_type type;
  long min_value;
  long max_value;
  long step;
  const char *const *enum_values;
  int enum_count;
  int index; /* plugin index, hotkey index or conflict index */
  bool warning;
  bool bullet_slot;
} bd_settings_row;

typedef struct bd_settings_page_def {
  const char *title;
  const char *subtitle;
  const char *hint;
  const char *const *preferred; /* ordered keys first */
  const char *const *sections;  /* extra schema keys are appended */
  const char *const *skip;      /* keys not shown on this page */
} bd_settings_page_def;

typedef struct bd_settings_label {
  const char *key;
  const char *label;
  const char *description;
  const char *unit;
  long step;
} bd_settings_label;

extern const struct bd_settings_page_def BD_SETTINGS_PAGES[];

typedef enum bd_settings_subpage_kind {
  BD_SETTINGS_SUBPAGE_NONE = 0,
  BD_SETTINGS_SUBPAGE_ACCENT,
  BD_SETTINGS_SUBPAGE_PLUGIN,
  BD_SETTINGS_SUBPAGE_HOTKEY
} bd_settings_subpage_kind;

struct bd_settings {
  bd_gfx *gfx;
  bd_config *config;
  bd_settings_style style;
  bd_settings_palette palette;
  bd_accent_palette accent;
  bd_settings_fonts fonts;
  bd_ui_fonts plugin_fonts;

  bd_settings_system system;
  bd_settings_perf perf_apply;
  bd_plugin_host *host;
  const bd_battery_history *battery_history;
  bd_settings_status status;
  char clock[16];
  bd_settings_storage storage;

  bool open;
  int category;
  int focus;
  int scroll;
  bd_settings_subpage_kind subpage;
  int plugin_index;
  int plugin_node_focus;
  int plugin_node_count;
  int plugin_nodes[BD_SETTINGS_MAX_PLUGIN_NODES];
  int custom_r;
  int custom_g;
  int custom_b;

  bd_settings_row rows[BD_SETTINGS_MAX_ROWS];
  int row_count;

  bd_hotkey_table hotkeys;
  bool hotkeys_loaded;
  int conflict_count;
  char edit_hotkey[BD_HOTKEY_ID_MAX];
  bool capture_active;
  bool capture_wait_release;
  bd_key capture_keys[BD_HOTKEY_MAX_KEYS];
  bool capture_pressed[BD_KEY_COUNT];
  int capture_count;
  char scope_options[BD_SETTINGS_MAX_SCOPES][BD_HOTKEY_SCOPE_MAX];
  const char *scope_option_ptrs[BD_SETTINGS_MAX_SCOPES];
  int scope_option_count;
  char scope_app_ids[BD_SETTINGS_MAX_SCOPES][96];
  int scope_app_count;

  bd_perf_policy perf;
  bool perf_loaded;
  const char *profile_names[BD_PERF_MAX_PROFILES + 1];
  int profile_count;

  bd_plugin *plugins[BD_SETTINGS_MAX_PLUGINS];
  int plugin_count;

  bool hint_visible;
  bool confirm_open;
  int confirm_focus;

  bool held;
  bd_settings_action held_action;
  uint64_t next_repeat_ms;
  bool has_time;
  uint64_t now_ms;

  char error[192];

  bool full_dirty;
  bool table_dirty;
  bool hint_dirty;
  uint64_t dirty_rows[2];
};

void bd_settings_apply_theme(bd_settings *settings);
void bd_settings_palette_apply(bd_settings *settings);
bool bd_settings_rebuild(bd_settings *settings);
bool bd_settings_rebuild_rows(bd_settings *settings);
void bd_settings_mark_row(bd_settings *settings, int index);
void bd_settings_mark_all(bd_settings *settings);
void bd_settings_damage_row(bd_settings *settings, int index);
bd_rect bd_settings_table_rect(const bd_settings *settings);
bd_rect bd_settings_row_rect(const bd_settings *settings, int index);
int bd_settings_row_at_point(const bd_settings *settings, int y);
int bd_settings_visible_rows(const bd_settings *settings);
int bd_settings_max_scroll(const bd_settings *settings);

void bd_settings_draw_all(bd_settings *settings, bd_gfx *gfx);
void bd_settings_draw_hint(bd_settings *settings, bd_gfx *gfx);
void bd_settings_draw_confirm(bd_settings *settings, bd_gfx *gfx);

/* Value text for a built row; also used by the test harness. */
void bd_settings_row_value(const bd_settings *settings,
                           const bd_settings_row *row, char *buffer,
                           size_t size);
long bd_settings_row_slider_value(const bd_settings *settings,
                                  const bd_settings_row *row);

#endif /* BRICKDESK_SETTINGS_INTERNAL_H */
