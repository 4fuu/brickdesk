#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

/*
 * End-to-end host test for the M1-09 menu shim.
 *
 * Phase 1 launches apps/demo through bd_session under LD_PRELOAD, while a real
 * brickdesk-inputd replays the Menu long-press, navigation and confirmation
 * keys. The shim opens the overlay, writes the last frame, grabs input, sends
 * the background and quit menu actions through the session menu socket, and
 * releases the grab. The test checks the session state transitions, the state
 * file, the last-frame header, inputd's grab/ungrab log, and copies six
 * deterministic menu screenshots for baseline comparison.  Three more phases
 * run apps/demo directly with a fake backlight (writable, read-only, absent) so
 * the production device backend is exercised without the test stub.
 *
 * The final phase runs tests/menu_state_app under the same shim and a replay
 * that opens and closes the menu; that app verifies the GL state and pixels
 * after the overlay has gone.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "brickdesk/app.h"

#define TEST_EVENTS 64
#define TEST_ACTIONS 8

typedef struct observation {
  bd_session_state last_state;
  bool foreground_seen;
  bool background_seen;
  bool exited;
  int exit_code;
  int term_signal;
  bool crashed;
  bool want_background;
  bool want_quit;
  char actions[TEST_ACTIONS][32];
  int action_count;
  int event_count;
} observation;

typedef struct test_context {
  bd_session *session;
  const char *app_id;
  observation obs;
} test_context;

static int g_failures;

static void check(bool condition, const char *format, ...) {
  if (condition) {
    return;
  }
  ++g_failures;
  fprintf(stderr, "menu_test: FAIL: ");
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
}

static void sleep_ms(int milliseconds) {
  struct timespec delay;
  delay.tv_sec = milliseconds / 1000;
  delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
  nanosleep(&delay, NULL);
}

static bool join_path(char *out, size_t out_size, const char *dir,
                      const char *name) {
  size_t dir_length = strlen(dir);
  size_t name_length = strlen(name);
  if (dir_length + 1 + name_length + 1 > out_size) {
    return false;
  }
  memcpy(out, dir, dir_length);
  out[dir_length] = '/';
  memcpy(out + dir_length + 1, name, name_length + 1);
  return true;
}

static bool make_dir(const char *path) {
  return mkdir(path, 0755) == 0 || errno == EEXIST;
}

static bool write_file(const char *path, const char *content) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs(content, file) >= 0;
  if (fclose(file) != 0) {
    ok = false;
  }
  return ok;
}

static char *read_file(const char *path) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  char *content = (char *)malloc((size_t)size + 1);
  if (content == NULL) {
    fclose(file);
    return NULL;
  }
  size_t got = fread(content, 1, (size_t)size, file);
  fclose(file);
  content[got] = '\0';
  return content;
}

static bool copy_file(const char *from, const char *to) {
  FILE *source = fopen(from, "rb");
  if (source == NULL) {
    return false;
  }
  FILE *target = fopen(to, "wb");
  if (target == NULL) {
    fclose(source);
    return false;
  }
  char buffer[8192];
  bool ok = true;
  for (;;) {
    size_t amount = fread(buffer, 1, sizeof(buffer), source);
    if (amount > 0 && fwrite(buffer, 1, amount, target) != amount) {
      ok = false;
      break;
    }
    if (amount < sizeof(buffer)) {
      if (ferror(source)) {
        ok = false;
      }
      break;
    }
  }
  fclose(source);
  if (fclose(target) != 0) {
    ok = false;
  }
  return ok;
}

static bool wait_for_file(const char *path, int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 25) {
    if (access(path, R_OK) == 0) {
      return true;
    }
    sleep_ms(25);
  }
  return false;
}

static int remove_entry(const char *path, const struct stat *status, int flag,
                        struct FTW *tree) {
  (void)status;
  (void)flag;
  (void)tree;
  return remove(path);
}

static void remove_tree(const char *path) {
  if (path == NULL) {
    return;
  }
  nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static void on_event(void *user, const bd_session_event *event) {
  test_context *context = (test_context *)user;
  observation *obs = &context->obs;
  ++obs->event_count;
  switch (event->kind) {
    case BD_SESSION_EVENT_STATE:
      obs->last_state = event->state;
      if (getenv("MENU_TEST_DEBUG") != NULL) {
        fprintf(stderr, "menu_test: state %s -> %s\n",
                bd_session_state_name(event->previous),
                bd_session_state_name(event->state));
      }
      if (event->state == BD_SESSION_FOREGROUND) {
        obs->foreground_seen = true;
      }
      if (event->state == BD_SESSION_BACKGROUND) {
        obs->background_seen = true;
      }
      break;
    case BD_SESSION_EVENT_EXIT:
      obs->exited = true;
      obs->exit_code = event->exit_code;
      obs->term_signal = event->term_signal;
      break;
    case BD_SESSION_EVENT_CRASH:
      obs->exited = true;
      obs->crashed = true;
      obs->exit_code = event->exit_code;
      obs->term_signal = event->term_signal;
      break;
    case BD_SESSION_EVENT_MENU_ACTION:
      if (getenv("MENU_TEST_DEBUG") != NULL) {
        fprintf(stderr, "menu_test: menu action %s\n", event->method);
      }
      if (obs->action_count < TEST_ACTIONS && event->method != NULL) {
        snprintf(obs->actions[obs->action_count], sizeof(obs->actions[0]),
                 "%s", event->method);
        ++obs->action_count;
      }
      if (event->method != NULL && strcmp(event->method, "background") == 0) {
        obs->want_background = true;
      } else if (event->method != NULL &&
                 strcmp(event->method, "quit") == 0) {
        obs->want_quit = true;
      }
      break;
    default:
      break;
  }
}

static pid_t start_inputd(const char *inputd, const char *config,
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
  execl(inputd, inputd, "--config-dir", config, "--socket", socket_path,
        "--replay", replay, "--verbose", (char *)NULL);
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

static char *find_last_frame(const char *directory) {
  char pattern[PATH_MAX];
  join_path(pattern, sizeof(pattern), directory, "last-frame-*.bin");
  glob_t matches;
  memset(&matches, 0, sizeof(matches));
  char *result = NULL;
  if (glob(pattern, 0, NULL, &matches) == 0 && matches.gl_pathc > 0) {
    result = strdup(matches.gl_pathv[0]);
  }
  globfree(&matches);
  return result;
}

static void write_demo_manifest(const char *path, const char *demo,
                                const char *shim, const char *capture_dir,
                                const char *input_socket) {
  static const char *format =
           "{\n"
           "  \"schema\": 1,\n"
           "  \"id\": \"dev.brickdesk.demo\",\n"
           "  \"name\": \"Brickdesk Demo\",\n"
           "  \"type\": \"app\",\n"
           "  \"category\": \"app\",\n"
           "  \"exec\": [\"%s\"],\n"
           "  \"cwd\": \".\",\n"
           "  \"env\": {\n"
           "    \"LD_PRELOAD\": \"%s\",\n"
           "    \"BRICKDESK_INPUT_SOCKET\": \"%s\",\n"
           "    \"BRICKDESK_MENU_CAPTURE\": \"%s\",\n"
           "    \"BRICKDESK_DEMO_WIDTH\": \"1024\",\n"
           "    \"BRICKDESK_DEMO_HEIGHT\": \"768\",\n"
           "    \"BRICKDESK_DEMO_FIXED_TIME_MS\": \"1000\",\n"
           "    \"BRICKDESK_MENU_SYSTEM\": \"stub\",\n"
           "    \"BRICKDESK_MENU_DEBUG\": \"1\"\n"
           "  },\n"
           "  \"capabilities\": {\n"
           "    \"pause\": true, \"resume\": true, \"save_state\": true,\n"
           "    \"load_state\": true, \"screenshot\": true,\n"
           "    \"background\": false, \"display_release\": true\n"
           "  },\n"
           "  \"hooks\": {\n"
           "    \"pause\": {\"type\":\"ipc\",\"method\":\"app.pause\","
           "\"timeout_ms\":500},\n"
           "    \"resume\": {\"type\":\"ipc\",\"method\":\"app.resume\","
           "\"timeout_ms\":2000},\n"
           "    \"save\": {\"type\":\"ipc\",\"method\":\"state.save\","
           "\"timeout_ms\":2000},\n"
           "    \"quit\": {\"type\":\"ipc\",\"method\":\"app.quit\","
           "\"timeout_ms\":1000}\n"
           "  },\n"
           "  \"menu\": {\"mode\": \"overlay\"},\n"
           "  \"state\": {\"path\": \"state.txt\", \"format\": \"text\","
           "\"max_slots\": 1}\n"
           "}\n";
  int needed = snprintf(NULL, 0, format, demo, shim, input_socket, capture_dir);
  char *manifest = needed > 0 ? (char *)malloc((size_t)needed + 1) : NULL;
  if (manifest != NULL) {
    snprintf(manifest, (size_t)needed + 1, format, demo, shim, input_socket,
             capture_dir);
  }
  if (manifest == NULL || !write_file(path, manifest)) {
    fprintf(stderr, "menu_test: cannot write %s\n", path);
  }
  free(manifest);
}

static void check_last_frame(const char *run_dir) {
  char *path = find_last_frame(run_dir);
  if (path == NULL) {
    check(false, "last-frame file was not written");
    return;
  }
  bd_app_last_frame frame;
  memset(&frame, 0, sizeof(frame));
  char error[256] = {0};
  if (!bd_app_last_frame_open(path, &frame, error, sizeof(error))) {
    check(false, "last frame: %s", error);
    free(path);
    return;
  }
  check(frame.header.width == 1024 && frame.header.height == 768,
        "last frame is %ux%u, expected 1024x768", frame.header.width,
        frame.header.height);
  check(frame.header.format == BD_APP_PIXEL_RGBA8888,
        "last frame format is %u", frame.header.format);
  uint8_t *pixels = (uint8_t *)malloc((size_t)frame.header.stride *
                                      frame.header.height);
  bd_app_last_frame_header copied;
  if (pixels == NULL ||
      !bd_app_last_frame_copy(&frame, pixels,
                              (size_t)frame.header.stride * frame.header.height,
                              &copied, error, sizeof(error))) {
    check(false, "last frame copy: %s", error);
  } else {
    check(copied.frame_seq > 0, "last frame sequence was not published");
  }
  free(pixels);
  bd_app_last_frame_close(&frame);
  free(path);
}

static void run_demo_phase(const char *demo, const char *inputd,
                           const char *shim, const char *config,
                           const char *assets, const char *out_dir,
                           bool quit_phase) {
  (void)assets;
  char root[256];
  snprintf(root, sizeof(root), "/tmp/bd-menu-test-XXXXXX");
  if (mkdtemp(root) == NULL) {
    check(false, "mkdtemp: %s", strerror(errno));
    return;
  }
  char run_dir[PATH_MAX];
  char state_dir[PATH_MAX];
  char cap_dir[PATH_MAX];
  char app_dir[PATH_MAX];
  char socket_path[PATH_MAX];
  char replay_path[PATH_MAX];
  char log_path[PATH_MAX];
  char manifest_path[PATH_MAX];
  char state_file[PATH_MAX];
  join_path(run_dir, sizeof(run_dir), root, "run");
  join_path(state_dir, sizeof(state_dir), root, "state");
  join_path(cap_dir, sizeof(cap_dir), root, "capture");
  join_path(app_dir, sizeof(app_dir), root, "apps/demo");
  join_path(socket_path, sizeof(socket_path), root, "inputd.sock");
  join_path(replay_path, sizeof(replay_path), root, "replay.txt");
  join_path(log_path, sizeof(log_path), root, "inputd.log");
  join_path(manifest_path, sizeof(manifest_path), app_dir, "app.json");
  join_path(state_file, sizeof(state_file), state_dir,
              "dev.brickdesk.demo/state.txt");
  char apps_dir[PATH_MAX];
  join_path(apps_dir, sizeof(apps_dir), root, "apps");
  make_dir(run_dir);
  make_dir(state_dir);
  make_dir(cap_dir);
  make_dir(apps_dir);
  make_dir(app_dir);

  const char *replay =
      quit_phase
          ? "5000 key MENU down\n"
            "5600 key MENU up\n"
            "6200 key DOWN down\n"
            "6250 key DOWN up\n"
            "6600 key DOWN down\n"
            "6650 key DOWN up\n"
            "7000 key A down\n"
            "7050 key A up\n"
            "9000 quit\n"
          : "5000 key MENU down\n"
            "5600 key MENU up\n"
            "6200 key DOWN down\n"
            "6250 key DOWN up\n"
            "6500 key DOWN down\n"
            "6550 key DOWN up\n"
            "6800 key DOWN down\n"
            "6850 key DOWN up\n"
            "7100 key LEFT down\n"
            "7150 key LEFT up\n"
            "7400 key UP down\n"
            "7450 key UP up\n"
            "7700 key UP down\n"
            "7750 key UP up\n"
            "8000 key A down\n"
            "8050 key A up\n"
            "9500 quit\n";
  write_file(replay_path, replay);
  write_demo_manifest(manifest_path, demo, shim, cap_dir, socket_path);

  pid_t inputd_pid = start_inputd(inputd, config, socket_path, replay_path,
                                  log_path);
  check(inputd_pid > 0, "cannot start brickdesk-inputd");
  check(wait_for_file(socket_path, 3000), "inputd socket did not appear");

  bd_app app;
  memset(&app, 0, sizeof(app));
  char error[512] = {0};
  bool parsed = bd_app_parse_file(manifest_path, &app, error, sizeof(error));
  check(parsed, "manifest: %s", error);

  test_context context;
  memset(&context, 0, sizeof(context));
  context.app_id = "dev.brickdesk.demo";
  context.obs.last_state = BD_SESSION_IDLE;
  bd_session_config session_config;
  memset(&session_config, 0, sizeof(session_config));
  session_config.socket_dir = run_dir;
  session_config.frame_dir = run_dir;
  session_config.state_root = state_dir;
  session_config.session_id = "menutest";
  session_config.on_event = on_event;
  session_config.event_user = &context;
  bd_session *session = NULL;
  if (parsed) {
    session = bd_session_create(&session_config, error, sizeof(error));
  }
  check(session != NULL, "session: %s", error);
  context.session = session;
  check(session == NULL || bd_session_menu_socket(session) != NULL,
        "session did not create a menu socket");
  if (session != NULL) {
    bd_session_result launched = bd_session_launch(session, &app, error,
                                                   sizeof(error));
    check(launched == BD_SESSION_OK, "launch: %s",
          bd_session_result_name(launched));
  }

  bool copied_open = false;
  bool copied_focus = false;
  bool copied_controls = false;
  bool resumed = false;
  bool terminated = false;
  char open_path[PATH_MAX];
  char focus_path[PATH_MAX];
  char controls_path[PATH_MAX];
  join_path(open_path, sizeof(open_path), cap_dir, "menu-0001.png");
  join_path(focus_path, sizeof(focus_path), cap_dir, "menu-0002.png");
  join_path(controls_path, sizeof(controls_path), cap_dir, "menu-0004.png");

  int resumed_wait = 0;
  for (int waited = 0; session != NULL && waited < 18000; waited += 10) {
    bd_session_poll(session);
    observation *obs = &context.obs;
    if (!quit_phase && obs->want_background) {
      obs->want_background = false;
      bd_session_result result =
          bd_session_background(session, context.app_id);
      if (getenv("MENU_TEST_DEBUG") != NULL) {
        fprintf(stderr, "menu_test: background result=%s state=%s\n",
                bd_session_result_name(result),
                bd_session_state_name(obs->last_state));
      }
      check(result == BD_SESSION_OK || result == BD_SESSION_INVALID_STATE,
            "background: %s", bd_session_result_name(result));
    }
    if (!quit_phase && obs->background_seen && !resumed) {
      resumed = true;
      bd_session_result result = bd_session_resume(session, context.app_id);
      if (getenv("MENU_TEST_DEBUG") != NULL) {
        fprintf(stderr, "menu_test: resume result=%s\n",
                bd_session_result_name(result));
      }
      check(result == BD_SESSION_OK || result == BD_SESSION_INVALID_STATE,
            "resume: %s", bd_session_result_name(result));
      resumed_wait = 50;
    }
    if (!quit_phase && resumed && !terminated && resumed_wait > 0) {
      --resumed_wait;
      if (resumed_wait == 0) {
        bd_session_result result =
            bd_session_terminate(session, context.app_id);
        check(result == BD_SESSION_OK || result == BD_SESSION_INVALID_STATE,
              "terminate after background: %s",
              bd_session_result_name(result));
        terminated = true;
      }
    }
    if (quit_phase && obs->want_quit) {
      obs->want_quit = false;
      bd_session_result result = bd_session_terminate(session, context.app_id);
      check(result == BD_SESSION_OK || result == BD_SESSION_INVALID_STATE,
            "terminate: %s", bd_session_result_name(result));
      terminated = true;
    }
    if (!quit_phase && !copied_open && access(open_path, R_OK) == 0) {
      char target[PATH_MAX];
      join_path(target, sizeof(target), out_dir, "menu_open.png");
      check(copy_file(open_path, target), "cannot copy menu_open.png");
      copied_open = true;
    }
    if (!quit_phase && !copied_focus && access(focus_path, R_OK) == 0) {
      char target[PATH_MAX];
      join_path(target, sizeof(target), out_dir, "menu_focus.png");
      check(copy_file(focus_path, target), "cannot copy menu_focus.png");
      copied_focus = true;
    }
    if (!quit_phase && !copied_controls && access(controls_path, R_OK) == 0) {
      char target[PATH_MAX];
      join_path(target, sizeof(target), out_dir, "menu_brightness.png");
      check(copy_file(controls_path, target), "cannot copy menu_brightness.png");
      copied_controls = true;
    }
    if (obs->exited) {
      break;
    }
    sleep_ms(10);
  }

  check(context.obs.foreground_seen, "app never reached foreground");
  if (quit_phase) {
    check(terminated, "quit menu action never arrived");
    check(context.obs.action_count >= 1, "quit menu action count %d",
          context.obs.action_count);
    if (context.obs.action_count >= 1) {
      check(strcmp(context.obs.actions[0], "quit") == 0,
            "quit menu action was '%s'", context.obs.actions[0]);
    }
  } else {
    check(context.obs.background_seen, "app was never backgrounded");
    check(resumed, "app was never resumed");
    check(context.obs.action_count >= 1, "background menu action count %d",
          context.obs.action_count);
    if (context.obs.action_count >= 1) {
      check(strcmp(context.obs.actions[0], "background") == 0,
            "background menu action was '%s'", context.obs.actions[0]);
    }
    check(copied_open, "menu open screenshot was not produced");
    check(copied_focus, "menu focus screenshot was not produced");
    check(copied_controls, "menu brightness screenshot was not produced");
  }
  check(context.obs.exited, "app did not exit after termination");
  if (context.obs.exited) {
    check(!context.obs.crashed, "app crashed (signal %d)",
          context.obs.term_signal);
    check(context.obs.exit_code == 0, "app exit code %d",
          context.obs.exit_code);
  }
  check_last_frame(run_dir);

  char *state_content = read_file(state_file);
  check(state_content != NULL, "state file was not written");
  if (state_content != NULL) {
    check(strstr(state_content, "frames=") != NULL,
          "state file has no frame count");
    free(state_content);
  }

  char *inputd_log = read_file(log_path);
  check(inputd_log != NULL, "inputd log missing");
  if (inputd_log != NULL) {
    check(strstr(inputd_log, "action menu.open") != NULL,
          "inputd did not emit menu.open");
    check(strstr(inputd_log, "grab") != NULL, "inputd never grabbed input");
    check(strstr(inputd_log, "ungrab") != NULL, "inputd never ungrabbed");
    free(inputd_log);
  }

  stop_inputd(inputd_pid);
  if (session != NULL) {
    bd_session_destroy(session);
  }
  bd_app_destroy(&app);
  if (getenv("MENU_TEST_KEEP") == NULL) {
    remove_tree(root);
  } else {
    fprintf(stderr, "menu_test: kept %s\n", root);
  }
}

typedef enum device_case_kind {
  DEVICE_CASE_WRITABLE = 0,
  DEVICE_CASE_READ_ONLY,
  DEVICE_CASE_ABSENT
} device_case_kind;

static bool create_backlight_class(const char *root) {
  char path[PATH_MAX];
  join_path(path, sizeof(path), root, "sys");
  if (!make_dir(path)) {
    return false;
  }
  join_path(path, sizeof(path), root, "sys/class");
  if (!make_dir(path)) {
    return false;
  }
  join_path(path, sizeof(path), root, "sys/class/backlight");
  return make_dir(path);
}

static bool write_fake_backlight(const char *root, bool writable) {
  if (!create_backlight_class(root)) {
    return false;
  }
  char path[PATH_MAX];
  join_path(path, sizeof(path), root, "sys/class/backlight/bd-test");
  if (!make_dir(path)) {
    return false;
  }
  char brightness[PATH_MAX];
  char maximum[PATH_MAX];
  join_path(brightness, sizeof(brightness), path, "brightness");
  join_path(maximum, sizeof(maximum), path, "max_brightness");
  if (!write_file(brightness, "19\n") || !write_file(maximum, "64\n")) {
    return false;
  }
  if (!writable && chmod(brightness, 0444) != 0) {
    return false;
  }
  return true;
}

static int read_int_file(const char *path) {
  char *text = read_file(path);
  if (text == NULL) {
    return -1;
  }
  char *end = NULL;
  long value = strtol(text, &end, 10);
  bool ok = end != text;
  free(text);
  return ok ? (int)value : -1;
}

/*
 * Runs apps/demo directly under the shim and a replay inputd, with device_av
 * pointed at a fake backlight and no audio card.  The menu therefore uses the
 * production device backend rather than the explicit test stub.  Capture 5 is
 * after three DOWN presses (brightness focused) and one RIGHT press.
 */
