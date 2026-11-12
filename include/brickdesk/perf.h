#ifndef BRICKDESK_PERF_H
#define BRICKDESK_PERF_H

/*
 * Performance policy and sysfs transaction layer for brickdesk-perfd.
 *
 * The daemon is the only writer.  It probes the real (or a test) sysfs tree,
 * resolves one of the layered profiles from perf.ini, validates every target
 * value against what the kernel advertises, and applies the result as a
 * transaction: either all writes succeed, or the values seen before the
 * transaction are written back in reverse order.
 *
 * Paths in bd_perf_system are absolute.  `root` is an optional prefix so the
 * unit tests can point at tests/fixtures/sysfs instead of /.  An empty root
 * means the real /sys and /proc.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BD_PERF_PROTOCOL_VERSION 1
#define BD_PERF_NAME_MAX 64
#define BD_PERF_GOVERNOR_MAX 32
#define BD_PERF_PATH_MAX 1024
#define BD_PERF_MAX_PROFILES 32
#define BD_PERF_MAX_APPS 32
#define BD_PERF_MAX_CORES 32
#define BD_PERF_MAX_FREQS 32
#define BD_PERF_MAX_GOVERNORS 12
#define BD_PERF_MAX_CPUS 8
#define BD_PERF_MAX_THERMAL 8
#define BD_PERF_MAX_DEVFREQ 8
#define BD_PERF_MAX_LEASES 8
#define BD_PERF_SESSION_MAX 64

/* One [profile.<name>] or [core.<name>] section.  Unset fields keep their
 * has_* flag false, so a profile only overrides what it writes. */
typedef struct bd_perf_profile {
  char name[BD_PERF_NAME_MAX + 1];
  bool has_inherit;
  char inherit[BD_PERF_NAME_MAX + 1];
  bool has_governor;
  char governor[BD_PERF_GOVERNOR_MAX + 1];
  bool has_min_khz;
  int min_khz;
  bool has_max_khz;
  int max_khz;
  bool has_online_cores;
  int online_cores;
  bool has_swappiness;
  int swappiness;
} bd_perf_profile;

typedef struct bd_perf_app {
  char name[BD_PERF_NAME_MAX + 1];
  char profile[BD_PERF_NAME_MAX + 1];
} bd_perf_app;

typedef struct bd_perf_policy {
  bd_perf_profile profiles[BD_PERF_MAX_PROFILES];
  size_t profile_count;
  bd_perf_profile cores[BD_PERF_MAX_CORES];
  size_t core_count;
  bd_perf_app apps[BD_PERF_MAX_APPS];
  size_t app_count;
  bool overflow;
} bd_perf_policy;

/* Resolved values.  Every has_* flag says whether the value was supplied by
 * the profile chain; a false flag means "leave the running value alone". */
typedef struct bd_perf_limits {
  char profile[BD_PERF_NAME_MAX + 1];
  char app[BD_PERF_NAME_MAX + 1];
  char core[BD_PERF_NAME_MAX + 1];
  bool has_governor;
  char governor[BD_PERF_GOVERNOR_MAX + 1];
  bool has_min_khz;
  int min_khz;
  bool has_max_khz;
  int max_khz;
  bool has_online_cores;
  int online_cores;
  bool has_swappiness;
  int swappiness;
} bd_perf_limits;

/*
 * Loads perf.ini from config_dir (NULL uses the build-time config directory)
 * and builds the profile/app/core tables.  Returns false for an unreadable
 * directory, a perf.ini diagnostic, too many sections, or no profiles.
 */
bool bd_perf_policy_load(bd_perf_policy *policy, const char *config_dir,
                         char *error, size_t error_size);

const bd_perf_profile *bd_perf_policy_find_profile(const bd_perf_policy *policy,
                                                   const char *name);
const bd_perf_profile *bd_perf_policy_find_core(const bd_perf_policy *policy,
                                                const char *name);
const bd_perf_app *bd_perf_policy_find_app(const bd_perf_policy *policy,
                                           const char *name);

/*
 * Resolves the three-level policy:
 *
 *   profile (explicit) > app profile > global "default", then core overlay.
 *
 * `profile` may be NULL.  `app` and `core` may be NULL or empty.  An unknown
 * explicit profile, app profile target, or core is an error; an app id that is
 * not listed in perf.ini simply falls back to the profile rule above.  Core
 * inherit replaces the app/global base with the inherited profile chain.
 */
bool bd_perf_policy_resolve(const bd_perf_policy *policy, const char *profile,
                            const char *app, const char *core,
                            bd_perf_limits *limits, char *error,
                            size_t error_size);

/* Pure lease table used by the daemon and the unit tests.  The newest lease
 * wins; when it is restored, disconnects or expires, the next newest becomes
 * effective, and an empty table means the global profile. */
typedef struct bd_perf_lease {
  bool active;
  int owner;
  uint64_t order;
  char session[BD_PERF_SESSION_MAX + 1];
  char profile[BD_PERF_NAME_MAX + 1];
  char app[BD_PERF_NAME_MAX + 1];
  char core[BD_PERF_NAME_MAX + 1];
  uint64_t expires_ms; /* 0 means no expiry */
} bd_perf_lease;

typedef struct bd_perf_lease_table {
  bd_perf_lease items[BD_PERF_MAX_LEASES];
  uint64_t next_order;
} bd_perf_lease_table;

void bd_perf_leases_init(bd_perf_lease_table *table);
bd_perf_lease *bd_perf_leases_find(bd_perf_lease_table *table,
                                   const char *session);
