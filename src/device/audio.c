#define _POSIX_C_SOURCE 200809L

#include "device_av_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BD_AUDIO_DEFAULT_CARD "audiocodec"
#define BD_AUDIO_DEFAULT_VOLUME "digital volume"
#define BD_AUDIO_DEFAULT_SPEAKER_SWITCH "HpSpeaker Switch"
#define BD_AUDIO_DEFAULT_HEADPHONE_SWITCH "Headphone Switch"
#define BD_AUDIO_DEFAULT_HEADPHONE_VOLUME "Headphone Volume"
#define BD_AUDIO_DEFAULT_SPEAKER_MUTE "/sys/class/speaker/mute"
#define BD_AUDIO_MIXER_IFACE 2u /* SNDRV_CTL_ELEM_IFACE_MIXER */

static const char *config_or(const char *value, const char *fallback) {
  return value == NULL ? fallback : value;
}

static const char *config_snd_root(const bd_av_audio_config *config) {
  return config_or(config == NULL ? NULL : config->snd_root, "/dev/snd");
}

static const char *config_proc_cards(const bd_av_audio_config *config) {
  return config_or(config == NULL ? NULL : config->proc_cards,
                   "/proc/asound/cards");
}

static const char *config_speaker_mute(const bd_av_audio_config *config) {
  return config_or(config == NULL ? NULL : config->speaker_mute,
                   BD_AUDIO_DEFAULT_SPEAKER_MUTE);
}

static const char *config_card_name(const bd_av_audio_config *config) {
  return config_or(config == NULL ? NULL : config->card_name,
                   BD_AUDIO_DEFAULT_CARD);
}

static const bd_av_mixer_io *audio_mixer(const bd_av_audio *audio) {
  return (const bd_av_mixer_io *)audio->internal.mixer_io;
}

static bd_av_result result_from_error(bd_av_error *error,
                                      bd_av_result fallback,
                                      const char *context) {
  bd_av_result result =
      error != NULL && error->code != BD_AV_OK ? error->code : fallback;
  if (error != NULL && error->message[0] == '\0') {
    bd_av_set_error(error, result, "%s", context);
  }
  return result;
}

static bd_av_control_type public_type(unsigned int raw_type) {
  switch (raw_type) {
    case BD_AV_ALSA_TYPE_BOOLEAN:
      return BD_AV_CONTROL_BOOLEAN;
    case BD_AV_ALSA_TYPE_INTEGER:
      return BD_AV_CONTROL_INTEGER;
    case BD_AV_ALSA_TYPE_INTEGER64:
      return BD_AV_CONTROL_INTEGER64;
    case BD_AV_ALSA_TYPE_ENUMERATED:
      return BD_AV_CONTROL_ENUMERATED;
    default:
      return BD_AV_CONTROL_OTHER;
  }
}

static unsigned int raw_type(bd_av_control_type type) {
  switch (type) {
    case BD_AV_CONTROL_BOOLEAN:
      return BD_AV_ALSA_TYPE_BOOLEAN;
    case BD_AV_CONTROL_INTEGER:
      return BD_AV_ALSA_TYPE_INTEGER;
    case BD_AV_CONTROL_INTEGER64:
      return BD_AV_ALSA_TYPE_INTEGER64;
    case BD_AV_CONTROL_ENUMERATED:
      return BD_AV_ALSA_TYPE_ENUMERATED;
    default:
      return 0;
  }
}

static bool is_integer_type(bd_av_control_type type) {
  return type == BD_AV_CONTROL_INTEGER || type == BD_AV_CONTROL_INTEGER64;
}

static int find_integer_control(const bd_av_audio *audio, const char *name,
                                unsigned int minimum_count) {
  if (name == NULL || name[0] == '\0') {
    return -1;
  }
  for (size_t i = 0; i < audio->control_count; ++i) {
    const bd_av_control_info *control = &audio->controls[i];
    if (strcmp(control->name, name) != 0) {
      continue;
    }
    if (!is_integer_type(control->type) || !control->readable ||
        !control->writable || control->count < minimum_count ||
        control->max <= control->min) {
      return -1;
    }
    return (int)i;
  }
  return -1;
}

