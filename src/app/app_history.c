#define _POSIX_C_SOURCE 200809L

#include "brickdesk/app_history.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bd_app_history {
  bd_app_history_entry entries[BD_APP_HISTORY_MAX];
  size_t count;
};

static bool valid_id(const char *id) {
  return id != NULL && id[0] != '\0' && strlen(id) < BD_APP_HISTORY_ID_MAX &&
         strchr(id, '\t') == NULL && strchr(id, '\n') == NULL;
}

static int find_index(const bd_app_history *history, const char *id) {
  if (history == NULL || !valid_id(id)) {
    return -1;
  }
  for (size_t i = 0; i < history->count; ++i) {
    if (strcmp(history->entries[i].id, id) == 0) {
      return (int)i;
    }
  }
  return -1;
}

bd_app_history *bd_app_history_create(void) {
  return (bd_app_history *)calloc(1, sizeof(bd_app_history));
}

void bd_app_history_destroy(bd_app_history *history) {
  free(history);
}

static bool add_entry(bd_app_history *history, const char *id) {
  if (history == NULL || !valid_id(id)) {
    return false;
  }
  if (history->count >= BD_APP_HISTORY_MAX) {
    memmove(&history->entries[0], &history->entries[1],
            sizeof(history->entries[0]) * (BD_APP_HISTORY_MAX - 1));
    --history->count;
  }
  bd_app_history_entry *entry = &history->entries[history->count++];
  memset(entry, 0, sizeof(*entry));
  snprintf(entry->id, sizeof(entry->id), "%s", id);
  return true;
}

bool bd_app_history_load(bd_app_history *history, const char *path) {
  if (history == NULL || path == NULL) {
    return false;
  }
  history->count = 0;
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  char line[256];
  bool ok = true;
  while (fgets(line, sizeof(line), file) != NULL) {
    if (line[0] == '#' || isspace((unsigned char)line[0])) {
      continue;
    }
    char *save = NULL;
    char *kind = strtok_r(line, "\t\r\n", &save);
    if (kind == NULL) {
      continue;
    }
    if (strcmp(kind, "used") == 0) {
      const char *epoch_text = strtok_r(NULL, "\t\r\n", &save);
      const char *id = strtok_r(NULL, "\t\r\n", &save);
      if (epoch_text == NULL || id == NULL || !valid_id(id)) {
        ok = false;
        continue;
      }
      char *end = NULL;
      long long used = strtoll(epoch_text, &end, 10);
      if (end == epoch_text || used < 0) {
        ok = false;
        continue;
      }
      int index = find_index(history, id);
      if (index < 0) {
        if (!add_entry(history, id)) {
          ok = false;
          continue;
        }
        index = (int)history->count - 1;
      }
      history->entries[index].last_used_s = (int64_t)used;
    } else if (strcmp(kind, "fav") == 0) {
      const char *id = strtok_r(NULL, "\t\r\n", &save);
      if (id == NULL || !valid_id(id)) {
        ok = false;
        continue;
      }
      int index = find_index(history, id);
      if (index < 0) {
        if (!add_entry(history, id)) {
          ok = false;
          continue;
        }
        index = (int)history->count - 1;
      }
      history->entries[index].favorite = true;
    } else {
      ok = false;
    }
  }
  if (ferror(file)) {
    ok = false;
  }
  fclose(file);
  return ok;
}

bool bd_app_history_save(const bd_app_history *history, const char *path) {
  if (history == NULL || path == NULL) {
    return false;
  }
  char temp[1024];
  int written = snprintf(temp, sizeof(temp), "%s.tmp", path);
  if (written <= 0 || (size_t)written >= sizeof(temp)) {
    return false;
  }
  FILE *file = fopen(temp, "w");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs("# brickdesk app history v1\n", file) >= 0;
  for (size_t i = 0; ok && i < history->count; ++i) {
    const bd_app_history_entry *entry = &history->entries[i];
    ok = fprintf(file, "used\t%lld\t%s\n", (long long)entry->last_used_s,
                 entry->id) > 0;
    if (ok && entry->favorite) {
      ok = fprintf(file, "fav\t%s\n", entry->id) > 0;
    }
  }
  if (fclose(file) != 0) {
    ok = false;
  }
  if (!ok || rename(temp, path) != 0) {
    remove(temp);
    return false;
  }
  return true;
}

bool bd_app_history_record(bd_app_history *history, const char *id,
                           int64_t epoch_s) {
  if (history == NULL || !valid_id(id)) {
    return false;
  }
  int index = find_index(history, id);
  if (index >= 0) {
    bd_app_history_entry entry = history->entries[index];
    memmove(&history->entries[index], &history->entries[index + 1],
            sizeof(history->entries[0]) *
                (history->count - (size_t)index - 1));
    --history->count;
    entry.last_used_s = epoch_s;
    history->entries[history->count++] = entry;
    return true;
  }
  if (!add_entry(history, id)) {
    return false;
  }
  history->entries[history->count - 1].last_used_s = epoch_s;
  return true;
}

bool bd_app_history_set_favorite(bd_app_history *history, const char *id,
                                 bool favorite) {
  if (history == NULL || !valid_id(id)) {
    return false;
  }
  int index = find_index(history, id);
  if (index < 0) {
    if (!add_entry(history, id)) {
      return false;
    }
    index = (int)history->count - 1;
  }
  history->entries[index].favorite = favorite;
  return true;
}

bool bd_app_history_favorite(const bd_app_history *history, const char *id) {
  int index = find_index(history, id);
  return index >= 0 && history->entries[index].favorite;
}

size_t bd_app_history_count(const bd_app_history *history) {
  return history != NULL ? history->count : 0;
}

const bd_app_history_entry *bd_app_history_at(const bd_app_history *history,
                                              size_t index) {
  return history != NULL && index < history->count ? &history->entries[index]
                                                   : NULL;
}

const char *bd_app_history_most_recent(const bd_app_history *history) {
  if (history == NULL || history->count == 0) {
    return NULL;
  }
  const char *best = NULL;
  int64_t best_time = -1;
  for (size_t i = 0; i < history->count; ++i) {
    if (history->entries[i].last_used_s >= best_time) {
      best_time = history->entries[i].last_used_s;
      best = history->entries[i].id;
    }
  }
  return best;
}
