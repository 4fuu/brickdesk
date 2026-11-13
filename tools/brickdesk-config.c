#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brickdesk/config.h"

static void print_usage(FILE *stream) {
  fprintf(stream,
          "usage: brickdesk-config [--config-dir DIR] <command> [args]\n"
          "\n"
          "commands:\n"
          "  check                 validate every configuration file\n"
          "  get KEY               print an effective value\n"
          "  set KEY VALUE         write one user override to local.ini\n"
          "  unset KEY             remove a user override and save local.ini\n"
          "  dump                  print every effective value and its source\n"
          "\n"
          "options:\n"
          "  --config-dir DIR      override BRICKDESK_CONFIG_DIR\n"
          "  -h, --help            show this help\n");
}

static const char *severity_name(bd_config_severity severity) {
  return severity == BD_CONFIG_ERROR ? "error" : "warning";
}

static void print_diagnostic(const bd_config_diagnostic *diagnostic) {
  if (diagnostic->file != NULL) {
    fprintf(stderr, "%s:%d: %s: ", diagnostic->file, diagnostic->line,
            severity_name(diagnostic->severity));
  } else {
    fprintf(stderr, "%s: ", severity_name(diagnostic->severity));
  }
  if (diagnostic->key != NULL) {
    fprintf(stderr, "%s: ", diagnostic->key);
  }
  fprintf(stderr, "%s\n", diagnostic->message);
}

static bool format_value(const bd_config_value *value, char *buffer,
                         size_t buffer_size) {
  int written = -1;
  switch (value->type) {
    case BD_CONFIG_BOOL:
      written = snprintf(buffer, buffer_size, "%s",
                         value->boolean ? "true" : "false");
      break;
    case BD_CONFIG_INT:
      written = snprintf(buffer, buffer_size, "%ld", value->integer);
      break;
    case BD_CONFIG_DURATION:
      if (value->integer > 0 && value->integer % 1000 == 0) {
        written = snprintf(buffer, buffer_size, "%lds", value->integer / 1000);
      } else {
        written = snprintf(buffer, buffer_size, "%ldms", value->integer);
      }
      break;
    case BD_CONFIG_ENUM:
    case BD_CONFIG_STRING:
    case BD_CONFIG_ACCENT:
    case BD_CONFIG_COMBO:
      written = snprintf(buffer, buffer_size, "%s",
                         value->string != NULL ? value->string : "");
      break;
    default:
      return false;
  }
  return written >= 0 && (size_t)written < buffer_size;
}

static const char *layer_name(bd_config_layer layer) {
  switch (layer) {
    case BD_CONFIG_LAYER_SYSTEM:
      return "system";
    case BD_CONFIG_LAYER_USER:
      return "user";
    case BD_CONFIG_LAYER_DEFAULT:
    default:
      return "compiled";
  }
}

static int print_value(const bd_config *config, const char *key) {
  bd_config_value value;
  if (!bd_config_get(config, key, &value)) {
    fprintf(stderr, "brickdesk-config: unknown key '%s'\n", key);
    return 1;
  }
  char buffer[256];
  if (!format_value(&value, buffer, sizeof(buffer))) {
    fprintf(stderr, "brickdesk-config: cannot format '%s'\n", key);
    return 1;
  }
  printf("%s\n", buffer);
  return 0;
}

static int command_check(const bd_config *config) {
  size_t errors = 0;
  size_t warnings = 0;
  for (size_t i = 0; i < bd_config_diagnostic_count(config); ++i) {
    bd_config_diagnostic diagnostic;
    if (!bd_config_get_diagnostic(config, i, &diagnostic)) {
      continue;
    }
    print_diagnostic(&diagnostic);
    if (diagnostic.severity == BD_CONFIG_ERROR) {
      ++errors;
    } else {
      ++warnings;
    }
  }
  if (errors == 0) {
    printf("config: ok (%zu keys, %zu warning(s), dir=%s)\n",
           bd_config_key_count(config), warnings, bd_config_dir(config));
  } else {
    printf("config: %zu error(s), %zu warning(s)\n", errors, warnings);
  }
  return errors == 0 ? 0 : 1;
}

static int command_dump(const bd_config *config) {
  size_t count = bd_config_key_count(config);
  for (size_t i = 0; i < count; ++i) {
    const char *key = bd_config_key(config, i);
    if (key == NULL) {
      continue;
    }
    bd_config_value value;
    if (!bd_config_get(config, key, &value)) {
      continue;
    }
    char buffer[256];
    if (!format_value(&value, buffer, sizeof(buffer))) {
      continue;
    }
    int line = 0;
    const char *file = bd_config_source_file(config, key, &line);
    const char *layer = layer_name(bd_config_source_layer(config, key));
    if (file == NULL) {
      printf("%s = %s (%s)\n", key, buffer, layer);
    } else if (line > 0) {
      printf("%s = %s (%s:%s:%d)\n", key, buffer, layer, file, line);
    } else {
      printf("%s = %s (%s:%s)\n", key, buffer, layer, file);
    }
  }
  return 0;
}

