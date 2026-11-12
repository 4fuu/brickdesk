#include "gfx_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_truetype.h"

#define BD_FONT_MAX_FALLBACKS 4

typedef struct bd_glyph {
  uint32_t codepoint;
  int slot;
  int width;
  int height;
  int xoff;
  int yoff;
  int advance;
  float u0;
  float v0;
  float u1;
  float v1;
  uint64_t stamp;
} bd_glyph;

struct bd_font {
  bd_gfx *gfx;
  GLuint atlas;
  int atlas_size;
  int cell_size;
  int columns;
  int capacity;
  int pixel_size;
  int ascent;
  int descent;
  int line_height;
  float scale;
  stbtt_fontinfo info;
  unsigned char *data;
  bd_font *fallback;
  bd_glyph *glyphs;
  int *hash;
  int hash_size;
  int used_slots;
  uint64_t clock;
};

static int utf8_decode(const char **cursor, const char *end, uint32_t *codepoint) {
  const unsigned char *bytes = (const unsigned char *)*cursor;
  if (bytes >= (const unsigned char *)end) {
    return 0;
  }
  unsigned char first = bytes[0];
  int length = 1;
  uint32_t value = first;
  if ((first & 0x80u) == 0) {
    length = 1;
    value = first;
  } else if ((first & 0xE0u) == 0xC0u) {
    length = 2;
    value = first & 0x1Fu;
  } else if ((first & 0xF0u) == 0xE0u) {
    length = 3;
    value = first & 0x0Fu;
  } else if ((first & 0xF8u) == 0xF0u) {
    length = 4;
    value = first & 0x07u;
  } else {
    *cursor += 1;
    *codepoint = 0xFFFDu;
    return 1;
  }

  if ((const unsigned char *)end - bytes < length) {
    *cursor += 1;
    *codepoint = 0xFFFDu;
    return 1;
  }
  for (int i = 1; i < length; ++i) {
    if ((bytes[i] & 0xC0u) != 0x80u) {
      *cursor += 1;
      *codepoint = 0xFFFDu;
      return 1;
    }
    value = (value << 6) | (bytes[i] & 0x3Fu);
  }
  *cursor += length;
  *codepoint = value;
  return length;
}

static bd_font *font_for_codepoint(bd_font *font, uint32_t codepoint) {
  bd_font *cursor = font;
  for (int depth = 0; cursor != NULL && depth < BD_FONT_MAX_FALLBACKS; ++depth) {
    if (stbtt_FindGlyphIndex(&cursor->info, (int)codepoint) != 0) {
      return cursor;
    }
    cursor = cursor->fallback;
  }
  return NULL;
}

static unsigned int hash_codepoint(uint32_t codepoint, int mask) {
  return (unsigned int)((codepoint * 2654435761u) & (unsigned int)mask);
}

static int hash_lookup(const bd_font *font, uint32_t codepoint) {
  int mask = font->hash_size - 1;
  unsigned int index = hash_codepoint(codepoint, mask);
  for (int probe = 0; probe < font->hash_size; ++probe) {
    int value = font->hash[index];
    if (value == 0) {
      return -1;
    }
    if (value > 0 && font->glyphs[value - 1].codepoint == codepoint) {
      return value - 1;
    }
    index = (index + 1u) & (unsigned int)mask;
  }
  return -1;
}

static void hash_remove(bd_font *font, uint32_t codepoint) {
  int mask = font->hash_size - 1;
  unsigned int index = hash_codepoint(codepoint, mask);
  for (int probe = 0; probe < font->hash_size; ++probe) {
    int value = font->hash[index];
    if (value == 0) {
      return;
    }
    if (value > 0 && font->glyphs[value - 1].codepoint == codepoint) {
      font->hash[index] = -1;
      return;
    }
    index = (index + 1u) & (unsigned int)mask;
  }
}

static void hash_insert(bd_font *font, uint32_t codepoint, int slot) {
  int mask = font->hash_size - 1;
  unsigned int index = hash_codepoint(codepoint, mask);
  int tombstone = -1;
  for (int probe = 0; probe < font->hash_size; ++probe) {
    int value = font->hash[index];
    if (value == 0) {
      if (tombstone >= 0) {
        font->hash[tombstone] = slot + 1;
      } else {
        font->hash[index] = slot + 1;
      }
      return;
    }
    if (value == -1 && tombstone < 0) {
      tombstone = (int)index;
    }
    index = (index + 1u) & (unsigned int)mask;
  }
  if (tombstone >= 0) {
    font->hash[tombstone] = slot + 1;
  }
}

