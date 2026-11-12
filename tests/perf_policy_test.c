#define _POSIX_C_SOURCE 200809L

#include "brickdesk/perf.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef BRICKDESK_PERF_FIXTURE_DIR
#define BRICKDESK_PERF_FIXTURE_DIR "tests/fixtures"
#endif

#define PATH_SIZE 1024

static int g_failures;

#define CHECK(condition, ...)                  \
  do {                                         \
    if (!(condition)) {                        \
      fprintf(stderr, "perf_policy_test: ");   \
      fprintf(stderr, __VA_ARGS__);            \
      fprintf(stderr, "\n");                   \
      ++g_failures;                            \
    }                                          \
  } while (0)

static char *make_temp_copy(void) {
  char template[] = "/tmp/bd_perf_test.XXXXXX";
  char *directory = mkdtemp(template);
  if (directory == NULL) {
    fprintf(stderr, "perf_policy_test: mkdtemp: %s\n", strerror(errno));
    exit(1);
  }
  char *copy = strdup(directory);
  if (copy == NULL) {
    exit(1);
  }
  char command[PATH_SIZE * 2];
  int written = snprintf(command, sizeof(command), "cp -a '%s/sysfs/.' '%s/'",
                         BRICKDESK_PERF_FIXTURE_DIR, copy);
  if (written <= 0 || (size_t)written >= sizeof(command) ||
      system(command) != 0) {
    fprintf(stderr, "perf_policy_test: cannot copy sysfs fixture\n");
    exit(1);
  }
  return copy;
}

static void remove_temp_copy(char *directory) {
  if (directory == NULL) {
    return;
  }
  char command[PATH_SIZE * 2];
  snprintf(command, sizeof(command), "rm -rf '%s'", directory);
  int result = system(command);
  (void)result;
  free(directory);
}

static bool fixture_path(const char *root, const char *relative, char *out,
                         size_t out_size) {
  int written = snprintf(out, out_size, "%s/%s", root, relative);
  return written > 0 && (size_t)written < out_size;
}

static bool read_text(const char *path, char *out, size_t out_size) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  size_t used = fread(out, 1, out_size - 1, file);
  bool ok = !ferror(file);
  out[used] = '\0';
  fclose(file);
  size_t length = strlen(out);
  while (length > 0 &&
         (out[length - 1] == '\n' || out[length - 1] == '\r' ||
          out[length - 1] == ' ' || out[length - 1] == '\t')) {
    out[--length] = '\0';
  }
  return ok;
}

static long fixture_int(const char *root, const char *relative) {
  char path[PATH_SIZE];
  char text[64];
  if (!fixture_path(root, relative, path, sizeof(path)) ||
      !read_text(path, text, sizeof(text))) {
    return -1;
  }
  return strtol(text, NULL, 10);
}

static bool probe_fixture(const char *root, bd_perf_system *system) {
  char error[256] = "";
  if (!bd_perf_system_probe(root, system, error, sizeof(error))) {
    fprintf(stderr, "perf_policy_test: probe: %s\n", error);
    return false;
  }
  CHECK(system->cpufreq, "fixture has no cpufreq policy");
  CHECK(system->governor_count == 4, "expected 4 governors, got %zu",
        system->governor_count);
  CHECK(system->freq_count == 9, "expected 9 frequencies, got %zu",
        system->freq_count);
  CHECK(system->cpu_count == 4, "expected 4 CPUs, got %d", system->cpu_count);
  CHECK(system->has_current_freq && system->current_freq_khz == 1200000,
        "expected current frequency 1200000, got %d",
        system->current_freq_khz);
  return true;
}

static bool resolve_ok(const bd_perf_policy *policy, const char *profile,
                       const char *app, const char *core,
                       bd_perf_limits *limits) {
  char error[256] = "";
  if (!bd_perf_policy_resolve(policy, profile, app, core, limits, error,
                              sizeof(error))) {
    fprintf(stderr, "perf_policy_test: resolve failed: %s\n", error);
    return false;
  }
  return true;
}

