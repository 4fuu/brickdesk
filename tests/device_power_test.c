#define _POSIX_C_SOURCE 200809L

#include "brickdesk/device_power.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BRICKDESK_POWER_FIXTURE_DIR
#define BRICKDESK_POWER_FIXTURE_DIR "tests/fixtures/power"
#endif

#define MAX_CALLS 64
#define PATH_SIZE 1024

static int g_failures;

#define CHECK(condition, ...)                    \
  do {                                           \
    if (!(condition)) {                          \
      fprintf(stderr, "device_power_test: ");    \
      fprintf(stderr, __VA_ARGS__);              \
      fprintf(stderr, "\n");                     \
      ++g_failures;                              \
    }                                            \
  } while (0)

typedef struct fake {
  bd_power *power;
  bd_power_step calls[MAX_CALLS];
  int call_count;
  bd_power_step fail_step;
  int fail_code;
  bd_power_step fail_restore_step;
  int fail_restore_code;
  int backend_deep_status;
  int backend_shutdown_status;
  int backend_errno;
  int deep_calls;
  int shutdown_calls;
  bool shutdown_reboot;
  bool reenter_on_display_off;
  bd_power_result reenter_result;
} fake;

static void fake_init(fake *state) {
  memset(state, 0, sizeof(*state));
  state->fail_step = BD_POWER_STEP_NONE;
  state->fail_restore_step = BD_POWER_STEP_NONE;
  state->fail_code = 42;
  state->fail_restore_code = 43;
  state->backend_deep_status = BD_POWER_BACKEND_OK;
  state->backend_shutdown_status = BD_POWER_BACKEND_ERROR;
  state->backend_errno = EPERM;
}

static void fake_clear_calls(fake *state) {
  state->call_count = 0;
  memset(state->calls, 0, sizeof(state->calls));
}

static void log_call(fake *state, bd_power_step step) {
  if (state->call_count < MAX_CALLS) {
    state->calls[state->call_count] = step;
  }
  ++state->call_count;
}

static int fail_code(const fake *state, bd_power_step step) {
  if (state->fail_step == step) {
    return state->fail_code;
  }
  if (state->fail_restore_step == step) {
    return state->fail_restore_code;
  }
  return 0;
}

static int hook_input_release(void *user) {
  fake *state = user;
  log_call(state, BD_POWER_STEP_INPUT_RELEASE);
  return fail_code(state, BD_POWER_STEP_INPUT_RELEASE);
}

static int hook_app_pause(void *user) {
  fake *state = user;
  log_call(state, BD_POWER_STEP_APP_PAUSE);
  return fail_code(state, BD_POWER_STEP_APP_PAUSE);
}

static int hook_resource_prepare(void *user, bd_power_sleep_mode mode) {
  fake *state = user;
  bd_power_step step = mode == BD_POWER_SLEEP_LIGHT
                           ? BD_POWER_STEP_RESOURCE_PREPARE_LIGHT
                           : BD_POWER_STEP_RESOURCE_PREPARE_DEEP;
  log_call(state, step);
  return fail_code(state, step);
}

static int hook_display_off(void *user) {
  fake *state = user;
  log_call(state, BD_POWER_STEP_DISPLAY_OFF);
  if (state->reenter_on_display_off && state->power != NULL) {
    bd_power_report report;
    state->reenter_result = bd_power_sleep_light(state->power, &report);
  }
  return fail_code(state, BD_POWER_STEP_DISPLAY_OFF);
}

static int hook_display_on(void *user) {
  fake *state = user;
  log_call(state, BD_POWER_STEP_DISPLAY_ON);
  return fail_code(state, BD_POWER_STEP_DISPLAY_ON);
}

static int hook_resource_restore(void *user, bd_power_sleep_mode mode) {
  fake *state = user;
  bd_power_step step = mode == BD_POWER_SLEEP_LIGHT
                           ? BD_POWER_STEP_RESOURCE_RESTORE_LIGHT
                           : BD_POWER_STEP_RESOURCE_RESTORE_DEEP;
  log_call(state, step);
  return fail_code(state, step);
}

static int hook_app_resume(void *user) {
  fake *state = user;
  log_call(state, BD_POWER_STEP_APP_RESUME);
  return fail_code(state, BD_POWER_STEP_APP_RESUME);
}

static int hook_input_acquire(void *user) {
  fake *state = user;
  log_call(state, BD_POWER_STEP_INPUT_ACQUIRE);
  return fail_code(state, BD_POWER_STEP_INPUT_ACQUIRE);
}

static bd_power_backend_status fake_deep_sleep(void *user) {
  fake *state = user;
  ++state->deep_calls;
  log_call(state, BD_POWER_STEP_DEEP_SLEEP);
  bd_power_backend_status status =
      (bd_power_backend_status)state->backend_deep_status;
  if (status == BD_POWER_BACKEND_ERROR) {
    errno = state->backend_errno;
  }
  return status;
}

