#define _POSIX_C_SOURCE 200809L

#include "brickdesk/device_av.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct options {
  const char *sysfs_root;
  const char *backlight;
  const char *disp_path;
  const char *model_path;
  const char *allowed_models;
  int disp_raw_max;
  const char *snd_root;
  const char *proc_cards;
  const char *speaker_mute;
  const char *card_name;
  int card_index;
  bool card_by_index;
  const char *volume_control;
  bd_av_volume_map volume_map;
  const char *mute_control;
  const char *speaker_switch;
  const char *headphone_switch;
  const char *headphone_volume;
  bool manual_route;
} options;

static void usage(FILE *out) {
  fprintf(out,
          "usage: bd-av [options] probe\n"
          "       bd-av [options] brightness get\n"
          "       bd-av [options] brightness set PERCENT\n"
          "       bd-av [options] brightness set raw RAW\n"
          "       bd-av [options] audio status\n"
          "       bd-av [options] audio controls\n"
          "       bd-av [options] audio volume [PERCENT]\n"
          "       bd-av [options] audio mute [on|off]\n"
          "       bd-av [options] audio route [speaker|headphone|auto]\n"
          "\n"
          "options:\n"
          "  --sysfs-root DIR       prefix before /sys (default real /sys)\n"
          "  --backlight PATH       explicit backlight directory\n"
          "  --disp PATH            /dev/disp path (default /dev/disp)\n"
          "  --disp-raw-max N       raw maximum for /dev/disp (default 255)\n"
          "  --model PATH           model file (default /etc/model)\n"
          "  --allowed-models LIST  comma-separated /dev/disp allowlist\n"
          "  --snd-root DIR         directory with controlC<N> (default /dev/snd)\n"
          "  --proc-cards FILE      default /proc/asound/cards\n"
          "  --speaker-mute PATH    default /sys/class/speaker/mute; \"\" disables\n"
          "  --card NAME            default audiocodec\n"
          "  --card-index N         select a card by index instead of name\n"
          "  --volume-control NAME  default 'digital volume'\n"
          "  --volume-map MAP       default|inverted|linear\n"
          "  --mute-control NAME    optional ALSA boolean mute switch\n"
          "  --speaker-switch NAME  default 'HpSpeaker Switch'\n"
          "  --headphone-switch NAME default 'Headphone Switch'\n"
          "  --headphone-volume NAME default 'Headphone Volume'\n"
          "  --manual-route         record jack edges without switching paths\n");
}

static bool parse_int(const char *text, int *out) {
  if (text == NULL || out == NULL || text[0] == '\0') {
    return false;
  }
  errno = 0;
  char *end = NULL;
  long value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }
  *out = (int)value;
  return true;
}

static int parse_options(int argc, char **argv, options *out,
                         int *command_index) {
  memset(out, 0, sizeof(*out));
  out->volume_map = BD_AV_VOLUME_MAP_DEFAULT;
  out->card_index = 0;
  *command_index = argc;
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (arg[0] != '-') {
      *command_index = i;
      return 0;
    }
    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      usage(stdout);
      exit(0);
    }
    if (strcmp(arg, "--manual-route") == 0) {
      out->manual_route = true;
      continue;
    }
    if (i + 1 >= argc) {
      fprintf(stderr, "bd-av: %s needs a value\n", arg);
      return -1;
    }
    const char *value = argv[++i];
    if (strcmp(arg, "--sysfs-root") == 0) {
      out->sysfs_root = value;
    } else if (strcmp(arg, "--backlight") == 0) {
      out->backlight = value;
    } else if (strcmp(arg, "--disp") == 0) {
      out->disp_path = value;
    } else if (strcmp(arg, "--model") == 0) {
      out->model_path = value;
    } else if (strcmp(arg, "--allowed-models") == 0) {
      out->allowed_models = value;
    } else if (strcmp(arg, "--disp-raw-max") == 0) {
      if (!parse_int(value, &out->disp_raw_max)) {
        fprintf(stderr, "bd-av: invalid --disp-raw-max '%s'\n", value);
        return -1;
      }
    } else if (strcmp(arg, "--snd-root") == 0) {
      out->snd_root = value;
    } else if (strcmp(arg, "--proc-cards") == 0) {
      out->proc_cards = value;
    } else if (strcmp(arg, "--speaker-mute") == 0) {
      out->speaker_mute = value;
    } else if (strcmp(arg, "--card") == 0) {
      out->card_name = value;
    } else if (strcmp(arg, "--card-index") == 0) {
      if (!parse_int(value, &out->card_index) || out->card_index < 0) {
        fprintf(stderr, "bd-av: invalid --card-index '%s'\n", value);
        return -1;
      }
      out->card_by_index = true;
    } else if (strcmp(arg, "--volume-control") == 0) {
      out->volume_control = value;
    } else if (strcmp(arg, "--volume-map") == 0) {
      if (strcmp(value, "linear") == 0) {
        out->volume_map = BD_AV_VOLUME_MAP_LINEAR;
      } else if (strcmp(value, "inverted") == 0) {
        out->volume_map = BD_AV_VOLUME_MAP_INVERTED;
      } else if (strcmp(value, "default") == 0) {
        out->volume_map = BD_AV_VOLUME_MAP_DEFAULT;
      } else {
        fprintf(stderr, "bd-av: invalid --volume-map '%s'\n", value);
        return -1;
      }
    } else if (strcmp(arg, "--mute-control") == 0) {
      out->mute_control = value;
    } else if (strcmp(arg, "--speaker-switch") == 0) {
      out->speaker_switch = value;
    } else if (strcmp(arg, "--headphone-switch") == 0) {
      out->headphone_switch = value;
    } else if (strcmp(arg, "--headphone-volume") == 0) {
      out->headphone_volume = value;
    } else {
      fprintf(stderr, "bd-av: unknown option '%s'\n", arg);
      return -1;
    }
  }
  return 0;
}

