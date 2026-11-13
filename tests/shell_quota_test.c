#define _XOPEN_SOURCE 700

#include "brickdesk/app.h"
#include "brickdesk/config.h"
#include "brickdesk/gfx.h"
#include "brickdesk/home.h"
#include "brickdesk/platform.h"
#include "brickdesk/shell.h"

#include <errno.h>
#include <ftw.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define TEST_PATH 1024
#define TEST_APP_JSON 4096

static int failures = 0;

#define CHECK(condition, ...)                                                  \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "shell_quota_test: FAIL: ");                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fputc('\n', stderr);                                                     \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static bool make_dir(const char *path) {
  if (mkdir(path, 0777) == 0) {
    return true;
  }
  return errno == EEXIST;
}

static bool join(char *out, size_t size, const char *a, const char *b) {
  int written = snprintf(out, size, "%s/%s", a, b);
  return written > 0 && (size_t)written < size;
}

static bool write_file(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs(text, file) >= 0;
  return fclose(file) == 0 && ok;
}

static int remove_entry(const char *path, const struct stat *info, int flag,
                        struct FTW *state) {
  (void)info;
  (void)flag;
  (void)state;
  return remove(path);
}

static void remove_tree(const char *path) {
  nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static void sleep_ms(int milliseconds) {
  struct timespec delay;
  delay.tv_sec = milliseconds / 1000;
  delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
  nanosleep(&delay, NULL);
}

static uint64_t real_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

typedef struct test_runtime {
  bd_platform *platform;
  bd_gfx *gfx;
  bd_config *config;
  bd_shell *shell;
  bd_session *session;
  char root[TEST_PATH];
  char apps[TEST_PATH];
} test_runtime;

static void destroy_runtime(test_runtime *runtime) {
  if (runtime->shell != NULL) {
    bd_shell_destroy(runtime->shell);
  }
  if (runtime->config != NULL) {
    bd_config_free(runtime->config);
  }
  if (runtime->gfx != NULL) {
    bd_gfx_destroy(runtime->gfx);
  }
  if (runtime->platform != NULL) {
    bd_platform_destroy(runtime->platform);
  }
  memset(runtime, 0, sizeof(*runtime));
}

static bool write_manifest(const char *apps, const char *name,
                           const char *id, const char *child_path) {
  char directory[TEST_PATH];
  char path[TEST_PATH];
  if (!join(directory, sizeof(directory), apps, name) ||
      !make_dir(directory) || !join(path, sizeof(path), directory, "app.json")) {
    return false;
  }
  char manifest[TEST_APP_JSON];
  int written = snprintf(
      manifest, sizeof(manifest),
      "{\"schema\":1,\"id\":\"%s\",\"name\":\"Quota %s\",\"version\":\"1.0\","
      "\"type\":\"app\",\"category\":\"app\",\"exec\":[\"%s\",\"idle\"],"
      "\"cwd\":\".\",\"capabilities\":{\"pause\":true,\"resume\":true,"
      "\"background\":false},\"hooks\":{},\"menu\":{\"mode\":\"overlay\"},"
      "\"accent\":\"#336699\"}\n",
      id, name, child_path);
  return written > 0 && (size_t)written < sizeof(manifest) &&
         write_file(path, manifest);
}

static bool create_runtime(test_runtime *runtime, const char *root,
                           const char *name, const char *child_path) {
  memset(runtime, 0, sizeof(*runtime));
  snprintf(runtime->root, sizeof(runtime->root), "%s/%s", root, name);
  CHECK(make_dir(runtime->root), "create case directory %s", runtime->root);
  if (!join(runtime->apps, sizeof(runtime->apps), runtime->root, "apps")) {
    return false;
  }
  CHECK(make_dir(runtime->apps), "create apps directory");
  for (int i = 0; i < 4; ++i) {
    char id[32];
    char fixture[16];
    snprintf(id, sizeof(id), "quota.%c", 'a' + i);
    snprintf(fixture, sizeof(fixture), "%c", 'a' + i);
    CHECK(write_manifest(runtime->apps, fixture, id, child_path),
          "write app %s", id);
  }

  char sockets[TEST_PATH];
  char frames[TEST_PATH];
  char state[TEST_PATH];
  CHECK(join(sockets, sizeof(sockets), runtime->root, "sockets"), "sockets");
  CHECK(join(frames, sizeof(frames), runtime->root, "frames"), "frames");
  CHECK(join(state, sizeof(state), runtime->root, "state"), "state");
  CHECK(make_dir(sockets), "create sockets");
  CHECK(make_dir(frames), "create frames");
  CHECK(make_dir(state), "create state");

  bd_platform_config platform_config;
  memset(&platform_config, 0, sizeof(platform_config));
  platform_config.title = "brickdesk quota test";
  platform_config.width = 1024;
  platform_config.height = 768;
  platform_config.hidden = true;
  runtime->platform = bd_platform_create(&platform_config);
  if (runtime->platform == NULL) {
    return false;
  }
  runtime->gfx = bd_gfx_create(1024, 768);
  if (runtime->gfx == NULL) {
    return false;
  }

  bd_config_options config_options;
  memset(&config_options, 0, sizeof(config_options));
  config_options.config_dir = BRICKDESK_TEST_CONFIG_DIR;
  config_options.migrate = false;
  char config_error[256] = {0};
  runtime->config =
      bd_config_load(&config_options, config_error, sizeof(config_error));
  if (runtime->config == NULL) {
    fprintf(stderr, "shell_quota_test: config: %s\n", config_error);
    return false;
  }

  bd_shell_config shell_config;
  memset(&shell_config, 0, sizeof(shell_config));
  shell_config.platform = runtime->platform;
  shell_config.gfx = runtime->gfx;
  shell_config.settings = runtime->config;
  shell_config.apps_dir = runtime->apps;
  shell_config.session_socket_dir = sockets;
  shell_config.session_frame_dir = frames;
  shell_config.session_state_root = state;
  shell_config.verbose = false;
  runtime->shell = bd_shell_create(&shell_config);
  if (runtime->shell == NULL) {
    return false;
  }
  runtime->session = bd_shell_test_session(runtime->shell);
  CHECK(runtime->session != NULL, "shell session");
  return runtime->session != NULL;
}

static bool wait_state(test_runtime *runtime, const char *id,
                       bd_session_state state, int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 10) {
    bd_shell_iterate(runtime->shell, 5);
    bd_session_app_info info;
    if (bd_session_find(runtime->session, id, &info) && info.state == state) {
      return true;
    }
    sleep_ms(5);
  }
  bd_session_app_info info;
  if (bd_session_find(runtime->session, id, &info)) {
    fprintf(stderr, "shell_quota_test: %s state %s, wanted %s\n", id,
            bd_session_state_name(info.state), bd_session_state_name(state));
  }
  return false;
}