static int find_switch_control(const bd_av_audio *audio, const char *name) {
  if (name == NULL || name[0] == '\0') {
    return -1;
  }
  for (size_t i = 0; i < audio->control_count; ++i) {
    const bd_av_control_info *control = &audio->controls[i];
    if (strcmp(control->name, name) != 0) {
      continue;
    }
    if (control->type != BD_AV_CONTROL_BOOLEAN || !control->readable ||
        !control->writable || control->count < 1) {
      return -1;
    }
    return (int)i;
  }
  return -1;
}

static void mixer_control_from_info(const bd_av_control_info *info,
                                    bd_av_mixer_control *out) {
  memset(out, 0, sizeof(*out));
  out->numid = info->numid;
  out->iface = BD_AUDIO_MIXER_IFACE;
  out->device = info->device;
  out->subdevice = info->subdevice;
  out->index = info->index;
  out->type = raw_type(info->type);
  out->access = info->access;
  out->count = info->count;
  out->min = info->min;
  out->max = info->max;
  out->step = info->step;
  bd_av_copy_string(out->name, sizeof(out->name), info->name);
}

static bd_av_result audio_read_values(const bd_av_audio *audio, int index,
                                      long *values, size_t capacity,
                                      size_t *count, bd_av_error *error) {
  if (index < 0 || (size_t)index >= audio->control_count) {
    bd_av_set_error(error, BD_AV_ERR_STATE,
                    "control index %d is not bound", index);
    return BD_AV_ERR_STATE;
  }
  const bd_av_control_info *info = &audio->controls[index];
  if (!info->readable) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "control '%s' is not readable", info->name);
    return BD_AV_ERR_UNSUPPORTED;
  }
  size_t want = info->count < capacity ? info->count : capacity;
  if (want == 0) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "control '%s' has no values", info->name);
    return BD_AV_ERR_UNSUPPORTED;
  }
  bd_av_mixer_control mixer;
  mixer_control_from_info(info, &mixer);
  const bd_av_mixer_io *io = audio_mixer(audio);
  bd_av_error_clear(error);
  if (!io->read_control(io->user, audio->internal.fd, &mixer, values, want,
                        error)) {
    return result_from_error(error, BD_AV_ERR_IO,
                             "cannot read ALSA control");
  }
  *count = want;
  return BD_AV_OK;
}

static bd_av_result audio_write_values(const bd_av_audio *audio, int index,
                                       const long *values, size_t count,
                                       bd_av_error *error) {
  if (index < 0 || (size_t)index >= audio->control_count) {
    bd_av_set_error(error, BD_AV_ERR_STATE,
                    "control index %d is not bound", index);
    return BD_AV_ERR_STATE;
  }
  const bd_av_control_info *info = &audio->controls[index];
  if (!info->writable) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "control '%s' is not writable", info->name);
    return BD_AV_ERR_UNSUPPORTED;
  }
  if (count > info->count) {
    count = info->count;
  }
  bd_av_mixer_control mixer;
  mixer_control_from_info(info, &mixer);
  const bd_av_mixer_io *io = audio_mixer(audio);
  bd_av_error_clear(error);
  if (!io->write_control(io->user, audio->internal.fd, &mixer, values, count,
                         error)) {
    return result_from_error(error, BD_AV_ERR_IO,
                             "cannot write ALSA control");
  }
  return BD_AV_OK;
}

static bd_av_result read_switch_value(const bd_av_audio *audio, int index,
                                      bool *value, bd_av_error *error) {
  long values[8];
  size_t count = 0;
  bd_av_result result =
      audio_read_values(audio, index, values, 8, &count, error);
  if (result != BD_AV_OK) {
    return result;
  }
  if (values[0] != 0 && values[0] != 1) {
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "control '%s' returned %ld instead of 0/1",
                    audio->controls[index].name, values[0]);
    return BD_AV_ERR_IO;
  }
  *value = values[0] != 0;
  return BD_AV_OK;
}

static bd_av_result write_switch_value(const bd_av_audio *audio, int index,
                                       bool value, bd_av_error *error) {
  long values[8];
  const bd_av_control_info *info = &audio->controls[index];
  size_t count = info->count < 8 ? info->count : 8;
  if (count == 0) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "control '%s' has no values", info->name);
    return BD_AV_ERR_UNSUPPORTED;
  }
  for (size_t i = 0; i < count; ++i) {
    values[i] = value ? 1 : 0;
  }
  bd_av_result result =
      audio_write_values(audio, index, values, count, error);
  if (result != BD_AV_OK) {
    return result;
  }
  bool actual = false;
  result = read_switch_value(audio, index, &actual, error);
  if (result != BD_AV_OK) {
    return result;
  }
  if (actual != value) {
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "control '%s' readback is %d after writing %d",
                    info->name, actual ? 1 : 0, value ? 1 : 0);
    return BD_AV_ERR_IO;
  }
  return BD_AV_OK;
}