bd_perf_lease *bd_perf_leases_apply(bd_perf_lease_table *table,
                                    const char *session, int owner,
                                    const char *profile, const char *app,
                                    const char *core, uint64_t ttl_ms,
                                    uint64_t now_ms);
bool bd_perf_leases_restore(bd_perf_lease_table *table, const char *session);
size_t bd_perf_leases_drop_owner(bd_perf_lease_table *table, int owner);
size_t bd_perf_leases_expire(bd_perf_lease_table *table, uint64_t now_ms);
bool bd_perf_leases_next_deadline(const bd_perf_lease_table *table,
                                  uint64_t *deadline_ms);
const bd_perf_lease *bd_perf_leases_effective(
    const bd_perf_lease_table *table);

typedef struct bd_perf_thermal {
  char zone[32];
  char type[BD_PERF_NAME_MAX + 1];
  long temp_mc;
} bd_perf_thermal;

typedef struct bd_perf_devfreq {
  char name[BD_PERF_NAME_MAX + 1];
  long cur_freq;
  long available[BD_PERF_MAX_FREQS];
  size_t available_count;
  bool has_available;
} bd_perf_devfreq;

/*
 * Result of probing.  cpufreq is false when no usable policy0 is present.
 * Thermal zones and devfreq entries are read-only and only for display.
 */
typedef struct bd_perf_system {
  char root[BD_PERF_PATH_MAX];
  bool cpufreq;
  int policy_count;

  char governor_path[BD_PERF_PATH_MAX];
  char min_path[BD_PERF_PATH_MAX];
  char max_path[BD_PERF_PATH_MAX];
  char swappiness_path[BD_PERF_PATH_MAX];
  bool has_swappiness;
  int swappiness;

  char online_path[BD_PERF_MAX_CPUS][BD_PERF_PATH_MAX];
  int cpu_count; /* includes cpu0 */
  int online_count;

  char current_governor[BD_PERF_GOVERNOR_MAX + 1];
  char current_freq_path[BD_PERF_PATH_MAX];
  bool has_current_freq;
  int current_freq_khz;
  int current_min_khz;
  int current_max_khz;
  int cpuinfo_min_khz;
  int cpuinfo_max_khz;

  char governors[BD_PERF_MAX_GOVERNORS][BD_PERF_GOVERNOR_MAX + 1];
  size_t governor_count;
  int freqs[BD_PERF_MAX_FREQS];
  size_t freq_count;

  size_t thermal_count;
  bd_perf_thermal thermals[BD_PERF_MAX_THERMAL];
  size_t devfreq_count;
  bd_perf_devfreq devfreqs[BD_PERF_MAX_DEVFREQ];
} bd_perf_system;

/* Probes root + /sys/... and root + /proc/... .  Never writes. */
bool bd_perf_system_probe(const char *root, bd_perf_system *system,
                          char *error, size_t error_size);

/* The concrete delta computed from a system snapshot and resolved limits. */
typedef struct bd_perf_plan {
  char profile[BD_PERF_NAME_MAX + 1];
  char app[BD_PERF_NAME_MAX + 1];
  char core[BD_PERF_NAME_MAX + 1];

  bool governor_change;
  bool governor_fallback;
  char governor[BD_PERF_GOVERNOR_MAX + 1];
  char current_governor[BD_PERF_GOVERNOR_MAX + 1];

  bool min_change;
  int min_khz;
  int current_min_khz;
  bool max_change;
  int max_khz;
  int current_max_khz;

  bool online_change;
  int online_cores;
  int current_online_cores;

  bool swappiness_change;
  bool swappiness_unsupported;
  int swappiness;
  int current_swappiness;
} bd_perf_plan;

/*
 * Validates and clamps limits against the probed system.  Requests are
 * clamped to [cpuinfo_min_freq, cpuinfo_max_freq], then snapped down to the
 * nearest available frequency.  min > max is rejected.  A missing governor is
 * rejected, except schedutil falls back to ondemand then performance.
 */
bool bd_perf_plan_build(const bd_perf_system *system,
                        const bd_perf_limits *limits, bd_perf_plan *plan,
                        char *error, size_t error_size);

typedef struct bd_perf_apply_report {
  bool changed;
  bool rolled_back;
  int writes;
} bd_perf_apply_report;

/*
 * Applies plan as a transaction.  Writes are verified by reading the file
 * back; any failure restores previously written values in reverse order.
 * `system` supplies the whitelisted paths and is not modified.
 */
bool bd_perf_apply(const bd_perf_system *system, const bd_perf_plan *plan,
                   bd_perf_apply_report *report, char *error,
                   size_t error_size);

/* Startup values used to restore the machine when the daemon exits. */
typedef struct bd_perf_snapshot {
  bool has_governor;
  char governor[BD_PERF_GOVERNOR_MAX + 1];
  bool has_min;
  int min_khz;
  bool has_max;
  int max_khz;
  int online[BD_PERF_MAX_CPUS]; /* -1 unknown, 0 offline, 1 online */
  bool has_swappiness;
  int swappiness;
} bd_perf_snapshot;

bool bd_perf_snapshot_take(const bd_perf_system *system,
                           bd_perf_snapshot *snapshot);
bool bd_perf_snapshot_restore(const bd_perf_system *system,
                              const bd_perf_snapshot *snapshot, char *error,
                              size_t error_size);

#endif /* BRICKDESK_PERF_H */
