#include "gfx_internal.h"

#include <SDL.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"

#define BD_IMAGE_MAX_RESULTS 8
#define BD_IMAGE_MAX_PATH 1024

static char *duplicate_path(const char *path) {
  size_t length = strlen(path);
  if (length == 0 || length >= BD_IMAGE_MAX_PATH) {
    return NULL;
  }
  char *copy = (char *)malloc(length + 1);
  if (copy != NULL) {
    memcpy(copy, path, length + 1);
  }
  return copy;
}

static bd_image_entry *find_entry(bd_gfx *gfx, const char *path, int width,
                                  int height) {
  for (bd_image_entry *entry = gfx->image_entries; entry != NULL; entry = entry->next) {
    if (entry->width == width && entry->height == height &&
        strcmp(entry->path, path) == 0) {
      return entry;
    }
  }
  return NULL;
}

static bd_image_entry *find_entry_by_id(bd_gfx *gfx, uint64_t id) {
  for (bd_image_entry *entry = gfx->image_entries; entry != NULL; entry = entry->next) {
    if (entry->id == id) {
      return entry;
    }
  }
  return NULL;
}

static bd_image_entry *create_entry(bd_gfx *gfx, const char *path, int width,
                                    int height) {
  bd_image_entry *entry = (bd_image_entry *)calloc(1, sizeof(*entry));
  if (entry == NULL) {
    return NULL;
  }
  entry->path = duplicate_path(path);
  entry->texture = (bd_texture *)calloc(1, sizeof(*entry->texture));
  if (entry->path == NULL || entry->texture == NULL) {
    free(entry->path);
    free(entry->texture);
    free(entry);
    return NULL;
  }
  entry->width = width;
  entry->height = height;
  entry->id = gfx->image_next_id++;
  entry->state = BD_IMAGE_PENDING;
  entry->texture->cache_managed = true;
  entry->next = gfx->image_entries;
  gfx->image_entries = entry;
  return entry;
}

static void insert_job_locked(bd_gfx *gfx, bd_image_job *job) {
  bd_image_job **link = &gfx->image_jobs_head;
  while (*link != NULL && (*link)->distance <= job->distance) {
    link = &(*link)->next;
  }
  job->next = *link;
  *link = job;
  gfx->image_jobs_tail = NULL;
  bd_image_job *tail = gfx->image_jobs_head;
  while (tail != NULL) {
    gfx->image_jobs_tail = tail;
    tail = tail->next;
  }
}

static void remove_job_locked(bd_gfx *gfx, bd_image_job *job) {
  bd_image_job **link = &gfx->image_jobs_head;
  while (*link != NULL && *link != job) {
    link = &(*link)->next;
  }
  if (*link == job) {
    *link = job->next;
  }
  gfx->image_jobs_tail = NULL;
  bd_image_job *tail = gfx->image_jobs_head;
  while (tail != NULL) {
    gfx->image_jobs_tail = tail;
    tail = tail->next;
  }
}

static void enqueue_job_locked(bd_gfx *gfx, bd_image_entry *entry) {
  if (entry->job_queued || entry->in_flight) {
    return;
  }
  bd_image_job *job = (bd_image_job *)calloc(1, sizeof(*job));
  if (job == NULL) {
    entry->state = BD_IMAGE_FAILED;
    return;
  }
  job->path = duplicate_path(entry->path);
  if (job->path == NULL) {
    free(job);
    entry->state = BD_IMAGE_FAILED;
    return;
  }
  job->width = entry->width;
  job->height = entry->height;
  job->distance = entry->distance;
  job->id = entry->id;
  job->entry = entry;
  entry->queued_job = job;
  entry->job_queued = true;
  entry->state = BD_IMAGE_PENDING;
  insert_job_locked(gfx, job);
  SDL_CondSignal((SDL_cond *)gfx->image_work_cond);
}

static void reprioritize_job_locked(bd_gfx *gfx, bd_image_entry *entry, int distance) {
  if (!entry->job_queued || entry->queued_job == NULL) {
    return;
  }
  if (entry->queued_job->distance == distance) {
    return;
  }
  remove_job_locked(gfx, entry->queued_job);
  entry->queued_job->distance = distance;
  insert_job_locked(gfx, entry->queued_job);
}