static void test_inherit(const bd_perf_policy *policy) {
  bd_perf_limits limits;

  CHECK(resolve_ok(policy, "performance", NULL, NULL, &limits),
        "performance resolves");
  CHECK(limits.has_governor && strcmp(limits.governor, "performance") == 0,
        "performance governor");
  CHECK(limits.has_min_khz && limits.min_khz == 816000,
        "performance min comes from its own field");
  CHECK(limits.has_max_khz && limits.max_khz == 1800000,
        "performance max comes from its own field");
  CHECK(limits.has_online_cores && limits.online_cores == 4,
        "performance online cores comes from default");
  CHECK(limits.has_swappiness && limits.swappiness == 20,
        "performance swappiness comes from default");

  CHECK(resolve_ok(policy, "auto", NULL, NULL, &limits), "auto resolves");
  CHECK(limits.has_governor && strcmp(limits.governor, "schedutil") == 0,
        "auto governor follows default");
  CHECK(limits.has_max_khz && limits.max_khz == 1200000,
        "auto max follows default");
  CHECK(limits.has_online_cores && limits.online_cores == 4,
        "auto online cores follows default");

  CHECK(resolve_ok(policy, "balanced", NULL, NULL, &limits),
        "balanced resolves");
  CHECK(limits.has_governor && strcmp(limits.governor, "schedutil") == 0,
        "balanced keeps default governor");
  CHECK(limits.has_min_khz && limits.min_khz == 408000,
        "balanced keeps default min");
  CHECK(limits.has_max_khz && limits.max_khz == 1416000,
        "balanced overrides max");

  CHECK(resolve_ok(policy, NULL, "test.app", NULL, &limits),
        "app profile resolves");
  CHECK(strcmp(limits.profile, "performance") == 0,
        "app profile selects performance");
  CHECK(strcmp(limits.app, "test.app") == 0, "app id is reported");

  CHECK(resolve_ok(policy, NULL, "test.app", "snes9x", &limits),
        "core profile resolves");
  CHECK(strcmp(limits.profile, "performance") == 0,
        "core inherits the performance chain");
  CHECK(strcmp(limits.core, "snes9x") == 0, "core name is reported");
  CHECK(limits.has_max_khz && limits.max_khz == 1416000,
        "core overrides max");
  CHECK(limits.has_min_khz && limits.min_khz == 816000,
        "core keeps performance min");

  CHECK(resolve_ok(policy, NULL, "test.app", "pcsx", &limits),
        "pcsx core resolves");
  CHECK(limits.has_max_khz && limits.max_khz == 1800000,
        "pcsx allows full performance max");

  CHECK(resolve_ok(policy, NULL, "unlisted.app", NULL, &limits),
        "unknown app falls back");
  CHECK(strcmp(limits.profile, "default") == 0,
        "unknown app uses the default profile");
}

static void test_cycles(const bd_perf_policy *policy) {
  char error[256] = "";
  bd_perf_limits limits;

  CHECK(!bd_perf_policy_resolve(policy, "cycle.a", NULL, NULL, &limits, error,
                                sizeof(error)),
        "cycle.a must fail");
  CHECK(strstr(error, "inheritance cycle") != NULL,
        "cycle error names the cycle: %s", error);

  CHECK(!bd_perf_policy_resolve(policy, NULL, "test.cycle", NULL, &limits,
                                error, sizeof(error)),
        "app cycle must fail");
  CHECK(strstr(error, "inheritance cycle") != NULL, "app cycle error: %s",
        error);

  CHECK(!bd_perf_policy_resolve(policy, NULL, "test.app", "cycle", &limits,
                                error, sizeof(error)),
        "core cycle must fail");
  CHECK(strstr(error, "inheritance cycle") != NULL, "core cycle error: %s",
        error);

  CHECK(!bd_perf_policy_resolve(policy, "unknown_inherit", NULL, NULL, &limits,
                                error, sizeof(error)),
        "unknown inherit must fail");
  CHECK(strstr(error, "missing_profile") != NULL,
        "unknown inherit names the target: %s", error);

  CHECK(!bd_perf_policy_resolve(policy, NULL, "test.missing", NULL, &limits,
                                error, sizeof(error)),
        "unknown app profile must fail");
  CHECK(strstr(error, "does_not_exist") != NULL,
        "unknown app profile names the target: %s", error);

  CHECK(!bd_perf_policy_resolve(policy, "no_such_profile", NULL, NULL, &limits,
                                error, sizeof(error)),
        "unknown explicit profile must fail");
}

