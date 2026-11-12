#include "gfx_internal.h"

#include <stddef.h>
#include <string.h>

static int clamp_int(int value, int minimum, int maximum) {
  if (value < minimum) {
    return minimum;
  }
  if (value > maximum) {
    return maximum;
  }
  return value;
}

static bd_rect clamp_rect(const bd_gfx *gfx, bd_rect rect) {
  int x0 = clamp_int(rect.x, 0, gfx->width);
  int y0 = clamp_int(rect.y, 0, gfx->height);
  int x1 = clamp_int(rect.x + rect.w, 0, gfx->width);
  int y1 = clamp_int(rect.y + rect.h, 0, gfx->height);
  if (x1 < x0) {
    int swap = x0;
    x0 = x1;
    x1 = swap;
  }
  if (y1 < y0) {
    int swap = y0;
    y0 = y1;
    y1 = swap;
  }
  return bd_rect_make(x0, y0, x1 - x0, y1 - y0);
}

static bd_rect align_rect(bd_rect rect) {
  if (rect.w <= 0 || rect.h <= 0) {
    return bd_rect_make(0, 0, 0, 0);
  }
  int x0 = rect.x & ~3;
  int y0 = rect.y & ~3;
  int x1 = (rect.x + rect.w + 3) & ~3;
  int y1 = (rect.y + rect.h + 3) & ~3;
  return bd_rect_make(x0, y0, x1 - x0, y1 - y0);
}

bd_rect bd_rect_union(bd_rect a, bd_rect b) {
  if (a.w <= 0 || a.h <= 0) {
    return b;
  }
  if (b.w <= 0 || b.h <= 0) {
    return a;
  }
  int x0 = a.x < b.x ? a.x : b.x;
  int y0 = a.y < b.y ? a.y : b.y;
  int ax1 = a.x + a.w;
  int bx1 = b.x + b.w;
  int ay1 = a.y + a.h;
  int by1 = b.y + b.h;
  int x1 = ax1 > bx1 ? ax1 : bx1;
  int y1 = ay1 > by1 ? ay1 : by1;
  return bd_rect_make(x0, y0, x1 - x0, y1 - y0);
}

bool bd_rect_overlaps_or_touches(bd_rect a, bd_rect b, int slop) {
  if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0) {
    return false;
  }
  return a.x <= b.x + b.w + slop && b.x <= a.x + a.w + slop &&
         a.y <= b.y + b.h + slop && b.y <= a.y + a.h + slop;
}

bd_rect bd_gfx_batch_current_clip(const bd_gfx *gfx) {
  if (gfx->clip_depth > 0) {
    return gfx->clip_stack[gfx->clip_depth - 1];
  }
  return bd_rect_make(0, 0, gfx->width, gfx->height);
}

void bd_gfx_batch_push_clip(bd_gfx *gfx, bd_rect rect) {
  if (gfx == NULL) {
    return;
  }
  bd_rect current = bd_gfx_batch_current_clip(gfx);
  bd_rect clipped = bd_rect_intersect(current, rect);
  clipped = clamp_rect(gfx, clipped);
  if (gfx->clip_depth < BD_GFX_MAX_CLIP_DEPTH) {
    gfx->clip_stack[gfx->clip_depth++] = clipped;
  }
}

void bd_gfx_batch_pop_clip(bd_gfx *gfx) {
  if (gfx != NULL && gfx->clip_depth > 0) {
    --gfx->clip_depth;
  }
}

void bd_gfx_batch_damage_all(bd_gfx *gfx) {
  if (gfx == NULL) {
    return;
  }
  gfx->damage_all = true;
  gfx->damage_ready = false;
}

void bd_gfx_batch_damage_rect(bd_gfx *gfx, bd_rect rect) {
  if (gfx == NULL) {
    return;
  }
  rect = clamp_rect(gfx, rect);
  if (rect.w <= 0 || rect.h <= 0) {
    return;
  }
  if (gfx->damage_pending_count >= BD_GFX_MAX_PENDING_DAMAGE) {
    gfx->damage_all = true;
    gfx->damage_ready = false;
    return;
  }
  gfx->damage_pending[gfx->damage_pending_count++] = rect;
  gfx->damage_ready = false;
}

bool bd_gfx_batch_has_damage(const bd_gfx *gfx) {
  return gfx != NULL && (gfx->damage_all || gfx->damage_pending_count > 0);
}