static uint8_t *resize_rgba(const uint8_t *source, int source_width,
                            int source_height, int target_width, int target_height) {
  if (source_width == target_width && source_height == target_height) {
    size_t bytes = (size_t)target_width * (size_t)target_height * 4u;
    uint8_t *copy = (uint8_t *)malloc(bytes);
    if (copy != NULL) {
      memcpy(copy, source, bytes);
    }
    return copy;
  }

  size_t bytes = (size_t)target_width * (size_t)target_height * 4u;
  uint8_t *target = (uint8_t *)malloc(bytes);
  if (target == NULL) {
    return NULL;
  }
  for (int y = 0; y < target_height; ++y) {
    float sy = ((float)y + 0.5f) * (float)source_height / (float)target_height - 0.5f;
    if (sy < 0.0f) {
      sy = 0.0f;
    }
    int y0 = (int)sy;
    int y1 = y0 + 1 < source_height ? y0 + 1 : y0;
    float fy = sy - (float)y0;
    for (int x = 0; x < target_width; ++x) {
      float sx = ((float)x + 0.5f) * (float)source_width / (float)target_width - 0.5f;
      if (sx < 0.0f) {
        sx = 0.0f;
      }
      int x0 = (int)sx;
      int x1 = x0 + 1 < source_width ? x0 + 1 : x0;
      float fx = sx - (float)x0;
      const uint8_t *p00 = source + ((size_t)y0 * source_width + x0) * 4u;
      const uint8_t *p10 = source + ((size_t)y0 * source_width + x1) * 4u;
      const uint8_t *p01 = source + ((size_t)y1 * source_width + x0) * 4u;
      const uint8_t *p11 = source + ((size_t)y1 * source_width + x1) * 4u;
      uint8_t *out = target + ((size_t)y * target_width + x) * 4u;
      for (int channel = 0; channel < 4; ++channel) {
        float top = (float)p00[channel] + ((float)p10[channel] - (float)p00[channel]) * fx;
        float bottom = (float)p01[channel] + ((float)p11[channel] - (float)p01[channel]) * fx;
        float value = top + (bottom - top) * fy;
        out[channel] = (uint8_t)(value + 0.5f);
      }
    }
  }
  return target;
}

static bd_image_result *decode_job(const bd_image_job *job) {
  bd_image_result *result = (bd_image_result *)calloc(1, sizeof(*result));
  if (result == NULL) {
    return NULL;
  }
  result->id = job->id;
  result->width = job->width;
  result->height = job->height;
  result->opaque = false;

  int width = 0;
  int height = 0;
  int channels = 0;
  uint8_t *pixels = stbi_load(job->path, &width, &height, &channels, 4);
  if (pixels == NULL || width <= 0 || height <= 0) {
    stbi_image_free(pixels);
    return result;
  }

  uint8_t *resized = resize_rgba(pixels, width, height, job->width, job->height);
  stbi_image_free(pixels);
  if (resized == NULL) {
    return result;
  }

  result->pixels = resized;
  result->opaque = true;
  size_t count = (size_t)job->width * (size_t)job->height;
  for (size_t i = 0; i < count; ++i) {
    if (resized[i * 4u + 3u] != 255) {
      result->opaque = false;
      break;
    }
  }
  return result;
}