static void test_clamp(const char *root) {
  bd_perf_system system;
  if (!probe_fixture(root, &system)) {
    return;
  }

  bd_perf_limits limits;
  memset(&limits, 0, sizeof(limits));
  limits.has_governor = true;
  snprintf(limits.governor, sizeof(limits.governor), "schedutil");
  limits.has_min_khz = true;
  limits.min_khz = 100000;
  limits.has_max_khz = true;
  limits.max_khz = 3000000;

  bd_perf_plan plan;
  char error[256] = "";
  CHECK(bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error)),
        "clamp plan builds: %s", error);
  CHECK(plan.min_khz == 408000, "min is clamped to cpuinfo_min: %d",
        plan.min_khz);
  CHECK(plan.max_khz == 2000000, "max is clamped to cpuinfo_max: %d",
        plan.max_khz);

  memset(&limits, 0, sizeof(limits));
  limits.has_min_khz = true;
  limits.min_khz = 900000;
  limits.has_max_khz = true;
  limits.max_khz = 1700000;
  CHECK(bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error)),
        "snap plan builds: %s", error);
  CHECK(plan.min_khz == 816000,
        "min snaps down to the nearest available frequency: %d", plan.min_khz);
  CHECK(plan.max_khz == 1608000,
        "max snaps down to the nearest available frequency: %d", plan.max_khz);

  memset(&limits, 0, sizeof(limits));
  limits.has_min_khz = true;
  limits.min_khz = 1608000;
  limits.has_max_khz = true;
  limits.max_khz = 1200000;
  CHECK(!bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error)),
        "min > max must be rejected");
  CHECK(strstr(error, "greater") != NULL, "min > max error: %s", error);

  memset(&limits, 0, sizeof(limits));
  limits.has_online_cores = true;
  limits.online_cores = 5;
  CHECK(!bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error)),
        "online_cores 5 must be rejected");
  limits.online_cores = 1;
  CHECK(!bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error)),
        "online_cores 1 must be rejected");
  limits.online_cores = 2;
  CHECK(bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error)),
        "online_cores 2 builds: %s", error);
  CHECK(plan.online_change && plan.online_cores == 2,
        "online_cores 2 is a change from 4");
}

static void remove_governor(bd_perf_system *system, const char *name) {
  for (size_t i = 0; i < system->governor_count; ++i) {
    if (strcmp(system->governors[i], name) != 0) {
      continue;
    }
    for (size_t j = i + 1; j < system->governor_count; ++j) {
      snprintf(system->governors[j - 1], sizeof(system->governors[j - 1]),
               "%s", system->governors[j]);
    }
    --system->governor_count;
    system->governors[system->governor_count][0] = '\0';
    return;
  }
}

