#define _POSIX_C_SOURCE 200809L

#include "brickdesk/device_feedback.h"
#include "bd-feedback-play.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct options {
  const char *sysfs_root;
  const char *led;
  const char *led_name;
  bool led_auto;
  int led_max;
  const char *event;
  int rumble_max;
  int rumble_max_ms;
} options;

static void usage(FILE *out) {
  fprintf(out,
          "usage: bd-feedback [options] probe\n"
          "       bd-feedback [options] led get\n"
          "       bd-feedback [options] led set PERCENT\n"
          "       bd-feedback [options] led off\n"
          "       bd-feedback [options] rumble probe\n"
          "       bd-feedback [options] rumble play [PERCENT] [MS]\n"
          "       bd-feedback [options] rumble stop\n"
          "\n"
          "options:\n"
          "  --sysfs-root DIR       prefix before /sys (default real /sys)\n"
          "  --led PATH             explicit /sys/class/leds/<device> directory\n"
          "  --led-name NAME        exact LED class entry name\n"
          "  --led-auto             auto-discover the first usable LED\n"
          "  --led-max PERCENT      LED write ceiling, 1..100 (default 100)\n"
          "  --event PATH           explicit /dev/input/eventN with FF_RUMBLE\n"
          "  --rumble-max PERCENT   rumble strength ceiling, 1..100 (default 100)\n"
          "  --rumble-max-ms MS     rumble duration ceiling, 1..60000 (default 1000)\n"
          "\n"
          "probe is read-only.  led set/off and rumble play/stop perform only\n"
          "the explicitly requested standard Linux operation; vendor\n"
          "/sys/class/led_anim and /sys/class/motor paths are never written.\n"
          "rumble play keeps the event node open for MS, then stops/erases it;\n"
          "SIGINT/SIGTERM interrupts the wait and still cleans up.\n");
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
  if (value < -2147483647L || value > 2147483647L) {
    return false;
  }
  *out = (int)value;
  return true;
}

static int parse_options(int argc, char **argv, options *out,
                         int *command_index) {
  memset(out, 0, sizeof(*out));
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
    if (strcmp(arg, "--led-auto") == 0) {
      out->led_auto = true;
      continue;
    }
    if (i + 1 >= argc) {
      fprintf(stderr, "bd-feedback: %s needs a value\n", arg);
      return -1;
    }
    const char *value = argv[++i];
    if (strcmp(arg, "--sysfs-root") == 0) {
      out->sysfs_root = value;
    } else if (strcmp(arg, "--led") == 0) {
      out->led = value;
    } else if (strcmp(arg, "--led-name") == 0) {
      out->led_name = value;
    } else if (strcmp(arg, "--led-max") == 0) {
      if (!parse_int(value, &out->led_max) || out->led_max < 1 ||
          out->led_max > 100) {
        fprintf(stderr, "bd-feedback: invalid --led-max '%s'\n", value);
        return -1;
      }
    } else if (strcmp(arg, "--event") == 0) {
      out->event = value;
    } else if (strcmp(arg, "--rumble-max") == 0) {
      if (!parse_int(value, &out->rumble_max) || out->rumble_max < 1 ||
          out->rumble_max > 100) {
        fprintf(stderr, "bd-feedback: invalid --rumble-max '%s'\n", value);
        return -1;
      }
    } else if (strcmp(arg, "--rumble-max-ms") == 0) {
      if (!parse_int(value, &out->rumble_max_ms) ||
          out->rumble_max_ms < 1 || out->rumble_max_ms > 60000) {
        fprintf(stderr, "bd-feedback: invalid --rumble-max-ms '%s'\n", value);
        return -1;
      }
    } else {
      fprintf(stderr, "bd-feedback: unknown option '%s'\n", arg);
      return -1;
    }
  }
  return 0;
}

static void fill_led_config(const options *options,
                            bd_feedback_led_config *out) {
  memset(out, 0, sizeof(*out));
  out->sysfs_root = options->sysfs_root;
  out->led = options->led;
  out->name = options->led_name;
  out->allow_auto_discover = options->led_auto;
  out->max_percent = options->led_max;
  out->backend = BD_FEEDBACK_LED_AUTO;
}

static void fill_rumble_config(const options *options,
                               bd_feedback_rumble_config *out) {
  memset(out, 0, sizeof(*out));
  out->event = options->event;
  out->sysfs_root = options->sysfs_root;
  out->max_percent = options->rumble_max;
  out->max_duration_ms = options->rumble_max_ms;
  out->backend = BD_FEEDBACK_RUMBLE_AUTO;
}

static void print_error(const char *what, const bd_feedback_error *error) {
  fprintf(stderr, "bd-feedback: %s: %s", what,
          bd_feedback_result_name(error->code));
  if (error->message[0] != '\0') {
    fprintf(stderr, " (%s)", error->message);
  }
  fputc('\n', stderr);
}