static bool parse_mute_text(const char *text, bool *muted) {
  long value = 0;
  if (!bd_av_parse_long(text, &value) || (value != 0 && value != 1)) {
    return false;
  }
  *muted = value != 0;
  return true;
}

static bd_av_result read_mute_path(const bd_av_audio *audio, bool *muted,
                                   bd_av_error *error) {
  char text[32];
  if (!bd_av_read_text(audio->internal.mute_path, text, sizeof(text))) {
    bd_av_set_error(error, BD_AV_ERR_IO, "cannot read %s",
                    audio->internal.mute_path);
    return BD_AV_ERR_IO;
  }
  if (!parse_mute_text(text, muted)) {
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "%s returned an unexpected mute value '%s'",
                    audio->internal.mute_path, text);
    return BD_AV_ERR_IO;
  }
  return BD_AV_OK;
}

static bd_av_result read_mute(const bd_av_audio *audio, bool *muted,
                              bd_av_error *error) {
  if (!audio->has_mute) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "no mute control or speaker mute path is bound");
    return BD_AV_ERR_UNSUPPORTED;
  }
  if (audio->internal.mute_is_alsa) {
    bool audible = false;
    bd_av_result result = read_switch_value(
        audio, audio->internal.mute_index, &audible, error);
    if (result != BD_AV_OK) {
      return result;
    }
    *muted = !audible;
    return BD_AV_OK;
  }
  return read_mute_path(audio, muted, error);
}

static bd_av_result apply_mute(const bd_av_audio *audio, bool muted,
                               bd_av_error *error) {
  if (!audio->has_mute) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "no mute control or speaker mute path is bound");
    return BD_AV_ERR_UNSUPPORTED;
  }
  if (audio->internal.mute_is_alsa) {
    return write_switch_value(audio, audio->internal.mute_index, !muted,
                              error);
  }
  const char *text = muted ? "1" : "0";
  if (!bd_av_write_text(audio->internal.mute_path, text)) {
    bd_av_set_error(error, BD_AV_ERR_IO, "cannot write %s",
                    audio->internal.mute_path);
    return BD_AV_ERR_IO;
  }
  bool actual = false;
  bd_av_result result = read_mute_path(audio, &actual, error);
  if (result != BD_AV_OK) {
    return result;
  }
  if (actual != muted) {
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "%s readback is %d after writing %d",
                    audio->internal.mute_path, actual ? 1 : 0,
                    muted ? 1 : 0);
    return BD_AV_ERR_IO;
  }
  return BD_AV_OK;
}

static long clamp_long(long value, long minimum, long maximum) {
  if (value < minimum) {
    return minimum;
  }
  if (value > maximum) {
    return maximum;
  }
  return value;
}

static long volume_percent_to_raw(const bd_av_audio *audio, int percent) {
  long long span = (long long)audio->volume_max - audio->volume_min;
  if (span <= 0) {
    return audio->volume_min;
  }
  if (percent <= 0) {
    return audio->volume_inverted ? audio->volume_max : audio->volume_min;
  }
  if (percent >= 100) {
    return audio->volume_inverted ? audio->volume_min : audio->volume_max;
  }
  long long offset = ((long long)percent * span + 50) / 100;
  if (audio->volume_inverted) {
    offset = span - offset;
  }
  long raw = (long)(audio->volume_min + offset);
  long step = audio->volume_step > 1 ? audio->volume_step : 1;
  if (step > 1) {
    raw = audio->volume_min + ((raw - audio->volume_min + step / 2) / step) *
                                  step;
  }
  return clamp_long(raw, audio->volume_min, audio->volume_max);
}

static int volume_raw_to_percent(const bd_av_audio *audio, long raw) {
  long long span = (long long)audio->volume_max - audio->volume_min;
  if (span <= 0) {
    return 0;
  }
  long long offset = (long long)raw - audio->volume_min;
  if (audio->volume_inverted) {
    offset = span - offset;
  }
  if (offset < 0) {
    offset = 0;
  }
  if (offset > span) {
    offset = span;
  }
  return (int)((offset * 100 + span / 2) / span);
}