static bool launch_background(test_runtime *runtime, const char *id) {
  const bd_app *app = NULL;
  size_t count = bd_shell_app_count(runtime->shell);
  for (size_t i = 0; i < count; ++i) {
    const bd_app *candidate = bd_shell_app_at(runtime->shell, i);
    if (candidate != NULL && strcmp(candidate->id, id) == 0) {
      app = candidate;
      break;
    }
  }
  CHECK(app != NULL, "find app %s", id);
  if (app == NULL) {
    return false;
  }
  char error[256] = {0};
  CHECK(bd_session_launch(runtime->session, app, error, sizeof(error)) ==
            BD_SESSION_OK,
        "launch %s: %s", id, error);
  CHECK(wait_state(runtime, id, BD_SESSION_FOREGROUND, 2000),
        "%s foreground", id);
  CHECK(bd_session_background(runtime->session, id) == BD_SESSION_OK,
        "background %s", id);
  CHECK(wait_state(runtime, id, BD_SESSION_BACKGROUND, 2000),
        "%s background", id);
  CHECK(bd_session_background_count(runtime->session) >= 1,
        "background count after %s", id);
  for (int i = 0; i < 100; ++i) {
    bd_shell_iterate(runtime->shell, 5);
    uint64_t until = 0;
    bd_home *home = bd_shell_home(runtime->shell);
    if (bd_home_launch_target(home) == NULL &&
        !bd_home_needs_frame_until(home, &until)) {
      break;
    }
    sleep_ms(5);
  }
  return true;
}

