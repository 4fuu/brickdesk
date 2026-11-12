#define _POSIX_C_SOURCE 200809L

#include "brickdesk/perf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brickdesk/config.h"

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
}

static void copy_text(char *dst, size_t dst_size, const char *src) {
  if (dst_size == 0) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  snprintf(dst, dst_size, "%s", src);
}

static const char *base_name(const char *path) {
  if (path == NULL) {
    return "";
  }
  const char *slash = strrchr(path, '/');
  return slash != NULL ? slash + 1 : path;
}

static bool key_has_prefix(const char *key, const char *prefix) {
  return key != NULL && strncmp(key, prefix, strlen(prefix)) == 0;
}

/* Returns the name between `prefix` and `suffix` in a dotted dynamic key. */
static bool dynamic_name(const char *key, const char *prefix,
                         const char *suffix, char *name, size_t name_size) {
  if (key == NULL || name == NULL || name_size == 0) {
    return false;
  }
  size_t key_length = strlen(key);
  size_t prefix_length = strlen(prefix);
  size_t suffix_length = strlen(suffix);
  if (key_length <= prefix_length + suffix_length) {
    return false;
  }
  if (strncmp(key, prefix, prefix_length) != 0 ||
      strcmp(key + key_length - suffix_length, suffix) != 0) {
    return false;
  }
  size_t length = key_length - prefix_length - suffix_length;
  if (length == 0 || length > BD_PERF_NAME_MAX || length >= name_size) {
    return false;
  }
  memcpy(name, key + prefix_length, length);
  name[length] = '\0';
  return true;
}

static bool config_get_int(const bd_config *config, const char *key,
                           int *out) {
  bd_config_value value;
  if (!bd_config_get(config, key, &value)) {
    return false;
  }
  if (value.type != BD_CONFIG_INT) {
    return false;
  }
  *out = (int)value.integer;
  return true;
}

static bool config_get_string(const bd_config *config, const char *key,
                              char *out, size_t out_size) {
  bd_config_value value;
  if (!bd_config_get(config, key, &value)) {
    return false;
  }
  if (value.type != BD_CONFIG_STRING && value.type != BD_CONFIG_ENUM) {
    return false;
  }
  copy_text(out, out_size, value.string);
  return true;
}

static bd_perf_profile *find_or_add_profile(bd_perf_policy *policy,
                                            const char *name, bool core) {
  bd_perf_profile *items = core ? policy->cores : policy->profiles;
  size_t *count = core ? &policy->core_count : &policy->profile_count;
  for (size_t i = 0; i < *count; ++i) {
    if (strcmp(items[i].name, name) == 0) {
      return &items[i];
    }
  }
  if (*count >= (core ? BD_PERF_MAX_CORES : BD_PERF_MAX_PROFILES)) {
    policy->overflow = true;
    return NULL;
  }
  bd_perf_profile *profile = &items[(*count)++];
  memset(profile, 0, sizeof(*profile));
  copy_text(profile->name, sizeof(profile->name), name);
  return profile;
}

static bd_perf_app *find_or_add_app(bd_perf_policy *policy, const char *name) {
  for (size_t i = 0; i < policy->app_count; ++i) {
    if (strcmp(policy->apps[i].name, name) == 0) {
      return &policy->apps[i];
    }
  }
  if (policy->app_count >= BD_PERF_MAX_APPS) {
    policy->overflow = true;
    return NULL;
  }
  bd_perf_app *app = &policy->apps[policy->app_count++];
  memset(app, 0, sizeof(*app));
  copy_text(app->name, sizeof(app->name), name);
  return app;
}

static bool is_profile_key(const char *key) {
  return key_has_prefix(key, "profile.") || key_has_prefix(key, "app.") ||
         key_has_prefix(key, "core.");
}

static bool check_diagnostics(const bd_config *config, char *error,
                              size_t error_size) {
  size_t count = bd_config_diagnostic_count(config);
  for (size_t i = 0; i < count; ++i) {
    bd_config_diagnostic diagnostic;
    if (!bd_config_get_diagnostic(config, i, &diagnostic)) {
      continue;
    }
    if (diagnostic.severity != BD_CONFIG_ERROR) {
      continue;
    }
    bool perf_file =
        strcmp(base_name(diagnostic.file), "perf.ini") == 0;
    if (!perf_file && !is_profile_key(diagnostic.key)) {
      continue;
    }
    set_error(error, error_size, "%s:%d: %s: %s",
              diagnostic.file != NULL ? diagnostic.file : "config",
              diagnostic.line,
              diagnostic.key != NULL ? diagnostic.key : "perf.ini",
              diagnostic.message != NULL ? diagnostic.message : "invalid value");
    return false;
  }
  return true;
}

