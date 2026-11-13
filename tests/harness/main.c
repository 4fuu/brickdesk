#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "brickdesk/config.h"
#include "brickdesk/gfx.h"
#include "brickdesk/platform.h"
#include "brickdesk/shell.h"
#include "scenario.h"

#define BD_PATH_MAX 1024

typedef struct harness_options {
  const char *scenario_path;
  const char *out_dir;
  const char *config_dir;
  const char *demo;
  const char *shim;
  const char *inputd;
  const char *replay;
  bool verbose;
} harness_options;

static void print_usage(FILE *stream) {
  fprintf(stream,
          "usage: bd_harness --scenario FILE --out-dir DIR [--config-dir DIR]\n"
          "                  [--demo PATH --shim PATH --inputd PATH --replay FILE]\n"
          "                  [--verbose]\n"
          "\n"
          "Replays a JSON input scenario and writes screenshots as PNG.\n");
}

static bool make_directory(const char *path) {
  if (mkdir(path, 0777) == 0) {
    return true;
  }
  return errno == EEXIST;
}

static bool join_path(char *out, size_t out_size, const char *directory,
                      const char *name) {
  int written = snprintf(out, out_size, "%s/%s", directory, name);
  return written > 0 && (size_t)written < out_size;
}

static void sleep_ms(int milliseconds) {
  struct timespec delay;
  delay.tv_sec = milliseconds / 1000;
  delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
  nanosleep(&delay, NULL);
}

static bool write_file(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs(text, file) >= 0;
  if (fclose(file) != 0) {
    ok = false;
  }
  return ok;
}

static pid_t start_inputd(const char *inputd, const char *config_dir,
                          const char *socket_path, const char *replay,
                          const char *log_path) {
  pid_t pid = fork();
  if (pid != 0) {
    return pid;
  }
  int log = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (log >= 0) {
    dup2(log, STDOUT_FILENO);
    dup2(log, STDERR_FILENO);
    close(log);
  }
  if (config_dir != NULL) {
    execl(inputd, inputd, "--config-dir", config_dir, "--socket", socket_path,
          "--replay", replay, "--verbose", (char *)NULL);
  } else {
    execl(inputd, inputd, "--socket", socket_path, "--replay", replay,
          "--verbose", (char *)NULL);
  }
  _exit(127);
}

static void stop_inputd(pid_t pid) {
  if (pid <= 0) {
    return;
  }
  kill(pid, SIGTERM);
  for (int attempt = 0; attempt < 100; ++attempt) {
    int status = 0;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD)) {
      return;
    }
    sleep_ms(20);
  }
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
}

static bool wait_for_path(const char *path, int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 10) {
    if (access(path, F_OK) == 0) {
      return true;
    }
    sleep_ms(10);
  }
  return false;
}

static void write_demo_manifest(const char *path, const char *demo,
                                const char *shim, const char *input_socket) {
  static const char *format =
      "{\n"
      "  \"schema\": 1,\n"
      "  \"id\": \"dev.brickdesk.demo\",\n"
      "  \"name\": \"Brickdesk Demo\",\n"
      "  \"subtitle\": \"Session and menu shim\",\n"
      "  \"type\": \"app\",\n"
      "  \"category\": \"app\",\n"
      "  \"exec\": [\"%s\"],\n"
      "  \"cwd\": \".\",\n"
      "  \"env\": {\n"
      "    \"LD_PRELOAD\": \"%s\",\n"
      "    \"BRICKDESK_INPUT_SOCKET\": \"%s\",\n"
      "    \"BRICKDESK_DEMO_WIDTH\": \"1024\",\n"
      "    \"BRICKDESK_DEMO_HEIGHT\": \"768\",\n"
      "    \"BRICKDESK_DEMO_FIXED_TIME_MS\": \"1000\",\n"
      "    \"BRICKDESK_DEMO_STABLE_FRAME\": \"1\"\n"
      "  },\n"
      "  \"accent\": \"#7B5A8C\",\n"
      "  \"capabilities\": {\n"
      "    \"pause\": true, \"resume\": true, \"save_state\": true,\n"
      "    \"load_state\": true, \"screenshot\": true,\n"
      "    \"background\": false, \"display_release\": true\n"
      "  },\n"
      "  \"hooks\": {\n"
      "    \"pause\": {\"type\":\"ipc\",\"method\":\"app.pause\",\"timeout_ms\":500},\n"
      "    \"resume\": {\"type\":\"ipc\",\"method\":\"app.resume\",\"timeout_ms\":2000},\n"
      "    \"save\": {\"type\":\"ipc\",\"method\":\"state.save\",\"timeout_ms\":2000},\n"
      "    \"quit\": {\"type\":\"ipc\",\"method\":\"app.quit\",\"timeout_ms\":1000}\n"
      "  },\n"
      "  \"menu\": {\"mode\": \"overlay\"},\n"
      "  \"state\": {\"path\": \"state.txt\", \"format\": \"text\", \"max_slots\": 1}\n"
      "}\n";
  int needed = snprintf(NULL, 0, format, demo, shim, input_socket);
  char *manifest = needed > 0 ? (char *)malloc((size_t)needed + 1) : NULL;
  if (manifest != NULL) {
    snprintf(manifest, (size_t)needed + 1, format, demo, shim, input_socket);
  }
  if (manifest == NULL || !write_file(path, manifest)) {
    fprintf(stderr, "harness: cannot write %s\n", path);
  }
  free(manifest);
}