static void test_governor_whitelist(const char *root) {
  bd_perf_system system;
  if (!probe_fixture(root, &system)) {
    return;
  }
  bd_perf_limits limits;
  memset(&limits, 0, sizeof(limits));
  limits.has_governor = true;
  snprintf(limits.governor, sizeof(limits.governor), "bogus");

  bd_perf_plan plan;
  char error[256] = "";
  CHECK(!bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error)),
        "unknown governor must be rejected");
  CHECK(strstr(error, "not available") != NULL, "governor error: %s", error);

  memset(&limits, 0, sizeof(limits));
  limits.has_governor = true;
  snprintf(limits.governor, sizeof(limits.governor), "performance");
  CHECK(bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error)),
        "governor-only plan builds: %s", error);
  CHECK(plan.governor_change && !plan.min_change && !plan.max_change,
        "governor-only plan leaves frequency limits alone");

  remove_governor(&system, "powersave");
  snprintf(limits.governor, sizeof(limits.governor), "powersave");
  CHECK(!bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error)),
        "known but unavailable governor must be rejected");
  CHECK(strstr(error, "not available") != NULL,
        "unavailable governor error: %s", error);

  remove_governor(&system, "schedutil");
  snprintf(limits.governor, sizeof(limits.governor), "schedutil");
  CHECK(bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error)),
        "schedutil falls back when unavailable: %s", error);
  CHECK(strcmp(plan.governor, "ondemand") == 0,
        "schedutil falls back to ondemand, got %s", plan.governor);
  CHECK(plan.governor_fallback, "fallback is reported");

  remove_governor(&system, "ondemand");
  CHECK(bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error)),
        "schedutil falls back to performance: %s", error);
  CHECK(strcmp(plan.governor, "performance") == 0,
        "second fallback is performance, got %s", plan.governor);

  remove_governor(&system, "performance");
  CHECK(!bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error)),
        "no usable governor must be rejected");
}

static void test_rollback(const char *root, const bd_perf_policy *policy) {
  bd_perf_system system;
  if (!probe_fixture(root, &system)) {
    return;
  }
  bd_perf_limits limits;
  char error[256] = "";
  CHECK(bd_perf_policy_resolve(policy, "performance", NULL, NULL, &limits,
                               error, sizeof(error)),
        "performance resolves for rollback: %s", error);

  bd_perf_plan plan;
  CHECK(bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error)),
        "rollback plan builds: %s", error);

  char directory[PATH_SIZE];
  CHECK(fixture_path(root, "sys", directory, sizeof(directory)),
        "can build a directory path");
  snprintf(system.min_path, sizeof(system.min_path), "%s", directory);

  bd_perf_apply_report report;
  CHECK(!bd_perf_apply(&system, &plan, &report, error, sizeof(error)),
        "write to a directory must fail");
  CHECK(report.rolled_back, "failed transaction reports a rollback");
  CHECK(report.writes >= 2, "failed transaction wrote before failing: %d",
        report.writes);

  char governor[64];
  char path[PATH_SIZE];
  if (fixture_path(root,
                   "sys/devices/system/cpu/cpufreq/policy0/scaling_governor",
                   path, sizeof(path)) &&
      read_text(path, governor, sizeof(governor))) {
    CHECK(strcmp(governor, "schedutil") == 0,
          "governor rolled back to schedutil, got %s", governor);
  }
  CHECK(fixture_int(root,
                    "sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq") ==
            408000,
        "min_freq rolled back to 408000");
  CHECK(fixture_int(root,
                    "sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq") ==
            1200000,
        "max_freq rolled back to 1200000");
}

