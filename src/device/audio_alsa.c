#define _POSIX_C_SOURCE 200809L

#include "device_av_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <sound/asound.h>

#define BD_ALSA_MAX_ENUMERATED 4096u
#define BD_ALSA_MAX_VALUES 128u

static void copy_alsa_name(char *dst, size_t dst_size, const unsigned char *src,
                           size_t src_size) {
  size_t length = 0;
  while (length < src_size && src[length] != '\0') {
    ++length;
  }
  if (length >= dst_size) {
    length = dst_size - 1;
  }
  memcpy(dst, src, length);
  dst[length] = '\0';
  bd_av_trim(dst);
}

bool bd_av_alsa_find_card(const char *proc_cards, const char *card_name,
                          bool by_index, int requested_index,
                          int *card_index, char *card_id,
                          size_t card_id_size, bd_av_error *error) {
  if (proc_cards == NULL || card_index == NULL || card_id == NULL ||
      card_id_size == 0) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT,
                    "invalid ALSA card discovery arguments");
    return false;
  }
  if (!by_index && (card_name == NULL || card_name[0] == '\0')) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT,
                    "card_name is empty and card_by_index is false");
    return false;
  }
  if (by_index && requested_index < 0) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "card_index %d is invalid",
                    requested_index);
    return false;
  }

  FILE *file = fopen(proc_cards, "r");
  if (file == NULL) {
    bd_av_set_error(error, BD_AV_ERR_IO, "cannot read %s: %s", proc_cards,
                    strerror(errno));
    return false;
  }
  char line[256];
  bool found = false;
  while (fgets(line, sizeof(line), file) != NULL) {
    int index = -1;
    char id[BD_AV_NAME_MAX];
    if (sscanf(line, " %d [%63[^]]]", &index, id) != 2) {
      continue;
    }
    bd_av_trim(id);
    if (by_index) {
      if (index != requested_index) {
        continue;
      }
    } else if (strcmp(id, card_name) != 0) {
      continue;
    }
    *card_index = index;
    bd_av_copy_string(card_id, card_id_size, id);
    found = true;
    break;
  }
  fclose(file);
  if (!found) {
    if (by_index) {
      bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                      "card index %d is not listed in %s", requested_index,
                      proc_cards);
    } else {
      bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                      "card '%s' is not listed in %s", card_name, proc_cards);
    }
    return false;
  }
  return true;
}

static bool alsa_find_card(void *user, const char *proc_cards,
                           const char *card_name, bool by_index,
                           int requested_index, int *card_index,
                           char *card_id, size_t card_id_size,
                           bd_av_error *error) {
  (void)user;
  return bd_av_alsa_find_card(proc_cards, card_name, by_index,
                              requested_index, card_index, card_id,
                              card_id_size, error);
}

static int alsa_open_card(void *user, const char *snd_root, int card_index,
                          const char *card_name, bool by_index, char *card_id,
                          size_t card_id_size, bd_av_error *error) {
  (void)user;
  if (snd_root == NULL || snd_root[0] == '\0') {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "snd_root is empty");
    return -1;
  }
  char path[BD_AV_PATH_MAX];
  int written = snprintf(path, sizeof(path), "%s/controlC%d", snd_root,
                         card_index);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT,
                    "ALSA control path is too long");
    return -1;
  }
  int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    bd_av_set_error(error, BD_AV_ERR_IO, "cannot open %s: %s", path,
                    strerror(errno));
    return -1;
  }

  struct snd_ctl_card_info info;
  memset(&info, 0, sizeof(info));
  if (ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &info) < 0) {
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "cannot read card info from %s: %s", path,
                    strerror(errno));
    close(fd);
    return -1;
  }
  if (info.card != card_index) {
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "%s reports card %d instead of %d", path, info.card,
                    card_index);
    close(fd);
    return -1;
  }

  char actual_id[17];
  memset(actual_id, 0, sizeof(actual_id));
  copy_alsa_name(actual_id, sizeof(actual_id), info.id, sizeof(info.id));
  if (!by_index && card_name != NULL &&
      strcmp(actual_id, card_name) != 0) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "%s card id is '%s', expected '%s'", path, actual_id,
                    card_name);
    close(fd);
    return -1;
  }
  if (!bd_av_copy_string(card_id, card_id_size, actual_id)) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "ALSA card id is too long");
    close(fd);
    return -1;
  }
  return fd;
}