static void run_device_phase(const char *demo, const char *inputd,
                             const char *shim, const char *config,
                             const char *assets, const char *out_dir,
                             device_case_kind kind, const char *capture_name,
                             int expected_raw) {
  char root[256];
  snprintf(root, sizeof(root), "/tmp/bd-menu-device-XXXXXX");
  if (mkdtemp(root) == NULL) {
    check(false, "device mkdtemp: %s", strerror(errno));
    return;
  }
  char capture_dir[PATH_MAX];
  char socket_path[PATH_MAX];
  char replay_path[PATH_MAX];
  char log_path[PATH_MAX];
  char app_log[PATH_MAX];
  join_path(capture_dir, sizeof(capture_dir), root, "capture");
  join_path(socket_path, sizeof(socket_path), root, "inputd.sock");
  join_path(replay_path, sizeof(replay_path), root, "replay.txt");
  join_path(log_path, sizeof(log_path), root, "inputd.log");
  join_path(app_log, sizeof(app_log), root, "demo.log");
  make_dir(capture_dir);
  if (kind == DEVICE_CASE_ABSENT) {
    check(create_backlight_class(root),
          "cannot create empty fake backlight class");
  } else {
    check(write_fake_backlight(root, kind == DEVICE_CASE_WRITABLE),
          "cannot create fake backlight");
  }
  write_file(replay_path,
             "4000 key MENU down\n"
             "4600 key MENU up\n"
             "5400 key DOWN down\n"
             "5450 key DOWN up\n"
             "5700 key DOWN down\n"
             "5750 key DOWN up\n"
             "6000 key DOWN down\n"
             "6050 key DOWN up\n"
             "6400 key RIGHT down\n"
             "6450 key RIGHT up\n"
             "8200 quit\n");

  pid_t inputd_pid = start_inputd(inputd, config, socket_path, replay_path,
                                  log_path);
  check(inputd_pid > 0, "cannot start inputd for the device case");
  check(wait_for_file(socket_path, 3000), "device inputd socket missing");

  pid_t child = fork();
  if (child == 0) {
    char snd_root[PATH_MAX];
    char proc_cards[PATH_MAX];
    char speaker_mute[PATH_MAX];
    join_path(snd_root, sizeof(snd_root), root, "no-snd");
    join_path(proc_cards, sizeof(proc_cards), root, "no-cards");
    join_path(speaker_mute, sizeof(speaker_mute), root, "no-mute");
    setenv("SDL_VIDEODRIVER", "offscreen", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    setenv("BRICKDESK_HEADLESS", "1", 1);
    setenv("LD_PRELOAD", shim, 1);
    setenv("BRICKDESK_INPUT_SOCKET", socket_path, 1);
    setenv("BRICKDESK_MENU_CAPTURE", capture_dir, 1);
    setenv("BRICKDESK_ASSET_DIR", assets, 1);
    setenv("BRICKDESK_CONFIG_DIR", config, 1);
    setenv("BRICKDESK_MENU_DEBUG", "1", 1);
    setenv("BRICKDESK_MENU_SYSTEM", "device", 1);
    setenv("BRICKDESK_DEMO_WIDTH", "1024", 1);
    setenv("BRICKDESK_DEMO_HEIGHT", "768", 1);
    setenv("BRICKDESK_DEMO_FIXED_TIME_MS", "1000", 1);
    setenv("BRICKDESK_AV_SYSFS_ROOT", root, 1);
    setenv("BRICKDESK_AV_BRIGHTNESS_BACKEND", "sysfs", 1);
    setenv("BRICKDESK_AV_SND_ROOT", snd_root, 1);
    setenv("BRICKDESK_AV_PROC_CARDS", proc_cards, 1);
    setenv("BRICKDESK_AV_SPEAKER_MUTE", speaker_mute, 1);
    int log = open(app_log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log >= 0) {
      dup2(log, STDOUT_FILENO);
      dup2(log, STDERR_FILENO);
      close(log);
    }
    execl(demo, demo, (char *)NULL);
    _exit(127);
  }
  check(child > 0, "cannot start the device demo");
  if (child < 0) {
    stop_inputd(inputd_pid);
    remove_tree(root);
    return;
  }

  char capture_path[PATH_MAX];
  join_path(capture_path, sizeof(capture_path), capture_dir, "menu-0005.png");
  check(wait_for_file(capture_path, 15000),
        "device case did not produce the adjusted menu capture");
  sleep_ms(200);
  kill(child, SIGTERM);
  int status = 0;
  bool finished = false;
  for (int waited = 0; waited < 2000; waited += 20) {
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child) {
      finished = true;
      break;
    }
    sleep_ms(20);
  }
  if (!finished) {
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    check(false, "device demo did not exit");
  } else {
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "device demo exit status 0x%x", status);
  }

  if (access(capture_path, R_OK) == 0) {
    char target[PATH_MAX];
    join_path(target, sizeof(target), out_dir, capture_name);
    check(copy_file(capture_path, target), "cannot copy %s", capture_name);
  } else {
    check(false, "missing adjusted capture for %s", capture_name);
  }

  if (kind == DEVICE_CASE_ABSENT) {
    char path[PATH_MAX];
    join_path(path, sizeof(path), root,
              "sys/class/backlight/bd-test/brightness");
    check(access(path, F_OK) != 0, "absent case unexpectedly has a backlight");
  } else {
    char path[PATH_MAX];
    join_path(path, sizeof(path), root,
              "sys/class/backlight/bd-test/brightness");
    int raw = read_int_file(path);
    check(raw == expected_raw, "%s brightness raw %d, expected %d",
          capture_name, raw, expected_raw);
  }

  char *app_content = read_file(app_log);
  check(app_content != NULL, "device demo log missing");
  if (app_content != NULL) {
    check(strstr(app_content, "menu.open action") != NULL,
          "device demo never opened the menu");
    check(strstr(app_content, "audio unavailable") != NULL,
          "device demo did not report the missing audio device");
    if (kind == DEVICE_CASE_WRITABLE) {
      check(strstr(app_content, "brightness write failed") == NULL,
            "writable fake backlight rejected the write");
      check(strstr(app_content, "brightness unavailable") == NULL,
            "writable fake backlight was not probed");
    } else if (kind == DEVICE_CASE_READ_ONLY) {
      check(strstr(app_content, "brightness write failed") != NULL,
            "read-only fake backlight did not reject the write");
    } else {
      check(strstr(app_content, "brightness unavailable") != NULL,
            "empty fake sysfs was not reported as unavailable");
    }
    free(app_content);
  }

  char *inputd_log = read_file(log_path);
  check(inputd_log != NULL, "device inputd log missing");
  if (inputd_log != NULL) {
    check(strstr(inputd_log, "action menu.open") != NULL,
          "device inputd did not emit menu.open");
    check(strstr(inputd_log, "grab") != NULL,
          "device inputd never grabbed input");
    free(inputd_log);
  }

  stop_inputd(inputd_pid);
  if (getenv("MENU_TEST_KEEP") == NULL) {
    remove_tree(root);
  } else {
    fprintf(stderr, "menu_test: kept %s\n", root);
  }
}