static void test_online_and_swappiness(const char *root,
                                       const bd_perf_policy *policy) {
  bd_perf_system system;
  if (!probe_fixture(root, &system)) {
    return;
  }
  bd_perf_limits limits;
  char error[256] = "";
  CHECK(bd_perf_policy_resolve(policy, "powersave", NULL, NULL, &limits,
                               error, sizeof(error)),
        "powersave resolves: %s", error);
  limits.has_swappiness = true;
  limits.swappiness = 30;

  bd_perf_plan plan;
  CHECK(bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error)),
        "powersave plan builds: %s", error);
  CHECK(plan.max_khz == 816000, "powersave max is 816000, got %d",
        plan.max_khz);
  CHECK(plan.online_cores == 2, "powersave online cores is 2, got %d",
        plan.online_cores);
  CHECK(plan.swappiness_change && plan.swappiness == 30,
        "swappiness override is planned");
  CHECK(bd_perf_apply(&system, &plan, NULL, error, sizeof(error)),
        "powersave applies: %s", error);

  char governor[64];
  char path[PATH_SIZE];
  if (fixture_path(root,
                   "sys/devices/system/cpu/cpufreq/policy0/scaling_governor",
                   path, sizeof(path)) &&
      read_text(path, governor, sizeof(governor))) {
    CHECK(strcmp(governor, "powersave") == 0,
          "powersave governor is active, got %s", governor);
  }
  CHECK(fixture_int(root,
                    "sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq") ==
            816000,
        "powersave max_freq is active");
  CHECK(fixture_int(root, "sys/devices/system/cpu/cpu3/online") == 0,
        "cpu3 is offline under powersave");
  CHECK(fixture_int(root, "sys/devices/system/cpu/cpu2/online") == 0,
        "cpu2 is offline under powersave");
  CHECK(fixture_int(root, "sys/devices/system/cpu/cpu1/online") == 1,
        "cpu1 stays online under powersave");
  CHECK(fixture_int(root, "proc/sys/vm/swappiness") == 30,
        "swappiness override is active");

  if (!probe_fixture(root, &system)) {
    return;
  }
  CHECK(bd_perf_policy_resolve(policy, "default", NULL, NULL, &limits, error,
                               sizeof(error)),
        "default resolves: %s", error);
  CHECK(bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error)),
        "default plan builds: %s", error);
  CHECK(bd_perf_apply(&system, &plan, NULL, error, sizeof(error)),
        "default restores: %s", error);
  CHECK(fixture_int(root, "sys/devices/system/cpu/cpu3/online") == 1,
        "cpu3 is online again");
  CHECK(fixture_int(root, "sys/devices/system/cpu/cpu2/online") == 1,
        "cpu2 is online again");
  CHECK(fixture_int(root,
                    "sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq") ==
            1200000,
        "default max_freq is active");
  CHECK(fixture_int(root, "proc/sys/vm/swappiness") == 20,
        "default swappiness is restored");
}

static void apply_named(const char *root, const bd_perf_policy *policy,
                        const char *name) {
  bd_perf_system system;
  if (!probe_fixture(root, &system)) {
    return;
  }
  bd_perf_limits limits;
  char error[256] = "";
  if (!bd_perf_policy_resolve(policy, name, NULL, NULL, &limits, error,
                              sizeof(error))) {
    CHECK(false, "resolve %s: %s", name, error);
    return;
  }
  bd_perf_plan plan;
  if (!bd_perf_plan_build(&system, &limits, &plan, error, sizeof(error))) {
    CHECK(false, "plan %s: %s", name, error);
    return;
  }
  CHECK(bd_perf_apply(&system, &plan, NULL, error, sizeof(error)),
        "apply %s: %s", name, error);
}

static void test_snapshot(const char *root, const bd_perf_policy *policy) {
  bd_perf_system system;
  if (!probe_fixture(root, &system)) {
    return;
  }
  bd_perf_snapshot snapshot;
  CHECK(bd_perf_snapshot_take(&system, &snapshot), "snapshot is taken");
  apply_named(root, policy, "performance");
  if (!probe_fixture(root, &system)) {
    return;
  }
  char error[256] = "";
  CHECK(bd_perf_snapshot_restore(&system, &snapshot, error, sizeof(error)),
        "snapshot restores: %s", error);
  char governor[64];
  char path[PATH_SIZE];
  if (fixture_path(root,
                   "sys/devices/system/cpu/cpufreq/policy0/scaling_governor",
                   path, sizeof(path)) &&
      read_text(path, governor, sizeof(governor))) {
    CHECK(strcmp(governor, "schedutil") == 0,
          "snapshot restores governor, got %s", governor);
  }
  CHECK(fixture_int(root,
                    "sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq") ==
            408000,
        "snapshot restores min_freq");
  CHECK(fixture_int(root,
                    "sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq") ==
            1200000,
        "snapshot restores max_freq");
  CHECK(fixture_int(root, "sys/devices/system/cpu/cpu3/online") == 1,
        "snapshot restores online cores");
}