static bd_av_result bind_roles(bd_av_audio *audio,
                               const bd_av_audio_config *config,
                               bd_av_error *error) {
  audio->internal.volume_index =
      find_integer_control(audio, audio->volume_control, 1);
  if (audio->internal.volume_index >= 0) {
    const bd_av_control_info *control =
        &audio->controls[audio->internal.volume_index];
    audio->has_volume = true;
    audio->volume_min = control->min;
    audio->volume_max = control->max;
    audio->volume_step = control->step > 0 ? control->step : 1;
    audio->volume_count = control->count;
    bd_av_volume_map map =
        config == NULL ? BD_AV_VOLUME_MAP_DEFAULT : config->volume_map;
    audio->volume_inverted = map != BD_AV_VOLUME_MAP_LINEAR;
  }

  audio->internal.mute_index =
      find_switch_control(audio, audio->mute_control);
  if (audio->internal.mute_index >= 0) {
    audio->has_mute = true;
    audio->internal.mute_is_alsa = true;
  } else {
    const char *mute_path = config_speaker_mute(config);
    if (mute_path != NULL && mute_path[0] != '\0' &&
        bd_av_writable(mute_path)) {
      char text[32];
      bool muted = false;
      if (bd_av_read_text(mute_path, text, sizeof(text)) &&
          parse_mute_text(text, &muted)) {
        if (bd_av_copy_string(audio->internal.mute_path,
                              sizeof(audio->internal.mute_path),
                              mute_path)) {
          audio->has_mute = true;
          audio->internal.mute_is_alsa = false;
        }
      }
    }
  }

  audio->internal.speaker_index =
      find_switch_control(audio, audio->speaker_switch);
  audio->internal.headphone_index =
      find_switch_control(audio, audio->headphone_switch);
  audio->has_route = audio->internal.speaker_index >= 0 &&
                     audio->internal.headphone_index >= 0;

  audio->internal.headphone_volume_index =
      find_integer_control(audio, audio->headphone_volume, 1);
  audio->has_headphone_volume =
      audio->internal.headphone_volume_index >= 0;

  (void)error;
  return BD_AV_OK;
}

