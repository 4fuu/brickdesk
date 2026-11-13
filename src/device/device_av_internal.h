#ifndef BRICKDESK_DEVICE_AV_INTERNAL_H
#define BRICKDESK_DEVICE_AV_INTERNAL_H

/*
 * Test seams for the device capability layer.  This header is private to
 * src/device and the unit tests; it is not installed or shipped.
 *
 * Production callers use brickdesk/device_av.h.  The *_with_io entry points
 * exist so host tests can exercise range, readback, failure and routing
 * behaviour without /dev/disp or a real sound card.
 */

#include "brickdesk/device_av.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct bd_av_disp_io {
  void *user;
  int (*open)(void *user, const char *path, int flags);
  long (*ioctl)(void *user, int fd, unsigned long request, unsigned long *args);
  int (*close)(void *user, int fd);
} bd_av_disp_io;

typedef struct bd_av_mixer_control {
  unsigned int numid;
  unsigned int iface;
  unsigned int device;
  unsigned int subdevice;
  unsigned int index;
  unsigned int type;
  unsigned int access;
  unsigned int count;
  long min;
  long max;
  long step;
  char name[BD_AV_NAME_MAX];
} bd_av_mixer_control;

/* Kernel UAPI values, kept here so audio.c does not include sound/asound.h. */
#define BD_AV_ALSA_TYPE_BOOLEAN 1u
#define BD_AV_ALSA_TYPE_INTEGER 2u
#define BD_AV_ALSA_TYPE_ENUMERATED 3u
#define BD_AV_ALSA_TYPE_INTEGER64 6u
#define BD_AV_ALSA_ACCESS_READ (1u << 0)
#define BD_AV_ALSA_ACCESS_WRITE (1u << 1)
#define BD_AV_ALSA_ACCESS_INACTIVE (1u << 8)

typedef struct bd_av_mixer_io {
  void *user;
  bool (*find_card)(void *user, const char *proc_cards,
                    const char *card_name, bool by_index,
                    int requested_index, int *card_index, char *card_id,
                    size_t card_id_size, bd_av_error *error);
  int (*open_card)(void *user, const char *snd_root, int card_index,
                   const char *card_name, bool by_index, char *card_id,
                   size_t card_id_size, bd_av_error *error);
  bool (*list_controls)(void *user, int fd, bd_av_mixer_control *controls,
                        size_t capacity, size_t *count, bool *truncated,
                        bd_av_error *error);
  bool (*read_control)(void *user, int fd,
                       const bd_av_mixer_control *control, long *values,
                       size_t count, bd_av_error *error);
  bool (*write_control)(void *user, int fd,
                        const bd_av_mixer_control *control,
                        const long *values, size_t count,
                        bd_av_error *error);
  void (*close_card)(void *user, int fd);
} bd_av_mixer_io;

const bd_av_disp_io *bd_av_disp_io_default(void);
const bd_av_mixer_io *bd_av_mixer_io_default(void);

bd_av_result bd_av_brightness_probe_with_io(
    bd_av_brightness *brightness, const bd_av_brightness_config *config,
    const bd_av_disp_io *disp_io, bd_av_error *error);

bd_av_result bd_av_audio_open_with_io(
    bd_av_audio *audio, const bd_av_audio_config *config,
    const bd_av_mixer_io *mixer_io, bd_av_error *error);

/* Exposed for the /proc/asound/cards parser test. */
bool bd_av_alsa_find_card(const char *proc_cards, const char *card_name,
                          bool by_index, int requested_index,
                          int *card_index, char *card_id,
                          size_t card_id_size, bd_av_error *error);

/* Shared helpers, implemented in brightness_common.c. */
void bd_av_set_error(bd_av_error *error, bd_av_result code,
                     const char *format, ...);
bool bd_av_copy_string(char *dst, size_t dst_size, const char *src);
void bd_av_trim(char *text);
bool bd_av_parse_long(const char *text, long *out);
bool bd_av_read_text(const char *path, char *out, size_t out_size);
bool bd_av_write_text(const char *path, const char *text);
bool bd_av_path_join(const char *root, const char *path, char *out,
                     size_t out_size);
bool bd_av_file_exists(const char *path);
bool bd_av_writable(const char *path);

#endif /* BRICKDESK_DEVICE_AV_INTERNAL_H */
