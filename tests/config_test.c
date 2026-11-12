#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "brickdesk/config.h"

#define PATH_SIZE 1024

static int g_failures;

#define CHECK(condition, ...)                  \
  do {                                         \
    if (!(condition)) {                        \
      fprintf(stderr, "config_test: ");        \
      fprintf(stderr, __VA_ARGS__);            \
      fprintf(stderr, "\n");                   \
      ++g_failures;                            \
    }                                          \
  } while (0)

static char *make_temp_dir(void) {
  char template[] = "/tmp/bd_config_test.XXXXXX";
  char *created = mkdtemp(template);
  if (created == NULL) {
    fprintf(stderr, "config_test: mkdtemp failed: %s\n", strerror(errno));
    exit(1);
  }
  char *copy = malloc(strlen(created) + 1);
  if (copy == NULL) {
    exit(1);
  }
  strcpy(copy, created);
  return copy;
}

static bool make_path(const char *dir, const char *name, char *out,
                      size_t out_size) {
  int written = snprintf(out, out_size, "%s/%s", dir, name);
  return written > 0 && (size_t)written < out_size;
}

static bool write_file(const char *dir, const char *name, const char *content) {
  char path[PATH_SIZE];
  if (!make_path(dir, name, path, sizeof(path))) {
    return false;
  }
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    fprintf(stderr, "config_test: cannot write %s: %s\n", path,
            strerror(errno));
    return false;
  }
  bool ok = fputs(content, file) >= 0;
  if (fclose(file) != 0) {
    ok = false;
  }
  return ok;
}

static char *read_file(const char *dir, const char *name) {
  char path[PATH_SIZE];
  if (!make_path(dir, name, path, sizeof(path))) {
    return NULL;
  }
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long length = ftell(file);
  if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  char *content = malloc((size_t)length + 1);
  if (content == NULL) {
    fclose(file);
    return NULL;
  }
  size_t got = fread(content, 1, (size_t)length, file);
  fclose(file);
  if (got != (size_t)length) {
    free(content);
    return NULL;
  }
  content[got] = '\0';
  return content;
}

static bool file_exists(const char *dir, const char *name) {
  char path[PATH_SIZE];
  struct stat status;
  return make_path(dir, name, path, sizeof(path)) &&
         stat(path, &status) == 0 && S_ISREG(status.st_mode);
}

static bool text_contains(const char *text, const char *needle) {
  return text != NULL && strstr(text, needle) != NULL;
}

static size_t text_count(const char *text, const char *needle) {
  if (text == NULL || needle == NULL || *needle == '\0') {
    return 0;
  }
  size_t count = 0;
  const char *cursor = text;
  size_t length = strlen(needle);
  while ((cursor = strstr(cursor, needle)) != NULL) {
    ++count;
    cursor += length;
  }
  return count;
}

static bool directory_has_temporary(const char *dir) {
  DIR *stream = opendir(dir);
  if (stream == NULL) {
    return false;
  }
  bool found = false;
  struct dirent *entry;
  while ((entry = readdir(stream)) != NULL) {
    if (strstr(entry->d_name, ".tmp.") != NULL) {
      found = true;
      break;
    }
  }
  closedir(stream);
  return found;
}

static void cleanup_dir(const char *dir) {
  DIR *stream = opendir(dir);
  if (stream != NULL) {
    struct dirent *entry;
    while ((entry = readdir(stream)) != NULL) {
      if (strcmp(entry->d_name, ".") == 0 ||
          strcmp(entry->d_name, "..") == 0) {
        continue;
      }
      char path[PATH_SIZE];
      if (make_path(dir, entry->d_name, path, sizeof(path))) {
        remove(path);
      }
    }
    closedir(stream);
  }
  rmdir(dir);
}

static bool has_diagnostic(const bd_config *config, bd_config_severity severity,
                           const char *file_part, int line,
                           const char *key, const char *message_part) {
  for (size_t i = 0; i < bd_config_diagnostic_count(config); ++i) {
    bd_config_diagnostic diagnostic;
    if (!bd_config_get_diagnostic(config, i, &diagnostic)) {
      continue;
    }
    if (diagnostic.severity != severity) {
      continue;
    }
    if (file_part != NULL &&
        (diagnostic.file == NULL ||
         strstr(diagnostic.file, file_part) == NULL)) {
      continue;
    }
    if (line >= 0 && diagnostic.line != line) {
      continue;
    }
    if (key != NULL &&
        (diagnostic.key == NULL || strcmp(diagnostic.key, key) != 0)) {
      continue;
    }
    if (message_part != NULL &&
        (diagnostic.message == NULL ||
         strstr(diagnostic.message, message_part) == NULL)) {
      continue;
    }
    return true;
  }
  return false;
}

