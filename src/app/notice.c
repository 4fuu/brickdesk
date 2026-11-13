#define _POSIX_C_SOURCE 200809L

#include "brickdesk/notice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brickdesk/ui.h"

#ifndef BRICKDESK_ASSET_DIR
#define BRICKDESK_ASSET_DIR "assets"
#endif

/*
 * The bundled Noto faces declare a 1.448 em span, so loading 1.448x the CSS
 * size keeps notices at the same optical size as the settings page on the
 * small 1024x768 panel.  Do not shrink these further.
 */
#define BD_NOTICE_FONT_EM_SCALE 1.448f

static bd_font *notice_load_font(bd_gfx *gfx, const char *name,
                                 int pixel_size) {
  char path[1024];
  int written = snprintf(path, sizeof(path), "%s/fonts/%s", BRICKDESK_ASSET_DIR,
                         name);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    return NULL;
  }
  int nominal = (int)((float)pixel_size * BD_NOTICE_FONT_EM_SCALE + 0.5f);
  return bd_font_create(gfx, path, nominal);
}

static bd_font *notice_load_fallback(bd_gfx *gfx) {
  static const char *const CANDIDATES[] = {
      "NotoSansSC-Full.ttf", "NotoSansSC-Full.otf",
      "NotoSansCJKsc-Regular.otf", "NotoSansSC-Regular.otf",
  };
  for (size_t i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); ++i) {
    char path[1024];
    int written = snprintf(path, sizeof(path), "%s/fonts/%s",
                           BRICKDESK_ASSET_DIR, CANDIDATES[i]);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
      continue;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
      continue;
    }
    fclose(file);
    bd_font *font = bd_font_create(gfx, path, 29);
    if (font != NULL) {
      return font;
    }
  }
  return NULL;
}

static bool notice_fonts_load(bd_ui_fonts *fonts, bd_gfx *gfx) {
  memset(fonts, 0, sizeof(*fonts));
  fonts->title = notice_load_font(gfx, "NotoSansSC-700-subset.ttf", 24);
  fonts->body = notice_load_font(gfx, "NotoSansSC-500-subset.ttf", 22);
  fonts->badge = notice_load_font(gfx, "NotoSansSC-500-subset.ttf", 20);
  if (fonts->title == NULL || fonts->body == NULL || fonts->badge == NULL) {
    bd_ui_fonts_free(fonts);
    return false;
  }
  fonts->fallback_full = notice_load_fallback(gfx);
  bd_font_set_fallback(fonts->title, fonts->fallback_full);
  bd_font_set_fallback(fonts->body, fonts->fallback_full);
  bd_font_set_fallback(fonts->badge, fonts->fallback_full);
  return true;
}

typedef struct bd_notice_toast {
  char text[BD_NOTICE_TEXT_MAX];
  bd_notice_level level;
  uint32_t timeout_ms;
  uint64_t shown_ms;
  bool shown;
} bd_notice_toast;

struct bd_notice {
  bd_gfx *gfx;
  bd_ui_fonts fonts;
  bool fonts_ready;

  bd_notice_toast queue[BD_NOTICE_QUEUE_MAX];
  size_t head;
  size_t count;

  bool confirm_open;
  char confirm_title[BD_NOTICE_TITLE_MAX];
  char confirm_text[BD_NOTICE_TEXT_MAX];
  uint32_t confirm_timeout_ms;
  uint64_t confirm_shown_ms;
  int confirm_focus; /* 0 = cancel, 1 = confirm */
  bd_notice_confirm_fn confirm_cb;
  void *confirm_user;

  bd_rect last_toast;
  bool last_toast_valid;
  bd_rect last_confirm;
  bool last_confirm_valid;
};

static const bd_color NOTICE_SURFACE = {34.0f / 255.0f, 31.0f / 255.0f,
                                        25.0f / 255.0f, 246.0f / 255.0f};
static const bd_color NOTICE_INK = {243.0f / 255.0f, 237.0f / 255.0f,
                                    226.0f / 255.0f, 1.0f};
static const bd_color NOTICE_INK2 = {188.0f / 255.0f, 178.0f / 255.0f,
                                     163.0f / 255.0f, 1.0f};
static const bd_color NOTICE_SCRIM = {0.0f, 0.0f, 0.0f, 0.58f};

static bd_color level_color(bd_notice_level level) {
  switch (level) {
    case BD_NOTICE_SUCCESS:
      return bd_color_rgb(111, 207, 151);
    case BD_NOTICE_WARNING:
      return bd_color_rgb(242, 201, 76);
    case BD_NOTICE_ERROR:
      return bd_color_rgb(235, 87, 87);
    case BD_NOTICE_INFO:
    default:
      return bd_color_rgb(111, 168, 255);
  }
}