static size_t print_errors(const bd_config *config) {
  size_t errors = 0;
  for (size_t i = 0; i < bd_config_diagnostic_count(config); ++i) {
    bd_config_diagnostic diagnostic;
    if (!bd_config_get_diagnostic(config, i, &diagnostic)) {
      continue;
    }
    if (diagnostic.severity == BD_CONFIG_ERROR) {
      print_diagnostic(&diagnostic);
      ++errors;
    }
  }
  return errors;
}

int main(int argc, char **argv) {
  const char *config_dir = NULL;
  const char *command = NULL;
  const char *arguments[2] = {NULL, NULL};
  size_t argument_count = 0;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (strcmp(arg, "--config-dir") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "brickdesk-config: --config-dir needs a directory\n");
        return 2;
      }
      config_dir = argv[++i];
      continue;
    }
    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      print_usage(stdout);
      return 0;
    }
    if (command == NULL) {
      command = arg;
      continue;
    }
    if (argument_count >= sizeof(arguments) / sizeof(arguments[0])) {
      fprintf(stderr, "brickdesk-config: too many arguments\n");
      print_usage(stderr);
      return 2;
    }
    arguments[argument_count++] = arg;
  }

  if (command == NULL) {
    print_usage(stderr);
    return 2;
  }

  bool known_command = strcmp(command, "check") == 0 ||
                       strcmp(command, "get") == 0 ||
                       strcmp(command, "set") == 0 ||
                       strcmp(command, "unset") == 0 ||
                       strcmp(command, "dump") == 0;
  if (!known_command) {
    fprintf(stderr, "brickdesk-config: unknown command '%s'\n", command);
    print_usage(stderr);
    return 2;
  }

  if (strcmp(command, "check") == 0 && argument_count != 0) {
    fprintf(stderr, "brickdesk-config: check takes no arguments\n");
    return 2;
  }
  if (strcmp(command, "get") == 0 && argument_count != 1) {
    fprintf(stderr, "brickdesk-config: get needs one key\n");
    return 2;
  }
  if (strcmp(command, "set") == 0 && argument_count != 2) {
    fprintf(stderr, "brickdesk-config: set needs a key and a value\n");
    return 2;
  }
  if (strcmp(command, "unset") == 0 && argument_count != 1) {
    fprintf(stderr, "brickdesk-config: unset needs one key\n");
    return 2;
  }
  if (strcmp(command, "dump") == 0 && argument_count != 0) {
    fprintf(stderr, "brickdesk-config: dump takes no arguments\n");
    return 2;
  }

  bd_config_options options;
  options.config_dir = config_dir;
  /* check must not migrate files; every other command may. */
  options.migrate = strcmp(command, "check") != 0;

  char error[256];
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  if (config == NULL) {
    fprintf(stderr, "brickdesk-config: %s\n", error);
    return 1;
  }

  int result = 0;
  if (strcmp(command, "check") != 0) {
    if (print_errors(config) > 0) {
      result = 1;
    }
  }
  if (strcmp(command, "check") == 0) {
    result = command_check(config);
  } else if (strcmp(command, "get") == 0) {
    int get_result = print_value(config, arguments[0]);
    if (get_result != 0) {
      result = get_result;
    }
  } else if (strcmp(command, "set") == 0) {
    if (!bd_config_set(config, arguments[0], arguments[1], error,
                       sizeof(error))) {
      fprintf(stderr, "brickdesk-config: %s\n", error);
      result = 1;
    } else if (!bd_config_save(config, error, sizeof(error))) {
      fprintf(stderr, "brickdesk-config: cannot save: %s\n", error);
      result = 1;
    } else {
      result = print_value(config, arguments[0]);
    }
  } else if (strcmp(command, "unset") == 0) {
    if (!bd_config_unset(config, arguments[0], error, sizeof(error))) {
      fprintf(stderr, "brickdesk-config: %s\n", error);
      result = 1;
    } else if (!bd_config_save(config, error, sizeof(error))) {
      fprintf(stderr, "brickdesk-config: cannot save: %s\n", error);
      result = 1;
    } else {
      result = print_value(config, arguments[0]);
    }
  } else {
    int dump_result = command_dump(config);
    if (dump_result != 0) {
      result = dump_result;
    }
  }

  bd_config_free(config);
  return result;
}