static void read_profile_fields(bd_perf_policy *policy, bd_config *config,
                                const char *name, bool core) {
  bd_perf_profile *profile = find_or_add_profile(policy, name, core);
  if (profile == NULL) {
    return;
  }
  char field_key[256];
  const char *prefix = core ? "core." : "profile.";

  snprintf(field_key, sizeof(field_key), "%s%s.inherit", prefix, name);
  if (config_get_string(config, field_key, profile->inherit,
                        sizeof(profile->inherit))) {
    profile->has_inherit = profile->inherit[0] != '\0';
  }
  if (!profile->has_inherit) {
    profile->inherit[0] = '\0';
  }

  snprintf(field_key, sizeof(field_key), "%s%s.governor", prefix, name);
  if (config_get_string(config, field_key, profile->governor,
                        sizeof(profile->governor))) {
    profile->has_governor = profile->governor[0] != '\0';
  }

  snprintf(field_key, sizeof(field_key), "%s%s.min_khz", prefix, name);
  profile->has_min_khz = config_get_int(config, field_key, &profile->min_khz);
  snprintf(field_key, sizeof(field_key), "%s%s.max_khz", prefix, name);
  profile->has_max_khz = config_get_int(config, field_key, &profile->max_khz);
  snprintf(field_key, sizeof(field_key), "%s%s.online_cores", prefix, name);
  profile->has_online_cores =
      config_get_int(config, field_key, &profile->online_cores);
  snprintf(field_key, sizeof(field_key), "%s%s.swappiness", prefix, name);
  profile->has_swappiness =
      config_get_int(config, field_key, &profile->swappiness);
}

bool bd_perf_policy_load(bd_perf_policy *policy, const char *config_dir,
                         char *error, size_t error_size) {
  if (policy == NULL) {
    set_error(error, error_size, "policy is NULL");
    return false;
  }
  memset(policy, 0, sizeof(*policy));

  bd_config_options options;
  memset(&options, 0, sizeof(options));
  options.config_dir = config_dir;
  options.migrate = false;

  char config_error[256] = "";
  bd_config *config =
      bd_config_load(&options, config_error, sizeof(config_error));
  if (config == NULL) {
    set_error(error, error_size, "cannot load configuration: %s",
              config_error[0] != '\0' ? config_error : "out of memory");
    return false;
  }
  if (!check_diagnostics(config, error, error_size)) {
    bd_config_free(config);
    return false;
  }

  size_t key_count = bd_config_key_count(config);
  for (size_t i = 0; i < key_count; ++i) {
    const char *key = bd_config_key(config, i);
    if (key == NULL) {
      continue;
    }

    static const char *const profile_fields[] = {
        "inherit", "governor", "min_khz", "max_khz", "online_cores",
        "swappiness"};
    char name[BD_PERF_NAME_MAX + 1];
    bool matched = false;

    for (size_t f = 0;
         f < sizeof(profile_fields) / sizeof(profile_fields[0]); ++f) {
      char suffix[32];
      snprintf(suffix, sizeof(suffix), ".%s", profile_fields[f]);
      if (dynamic_name(key, "profile.", suffix, name, sizeof(name))) {
        read_profile_fields(policy, config, name, false);
        matched = true;
        break;
      }
      if (dynamic_name(key, "core.", suffix, name, sizeof(name))) {
        read_profile_fields(policy, config, name, true);
        matched = true;
        break;
      }
    }
    if (matched) {
      continue;
    }

    if (dynamic_name(key, "app.", ".profile", name, sizeof(name))) {
      bd_perf_app *app = find_or_add_app(policy, name);
      if (app != NULL) {
        char field_key[256];
        snprintf(field_key, sizeof(field_key), "app.%s.profile", name);
        config_get_string(config, field_key, app->profile,
                          sizeof(app->profile));
      }
    }
  }

  bd_config_free(config);

  if (policy->overflow) {
    set_error(error, error_size,
              "too many profile/app/core sections (limits: %d/%d/%d)",
              BD_PERF_MAX_PROFILES, BD_PERF_MAX_APPS, BD_PERF_MAX_CORES);
    return false;
  }
  if (policy->profile_count == 0) {
    set_error(error, error_size,
              "no [profile.<name>] sections found in perf.ini");
    return false;
  }
  return true;
}

const bd_perf_profile *bd_perf_policy_find_profile(const bd_perf_policy *policy,
                                                   const char *name) {
  if (policy == NULL || name == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < policy->profile_count; ++i) {
    if (strcmp(policy->profiles[i].name, name) == 0) {
      return &policy->profiles[i];
    }
  }
  return NULL;
}