static int next_power_of_two(int value) {
  int result = 64;
  while (result < value) {
    result *= 2;
  }
  return result;
}

static int find_free_slot(bd_font *font) {
  for (int i = 0; i < font->capacity; ++i) {
    if (font->glyphs[i].codepoint == 0) {
      return i;
    }
  }
  return -1;
}

static int evict_lru_slot(bd_font *font) {
  int victim = -1;
  uint64_t oldest = UINT64_MAX;
  for (int i = 0; i < font->capacity; ++i) {
    if (font->glyphs[i].codepoint != 0 && font->glyphs[i].stamp < oldest) {
      oldest = font->glyphs[i].stamp;
      victim = i;
    }
  }
  if (victim >= 0) {
    hash_remove(font, font->glyphs[victim].codepoint);
    font->glyphs[victim].codepoint = 0;
    --font->used_slots;
  }
  return victim;
}

static bool get_glyph(bd_font *font, uint32_t codepoint, bd_glyph **out) {
  int existing = hash_lookup(font, codepoint);
  if (existing >= 0) {
    font->glyphs[existing].stamp = ++font->clock;
    *out = &font->glyphs[existing];
    return true;
  }

  int slot = find_free_slot(font);
  if (slot < 0) {
    slot = evict_lru_slot(font);
  }
  if (slot < 0) {
    return false;
  }

  bd_glyph *glyph = &font->glyphs[slot];
  memset(glyph, 0, sizeof(*glyph));
  glyph->codepoint = codepoint;
  glyph->slot = slot;

  int width = 0;
  int height = 0;
  int xoff = 0;
  int yoff = 0;
  unsigned char *bitmap = stbtt_GetCodepointBitmap(
      &font->info, font->scale, font->scale, (int)codepoint, &width, &height,
      &xoff, &yoff);
  int column = slot % font->columns;
  int row = slot / font->columns;
  int origin_x = column * font->cell_size + 1;
  int origin_y = row * font->cell_size + 1;
  if (bitmap != NULL && width > 0 && height > 0 &&
      width <= font->cell_size - 2 && height <= font->cell_size - 2) {
    glBindTexture(GL_TEXTURE_2D, font->atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, origin_x, origin_y, width, height,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, bitmap);
    glyph->width = width;
    glyph->height = height;
    glyph->xoff = xoff;
    glyph->yoff = yoff;
    glyph->u0 = (float)origin_x / (float)font->atlas_size;
    glyph->v0 = (float)origin_y / (float)font->atlas_size;
    glyph->u1 = (float)(origin_x + width) / (float)font->atlas_size;
    glyph->v1 = (float)(origin_y + height) / (float)font->atlas_size;
    ++font->gfx->stats.glyphs_rasterized;
  }
  if (bitmap != NULL) {
    stbtt_FreeBitmap(bitmap, NULL);
  }

  int advance = 0;
  int left_bearing = 0;
  stbtt_GetCodepointHMetrics(&font->info, (int)codepoint, &advance, &left_bearing);
  glyph->advance = (int)floorf((float)advance * font->scale + 0.5f);
  glyph->stamp = ++font->clock;
  ++font->used_slots;
  hash_insert(font, codepoint, slot);
  *out = glyph;
  return true;
}

static float kern_advance(const bd_font *font, uint32_t previous, uint32_t current) {
  if (previous == 0) {
    return 0.0f;
  }
  return (float)stbtt_GetCodepointKernAdvance(&font->info, (int)previous,
                                              (int)current) *
         font->scale;
}

static int text_advance(bd_font *font, uint32_t previous_codepoint,
                        bd_font **previous_font, uint32_t codepoint) {
  bd_font *resolved = font_for_codepoint(font, codepoint);
  if (resolved == NULL) {
    ++font->gfx->stats.missing_glyphs;
    *previous_font = NULL;
    return font->pixel_size / 2;
  }
  int advance = 0;
  int left_bearing = 0;
  stbtt_GetCodepointHMetrics(&resolved->info, (int)codepoint, &advance,
                             &left_bearing);
  int width = (int)floorf((float)advance * resolved->scale + 0.5f);
  if (*previous_font == resolved && previous_codepoint != 0) {
    width += (int)floorf(kern_advance(resolved, previous_codepoint, codepoint) + 0.5f);
  }
  *previous_font = resolved;
  return width;
}