static size_t count_diagnostics(const bd_config *config,
                                bd_config_severity severity) {
  size_t count = 0;
  for (size_t i = 0; i < bd_config_diagnostic_count(config); ++i) {
    bd_config_diagnostic diagnostic;
    if (bd_config_get_diagnostic(config, i, &diagnostic) &&
        diagnostic.severity == severity) {
      ++count;
    }
  }
  return count;
}

static void print_diagnostics(const bd_config *config) {
  for (size_t i = 0; i < bd_config_diagnostic_count(config); ++i) {
    bd_config_diagnostic diagnostic;
    if (!bd_config_get_diagnostic(config, i, &diagnostic)) {
      continue;
    }
    fprintf(stderr, "  %s:%d: %s: %s\n",
            diagnostic.file != NULL ? diagnostic.file : "<compiled>",
            diagnostic.line,
            diagnostic.severity == BD_CONFIG_ERROR ? "error" : "warning",
            diagnostic.message);
  }
}

static void test_repo_defaults(void) {
  unsetenv("BRICKDESK_CONFIG_DIR");
  const char *source = bd_config_default_dir();
  char *dir = make_temp_dir();
  static const char *const files[] = {"brickdesk.ini", "perf.ini",
                                      "hotkeys.ini", "input.ini"};
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
    char *content = read_file(source, files[i]);
    CHECK(content != NULL, "repo defaults: cannot read %s/%s", source,
          files[i]);
    if (content != NULL) {
      CHECK(write_file(dir, files[i], content),
            "repo defaults: cannot copy %s", files[i]);
      free(content);
    }
  }

  char error[256];
  setenv("BRICKDESK_CONFIG_DIR", dir, 1);
  CHECK(strcmp(bd_config_default_dir(), dir) == 0,
        "repo defaults: BRICKDESK_CONFIG_DIR override");
  unsetenv("BRICKDESK_CONFIG_DIR");
  bd_config_options options = {dir, true};
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "repo defaults: load failed: %s", error);
  if (config == NULL) {
    return;
  }
  CHECK(bd_config_diagnostic_count(config) == 0,
        "repo defaults: expected no diagnostics, got %zu",
        bd_config_diagnostic_count(config));
  if (bd_config_diagnostic_count(config) != 0) {
    print_diagnostics(config);
  }
  CHECK(strcmp(bd_config_get_string(config, "theme.mode", ""), "dark") == 0,
        "repo defaults: theme.mode");
  CHECK(strcmp(bd_config_get_string(config, "theme.accent", ""), "persimmon") ==
            0,
        "repo defaults: theme.accent");
  CHECK(strcmp(bd_config_get_string(config, "theme.ambient", ""), "baked") == 0,
        "repo defaults: theme.ambient");
  CHECK(bd_config_get_int(config, "input.long_press_ms", -1) == 500,
        "repo defaults: input.long_press_ms");
  CHECK(bd_config_get_int(config, "hotkey.menu.hold_ms", -1) == 600,
        "repo defaults: hotkey.menu.hold_ms");
  CHECK(bd_config_get_int(config, "hotkey.hint.hold_ms", -1) == 400,
        "repo defaults: hotkey.hint.hold_ms");
  CHECK(bd_config_get_int(config, "profile.performance.max_khz", -1) == 1800000,
        "repo defaults: profile.performance.max_khz");
  CHECK(strcmp(bd_config_get_string(config, "device.TrimUI Gamepad.A", ""),
               "BTN_EAST") == 0,
        "repo defaults: device.TrimUI Gamepad.A");
  CHECK(strcmp(bd_config_dir(config), dir) == 0,
        "repo defaults: config dir");
  CHECK(bd_config_source_layer(config, "theme.mode") == BD_CONFIG_LAYER_SYSTEM,
        "repo defaults: theme.mode layer");
  bd_config_free(config);
  cleanup_dir(dir);
  free(dir);
  printf("ok: repo defaults\n");
}