static void copy_text(char *dst, size_t capacity, const char *src) {
  if (capacity == 0) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  size_t length = strlen(src);
  if (length >= capacity) {
    length = capacity - 1;
  }
  memcpy(dst, src, length);
  dst[length] = '\0';
}

static bd_notice_toast *current_toast(bd_notice *notice) {
  if (notice == NULL || notice->count == 0) {
    return NULL;
  }
  return &notice->queue[notice->head];
}

static void drop_current_toast(bd_notice *notice) {
  if (notice == NULL || notice->count == 0) {
    return;
  }
  if (notice->last_toast_valid) {
    bd_gfx_damage_rect(notice->gfx, notice->last_toast);
    notice->last_toast_valid = false;
  }
  notice->queue[notice->head].shown = false;
  notice->head = (notice->head + 1) % BD_NOTICE_QUEUE_MAX;
  --notice->count;
}

static bd_rect toast_rect(const bd_notice *notice, const bd_notice_toast *toast) {
  int width = bd_font_measure_text(notice->fonts.body, toast->text) + 110;
  if (width < 280) {
    width = 280;
  }
  if (width > 760) {
    width = 760;
  }
  int height = 72;
  int x = (bd_gfx_width(notice->gfx) - width) / 2;
  int y = 84;
  return bd_rect_make(x, y, width, height);
}

static bd_rect confirm_card_rect(const bd_notice *notice) {
  int width = 580;
  int height = 270;
  int x = (bd_gfx_width(notice->gfx) - width) / 2;
  int y = (bd_gfx_height(notice->gfx) - height) / 2;
  return bd_rect_make(x, y, width, height);
}

bd_notice *bd_notice_create(bd_gfx *gfx) {
  if (gfx == NULL) {
    return NULL;
  }
  bd_notice *notice = calloc(1, sizeof(*notice));
  if (notice == NULL) {
    return NULL;
  }
  notice->gfx = gfx;
  notice->fonts_ready = notice_fonts_load(&notice->fonts, gfx);
  if (!notice->fonts_ready) {
    free(notice);
    return NULL;
  }
  notice->confirm_focus = 0;
  return notice;
}

void bd_notice_destroy(bd_notice *notice) {
  if (notice == NULL) {
    return;
  }
  if (notice->confirm_open && notice->confirm_cb != NULL) {
    notice->confirm_cb(notice->confirm_user, false);
  }
  if (notice->fonts_ready) {
    bd_ui_fonts_free(&notice->fonts);
  }
  free(notice);
}

bool bd_notice_show(bd_notice *notice, bd_notice_level level,
                    const char *text, uint32_t timeout_ms, uint64_t now_ms) {
  (void)now_ms;
  if (notice == NULL || text == NULL || text[0] == '\0') {
    return false;
  }
  if (strlen(text) >= BD_NOTICE_TEXT_MAX || notice->count >= BD_NOTICE_QUEUE_MAX) {
    return false;
  }
  size_t index = (notice->head + notice->count) % BD_NOTICE_QUEUE_MAX;
  bd_notice_toast *toast = &notice->queue[index];
  memset(toast, 0, sizeof(*toast));
  copy_text(toast->text, sizeof(toast->text), text);
  toast->level = level;
  toast->timeout_ms =
      timeout_ms == 0 ? BD_NOTICE_DEFAULT_TIMEOUT_MS : timeout_ms;
  ++notice->count;
  return true;
}

bool bd_notice_confirm(bd_notice *notice, const char *title, const char *text,
                       uint32_t timeout_ms, bd_notice_confirm_fn on_done,
                       void *user, uint64_t now_ms) {
  if (notice == NULL || title == NULL || title[0] == '\0' || text == NULL ||
      notice->confirm_open) {
    return false;
  }
  if (strlen(title) >= BD_NOTICE_TITLE_MAX ||
      strlen(text) >= BD_NOTICE_TEXT_MAX) {
    return false;
  }
  copy_text(notice->confirm_title, sizeof(notice->confirm_title), title);
  copy_text(notice->confirm_text, sizeof(notice->confirm_text), text);
  notice->confirm_timeout_ms = timeout_ms;
  notice->confirm_shown_ms = now_ms;
  notice->confirm_focus = 1;
  notice->confirm_cb = on_done;
  notice->confirm_user = user;
  notice->confirm_open = true;
  bd_gfx_damage_all(notice->gfx);
  return true;
}