static void merge_into(bd_gfx *gfx, bd_rect *rects, int *count, bd_rect rect) {
  rect = clamp_rect(gfx, align_rect(rect));
  if (rect.w <= 0 || rect.h <= 0) {
    return;
  }

  for (;;) {
    bool merged = false;
    for (int i = 0; i < *count; ++i) {
      if (bd_rect_overlaps_or_touches(rects[i], rect, 2)) {
        rect = bd_rect_union(rects[i], rect);
        rects[i] = rects[*count - 1];
        --(*count);
        merged = true;
        break;
      }
    }
    if (!merged) {
      break;
    }
  }

  if (*count < BD_GFX_MAX_PENDING_DAMAGE + BD_GFX_MAX_BLUR_REGIONS + 1) {
    rects[(*count)++] = rect;
  } else {
    gfx->damage_all = true;
  }
}

void bd_gfx_batch_finalize_damage(bd_gfx *gfx) {
  if (gfx == NULL || gfx->damage_ready) {
    return;
  }
  gfx->damage_ready = true;
  gfx->damage_final_count = 0;
  gfx->damage_pixels = 0;

  bd_rect work[BD_GFX_MAX_PENDING_DAMAGE + BD_GFX_MAX_BLUR_REGIONS + 1];
  int count = 0;

  if (gfx->damage_all) {
    work[count++] = bd_rect_make(0, 0, gfx->width, gfx->height);
  } else {
    for (int i = 0; i < gfx->damage_pending_count; ++i) {
      merge_into(gfx, work, &count, gfx->damage_pending[i]);
      if (gfx->damage_all) {
        break;
      }
    }
  }

  /*
   * A floating blur bar samples the scene underneath it. If anything inside
   * the bar changed, the whole bar must be recomposited next time it is
   * drawn; add the previous frame's blur strips as damage dependencies.
   */
  if (!gfx->damage_all) {
    for (int i = 0; i < gfx->blur_active_count; ++i) {
      bool hit = false;
      for (int r = 0; r < count; ++r) {
        if (bd_rect_overlaps_or_touches(work[r], gfx->blur_active[i], 0)) {
          hit = true;
          break;
        }
      }
      if (hit) {
        merge_into(gfx, work, &count, gfx->blur_active[i]);
        if (gfx->damage_all) {
          break;
        }
      }
    }
  }

  bool full = gfx->damage_all || count > BD_GFX_MAX_DAMAGE_RECTS;
  if (!full && count == 0) {
    gfx->stats.damage_rects = 0;
    gfx->stats.damage_pixels = 0;
    gfx->stats.full_redraw = false;
    return;
  }

  if (full) {
    gfx->damage_final[0] = bd_rect_make(0, 0, gfx->width, gfx->height);
    gfx->damage_final_count = 1;
    gfx->damage_pixels = (int64_t)gfx->width * (int64_t)gfx->height;
    gfx->stats.full_redraw = true;
  } else {
    int64_t pixels = 0;
    for (int i = 0; i < count; ++i) {
      gfx->damage_final[i] = work[i];
      pixels += (int64_t)work[i].w * (int64_t)work[i].h;
    }
    gfx->damage_final_count = count;
    gfx->damage_pixels = pixels;
    gfx->stats.full_redraw = false;
  }

  gfx->stats.damage_rects = gfx->damage_final_count;
  gfx->stats.damage_pixels = gfx->damage_pixels;
}

int bd_gfx_batch_damage_rects(const bd_gfx *gfx, bd_rect *out, int capacity) {
  if (gfx == NULL || out == NULL || !gfx->damage_ready || capacity <= 0) {
    return 0;
  }
  int count = gfx->damage_final_count < capacity ? gfx->damage_final_count : capacity;
  for (int i = 0; i < count; ++i) {
    out[i] = gfx->damage_final[i];
  }
  return count;
}

void bd_gfx_batch_begin_frame(bd_gfx *gfx) {
  if (gfx == NULL) {
    return;
  }
  gfx->vertex_count = 0;
  gfx->batch_count = 0;
  gfx->clip_depth = 0;
  gfx->blur_frame_count = 0;
  gfx->damage_ready = false;
  gfx->in_frame = true;
  ++gfx->frame_number;
  memset(&gfx->stats, 0, sizeof(gfx->stats));
}

void bd_gfx_batch_after_frame(bd_gfx *gfx) {
  if (gfx == NULL) {
    return;
  }
  gfx->damage_pending_count = 0;
  gfx->damage_all = false;
  gfx->in_frame = false;
}

void bd_gfx_batch_reset(bd_gfx *gfx) {
  gfx->vertex_count = 0;
  gfx->batch_count = 0;
}

