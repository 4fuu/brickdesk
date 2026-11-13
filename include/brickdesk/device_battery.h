#ifndef BRICKDESK_DEVICE_BATTERY_H
#define BRICKDESK_DEVICE_BATTERY_H

/*
 * Battery/charger observation and low-battery policy for brickdesk.
 *
 * The reader is a read-only view of the Linux power_supply class.  It never
 * estimates capacity from voltage, never treats a missing battery node as 0%,
 * and never writes a sysfs file, PMIC register or the power state machine.
 * Every value that the kernel does not provide (or provides as a malformed
 * value) is reported as unknown through a *_known flag.
 *
 * The policy turns snapshots into UI events and advisory recommendations.
 * It does not power off, sleep, wake or evict anything.  The shell/session
 * owner decides what to do with BD_BATTERY_RECOMMEND_PREPARE_SHUTDOWN and
 * calls bd_power_* itself after its own safety checks.
 *
 * Units follow the Linux power_supply ABI:
 *   capacity      percent (0..100)
 *   voltage       microvolts (uV)
 *   current       microamps (uA), positive while charging, negative while
 *                 discharging; the sign convention is the driver's and may be
 *                 absent on hardware that does not expose current_now
 *   temperature   tenths of a degree Celsius (305 means 30.5 C)
 *   time          seconds
 *
 * Scope, evidence, version pins and the real-hardware acceptance matrix are
 * documented in docs/device-battery.md.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BD_BATTERY_PATH_MAX 512
#define BD_BATTERY_NAME_MAX 64
#define BD_BATTERY_TYPE_MAX 32
#define BD_BATTERY_MESSAGE_MAX 192
#define BD_BATTERY_MAX_SUPPLIES 16

/* ---------------------------------------------------------------------- */
/* Reader                                                                 */
/* ---------------------------------------------------------------------- */

typedef enum bd_battery_result {
  BD_BATTERY_OK = 0,
  /* Invalid argument or an unusable configuration. */
  BD_BATTERY_ERR_ARGUMENT,
  /* There is no power_supply class, or no battery node was found. */
  BD_BATTERY_ERR_UNSUPPORTED,
  /*
   * The selected battery node disappeared (or stopped being a battery)
   * after it was opened.  The snapshot contains no measurements.
   */
  BD_BATTERY_ERR_STALE,
  /* Opening the class directory or enumerating it failed. */
  BD_BATTERY_ERR_IO
} bd_battery_result;

typedef struct bd_battery_error {
  bd_battery_result code;
  char message[BD_BATTERY_MESSAGE_MAX];
} bd_battery_error;

const char *bd_battery_result_name(bd_battery_result result);
void bd_battery_error_clear(bd_battery_error *error);

typedef enum bd_battery_supply_type {
  BD_BATTERY_SUPPLY_UNKNOWN = 0,
  BD_BATTERY_SUPPLY_BATTERY,
  BD_BATTERY_SUPPLY_USB,
  BD_BATTERY_SUPPLY_MAINS,
  BD_BATTERY_SUPPLY_UPS,
  BD_BATTERY_SUPPLY_OTHER
} bd_battery_supply_type;

const char *bd_battery_supply_type_name(bd_battery_supply_type type);

typedef enum bd_battery_charge_status {
  BD_BATTERY_CHARGE_UNKNOWN = 0,
  BD_BATTERY_CHARGE_CHARGING,
  BD_BATTERY_CHARGE_DISCHARGING,
  BD_BATTERY_CHARGE_NOT_CHARGING,
  BD_BATTERY_CHARGE_FULL
} bd_battery_charge_status;

const char *bd_battery_charge_status_name(bd_battery_charge_status status);