static bool alsa_list_controls(void *user, int fd,
                               bd_av_mixer_control *controls,
                               size_t capacity, size_t *count,
                               bool *truncated, bd_av_error *error) {
  (void)user;
  if (controls == NULL || count == NULL || truncated == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT,
                    "invalid ALSA control list arguments");
    return false;
  }
  *count = 0;
  *truncated = false;

  struct snd_ctl_elem_list list;
  memset(&list, 0, sizeof(list));
  if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) {
    bd_av_set_error(error, BD_AV_ERR_IO, "cannot count ALSA controls: %s",
                    strerror(errno));
    return false;
  }
  unsigned int total = list.count;
  if (total == 0) {
    return true;
  }
  if (total > BD_ALSA_MAX_ENUMERATED) {
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "ALSA reports %u controls, above the safety limit %u",
                    total, BD_ALSA_MAX_ENUMERATED);
    return false;
  }

  struct snd_ctl_elem_id *ids =
      (struct snd_ctl_elem_id *)calloc(total, sizeof(*ids));
  if (ids == NULL) {
    bd_av_set_error(error, BD_AV_ERR_IO, "cannot allocate ALSA control list");
    return false;
  }
  memset(&list, 0, sizeof(list));
  list.pids = ids;
  list.space = total;
  list.offset = 0;
  if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) {
    bd_av_set_error(error, BD_AV_ERR_IO, "cannot list ALSA controls: %s",
                    strerror(errno));
    free(ids);
    return false;
  }

  size_t used = list.used;
  if (used > total) {
    used = total;
  }
  size_t out = 0;
  for (size_t i = 0; i < used; ++i) {
    if (ids[i].iface != SNDRV_CTL_ELEM_IFACE_MIXER) {
      continue;
    }
    if (out >= capacity) {
      *truncated = true;
      continue;
    }
    struct snd_ctl_elem_info info;
    memset(&info, 0, sizeof(info));
    info.id = ids[i];
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) {
      continue;
    }
    bd_av_mixer_control *control = &controls[out];
    memset(control, 0, sizeof(*control));
    control->numid = ids[i].numid;
    control->iface = ids[i].iface;
    control->device = ids[i].device;
    control->subdevice = ids[i].subdevice;
    control->index = ids[i].index;
    control->type = info.type;
    control->access = info.access;
    control->count = info.count;
    copy_alsa_name(control->name, sizeof(control->name), ids[i].name,
                   sizeof(ids[i].name));
    if (control->name[0] == '\0') {
      continue;
    }
    if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
      control->min = info.value.integer.min;
      control->max = info.value.integer.max;
      control->step = info.value.integer.step;
    } else if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER64) {
      control->min = (long)info.value.integer64.min;
      control->max = (long)info.value.integer64.max;
      control->step = (long)info.value.integer64.step;
    } else if (info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN) {
      control->min = 0;
      control->max = 1;
      control->step = 1;
    }
    ++out;
  }
  free(ids);
  *count = out;
  return true;
}

static void fill_control_id(struct snd_ctl_elem_id *id,
                            const bd_av_mixer_control *control,
                            unsigned int numid) {
  memset(id, 0, sizeof(*id));
  id->numid = numid;
  id->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
  id->device = control->device;
  id->subdevice = control->subdevice;
  id->index = control->index;
  copy_alsa_name((char *)id->name, sizeof(id->name),
                 (const unsigned char *)control->name,
                 strlen(control->name));
}