static void test_leases(const char *root, const bd_perf_policy *policy) {
  bd_perf_lease_table leases;
  bd_perf_leases_init(&leases);
  bd_perf_lease *lease =
      bd_perf_leases_apply(&leases, "s1", 1, "performance", NULL, NULL, 1000,
                           5000);
  CHECK(lease != NULL, "lease is created");
  CHECK(lease != NULL && lease->expires_ms == 6000, "expiry is now + ttl");
  const bd_perf_lease *effective = bd_perf_leases_effective(&leases);
  CHECK(effective != NULL && strcmp(effective->session, "s1") == 0,
        "new lease is effective");

  apply_named(root, policy, "performance");
  CHECK(bd_perf_leases_expire(&leases, 5999) == 0,
        "lease is alive before its deadline");
  CHECK(bd_perf_leases_effective(&leases) != NULL,
        "lease is still effective before its deadline");
  CHECK(fixture_int(root,
                    "sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq") ==
            1800000,
        "leased performance profile is active before expiry");

  CHECK(bd_perf_leases_expire(&leases, 6000) == 1,
        "lease expires at its deadline");
  CHECK(bd_perf_leases_effective(&leases) == NULL,
        "expired lease is no longer effective");
  apply_named(root, policy, "default");
  CHECK(fixture_int(root,
                    "sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq") ==
            1200000,
        "default is restored after lease expiry");

  bd_perf_leases_apply(&leases, "a", 10, "performance", NULL, NULL, 0, 1000);
  bd_perf_leases_apply(&leases, "b", 11, "powersave", NULL, NULL, 0, 1000);
  effective = bd_perf_leases_effective(&leases);
  CHECK(effective != NULL && strcmp(effective->session, "b") == 0,
        "newest lease wins");
  CHECK(bd_perf_leases_drop_owner(&leases, 11) == 1,
        "dropping the owner removes its lease");
  effective = bd_perf_leases_effective(&leases);
  CHECK(effective != NULL && strcmp(effective->session, "a") == 0,
        "next newest lease becomes effective");
  CHECK(bd_perf_leases_drop_owner(&leases, 10) == 1,
        "dropping the last owner removes the lease");
  CHECK(bd_perf_leases_effective(&leases) == NULL,
        "empty lease table falls back to global");

  bd_perf_leases_apply(&leases, "never", 12, "default", NULL, NULL, 0, 1000);
  CHECK(bd_perf_leases_expire(&leases, 9000) == 0,
        "a lease without ttl never expires");
  CHECK(bd_perf_leases_restore(&leases, "never"), "explicit restore removes it");
  CHECK(bd_perf_leases_effective(&leases) == NULL,
        "restored lease is no longer effective");
}

int main(void) {
  bd_perf_policy policy;
  char error[256] = "";
  CHECK(bd_perf_policy_load(&policy, BRICKDESK_PERF_FIXTURE_DIR "/perf", error,
                            sizeof(error)),
        "policy fixture loads: %s", error);
  CHECK(policy.profile_count == 8, "expected 8 profiles, got %zu",
        policy.profile_count);
  CHECK(policy.app_count == 3, "expected 3 apps, got %zu", policy.app_count);
  CHECK(policy.core_count == 3, "expected 3 cores, got %zu",
        policy.core_count);

  test_inherit(&policy);
  test_cycles(&policy);

  char *root = make_temp_copy();
  test_clamp(root);
  test_governor_whitelist(root);
  test_rollback(root, &policy);
  test_online_and_swappiness(root, &policy);
  test_snapshot(root, &policy);
  test_leases(root, &policy);
  remove_temp_copy(root);

  if (g_failures != 0) {
    fprintf(stderr, "perf_policy_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("perf_policy_test: all checks passed\n");
  return 0;
}