typedef enum bd_battery_health {
  BD_BATTERY_HEALTH_UNKNOWN = 0,
  BD_BATTERY_HEALTH_GOOD,
  BD_BATTERY_HEALTH_OVERHEAT,
  BD_BATTERY_HEALTH_DEAD,
  BD_BATTERY_HEALTH_OVER_VOLTAGE,
  BD_BATTERY_HEALTH_FAILURE,
  BD_BATTERY_HEALTH_COLD,
  BD_BATTERY_HEALTH_OVER_CURRENT,
  BD_BATTERY_HEALTH_OTHER
} bd_battery_health;

const char *bd_battery_health_name(bd_battery_health health);

/* One entry from <sysfs>/class/power_supply. */
typedef struct bd_battery_supply {
  char name[BD_BATTERY_NAME_MAX];
  /* Exact text from the type attribute, empty when unreadable. */
  char type_text[BD_BATTERY_TYPE_MAX];
  bd_battery_supply_type type;
  /* True for the battery selected by bd_battery_open/read. */
  bool selected;
  bool present_known;
  bool present;
  bool online_known;
  bool online;
  bool status_known;
  bd_battery_charge_status status;
} bd_battery_supply;

/*
 * A snapshot.  A false *_known flag means "the kernel did not provide a
 * usable value"; the matching numeric field must not be displayed or acted
 * on.  This includes a detached battery (present_known && !present), where
 * capacity/status/voltage/current/temperature/time/health are all unknown.
 *
 * read_errors counts attributes that existed but could not be read or parsed
 * (for example a transient "7x" or a value outside 0..100).  A missing
 * optional attribute is not a read error.
 */
typedef struct bd_battery_status {
  bool battery_available;
  char battery_name[BD_BATTERY_NAME_MAX];

  bool present_known;
  bool present;

  bool capacity_known;
  int capacity_percent;

  bool status_known;
  bd_battery_charge_status status;

  /*
   * True when at least one USB/Mains supply reports online=1.
   * external_power is known false only when every external supply has a
   * readable online=0.  If any external supply's online is missing/invalid and
   * none is online, external_power_known is false rather than guessing
   * "unplugged".  The aggregate scans every enumerated node, including nodes
   * beyond the public supplies[] table; truncation never hides an online
   * supply.
   */
  bool external_power_known;
  bool external_power;

  bool voltage_known;
  int64_t voltage_uv;

  bool current_known;
  int64_t current_ua;

  bool temp_known;
  int32_t temp_deci_c;

  bool time_to_empty_known;
  int64_t time_to_empty_s;

  bool time_to_full_known;
  int64_t time_to_full_s;

  bool health_known;
  bd_battery_health health;

  unsigned read_errors;
  unsigned supply_count;
  bool supplies_truncated;
  bd_battery_supply supplies[BD_BATTERY_MAX_SUPPLIES];
} bd_battery_status;

typedef struct bd_battery_config {
  /*
   * Prefix before /sys.  NULL or "" means the real /sys.  Tests point this at
   * a fake tree, for example "/tmp/bd-battery-root".
   */
  const char *sysfs_root;
  /*
   * Exact power_supply name.  NULL/"" discovers a battery automatically:
   * prefer a present "axp2202-battery", then the first present battery in
   * name order.  An explicit name must resolve to type Battery; a USB or
   * Mains node is rejected instead of being treated as a battery.
   */
  const char *battery_name;
} bd_battery_config;

typedef struct bd_battery {
  char root[BD_BATTERY_PATH_MAX];
  char class_dir[BD_BATTERY_PATH_MAX];
  char battery_name[BD_BATTERY_NAME_MAX];
  char battery_path[BD_BATTERY_PATH_MAX];
  bool explicit_battery;
  bool selected;
} bd_battery;

/*
 * Discovers the battery and records its path.  Returns UNSUPPORTED when the
 * power_supply class or a suitable battery is absent; the handle is still
 * initialized and bd_battery_read() may later acquire a battery that appears.
 * Never writes any file.
 */
bd_battery_result bd_battery_open(bd_battery *battery,
                                  const bd_battery_config *config,
                                  bd_battery_error *error);