static bool alsa_read_control(void *user, int fd,
                              const bd_av_mixer_control *control,
                              long *values, size_t count,
                              bd_av_error *error) {
  (void)user;
  if (control == NULL || values == NULL || count == 0) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT,
                    "invalid ALSA control read arguments");
    return false;
  }
  if (count > BD_ALSA_MAX_VALUES || count > control->count ||
      (control->type == SNDRV_CTL_ELEM_TYPE_INTEGER64 && count > 64)) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT,
                    "ALSA control '%s' value count is invalid",
                    control->name);
    return false;
  }
  if (control->type != SNDRV_CTL_ELEM_TYPE_INTEGER &&
      control->type != SNDRV_CTL_ELEM_TYPE_INTEGER64 &&
      control->type != SNDRV_CTL_ELEM_TYPE_BOOLEAN) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "ALSA control '%s' has an unsupported type",
                    control->name);
    return false;
  }

  struct snd_ctl_elem_value value;
  memset(&value, 0, sizeof(value));
  fill_control_id(&value.id, control, 0);
  if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &value) < 0) {
    int first_error = errno;
    if (control->numid != 0) {
      memset(&value, 0, sizeof(value));
      fill_control_id(&value.id, control, control->numid);
      if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &value) == 0) {
        goto read_ok;
      }
    }
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "cannot read ALSA control '%s': %s", control->name,
                    strerror(first_error));
    return false;
  }

read_ok:
  for (size_t i = 0; i < count; ++i) {
    if (control->type == SNDRV_CTL_ELEM_TYPE_INTEGER64) {
      values[i] = (long)value.value.integer64.value[i];
    } else {
      values[i] = value.value.integer.value[i];
    }
  }
  return true;
}

static bool alsa_write_control(void *user, int fd,
                               const bd_av_mixer_control *control,
                               const long *values, size_t count,
                               bd_av_error *error) {
  (void)user;
  if (control == NULL || values == NULL || count == 0) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT,
                    "invalid ALSA control write arguments");
    return false;
  }
  if (count > BD_ALSA_MAX_VALUES || count > control->count ||
      (control->type == SNDRV_CTL_ELEM_TYPE_INTEGER64 && count > 64)) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT,
                    "ALSA control '%s' value count is invalid",
                    control->name);
    return false;
  }
  if (control->type != SNDRV_CTL_ELEM_TYPE_INTEGER &&
      control->type != SNDRV_CTL_ELEM_TYPE_INTEGER64 &&
      control->type != SNDRV_CTL_ELEM_TYPE_BOOLEAN) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "ALSA control '%s' has an unsupported type",
                    control->name);
    return false;
  }

  struct snd_ctl_elem_value value;
  memset(&value, 0, sizeof(value));
  fill_control_id(&value.id, control, 0);
  for (size_t i = 0; i < count; ++i) {
    if (control->type == SNDRV_CTL_ELEM_TYPE_INTEGER64) {
      value.value.integer64.value[i] = (long long)values[i];
    } else {
      value.value.integer.value[i] = values[i];
    }
  }
  if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) < 0) {
    int first_error = errno;
    if (control->numid != 0) {
      memset(&value, 0, sizeof(value));
      fill_control_id(&value.id, control, control->numid);
      for (size_t i = 0; i < count; ++i) {
        if (control->type == SNDRV_CTL_ELEM_TYPE_INTEGER64) {
          value.value.integer64.value[i] = (long long)values[i];
        } else {
          value.value.integer.value[i] = values[i];
        }
      }
      if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == 0) {
        return true;
      }
    }
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "cannot write ALSA control '%s': %s", control->name,
                    strerror(first_error));
    return false;
  }
  return true;
}

static void alsa_close_card(void *user, int fd) {
  (void)user;
  if (fd >= 0) {
    close(fd);
  }
}

const bd_av_mixer_io *bd_av_mixer_io_default(void) {
  static const bd_av_mixer_io io = {
      .user = NULL,
      .find_card = alsa_find_card,
      .open_card = alsa_open_card,
      .list_controls = alsa_list_controls,
      .read_control = alsa_read_control,
      .write_control = alsa_write_control,
      .close_card = alsa_close_card,
  };
  return &io;
}