static void test_layer_order(void) {
  char *dir = make_temp_dir();
  CHECK(write_file(dir, "brickdesk.ini",
                   "[meta]\n"
                   "schema_version = 1\n"
                   "\n"
                   "[theme]\n"
                   "mode = light\n"
                   "\n"
                   "[display]\n"
                   "brightness = 40\n"),
        "layer order: write system file");
  CHECK(write_file(dir, "local.ini",
                   "[meta]\n"
                   "schema_version = 1\n"
                   "\n"
                   "[theme]\n"
                   "mode = dark\n"
                   "\n"
                   "[display]\n"
                   "brightness = 80\n"),
        "layer order: write user file");

  char error[256];
  bd_config_options options = {dir, true};
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "layer order: load: %s", error);
  if (config != NULL) {
    CHECK(strcmp(bd_config_get_string(config, "theme.mode", ""), "dark") == 0,
          "layer order: local.ini must win (got %s)",
          bd_config_get_string(config, "theme.mode", "<unset>"));
    CHECK(bd_config_get_int(config, "display.brightness", -1) == 80,
          "layer order: brightness must come from local.ini");
    CHECK(bd_config_source_layer(config, "theme.mode") == BD_CONFIG_LAYER_USER,
          "layer order: theme.mode should be user layer");
    int line = 0;
    const char *file = bd_config_source_file(config, "theme.mode", &line);
    CHECK(file != NULL && strstr(file, "local.ini") != NULL,
          "layer order: source file");
    CHECK(line == 5, "layer order: expected local.ini:5, got %d", line);
    CHECK(bd_config_diagnostic_count(config) == 0,
          "layer order: unexpected diagnostics");
    bd_config_free(config);
  }
  cleanup_dir(dir);
  free(dir);
  printf("ok: layer order\n");
}

static void test_bad_lines(void) {
  char *dir = make_temp_dir();
  CHECK(write_file(dir, "brickdesk.ini",
                   "[meta]\n"
                   "schema_version = 1\n"
                   "\n"
                   "[input]\n"
                   "long_press_ms = 700\n"
                   "this is not a key\n"),
        "bad lines: write syntax file");
  char error[256];
  bd_config_options options = {dir, true};
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "bad lines: load: %s", error);
  if (config != NULL) {
    CHECK(has_diagnostic(config, BD_CONFIG_ERROR, "brickdesk.ini", 6, NULL,
                         "syntax"),
          "bad lines: expected syntax error at brickdesk.ini:6");
    CHECK(bd_config_get_int(config, "input.long_press_ms", -1) == 700,
          "bad lines: valid values before the bad line still apply");
    bd_config_free(config);
  }
  cleanup_dir(dir);
  free(dir);

  dir = make_temp_dir();
  options.config_dir = dir;
  CHECK(write_file(dir, "brickdesk.ini",
                   "[meta]\n"
                   "schema_version = 1\n"
                   "\n"
                   "[input]\n"
                   "long_press_ms = 700\n"),
        "bad value: write system file");
  CHECK(write_file(dir, "local.ini",
                   "[meta]\n"
                   "schema_version = 1\n"
                   "\n"
                   "[input]\n"
                   "long_press_ms = fast\n"),
        "bad value: write user file");
  config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "bad value: load: %s", error);
  if (config != NULL) {
    CHECK(has_diagnostic(config, BD_CONFIG_ERROR, "local.ini", 5,
                         "input.long_press_ms", "integer"),
          "bad value: expected integer error at local.ini:5");
    CHECK(bd_config_get_int(config, "input.long_press_ms", -1) == 700,
          "bad value: lower valid layer must stay in effect");
    bd_config_free(config);
  }
  cleanup_dir(dir);
  free(dir);
  printf("ok: bad lines\n");
}

