#ifndef BRICKDESK_HOME_INTERNAL_H
#define BRICKDESK_HOME_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "brickdesk/gfx.h"
#include "brickdesk/home.h"
#include "brickdesk/theme.h"

#define BD_HOME_ROWS 4
#define BD_HOME_CELL 188
#define BD_HOME_GAP 5
#define BD_HOME_PITCH (BD_HOME_CELL + BD_HOME_GAP)
#define BD_HOME_WALL_HEIGHT (BD_HOME_ROWS * BD_HOME_CELL + (BD_HOME_ROWS - 1) * BD_HOME_GAP)
#define BD_HOME_TOP_BAR 68
#define BD_HOME_TOP_STRIP 92
#define BD_HOME_HINT_BAR 58
#define BD_HOME_HINT_STRIP 88
#define BD_HOME_PAN_MS 220
#define BD_HOME_LAUNCH_MS 420
#define BD_HOME_PANEL_MS 200
#define BD_HOME_DIALOG_MS 180
#define BD_HOME_CLOSE_MS 180
#define BD_HOME_PREFETCH 1024
#define BD_HOME_MAX_RUNNING 8
#define BD_HOME_MAX_QUERY BD_HOME_QUERY_MAX
#define BD_HOME_ID_MAX 65
#define BD_HOME_PATH_MAX 512
#define BD_HOME_TITLE_MAX 256

typedef struct bd_home_palette {
  bd_color bg;
  bd_color bg_deep;
  bd_color surface;
  bd_color surface2;
  bd_color surface3;
  bd_color ink;
  bd_color ink2;
  bd_color ink3;
  bd_color line;
  bd_color line_strong;
  bd_color chrome_strong;
  bd_color chrome_base;
  bd_color scrim;
  bd_color tile_shade_a;
  bd_color tile_shade_b;
  bd_color cover_ink;
  bd_color cover_ink2;
  bd_color focus_ring;
} bd_home_palette;

typedef struct bd_home_fonts {
  bd_font *title31;
  bd_font *title25;
  bd_font *title22;
  bd_font *title18;
  bd_font *tab18;
  bd_font *body16;
  bd_font *body15;
  bd_font *body14;
  bd_font *badge13;
  bd_font *serif25;
  bd_font *serif22;
  bd_font *serif17;
  bd_font *fallback;
} bd_home_fonts;

typedef struct bd_home_entry {
  bd_home_item item;
  char *id;
  char *title;
  char *subtitle;
  char *image;
  int col;
  int row;
  int w;
  int h;
  bd_rect rect;              /* wall coordinates */
  bd_texture *texture;       /* cover from the async cache */
  uint32_t stamp;
  bool running;
} bd_home_entry;

typedef struct bd_home_index_node {
  int item;
  int next;
} bd_home_index_node;

typedef struct bd_home_running {
  char id[BD_HOME_ID_MAX];
  char frame_path[BD_HOME_PATH_MAX];
  bd_texture *frame;
  int item_index;
  int memory_mb;
  bool paused;
  uint64_t since_ms;
} bd_home_running;

typedef struct bd_home_pan {
  bool active;
  uint64_t start_ms;
  int from;
  int to;
} bd_home_pan;

typedef enum bd_home_launch_mode {
  BD_HOME_LAUNCH_NONE = 0,
  BD_HOME_LAUNCH_OPEN,
  BD_HOME_LAUNCH_RETURN
} bd_home_launch_mode;

struct bd_home {
  bd_gfx *gfx;
  int width;
  int height;
  bd_home_style style;
  bd_home_palette palette;
  bd_accent_palette accent;
  bd_home_fonts fonts;

  bd_home_entry *entries;
  int count;
  int *col_head;
  int cols;
  bd_home_index_node *nodes;
  int node_count;
  int wall_width;
  int free_cells;

  int focus;
  int nav_row;
  int pan;
  int tab;
  bd_home_pan pan_anim;

  bd_home_status status;
  char clock[16];
  bd_home_live live;
  char live_title[128];
  char live_artist[128];
  char live_previous[256];
  char live_current[256];
  char live_next[256];

  bd_home_running running[BD_HOME_MAX_RUNNING];
  int running_count;
  uint64_t now_ms;

  bool quick_open;
  bool quick_anim;
  bool quick_target;
  uint64_t quick_start;
  float quick_from;
  int quick_last_x;
  int quick_sel;

  bool hint_visible;
  bool hint_anim;
  bool hint_target;
  uint64_t hint_start;
  float hint_from;

  bool dialog_open;
  bool dialog_visible;
  bool dialog_anim;
  bool dialog_target;
  uint64_t dialog_start;
  float dialog_from;
  int dialog_item;
  int dialog_focus;
  int dialog_kind;
  int dialog_result;
  int dialog_max_background;
  char dialog_pending_id[BD_HOME_ID_MAX];
  char dialog_victim_id[BD_HOME_ID_MAX];

  bd_home_launch_mode launch;
  int launch_item;
  uint64_t launch_start;
  bd_rect launch_from;
  bool launch_complete;
  bool launch_painted;
  bool launch_app_painted;

  bool closing_active;
  int closing_item;
  uint64_t closing_start;

  int brightness;
  int volume;
  bool wifi;
  bool bluetooth;

  uint32_t stamp;
  bd_home_stats stats;
  int query_visited;
  int query_buckets;
};

void bd_home_palette_apply(bd_home *home);

void bd_home_draw_all(bd_home *home, bd_gfx *gfx);

bd_rect bd_home_screen_rect(const bd_home *home, int index);
bool bd_home_item_visible(const bd_home *home, int index);
int bd_home_running_index(const bd_home *home, const char *id);
bool bd_home_item_matches_tab(const bd_home *home, int index);
int bd_home_query_indexed(bd_home *home, bd_rect rect, int *out, int capacity,
                          int *visited_out, int *buckets_out);

uint64_t bd_home_pan_end(const bd_home *home);
uint64_t bd_home_launch_end(const bd_home *home);
uint64_t bd_home_quick_end(const bd_home *home);
uint64_t bd_home_hint_end(const bd_home *home);
uint64_t bd_home_dialog_end(const bd_home *home);
uint64_t bd_home_closing_end(const bd_home *home);

float bd_home_quick_progress(const bd_home *home);
float bd_home_hint_progress(const bd_home *home);
float bd_home_dialog_progress(const bd_home *home);
bd_rect bd_home_quick_panel_rect(const bd_home *home);
bd_rect bd_home_launch_rect(const bd_home *home);

void bd_home_damage_rect(bd_home *home, bd_rect rect);
void bd_home_damage_item(bd_home *home, int index);
void bd_home_damage_all(bd_home *home);

float bd_home_ease_wall(float t);
float bd_home_ease_out(float t);
float bd_home_ease_standard(float t);
float bd_home_lerp(float a, float b, float t);
int bd_home_lerp_int(int a, int b, float t);
float bd_home_clampf(float value, float minimum, float maximum);

#endif /* BRICKDESK_HOME_INTERNAL_H */
