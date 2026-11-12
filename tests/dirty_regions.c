#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brickdesk/desktop.h"
#include "brickdesk/gfx.h"
#include "brickdesk/platform.h"
#include "brickdesk/theme.h"

#define TEST_WIDTH 1024
#define TEST_HEIGHT 768

static bool draw_frame(bd_platform *platform, bd_gfx *gfx, bd_desktop *desktop,
                       bd_image *capture) {
  bd_gfx_begin_frame(gfx);
  bd_desktop_draw(desktop, gfx);
  bd_gfx_end_frame(gfx);
  if (bd_gfx_get_stats(gfx).presented) {
    bd_platform_present(platform);
  }
  if (capture != NULL) {
    return bd_gfx_read_image(gfx, capture);
  }
  return true;
}

static bool images_equal(const bd_image *a, const bd_image *b) {
  if (a->width != b->width || a->height != b->height) {
    return false;
  }
  return memcmp(a->pixels, b->pixels, (size_t)a->width * a->height * 4u) == 0;
}

int main(void) {
  bd_platform_config config;
  config.title = "brickdesk dirty regions";
  config.width = TEST_WIDTH;
  config.height = TEST_HEIGHT;
  config.hidden = true;
  bd_platform *platform = bd_platform_create(&config);
  if (platform == NULL) {
    return 1;
  }
  bd_gfx *gfx = bd_gfx_create(TEST_WIDTH, TEST_HEIGHT);
  if (gfx == NULL) {
    bd_platform_destroy(platform);
    return 1;
  }
  bd_desktop_theme theme;
  theme.dark = false;
  theme.ambient_baked = true;
  if (!bd_color_from_hex("#ae432b", &theme.accent)) {
    fprintf(stderr, "dirty: invalid test accent\n");
    return 1;
  }
  bd_desktop *desktop = bd_desktop_create(gfx, &theme);
  if (desktop == NULL) {
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }

  bd_desktop_set_focus(desktop, 5);
  int quiet = 0;
  for (int tries = 0; tries < 300 && quiet < 2; ++tries) {
    draw_frame(platform, gfx, desktop, NULL);
    if (bd_desktop_has_pending_images(desktop)) {
      quiet = 0;
    } else {
      ++quiet;
    }
    bd_event event;
    bd_platform_wait_event(platform, &event, 5);
  }

  bd_gfx_damage_all(gfx);
  if (!draw_frame(platform, gfx, desktop, NULL)) {
    fprintf(stderr, "dirty: initial capture failed\n");
    return 1;
  }

  if (!bd_desktop_handle_key(desktop, BD_KEY_RIGHT, true)) {
    fprintf(stderr, "dirty: focus did not move\n");
    return 1;
  }
  bd_image partial;
  if (!draw_frame(platform, gfx, desktop, &partial)) {
    fprintf(stderr, "dirty: partial capture failed\n");
    return 1;
  }
  bd_gfx_stats partial_stats = bd_gfx_get_stats(gfx);
  bd_rect damage[8];
  int damage_count = bd_gfx_damage_rects(gfx, damage, 8);
  printf("dirty: focus=%d damage_px=%lld screen_px=%d ratio=%.2f%% rects=%d "
         "draws=%d\n",
         bd_desktop_focus(desktop), (long long)partial_stats.damage_pixels,
         TEST_WIDTH * TEST_HEIGHT,
         100.0 * (double)partial_stats.damage_pixels /
             (double)(TEST_WIDTH * TEST_HEIGHT),
         damage_count, partial_stats.draw_calls);

  if (partial_stats.damage_pixels >=
      (int64_t)(TEST_WIDTH * TEST_HEIGHT / 5)) {
    fprintf(stderr, "dirty: focus move repainted too much (expected < 20%%)\n");
    return 1;
  }
  if (damage_count > 2) {
    fprintf(stderr, "dirty: focus move produced %d damage rects (expected <= 2)\n",
            damage_count);
    return 1;
  }

  bd_gfx_damage_all(gfx);
  bd_image full;
  if (!draw_frame(platform, gfx, desktop, &full)) {
    fprintf(stderr, "dirty: full capture failed\n");
    return 1;
  }
  if (!images_equal(&partial, &full)) {
    fprintf(stderr, "dirty: partial redraw differs from a full redraw\n");
    return 1;
  }
  printf("dirty: partial image matches full redraw exactly\n");
  bd_image_free(&partial);
  bd_image_free(&full);

  bd_gfx_begin_frame(gfx);
  bd_desktop_draw(desktop, gfx);
  bd_gfx_end_frame(gfx);
  bd_gfx_stats idle = bd_gfx_get_stats(gfx);
  printf("dirty: idle damage_px=%lld draws=%d blur=%d presented=%d\n",
         (long long)idle.damage_pixels, idle.draw_calls, idle.blur_passes,
         idle.presented ? 1 : 0);
  if (idle.damage_pixels != 0 || idle.draw_calls != 0 || idle.blur_passes != 0 ||
      idle.presented) {
    fprintf(stderr, "dirty: idle frame was not free\n");
    return 1;
  }

  /* A change under the floating top bar must dirty the whole blur strip. */
  bd_gfx_damage_rect(gfx, bd_rect_make(100, 10, 40, 40));
  bd_gfx_begin_frame(gfx);
  bd_desktop_draw(desktop, gfx);
  bd_gfx_end_frame(gfx);
  bd_gfx_stats blur_stats = bd_gfx_get_stats(gfx);
  int64_t bar_pixels = (int64_t)TEST_WIDTH * 76;
  printf("dirty: blur dependency damage_px=%lld bar_px=%lld rects=%d\n",
         (long long)blur_stats.damage_pixels, (long long)bar_pixels,
         blur_stats.damage_rects);
  if (blur_stats.damage_pixels < bar_pixels) {
    fprintf(stderr, "dirty: blur strip was not included in damage\n");
    return 1;
  }

  bd_desktop_set_accent(desktop, bd_color_rgb(47, 111, 143));
  bd_gfx_begin_frame(gfx);
  bd_desktop_draw(desktop, gfx);
  bd_gfx_end_frame(gfx);
  bd_gfx_stats accent = bd_gfx_get_stats(gfx);
  printf("dirty: runtime accent change redrew %lld px full=%d\n",
         (long long)accent.damage_pixels, accent.full_redraw ? 1 : 0);
  if (!accent.full_redraw) {
    fprintf(stderr, "dirty: accent change did not request a full redraw\n");
    return 1;
  }

  bd_desktop_destroy(desktop);
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  return 0;
}