static void test_range_and_enum(void) {
  char *dir = make_temp_dir();
  CHECK(write_file(dir, "local.ini",
                   "[meta]\n"
                   "schema_version = 1\n"
                   "\n"
                   "[input]\n"
                   "long_press_ms = 99999\n"
                   "\n"
                   "[theme]\n"
                   "mode = blue\n"
                   "ambient = wood\n"),
        "range: write file");
  char error[256];
  bd_config_options options = {dir, true};
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "range: load: %s", error);
  if (config != NULL) {
    CHECK(has_diagnostic(config, BD_CONFIG_ERROR, "local.ini", 5,
                         "input.long_press_ms", "between 100 and 3000"),
          "range: expected range error at local.ini:5");
    CHECK(has_diagnostic(config, BD_CONFIG_ERROR, "local.ini", 8, "theme.mode",
                         "dark, light, auto"),
          "range: expected enum error at local.ini:8");
    CHECK(has_diagnostic(config, BD_CONFIG_ERROR, "local.ini", 9,
                         "theme.ambient", "baked, solid"),
          "range: expected ambient enum error at local.ini:9");
    CHECK(bd_config_get_int(config, "input.long_press_ms", -1) == 500,
          "range: invalid value must fall back to the compiled default");
    CHECK(strcmp(bd_config_get_string(config, "theme.mode", ""), "dark") == 0,
          "range: invalid enum must fall back to the default");
    bd_config_free(config);
  }
  cleanup_dir(dir);
  free(dir);

  dir = make_temp_dir();
  options.config_dir = dir;
  CHECK(write_file(dir, "local.ini",
                   "[meta]\n"
                   "schema_version = 1\n"
                   "\n"
                   "[input]\n"
                   "long_press_ms = 100\n"
                   "\n"
                   "[theme]\n"
                   "mode = LIGHT\n"
                   "accent = #A1B2C3\n"),
        "boundaries: write file");
  config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "boundaries: load: %s", error);
  if (config != NULL) {
    CHECK(bd_config_diagnostic_count(config) == 0,
          "boundaries: unexpected diagnostics (%zu)",
          bd_config_diagnostic_count(config));
    CHECK(bd_config_get_int(config, "input.long_press_ms", -1) == 100,
          "boundaries: min must be accepted");
    CHECK(strcmp(bd_config_get_string(config, "theme.mode", ""), "light") == 0,
          "boundaries: enum is case-insensitive and canonicalized");
    CHECK(strcmp(bd_config_get_string(config, "theme.accent", ""),
                 "#a1b2c3") == 0,
          "boundaries: custom accent is lowercased");
    bd_config_free(config);
  }
  cleanup_dir(dir);
  free(dir);
  printf("ok: range and enum\n");
}

static void test_theme_options(void) {
  char *dir = make_temp_dir();
  CHECK(write_file(dir, "local.ini",
                   "[meta]\n"
                   "schema_version = 1\n"
                   "\n"
                   "[theme]\n"
                   "mode = auto\n"
                   "accent = teal\n"
                   "ambient = solid\n"),
        "theme: write file");
  char error[256];
  bd_config_options options = {dir, true};
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "theme: load: %s", error);
  if (config != NULL) {
    CHECK(bd_config_diagnostic_count(config) == 0,
          "theme: unexpected diagnostics");
    CHECK(strcmp(bd_config_get_string(config, "theme.mode", ""), "auto") == 0,
          "theme: mode auto");
    CHECK(strcmp(bd_config_get_string(config, "theme.accent", ""), "teal") == 0,
          "theme: accent teal");
    CHECK(strcmp(bd_config_get_string(config, "theme.ambient", ""), "solid") ==
              0,
          "theme: ambient solid");
    bd_config_free(config);
  }
  cleanup_dir(dir);
  free(dir);
  printf("ok: theme options\n");
}