static int run_state_phase(const char *state_app, const char *inputd,
                           const char *shim, const char *config,
                           const char *assets) {
  char root[256];
  snprintf(root, sizeof(root), "/tmp/bd-menu-state-XXXXXX");
  if (mkdtemp(root) == NULL) {
    check(false, "mkdtemp: %s", strerror(errno));
    return 1;
  }
  char socket_path[PATH_MAX];
  char replay_path[PATH_MAX];
  char log_path[PATH_MAX];
  char app_log[PATH_MAX];
  join_path(socket_path, sizeof(socket_path), root, "inputd.sock");
  join_path(replay_path, sizeof(replay_path), root, "replay.txt");
  join_path(log_path, sizeof(log_path), root, "inputd.log");
  join_path(app_log, sizeof(app_log), root, "state.log");
  const char *replay =
      "5000 key MENU down\n"
      "5600 key MENU up\n"
      "6400 key B down\n"
      "6450 key B up\n"
      "8000 quit\n";
  write_file(replay_path, replay);

  pid_t inputd_pid = start_inputd(inputd, config, socket_path, replay_path,
                                  log_path);
  check(inputd_pid > 0, "cannot start inputd for the state probe");
  check(wait_for_file(socket_path, 3000), "state inputd socket missing");

  pid_t child = fork();
  if (child == 0) {
    setenv("SDL_VIDEODRIVER", "offscreen", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    setenv("BRICKDESK_HEADLESS", "1", 1);
    setenv("LD_PRELOAD", shim, 1);
    setenv("BRICKDESK_INPUT_SOCKET", socket_path, 1);
    setenv("BRICKDESK_ASSET_DIR", assets, 1);
    setenv("BRICKDESK_CONFIG_DIR", config, 1);
    setenv("BRICKDESK_MENU_DEBUG", "1", 1);
    int log = open(app_log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log >= 0) {
      dup2(log, STDOUT_FILENO);
      dup2(log, STDERR_FILENO);
      close(log);
    }
    execl(state_app, state_app, (char *)NULL);
    _exit(127);
  }
  check(child > 0, "cannot start the state probe");
  int status = 0;
  bool finished = false;
  for (int waited = 0; waited < 20000; waited += 20) {
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child) {
      finished = true;
      break;
    }
    sleep_ms(20);
  }
  if (!finished) {
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    check(false, "state probe timed out");
  } else {
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "state probe exit status 0x%x", status);
  }
  char *content = read_file(app_log);
  check(content != NULL, "state probe log missing");
  if (content != NULL) {
    check(strstr(content, "menu_state: OK") != NULL,
          "state probe did not report OK: %s", content);
    free(content);
  }
  stop_inputd(inputd_pid);
  if (getenv("MENU_TEST_KEEP") == NULL) {
    remove_tree(root);
  } else {
    fprintf(stderr, "menu_test: kept %s\n", root);
  }
  return g_failures == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
  const char *demo = NULL;
  const char *state_app = NULL;
  const char *inputd = NULL;
  const char *shim = NULL;
  const char *config = NULL;
  const char *assets = NULL;
  const char *out_dir = NULL;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--demo") == 0 && i + 1 < argc) {
      demo = argv[++i];
    } else if (strcmp(argv[i], "--state-app") == 0 && i + 1 < argc) {
      state_app = argv[++i];
    } else if (strcmp(argv[i], "--inputd") == 0 && i + 1 < argc) {
      inputd = argv[++i];
    } else if (strcmp(argv[i], "--shim") == 0 && i + 1 < argc) {
      shim = argv[++i];
    } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      config = argv[++i];
    } else if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc) {
      assets = argv[++i];
    } else if (strcmp(argv[i], "--out-dir") == 0 && i + 1 < argc) {
      out_dir = argv[++i];
    } else {
      fprintf(stderr, "menu_test: unknown argument: %s\n", argv[i]);
      return 2;
    }
  }
  if (demo == NULL || state_app == NULL || inputd == NULL || shim == NULL ||
      config == NULL || assets == NULL || out_dir == NULL) {
    fprintf(stderr,
            "usage: bd_menu_test --demo PATH --state-app PATH --inputd PATH"
            " --shim PATH --config DIR --assets DIR --out-dir DIR\n");
    return 2;
  }
  make_dir(out_dir);
  signal(SIGPIPE, SIG_IGN);

  run_demo_phase(demo, inputd, shim, config, assets, out_dir, false);
  if (g_failures == 0) {
    run_demo_phase(demo, inputd, shim, config, assets, out_dir, true);
  }
  if (g_failures == 0) {
    run_device_phase(demo, inputd, shim, config, assets, out_dir,
                     DEVICE_CASE_WRITABLE, "menu_device_readback.png", 22);
  }
  if (g_failures == 0) {
    run_device_phase(demo, inputd, shim, config, assets, out_dir,
                     DEVICE_CASE_READ_ONLY, "menu_device_write_failed.png", 19);
  }
  if (g_failures == 0) {
    run_device_phase(demo, inputd, shim, config, assets, out_dir,
                     DEVICE_CASE_ABSENT, "menu_device_unavailable.png", -1);
  }
  if (g_failures == 0) {
    run_state_phase(state_app, inputd, shim, config, assets);
  }
  if (g_failures == 0) {
    printf("menu_test: OK\n");
  }
  return g_failures == 0 ? 0 : 1;
}