static void put_vertex(bd_gfx *gfx, float x, float y, float u, float v,
                       bd_color color, float cx, float cy, float hx, float hy,
                       float radius) {
  bd_vertex *vertex = &gfx->vertices[gfx->vertex_count++];
  vertex->x = x;
  vertex->y = y;
  vertex->u = u;
  vertex->v = v;
  vertex->r = (uint8_t)(color.r * 255.0f + 0.5f);
  vertex->g = (uint8_t)(color.g * 255.0f + 0.5f);
  vertex->b = (uint8_t)(color.b * 255.0f + 0.5f);
  vertex->a = (uint8_t)(color.a * 255.0f + 0.5f);
  vertex->cx = cx;
  vertex->cy = cy;
  vertex->hx = hx;
  vertex->hy = hy;
  vertex->radius = radius;
}

static bool batch_matches(const bd_batch *batch, GLuint texture, int mode, int flip,
                          float stroke, bd_rect clip) {
  return batch->texture == texture && batch->mode == mode && batch->flip == flip &&
         batch->stroke == stroke && batch->clip.x == clip.x &&
         batch->clip.y == clip.y && batch->clip.w == clip.w &&
         batch->clip.h == clip.h;
}

void bd_gfx_batch_add_quad(bd_gfx *gfx, GLuint texture, int mode, int flip,
                           float stroke, bd_rect rect, int radius, float u0,
                           float v0, float u1, float v1,
                           const bd_color colors[4]) {
  if (gfx == NULL || !gfx->in_frame || rect.w <= 0 || rect.h <= 0) {
    return;
  }
  bd_gfx_batch_finalize_damage(gfx);
  if (gfx->damage_final_count == 0) {
    return;
  }

  bd_rect clip = bd_gfx_batch_current_clip(gfx);
  bd_rect clipped = bd_rect_intersect(rect, clip);
  if (clipped.w <= 0 || clipped.h <= 0) {
    return;
  }

  if (gfx->batch_count >= BD_GFX_MAX_BATCHES ||
      gfx->vertex_count + 4 > BD_GFX_MAX_QUADS * 4) {
    bd_gfx_batch_flush(gfx);
  }

  bd_batch *batch = gfx->batch_count > 0 ? &gfx->batches[gfx->batch_count - 1] : NULL;
  if (batch == NULL || !batch_matches(batch, texture, mode, flip, stroke, clip)) {
    if (gfx->batch_count >= BD_GFX_MAX_BATCHES) {
      return;
    }
    batch = &gfx->batches[gfx->batch_count++];
    batch->texture = texture;
    batch->mode = mode;
    batch->flip = flip;
    batch->stroke = stroke;
    batch->clip = clip;
    batch->first_vertex = gfx->vertex_count;
    batch->quad_count = 0;
  }

  float cx = (float)rect.x + (float)rect.w * 0.5f;
  float cy = (float)rect.y + (float)rect.h * 0.5f;
  float hx = (float)rect.w * 0.5f;
  float hy = (float)rect.h * 0.5f;
  float x0 = (float)rect.x;
  float y0 = (float)rect.y;
  float x1 = (float)(rect.x + rect.w);
  float y1 = (float)(rect.y + rect.h);

  put_vertex(gfx, x0, y0, u0, v0, colors[0], cx, cy, hx, hy, (float)radius);
  put_vertex(gfx, x1, y0, u1, v0, colors[1], cx, cy, hx, hy, (float)radius);
  put_vertex(gfx, x0, y1, u0, v1, colors[2], cx, cy, hx, hy, (float)radius);
  put_vertex(gfx, x1, y1, u1, v1, colors[3], cx, cy, hx, hy, (float)radius);
  ++batch->quad_count;
}

void bd_gfx_batch_add_gradient3(bd_gfx *gfx, bd_rect rect, int radius,
                                bd_color start, bd_color middle, bd_color end,
                                bd_gradient_axis axis) {
  if (gfx == NULL || !gfx->in_frame || rect.w <= 0 || rect.h <= 0) {
    return;
  }
  bd_gfx_batch_finalize_damage(gfx);
  if (gfx->damage_final_count == 0) {
    return;
  }
  bd_rect clip = bd_gfx_batch_current_clip(gfx);
  bd_rect clipped = bd_rect_intersect(rect, clip);
  if (clipped.w <= 0 || clipped.h <= 0) {
    return;
  }
  if (gfx->batch_count >= BD_GFX_MAX_BATCHES ||
      gfx->vertex_count + 4 > BD_GFX_MAX_QUADS * 4) {
    bd_gfx_batch_flush(gfx);
  }
  if (gfx->batch_count >= BD_GFX_MAX_BATCHES) {
    return;
  }

  bd_batch *batch = &gfx->batches[gfx->batch_count++];
  batch->texture = gfx->white_texture;
  batch->mode = BD_GFX_MODE_GRADIENT3;
  batch->flip = 0;
  batch->stroke = 0.0f;
  batch->clip = clip;
  batch->gradient_rect = rect;
  batch->gradient[0] = start;
  batch->gradient[1] = middle;
  batch->gradient[2] = end;
  batch->gradient_axis = (int)axis;
  batch->first_vertex = gfx->vertex_count;
  batch->quad_count = 0;

  float cx = (float)rect.x + (float)rect.w * 0.5f;
  float cy = (float)rect.y + (float)rect.h * 0.5f;
  float hx = (float)rect.w * 0.5f;
  float hy = (float)rect.h * 0.5f;
  float x0 = (float)rect.x;
  float y0 = (float)rect.y;
  float x1 = (float)(rect.x + rect.w);
  float y1 = (float)(rect.y + rect.h);
  put_vertex(gfx, x0, y0, 0.0f, 0.0f, start, cx, cy, hx, hy, (float)radius);
  put_vertex(gfx, x1, y0, 0.0f, 0.0f, start, cx, cy, hx, hy, (float)radius);
  put_vertex(gfx, x0, y1, 0.0f, 0.0f, start, cx, cy, hx, hy, (float)radius);
  put_vertex(gfx, x1, y1, 0.0f, 0.0f, start, cx, cy, hx, hy, (float)radius);
  ++batch->quad_count;
}