static void test_accent(void) {
  CHECK(strcmp(bd_config_accent_hex("persimmon", true), "#e07a5f") == 0,
        "accent: persimmon dark");
  CHECK(strcmp(bd_config_accent_hex("persimmon", false), "#ae432b") == 0,
        "accent: persimmon light");
  CHECK(strcmp(bd_config_accent_hex("coral", true), "#fd5c47") == 0,
        "accent: coral dark");
  CHECK(bd_config_accent_hex("orange", true) == NULL,
        "accent: unknown preset must return NULL");
  CHECK(bd_config_accent_hex("#123456", true) != NULL,
        "accent: custom hex must resolve");
  CHECK(bd_config_accent_hex("#12345", true) == NULL,
        "accent: short hex must be rejected");

  char *dir = make_temp_dir();
  CHECK(write_file(dir, "local.ini",
                   "[meta]\n"
                   "schema_version = 1\n"
                   "\n"
                   "[theme]\n"
                   "accent = #GG0000\n"),
        "accent: write invalid file");
  char error[256];
  bd_config_options options = {dir, true};
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "accent: load: %s", error);
  if (config != NULL) {
    CHECK(has_diagnostic(config, BD_CONFIG_ERROR, "local.ini", 5,
                         "theme.accent", "RRGGBB"),
          "accent: expected hex error at local.ini:5");
    CHECK(strcmp(bd_config_get_string(config, "theme.accent", ""),
                 "persimmon") == 0,
          "accent: invalid value must fall back to persimmon");
    bd_config_free(config);
  }
  cleanup_dir(dir);
  free(dir);

  dir = make_temp_dir();
  options.config_dir = dir;
  CHECK(write_file(dir, "local.ini",
                   "[meta]\n"
                   "schema_version = 1\n"
                   "\n"
                   "[theme]\n"
                   "accent = orange\n"),
        "accent: write unknown name");
  config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "accent: load unknown: %s", error);
  if (config != NULL) {
    CHECK(has_diagnostic(config, BD_CONFIG_ERROR, "local.ini", 5,
                         "theme.accent", "preset"),
          "accent: expected preset error at local.ini:5");
    bd_config_free(config);
  }
  cleanup_dir(dir);
  free(dir);
  printf("ok: accent\n");
}

static void test_combo(void) {
  char *dir = make_temp_dir();
  CHECK(write_file(dir, "hotkeys.ini",
                   "[meta]\n"
                   "schema_version = 1\n"
                   "\n"
                   "[hotkey.a]\n"
                   "combo = L1+R1+Start\n"
                   "action = builtin.screenshot\n"
                   "\n"
                   "[hotkey.b]\n"
                   "combo = Select,Select\n"
                   "action = ui.quick_panel\n"
                   "\n"
                   "[hotkey.c]\n"
                   "combo = Menu(hold:800)\n"
                   "action = menu.open\n"),
        "combo: write valid file");
  char error[256];
  bd_config_options options = {dir, true};
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "combo: load: %s", error);
  if (config != NULL) {
    CHECK(bd_config_diagnostic_count(config) == 0,
          "combo: valid file produced %zu diagnostics",
          bd_config_diagnostic_count(config));
    CHECK(strcmp(bd_config_get_string(config, "hotkey.a.combo", ""),
                 "L1+R1+START") == 0,
          "combo: canonical chord");
    CHECK(strcmp(bd_config_get_string(config, "hotkey.b.combo", ""),
                 "SELECT,SELECT") == 0,
          "combo: canonical sequence");
    CHECK(strcmp(bd_config_get_string(config, "hotkey.c.combo", ""),
                 "MENU(hold:800)") == 0,
          "combo: canonical hold modifier");
    bd_config_free(config);
  }
  cleanup_dir(dir);
  free(dir);

  dir = make_temp_dir();
  options.config_dir = dir;
  CHECK(write_file(dir, "hotkeys.ini",
                   "[meta]\n"
                   "schema_version = 1\n"
                   "\n"
                   "[hotkey.bad]\n"
                   "combo = Menu++X\n"
                   "action = menu.open\n"
                   "\n"
                   "[hotkey.unknown]\n"
                   "combo = NOPE+X\n"
                   "action = menu.open\n"
                   "\n"
                   "[hotkey.dup]\n"
                   "combo = A+A\n"
                   "action = menu.open\n"
                   "\n"
                   "[hotkey.missing]\n"
                   "combo = Menu\n"
                   "\n"
                   "[hotkey.empty]\n"
                   "combo = Menu\n"
                   "action =\n"),
        "combo: write invalid file");
  config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "combo: load invalid: %s", error);
  if (config != NULL) {
    CHECK(has_diagnostic(config, BD_CONFIG_ERROR, "hotkeys.ini", 5,
                         "hotkey.bad.combo", "empty key"),
          "combo: expected empty-key error at hotkeys.ini:5");
    CHECK(has_diagnostic(config, BD_CONFIG_ERROR, "hotkeys.ini", 9,
                         "hotkey.unknown.combo", "unknown key"),
          "combo: expected unknown-key error at hotkeys.ini:9");
    CHECK(has_diagnostic(config, BD_CONFIG_ERROR, "hotkeys.ini", 13,
                         "hotkey.dup.combo", "duplicate"),
          "combo: expected duplicate-key error at hotkeys.ini:13");
    CHECK(has_diagnostic(config, BD_CONFIG_ERROR, "hotkeys.ini", 17,
                         "hotkey.missing.action", "missing required"),
          "combo: expected missing-action error at hotkeys.ini:17");
    CHECK(has_diagnostic(config, BD_CONFIG_ERROR, "hotkeys.ini", 21,
                         "hotkey.empty.action", "must not be empty"),
          "combo: expected empty-action error at hotkeys.ini:21");
    CHECK(bd_config_get_string(config, "hotkey.bad.combo", NULL) == NULL,
          "combo: invalid combo must not become effective");
    bd_config_free(config);
  }
  cleanup_dir(dir);
  free(dir);
  printf("ok: combo\n");
}