/*
 * Reads one snapshot.  Re-enumerates supplies so USB/Mains hotplug is
 * visible.  A missing optional attribute leaves the matching field unknown.
 * A malformed value leaves that field unknown and increments read_errors.
 * A selected battery that disappeared returns STALE and clears all battery
 * measurements; the caller should close/reopen discovery.
 *
 * The handle is not modified; a handle opened while no battery was present
 * may acquire one on a later read, but it never silently switches away from a
 * battery that was already selected.
 */
bd_battery_result bd_battery_read(const bd_battery *battery,
                                  bd_battery_status *status,
                                  bd_battery_error *error);

const char *bd_battery_selected_name(const bd_battery *battery);
const char *bd_battery_selected_path(const bd_battery *battery);
/*
 * True when the last bd_battery_open() selected a battery.  A later read may
 * acquire a battery after an initially unsupported open without mutating the
 * handle; bd_battery_status.battery_available is authoritative per snapshot.
 */
bool bd_battery_selected(const bd_battery *battery);

/* ---------------------------------------------------------------------- */
/* Low-battery policy                                                     */
/* ---------------------------------------------------------------------- */

typedef enum bd_battery_policy_level {
  BD_BATTERY_LEVEL_UNKNOWN = 0,
  BD_BATTERY_LEVEL_NORMAL,
  BD_BATTERY_LEVEL_REMINDER,
  BD_BATTERY_LEVEL_CRITICAL
} bd_battery_policy_level;

const char *bd_battery_policy_level_name(bd_battery_policy_level level);

typedef enum bd_battery_recommendation {
  BD_BATTERY_RECOMMEND_NONE = 0,
  BD_BATTERY_RECOMMEND_SHOW_REMINDER,
  BD_BATTERY_RECOMMEND_SHOW_CRITICAL,
  /*
   * Advisory only.  Emitted only when capacity is confirmed critical, the
   * battery is known present, and external power is known to be absent.
   * The consumer still owns the decision, the user prompt and the call to
   * bd_power_poweroff(); this module never acts on it.
   */
  BD_BATTERY_RECOMMEND_PREPARE_SHUTDOWN
} bd_battery_recommendation;

const char *bd_battery_recommendation_name(
    bd_battery_recommendation recommendation);

#define BD_BATTERY_EVENT_LOW (1u << 0)
#define BD_BATTERY_EVENT_CRITICAL (1u << 1)
#define BD_BATTERY_EVENT_RECOVERED (1u << 2)
#define BD_BATTERY_EVENT_PLUGGED (1u << 3)
#define BD_BATTERY_EVENT_UNPLUGGED (1u << 4)
#define BD_BATTERY_EVENT_UNKNOWN (1u << 5)

const char *bd_battery_event_name(uint32_t event);

typedef struct bd_battery_policy_config {
  /* ---- Basic settings: safe to expose on the home/settings page. ---- */
  bool enabled;
  /* Percent at or below which the reminder level is entered. */
  int reminder_percent;
  /* Percent at or below which the critical level is entered. */
  int critical_percent;
  /* Suppress alerts while external power is present or charging is known. */
  bool suppress_while_plugged;
  /* Allow PREPARE_SHUTDOWN; the consumer still owns the actual shutdown. */
  bool recommend_shutdown;

  /* ---- Advanced settings: config file only, not a home-page option. ---- */
  /* Capacity must rise to reminder + this before leaving the reminder level. */
  int reminder_hysteresis_percent;
  /* Capacity must rise to critical + this before leaving the critical level. */
  int critical_hysteresis_percent;
  /* Consecutive samples required before a threshold transition is applied. */
  unsigned confirm_samples;
  /* Re-alert intervals while the level remains active and unplugged. */
  uint32_t reminder_repeat_ms;
  uint32_t critical_repeat_ms;
  /* Sampling deadlines; the policy returns an absolute monotonic deadline. */
  uint32_t sample_normal_ms;
  uint32_t sample_reminder_ms;
  uint32_t sample_critical_ms;
  uint32_t sample_charging_ms;
  /* A pending threshold transition is sampled sooner than a stable level. */
  uint32_t sample_confirm_ms;
  /* Unknown capacity backs off from initial to max instead of polling fast. */
  uint32_t unknown_initial_ms;
  uint32_t unknown_max_ms;
} bd_battery_policy_config;