static int image_worker(void *userdata) {
  bd_gfx *gfx = (bd_gfx *)userdata;
  for (;;) {
    SDL_LockMutex((SDL_mutex *)gfx->image_mutex);
    while (!gfx->image_quit && gfx->image_jobs_head == NULL) {
      SDL_CondWait((SDL_cond *)gfx->image_work_cond,
                   (SDL_mutex *)gfx->image_mutex);
    }
    if (gfx->image_quit) {
      SDL_UnlockMutex((SDL_mutex *)gfx->image_mutex);
      break;
    }
    bd_image_job *job = gfx->image_jobs_head;
    gfx->image_jobs_head = job->next;
    if (gfx->image_jobs_head == NULL) {
      gfx->image_jobs_tail = NULL;
    }
    job->entry->job_queued = false;
    job->entry->queued_job = NULL;
    job->entry->in_flight = true;
    SDL_UnlockMutex((SDL_mutex *)gfx->image_mutex);

    bd_image_result *result = decode_job(job);

    SDL_LockMutex((SDL_mutex *)gfx->image_mutex);
    job->entry->in_flight = false;
    if (!gfx->image_quit && job->entry->wanted) {
      while (!gfx->image_quit && gfx->image_result_count >= BD_IMAGE_MAX_RESULTS) {
        SDL_CondWait((SDL_cond *)gfx->image_done_cond,
                     (SDL_mutex *)gfx->image_mutex);
      }
      if (!gfx->image_quit && job->entry->wanted && result != NULL) {
        if (gfx->image_results_tail == NULL) {
          gfx->image_results_head = result;
        } else {
          gfx->image_results_tail->next = result;
        }
        gfx->image_results_tail = result;
        ++gfx->image_result_count;
        result = NULL;
      }
    }
    if (result != NULL) {
      free(result->pixels);
      free(result);
    }
    free(job->path);
    free(job);
    SDL_CondBroadcast((SDL_cond *)gfx->image_done_cond);
    SDL_UnlockMutex((SDL_mutex *)gfx->image_mutex);
  }
  return 0;
}

bool bd_gfx_image_init(bd_gfx *gfx) {
  gfx->image_mutex = SDL_CreateMutex();
  gfx->image_work_cond = SDL_CreateCond();
  gfx->image_done_cond = SDL_CreateCond();
  if (gfx->image_mutex == NULL || gfx->image_work_cond == NULL ||
      gfx->image_done_cond == NULL) {
    bd_gfx_set_error(gfx, "failed to create image worker synchronization");
    return false;
  }
  gfx->image_thread = SDL_CreateThread(image_worker, "bd-image", gfx);
  if (gfx->image_thread == NULL) {
    bd_gfx_set_error(gfx, "failed to create image worker thread");
    return false;
  }
  return true;
}

void bd_gfx_image_shutdown(bd_gfx *gfx) {
  if (gfx == NULL) {
    return;
  }
  if (gfx->image_mutex != NULL) {
    SDL_LockMutex((SDL_mutex *)gfx->image_mutex);
    gfx->image_quit = true;
    SDL_CondBroadcast((SDL_cond *)gfx->image_work_cond);
    SDL_CondBroadcast((SDL_cond *)gfx->image_done_cond);
    SDL_UnlockMutex((SDL_mutex *)gfx->image_mutex);
  }
  if (gfx->image_thread != NULL) {
    SDL_WaitThread((SDL_Thread *)gfx->image_thread, NULL);
    gfx->image_thread = NULL;
  }

  bd_image_job *job = gfx->image_jobs_head;
  while (job != NULL) {
    bd_image_job *next = job->next;
    free(job->path);
    free(job);
    job = next;
  }
  gfx->image_jobs_head = NULL;
  gfx->image_jobs_tail = NULL;

  bd_image_result *result = gfx->image_results_head;
  while (result != NULL) {
    bd_image_result *next = result->next;
    free(result->pixels);
    free(result);
    result = next;
  }
  gfx->image_results_head = NULL;
  gfx->image_results_tail = NULL;

  bd_image_entry *entry = gfx->image_entries;
  while (entry != NULL) {
    bd_image_entry *next = entry->next;
    if (entry->texture != NULL) {
      if (entry->texture->id != 0) {
        glDeleteTextures(1, &entry->texture->id);
      }
      free(entry->texture);
    }
    free(entry->path);
    free(entry);
    entry = next;
  }
  gfx->image_entries = NULL;

  if (gfx->image_work_cond != NULL) {
    SDL_DestroyCond((SDL_cond *)gfx->image_work_cond);
    gfx->image_work_cond = NULL;
  }
  if (gfx->image_done_cond != NULL) {
    SDL_DestroyCond((SDL_cond *)gfx->image_done_cond);
    gfx->image_done_cond = NULL;
  }
  if (gfx->image_mutex != NULL) {
    SDL_DestroyMutex((SDL_mutex *)gfx->image_mutex);
    gfx->image_mutex = NULL;
  }
}