static int measure_text(const bd_font *font, const char *utf8) {
  if (font == NULL || utf8 == NULL) {
    return 0;
  }
  bd_font *mutable_font = (bd_font *)font;
  const char *cursor = utf8;
  const char *end = utf8 + strlen(utf8);
  int width = 0;
  uint32_t previous = 0;
  bd_font *previous_font = NULL;
  while (cursor < end) {
    uint32_t codepoint = 0;
    if (utf8_decode(&cursor, end, &codepoint) <= 0) {
      break;
    }
    width += text_advance(mutable_font, previous, &previous_font, codepoint);
    previous = codepoint;
  }
  return width;
}

int bd_font_measure_text(const bd_font *font, const char *utf8) {
  return measure_text(font, utf8);
}

bd_font *bd_font_create(bd_gfx *gfx, const char *path, int pixel_size) {
  if (gfx == NULL || path == NULL || pixel_size <= 0 || pixel_size > 192) {
    return NULL;
  }
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    bd_gfx_set_error(gfx, "cannot open font file");
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    bd_gfx_set_error(gfx, "cannot seek font file");
    return NULL;
  }
  long size = ftell(file);
  if (size <= 0 || size > 16 * 1024 * 1024) {
    fclose(file);
    bd_gfx_set_error(gfx, "font file is empty or too large");
    return NULL;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    bd_gfx_set_error(gfx, "cannot rewind font file");
    return NULL;
  }
  unsigned char *data = (unsigned char *)malloc((size_t)size);
  if (data == NULL) {
    fclose(file);
    bd_gfx_set_error(gfx, "out of memory for font data");
    return NULL;
  }
  if (fread(data, 1, (size_t)size, file) != (size_t)size) {
    free(data);
    fclose(file);
    bd_gfx_set_error(gfx, "short read on font file");
    return NULL;
  }
  fclose(file);

  bd_font *font = (bd_font *)calloc(1, sizeof(*font));
  if (font == NULL) {
    free(data);
    return NULL;
  }
  font->gfx = gfx;
  font->data = data;
  font->pixel_size = pixel_size;

  int offset = stbtt_GetFontOffsetForIndex(data, 0);
  if (offset < 0 || !stbtt_InitFont(&font->info, data, offset)) {
    bd_gfx_set_error(gfx, "stb_truetype failed to parse font");
    bd_font_destroy(font);
    return NULL;
  }

  int ascent = 0;
  int descent = 0;
  int line_gap = 0;
  stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &line_gap);
  font->scale = stbtt_ScaleForPixelHeight(&font->info, (float)pixel_size);
  font->ascent = (int)ceilf((float)ascent * font->scale);
  font->descent = (int)floorf((float)descent * font->scale);
  font->line_height = font->ascent - font->descent +
                      (int)floorf((float)line_gap * font->scale + 0.5f);
  if (font->line_height < pixel_size) {
    font->line_height = pixel_size;
  }
  font->atlas_size = pixel_size >= 28 ? 1024 : 512;
  font->cell_size = (int)ceilf((float)pixel_size * 1.45f) + 4;
  if (font->cell_size < 16) {
    font->cell_size = 16;
  }
  font->columns = font->atlas_size / font->cell_size;
  font->capacity = font->columns * font->columns;
  if (font->capacity < 1) {
    bd_gfx_set_error(gfx, "font size leaves no atlas capacity");
    bd_font_destroy(font);
    return NULL;
  }
  font->hash_size = next_power_of_two(font->capacity * 2);
  font->glyphs = (bd_glyph *)calloc((size_t)font->capacity, sizeof(bd_glyph));
  font->hash = (int *)calloc((size_t)font->hash_size, sizeof(int));
  if (font->glyphs == NULL || font->hash == NULL) {
    bd_gfx_set_error(gfx, "out of memory for font atlas cache");
    bd_font_destroy(font);
    return NULL;
  }

  font->atlas = bd_gfx_create_gl_texture(
      gfx, font->atlas_size, font->atlas_size, BD_TEXTURE_NEAREST, GL_LUMINANCE,
      GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
  if (font->atlas == 0) {
    bd_font_destroy(font);
    return NULL;
  }
  return font;
}

void bd_font_destroy(bd_font *font) {
  if (font == NULL) {
    return;
  }
  if (font->atlas != 0) {
    glDeleteTextures(1, &font->atlas);
  }
  free(font->glyphs);
  free(font->hash);
  free(font->data);
  free(font);
}

void bd_font_set_fallback(bd_font *font, bd_font *fallback) {
  if (font == NULL || fallback == font) {
    return;
  }
  font->fallback = fallback;
}

int bd_font_pixel_size(const bd_font *font) {
  return font != NULL ? font->pixel_size : 0;
}

int bd_font_ascent(const bd_font *font) {
  return font != NULL ? font->ascent : 0;
}