static int count_running_id(test_runtime *runtime, const char *id) {
  int count = 0;
  size_t running = bd_session_running_count(runtime->session);
  for (size_t i = 0; i < running; ++i) {
    bd_session_app_info info;
    if (bd_session_running_at(runtime->session, i, &info) &&
        strcmp(info.app_id, id) == 0) {
      ++count;
    }
  }
  return count;
}

static void press_key(test_runtime *runtime, bd_key key, uint64_t *time_ms) {
  bd_shell_test_key(runtime->shell, key, true, *time_ms);
  *time_ms += 30;
  bd_shell_test_key(runtime->shell, key, false, *time_ms);
  *time_ms += 30;
}

static void fill_three(test_runtime *runtime, const char *const ids[3]) {
  for (int i = 0; i < 3; ++i) {
    CHECK(launch_background(runtime, ids[i]), "fill %s", ids[i]);
  }
}

static void test_limit_and_cancel(const char *root, const char *child_path) {
  test_runtime runtime;
  if (!create_runtime(&runtime, root, "cancel", child_path)) {
    destroy_runtime(&runtime);
    return;
  }
  bd_home *home = bd_shell_home(runtime.shell);
  const char *ids[3] = {"quota.a", "quota.b", "quota.c"};
  fill_three(&runtime, ids);
  CHECK(bd_home_running_count(home) == 3, "three running tiles");
  int memory = bd_home_running_memory(home, "quota.a");
  CHECK(memory >= 0, "process-tree memory sampled, got %d", memory);

  CHECK(bd_shell_test_launch(runtime.shell, "quota.d"), "launch over limit");
  CHECK(bd_home_dialog_open(home), "background-full dialog opened");
  bd_session_app_info info;
  CHECK(!bd_session_find(runtime.session, "quota.d", &info),
        "quota.d must not launch before eviction");
  CHECK(bd_session_background_count(runtime.session) == 3,
        "background count stays at three");
  bd_session_eviction eviction;
  CHECK(bd_session_suggest_eviction(runtime.session, &eviction),
        "suggest eviction");
  CHECK(strcmp(eviction.app_id, "quota.a") == 0,
        "LRU victim is quota.a, got %s", eviction.app_id);

  uint64_t now = real_ms();
  press_key(&runtime, BD_KEY_B, &now);
  CHECK(!bd_home_dialog_open(home), "cancel closes dialog");
  for (int i = 0; i < 30; ++i) {
    bd_shell_iterate(runtime.shell, 5);
    sleep_ms(5);
  }
  CHECK(!bd_session_find(runtime.session, "quota.d", &info),
        "cancel must not launch quota.d");
  CHECK(bd_session_background_count(runtime.session) == 3,
        "cancel keeps three backgrounds");
  destroy_runtime(&runtime);
}