/*
 * Defaults: 20% reminder, 5% critical, 3/2 point hysteresis, 2 confirming
 * samples, 10 min/2 min repeat, 60/30/15/60 s normal/reminder/critical/
 * charging sampling, 5 s confirmation sampling, 5 s..120 s unknown backoff.
 */
void bd_battery_policy_config_default(bd_battery_policy_config *config);

/*
 * Validates an enabled configuration.  A disabled configuration is accepted
 * as-is.  On failure, error receives a readable reason when it is non-NULL.
 */
bool bd_battery_policy_config_validate(const bd_battery_policy_config *config,
                                       char *error, size_t error_size);

typedef enum bd_battery_policy_result {
  BD_BATTERY_POLICY_OK = 0,
  BD_BATTERY_POLICY_DISABLED,
  BD_BATTERY_POLICY_INVALID
} bd_battery_policy_result;

const char *bd_battery_policy_result_name(bd_battery_policy_result result);

typedef struct bd_battery_policy_output {
  uint32_t events;
  bd_battery_policy_level level;
  bd_battery_recommendation recommendation;
  bool capacity_known;
  bool plugged_in_known;
  bool plugged_in;
  bool suppressed;
  /* Absolute CLOCK_MONOTONIC deadline and the delta from the input now. */
  uint64_t next_sample_ms;
  uint32_t next_sample_after_ms;
} bd_battery_policy_output;

typedef struct bd_battery_policy {
  bd_battery_policy_config config;

  bd_battery_policy_level level;
  bd_battery_policy_level candidate;
  unsigned candidate_count;

  bool initialized;
  bool plug_state_seen;
  bool plugged_known;
  bool plugged;
  bool suppressed;

  bool low_alerted;
  bool critical_alerted;
  uint64_t last_low_ms;
  uint64_t last_critical_ms;

  unsigned unknown_streak;
  uint64_t next_sample_ms;
} bd_battery_policy;

/*
 * Initializes the policy and asks the caller to sample immediately:
 * next_sample_ms == now_ms.  NULL config uses the defaults.  Returns
 * DISABLED for an explicitly disabled policy; the policy is still initialized
 * with next_sample_ms == 0.
 */
bd_battery_policy_result bd_battery_policy_init(
    bd_battery_policy *policy, const bd_battery_policy_config *config,
    uint64_t now_ms, bd_battery_policy_output *output,
    bd_battery_error *error);

/*
 * Feeds one snapshot.  status may be NULL, which is treated as unknown
 * capacity.  Call at or after output.next_sample_ms, or immediately after an
 * external event such as a power_supply uevent.  The policy never sleeps,
 * never reads sysfs and never calls into the power state machine.
 *
 * An unknown capacity resets the level to UNKNOWN and can never produce LOW,
 * CRITICAL, RECOVERED or PREPARE_SHUTDOWN.  It schedules a backoff sample so a
 * device without a battery is not polled aggressively.
 *
 * When external power is unknown, the policy uses a Known charging/full
 * battery status as "plugged" and Known discharging as "unplugged" so
 * suppression and recovery still work on kernels that do not expose an
 * external supply node.  Shutdown recommendations still require
 * external_power_known == true, so an inferred state never triggers one.
 */
bd_battery_policy_result bd_battery_policy_update(
    bd_battery_policy *policy, const bd_battery_status *status,
    uint64_t now_ms, bd_battery_policy_output *output);

uint64_t bd_battery_policy_next_sample_ms(const bd_battery_policy *policy);

#ifdef __cplusplus
}
#endif

#endif /* BRICKDESK_DEVICE_BATTERY_H */