static void fill_config(const options *options, bd_av_brightness_config *out) {
  memset(out, 0, sizeof(*out));
  out->sysfs_root = options->sysfs_root;
  out->backlight = options->backlight;
  out->disp_path = options->disp_path;
  out->model_path = options->model_path;
  out->allowed_models = options->allowed_models;
  out->disp_raw_max = options->disp_raw_max;
}

static void fill_audio_config(const options *options,
                              bd_av_audio_config *out) {
  memset(out, 0, sizeof(*out));
  out->snd_root = options->snd_root;
  out->proc_cards = options->proc_cards;
  out->speaker_mute = options->speaker_mute;
  out->card_name = options->card_name;
  out->card_by_index = options->card_by_index;
  out->card_index = options->card_index;
  out->volume_control = options->volume_control;
  out->volume_map = options->volume_map;
  out->mute_control = options->mute_control;
  out->speaker_switch = options->speaker_switch;
  out->headphone_switch = options->headphone_switch;
  out->headphone_volume = options->headphone_volume;
  out->manual_route = options->manual_route;
}

static void print_error(const char *what, bd_av_error *error) {
  fprintf(stderr, "bd-av: %s: %s", what, bd_av_result_name(error->code));
  if (error->message[0] != '\0') {
    fprintf(stderr, " (%s)", error->message);
  }
  fputc('\n', stderr);
}

static int print_brightness(bd_av_brightness *brightness, bd_av_error *error) {
  printf("brightness.backend=%s\n",
         bd_av_brightness_backend_name(brightness->backend));
  printf("brightness.device=%s\n", brightness->device);
  printf("brightness.model=%s\n", brightness->model);
  printf("brightness.raw_range=%d..%d\n", brightness->raw_min,
         brightness->raw_max);
  printf("brightness.readable=%s\n", brightness->readable ? "yes" : "no");
  printf("brightness.writable=%s\n", brightness->writable ? "yes" : "no");
  int raw = 0;
  bd_av_result result = bd_av_brightness_get_raw(brightness, &raw, error);
  if (result == BD_AV_OK) {
    int percent = 0;
    result = bd_av_brightness_get(brightness, &percent, error);
    if (result == BD_AV_OK) {
      printf("brightness.current_raw=%d\n", raw);
      printf("brightness.current_percent=%d\n", percent);
    } else {
      print_error("brightness percentage", error);
    }
  } else {
    print_error("brightness value", error);
  }
  return result == BD_AV_OK ? 0 : 1;
}

static int print_controls(const bd_av_audio *audio) {
  printf("audio.control_count=%zu\n", bd_av_audio_control_count(audio));
  printf("audio.controls_truncated=%s\n",
         audio->controls_truncated ? "yes" : "no");
  for (size_t i = 0; i < bd_av_audio_control_count(audio); ++i) {
    const bd_av_control_info *control = bd_av_audio_control_at(audio, i);
    if (control == NULL) {
      continue;
    }
    printf("control.%zu.name=%s\n", i, control->name);
    printf("control.%zu.type=%s\n", i,
           bd_av_control_type_name(control->type));
    printf("control.%zu.count=%u\n", i, control->count);
    printf("control.%zu.range=%ld..%ld step=%ld\n", i, control->min,
           control->max, control->step);
    printf("control.%zu.access=%s%s\n", i,
           control->readable ? "r" : "-", control->writable ? "w" : "-");
  }
  return 0;
}

