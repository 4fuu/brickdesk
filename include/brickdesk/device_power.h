#ifndef BRICKDESK_DEVICE_POWER_H
#define BRICKDESK_DEVICE_POWER_H

/*
 * Power state machine and Linux backend for brickdesk.
 *
 * The module owns no timers and no threads.  The desktop shell owns the
 * policy (idle time, power-button hold, charging) and calls the explicit
 * functions below from its event loop.  Sleep consists of two distinct
 * layers:
 *
 *   light sleep: caller hooks release input, pause the foreground app,
 *                prepare resources and turn the display off.  This is a
 *                software light-sleep state, not suspend-to-RAM.
 *   deep sleep:  the kernel backend writes "mem" to /sys/power/state.
 *                The state machine never falls back to light sleep when the
 *                backend is unsupported or fails.
 *
 * All hooks run on the calling thread.  Re-entering the state machine from a
 * hook returns BD_POWER_BUSY instead of starting a nested transition.
 * Hooks that change state must be idempotent: the state machine calls the
 * matching restore hook even when the enter hook reported a failure, because
 * the hook may have applied a partial change before failing.
 *
 * Hooks are supplied by the caller (the shell / session manager / M4-A
 * display and audio owners).  This header deliberately has no dependency on
 * SDL, GLES, ALSA or the backlight implementation.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BD_POWER_PATH_MAX 1024
#define BD_POWER_TEXT_MAX 256
#define BD_POWER_DETAIL_MAX 192

typedef enum bd_power_result {
  BD_POWER_OK = 0,
  /* The requested action was already active or a cancel was already queued. */
  BD_POWER_ALREADY,
  /* A transition is in progress, or a hook re-entered the state machine. */
  BD_POWER_BUSY,
  /* A queued sleep request was cancelled before it started. */
  BD_POWER_CANCELLED,
  /* Wake was requested while the machine is already idle. */
  BD_POWER_NOT_SLEEPING,
  /* The action is disabled by configuration or by the backend safety gate. */
  BD_POWER_DISABLED,
  /* The backend or platform does not implement the action. */
  BD_POWER_UNSUPPORTED,
  /* A previous restore failed; call bd_power_wake() to retry recovery. */
  BD_POWER_RECOVERY_REQUIRED,
  /* A caller hook returned nonzero. */
  BD_POWER_HOOK_FAILED,
  /* The backend returned an error (errno is in the report). */
  BD_POWER_BACKEND_FAILED,
  /* The shutdown backend returned; poweroff/reboot cannot be proven. */
  BD_POWER_NO_EFFECT,
  /* The requested transition happened, but a restore hook failed. */
  BD_POWER_DEGRADED,
  /* Invalid argument or an incomplete configuration. */
  BD_POWER_INVALID
} bd_power_result;

typedef enum bd_power_state {
  BD_POWER_STATE_UNKNOWN = -1,
  BD_POWER_STATE_IDLE = 0,
  BD_POWER_STATE_ENTERING_LIGHT,
  BD_POWER_STATE_LIGHT_SLEEP,
  BD_POWER_STATE_ENTERING_DEEP,
  BD_POWER_STATE_WAKING,
  BD_POWER_STATE_SHUTTING_DOWN,
  BD_POWER_STATE_REBOOTING,
  /* Resources may still be paused because a restore hook failed. */
  BD_POWER_STATE_DEGRADED
} bd_power_state;

typedef enum bd_power_action {
  BD_POWER_ACTION_NONE = 0,
  BD_POWER_ACTION_LIGHT_SLEEP,
  BD_POWER_ACTION_DEEP_SLEEP,
  BD_POWER_ACTION_WAKE,
  BD_POWER_ACTION_CANCEL,
  BD_POWER_ACTION_POWEROFF,
  BD_POWER_ACTION_REBOOT
} bd_power_action;

typedef enum bd_power_sleep_mode {
  BD_POWER_SLEEP_LIGHT = 1,
  BD_POWER_SLEEP_DEEP = 2
} bd_power_sleep_mode;

typedef enum bd_power_poweroff_mode {
  /* Standard Linux reboot(RB_POWER_OFF); unverified on the Brick Pro. */
  BD_POWER_POWEROFF_STANDARD = 0,
  /*
   * Vendor AXP2202 soft-poweroff sequence.  Always unsupported until the
   * sequence is verified on TG4040 hardware: no registers are written here.
   */
  BD_POWER_POWEROFF_PMIC_SAFE = 1
} bd_power_poweroff_mode;