static bd_power_backend_status fake_shutdown(void *user, bool reboot) {
  fake *state = user;
  ++state->shutdown_calls;
  state->shutdown_reboot = reboot;
  log_call(state, reboot ? BD_POWER_STEP_REBOOT : BD_POWER_STEP_POWEROFF);
  bd_power_backend_status status =
      (bd_power_backend_status)state->backend_shutdown_status;
  if (status == BD_POWER_BACKEND_ERROR) {
    errno = state->backend_errno;
  }
  return status;
}

static void fill_config(bd_power_config *config, fake *state) {
  memset(config, 0, sizeof(*config));
  config->hooks.user = state;
  config->hooks.input_release = hook_input_release;
  config->hooks.app_pause = hook_app_pause;
  config->hooks.resource_prepare = hook_resource_prepare;
  config->hooks.display_off = hook_display_off;
  config->hooks.display_on = hook_display_on;
  config->hooks.resource_restore = hook_resource_restore;
  config->hooks.app_resume = hook_app_resume;
  config->hooks.input_acquire = hook_input_acquire;
  config->backend.user = state;
  config->backend.deep_sleep = fake_deep_sleep;
  config->backend.shutdown = fake_shutdown;
  config->allow_light_sleep = true;
}

static bd_power *make_power(fake *state, bool deep, bool poweroff,
                            bool reboot) {
  bd_power_config config;
  fill_config(&config, state);
  config.allow_deep_sleep = deep;
  config.allow_poweroff = poweroff;
  config.allow_reboot = reboot;
  char error[256] = "";
  bd_power *power = bd_power_create(&config, error, sizeof(error));
  if (power == NULL) {
    fprintf(stderr, "device_power_test: create: %s\n", error);
  }
  state->power = power;
  return power;
}

static void expect_calls(const fake *state, const bd_power_step *expected,
                         int count, const char *label) {
  if (state->call_count != count) {
    fprintf(stderr, "device_power_test: %s: call count %d, expected %d\n",
            label, state->call_count, count);
    for (int i = 0; i < state->call_count; ++i) {
      fprintf(stderr, "  actual[%d]=%s\n", i,
              bd_power_step_name(state->calls[i]));
    }
    ++g_failures;
    return;
  }
  for (int i = 0; i < count; ++i) {
    if (state->calls[i] != expected[i]) {
      fprintf(stderr,
              "device_power_test: %s: call[%d]=%s, expected %s\n", label, i,
              bd_power_step_name(state->calls[i]),
              bd_power_step_name(expected[i]));
      ++g_failures;
      return;
    }
  }
}

static void expect_report(const bd_power_report *report,
                          bd_power_result result, bd_power_state state,
                          const char *label) {
  CHECK(report->result == result, "%s: result=%s, expected %s", label,
        bd_power_result_name(report->result),
        bd_power_result_name(result));
  CHECK(report->state_after == state, "%s: state_after=%s, expected %s",
        label, bd_power_state_name(report->state_after),
        bd_power_state_name(state));
}

static bool read_file(const char *path, char *out, size_t out_size) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  size_t used = fread(out, 1, out_size - 1, file);
  bool ok = !ferror(file);
  out[used] = '\0';
  fclose(file);
  return ok;
}

static void fixture_root(char *out, size_t out_size, const char *name) {
  snprintf(out, out_size, "%s/%s", BRICKDESK_POWER_FIXTURE_DIR, name);
}

static void test_create_validation(void) {
  fake state;
  fake_init(&state);
  bd_power_config config;
  fill_config(&config, &state);
  config.allow_light_sleep = true;
  config.hooks.display_off = NULL;
  char error[256] = "";
  bd_power *power = bd_power_create(&config, error, sizeof(error));
  CHECK(power == NULL, "missing display_off should fail create");
  CHECK(error[0] != '\0', "missing hook should set an error");
  bd_power_destroy(power);

  fake_init(&state);
  fill_config(&config, &state);
  config.hooks.resource_restore = NULL;
  error[0] = '\0';
  power = bd_power_create(&config, error, sizeof(error));
  CHECK(power == NULL, "unpaired resource hooks should fail create");
  CHECK(error[0] != '\0', "unpaired resource hooks should set an error");

  fake_init(&state);
  fill_config(&config, &state);
  config.allow_deep_sleep = true;
  config.backend.deep_sleep = NULL;
  power = bd_power_create(&config, error, sizeof(error));
  CHECK(power == NULL, "deep sleep without a backend should fail create");

  fake_init(&state);
  fill_config(&config, &state);
  config.allow_poweroff = true;
  config.backend.shutdown = NULL;
  power = bd_power_create(&config, error, sizeof(error));
  CHECK(power == NULL, "poweroff without a backend should fail create");

  fake_init(&state);
  fill_config(&config, &state);
  config.allow_light_sleep = false;
  power = bd_power_create(&config, error, sizeof(error));
  CHECK(power != NULL, "a machine with no enabled action should still exist");
  bd_power_destroy(power);
}