static void print_vendor_led(const bd_feedback_led *led) {
  printf("led.vendor_led_anim=%s\n",
         led->vendor_led_anim_present ? "present" : "absent");
  if (led->vendor_led_blocker[0] != '\0') {
    printf("led.vendor_blocker=%s\n", led->vendor_led_blocker);
  }
}

static int print_led(const bd_feedback_led *led, bd_feedback_error *error) {
  printf("led.backend=%s\n", bd_feedback_led_backend_name(led->backend));
  printf("led.device=%s\n", led->device);
  printf("led.name=%s\n", led->name);
  printf("led.raw_range=%d..%d\n", led->raw_min, led->raw_max);
  printf("led.max_percent=%d\n", led->max_percent);
  printf("led.readable=%s\n", led->readable ? "yes" : "no");
  printf("led.writable=%s\n", led->writable ? "yes" : "no");
  print_vendor_led(led);
  int percent = 0;
  bd_feedback_result result = bd_feedback_led_get(led, &percent, error);
  if (result == BD_FEEDBACK_OK) {
    printf("led.current_percent=%d\n", percent);
  } else {
    print_error("led get", error);
    return 1;
  }
  return 0;
}

static void print_vendor_motor(const bd_feedback_rumble *rumble) {
  printf("rumble.vendor_motor_voltage=%s\n",
         rumble->vendor_motor_voltage_present ? "present" : "absent");
  printf("rumble.vendor_motor_enable=%s\n",
         rumble->vendor_motor_enable_present ? "present" : "absent");
  if (rumble->vendor_blocker[0] != '\0') {
    printf("rumble.vendor_blocker=%s\n", rumble->vendor_blocker);
  }
}

static int print_rumble(const bd_feedback_rumble *rumble) {
  printf("rumble.backend=%s\n",
         bd_feedback_rumble_backend_name(rumble->backend));
  printf("rumble.event=%s\n", rumble->event);
  printf("rumble.device_name=%s\n", rumble->device_name);
  printf("rumble.effects=%d\n", rumble->effects);
  printf("rumble.ff_rumble=%s\n", rumble->has_ff_rumble ? "yes" : "no");
  printf("rumble.max_percent=%d\n", rumble->max_percent);
  printf("rumble.max_duration_ms=%d\n", rumble->max_duration_ms);
  printf("rumble.active=%s\n", rumble->active ? "yes" : "no");
  print_vendor_motor(rumble);
  return 0;
}

static int command_probe(const options *options) {
  bd_feedback_led_config led_config;
  fill_led_config(options, &led_config);
  bd_feedback_led led;
  bd_feedback_error error;
  bd_feedback_error_clear(&error);
  bd_feedback_result led_result =
      bd_feedback_led_probe(&led, &led_config, &error);
  if (led_result == BD_FEEDBACK_OK) {
    print_led(&led, &error);
  } else {
    print_error("led probe", &error);
    printf("led.available=no\n");
    print_vendor_led(&led);
  }

  bd_feedback_rumble_config rumble_config;
  fill_rumble_config(options, &rumble_config);
  bd_feedback_rumble rumble;
  bd_feedback_error_clear(&error);
  bd_feedback_result rumble_result =
      bd_feedback_rumble_probe(&rumble, &rumble_config, &error);
  if (rumble_result == BD_FEEDBACK_OK) {
    print_rumble(&rumble);
    bd_feedback_rumble_close(&rumble, NULL);
  } else {
    print_error("rumble probe", &error);
    printf("rumble.available=no\n");
    print_vendor_motor(&rumble);
  }
  return 0;
}

#define BD_FEEDBACK_PLAY_SIGNAL_COUNT 4

static volatile sig_atomic_t g_play_signal;

static void play_signal_handler(int signum) {
  g_play_signal = signum;
}

static bool install_play_signal_handlers(
    struct sigaction old[BD_FEEDBACK_PLAY_SIGNAL_COUNT]) {
  static const int signals[BD_FEEDBACK_PLAY_SIGNAL_COUNT] = {
      SIGINT, SIGTERM, SIGHUP, SIGQUIT};
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = play_signal_handler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  for (int i = 0; i < BD_FEEDBACK_PLAY_SIGNAL_COUNT; ++i) {
    if (sigaction(signals[i], &action, &old[i]) != 0) {
      for (int j = 0; j < i; ++j) {
        sigaction(signals[j], &old[j], NULL);
      }
      return false;
    }
  }
  return true;
}

static void restore_play_signal_handlers(
    struct sigaction old[BD_FEEDBACK_PLAY_SIGNAL_COUNT]) {
  static const int signals[BD_FEEDBACK_PLAY_SIGNAL_COUNT] = {
      SIGINT, SIGTERM, SIGHUP, SIGQUIT};
  for (int i = 0; i < BD_FEEDBACK_PLAY_SIGNAL_COUNT; ++i) {
    sigaction(signals[i], &old[i], NULL);
  }
}