typedef enum bd_power_step {
  BD_POWER_STEP_NONE = 0,
  BD_POWER_STEP_INPUT_RELEASE,
  BD_POWER_STEP_APP_PAUSE,
  BD_POWER_STEP_RESOURCE_PREPARE_LIGHT,
  BD_POWER_STEP_RESOURCE_PREPARE_DEEP,
  BD_POWER_STEP_DISPLAY_OFF,
  BD_POWER_STEP_DEEP_SLEEP,
  BD_POWER_STEP_DISPLAY_ON,
  BD_POWER_STEP_RESOURCE_RESTORE_DEEP,
  BD_POWER_STEP_RESOURCE_RESTORE_LIGHT,
  BD_POWER_STEP_APP_RESUME,
  BD_POWER_STEP_INPUT_ACQUIRE,
  BD_POWER_STEP_POWEROFF,
  BD_POWER_STEP_REBOOT
} bd_power_step;

typedef enum bd_power_backend_status {
  BD_POWER_BACKEND_OK = 0,
  BD_POWER_BACKEND_ERROR,
  BD_POWER_BACKEND_DISABLED,
  BD_POWER_BACKEND_UNSUPPORTED,
  BD_POWER_BACKEND_NO_EFFECT
} bd_power_backend_status;

typedef enum bd_power_support {
  BD_POWER_SUPPORT_UNKNOWN = 0,
  BD_POWER_SUPPORT_SUPPORTED,
  BD_POWER_SUPPORT_UNSUPPORTED,
  BD_POWER_SUPPORT_DISABLED
} bd_power_support;

/*
 * Caller hooks.  Every function returns 0 on success and nonzero on failure.
 * Pairs (input_release/input_acquire, app_pause/app_resume,
 * display_off/display_on, resource_prepare/resource_restore) must accept being
 * called in either order and after a partially applied failure.
 *
 * Required for light/deep sleep and for poweroff/reboot:
 *   input_release, input_acquire, app_pause, app_resume,
 *   display_off, display_on.
 * resource_prepare/resource_restore are optional but must be supplied as a
 * pair.  bd_power_create() rejects a missing required hook; a caller with no
 * physical display can supply a no-op display pair.  Deep sleep still requires
 * a working kernel backend and never substitutes display_off for it.
 */
typedef struct bd_power_hooks {
  void *user;
  /* Stop delivering/consuming input; keep the wake source readable. */
  int (*input_release)(void *user);
  /* Pause the foreground app and persist what the session layer owns. */
  int (*app_pause)(void *user);
  /* Stop services / flush state for mode.  May be called with LIGHT then DEEP. */
  int (*resource_prepare)(void *user, bd_power_sleep_mode mode);
  /* Turn the panel/backlight off.  This is not a substitute for deep sleep. */
  int (*display_off)(void *user);
  int (*display_on)(void *user);
  /* Undo resource_prepare for the same mode; safe after a partial prepare. */
  int (*resource_restore)(void *user, bd_power_sleep_mode mode);
  int (*app_resume)(void *user);
  /* Resume input delivery after app_resume. */
  int (*input_acquire)(void *user);
} bd_power_hooks;

/*
 * Kernel/system backend.  deep_sleep must block until the sleep/resume cycle
 * returns and return OK only after the kernel accepted "mem".  It cannot prove
 * that the machine actually lost power; the acceptance matrix in
 * docs/device-power.md covers that on hardware.
 *
 * shutdown must not return on success.  A return value of OK is therefore
 * reported as BD_POWER_NO_EFFECT (with backend status
 * BD_POWER_BACKEND_NO_EFFECT) and the quiesce hooks are rolled back.
 */
typedef struct bd_power_backend {
  void *user;
  bd_power_backend_status (*deep_sleep)(void *user);
  bd_power_backend_status (*shutdown)(void *user, bool reboot);
} bd_power_backend;

typedef struct bd_power_config {
  bd_power_hooks hooks;
  bd_power_backend backend;
  bool allow_light_sleep;
  bool allow_deep_sleep;
  bool allow_poweroff;
  bool allow_reboot;
} bd_power_config;

typedef struct bd_power_report {
  bd_power_action action;
  bd_power_sleep_mode mode;
  bd_power_result result;
  bd_power_state state_before;
  bd_power_state state_after;
  /* First failing step, or NONE. */
  bd_power_step failed_step;
  /* Raw hook return code, or errno captured from the backend. */
  int step_code;
  int step_errno;
  bool backend_called;
  bool rollback_attempted;
  bool recovery_complete;
  unsigned hook_calls;
  unsigned restore_failures;
  char detail[BD_POWER_DETAIL_MAX];
} bd_power_report;

typedef struct bd_power bd_power;

/*
 * Creates a state machine from a copied configuration.  Returns NULL and
 * writes a diagnostic to error when an enabled action lacks a required hook
 * or backend function, or when a prepare/restore pair is incomplete.
 */
bd_power *bd_power_create(const bd_power_config *config, char *error,
                          size_t error_size);