static void test_light_cycle(void) {
  static const bd_power_step enter[] = {
      BD_POWER_STEP_INPUT_RELEASE, BD_POWER_STEP_APP_PAUSE,
      BD_POWER_STEP_RESOURCE_PREPARE_LIGHT, BD_POWER_STEP_DISPLAY_OFF};
  static const bd_power_step leave[] = {
      BD_POWER_STEP_DISPLAY_ON, BD_POWER_STEP_RESOURCE_RESTORE_LIGHT,
      BD_POWER_STEP_APP_RESUME, BD_POWER_STEP_INPUT_ACQUIRE};

  fake state;
  fake_init(&state);
  bd_power *power = make_power(&state, false, false, false);
  CHECK(power != NULL, "create light power machine");
  if (power == NULL) {
    return;
  }

  bd_power_report report;
  CHECK(bd_power_sleep_light(power, &report) == BD_POWER_OK,
        "light sleep result");
  expect_report(&report, BD_POWER_OK, BD_POWER_STATE_LIGHT_SLEEP, "light");
  expect_calls(&state, enter, 4, "light enter");
  CHECK(report.hook_calls == 4, "light report hook calls");
  CHECK(strstr(report.detail, "not suspend-to-RAM") != NULL,
        "light detail must distinguish light sleep from deep sleep");

  fake_clear_calls(&state);
  CHECK(bd_power_sleep_light(power, &report) == BD_POWER_ALREADY,
        "duplicate light sleep");
  expect_report(&report, BD_POWER_ALREADY, BD_POWER_STATE_LIGHT_SLEEP,
                "light duplicate");
  CHECK(state.call_count == 0, "duplicate light sleep must not call hooks");

  fake_clear_calls(&state);
  CHECK(bd_power_wake(power, &report) == BD_POWER_OK, "light wake result");
  expect_report(&report, BD_POWER_OK, BD_POWER_STATE_IDLE, "light wake");
  expect_calls(&state, leave, 4, "light leave");
  CHECK(report.recovery_complete, "wake reports complete recovery");

  fake_clear_calls(&state);
  CHECK(bd_power_wake(power, &report) == BD_POWER_NOT_SLEEPING,
        "wake while idle");
  CHECK(report.result == BD_POWER_NOT_SLEEPING, "idle wake result");
  CHECK(state.call_count == 0, "idle wake must not call hooks");
  CHECK(strcmp(bd_power_state_name(bd_power_current_state(power)), "idle") == 0,
        "state name");
  bd_power_destroy(power);
}

static void test_cancel(void) {
  fake state;
  fake_init(&state);
  bd_power *power = make_power(&state, false, false, false);
  CHECK(power != NULL, "create cancel power machine");
  if (power == NULL) {
    return;
  }

  bd_power_report report;
  CHECK(bd_power_cancel(power, &report) == BD_POWER_OK, "queue cancel");
  CHECK(bd_power_pending_cancel(power), "cancel should be pending");
  CHECK(bd_power_cancel(power, &report) == BD_POWER_ALREADY,
        "duplicate cancel");
  fake_clear_calls(&state);
  CHECK(bd_power_sleep_light(power, &report) == BD_POWER_CANCELLED,
        "cancelled sleep");
  expect_report(&report, BD_POWER_CANCELLED, BD_POWER_STATE_IDLE,
                "cancelled sleep");
  CHECK(state.call_count == 0, "cancelled sleep must not call hooks");
  CHECK(!bd_power_pending_cancel(power), "cancel is consumed by sleep");

  CHECK(bd_power_sleep_light(power, &report) == BD_POWER_OK,
        "sleep after cancel");
  fake_clear_calls(&state);
  CHECK(bd_power_cancel(power, &report) == BD_POWER_OK,
        "cancel while light sleeping");
  expect_report(&report, BD_POWER_OK, BD_POWER_STATE_IDLE, "cancel light");
  CHECK(report.action == BD_POWER_ACTION_CANCEL, "cancel report action");
  CHECK(state.call_count == 4, "cancel light restores all layers");
  bd_power_destroy(power);
}