static void change_callback(void *user, const char *key) {
  int *count = user;
  ++*count;
  (void)key;
}

static void test_set_save(void) {
  char *dir = make_temp_dir();
  char error[256];
  bd_config_options options = {dir, true};
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "set: load: %s", error);
  if (config == NULL) {
    cleanup_dir(dir);
    free(dir);
    return;
  }

  int changes = 0;
  bd_config_set_change_callback(config, change_callback, &changes);
  CHECK(bd_config_set(config, "theme.mode", "light", error, sizeof(error)),
        "set: valid value: %s", error);
  CHECK(changes == 1, "set: callback should fire once (got %d)", changes);
  CHECK(bd_config_set(config, "theme.mode", "light", error, sizeof(error)),
        "set: same value: %s", error);
  CHECK(changes == 1, "set: same value should not fire the callback");
  CHECK(bd_config_set(config, "theme.mode", "dark", error, sizeof(error)),
        "set: second change: %s", error);
  CHECK(changes == 2, "set: callback should fire twice (got %d)", changes);
  CHECK(!bd_config_set(config, "theme.mode", "blue", error, sizeof(error)),
        "set: invalid enum must fail");
  CHECK(!bd_config_set(config, "no.such.key", "x", error, sizeof(error)),
        "set: unknown key must fail");
  CHECK(strcmp(bd_config_get_string(config, "theme.mode", ""), "dark") == 0,
        "set: invalid value must not change the effective config");

  CHECK(bd_config_save(config, error, sizeof(error)), "set: save: %s", error);
  char *content = read_file(dir, "local.ini");
  CHECK(content != NULL, "set: local.ini was not created");
  CHECK(text_contains(content, "[meta]"), "set: local.ini meta section");
  CHECK(text_contains(content, "schema_version = 1"),
        "set: local.ini schema version");
  CHECK(text_contains(content, "[theme]"), "set: local.ini theme section");
  CHECK(text_contains(content, "mode = dark"), "set: local.ini mode");
  CHECK(!text_contains(content, "[input]"),
        "set: local.ini must not contain untouched sections");
  CHECK(!text_contains(content, "long_press_ms"),
        "set: local.ini must not contain untouched keys");
  free(content);

  CHECK(bd_config_set(config, "display.brightness", "80", error, sizeof(error)),
        "set: brightness: %s", error);
  CHECK(bd_config_save(config, error, sizeof(error)), "set: second save: %s",
        error);
  CHECK(file_exists(dir, "local.ini.bak"), "set: backup file is missing");
  char *backup = read_file(dir, "local.ini.bak");
  CHECK(backup != NULL && text_contains(backup, "mode = dark"),
        "set: backup should hold the previous local.ini");
  free(backup);
  bd_config_free(config);

  config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "set: reload: %s", error);
  if (config != NULL) {
    CHECK(strcmp(bd_config_get_string(config, "theme.mode", ""), "dark") == 0,
          "set: reloaded mode");
    CHECK(bd_config_get_int(config, "display.brightness", -1) == 80,
          "set: reloaded brightness");
    CHECK(bd_config_source_layer(config, "display.brightness") ==
              BD_CONFIG_LAYER_USER,
          "set: reloaded brightness layer");
    bd_config_free(config);
  }
  cleanup_dir(dir);
  free(dir);

  dir = make_temp_dir();
  options.config_dir = dir;
  CHECK(write_file(dir, "local.ini",
                   "[meta]\n"
                   "schema_version = 1\n"
                   "custom = keep\n"
                   "\n"
                   "[widget]\n"
                   "foo = bar\n"),
        "set: write unknown key file");
  config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "set: load unknown key: %s", error);
  if (config != NULL) {
    CHECK(has_diagnostic(config, BD_CONFIG_WARNING, "local.ini", 3,
                         "meta.custom", "unknown key"),
          "set: unknown meta key should warn");
    CHECK(has_diagnostic(config, BD_CONFIG_WARNING, "local.ini", 6,
                         "widget.foo", "unknown key"),
          "set: unknown key should warn");
    CHECK(bd_config_set(config, "theme.mode", "light", error, sizeof(error)),
          "set: set with unknown key: %s", error);
    CHECK(bd_config_save(config, error, sizeof(error)),
          "set: save with unknown key: %s", error);
    content = read_file(dir, "local.ini");
    CHECK(text_contains(content, "[widget]"),
          "set: unknown section should be preserved");
    CHECK(text_contains(content, "foo = bar"),
          "set: unknown key should be preserved");
    CHECK(text_count(content, "[meta]") == 1,
          "set: local.ini must contain exactly one [meta]");
    CHECK(text_count(content, "schema_version = 1") == 1,
          "set: local.ini must contain exactly one schema_version");
    CHECK(text_contains(content, "custom = keep"),
          "set: unknown meta key should be preserved");
    free(content);
    bd_config_free(config);
  }
  cleanup_dir(dir);
  free(dir);
  printf("ok: set and save\n");
}