/* Does not restore hooks; call bd_power_wake() first if sleeping. */
void bd_power_destroy(bd_power *power);

/*
 * Transitions (all synchronous, all on the calling thread):
 *
 *   sleep_light: IDLE -> LIGHT_SLEEP
 *   sleep_deep:  IDLE -> IDLE, or LIGHT_SLEEP -> LIGHT_SLEEP after a
 *                sleep/resume cycle.  LIGHT_SLEEP keeps the caller's light
 *                layer, so the shell can later wake fully.
 *   wake:        LIGHT_SLEEP or DEGRADED -> IDLE, retrying any failed restore.
 *   cancel:      IDLE queues a cancel for the next sleep request;
 *                LIGHT_SLEEP cancels by waking.
 *   poweroff:    IDLE or LIGHT_SLEEP -> backend.  Standard Linux poweroff
 *                only; PMIC_SAFE returns BD_POWER_UNSUPPORTED.
 *   reboot:      IDLE or LIGHT_SLEEP -> backend.
 *
 * Duplicate light-sleep requests return BD_POWER_ALREADY without calling
 * hooks.  An in-flight transition returns BD_POWER_BUSY.  A cancelled queued
 * sleep returns BD_POWER_CANCELLED without calling hooks.  A failed restore
 * leaves the machine DEGRADED and returns BD_POWER_DEGRADED; only wake/cancel
 * may be used to retry recovery.  report may be NULL; the last report is also
 * available through bd_power_last_report().
 */
bd_power_result bd_power_sleep_light(bd_power *power, bd_power_report *report);
bd_power_result bd_power_sleep_deep(bd_power *power, bd_power_report *report);
bd_power_result bd_power_wake(bd_power *power, bd_power_report *report);
bd_power_result bd_power_cancel(bd_power *power, bd_power_report *report);
bd_power_result bd_power_poweroff(bd_power *power,
                                  bd_power_poweroff_mode mode,
                                  bd_power_report *report);
bd_power_result bd_power_reboot(bd_power *power, bd_power_report *report);

bd_power_state bd_power_current_state(const bd_power *power);
bool bd_power_pending_cancel(const bd_power *power);
/* Valid until the next call on power, or until destroy. */
const bd_power_report *bd_power_last_report(const bd_power *power);

const char *bd_power_state_name(bd_power_state state);
const char *bd_power_result_name(bd_power_result result);
const char *bd_power_action_name(bd_power_action action);
const char *bd_power_step_name(bd_power_step step);

/* Read-only Linux capability probe.  Never writes any file. */
typedef struct bd_power_probe {
  bool read_only;
  bool state_readable;
  bool mem_supported;
  char states[BD_POWER_TEXT_MAX];
  bool mem_sleep_present;
  char mem_sleep_modes[BD_POWER_TEXT_MAX];
  char mem_sleep_current[BD_POWER_TEXT_MAX];
  bool usb_online_known;
  bool usb_online;
  bool battery_capacity_known;
  int battery_capacity_percent;
  bd_power_support pmic_safe_poweroff;
  char pmic_blocker[BD_POWER_DETAIL_MAX];
  char error[BD_POWER_DETAIL_MAX];
} bd_power_probe;

typedef struct bd_power_linux {
  char sysfs_root[BD_POWER_PATH_MAX];
  char mem_sleep_mode[32];
  bool allow_state_changes;
} bd_power_linux;

/*
 * Initializes a Linux backend.  sysfs_root is "" or "/" for the real sysfs,
 * or a fixture prefix for tests/probes.  allow_state_changes is the explicit
 * opt-in gate for writing /sys/power/state, /sys/power/mem_sleep and for the
 * reboot(2) syscall; when false every state-changing backend function returns
 * BD_POWER_BACKEND_DISABLED.
 *
 * mem_sleep_mode may be NULL/"" to keep the kernel's current selection, or a
 * mode such as "deep"/"s2idle".  /sys/power/mem_sleep is optional on the
 * vendor 4.9.191 kernel; when a mode is requested it must be advertised or the
 * backend returns UNSUPPORTED before writing "mem".
 */
bool bd_power_linux_init(bd_power_linux *linux, const char *sysfs_root,
                         bool allow_state_changes,
                         const char *mem_sleep_mode, char *error,
                         size_t error_size);
bool bd_power_linux_probe(const bd_power_linux *linux,
                          bd_power_probe *probe);
bd_power_backend bd_power_linux_backend(bd_power_linux *linux);

/* Always BD_POWER_BACKEND_UNSUPPORTED; see the blocker text for the reason. */
bd_power_backend_status bd_power_linux_pmic_poweroff(bd_power_linux *linux);
const char *bd_power_linux_pmic_blocker(void);

#endif /* BRICKDESK_DEVICE_POWER_H */