bd_av_result bd_av_audio_open_with_io(bd_av_audio *audio,
                                      const bd_av_audio_config *config,
                                      const bd_av_mixer_io *mixer_io,
                                      bd_av_error *error) {
  bd_av_error_clear(error);
  if (audio == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "audio is NULL");
    return BD_AV_ERR_ARGUMENT;
  }
  memset(audio, 0, sizeof(*audio));
  audio->internal.fd = -1;
  audio->internal.volume_index = -1;
  audio->internal.mute_index = -1;
  audio->internal.speaker_index = -1;
  audio->internal.headphone_index = -1;
  audio->internal.headphone_volume_index = -1;

  if (mixer_io == NULL) {
    mixer_io = bd_av_mixer_io_default();
  }
  bd_av_volume_map map =
      config == NULL ? BD_AV_VOLUME_MAP_DEFAULT : config->volume_map;
  if (map != BD_AV_VOLUME_MAP_DEFAULT && map != BD_AV_VOLUME_MAP_INVERTED &&
      map != BD_AV_VOLUME_MAP_LINEAR) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "unknown volume map");
    return BD_AV_ERR_ARGUMENT;
  }
  audio->internal.mixer_io = mixer_io;
  audio->manual_route = config != NULL && config->manual_route;

  const char *volume_control = config_or(
      config == NULL ? NULL : config->volume_control,
      BD_AUDIO_DEFAULT_VOLUME);
  const char *mute_control =
      config_or(config == NULL ? NULL : config->mute_control, "");
  const char *speaker_switch = config_or(
      config == NULL ? NULL : config->speaker_switch,
      BD_AUDIO_DEFAULT_SPEAKER_SWITCH);
  const char *headphone_switch = config_or(
      config == NULL ? NULL : config->headphone_switch,
      BD_AUDIO_DEFAULT_HEADPHONE_SWITCH);
  const char *headphone_volume = config_or(
      config == NULL ? NULL : config->headphone_volume,
      BD_AUDIO_DEFAULT_HEADPHONE_VOLUME);

  if (!bd_av_copy_string(audio->volume_control,
                         sizeof(audio->volume_control), volume_control) ||
      !bd_av_copy_string(audio->mute_control, sizeof(audio->mute_control),
                         mute_control) ||
      !bd_av_copy_string(audio->speaker_switch,
                         sizeof(audio->speaker_switch), speaker_switch) ||
      !bd_av_copy_string(audio->headphone_switch,
                         sizeof(audio->headphone_switch),
                         headphone_switch) ||
      !bd_av_copy_string(audio->headphone_volume,
                         sizeof(audio->headphone_volume),
                         headphone_volume)) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT,
                    "a configured ALSA control name is too long");
    return BD_AV_ERR_ARGUMENT;
  }

  const char *snd_root = config_snd_root(config);
  const char *proc_cards = config_proc_cards(config);
  const char *card_name = config_card_name(config);
  bool by_index = config != NULL && config->card_by_index;
  int requested_index = config == NULL ? 0 : config->card_index;
  if (snd_root[0] == '\0' || proc_cards[0] == '\0') {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT,
                    "snd_root and proc_cards must not be empty");
    return BD_AV_ERR_ARGUMENT;
  }
  if (!by_index && card_name[0] == '\0') {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT,
                    "card_name is empty and card_by_index is false");
    return BD_AV_ERR_ARGUMENT;
  }
  if (by_index && requested_index < 0) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT,
                    "card_index %d is invalid", requested_index);
    return BD_AV_ERR_ARGUMENT;
  }

  int card_index = -1;
  char card_id[BD_AV_NAME_MAX] = "";
  if (!mixer_io->find_card(mixer_io->user, proc_cards, card_name, by_index,
                           requested_index, &card_index, card_id,
                           sizeof(card_id), error)) {
    return result_from_error(error, BD_AV_ERR_UNSUPPORTED,
                             "cannot find the configured ALSA card");
  }

  int fd = mixer_io->open_card(mixer_io->user, snd_root, card_index,
                               card_name, by_index, card_id,
                               sizeof(card_id), error);
  if (fd < 0) {
    return result_from_error(error, BD_AV_ERR_IO,
                             "cannot open the ALSA control device");
  }
  audio->internal.fd = fd;
  audio->card_index = card_index;
  bd_av_copy_string(audio->card_id, sizeof(audio->card_id), card_id);

  bd_av_mixer_control raw[BD_AV_MAX_CONTROLS];
  size_t raw_count = 0;
  bool truncated = false;
  bd_av_error_clear(error);
  if (!mixer_io->list_controls(mixer_io->user, fd, raw, BD_AV_MAX_CONTROLS,
                               &raw_count, &truncated, error)) {
    bd_av_result result = result_from_error(
        error, BD_AV_ERR_IO, "cannot enumerate ALSA mixer controls");
    bd_av_audio_close(audio);
    return result;
  }
  if (raw_count > BD_AV_MAX_CONTROLS) {
    raw_count = BD_AV_MAX_CONTROLS;
    truncated = true;
  }
  for (size_t i = 0; i < raw_count; ++i) {
    bd_av_control_info *info = &audio->controls[i];
    memset(info, 0, sizeof(*info));
    bd_av_copy_string(info->name, sizeof(info->name), raw[i].name);
    info->index = raw[i].index;
    info->type = public_type(raw[i].type);
    info->count = raw[i].count;
    info->min = raw[i].min;
    info->max = raw[i].max;
    info->step = raw[i].step;
    info->readable = (raw[i].access & BD_AV_ALSA_ACCESS_READ) != 0 &&
                     (raw[i].access & BD_AV_ALSA_ACCESS_INACTIVE) == 0;
    info->writable = (raw[i].access & BD_AV_ALSA_ACCESS_WRITE) != 0 &&
                     (raw[i].access & BD_AV_ALSA_ACCESS_INACTIVE) == 0;
    info->numid = raw[i].numid;
    info->device = raw[i].device;
    info->subdevice = raw[i].subdevice;
    info->access = raw[i].access;
  }
  audio->control_count = raw_count;
  audio->controls_truncated = truncated;

  return bind_roles(audio, config, error);
}

bd_av_result bd_av_audio_open(bd_av_audio *audio,
                              const bd_av_audio_config *config,
                              bd_av_error *error) {
  return bd_av_audio_open_with_io(audio, config, bd_av_mixer_io_default(),
                                  error);
}

void bd_av_audio_close(bd_av_audio *audio) {
  if (audio == NULL) {
    return;
  }
  const bd_av_mixer_io *io = audio_mixer(audio);
  if (audio->internal.fd >= 0 && io != NULL && io->close_card != NULL) {
    io->close_card(io->user, audio->internal.fd);
  }
  memset(audio, 0, sizeof(*audio));
  audio->internal.fd = -1;
}