static void test_evict_failure_keeps_choice(const char *root,
                                            const char *child_path) {
  test_runtime runtime;
  if (!create_runtime(&runtime, root, "failure", child_path)) {
    destroy_runtime(&runtime);
    return;
  }
  bd_home *home = bd_shell_home(runtime.shell);
  const char *ids[3] = {"quota.a", "quota.b", "quota.c"};
  fill_three(&runtime, ids);
  CHECK(bd_shell_test_launch(runtime.shell, "quota.d"),
        "launch over limit for failure");
  CHECK(bd_home_dialog_open(home), "failure dialog opened");

  bd_session_eviction eviction;
  CHECK(bd_session_suggest_eviction(runtime.session, &eviction),
        "failure victim");
  bd_session_app_info victim;
  CHECK(bd_session_find(runtime.session, eviction.app_id, &victim),
        "find failure victim");
  CHECK(victim.pid > 0, "failure victim pid %d", victim.pid);
  if (victim.pid <= 0) {
    destroy_runtime(&runtime);
    return;
  }
  CHECK(kill(victim.pid, SIGKILL) == 0, "kill victim externally");
  for (int i = 0; i < 200; ++i) {
    bd_session_poll(runtime.session);
    if (bd_session_find(runtime.session, eviction.app_id, &victim) &&
        victim.state == BD_SESSION_EXITED) {
      break;
    }
    sleep_ms(5);
  }
  bd_session_clear_terminated(runtime.session);

  uint64_t now = real_ms();
  press_key(&runtime, BD_KEY_RIGHT, &now);
  press_key(&runtime, BD_KEY_A, &now);
  CHECK(bd_home_dialog_open(home), "failure keeps the choice dialog open");
  CHECK(bd_home_dialog_error(home)[0] != '\0',
        "failure exposes an error message");
  bd_session_app_info info;
  CHECK(!bd_session_find(runtime.session, "quota.d", &info),
        "failed eviction must not launch quota.d");
  for (int i = 0; i < 30; ++i) {
    bd_shell_iterate(runtime.shell, 5);
    sleep_ms(5);
  }
  CHECK(!bd_session_find(runtime.session, "quota.d", &info),
        "failed eviction must not retry in the background");

  press_key(&runtime, BD_KEY_B, &now);
  CHECK(!bd_home_dialog_open(home), "failure cancel closes dialog");
  destroy_runtime(&runtime);
}

static void test_evict_success_launches_once(const char *root,
                                             const char *child_path) {
  test_runtime runtime;
  if (!create_runtime(&runtime, root, "success", child_path)) {
    destroy_runtime(&runtime);
    return;
  }
  bd_home *home = bd_shell_home(runtime.shell);
  const char *ids[3] = {"quota.a", "quota.b", "quota.c"};
  fill_three(&runtime, ids);
  CHECK(bd_shell_test_launch(runtime.shell, "quota.d"),
        "launch over limit for success");
  CHECK(bd_home_dialog_open(home), "success dialog opened");

  uint64_t now = real_ms();
  press_key(&runtime, BD_KEY_RIGHT, &now);
  press_key(&runtime, BD_KEY_A, &now);
  CHECK(wait_state(&runtime, "quota.d", BD_SESSION_FOREGROUND, 5000),
        "quota.d launches after eviction");
  CHECK(count_running_id(&runtime, "quota.d") == 1,
        "quota.d launches exactly once");
  CHECK(bd_session_background_count(runtime.session) == 2,
        "eviction leaves two backgrounds");

  for (int i = 0; i < 40; ++i) {
    bd_shell_iterate(runtime.shell, 5);
    sleep_ms(5);
  }
  CHECK(count_running_id(&runtime, "quota.d") == 1,
        "no duplicate launch after settling");
  destroy_runtime(&runtime);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: shell_quota_test CHILD_BINARY\n");
    return 2;
  }
  char root[] = "/tmp/bd_shell_quota.XXXXXX";
  if (mkdtemp(root) == NULL) {
    fprintf(stderr, "shell_quota_test: mkdtemp failed: %s\n", strerror(errno));
    return 1;
  }
  test_limit_and_cancel(root, argv[1]);
  test_evict_failure_keeps_choice(root, argv[1]);
  test_evict_success_launches_once(root, argv[1]);
  remove_tree(root);
  if (failures != 0) {
    fprintf(stderr, "shell_quota_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("shell_quota_test: ok\n");
  return 0;
}