bool bd_notice_confirm_open(const bd_notice *notice) {
  return notice != NULL && notice->confirm_open;
}

bool bd_notice_active(const bd_notice *notice) {
  return notice != NULL && (notice->confirm_open || notice->count > 0);
}

uint64_t bd_notice_next_deadline(const bd_notice *notice) {
  if (notice == NULL) {
    return 0;
  }
  if (notice->confirm_open && notice->confirm_timeout_ms > 0) {
    return notice->confirm_shown_ms + notice->confirm_timeout_ms;
  }
  bd_notice_toast *toast =
      (bd_notice_toast *)current_toast((bd_notice *)notice);
  if (toast != NULL && toast->shown && toast->timeout_ms > 0) {
    return toast->shown_ms + toast->timeout_ms;
  }
  return 0;
}

bool bd_notice_update(bd_notice *notice, uint64_t now_ms) {
  if (notice == NULL) {
    return false;
  }
  bool changed = false;
  if (notice->confirm_open && notice->confirm_timeout_ms > 0 &&
      now_ms >= notice->confirm_shown_ms + notice->confirm_timeout_ms) {
    bd_notice_confirm_fn callback = notice->confirm_cb;
    void *user = notice->confirm_user;
    notice->confirm_open = false;
    notice->confirm_cb = NULL;
    notice->confirm_user = NULL;
    bd_gfx_damage_all(notice->gfx);
    changed = true;
    if (callback != NULL) {
      callback(user, false);
    }
  }
  if (notice->confirm_open) {
    return changed;
  }
  bd_notice_toast *toast = current_toast(notice);
  if (toast == NULL) {
    return changed;
  }
  if (!toast->shown) {
    /* The app owned the screen while this was queued; start the clock now. */
    toast->shown = true;
    toast->shown_ms = now_ms;
    changed = true;
  }
  if (toast->timeout_ms > 0 &&
      now_ms >= toast->shown_ms + toast->timeout_ms) {
    drop_current_toast(notice);
    changed = true;
  }
  return changed;
}

static void draw_toast(bd_notice *notice, bd_gfx *gfx) {
  bd_notice_toast *toast = current_toast(notice);
  if (toast == NULL) {
    return;
  }
  bd_rect rect = toast_rect(notice, toast);
  bd_gfx_damage_rect(gfx, rect);
  notice->last_toast = rect;
  notice->last_toast_valid = true;

  bd_gfx_fill_round_rect(gfx, rect, 14, NOTICE_SURFACE);
  bd_gfx_stroke_round_rect(gfx, rect, 14, 1,
                           bd_color_rgba(243, 237, 226, 34));
  bd_rect accent = bd_rect_make(rect.x + 1, rect.y + 15, 4, rect.h - 30);
  bd_gfx_fill_round_rect(gfx, accent, 2, level_color(toast->level));
  bd_rect dot = bd_rect_make(rect.x + 24, rect.y + rect.h / 2 - 6, 12, 12);
  bd_gfx_fill_round_rect(gfx, dot, 6, level_color(toast->level));
  bd_gfx_draw_text_ellipsized(gfx, notice->fonts.body, rect.x + 48,
                              rect.y + rect.h / 2 + 8, rect.w - 72, NOTICE_INK,
                              toast->text);
}

static void draw_button(bd_notice *notice, bd_gfx *gfx, bd_rect rect,
                        const char *label, bool primary, bool focused) {
  bd_color fill = primary ? bd_color_rgb(224, 122, 95)
                          : bd_color_rgba(243, 237, 226, 20);
  bd_gfx_fill_round_rect(gfx, rect, 10, fill);
  if (!primary) {
    bd_gfx_stroke_round_rect(gfx, rect, 10, 1,
                             bd_color_rgba(243, 237, 226, 40));
  }
  int width = bd_font_measure_text(notice->fonts.body, label);
  bd_gfx_draw_text(gfx, notice->fonts.body, rect.x + (rect.w - width) / 2,
                   rect.y + 32, primary ? bd_color_rgb(28, 24, 20) : NOTICE_INK,
                   label);
  if (focused) {
    bd_rect outline = bd_rect_make(rect.x - 3, rect.y - 3, rect.w + 6,
                                   rect.h + 6);
    bd_gfx_stroke_round_rect(gfx, outline, 13, 2, NOTICE_INK);
  }
}