size_t bd_av_audio_control_count(const bd_av_audio *audio) {
  return audio == NULL ? 0 : audio->control_count;
}

const bd_av_control_info *bd_av_audio_control_at(const bd_av_audio *audio,
                                                 size_t index) {
  if (audio == NULL || index >= audio->control_count) {
    return NULL;
  }
  return &audio->controls[index];
}

bd_av_result bd_av_audio_get_volume(const bd_av_audio *audio, int *percent,
                                    bd_av_error *error) {
  bd_av_error_clear(error);
  if (audio == NULL || percent == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "audio or percent is NULL");
    return BD_AV_ERR_ARGUMENT;
  }
  if (!audio->has_volume) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "no writable volume control '%s' was enumerated",
                    audio->volume_control);
    return BD_AV_ERR_UNSUPPORTED;
  }
  long values[8];
  size_t count = 0;
  bd_av_result result = audio_read_values(
      audio, audio->internal.volume_index, values, 8, &count, error);
  if (result != BD_AV_OK) {
    return result;
  }
  if (values[0] < audio->volume_min || values[0] > audio->volume_max) {
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "control '%s' returned %ld outside %ld..%ld",
                    audio->volume_control, values[0], audio->volume_min,
                    audio->volume_max);
    return BD_AV_ERR_IO;
  }
  *percent = volume_raw_to_percent(audio, values[0]);
  return BD_AV_OK;
}

bd_av_result bd_av_audio_set_volume(bd_av_audio *audio, int percent,
                                    bd_av_error *error) {
  bd_av_error_clear(error);
  if (audio == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "audio is NULL");
    return BD_AV_ERR_ARGUMENT;
  }
  if (percent < 0 || percent > 100) {
    bd_av_set_error(error, BD_AV_ERR_RANGE,
                    "volume percent %d is outside 0..100", percent);
    return BD_AV_ERR_RANGE;
  }
  if (!audio->has_volume) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "no writable volume control '%s' was enumerated",
                    audio->volume_control);
    return BD_AV_ERR_UNSUPPORTED;
  }
  long raw = volume_percent_to_raw(audio, percent);
  unsigned int count = audio->volume_count;
  if (count == 0 || count > 8) {
    bd_av_set_error(error, BD_AV_ERR_STATE,
                    "volume control '%s' has an unusable value count %u",
                    audio->volume_control, count);
    return BD_AV_ERR_STATE;
  }
  long values[8];
  for (unsigned int i = 0; i < count; ++i) {
    values[i] = raw;
  }
  bd_av_result result = audio_write_values(
      audio, audio->internal.volume_index, values, count, error);
  if (result != BD_AV_OK) {
    return result;
  }
  long actual[8];
  size_t actual_count = 0;
  result = audio_read_values(audio, audio->internal.volume_index, actual, 8,
                             &actual_count, error);
  if (result != BD_AV_OK) {
    return result;
  }
  for (size_t i = 0; i < actual_count; ++i) {
    if (actual[i] != raw) {
      bd_av_set_error(error, BD_AV_ERR_IO,
                      "volume readback is %ld after writing %ld on '%s'",
                      actual[i], raw, audio->volume_control);
      return BD_AV_ERR_IO;
    }
  }

  if (audio->has_mute) {
    bool desired = percent == 0 ? true : audio->muted;
    bd_av_error mute_error;
    bd_av_error_clear(&mute_error);
    bd_av_result mute_result = apply_mute(audio, desired, &mute_error);
    if (mute_result != BD_AV_OK) {
      bd_av_set_error(error, mute_result,
                      "volume was written but mute failed: %s",
                      mute_error.message[0] != '\0' ? mute_error.message
                                                    : "unknown error");
      return mute_result;
    }
  }
  return BD_AV_OK;
}

bd_av_result bd_av_audio_get_muted(const bd_av_audio *audio, bool *muted,
                                   bd_av_error *error) {
  bd_av_error_clear(error);
  if (audio == NULL || muted == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "audio or muted is NULL");
    return BD_AV_ERR_ARGUMENT;
  }
  return read_mute(audio, muted, error);
}