const bd_perf_profile *bd_perf_policy_find_core(const bd_perf_policy *policy,
                                                const char *name) {
  if (policy == NULL || name == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < policy->core_count; ++i) {
    if (strcmp(policy->cores[i].name, name) == 0) {
      return &policy->cores[i];
    }
  }
  return NULL;
}

const bd_perf_app *bd_perf_policy_find_app(const bd_perf_policy *policy,
                                           const char *name) {
  if (policy == NULL || name == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < policy->app_count; ++i) {
    if (strcmp(policy->apps[i].name, name) == 0) {
      return &policy->apps[i];
    }
  }
  return NULL;
}

static void apply_profile_fields(bd_perf_limits *limits,
                                 const bd_perf_profile *profile) {
  if (profile->has_governor) {
    limits->has_governor = true;
    copy_text(limits->governor, sizeof(limits->governor), profile->governor);
  }
  if (profile->has_min_khz) {
    limits->has_min_khz = true;
    limits->min_khz = profile->min_khz;
  }
  if (profile->has_max_khz) {
    limits->has_max_khz = true;
    limits->max_khz = profile->max_khz;
  }
  if (profile->has_online_cores) {
    limits->has_online_cores = true;
    limits->online_cores = profile->online_cores;
  }
  if (profile->has_swappiness) {
    limits->has_swappiness = true;
    limits->swappiness = profile->swappiness;
  }
}

static bool resolve_profile_chain(const bd_perf_policy *policy, const char *name,
                                  bd_perf_limits *limits, char *error,
                                  size_t error_size) {
  const bd_perf_profile *chain[BD_PERF_MAX_PROFILES];
  size_t count = 0;
  const char *cursor = name;

  while (cursor != NULL && *cursor != '\0') {
    if (count >= BD_PERF_MAX_PROFILES) {
      set_error(error, error_size, "profile inherit chain is too long");
      return false;
    }
    const bd_perf_profile *profile =
        bd_perf_policy_find_profile(policy, cursor);
    if (profile == NULL) {
      set_error(error, error_size, "unknown profile '%s'", cursor);
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      if (strcmp(chain[i]->name, profile->name) == 0) {
        set_error(error, error_size,
                  "inheritance cycle at profile '%s'", profile->name);
        return false;
      }
    }
    chain[count++] = profile;
    cursor = profile->has_inherit ? profile->inherit : NULL;
  }

  for (size_t i = count; i > 0; --i) {
    apply_profile_fields(limits, chain[i - 1]);
  }
  return true;
}

bool bd_perf_policy_resolve(const bd_perf_policy *policy, const char *profile,
                            const char *app, const char *core,
                            bd_perf_limits *limits, char *error,
                            size_t error_size) {
  if (policy == NULL || limits == NULL) {
    set_error(error, error_size, "policy is NULL");
    return false;
  }
  memset(limits, 0, sizeof(*limits));

  const char *base_name_value = "default";
  if (profile != NULL && *profile != '\0') {
    base_name_value = profile;
  } else if (app != NULL && *app != '\0') {
    const bd_perf_app *mapping = bd_perf_policy_find_app(policy, app);
    if (mapping != NULL && mapping->profile[0] != '\0') {
      base_name_value = mapping->profile;
    }
  }

  if (!resolve_profile_chain(policy, base_name_value, limits, error,
                             error_size)) {
    return false;
  }
  copy_text(limits->profile, sizeof(limits->profile), base_name_value);
  if (app != NULL) {
    copy_text(limits->app, sizeof(limits->app), app);
  }

  if (core != NULL && *core != '\0') {
    const bd_perf_profile *core_profile =
        bd_perf_policy_find_core(policy, core);
    if (core_profile == NULL) {
      set_error(error, error_size, "unknown core '%s'", core);
      return false;
    }
    if (core_profile->has_inherit) {
      bd_perf_limits core_base;
      memset(&core_base, 0, sizeof(core_base));
      if (!resolve_profile_chain(policy, core_profile->inherit, &core_base,
                                 error, error_size)) {
        return false;
      }
      *limits = core_base;
      copy_text(limits->profile, sizeof(limits->profile),
                core_profile->inherit);
      if (app != NULL) {
        copy_text(limits->app, sizeof(limits->app), app);
      }
    }
    copy_text(limits->core, sizeof(limits->core), core_profile->name);
    apply_profile_fields(limits, core_profile);
  }
  return true;
}

void bd_perf_leases_init(bd_perf_lease_table *table) {
  if (table != NULL) {
    memset(table, 0, sizeof(*table));
  }
}