static void draw_confirm(bd_notice *notice, bd_gfx *gfx) {
  bd_gfx_fill_rect(gfx, bd_rect_make(0, 0, bd_gfx_width(gfx),
                                     bd_gfx_height(gfx)),
                   NOTICE_SCRIM);
  bd_rect card = confirm_card_rect(notice);
  notice->last_confirm = card;
  notice->last_confirm_valid = true;
  bd_gfx_damage_rect(gfx, card);

  bd_gfx_fill_round_rect(gfx, card, 16, NOTICE_SURFACE);
  bd_gfx_stroke_round_rect(gfx, card, 16, 1,
                           bd_color_rgba(243, 237, 226, 40));
  bd_gfx_draw_text_ellipsized(gfx, notice->fonts.title, card.x + 28,
                              card.y + 58, card.w - 56, NOTICE_INK,
                              notice->confirm_title);

  int y = card.y + 104;
  const char *text = notice->confirm_text;
  while (*text != '\0' && y < card.y + card.h - 110) {
    const char *newline = strchr(text, '\n');
    size_t length = newline != NULL ? (size_t)(newline - text) : strlen(text);
    char line[BD_NOTICE_TEXT_MAX];
    if (length >= sizeof(line)) {
      length = sizeof(line) - 1;
    }
    memcpy(line, text, length);
    line[length] = '\0';
    bd_gfx_draw_text_ellipsized(gfx, notice->fonts.body, card.x + 28, y,
                                card.w - 56, NOTICE_INK2, line);
    y += 32;
    if (newline == NULL) {
      break;
    }
    text = newline + 1;
  }

  int button_y = card.y + card.h - 24 - 48;
  bd_rect confirm = bd_rect_make(card.x + card.w - 24 - 128, button_y, 128, 48);
  bd_rect cancel = bd_rect_make(confirm.x - 10 - 128, button_y, 128, 48);
  draw_button(notice, gfx, cancel, "取消", false, notice->confirm_focus == 0);
  draw_button(notice, gfx, confirm, "确认", true, notice->confirm_focus == 1);
}

void bd_notice_draw(bd_notice *notice, bd_gfx *gfx, uint64_t now_ms) {
  (void)now_ms;
  if (notice == NULL || gfx == NULL) {
    return;
  }
  if (notice->last_confirm_valid && !notice->confirm_open) {
    notice->last_confirm_valid = false;
  }
  if (notice->confirm_open) {
    draw_confirm(notice, gfx);
    return;
  }
  draw_toast(notice, gfx);
}

bool bd_notice_input(bd_notice *notice, bd_notice_action action) {
  if (notice == NULL || !notice->confirm_open) {
    return false;
  }
  switch (action) {
    case BD_NOTICE_ACTION_LEFT:
    case BD_NOTICE_ACTION_RIGHT:
      notice->confirm_focus = notice->confirm_focus == 0 ? 1 : 0;
      bd_gfx_damage_rect(notice->gfx, notice->last_confirm);
      return true;
    case BD_NOTICE_ACTION_ACCEPT: {
      bd_notice_confirm_fn callback = notice->confirm_cb;
      void *user = notice->confirm_user;
      notice->confirm_open = false;
      notice->confirm_cb = NULL;
      notice->confirm_user = NULL;
      bd_gfx_damage_all(notice->gfx);
      if (callback != NULL) {
        callback(user, notice->confirm_focus == 1);
      }
      return true;
    }
    case BD_NOTICE_ACTION_CANCEL: {
      bd_notice_confirm_fn callback = notice->confirm_cb;
      void *user = notice->confirm_user;
      notice->confirm_open = false;
      notice->confirm_cb = NULL;
      notice->confirm_user = NULL;
      bd_gfx_damage_all(notice->gfx);
      if (callback != NULL) {
        callback(user, false);
      }
      return true;
    }
    case BD_NOTICE_ACTION_NONE:
    default:
      return true;
  }
}

void bd_notice_clear(bd_notice *notice) {
  if (notice == NULL) {
    return;
  }
  bool had = notice->confirm_open || notice->count > 0;
  if (notice->confirm_open && notice->confirm_cb != NULL) {
    notice->confirm_cb(notice->confirm_user, false);
  }
  notice->confirm_open = false;
  notice->confirm_cb = NULL;
  notice->confirm_user = NULL;
  notice->head = 0;
  notice->count = 0;
  for (size_t i = 0; i < BD_NOTICE_QUEUE_MAX; ++i) {
    notice->queue[i].shown = false;
  }
  if (had) {
    bd_gfx_damage_all(notice->gfx);
  }
}