void bd_gfx_image_begin_set(bd_gfx *gfx) {
  if (gfx == NULL || gfx->image_mutex == NULL) {
    return;
  }
  SDL_LockMutex((SDL_mutex *)gfx->image_mutex);
  ++gfx->image_set_epoch;
  for (bd_image_entry *entry = gfx->image_entries; entry != NULL; entry = entry->next) {
    entry->wanted = false;
  }
  SDL_UnlockMutex((SDL_mutex *)gfx->image_mutex);
}

static bd_image_state request_image(bd_gfx *gfx, const char *path, int width,
                                    int height, int distance, bd_texture **out) {
  if (out != NULL) {
    *out = NULL;
  }
  if (gfx == NULL || path == NULL || width <= 0 || height <= 0 ||
      gfx->image_mutex == NULL) {
    return BD_IMAGE_FAILED;
  }

  SDL_LockMutex((SDL_mutex *)gfx->image_mutex);
  bd_image_entry *entry = find_entry(gfx, path, width, height);
  if (entry == NULL) {
    entry = create_entry(gfx, path, width, height);
    if (entry == NULL) {
      SDL_UnlockMutex((SDL_mutex *)gfx->image_mutex);
      bd_gfx_set_error(gfx, "out of memory for image cache entry");
      return BD_IMAGE_FAILED;
    }
  }
  entry->wanted = true;
  entry->last_used = gfx->frame_number;
  entry->distance = distance;

  if (entry->state == BD_IMAGE_FAILED) {
    bd_image_state state = entry->state;
    SDL_UnlockMutex((SDL_mutex *)gfx->image_mutex);
    return state;
  }
  if (entry->state == BD_IMAGE_READY && entry->texture->id == 0) {
    entry->state = BD_IMAGE_PENDING;
  }
  if (entry->state == BD_IMAGE_READY) {
    if (out != NULL) {
      *out = entry->texture;
    }
    SDL_UnlockMutex((SDL_mutex *)gfx->image_mutex);
    return BD_IMAGE_READY;
  }
  if (entry->job_queued) {
    reprioritize_job_locked(gfx, entry, distance);
  } else if (!entry->in_flight) {
    enqueue_job_locked(gfx, entry);
  }
  bd_image_state state = entry->state;
  SDL_UnlockMutex((SDL_mutex *)gfx->image_mutex);
  return state;
}

bd_image_state bd_gfx_image_texture(bd_gfx *gfx, const char *path, int width,
                                    int height, bd_texture **out) {
  return request_image(gfx, path, width, height, 0, out);
}

bd_image_state bd_gfx_image_request(bd_gfx *gfx, const char *path, int width,
                                    int height, int distance, bd_texture **out) {
  return request_image(gfx, path, width, height, distance, out);
}

void bd_gfx_image_end_set(bd_gfx *gfx) {
  if (gfx == NULL || gfx->image_mutex == NULL) {
    return;
  }
  SDL_LockMutex((SDL_mutex *)gfx->image_mutex);
  bd_image_job **link = &gfx->image_jobs_head;
  while (*link != NULL) {
    bd_image_job *job = *link;
    if (!job->entry->wanted) {
      *link = job->next;
      job->entry->job_queued = false;
      job->entry->queued_job = NULL;
      if (job->entry->state != BD_IMAGE_READY) {
        job->entry->state = BD_IMAGE_PENDING;
      }
      free(job->path);
      free(job);
    } else {
      link = &job->next;
    }
  }
  gfx->image_jobs_tail = NULL;
  bd_image_job *tail = gfx->image_jobs_head;
  while (tail != NULL) {
    gfx->image_jobs_tail = tail;
    tail = tail->next;
  }
  SDL_UnlockMutex((SDL_mutex *)gfx->image_mutex);
}