bd_av_result bd_av_audio_set_muted(bd_av_audio *audio, bool muted,
                                   bd_av_error *error) {
  bd_av_error_clear(error);
  if (audio == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "audio is NULL");
    return BD_AV_ERR_ARGUMENT;
  }
  bd_av_result result = apply_mute(audio, muted, error);
  if (result != BD_AV_OK) {
    return result;
  }
  audio->muted = muted;
  return BD_AV_OK;
}

bd_av_result bd_av_audio_get_route(const bd_av_audio *audio,
                                   bd_av_route *route, bd_av_error *error) {
  bd_av_error_clear(error);
  if (audio == NULL || route == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "audio or route is NULL");
    return BD_AV_ERR_ARGUMENT;
  }
  if (!audio->has_route) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "both '%s' and '%s' must be writable to report a route",
                    audio->speaker_switch, audio->headphone_switch);
    return BD_AV_ERR_UNSUPPORTED;
  }
  bool speaker = false;
  bool headphone = false;
  bd_av_result result = read_switch_value(
      audio, audio->internal.speaker_index, &speaker, error);
  if (result != BD_AV_OK) {
    return result;
  }
  result = read_switch_value(audio, audio->internal.headphone_index,
                             &headphone, error);
  if (result != BD_AV_OK) {
    return result;
  }
  if (speaker && headphone) {
    *route = BD_AV_ROUTE_BOTH;
  } else if (speaker) {
    *route = BD_AV_ROUTE_SPEAKER;
  } else if (headphone) {
    *route = BD_AV_ROUTE_HEADPHONE;
  } else {
    *route = BD_AV_ROUTE_NONE;
  }
  return BD_AV_OK;
}

static bd_av_result force_route(bd_av_audio *audio, bool speaker,
                                bool headphone, bd_av_error *error) {
  int target = speaker ? audio->internal.speaker_index
                       : audio->internal.headphone_index;
  bd_av_result result = write_switch_value(audio, target, true, error);
  if (result != BD_AV_OK) {
    return result;
  }
  int other = speaker ? audio->internal.headphone_index
                      : audio->internal.speaker_index;
  result = write_switch_value(audio, other, false, error);
  if (result != BD_AV_OK) {
    return result;
  }
  bool actual_speaker = false;
  bool actual_headphone = false;
  result = read_switch_value(audio, audio->internal.speaker_index,
                             &actual_speaker, error);
  if (result != BD_AV_OK) {
    return result;
  }
  result = read_switch_value(audio, audio->internal.headphone_index,
                             &actual_headphone, error);
  if (result != BD_AV_OK) {
    return result;
  }
  if (actual_speaker != speaker || actual_headphone != headphone) {
    bd_av_set_error(error, BD_AV_ERR_IO,
                    "route readback is speaker=%d headphone=%d after writing "
                    "speaker=%d headphone=%d",
                    actual_speaker ? 1 : 0, actual_headphone ? 1 : 0,
                    speaker ? 1 : 0, headphone ? 1 : 0);
    return BD_AV_ERR_IO;
  }
  return BD_AV_OK;
}

bd_av_result bd_av_audio_set_route(bd_av_audio *audio, bd_av_route route,
                                   bd_av_error *error) {
  bd_av_error_clear(error);
  if (audio == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "audio is NULL");
    return BD_AV_ERR_ARGUMENT;
  }
  if (!audio->has_route) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "both '%s' and '%s' must be writable to set a route",
                    audio->speaker_switch, audio->headphone_switch);
    return BD_AV_ERR_UNSUPPORTED;
  }
  if (route == BD_AV_ROUTE_AUTO) {
    if (!audio->jack_known) {
      bd_av_set_error(error, BD_AV_ERR_STATE,
                      "automatic route has no headphone jack state");
      return BD_AV_ERR_STATE;
    }
    route = audio->headphone_plugged ? BD_AV_ROUTE_HEADPHONE
                                     : BD_AV_ROUTE_SPEAKER;
  }
  if (route == BD_AV_ROUTE_SPEAKER) {
    return force_route(audio, true, false, error);
  }
  if (route == BD_AV_ROUTE_HEADPHONE) {
    return force_route(audio, false, true, error);
  }
  bd_av_set_error(error, BD_AV_ERR_ARGUMENT,
                  "route '%s' cannot be forced; use speaker or headphone",
                  bd_av_route_name(route));
  return BD_AV_ERR_ARGUMENT;
}