static bool prepare_apps(const harness_options *options, char *apps_dir,
                         size_t apps_size, char *socket_path,
                         size_t socket_size) {
  if (!join_path(apps_dir, apps_size, options->out_dir, "apps") ||
      !make_directory(apps_dir)) {
    return false;
  }
  if (!join_path(socket_path, socket_size, options->out_dir, "inputd.sock")) {
    return false;
  }
  if (options->demo == NULL || options->shim == NULL) {
    return true;
  }
  char app_dir[BD_PATH_MAX];
  char manifest[BD_PATH_MAX];
  if (!join_path(app_dir, sizeof(app_dir), apps_dir, "demo") ||
      !make_directory(app_dir) ||
      !join_path(manifest, sizeof(manifest), app_dir, "app.json")) {
    return false;
  }
  write_demo_manifest(manifest, options->demo, options->shim, socket_path);
  return true;
}

static int run_scenario(const bd_scenario *scenario,
                        const harness_options *options) {
  bd_platform_config config;
  config.title = "brickdesk harness";
  config.width = scenario->width;
  config.height = scenario->height;
  config.hidden = true;

  bd_platform *platform = bd_platform_create(&config);
  if (platform == NULL) {
    return 1;
  }

  bd_gl_info gl;
  bd_platform_gl_info(platform, &gl);
  bd_gl_print_info(&gl, options->verbose);
  printf("window: %dx%d\n", bd_platform_width(platform),
         bd_platform_height(platform));

  if (bd_platform_width(platform) != scenario->width ||
      bd_platform_height(platform) != scenario->height) {
    fprintf(stderr, "harness: scenario asks for %dx%d but drawable is %dx%d\n",
            scenario->width, scenario->height, bd_platform_width(platform),
            bd_platform_height(platform));
    bd_platform_destroy(platform);
    return 1;
  }

  bd_gfx *gfx = bd_gfx_create(scenario->width, scenario->height);
  if (gfx == NULL) {
    fprintf(stderr, "harness: renderer creation failed\n");
    bd_platform_destroy(platform);
    return 1;
  }

  bd_config_options config_options;
  config_options.config_dir = options->config_dir;
  config_options.migrate = false;
  char config_error[256] = {0};
  bd_config *settings =
      bd_config_load(&config_options, config_error, sizeof(config_error));
  if (settings == NULL) {
    fprintf(stderr, "harness: config: %s\n", config_error);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }
  long cover_cache_mb = bd_config_get_int(settings, "ui.cover_cache_mb", 16);
  if (cover_cache_mb < 1) {
    cover_cache_mb = 1;
  }
  bd_gfx_image_cache_set_budget(gfx, (size_t)cover_cache_mb * 1024u * 1024u);

  char apps_dir[BD_PATH_MAX];
  char socket_path[BD_PATH_MAX];
  if (!prepare_apps(options, apps_dir, sizeof(apps_dir), socket_path,
                    sizeof(socket_path))) {
    fprintf(stderr, "harness: cannot prepare app directory\n");
    bd_config_free(settings);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }

  pid_t inputd_pid = -1;
  if (options->inputd != NULL && options->replay != NULL) {
    char log_path[BD_PATH_MAX];
    if (!join_path(log_path, sizeof(log_path), options->out_dir, "inputd.log")) {
      log_path[0] = '\0';
    }
    inputd_pid = start_inputd(options->inputd, options->config_dir, socket_path,
                              options->replay, log_path);
    if (inputd_pid <= 0 || !wait_for_path(socket_path, 3000)) {
      fprintf(stderr, "harness: inputd did not start\n");
      stop_inputd(inputd_pid);
      bd_config_free(settings);
      bd_gfx_destroy(gfx);
      bd_platform_destroy(platform);
      return 1;
    }
  }

  bd_shell_config shell_config;
  memset(&shell_config, 0, sizeof(shell_config));
  shell_config.platform = platform;
  shell_config.gfx = gfx;
  shell_config.settings = settings;
  shell_config.apps_dir = apps_dir;
  shell_config.input_socket = inputd_pid > 0 ? socket_path : NULL;
  shell_config.menu_shim = options->shim;
  shell_config.verbose = options->verbose;
  bd_shell *shell = bd_shell_create(&shell_config);
  if (shell == NULL) {
    fprintf(stderr, "harness: cannot create desktop loop\n");
    stop_inputd(inputd_pid);
    bd_config_free(settings);
    bd_gfx_destroy(gfx);
    bd_platform_destroy(platform);
    return 1;
  }
  printf("input: source=%s\n", bd_shell_input_source(shell));

  printf("scenario: %s (%dx%d, fixed_dt_ms=%d, steps=%d)\n", scenario->name,
         scenario->width, scenario->height, scenario->fixed_dt_ms,
         scenario->step_count);

  unsigned long frame = 0;
  int captures = 0;
  int result = 0;

  for (int i = 0; i < scenario->step_count; ++i) {
    const bd_step *step = &scenario->steps[i];
    uint64_t test_time = (uint64_t)frame * (uint64_t)scenario->fixed_dt_ms;
    switch (step->type) {
      case BD_STEP_WAIT:
        frame += (unsigned long)step->frames;
        bd_shell_test_advance(shell,
                              (uint64_t)frame * (uint64_t)scenario->fixed_dt_ms);
        break;

      case BD_STEP_KEY:
        printf("key: frame=%lu key=%s action=%s\n", frame,
               bd_key_name(step->key), step->pressed ? "down" : "up");
        bd_shell_test_key(shell, step->key, step->pressed, test_time);
        break;

      case BD_STEP_RUN:
        printf("run: frame=%lu ms=%d\n", frame, step->ms);
        if (!bd_shell_run_for(shell, step->ms)) {
          result = bd_shell_quit_requested(shell) ? 0 : 1;
        }
        frame += (unsigned long)((step->ms + scenario->fixed_dt_ms - 1) /
                                 scenario->fixed_dt_ms);
        if (result != 0) {
          break;
        }
        break;

      case BD_STEP_SCREENSHOT: {
        bd_shell_test_advance(shell,
                              (uint64_t)frame * (uint64_t)scenario->fixed_dt_ms);
        if (step->settle) {
          int quiet = 0;
          for (int wait = 0; wait < 300 && quiet < 2; ++wait) {
            bd_shell_iterate(shell, 0);
            if (bd_shell_has_pending_images(shell)) {
              quiet = 0;
            } else {
              ++quiet;
            }
            bd_event event;
            bd_platform_wait_event(platform, &event, 5);
          }
          bd_shell_render(shell);
        } else {
          /* One frame at the current transition timestamp. */
          bd_shell_iterate(shell, 0);
          bd_shell_render(shell);
        }

        char path[BD_PATH_MAX];
        char filename[80];
        snprintf(filename, sizeof(filename), "%s.png", step->screenshot);
        if (!join_path(path, sizeof(path), options->out_dir, filename)) {
          fprintf(stderr, "harness: screenshot path is too long\n");
          result = 1;
          break;
        }
        if (!bd_gfx_save_png(gfx, path)) {
          fprintf(stderr, "harness: capture '%s' failed: %s\n",
                  step->screenshot, bd_gfx_last_error(gfx));
          result = 1;
          break;
        }
        printf("capture: frame=%lu name=%s file=%s\n", frame, step->screenshot,
               path);
        ++captures;
        break;
      }
    }
    if (result != 0) {
      break;
    }
  }

  printf("done: frames=%lu captures=%d\n", frame, captures);

  bd_shell_destroy(shell);
  stop_inputd(inputd_pid);
  bd_config_free(settings);
  bd_gfx_destroy(gfx);
  bd_platform_destroy(platform);
  return result;
}