static int print_audio(bd_av_audio *audio, bd_av_error *error) {
  printf("audio.card_index=%d\n", audio->card_index);
  printf("audio.card_id=%s\n", audio->card_id);
  printf("audio.volume_control=%s bound=%s",
         audio->volume_control[0] != '\0' ? audio->volume_control : "(none)",
         audio->has_volume ? "yes" : "no");
  if (audio->has_volume) {
    printf(" range=%ld..%ld step=%ld inverted=%s", audio->volume_min,
           audio->volume_max, audio->volume_step,
           audio->volume_inverted ? "yes" : "no");
  }
  fputc('\n', stdout);
  printf("audio.mute=%s bound=%s\n",
         audio->internal.mute_is_alsa ? "alsa" : "sysfs",
         audio->has_mute ? "yes" : "no");
  printf("audio.route_bound=%s speaker=%s headphone=%s\n",
         audio->has_route ? "yes" : "no",
         audio->speaker_switch[0] != '\0' ? audio->speaker_switch : "(none)",
         audio->headphone_switch[0] != '\0' ? audio->headphone_switch
                                            : "(none)");
  printf("audio.headphone_volume_bound=%s\n",
         audio->has_headphone_volume ? "yes" : "no");
  printf("audio.jack_known=%s plugged=%s\n",
         audio->jack_known ? "yes" : "no",
         audio->headphone_plugged ? "yes" : "no");

  int percent = 0;
  if (audio->has_volume) {
    if (bd_av_audio_get_volume(audio, &percent, error) == BD_AV_OK) {
      printf("audio.volume_percent=%d\n", percent);
    } else {
      print_error("audio volume", error);
    }
  }
  bool muted = false;
  if (audio->has_mute) {
    if (bd_av_audio_get_muted(audio, &muted, error) == BD_AV_OK) {
      printf("audio.muted=%s\n", muted ? "yes" : "no");
    } else {
      print_error("audio mute", error);
    }
  }
  bd_av_route route = BD_AV_ROUTE_UNKNOWN;
  if (audio->has_route) {
    if (bd_av_audio_get_route(audio, &route, error) == BD_AV_OK) {
      printf("audio.route=%s\n", bd_av_route_name(route));
    } else {
      print_error("audio route", error);
    }
  }
  return 0;
}

static int command_probe(const options *options) {
  bd_av_brightness_config brightness_config;
  fill_config(options, &brightness_config);
  bd_av_brightness brightness;
  bd_av_error error;
  bd_av_error_clear(&error);
  bd_av_result brightness_result =
      bd_av_brightness_probe(&brightness, &brightness_config, &error);
  if (brightness_result == BD_AV_OK) {
    int ignored = print_brightness(&brightness, &error);
    (void)ignored;
  } else {
    print_error("brightness probe", &error);
    printf("brightness.backend=none\n");
  }

  bd_av_audio_config audio_config;
  fill_audio_config(options, &audio_config);
  bd_av_audio audio;
  bd_av_error_clear(&error);
  bd_av_result audio_result = bd_av_audio_open(&audio, &audio_config, &error);
  if (audio_result == BD_AV_OK) {
    print_audio(&audio, &error);
    print_controls(&audio);
    bd_av_audio_close(&audio);
  } else {
    print_error("audio open", &error);
    printf("audio.available=no\n");
  }
  return 0;
}

static int command_brightness(const options *options, int argc, char **argv) {
  bd_av_brightness_config config;
  fill_config(options, &config);
  bd_av_brightness brightness;
  bd_av_error error;
  bd_av_error_clear(&error);
  bd_av_result result = bd_av_brightness_probe(&brightness, &config, &error);
  if (result != BD_AV_OK) {
    print_error("brightness probe", &error);
    return 1;
  }
  if (argc < 1 || strcmp(argv[0], "get") == 0) {
    return print_brightness(&brightness, &error);
  }
  if (strcmp(argv[0], "set") != 0 || argc < 2) {
    usage(stderr);
    return 2;
  }
  if (strcmp(argv[1], "raw") == 0) {
    if (argc < 3) {
      usage(stderr);
      return 2;
    }
    int raw = 0;
    if (!parse_int(argv[2], &raw)) {
      fprintf(stderr, "bd-av: invalid raw value '%s'\n", argv[2]);
      return 2;
    }
    result = bd_av_brightness_set_raw(&brightness, raw, &error);
  } else {
    int percent = 0;
    if (!parse_int(argv[1], &percent)) {
      fprintf(stderr, "bd-av: invalid percent '%s'\n", argv[1]);
      return 2;
    }
    result = bd_av_brightness_set(&brightness, percent, &error);
  }
  if (result != BD_AV_OK) {
    print_error("brightness set", &error);
    return 1;
  }
  return print_brightness(&brightness, &error);
}