static void test_entry_failures(void) {
  struct case_data {
    bd_power_step fail;
    bd_power_step expected[8];
    int count;
    const char *label;
  } cases[] = {
      {BD_POWER_STEP_INPUT_RELEASE,
       {BD_POWER_STEP_INPUT_RELEASE, BD_POWER_STEP_INPUT_ACQUIRE}, 2,
       "input release"},
      {BD_POWER_STEP_APP_PAUSE,
       {BD_POWER_STEP_INPUT_RELEASE, BD_POWER_STEP_APP_PAUSE,
        BD_POWER_STEP_APP_RESUME, BD_POWER_STEP_INPUT_ACQUIRE},
       4, "app pause"},
      {BD_POWER_STEP_RESOURCE_PREPARE_LIGHT,
       {BD_POWER_STEP_INPUT_RELEASE, BD_POWER_STEP_APP_PAUSE,
        BD_POWER_STEP_RESOURCE_PREPARE_LIGHT,
        BD_POWER_STEP_RESOURCE_RESTORE_LIGHT, BD_POWER_STEP_APP_RESUME,
        BD_POWER_STEP_INPUT_ACQUIRE},
       6, "light resource prepare"},
      {BD_POWER_STEP_DISPLAY_OFF,
       {BD_POWER_STEP_INPUT_RELEASE, BD_POWER_STEP_APP_PAUSE,
        BD_POWER_STEP_RESOURCE_PREPARE_LIGHT, BD_POWER_STEP_DISPLAY_OFF,
        BD_POWER_STEP_DISPLAY_ON, BD_POWER_STEP_RESOURCE_RESTORE_LIGHT,
        BD_POWER_STEP_APP_RESUME, BD_POWER_STEP_INPUT_ACQUIRE},
       8, "display off"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    fake state;
    fake_init(&state);
    state.fail_step = cases[i].fail;
    bd_power *power = make_power(&state, false, false, false);
    CHECK(power != NULL, "create failure machine");
    if (power == NULL) {
      continue;
    }
    bd_power_report report;
    bd_power_result result = bd_power_sleep_light(power, &report);
    CHECK(result == BD_POWER_HOOK_FAILED, "%s: result=%s",
          cases[i].label, bd_power_result_name(result));
    expect_report(&report, BD_POWER_HOOK_FAILED, BD_POWER_STATE_IDLE,
                  cases[i].label);
    expect_calls(&state, cases[i].expected, cases[i].count, cases[i].label);
    CHECK(report.failed_step == cases[i].fail, "%s: failed step",
          cases[i].label);
    CHECK(report.rollback_attempted, "%s: rollback attempted",
          cases[i].label);
    CHECK(report.recovery_complete, "%s: recovery complete", cases[i].label);
    CHECK(state.deep_calls == 0, "%s: backend must not run", cases[i].label);
    CHECK(bd_power_current_state(power) == BD_POWER_STATE_IDLE,
          "%s: state returns idle", cases[i].label);
    bd_power_destroy(power);
  }
}

static void test_rollback_failure_and_recovery(void) {
  fake state;
  fake_init(&state);
  state.fail_step = BD_POWER_STEP_DISPLAY_OFF;
  state.fail_restore_step = BD_POWER_STEP_RESOURCE_RESTORE_LIGHT;
  bd_power *power = make_power(&state, false, false, false);
  CHECK(power != NULL, "create degraded machine");
  if (power == NULL) {
    return;
  }

  bd_power_report report;
  CHECK(bd_power_sleep_light(power, &report) == BD_POWER_DEGRADED,
        "rollback failure degrades");
  expect_report(&report, BD_POWER_DEGRADED, BD_POWER_STATE_DEGRADED,
                "degraded enter");
  CHECK(report.failed_step == BD_POWER_STEP_DISPLAY_OFF,
        "degraded report keeps first failure");
  CHECK(report.restore_failures == 1, "one restore failure");
  CHECK(!report.recovery_complete, "recovery incomplete");

  fake_clear_calls(&state);
  CHECK(bd_power_sleep_light(power, &report) ==
            BD_POWER_RECOVERY_REQUIRED,
        "sleep while degraded");
  CHECK(bd_power_cancel(power, &report) == BD_POWER_RECOVERY_REQUIRED,
        "cancel while degraded");
  CHECK(bd_power_poweroff(power, BD_POWER_POWEROFF_STANDARD, &report) ==
            BD_POWER_RECOVERY_REQUIRED,
        "poweroff while degraded");
  CHECK(state.call_count == 0, "degraded refusals must not call hooks");

  state.fail_step = BD_POWER_STEP_NONE;
  state.fail_restore_step = BD_POWER_STEP_NONE;
  fake_clear_calls(&state);
  CHECK(bd_power_wake(power, &report) == BD_POWER_OK,
        "wake retries recovery");
  expect_report(&report, BD_POWER_OK, BD_POWER_STATE_IDLE, "recovered");
  CHECK(state.call_count == 1, "only the failed light restore is retried");
  CHECK(state.calls[0] == BD_POWER_STEP_RESOURCE_RESTORE_LIGHT,
        "retry restores the light resource layer");
  bd_power_destroy(power);
}

static void test_deep_from_idle(void) {
  static const bd_power_step expected[] = {
      BD_POWER_STEP_INPUT_RELEASE, BD_POWER_STEP_APP_PAUSE,
      BD_POWER_STEP_RESOURCE_PREPARE_DEEP, BD_POWER_STEP_DISPLAY_OFF,
      BD_POWER_STEP_DEEP_SLEEP, BD_POWER_STEP_DISPLAY_ON,
      BD_POWER_STEP_RESOURCE_RESTORE_DEEP, BD_POWER_STEP_APP_RESUME,
      BD_POWER_STEP_INPUT_ACQUIRE};

  fake state;
  fake_init(&state);
  bd_power *power = make_power(&state, true, false, false);
  CHECK(power != NULL, "create deep power machine");
  if (power == NULL) {
    return;
  }

  bd_power_report report;
  CHECK(bd_power_sleep_deep(power, &report) == BD_POWER_OK,
        "deep sleep result");
  expect_report(&report, BD_POWER_OK, BD_POWER_STATE_IDLE, "deep");
  expect_calls(&state, expected, 9, "deep from idle");
  CHECK(state.deep_calls == 1, "deep backend called once");
  CHECK(report.backend_called, "deep report backend called");
  CHECK(strstr(report.detail, "unverified") != NULL,
        "deep detail must not claim verified hardware suspend");
  bd_power_destroy(power);
}

static void test_deep_from_light(void) {
  static const bd_power_step deep_calls[] = {
      BD_POWER_STEP_RESOURCE_PREPARE_DEEP, BD_POWER_STEP_DEEP_SLEEP,
      BD_POWER_STEP_RESOURCE_RESTORE_DEEP};
  static const bd_power_step wake_calls[] = {
      BD_POWER_STEP_DISPLAY_ON, BD_POWER_STEP_RESOURCE_RESTORE_LIGHT,
      BD_POWER_STEP_APP_RESUME, BD_POWER_STEP_INPUT_ACQUIRE};

  fake state;
  fake_init(&state);
  bd_power *power = make_power(&state, true, false, false);
  CHECK(power != NULL, "create deep-from-light machine");
  if (power == NULL) {
    return;
  }

  bd_power_report report;
  CHECK(bd_power_sleep_light(power, &report) == BD_POWER_OK, "light first");
  fake_clear_calls(&state);
  CHECK(bd_power_sleep_light(power, &report) == BD_POWER_ALREADY,
        "duplicate light while light");
  CHECK(state.call_count == 0, "duplicate must not call hooks");

  CHECK(bd_power_sleep_deep(power, &report) == BD_POWER_OK,
        "deep from light");
  expect_report(&report, BD_POWER_OK, BD_POWER_STATE_LIGHT_SLEEP,
                "deep from light");
  expect_calls(&state, deep_calls, 3, "deep from light calls");
  CHECK(state.deep_calls == 1, "deep backend called once");

  /* A second deep cycle is a new request, not a duplicate. */
  fake_clear_calls(&state);
  CHECK(bd_power_sleep_deep(power, &report) == BD_POWER_OK,
        "second deep from light");
  expect_calls(&state, deep_calls, 3, "second deep from light calls");
  CHECK(state.deep_calls == 2, "deep backend called twice");

  fake_clear_calls(&state);
  CHECK(bd_power_wake(power, &report) == BD_POWER_OK, "wake after deep");
  expect_calls(&state, wake_calls, 4, "wake after deep calls");
  bd_power_destroy(power);
}

static void test_deep_failures(void) {
  fake state;
  fake_init(&state);
  bd_power *power = make_power(&state, true, false, false);
  CHECK(power != NULL, "create deep failure machine");
  if (power == NULL) {
    return;
  }

  bd_power_report report;
  state.backend_deep_status = BD_POWER_BACKEND_ERROR;
  state.backend_errno = EINVAL;
  CHECK(bd_power_sleep_deep(power, &report) == BD_POWER_BACKEND_FAILED,
        "deep backend error");
  expect_report(&report, BD_POWER_BACKEND_FAILED, BD_POWER_STATE_IDLE,
                "deep backend error");
  CHECK(report.failed_step == BD_POWER_STEP_DEEP_SLEEP,
        "deep error failed step");
  CHECK(report.step_errno == EINVAL, "deep error errno");
  CHECK(report.rollback_attempted, "deep error rolled back");
  CHECK(report.recovery_complete, "deep error restored");
  CHECK(state.deep_calls == 1, "deep backend called");

  fake_clear_calls(&state);
  state.backend_deep_status = BD_POWER_BACKEND_DISABLED;
  CHECK(bd_power_sleep_deep(power, &report) == BD_POWER_DISABLED,
        "deep backend disabled");
  expect_report(&report, BD_POWER_DISABLED, BD_POWER_STATE_IDLE,
                "deep backend disabled");

  fake_clear_calls(&state);
  state.backend_deep_status = BD_POWER_BACKEND_UNSUPPORTED;
  CHECK(bd_power_sleep_deep(power, &report) == BD_POWER_UNSUPPORTED,
        "deep backend unsupported");
  expect_report(&report, BD_POWER_UNSUPPORTED, BD_POWER_STATE_IDLE,
                "deep backend unsupported");

  fake_clear_calls(&state);
  state.backend_deep_status = BD_POWER_BACKEND_NO_EFFECT;
  CHECK(bd_power_sleep_deep(power, &report) == BD_POWER_NO_EFFECT,
        "deep backend no effect");
  expect_report(&report, BD_POWER_NO_EFFECT, BD_POWER_STATE_IDLE,
                "deep backend no effect");
  bd_power_destroy(power);

  /* From light sleep, a backend failure must keep the light layer intact. */
  fake_init(&state);
  power = make_power(&state, true, false, false);
  CHECK(power != NULL, "create deep-from-light failure machine");
  if (power == NULL) {
    return;
  }
  CHECK(bd_power_sleep_light(power, &report) == BD_POWER_OK, "light first");
  fake_clear_calls(&state);
  state.backend_deep_status = BD_POWER_BACKEND_ERROR;
  CHECK(bd_power_sleep_deep(power, &report) == BD_POWER_BACKEND_FAILED,
        "deep from light backend error");
  expect_report(&report, BD_POWER_BACKEND_FAILED,
                BD_POWER_STATE_LIGHT_SLEEP, "deep from light error");
  CHECK(state.call_count == 3, "deep from light failure restores deep layer");
  CHECK(state.calls[0] == BD_POWER_STEP_RESOURCE_PREPARE_DEEP,
        "deep prepare");
  CHECK(state.calls[1] == BD_POWER_STEP_DEEP_SLEEP, "deep enter");
  CHECK(state.calls[2] == BD_POWER_STEP_RESOURCE_RESTORE_DEEP,
        "deep restore");
  CHECK(bd_power_wake(power, &report) == BD_POWER_OK, "wake after failure");
  bd_power_destroy(power);
}

static void test_deep_restore_degraded(void) {
  fake state;
  fake_init(&state);
  bd_power *power = make_power(&state, true, false, false);
  CHECK(power != NULL, "create deep restore failure machine");
  if (power == NULL) {
    return;
  }

  bd_power_report report;
  CHECK(bd_power_sleep_light(power, &report) == BD_POWER_OK, "light first");
  fake_clear_calls(&state);
  state.fail_restore_step = BD_POWER_STEP_RESOURCE_RESTORE_DEEP;
  CHECK(bd_power_sleep_deep(power, &report) == BD_POWER_DEGRADED,
        "deep restore failure degrades");
  expect_report(&report, BD_POWER_DEGRADED, BD_POWER_STATE_DEGRADED,
                "deep restore degraded");
  CHECK(report.failed_step == BD_POWER_STEP_RESOURCE_RESTORE_DEEP,
        "deep restore failed step");
  CHECK(report.restore_failures == 1, "deep restore failure count");

  state.fail_restore_step = BD_POWER_STEP_NONE;
  fake_clear_calls(&state);
  CHECK(bd_power_wake(power, &report) == BD_POWER_OK,
        "wake recovers deep restore");
  CHECK(state.call_count == 5, "wake retries display, deep, light, app, input");
  CHECK(state.calls[0] == BD_POWER_STEP_DISPLAY_ON, "display on first");
  CHECK(state.calls[1] == BD_POWER_STEP_RESOURCE_RESTORE_DEEP,
        "deep restore retried");
  CHECK(state.calls[2] == BD_POWER_STEP_RESOURCE_RESTORE_LIGHT,
        "light restore after deep");
  CHECK(state.calls[3] == BD_POWER_STEP_APP_RESUME, "app resume");
  CHECK(state.calls[4] == BD_POWER_STEP_INPUT_ACQUIRE, "input acquire");
  bd_power_destroy(power);
}

static void test_shutdown(void) {
  static const bd_power_step poweroff_calls[] = {
      BD_POWER_STEP_INPUT_RELEASE, BD_POWER_STEP_APP_PAUSE,
      BD_POWER_STEP_RESOURCE_PREPARE_DEEP, BD_POWER_STEP_DISPLAY_OFF,
      BD_POWER_STEP_POWEROFF, BD_POWER_STEP_DISPLAY_ON,
      BD_POWER_STEP_RESOURCE_RESTORE_DEEP, BD_POWER_STEP_APP_RESUME,
      BD_POWER_STEP_INPUT_ACQUIRE};

  fake state;
  fake_init(&state);
  bd_power *power = make_power(&state, false, true, true);
  CHECK(power != NULL, "create shutdown machine");
  if (power == NULL) {
    return;
  }

  bd_power_report report;
  CHECK(bd_power_poweroff(power, BD_POWER_POWEROFF_STANDARD, &report) ==
            BD_POWER_BACKEND_FAILED,
        "poweroff backend failure");
  expect_report(&report, BD_POWER_BACKEND_FAILED, BD_POWER_STATE_IDLE,
                "poweroff failure");
  expect_calls(&state, poweroff_calls, 9, "poweroff failure calls");
  CHECK(state.shutdown_calls == 1, "shutdown backend called");
  CHECK(!state.shutdown_reboot, "poweroff is not reboot");
  CHECK(report.failed_step == BD_POWER_STEP_POWEROFF, "poweroff failed step");
  CHECK(report.rollback_attempted && report.recovery_complete,
        "poweroff failure rolls back");

  fake_clear_calls(&state);
  state.shutdown_calls = 0;
  state.backend_shutdown_status = BD_POWER_BACKEND_OK;
  CHECK(bd_power_reboot(power, &report) == BD_POWER_NO_EFFECT,
        "reboot backend should not return successfully");
  expect_report(&report, BD_POWER_NO_EFFECT, BD_POWER_STATE_IDLE,
                "reboot no effect");
  CHECK(state.shutdown_calls == 1, "reboot backend called");
  CHECK(state.shutdown_reboot, "reboot flag passed");
  CHECK(report.failed_step == BD_POWER_STEP_REBOOT, "reboot failed step");

  fake_clear_calls(&state);
  state.shutdown_calls = 0;
  state.backend_shutdown_status = BD_POWER_BACKEND_DISABLED;
  CHECK(bd_power_poweroff(power, BD_POWER_POWEROFF_STANDARD, &report) ==
            BD_POWER_DISABLED,
        "poweroff backend disabled");
  expect_report(&report, BD_POWER_DISABLED, BD_POWER_STATE_IDLE,
                "poweroff disabled");
  CHECK(state.shutdown_calls == 1, "disabled backend still called");

  fake_clear_calls(&state);
  state.shutdown_calls = 0;
  state.backend_shutdown_status = BD_POWER_BACKEND_ERROR;
  CHECK(bd_power_poweroff(power, BD_POWER_POWEROFF_PMIC_SAFE, &report) ==
            BD_POWER_UNSUPPORTED,
        "PMIC safe poweroff is unsupported");
  CHECK(state.call_count == 0, "PMIC safe poweroff must not call hooks");
  CHECK(state.shutdown_calls == 0, "PMIC safe poweroff must not run backend");
  CHECK(strstr(report.detail, "AXP2202") != NULL,
        "PMIC detail names the blocker");

  /* Shutdown from light sleep keeps the light layer if it fails. */
  CHECK(bd_power_sleep_light(power, &report) == BD_POWER_OK, "light first");
  fake_clear_calls(&state);
  state.shutdown_calls = 0;
  CHECK(bd_power_poweroff(power, BD_POWER_POWEROFF_STANDARD, &report) ==
            BD_POWER_BACKEND_FAILED,
        "poweroff from light failure");
  expect_report(&report, BD_POWER_BACKEND_FAILED,
                BD_POWER_STATE_LIGHT_SLEEP, "poweroff from light failure");
  CHECK(state.call_count == 3, "light shutdown failure restores deep only");
  CHECK(state.calls[0] == BD_POWER_STEP_RESOURCE_PREPARE_DEEP,
        "deep prepare before shutdown");
  CHECK(state.calls[1] == BD_POWER_STEP_POWEROFF, "poweroff call");
  CHECK(state.calls[2] == BD_POWER_STEP_RESOURCE_RESTORE_DEEP,
        "deep restore after failed shutdown");
  CHECK(bd_power_wake(power, &report) == BD_POWER_OK, "wake after shutdown");

  bd_power_destroy(power);

  /* A machine that did not enable poweroff refuses without touching hooks. */
  fake_init(&state);
  power = make_power(&state, false, false, false);
  CHECK(power != NULL, "create poweroff-disabled machine");
  if (power == NULL) {
    return;
  }
  CHECK(bd_power_poweroff(power, BD_POWER_POWEROFF_STANDARD, &report) ==
            BD_POWER_DISABLED,
        "poweroff disabled by configuration");
  CHECK(state.call_count == 0, "disabled poweroff must not call hooks");
  bd_power_destroy(power);
}

static void test_disabled_and_reentrant(void) {
  fake state;
  fake_init(&state);
  state.reenter_on_display_off = true;
  bd_power *power = make_power(&state, false, false, false);
  CHECK(power != NULL, "create reentrant machine");
  if (power == NULL) {
    return;
  }
  bd_power_report report;
  CHECK(bd_power_sleep_light(power, &report) == BD_POWER_OK,
        "outer sleep survives reentrant request");
  CHECK(state.reenter_result == BD_POWER_BUSY,
        "reentrant sleep returns busy");
  CHECK(bd_power_current_state(power) == BD_POWER_STATE_LIGHT_SLEEP,
        "outer sleep reaches light sleep");
  CHECK(bd_power_wake(power, &report) == BD_POWER_OK, "wake after reentry");
  bd_power_destroy(power);

  fake_init(&state);
  power = make_power(&state, false, false, false);
  CHECK(power != NULL, "create disabled machine");
  if (power == NULL) {
    return;
  }
  /* Disable deep sleep at runtime by using a machine that never enabled it. */
  CHECK(bd_power_sleep_deep(power, &report) == BD_POWER_DISABLED,
        "deep sleep disabled");
  CHECK(state.call_count == 0, "disabled deep sleep must not call hooks");
  bd_power_destroy(power);
}

static void test_probe(void) {
  char root[PATH_SIZE];
  fixture_root(root, sizeof(root), "full");
  bd_power_linux linux;
  char error[256] = "";
  CHECK(bd_power_linux_init(&linux, root, false, NULL, error, sizeof(error)),
        "init probe backend");
  CHECK(strcmp(linux.sysfs_root, root) == 0, "sysfs root stored");
  CHECK(!linux.allow_state_changes, "probe backend changes disabled");

  char state_before[PATH_SIZE];
  char state_path[PATH_SIZE + 64];
  snprintf(state_path, sizeof(state_path), "%s/sys/power/state", root);
  CHECK(read_file(state_path, state_before, sizeof(state_before)),
        "read fixture state before probe");

  bd_power_probe probe;
  CHECK(bd_power_linux_probe(&linux, &probe), "probe full fixture");
  CHECK(probe.read_only, "probe is read-only");
  CHECK(probe.state_readable, "state readable");
  CHECK(probe.mem_supported, "mem supported");
  CHECK(strstr(probe.states, "mem") != NULL, "state list contains mem");
  CHECK(probe.mem_sleep_present, "mem_sleep present");
  CHECK(strstr(probe.mem_sleep_current, "deep") != NULL,
        "current mem_sleep is deep");
  CHECK(probe.usb_online_known && probe.usb_online, "usb online");
  CHECK(probe.battery_capacity_known &&
            probe.battery_capacity_percent == 73,
        "battery capacity");
  CHECK(probe.pmic_safe_poweroff == BD_POWER_SUPPORT_UNSUPPORTED,
        "PMIC safe poweroff unsupported");
  CHECK(strstr(probe.pmic_blocker, "AXP2202") != NULL, "PMIC blocker");
  CHECK(probe.error[0] == '\0', "probe has no error");

  char state_after[PATH_SIZE];
  CHECK(read_file(state_path, state_after, sizeof(state_after)),
        "read fixture state after probe");
  CHECK(strcmp(state_before, state_after) == 0,
        "probe must not modify /sys/power/state");

  fixture_root(root, sizeof(root), "limited");
  CHECK(bd_power_linux_init(&linux, root, false, NULL, error, sizeof(error)),
        "init limited backend");
  CHECK(bd_power_linux_probe(&linux, &probe), "probe limited fixture");
  CHECK(!probe.mem_supported, "limited fixture has no mem");
  CHECK(probe.mem_sleep_present, "limited mem_sleep present");
  CHECK(strcmp(probe.mem_sleep_current, "s2idle") == 0,
        "limited current mem_sleep");
  CHECK(!probe.usb_online_known, "limited fixture has no usb");
  CHECK(!probe.battery_capacity_known, "limited fixture has no battery");

  bd_power_linux missing;
  CHECK(bd_power_linux_init(&missing, "/nonexistent/brickdesk-power-test",
                            false, NULL, error, sizeof(error)),
        "init missing backend");
  CHECK(!bd_power_linux_probe(&missing, &probe),
        "probe missing sysfs fails");
  CHECK(probe.error[0] != '\0', "missing sysfs has an error");

  bd_power_linux invalid;
  CHECK(!bd_power_linux_init(&invalid, NULL, false, "deep mode", error,
                             sizeof(error)),
        "reject invalid mem_sleep mode");
  CHECK(!bd_power_linux_init(&invalid, NULL, false, "deep\n", error,
                             sizeof(error)),
        "reject mem_sleep with newline");
}

static void test_linux_backend_gate(void) {
  char root[PATH_SIZE];
  fixture_root(root, sizeof(root), "full");
  bd_power_linux linux;
  char error[256] = "";
  CHECK(bd_power_linux_init(&linux, root, false, NULL, error, sizeof(error)),
        "init gated backend");
  bd_power_backend backend = bd_power_linux_backend(&linux);
  CHECK(backend.deep_sleep != NULL && backend.shutdown != NULL,
        "gated backend still has functions");
  errno = 0;
  CHECK(backend.deep_sleep(backend.user) == BD_POWER_BACKEND_DISABLED,
        "deep sleep disabled without opt-in");
  CHECK(backend.shutdown(backend.user, false) == BD_POWER_BACKEND_DISABLED,
        "shutdown disabled without opt-in");

  char state_path[PATH_SIZE + 64];
  char state_text[PATH_SIZE];
  snprintf(state_path, sizeof(state_path), "%s/sys/power/state", root);
  CHECK(read_file(state_path, state_text, sizeof(state_text)),
        "read gated fixture state");
  CHECK(strstr(state_text, "mem") != NULL, "gated backend left fixture alone");

  CHECK(bd_power_linux_pmic_poweroff(&linux) ==
            BD_POWER_BACKEND_UNSUPPORTED,
        "PMIC call is always unsupported");
  CHECK(strlen(bd_power_linux_pmic_blocker()) > 0, "PMIC blocker text");

  /*
   * Exercise the mem_sleep gate without executing a real suspend: an
   * unsupported mode must be rejected before /sys/power/state is written.
   */
  bd_power_linux gated;
  CHECK(bd_power_linux_init(&gated, root, true, "bogus", error,
                            sizeof(error)),
        "init mode-gated backend");
  backend = bd_power_linux_backend(&gated);
  CHECK(backend.deep_sleep(backend.user) == BD_POWER_BACKEND_UNSUPPORTED,
        "unknown mem_sleep mode is unsupported");
  CHECK(read_file(state_path, state_text, sizeof(state_text)),
        "read fixture state after mode gate");
  CHECK(strstr(state_text, "mem") != NULL, "mode gate did not write state");
}

int main(void) {
  test_create_validation();
  test_light_cycle();
  test_cancel();
  test_entry_failures();
  test_rollback_failure_and_recovery();
  test_deep_from_idle();
  test_deep_from_light();
  test_deep_failures();
  test_deep_restore_degraded();
  test_shutdown();
  test_disabled_and_reentrant();
  test_probe();
  test_linux_backend_gate();

  if (g_failures != 0) {
    fprintf(stderr, "device_power_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("device_power_test: OK\n");
  return 0;
}