int bd_font_line_height(const bd_font *font) {
  return font != NULL ? font->line_height : 0;
}

int bd_gfx_draw_text(bd_gfx *gfx, const bd_font *font, int x, int baseline_y,
                     bd_color color, const char *utf8) {
  if (gfx == NULL || font == NULL || utf8 == NULL || !gfx->in_frame) {
    return 0;
  }
  bd_font *base = (bd_font *)font;
  const char *cursor = utf8;
  const char *end = utf8 + strlen(utf8);
  int pen_x = x;
  uint32_t previous = 0;
  bd_font *previous_font = NULL;
  while (cursor < end) {
    uint32_t codepoint = 0;
    if (utf8_decode(&cursor, end, &codepoint) <= 0) {
      break;
    }
    bd_font *resolved = font_for_codepoint(base, codepoint);
    if (resolved == NULL) {
      ++gfx->stats.missing_glyphs;
      pen_x += base->pixel_size / 2;
      previous = 0;
      previous_font = NULL;
      continue;
    }
    if (previous_font == resolved && previous != 0) {
      pen_x += (int)floorf(kern_advance(resolved, previous, codepoint) + 0.5f);
    }
    bd_glyph *glyph = NULL;
    if (get_glyph(resolved, codepoint, &glyph) && glyph->width > 0 &&
        glyph->height > 0) {
      bd_rect dst = bd_rect_make(pen_x + glyph->xoff, baseline_y + glyph->yoff,
                                 glyph->width, glyph->height);
      bd_color colors[4] = {color, color, color, color};
      bd_gfx_batch_add_quad(gfx, resolved->atlas, BD_GFX_MODE_LUMINANCE, 0, 0.0f,
                            dst, -1, glyph->u0, glyph->v0, glyph->u1, glyph->v1,
                            colors);
    }
    pen_x += glyph != NULL ? glyph->advance
                           : (int)floorf((float)resolved->pixel_size * 0.5f);
    previous = codepoint;
    previous_font = resolved;
  }
  return pen_x - x;
}

int bd_gfx_draw_text_ellipsized(bd_gfx *gfx, const bd_font *font, int x,
                                int baseline_y, int max_width, bd_color color,
                                const char *utf8) {
  if (gfx == NULL || font == NULL || utf8 == NULL || !gfx->in_frame ||
      max_width <= 0) {
    return 0;
  }
  int full_width = measure_text(font, utf8);
  if (full_width <= max_width) {
    return bd_gfx_draw_text(gfx, font, x, baseline_y, color, utf8);
  }

  static const char ELLIPSIS[] = "\xE2\x80\xA6";
  int ellipsis_width = measure_text(font, ELLIPSIS);
  if (max_width <= ellipsis_width) {
    return bd_gfx_draw_text(gfx, font, x, baseline_y, color, ELLIPSIS);
  }

  bd_font *base = (bd_font *)font;
  const char *cursor = utf8;
  const char *end = utf8 + strlen(utf8);
  const char *fit_end = utf8;
  int width = 0;
  uint32_t previous = 0;
  bd_font *previous_font = NULL;
  while (cursor < end) {
    const char *before = cursor;
    uint32_t codepoint = 0;
    if (utf8_decode(&cursor, end, &codepoint) <= 0) {
      break;
    }
    bd_font *resolved = font_for_codepoint(base, codepoint);
    int advance = 0;
    if (resolved == NULL) {
      advance = base->pixel_size / 2;
      previous = 0;
      previous_font = NULL;
    } else {
      int raw_advance = 0;
      int left_bearing = 0;
      stbtt_GetCodepointHMetrics(&resolved->info, (int)codepoint, &raw_advance,
                                 &left_bearing);
      advance = (int)floorf((float)raw_advance * resolved->scale + 0.5f);
      if (previous_font == resolved && previous != 0) {
        advance += (int)floorf(kern_advance(resolved, previous, codepoint) + 0.5f);
      }
      previous = codepoint;
      previous_font = resolved;
    }
    if (width + advance + ellipsis_width > max_width) {
      cursor = before;
      break;
    }
    width += advance;
    fit_end = cursor;
  }

  char buffer[1024];
  size_t prefix = (size_t)(fit_end - utf8);
  if (prefix > sizeof(buffer) - sizeof(ELLIPSIS)) {
    prefix = sizeof(buffer) - sizeof(ELLIPSIS);
  }
  memcpy(buffer, utf8, prefix);
  memcpy(buffer + prefix, ELLIPSIS, sizeof(ELLIPSIS));
  return bd_gfx_draw_text(gfx, font, x, baseline_y, color, buffer);
}