static int open_audio(const options *options, bd_av_audio *audio,
                      bd_av_error *error) {
  bd_av_audio_config config;
  fill_audio_config(options, &config);
  bd_av_error_clear(error);
  bd_av_result result = bd_av_audio_open(audio, &config, error);
  if (result != BD_AV_OK) {
    print_error("audio open", error);
    return 1;
  }
  return 0;
}

static int command_audio(const options *options, int argc, char **argv) {
  bd_av_audio audio;
  bd_av_error error;
  if (open_audio(options, &audio, &error) != 0) {
    return 1;
  }
  int status = 0;
  if (argc < 1 || strcmp(argv[0], "status") == 0) {
    status = print_audio(&audio, &error);
  } else if (strcmp(argv[0], "controls") == 0) {
    status = print_controls(&audio);
  } else if (strcmp(argv[0], "volume") == 0) {
    if (argc < 2) {
      status = print_audio(&audio, &error);
    } else {
      int percent = 0;
      if (!parse_int(argv[1], &percent)) {
        fprintf(stderr, "bd-av: invalid volume '%s'\n", argv[1]);
        status = 2;
      } else if (bd_av_audio_set_volume(&audio, percent, &error) !=
                 BD_AV_OK) {
        print_error("audio volume", &error);
        status = 1;
      }
    }
  } else if (strcmp(argv[0], "mute") == 0) {
    if (argc < 2) {
      bool muted = false;
      if (bd_av_audio_get_muted(&audio, &muted, &error) != BD_AV_OK) {
        print_error("audio mute", &error);
        status = 1;
      } else {
        printf("audio.muted=%s\n", muted ? "yes" : "no");
      }
    } else if (strcmp(argv[1], "on") == 0 ||
               strcmp(argv[1], "off") == 0) {
      if (bd_av_audio_set_muted(&audio, strcmp(argv[1], "on") == 0,
                                &error) != BD_AV_OK) {
        print_error("audio mute", &error);
        status = 1;
      }
    } else {
      usage(stderr);
      status = 2;
    }
  } else if (strcmp(argv[0], "route") == 0) {
    if (argc < 2) {
      bd_av_route route = BD_AV_ROUTE_UNKNOWN;
      if (bd_av_audio_get_route(&audio, &route, &error) != BD_AV_OK) {
        print_error("audio route", &error);
        status = 1;
      } else {
        printf("audio.route=%s\n", bd_av_route_name(route));
      }
    } else if (strcmp(argv[1], "speaker") == 0 ||
               strcmp(argv[1], "headphone") == 0 ||
               strcmp(argv[1], "auto") == 0) {
      bd_av_route route = strcmp(argv[1], "speaker") == 0
                              ? BD_AV_ROUTE_SPEAKER
                              : (strcmp(argv[1], "headphone") == 0
                                     ? BD_AV_ROUTE_HEADPHONE
                                     : BD_AV_ROUTE_AUTO);
      if (bd_av_audio_set_route(&audio, route, &error) != BD_AV_OK) {
        print_error("audio route", &error);
        status = 1;
      }
    } else {
      usage(stderr);
      status = 2;
    }
  } else {
    usage(stderr);
    status = 2;
  }
  bd_av_audio_close(&audio);
  return status;
}

int main(int argc, char **argv) {
  options options;
  int index = 0;
  if (parse_options(argc, argv, &options, &index) != 0) {
    return 2;
  }
  if (index >= argc) {
    usage(stderr);
    return 2;
  }
  const char *command = argv[index];
  if (strcmp(command, "probe") == 0) {
    return command_probe(&options);
  }
  if (strcmp(command, "brightness") == 0) {
    return command_brightness(&options, argc - index - 1, argv + index + 1);
  }
  if (strcmp(command, "audio") == 0) {
    return command_audio(&options, argc - index - 1, argv + index + 1);
  }
  usage(stderr);
  return 2;
}