static void test_atomic_failure(void) {
  char *dir = make_temp_dir();
  const char *original =
      "[meta]\n"
      "schema_version = 1\n"
      "\n"
      "[theme]\n"
      "mode = dark\n";
  CHECK(write_file(dir, "local.ini", original),
        "atomic: write original file");
  char error[256];
  bd_config_options options = {dir, true};
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "atomic: load: %s", error);
  if (config != NULL) {
    CHECK(bd_config_set(config, "theme.mode", "light", error, sizeof(error)),
          "atomic: set: %s", error);
    CHECK(setenv("BRICKDESK_CONFIG_FAIL_WRITE", "5", 1) == 0,
          "atomic: set failpoint");
    CHECK(!bd_config_save(config, error, sizeof(error)),
          "atomic: save must fail at the failpoint");
    unsetenv("BRICKDESK_CONFIG_FAIL_WRITE");
    char *after = read_file(dir, "local.ini");
    CHECK(after != NULL && strcmp(after, original) == 0,
          "atomic: original file changed after a failed write");
    free(after);
    CHECK(!directory_has_temporary(dir),
          "atomic: temporary file was left behind");
    CHECK(file_exists(dir, "local.ini.bak"),
          "atomic: backup should exist after the failed write");
    CHECK(bd_config_save(config, error, sizeof(error)),
          "atomic: save after the failpoint: %s", error);
    after = read_file(dir, "local.ini");
    CHECK(text_contains(after, "mode = light"),
          "atomic: successful save did not update local.ini");
    free(after);
    bd_config_free(config);
  }
  cleanup_dir(dir);
  free(dir);
  printf("ok: atomic write\n");
}

