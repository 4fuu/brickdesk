#define _POSIX_C_SOURCE 200809L

#include "brickdesk/battery_history.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bd_battery_history {
  bd_battery_point points[BD_BATTERY_HISTORY_MAX];
  size_t head;
  size_t count;
};

bd_battery_history *bd_battery_history_create(void) {
  return (bd_battery_history *)calloc(1, sizeof(bd_battery_history));
}

void bd_battery_history_destroy(bd_battery_history *history) {
  free(history);
}

static void clear(bd_battery_history *history) {
  history->head = 0;
  history->count = 0;
}

bool bd_battery_history_load(bd_battery_history *history, const char *path) {
  if (history == NULL || path == NULL) {
    return false;
  }
  clear(history);
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  char line[128];
  bool ok = true;
  while (fgets(line, sizeof(line), file) != NULL) {
    if (line[0] == '#' || isspace((unsigned char)line[0])) {
      continue;
    }
    char *end = NULL;
    errno = 0;
    long long epoch = strtoll(line, &end, 10);
    if (errno != 0 || end == line || *end != ',') {
      ok = false;
      continue;
    }
    const char *percent_text = end + 1;
    long percent = strtol(percent_text, &end, 10);
    if (end == percent_text || percent < 0 || percent > 100 || *end != ',') {
      ok = false;
      continue;
    }
    const char *charging_text = end + 1;
    long charging = strtol(charging_text, &end, 10);
    if (end == charging_text || epoch < 0) {
      ok = false;
      continue;
    }
    bd_battery_history_record(history, (int64_t)epoch, (int)percent,
                              charging != 0);
  }
  if (ferror(file)) {
    ok = false;
  }
  fclose(file);
  return ok;
}

bool bd_battery_history_save(const bd_battery_history *history,
                             const char *path) {
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
  bool ok = fputs("# brickdesk battery history v1\n", file) >= 0;
  for (size_t i = 0; ok && i < history->count; ++i) {
    const bd_battery_point *point =
        &history->points[(history->head + i) % BD_BATTERY_HISTORY_MAX];
    ok = fprintf(file, "%lld,%d,%d\n", (long long)point->epoch_s,
                 point->percent, point->charging ? 1 : 0) > 0;
  }
  if (fclose(file) != 0) {
    ok = false;
  }
  if (!ok) {
    remove(temp);
    return false;
  }
  if (rename(temp, path) != 0) {
    remove(temp);
    return false;
  }
  return true;
}

bool bd_battery_history_record(bd_battery_history *history, int64_t epoch_s,
                               int percent, bool charging) {
  if (history == NULL || percent < 0 || percent > 100 || epoch_s <= 0) {
    return false;
  }
  if (history->count > 0) {
    const bd_battery_point *last =
        &history->points[(history->head + history->count - 1) %
                         BD_BATTERY_HISTORY_MAX];
    if (epoch_s <= last->epoch_s) {
      return false;
    }
    if (last->percent == percent &&
        epoch_s - last->epoch_s < BD_BATTERY_HISTORY_INTERVAL_S) {
      return true;
    }
  }
  if (history->count == BD_BATTERY_HISTORY_MAX) {
    history->head = (history->head + 1) % BD_BATTERY_HISTORY_MAX;
    --history->count;
  }
  bd_battery_point *point =
      &history->points[(history->head + history->count) %
                       BD_BATTERY_HISTORY_MAX];
  point->epoch_s = epoch_s;
  point->percent = percent;
  point->charging = charging;
  ++history->count;
  return true;
}

size_t bd_battery_history_count(const bd_battery_history *history) {
  return history != NULL ? history->count : 0;
}

const bd_battery_point *bd_battery_history_at(const bd_battery_history *history,
                                              size_t index) {
  if (history == NULL || index >= history->count) {
    return NULL;
  }
  return &history->points[(history->head + index) % BD_BATTERY_HISTORY_MAX];
}

int bd_battery_history_percent_at(const bd_battery_history *history,
                                  int64_t epoch_s) {
  if (history == NULL || history->count == 0) {
    return -1;
  }
  int percent = -1;
  for (size_t i = 0; i < history->count; ++i) {
    const bd_battery_point *point =
        &history->points[(history->head + i) % BD_BATTERY_HISTORY_MAX];
    if (point->epoch_s > epoch_s) {
      break;
    }
    percent = point->percent;
  }
  return percent;
}

int bd_battery_history_remaining_minutes(const bd_battery_history *history,
                                         int64_t now_s) {
  if (history == NULL || history->count < 3) {
    return -1;
  }
  const bd_battery_point *latest = NULL;
  const bd_battery_point *oldest = NULL;
  for (size_t i = history->count; i > 0; --i) {
    const bd_battery_point *point =
        &history->points[(history->head + i - 1) % BD_BATTERY_HISTORY_MAX];
    if (point->epoch_s > now_s) {
      continue;
    }
    if (latest == NULL) {
      latest = point;
    }
    if (now_s - point->epoch_s > 3600) {
      break;
    }
    if (!point->charging) {
      oldest = point;
    }
  }
  if (latest == NULL || oldest == NULL || latest->charging) {
    return -1;
  }
  int64_t span = latest->epoch_s - oldest->epoch_s;
  if (span < 900) {
    return -1;
  }
  int drop = oldest->percent - latest->percent;
  if (drop <= 0 || latest->percent <= 0) {
    return -1;
  }
  double minutes = (double)latest->percent * (double)span / 60.0 / (double)drop;
  if (minutes <= 0) {
    return -1;
  }
  if (minutes > 24.0 * 60.0) {
    minutes = 24.0 * 60.0;
  }
  return (int)(minutes + 0.5);
}
