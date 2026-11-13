#define _POSIX_C_SOURCE 200809L

#include "brickdesk/keyboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BD_KEYBOARD_ROWS 5
#define BD_KEYBOARD_COLS 16
#define BD_KEYBOARD_TITLE_MAX 64

typedef enum bd_keyboard_action {
  BD_KEYBOARD_KEY_TEXT = 0,
  BD_KEYBOARD_KEY_SHIFT,
  BD_KEYBOARD_KEY_SYMBOLS,
  BD_KEYBOARD_KEY_BACKSPACE,
  BD_KEYBOARD_KEY_SPACE,
  BD_KEYBOARD_KEY_CANCEL,
  BD_KEYBOARD_KEY_DONE
} bd_keyboard_action;

typedef struct bd_keyboard_key {
  char label[12];
  char text[8];
  bd_keyboard_action action;
  int weight;
} bd_keyboard_key;

typedef enum bd_keyboard_mode {
  BD_KEYBOARD_MODE_LOWER = 0,
  BD_KEYBOARD_MODE_UPPER,
  BD_KEYBOARD_MODE_SYMBOLS
} bd_keyboard_mode;

struct bd_keyboard {
  bool open;
  char title[BD_KEYBOARD_TITLE_MAX];
  char text[BD_KEYBOARD_TEXT_MAX + 1];
  size_t max_length;
  bool mask;
  bd_keyboard_done_fn on_done;
  void *user;

  bd_keyboard_mode mode;
  int row;
  int col;
  int row_count;
  bd_keyboard_key keys[BD_KEYBOARD_ROWS][BD_KEYBOARD_COLS];
  int key_count[BD_KEYBOARD_ROWS];

  int pressed_row;
  int pressed_col;
  uint64_t pressed_until;
};

static void set_key(bd_keyboard_key *key, const char *label, const char *text,
                    bd_keyboard_action action, int weight) {
  snprintf(key->label, sizeof(key->label), "%s", label);
  snprintf(key->text, sizeof(key->text), "%s", text != NULL ? text : "");
  key->action = action;
  key->weight = weight;
}

static void append_char(bd_keyboard *keyboard, const char *text) {
  size_t length = strlen(keyboard->text);
  size_t add = strlen(text);
  if (length + add > keyboard->max_length) {
    return;
  }
  memcpy(keyboard->text + length, text, add + 1);
}