bd_av_result bd_av_audio_set_jack(bd_av_audio *audio, bool plugged,
                                  bd_av_error *error) {
  bd_av_error_clear(error);
  if (audio == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "audio is NULL");
    return BD_AV_ERR_ARGUMENT;
  }
  audio->jack_known = true;
  audio->headphone_plugged = plugged;
  if (audio->manual_route) {
    return BD_AV_OK;
  }
  if (!audio->has_route) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "jack state recorded, but no speaker/headphone switches "
                    "are bound");
    return BD_AV_ERR_UNSUPPORTED;
  }
  return bd_av_audio_set_route(audio, BD_AV_ROUTE_AUTO, error);
}

static int find_named_control(const bd_av_audio *audio, const char *name,
                              bool writable) {
  if (name == NULL || name[0] == '\0') {
    return -1;
  }
  for (size_t i = 0; i < audio->control_count; ++i) {
    const bd_av_control_info *control = &audio->controls[i];
    if (strcmp(control->name, name) == 0 && control->readable &&
        (!writable || control->writable) && control->count > 0) {
      return (int)i;
    }
  }
  return -1;
}

bool bd_av_audio_has_control(const bd_av_audio *audio, const char *name) {
  return audio != NULL && find_named_control(audio, name, false) >= 0;
}

bd_av_result bd_av_audio_set_switch_by_name(bd_av_audio *audio,
                                            const char *name, bool value,
                                            bd_av_error *error) {
  bd_av_error_clear(error);
  if (audio == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "audio is NULL");
    return BD_AV_ERR_ARGUMENT;
  }
  int index = find_named_control(audio, name, true);
  if (index < 0 ||
      audio->controls[index].type != BD_AV_CONTROL_BOOLEAN) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "boolean control '%s' is not available", name);
    return BD_AV_ERR_UNSUPPORTED;
  }
  return write_switch_value(audio, index, value, error);
}

bd_av_result bd_av_audio_set_percent_by_name(bd_av_audio *audio,
                                             const char *name, int percent,
                                             bd_av_error *error) {
  bd_av_error_clear(error);
  if (audio == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "audio is NULL");
    return BD_AV_ERR_ARGUMENT;
  }
  if (percent < 0 || percent > 100) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "percent %d is outside 0..100",
                    percent);
    return BD_AV_ERR_ARGUMENT;
  }
  int index = find_named_control(audio, name, true);
  if (index < 0 ||
      (audio->controls[index].type != BD_AV_CONTROL_INTEGER &&
       audio->controls[index].type != BD_AV_CONTROL_INTEGER64)) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "integer control '%s' is not available", name);
    return BD_AV_ERR_UNSUPPORTED;
  }
  const bd_av_control_info *info = &audio->controls[index];
  long span = info->max - info->min;
  long value = info->min + (span * percent + 50) / 100;
  if (info->step > 1) {
    value = info->min + ((value - info->min) / info->step) * info->step;
  }
  if (value < info->min) {
    value = info->min;
  }
  if (value > info->max) {
    value = info->max;
  }
  long values[8];
  size_t count = info->count < 8 ? info->count : 8;
  for (size_t i = 0; i < count; ++i) {
    values[i] = value;
  }
  return audio_write_values(audio, index, values, count, error);
}

bd_av_result bd_av_audio_get_percent_by_name(const bd_av_audio *audio,
                                             const char *name, int *percent,
                                             bd_av_error *error) {
  bd_av_error_clear(error);
  if (audio == NULL || percent == NULL) {
    bd_av_set_error(error, BD_AV_ERR_ARGUMENT, "audio or percent is NULL");
    return BD_AV_ERR_ARGUMENT;
  }
  int index = find_named_control(audio, name, false);
  if (index < 0 ||
      (audio->controls[index].type != BD_AV_CONTROL_INTEGER &&
       audio->controls[index].type != BD_AV_CONTROL_INTEGER64)) {
    bd_av_set_error(error, BD_AV_ERR_UNSUPPORTED,
                    "integer control '%s' is not available", name);
    return BD_AV_ERR_UNSUPPORTED;
  }
  long values[8];
  size_t count = 0;
  bd_av_result result =
      audio_read_values(audio, index, values, 8, &count, error);
  if (result != BD_AV_OK) {
    return result;
  }
  const bd_av_control_info *info = &audio->controls[index];
  long span = info->max - info->min;
  *percent = span > 0 ? (int)(((values[0] - info->min) * 100 + span / 2) / span)
                      : 0;
  return BD_AV_OK;
}