void bd_gfx_image_poll(bd_gfx *gfx) {
  if (gfx == NULL || gfx->image_mutex == NULL) {
    return;
  }
  SDL_LockMutex((SDL_mutex *)gfx->image_mutex);
  bd_image_result *results = gfx->image_results_head;
  gfx->image_results_head = NULL;
  gfx->image_results_tail = NULL;
  gfx->image_result_count = 0;
  SDL_CondBroadcast((SDL_cond *)gfx->image_done_cond);
  SDL_UnlockMutex((SDL_mutex *)gfx->image_mutex);

  while (results != NULL) {
    bd_image_result *next = results->next;

    SDL_LockMutex((SDL_mutex *)gfx->image_mutex);
    bd_image_entry *entry = find_entry_by_id(gfx, results->id);
    bool wanted = entry != NULL && entry->wanted;
    SDL_UnlockMutex((SDL_mutex *)gfx->image_mutex);

    if (entry != NULL && wanted) {
      if (entry->texture->id != 0) {
        glDeleteTextures(1, &entry->texture->id);
        gfx->image_resident -= entry->bytes;
      }
      bd_image_state state = BD_IMAGE_FAILED;
      if (results->pixels != NULL) {
        bd_texture *texture = bd_gfx_texture_create_internal(
            gfx, results->width, results->height, BD_TEXTURE_LINEAR, GL_RGBA,
            GL_RGBA, GL_UNSIGNED_BYTE, results->pixels, true, results->opaque);
        if (texture != NULL) {
          *entry->texture = *texture;
          free(texture);
          entry->texture->cache_managed = true;
          entry->bytes = (size_t)results->width * (size_t)results->height * 4u;
          gfx->image_resident += entry->bytes;
          state = BD_IMAGE_READY;
        }
      }
      entry->state = state;
    } else if (entry != NULL && entry->state != BD_IMAGE_READY) {
      entry->state = BD_IMAGE_PENDING;
    }

    free(results->pixels);
    free(results);
    results = next;
  }
}

void bd_gfx_image_end_frame(bd_gfx *gfx) {
  if (gfx == NULL) {
    return;
  }
  while (gfx->image_resident > gfx->image_budget) {
    bd_image_entry *victim = NULL;
    for (bd_image_entry *entry = gfx->image_entries; entry != NULL; entry = entry->next) {
      if (entry->texture->id == 0) {
        continue;
      }
      if (victim == NULL || (!entry->wanted && victim->wanted) ||
          (entry->wanted == victim->wanted && entry->last_used < victim->last_used)) {
        victim = entry;
      }
    }
    if (victim == NULL) {
      break;
    }
    glDeleteTextures(1, &victim->texture->id);
    victim->texture->id = 0;
    gfx->image_resident -= victim->bytes;
    victim->bytes = 0;
    victim->state = BD_IMAGE_PENDING;
  }
}

void bd_gfx_image_cache_set_budget(bd_gfx *gfx, size_t bytes) {
  if (gfx != NULL) {
    gfx->image_budget = bytes;
  }
}

bd_image_cache_stats bd_gfx_image_cache_get_stats(const bd_gfx *gfx) {
  bd_image_cache_stats stats;
  memset(&stats, 0, sizeof(stats));
  if (gfx == NULL) {
    return stats;
  }
  bd_gfx *mutable_gfx = (bd_gfx *)gfx;
  if (gfx->image_mutex != NULL) {
    SDL_LockMutex((SDL_mutex *)gfx->image_mutex);
  }
  stats.budget_bytes = gfx->image_budget;
  stats.resident_bytes = gfx->image_resident;
  for (const bd_image_entry *entry = gfx->image_entries; entry != NULL;
       entry = entry->next) {
    if (entry->texture->id != 0) {
      ++stats.textures;
    }
    if (entry->job_queued || entry->in_flight) {
      ++stats.pending;
    }
    if (entry->state == BD_IMAGE_FAILED) {
      ++stats.failed;
    }
  }
  /* Decoded results still need one begin_frame to upload their texture. */
  stats.pending += gfx->image_result_count;
  if (mutable_gfx->image_mutex != NULL) {
    SDL_UnlockMutex((SDL_mutex *)mutable_gfx->image_mutex);
  }
  return stats;
}