static void build_rows(bd_keyboard *keyboard) {
  static const char *const LOWER[] = {"1234567890", "qwertyuiop", "asdfghjkl"};
  static const char *const UPPER[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL"};
  static const char *const SYMBOLS[] = {"!@#$%^&*()", "-_=+[]{};:",
                                        "'\",.<>/?\\|`~"};
  const char *const *rows = LOWER;
  if (keyboard->mode == BD_KEYBOARD_MODE_UPPER) {
    rows = UPPER;
  } else if (keyboard->mode == BD_KEYBOARD_MODE_SYMBOLS) {
    rows = SYMBOLS;
  }
  memset(keyboard->keys, 0, sizeof(keyboard->keys));
  memset(keyboard->key_count, 0, sizeof(keyboard->key_count));
  for (int r = 0; r < 3; ++r) {
    const char *text = rows[r];
    int count = 0;
    for (const char *cursor = text; *cursor != '\0' && count < BD_KEYBOARD_COLS;
         ++cursor) {
      char label[2] = {*cursor, '\0'};
      char value[2] = {*cursor, '\0'};
      set_key(&keyboard->keys[r][count], label, value, BD_KEYBOARD_KEY_TEXT,
              10);
      ++count;
    }
    keyboard->key_count[r] = count;
  }

  int count = 0;
  if (keyboard->mode == BD_KEYBOARD_MODE_SYMBOLS) {
    set_key(&keyboard->keys[3][count++], "ABC", "", BD_KEYBOARD_KEY_SHIFT,
            16);
  } else {
    set_key(&keyboard->keys[3][count++], "大写", "",
            BD_KEYBOARD_KEY_SHIFT, 16);
  }
  set_key(&keyboard->keys[3][count++], "空格", " ", BD_KEYBOARD_KEY_SPACE,
          44);
  set_key(&keyboard->keys[3][count++], "删除", "",
          BD_KEYBOARD_KEY_BACKSPACE, 16);
  if (keyboard->mode == BD_KEYBOARD_MODE_SYMBOLS) {
    set_key(&keyboard->keys[3][count++], "字母", "",
            BD_KEYBOARD_KEY_SYMBOLS, 14);
  } else {
    set_key(&keyboard->keys[3][count++], "符号", "",
            BD_KEYBOARD_KEY_SYMBOLS, 14);
  }
  keyboard->key_count[3] = count;

  count = 0;
  set_key(&keyboard->keys[4][count++], "取消", "", BD_KEYBOARD_KEY_CANCEL,
          20);
  set_key(&keyboard->keys[4][count++], "完成", "", BD_KEYBOARD_KEY_DONE, 20);
  keyboard->key_count[4] = count;
  keyboard->row_count = BD_KEYBOARD_ROWS;
  if (keyboard->row >= keyboard->row_count) {
    keyboard->row = keyboard->row_count - 1;
  }
  if (keyboard->col >= keyboard->key_count[keyboard->row]) {
    keyboard->col = keyboard->key_count[keyboard->row] - 1;
  }
  if (keyboard->col < 0) {
    keyboard->col = 0;
  }
}

bd_keyboard *bd_keyboard_create(void) {
  bd_keyboard *keyboard = (bd_keyboard *)calloc(1, sizeof(*keyboard));
  if (keyboard == NULL) {
    return NULL;
  }
  keyboard->pressed_row = -1;
  keyboard->pressed_col = -1;
  return keyboard;
}

void bd_keyboard_destroy(bd_keyboard *keyboard) {
  free(keyboard);
}

bool bd_keyboard_open(bd_keyboard *keyboard, const char *title,
                      const char *initial, size_t max_length, bool mask,
                      bd_keyboard_done_fn on_done, void *user) {
  if (keyboard == NULL) {
    return false;
  }
  if (max_length > BD_KEYBOARD_TEXT_MAX) {
    max_length = BD_KEYBOARD_TEXT_MAX;
  }
  keyboard->open = true;
  keyboard->max_length = max_length;
  keyboard->mask = mask;
  keyboard->on_done = on_done;
  keyboard->user = user;
  keyboard->mode = BD_KEYBOARD_MODE_LOWER;
  keyboard->row = 0;
  keyboard->col = 0;
  keyboard->pressed_row = -1;
  keyboard->pressed_col = -1;
  snprintf(keyboard->title, sizeof(keyboard->title), "%s",
           title != NULL ? title : "输入");
  snprintf(keyboard->text, sizeof(keyboard->text), "%s",
           initial != NULL ? initial : "");
  if (strlen(keyboard->text) > max_length) {
    keyboard->text[max_length] = '\0';
  }
  build_rows(keyboard);
  return true;
}

void bd_keyboard_close(bd_keyboard *keyboard) {
  if (keyboard == NULL) {
    return;
  }
  keyboard->open = false;
  keyboard->on_done = NULL;
  keyboard->user = NULL;
}

bool bd_keyboard_is_open(const bd_keyboard *keyboard) {
  return keyboard != NULL && keyboard->open;
}

const char *bd_keyboard_text(const bd_keyboard *keyboard) {
  return keyboard != NULL ? keyboard->text : "";
}

static void finish(bd_keyboard *keyboard, bool accepted) {
  bd_keyboard_done_fn callback = keyboard->on_done;
  void *user = keyboard->user;
  char text[BD_KEYBOARD_TEXT_MAX + 1];
  snprintf(text, sizeof(text), "%s", keyboard->text);
  bd_keyboard_close(keyboard);
  if (callback != NULL) {
    callback(user, accepted, accepted ? text : NULL);
  }
}

static void move(bd_keyboard *keyboard, int dx, int dy) {
  int row = keyboard->row + dy;
  if (row < 0) {
    row = keyboard->row_count - 1;
  } else if (row >= keyboard->row_count) {
    row = 0;
  }
  keyboard->row = row;
  keyboard->col += dx;
  int count = keyboard->key_count[row];
  if (count <= 0) {
    keyboard->col = 0;
  } else if (keyboard->col < 0) {
    keyboard->col = count - 1;
  } else if (keyboard->col >= count) {
    keyboard->col = 0;
  }
}

static void activate(bd_keyboard *keyboard) {
  if (keyboard->row < 0 || keyboard->row >= keyboard->row_count ||
      keyboard->col < 0 || keyboard->col >= keyboard->key_count[keyboard->row]) {
    return;
  }
  const bd_keyboard_key *key = &keyboard->keys[keyboard->row][keyboard->col];
  switch (key->action) {
    case BD_KEYBOARD_KEY_TEXT:
      append_char(keyboard, key->text);
      if (keyboard->mode == BD_KEYBOARD_MODE_UPPER) {
        keyboard->mode = BD_KEYBOARD_MODE_LOWER;
        build_rows(keyboard);
      }
      break;
    case BD_KEYBOARD_KEY_SPACE:
      append_char(keyboard, " ");
      break;
    case BD_KEYBOARD_KEY_BACKSPACE: {
      size_t length = strlen(keyboard->text);
      if (length > 0) {
        keyboard->text[length - 1] = '\0';
      }
      break;
    }
    case BD_KEYBOARD_KEY_SHIFT:
      keyboard->mode = keyboard->mode == BD_KEYBOARD_MODE_SYMBOLS
                           ? BD_KEYBOARD_MODE_LOWER
                           : (keyboard->mode == BD_KEYBOARD_MODE_UPPER
                                  ? BD_KEYBOARD_MODE_LOWER
                                  : BD_KEYBOARD_MODE_UPPER);
      build_rows(keyboard);
      break;
    case BD_KEYBOARD_KEY_SYMBOLS:
      keyboard->mode = keyboard->mode == BD_KEYBOARD_MODE_SYMBOLS
                           ? BD_KEYBOARD_MODE_LOWER
                           : BD_KEYBOARD_MODE_SYMBOLS;
      build_rows(keyboard);
      break;
    case BD_KEYBOARD_KEY_CANCEL:
      finish(keyboard, false);
      break;
    case BD_KEYBOARD_KEY_DONE:
      finish(keyboard, true);
      break;
    default:
      break;
  }
}

bool bd_keyboard_input(bd_keyboard *keyboard, bd_key key, bool pressed) {
  if (keyboard == NULL || !keyboard->open) {
    return false;
  }
  if (!pressed) {
    return true;
  }
  switch (key) {
    case BD_KEY_UP:
      move(keyboard, 0, -1);
      break;
    case BD_KEY_DOWN:
      move(keyboard, 0, 1);
      break;
    case BD_KEY_LEFT:
      move(keyboard, -1, 0);
      break;
    case BD_KEY_RIGHT:
      move(keyboard, 1, 0);
      break;
    case BD_KEY_A:
      keyboard->pressed_row = keyboard->row;
      keyboard->pressed_col = keyboard->col;
      keyboard->pressed_until = 0;
      activate(keyboard);
      break;
    case BD_KEY_B: {
      size_t length = strlen(keyboard->text);
      if (length > 0) {
        keyboard->text[length - 1] = '\0';
      }
      break;
    }
    case BD_KEY_X:
      keyboard->mode = keyboard->mode == BD_KEYBOARD_MODE_SYMBOLS
                           ? BD_KEYBOARD_MODE_LOWER
                           : BD_KEYBOARD_MODE_SYMBOLS;
      build_rows(keyboard);
      break;
    case BD_KEY_Y:
      finish(keyboard, true);
      break;
    default:
      break;
  }
  return true;
}

static int row_weight(const bd_keyboard *keyboard, int row) {
  int total = 0;
  for (int i = 0; i < keyboard->key_count[row]; ++i) {
    total += keyboard->keys[row][i].weight;
  }
  return total > 0 ? total : 1;
}

static void draw_key(bd_keyboard *keyboard, bd_gfx *gfx,
                     const bd_ui_fonts *fonts, bd_color accent, int row,
                     int index, bd_rect rect, bool selected) {
  bd_color key_color = bd_color_rgb(42, 39, 34);
  bd_color border = bd_color_rgb(64, 59, 51);
  bd_color ink = bd_color_rgb(238, 231, 218);
  if (selected) {
    key_color = bd_color_rgb(58, 50, 43);
    border = accent;
    ink = bd_color_rgb(255, 246, 236);
  }
  bd_gfx_fill_round_rect(gfx, rect, 8, key_color);
  bd_gfx_stroke_round_rect(gfx, rect, 8, selected ? 2 : 1, border);
  const bd_keyboard_key *key = &keyboard->keys[row][index];
  bd_font *font = key->action == BD_KEYBOARD_KEY_TEXT ? fonts->latin
                                                       : fonts->body;
  if (font == NULL) {
    font = fonts->body;
  }
  int width = bd_font_measure_text(font, key->label);
  int baseline = rect.y + (rect.h + bd_font_pixel_size(font)) / 2 - 2;
  bd_gfx_draw_text(gfx, font, rect.x + (rect.w - width) / 2, baseline, ink,
                   key->label);
}

void bd_keyboard_draw(bd_keyboard *keyboard, bd_gfx *gfx,
                      const bd_ui_fonts *fonts, bd_color accent,
                      uint64_t now_ms) {
  if (keyboard == NULL || !keyboard->open || gfx == NULL || fonts == NULL) {
    return;
  }
  int width = bd_gfx_width(gfx);
  int height = bd_gfx_height(gfx);
  bd_gfx_fill_rect(gfx, bd_rect_make(0, 0, width, height),
                   bd_color_rgba(0, 0, 0, 150));

  int panel_x = 32;
  int panel_y = height > 700 ? 286 : height / 3;
  int panel_w = width - 64;
  int panel_h = height - panel_y - 20;
  bd_gfx_fill_round_rect(gfx, bd_rect_make(panel_x, panel_y, panel_w, panel_h),
                         14, bd_color_rgb(24, 22, 19));
  bd_gfx_stroke_round_rect(
      gfx, bd_rect_make(panel_x, panel_y, panel_w, panel_h), 14, 1,
      bd_color_rgb(70, 63, 54));

  int inner_x = panel_x + 24;
  int inner_w = panel_w - 48;
  bd_font *title_font = fonts->title != NULL ? fonts->title : fonts->body;
  bd_gfx_draw_text(gfx, title_font, inner_x, panel_y + 46,
                   bd_color_rgb(246, 238, 226), keyboard->title);

  int field_y = panel_y + 64;
  int field_h = 54;
  bd_gfx_fill_round_rect(gfx, bd_rect_make(inner_x, field_y, inner_w, field_h),
                         8, bd_color_rgb(15, 14, 12));
  bd_gfx_stroke_round_rect(
      gfx, bd_rect_make(inner_x, field_y, inner_w, field_h), 8, 1,
      bd_color_rgb(72, 65, 55));

  char display[BD_KEYBOARD_TEXT_MAX * 3 + 4];
  size_t used = 0;
  size_t length = strlen(keyboard->text);
  for (size_t i = 0; i < length && used + 4 < sizeof(display); ++i) {
    if (keyboard->mask) {
      display[used++] = '*';
    } else {
      display[used++] = keyboard->text[i];
    }
  }
  display[used] = '\0';
  bd_font *body = fonts->body;
  int text_width = bd_font_measure_text(body, display);
  int max_width = inner_w - 32;
  int text_x = inner_x + 16;
  if (text_width > max_width && length > 0) {
    /* Show the tail of long input; the cursor is always at the end. */
    size_t skip = length > 16 ? length - 16 : 0;
    used = 0;
    for (size_t i = skip; i < length && used + 4 < sizeof(display); ++i) {
      if (keyboard->mask) {
        display[used++] = '*';
      } else {
        display[used++] = keyboard->text[i];
      }
    }
    display[used] = '\0';
    text_width = bd_font_measure_text(body, display);
  }
  int baseline = field_y + (field_h + bd_font_pixel_size(body)) / 2 - 2;
  bd_gfx_draw_text(gfx, body, text_x, baseline, bd_color_rgb(244, 236, 223),
                   display);
  bd_gfx_fill_rect(gfx,
                   bd_rect_make(text_x + text_width + 3, field_y + 12, 2,
                                field_h - 24),
                   accent);

  int key_top = field_y + field_h + 18;
  int key_area_h = panel_y + panel_h - 18 - key_top;
  int row_gap = 8;
  int row_h = (key_area_h - row_gap * (keyboard->row_count - 1)) /
              keyboard->row_count;
  int col_gap = 8;
  for (int r = 0; r < keyboard->row_count; ++r) {
    int count = keyboard->key_count[r];
    if (count <= 0) {
      continue;
    }
    int total = row_weight(keyboard, r);
    int available = inner_w - col_gap * (count - 1);
    int x = inner_x;
    int y = key_top + r * (row_h + row_gap);
    for (int c = 0; c < count; ++c) {
      int key_w = available * keyboard->keys[r][c].weight / total;
      if (c == count - 1) {
        key_w = inner_x + inner_w - x;
      }
      bool selected = r == keyboard->row && c == keyboard->col;
      bool pressed = r == keyboard->pressed_row &&
                     c == keyboard->pressed_col &&
                     now_ms < keyboard->pressed_until;
      draw_key(keyboard, gfx, fonts, accent, r, c,
               bd_rect_make(x, y, key_w, row_h), selected || pressed);
      x += key_w + col_gap;
    }
  }
}