static void lease_compact(bd_perf_lease_table *table) {
  size_t write_index = 0;
  for (size_t i = 0; i < BD_PERF_MAX_LEASES; ++i) {
    if (!table->items[i].active) {
      continue;
    }
    if (write_index != i) {
      table->items[write_index] = table->items[i];
    }
    ++write_index;
  }
  for (size_t i = write_index; i < BD_PERF_MAX_LEASES; ++i) {
    memset(&table->items[i], 0, sizeof(table->items[i]));
  }
}

bd_perf_lease *bd_perf_leases_find(bd_perf_lease_table *table,
                                   const char *session) {
  if (table == NULL || session == NULL || *session == '\0') {
    return NULL;
  }
  for (size_t i = 0; i < BD_PERF_MAX_LEASES; ++i) {
    if (table->items[i].active &&
        strcmp(table->items[i].session, session) == 0) {
      return &table->items[i];
    }
  }
  return NULL;
}

bd_perf_lease *bd_perf_leases_apply(bd_perf_lease_table *table,
                                    const char *session, int owner,
                                    const char *profile, const char *app,
                                    const char *core, uint64_t ttl_ms,
                                    uint64_t now_ms) {
  if (table == NULL || session == NULL || *session == '\0' ||
      strlen(session) > BD_PERF_SESSION_MAX) {
    return NULL;
  }

  bd_perf_lease *lease = bd_perf_leases_find(table, session);
  if (lease == NULL) {
    for (size_t i = 0; i < BD_PERF_MAX_LEASES; ++i) {
      if (!table->items[i].active) {
        lease = &table->items[i];
        memset(lease, 0, sizeof(*lease));
        break;
      }
    }
  }
  if (lease == NULL) {
    return NULL;
  }

  lease->active = true;
  lease->owner = owner;
  lease->order = ++table->next_order;
  copy_text(lease->session, sizeof(lease->session), session);
  copy_text(lease->profile, sizeof(lease->profile), profile);
  copy_text(lease->app, sizeof(lease->app), app);
  copy_text(lease->core, sizeof(lease->core), core);
  if (ttl_ms == 0) {
    lease->expires_ms = 0;
  } else if (ttl_ms > UINT64_MAX - now_ms) {
    lease->expires_ms = UINT64_MAX;
  } else {
    lease->expires_ms = now_ms + ttl_ms;
  }
  return lease;
}

bool bd_perf_leases_restore(bd_perf_lease_table *table, const char *session) {
  bd_perf_lease *lease = bd_perf_leases_find(table, session);
  if (lease == NULL) {
    return false;
  }
  lease->active = false;
  lease_compact(table);
  return true;
}

size_t bd_perf_leases_drop_owner(bd_perf_lease_table *table, int owner) {
  if (table == NULL) {
    return 0;
  }
  size_t removed = 0;
  for (size_t i = 0; i < BD_PERF_MAX_LEASES; ++i) {
    if (table->items[i].active && table->items[i].owner == owner) {
      table->items[i].active = false;
      ++removed;
    }
  }
  if (removed > 0) {
    lease_compact(table);
  }
  return removed;
}

size_t bd_perf_leases_expire(bd_perf_lease_table *table, uint64_t now_ms) {
  if (table == NULL) {
    return 0;
  }
  size_t removed = 0;
  for (size_t i = 0; i < BD_PERF_MAX_LEASES; ++i) {
    bd_perf_lease *lease = &table->items[i];
    if (lease->active && lease->expires_ms != 0 &&
        lease->expires_ms <= now_ms) {
      lease->active = false;
      ++removed;
    }
  }
  if (removed > 0) {
    lease_compact(table);
  }
  return removed;
}

bool bd_perf_leases_next_deadline(const bd_perf_lease_table *table,
                                  uint64_t *deadline_ms) {
  if (table == NULL || deadline_ms == NULL) {
    return false;
  }
  bool found = false;
  uint64_t deadline = 0;
  for (size_t i = 0; i < BD_PERF_MAX_LEASES; ++i) {
    const bd_perf_lease *lease = &table->items[i];
    if (!lease->active || lease->expires_ms == 0) {
      continue;
    }
    if (!found || lease->expires_ms < deadline) {
      deadline = lease->expires_ms;
      found = true;
    }
  }
  if (found) {
    *deadline_ms = deadline;
  }
  return found;
}

const bd_perf_lease *bd_perf_leases_effective(
    const bd_perf_lease_table *table) {
  if (table == NULL) {
    return NULL;
  }
  const bd_perf_lease *effective = NULL;
  for (size_t i = 0; i < BD_PERF_MAX_LEASES; ++i) {
    const bd_perf_lease *lease = &table->items[i];
    if (!lease->active) {
      continue;
    }
    if (effective == NULL || lease->order > effective->order) {
      effective = lease;
    }
  }
  return effective;
}