int main(int argc, char **argv) {
  harness_options options;
  memset(&options, 0, sizeof(options));

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (strcmp(arg, "--scenario") == 0 && i + 1 < argc) {
      options.scenario_path = argv[++i];
    } else if (strcmp(arg, "--out-dir") == 0 && i + 1 < argc) {
      options.out_dir = argv[++i];
    } else if (strcmp(arg, "--config-dir") == 0 && i + 1 < argc) {
      options.config_dir = argv[++i];
    } else if (strcmp(arg, "--demo") == 0 && i + 1 < argc) {
      options.demo = argv[++i];
    } else if (strcmp(arg, "--shim") == 0 && i + 1 < argc) {
      options.shim = argv[++i];
    } else if (strcmp(arg, "--inputd") == 0 && i + 1 < argc) {
      options.inputd = argv[++i];
    } else if (strcmp(arg, "--replay") == 0 && i + 1 < argc) {
      options.replay = argv[++i];
    } else if (strcmp(arg, "--verbose") == 0) {
      options.verbose = true;
    } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      print_usage(stdout);
      return 0;
    } else {
      fprintf(stderr, "bd_harness: unknown or incomplete option: %s\n", arg);
      print_usage(stderr);
      return 2;
    }
  }

  if (options.scenario_path == NULL || options.out_dir == NULL) {
    fprintf(stderr, "bd_harness: --scenario and --out-dir are required\n");
    print_usage(stderr);
    return 2;
  }

  bd_scenario scenario;
  char error[512] = {0};
  if (!bd_scenario_load(options.scenario_path, &scenario, error,
                        sizeof(error))) {
    fprintf(stderr, "bd_harness: %s: %s\n", options.scenario_path, error);
    return 1;
  }

  if (!make_directory(options.out_dir)) {
    fprintf(stderr, "bd_harness: cannot create output directory '%s': %s\n",
            options.out_dir, strerror(errno));
    return 1;
  }

  return run_scenario(&scenario, &options);
}