void bd_gfx_batch_flush(bd_gfx *gfx) {
  if (gfx == NULL) {
    return;
  }
  bd_gfx_batch_finalize_damage(gfx);
  if (gfx->batch_count == 0 || gfx->vertex_count == 0 ||
      gfx->damage_final_count == 0) {
    bd_gfx_batch_reset(gfx);
    return;
  }

  glUseProgram(gfx->main_program);
  glUniform2f(gfx->main_viewport, (float)gfx->width, (float)gfx->height);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(gfx->main_tex, 0);

  glBindBuffer(GL_ARRAY_BUFFER, gfx->vbo);
  glBufferData(GL_ARRAY_BUFFER,
               (GLsizeiptr)(gfx->vertex_count * (int)sizeof(bd_vertex)),
               gfx->vertices, GL_STREAM_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gfx->ibo);

  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glEnableVertexAttribArray(3);
  glEnableVertexAttribArray(4);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_SCISSOR_TEST);

  const GLsizei stride = (GLsizei)sizeof(bd_vertex);
  for (int b = 0; b < gfx->batch_count; ++b) {
    const bd_batch *batch = &gfx->batches[b];
    const size_t base = (size_t)batch->first_vertex * sizeof(bd_vertex);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(base + offsetof(bd_vertex, x)));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(base + offsetof(bd_vertex, u)));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                          (const void *)(base + offsetof(bd_vertex, r)));
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(base + offsetof(bd_vertex, cx)));
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(base + offsetof(bd_vertex, radius)));

    glBindTexture(GL_TEXTURE_2D, batch->texture);
    glUniform1i(gfx->main_mode, batch->mode);
    glUniform1i(gfx->main_flip, batch->flip);
    glUniform1f(gfx->main_stroke, batch->stroke);
    if (batch->mode == BD_GFX_MODE_GRADIENT3) {
      glUniform4f(gfx->main_grad0, batch->gradient[0].r, batch->gradient[0].g,
                  batch->gradient[0].b, batch->gradient[0].a);
      glUniform4f(gfx->main_grad1, batch->gradient[1].r, batch->gradient[1].g,
                  batch->gradient[1].b, batch->gradient[1].a);
      glUniform4f(gfx->main_grad2, batch->gradient[2].r, batch->gradient[2].g,
                  batch->gradient[2].b, batch->gradient[2].a);
      glUniform4f(gfx->main_grad_rect, (float)batch->gradient_rect.x,
                  (float)batch->gradient_rect.y, (float)batch->gradient_rect.w,
                  (float)batch->gradient_rect.h);
      glUniform1i(gfx->main_grad_axis, batch->gradient_axis);
    }

    for (int d = 0; d < gfx->damage_final_count; ++d) {
      bd_rect scissor = bd_rect_intersect(gfx->damage_final[d], batch->clip);
      if (scissor.w <= 0 || scissor.h <= 0) {
        continue;
      }
      glScissor(scissor.x, gfx->height - (scissor.y + scissor.h), scissor.w,
                scissor.h);
      glDrawElements(GL_TRIANGLES, batch->quad_count * 6, GL_UNSIGNED_SHORT,
                     (const void *)0);
      ++gfx->stats.draw_calls;
    }
  }

  glDisable(GL_SCISSOR_TEST);
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
  glDisableVertexAttribArray(2);
  glDisableVertexAttribArray(3);
  glDisableVertexAttribArray(4);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

  gfx->stats.batches += gfx->batch_count;
  gfx->stats.vertices += gfx->vertex_count;
  bd_gfx_batch_reset(gfx);
}
