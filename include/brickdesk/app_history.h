#ifndef BRICKDESK_APP_HISTORY_H
#define BRICKDESK_APP_HISTORY_H

/*
 * Small, desktop-level app usage history and favourites.
 *
 * This is not a game library: it only remembers app ids that the shell has
 * launched, their last-used order and a favourite bit used by the existing
 * home tab filter.  The file lives next to local.ini and is capped so it
 * cannot grow without bound.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BD_APP_HISTORY_MAX 32
#define BD_APP_HISTORY_ID_MAX 128

typedef struct bd_app_history_entry {
  char id[BD_APP_HISTORY_ID_MAX];
  int64_t last_used_s;
  bool favorite;
} bd_app_history_entry;

typedef struct bd_app_history bd_app_history;

bd_app_history *bd_app_history_create(void);
void bd_app_history_destroy(bd_app_history *history);

/* Missing or malformed files leave an empty history and return false. */
bool bd_app_history_load(bd_app_history *history, const char *path);
bool bd_app_history_save(const bd_app_history *history, const char *path);

/* Moves the id to the most-recent end, creating it if needed. */
bool bd_app_history_record(bd_app_history *history, const char *id,
                           int64_t epoch_s);
bool bd_app_history_set_favorite(bd_app_history *history, const char *id,
                                 bool favorite);
bool bd_app_history_favorite(const bd_app_history *history, const char *id);

size_t bd_app_history_count(const bd_app_history *history);
const bd_app_history_entry *bd_app_history_at(const bd_app_history *history,
                                              size_t index);
/* Most recently used id, or NULL when the history is empty. */
const char *bd_app_history_most_recent(const bd_app_history *history);

#endif /* BRICKDESK_APP_HISTORY_H */