static void test_migration(void) {
  char *dir = make_temp_dir();
  const char *legacy =
      "[theme]\n"
      "mode = light\n"
      "\n"
      "[display]\n"
      "brightness = 55\n";
  CHECK(write_file(dir, "local.ini", legacy), "migrate: write legacy file");
  char error[256];
  bd_config_options options = {dir, true};
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "migrate: load: %s", error);
  if (config != NULL) {
    CHECK(count_diagnostics(config, BD_CONFIG_ERROR) == 0,
          "migrate: unexpected errors");
    CHECK(strcmp(bd_config_get_string(config, "theme.mode", ""), "light") == 0,
          "migrate: value must survive migration");
    CHECK(bd_config_get_int(config, "display.brightness", -1) == 55,
          "migrate: brightness must survive migration");
    bd_config_free(config);
  }
  char *migrated = read_file(dir, "local.ini");
  CHECK(text_contains(migrated, "[meta]"),
        "migrate: local.ini missing [meta] after migration");
  CHECK(text_contains(migrated, "schema_version = 1"),
        "migrate: local.ini missing schema_version after migration");
  CHECK(text_contains(migrated, "mode = light"),
        "migrate: migrated file lost values");
  free(migrated);
  char *backup = read_file(dir, "local.ini.bak.v0");
  CHECK(backup != NULL && strcmp(backup, legacy) == 0,
        "migrate: original file was not saved as local.ini.bak.v0");
  free(backup);

  config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "migrate: second load: %s", error);
  if (config != NULL) {
    CHECK(count_diagnostics(config, BD_CONFIG_ERROR) == 0 &&
              count_diagnostics(config, BD_CONFIG_WARNING) == 0,
          "migrate: second load should be clean");
    bd_config_free(config);
  }
  cleanup_dir(dir);
  free(dir);

  dir = make_temp_dir();
  options.config_dir = dir;
  CHECK(write_file(dir, "local.ini", legacy),
        "migrate dry run: write legacy file");
  bd_config_options dry = {dir, false};
  config = bd_config_load(&dry, error, sizeof(error));
  CHECK(config != NULL, "migrate dry run: load: %s", error);
  if (config != NULL) {
    CHECK(has_diagnostic(config, BD_CONFIG_WARNING, "local.ini", 0, NULL,
                         "migration needed"),
          "migrate dry run: expected a migration warning");
    bd_config_free(config);
  }
  char *unchanged = read_file(dir, "local.ini");
  CHECK(unchanged != NULL && strcmp(unchanged, legacy) == 0,
        "migrate dry run: check must not rewrite the file");
  free(unchanged);
  cleanup_dir(dir);
  free(dir);
  printf("ok: migration\n");
}

static void test_newer_version(void) {
  char *dir = make_temp_dir();
  CHECK(write_file(dir, "local.ini",
                   "[meta]\n"
                   "schema_version = 99\n"
                   "\n"
                   "[theme]\n"
                   "mode = light\n"),
        "newer: write file");
  char error[256];
  bd_config_options options = {dir, true};
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "newer: load: %s", error);
  if (config != NULL) {
    CHECK(has_diagnostic(config, BD_CONFIG_ERROR, "local.ini", 0, NULL,
                         "newer"),
          "newer: expected a newer-version error");
    CHECK(bd_config_read_only(config), "newer: config should be read-only");
    CHECK(!bd_config_set(config, "theme.mode", "dark", error, sizeof(error)),
          "newer: set must fail in read-only mode");
    CHECK(!bd_config_save(config, error, sizeof(error)),
          "newer: save must fail in read-only mode");
    bd_config_free(config);
  }
  cleanup_dir(dir);
  free(dir);

  dir = make_temp_dir();
  options.config_dir = dir;
  CHECK(write_file(dir, "local.ini",
                   "[meta]\n"
                   "schema_version = nope\n"
                   "\n"
                   "[theme]\n"
                   "mode = light\n"),
        "newer: write invalid version");
  config = bd_config_load(&options, error, sizeof(error));
  CHECK(config != NULL, "newer: load invalid: %s", error);
  if (config != NULL) {
    CHECK(has_diagnostic(config, BD_CONFIG_ERROR, "local.ini", 2,
                         "meta.schema_version", "integer"),
          "newer: expected invalid-version error at local.ini:2");
    CHECK(bd_config_read_only(config),
          "newer: invalid version should be read-only");
    bd_config_free(config);
  }
  cleanup_dir(dir);
  free(dir);
  printf("ok: newer version\n");
}

int main(void) {
  unsetenv("BRICKDESK_CONFIG_DIR");
  unsetenv("BRICKDESK_CONFIG_FAIL_WRITE");

  test_repo_defaults();
  test_layer_order();
  test_bad_lines();
  test_range_and_enum();
  test_theme_options();
  test_accent();
  test_combo();
  test_set_save();
  test_atomic_failure();
  test_migration();
  test_newer_version();

  if (g_failures != 0) {
    fprintf(stderr, "config_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("config_test: all checks passed\n");
  return 0;
}