static int command_led(const options *options, int argc, char **argv) {
  bd_feedback_led_config config;
  fill_led_config(options, &config);
  bd_feedback_led led;
  bd_feedback_error error;
  bd_feedback_error_clear(&error);
  bd_feedback_result result = bd_feedback_led_probe(&led, &config, &error);
  if (result != BD_FEEDBACK_OK) {
    print_error("led probe", &error);
    return 1;
  }
  if (argc < 1 || strcmp(argv[0], "get") == 0) {
    return print_led(&led, &error);
  }
  if (strcmp(argv[0], "off") == 0) {
    result = bd_feedback_led_set(&led, 0, &error);
  } else if (strcmp(argv[0], "set") == 0 && argc >= 2) {
    int percent = 0;
    if (!parse_int(argv[1], &percent)) {
      fprintf(stderr, "bd-feedback: invalid percent '%s'\n", argv[1]);
      return 2;
    }
    result = bd_feedback_led_set(&led, percent, &error);
  } else {
    usage(stderr);
    return 2;
  }
  if (result != BD_FEEDBACK_OK) {
    print_error("led set", &error);
    return 1;
  }
  return print_led(&led, &error);
}

static int command_rumble(const options *options, int argc, char **argv) {
  bd_feedback_rumble_config config;
  fill_rumble_config(options, &config);
  bd_feedback_rumble rumble;
  bd_feedback_error error;
  bd_feedback_error_clear(&error);
  bd_feedback_result result =
      bd_feedback_rumble_probe(&rumble, &config, &error);
  if (result != BD_FEEDBACK_OK) {
    print_error("rumble probe", &error);
    print_vendor_motor(&rumble);
    return 1;
  }
  int status = 0;
  if (argc < 1 || strcmp(argv[0], "probe") == 0) {
    status = print_rumble(&rumble);
  } else if (strcmp(argv[0], "play") == 0) {
    int strength = 50;
    int duration = 200;
    if (argc >= 2 && !parse_int(argv[1], &strength)) {
      fprintf(stderr, "bd-feedback: invalid strength '%s'\n", argv[1]);
      status = 2;
    } else if (argc >= 3 && !parse_int(argv[2], &duration)) {
      fprintf(stderr, "bd-feedback: invalid duration '%s'\n", argv[2]);
      status = 2;
    } else {
      struct sigaction old[BD_FEEDBACK_PLAY_SIGNAL_COUNT];
      g_play_signal = 0;
      if (!install_play_signal_handlers(old)) {
        fprintf(stderr, "bd-feedback: cannot install signal handlers: %s\n",
                strerror(errno));
        bd_feedback_error close_error;
        bd_feedback_error_clear(&close_error);
        if (bd_feedback_rumble_close(&rumble, &close_error) !=
            BD_FEEDBACK_OK) {
          print_error("rumble close", &close_error);
        }
        return 1;
      }
      bd_feedback_cli_play_report report;
      bd_feedback_cli_play_status play_status = bd_feedback_cli_play_and_wait(
          &rumble, strength, duration, bd_feedback_cli_wait_ms,
          (void *)&g_play_signal, &report);
      int signum = (int)g_play_signal;
      restore_play_signal_handlers(old);
      if (report.play_result != BD_FEEDBACK_OK) {
        print_error("rumble play", &report.play_error);
      }
      if (report.wait_result == BD_FEEDBACK_CLI_WAIT_INTERRUPTED) {
        fprintf(stderr,
                "bd-feedback: rumble play interrupted by signal %d; stopped "
                "and cleaned up\n",
                signum);
      } else if (report.wait_result == BD_FEEDBACK_CLI_WAIT_ERROR) {
        fprintf(stderr, "bd-feedback: rumble play: playback wait failed\n");
      }
      if (report.close_result != BD_FEEDBACK_OK) {
        print_error("rumble close", &report.close_error);
      }
      if (play_status == BD_FEEDBACK_CLI_PLAY_INTERRUPTED) {
        return signum > 0 ? 128 + signum : 130;
      }
      return play_status == BD_FEEDBACK_CLI_PLAY_OK ? 0 : 1;
    }
  } else if (strcmp(argv[0], "stop") == 0) {
    if (bd_feedback_rumble_stop(&rumble, &error) != BD_FEEDBACK_OK) {
      print_error("rumble stop", &error);
      status = 1;
    }
  } else {
    usage(stderr);
    status = 2;
  }
  bd_feedback_error close_error;
  bd_feedback_error_clear(&close_error);
  if (bd_feedback_rumble_close(&rumble, &close_error) != BD_FEEDBACK_OK) {
    print_error("rumble close", &close_error);
    status = 1;
  }
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
  if (strcmp(command, "led") == 0) {
    return command_led(&options, argc - index - 1, argv + index + 1);
  }
  if (strcmp(command, "rumble") == 0) {
    return command_rumble(&options, argc - index - 1, argv + index + 1);
  }
  usage(stderr);
  return 2;
}
