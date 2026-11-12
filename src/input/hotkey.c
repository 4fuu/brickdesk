#define _POSIX_C_SOURCE 200809L

#include "brickdesk/input.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brickdesk/config.h"

#define KEY_BIT(key) (1u << (unsigned)(key))

/* ------------------------------------------------------------- utilities */

static void copy_text(char *dst, size_t dst_size, const char *src) {
  if (dst == NULL || dst_size == 0) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  snprintf(dst, dst_size, "%s", src);
}

static bool starts_with(const char *text, const char *prefix) {
  return strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool ends_with(const char *text, const char *suffix) {
  size_t text_length = strlen(text);
  size_t suffix_length = strlen(suffix);
  return text_length >= suffix_length &&
         strcmp(text + text_length - suffix_length, suffix) == 0;
}

static char *trim_ws(char *text) {
  while (*text != '\0' && isspace((unsigned char)*text)) {
    ++text;
  }
  size_t length = strlen(text);
  while (length > 0 && isspace((unsigned char)text[length - 1])) {
    text[--length] = '\0';
  }
  return text;
}

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
}

/* --------------------------------------------------------------- parsing */

static bool parse_modifier(char *text, int *hold_ms, bool *double_tap,
                           char *error, size_t error_size) {
  char *open = strchr(text, '(');
  if (open == NULL) {
    return true;
  }
  *open = '\0';
  char *close = strchr(open + 1, ')');
  if (close == NULL) {
    set_error(error, error_size, "missing ')' in combo modifier");
    return false;
  }
  if (close[1] != '\0') {
    set_error(error, error_size, "unexpected text after combo modifier");
    return false;
  }
  *close = '\0';
  char *modifier = trim_ws(open + 1);
  if (starts_with(modifier, "hold:")) {
    char *end = NULL;
    long value = strtol(modifier + 5, &end, 10);
    if (end == modifier + 5 || *end != '\0' || value < 1 || value > 10000) {
      set_error(error, error_size, "hold modifier must be hold:<1..10000>");
      return false;
    }
    if (*double_tap) {
      set_error(error, error_size, "only one modifier per key is allowed");
      return false;
    }
    *hold_ms = (int)value;
    return true;
  }
  if (strcmp(modifier, "double") == 0) {
    if (*hold_ms != 0) {
      set_error(error, error_size, "only one modifier per key is allowed");
      return false;
    }
    *double_tap = true;
    return true;
  }
  set_error(error, error_size,
            "unknown combo modifier '%s' (use hold:<ms> or double)", modifier);
  return false;
}

static bool parse_step(char *text, bd_hotkey_step *step, char *error,
                       size_t error_size) {
  memset(step, 0, sizeof(*step));
  char *token = text;
  while (token != NULL) {
    char *plus = strchr(token, '+');
    if (plus != NULL) {
      *plus = '\0';
    }
    token = trim_ws(token);
    if (*token == '\0') {
      set_error(error, error_size, "empty key in combo");
      return false;
    }

    int hold_ms = 0;
    bool double_tap = false;
    if (!parse_modifier(token, &hold_ms, &double_tap, error, error_size)) {
      return false;
    }
    token = trim_ws(token);

    if (strcmp(token, "*") == 0) {
      set_error(error, error_size,
                "wildcard '*' is diagnostic only and cannot be bound");
      return false;
    }

    bd_key key = bd_key_from_name(token);
    if (key == BD_KEY_UNKNOWN) {
      set_error(error, error_size, "unknown key '%s'", token);
      return false;
    }
    if (step->key_count >= BD_HOTKEY_MAX_KEYS) {
      set_error(error, error_size, "too many keys in one chord");
      return false;
    }
    for (int i = 0; i < step->key_count; ++i) {
      if (step->keys[i] == key) {
        set_error(error, error_size, "duplicate key '%s' in chord",
                  bd_key_name(key));
        return false;
      }
    }
    step->keys[step->key_count++] = key;
    step->key_mask |= KEY_BIT(key);

    if (hold_ms != 0) {
      if (step->hold_ms != 0) {
        set_error(error, error_size, "only one hold modifier per chord");
        return false;
      }
      step->hold_ms = hold_ms;
    }
    if (double_tap) {
      if (step->double_tap) {
        set_error(error, error_size, "only one double modifier per chord");
        return false;
      }
      step->double_tap = true;
    }
    token = plus != NULL ? plus + 1 : NULL;
  }

  if (step->key_count == 0) {
    set_error(error, error_size, "empty key in combo");
    return false;
  }
  if ((step->hold_ms != 0 || step->double_tap) && step->key_count != 1) {
    set_error(error, error_size,
              "a combo modifier applies to a single key, not a chord");
    return false;
  }
  return true;
}

bool bd_hotkey_parse(bd_hotkey *out, const char *combo, char *error,
                     size_t error_size) {
  if (out == NULL || combo == NULL) {
    set_error(error, error_size, "invalid combo arguments");
    return false;
  }
  memset(out, 0, sizeof(*out));
  copy_text(out->combo, sizeof(out->combo), combo);
  copy_text(out->scope, sizeof(out->scope), "global");
  out->window_ms = 50;
  out->sequence_ms = 800;
  out->repeat_ms = 100;

  if (*combo == '\0') {
    set_error(error, error_size, "empty combo");
    return false;
  }
  if (strlen(combo) >= BD_HOTKEY_COMBO_MAX) {
    set_error(error, error_size, "combo is too long");
    return false;
  }

  char work[BD_HOTKEY_COMBO_MAX];
  copy_text(work, sizeof(work), combo);

  char *step_text = work;
  while (step_text != NULL) {
    char *comma = strchr(step_text, ',');
    if (comma != NULL) {
      *comma = '\0';
    }
    step_text = trim_ws(step_text);
    if (*step_text == '\0') {
      set_error(error, error_size, "empty step in combo");
      return false;
    }
    if (out->step_count >= BD_HOTKEY_MAX_STEPS) {
      set_error(error, error_size, "too many steps in combo");
      return false;
    }
    bd_hotkey_step *step = &out->steps[out->step_count];
    if (!parse_step(step_text, step, error, error_size)) {
      return false;
    }
    ++out->step_count;
    step_text = comma != NULL ? comma + 1 : NULL;
  }

  for (int i = 0; i < out->step_count; ++i) {
    if (out->steps[i].hold_ms != 0 || out->steps[i].double_tap) {
      if (out->step_count != 1) {
        set_error(error, error_size,
                  "hold/double modifier applies to a single-step binding");
        return false;
      }
      if (out->steps[i].hold_ms != 0) {
        out->hold_ms = out->steps[i].hold_ms;
      }
    }
  }
  return true;
}

/* ---------------------------------------------------------------- loading */

static void diagnostic_add(bd_hotkey_table *table, bool error,
                           const char *binding, const char *file, int line,
                           const char *format, ...) {
  if (table->diagnostic_count >= BD_HOTKEY_DIAGNOSTIC_MAX) {
    return;
  }
  bd_hotkey_diagnostic *diag = &table->diagnostics[table->diagnostic_count++];
  memset(diag, 0, sizeof(*diag));
  diag->error = error;
  copy_text(diag->binding, sizeof(diag->binding), binding);
  copy_text(diag->file, sizeof(diag->file), file);
  diag->line = line;
  va_list args;
  va_start(args, format);
  vsnprintf(diag->message, sizeof(diag->message), format, args);
  va_end(args);
}

static void conflict_add(bd_hotkey_table *table, bd_hotkey_conflict_kind kind,
                         int first, int second, const char *format, ...) {
  if (table->conflict_count >= BD_HOTKEY_CONFLICT_MAX) {
    return;
  }
  bd_hotkey_conflict *conflict = &table->conflicts[table->conflict_count++];
  memset(conflict, 0, sizeof(*conflict));
  conflict->kind = kind;
  conflict->first = first;
  conflict->second = second;
  va_list args;
  va_start(args, format);
  vsnprintf(conflict->message, sizeof(conflict->message), format, args);
  va_end(args);
}

static bool combo_equal(const bd_hotkey *a, const bd_hotkey *b) {
  if (a->step_count != b->step_count || a->hold_ms != b->hold_ms) {
    return false;
  }
  for (int i = 0; i < a->step_count; ++i) {
    if (a->steps[i].key_mask != b->steps[i].key_mask) {
      return false;
    }
    if (a->steps[i].double_tap != b->steps[i].double_tap) {
      return false;
    }
  }
  return true;
}

static bool scope_overlaps(const char *a, const char *b) {
  return strcmp(a, b) == 0 || strcmp(a, "global") == 0 || strcmp(b, "global") == 0;
}

static bool steps_prefix(const bd_hotkey *a, const bd_hotkey *b) {
  if (a->step_count > b->step_count) {
    return false;
  }
  for (int i = 0; i < a->step_count; ++i) {
    if (a->steps[i].key_mask != b->steps[i].key_mask ||
        a->steps[i].hold_ms != b->steps[i].hold_ms ||
        a->steps[i].double_tap != b->steps[i].double_tap) {
      return false;
    }
  }
  return true;
}

static bool first_step_subset(const bd_hotkey *a, const bd_hotkey *b) {
  uint32_t a_mask = a->steps[0].key_mask;
  uint32_t b_mask = b->steps[0].key_mask;
  return a_mask != 0 && b_mask != 0 &&
         ((a_mask & ~b_mask) == 0 || (b_mask & ~a_mask) == 0);
}

static bool is_wildcard_error(const char *message) {
  return strstr(message, "wildcard") != NULL;
}

static void describe_source(const bd_hotkey *hotkey, char *out, size_t out_size) {
  if (hotkey->source_file[0] != '\0' && hotkey->source_line > 0) {
    snprintf(out, out_size, "%s:%d", hotkey->source_file, hotkey->source_line);
  } else {
    copy_text(out, out_size, "(compiled default)");
  }
}

bool bd_hotkey_table_load(bd_hotkey_table *table,
                          const struct bd_config *config) {
  if (table == NULL || config == NULL) {
    return false;
  }
  memset(table, 0, sizeof(*table));

  size_t key_count = bd_config_key_count(config);
  for (size_t i = 0; i < key_count; ++i) {
    const char *key = bd_config_key(config, i);
    if (key == NULL || !starts_with(key, "hotkey.") || !ends_with(key, ".combo")) {
      continue;
    }

    const size_t prefix_length = strlen("hotkey.");
    const size_t suffix_length = strlen(".combo");
    size_t key_length = strlen(key);
    if (key_length <= prefix_length + suffix_length) {
      continue;
    }
    size_t id_length = key_length - prefix_length - suffix_length;
    if (id_length == 0 || id_length >= BD_HOTKEY_ID_MAX) {
      diagnostic_add(table, true, "", "", 0, "hotkey id is too long");
      continue;
    }
    char id[BD_HOTKEY_ID_MAX];
    memcpy(id, key + prefix_length, id_length);
    id[id_length] = '\0';

    char field[BD_HOTKEY_ID_MAX + 32];
    const char *combo = bd_config_get_string(config, key, NULL);
    if (combo == NULL) {
      continue;
    }

    int combo_line = 0;
    const char *combo_file = bd_config_source_file(config, key, &combo_line);
    bd_hotkey hotkey;
    char error[192];
    if (!bd_hotkey_parse(&hotkey, combo, error, sizeof(error))) {
      bool warning = is_wildcard_error(error);
      diagnostic_add(table, !warning, id,
                     combo_file != NULL ? combo_file : "", combo_line, "%s",
                     error);
      if (warning) {
        ++table->rejected;
      }
      continue;
    }
    copy_text(hotkey.id, sizeof(hotkey.id), id);

    snprintf(field, sizeof(field), "hotkey.%s.action", id);
    copy_text(hotkey.action, sizeof(hotkey.action),
              bd_config_get_string(config, field, ""));
    if (hotkey.action[0] == '\0') {
      diagnostic_add(table, true, id, "", 0, "missing action");
      continue;
    }

    snprintf(field, sizeof(field), "hotkey.%s.scope", id);
    copy_text(hotkey.scope, sizeof(hotkey.scope),
              bd_config_get_string(config, field, "global"));
    snprintf(field, sizeof(field), "hotkey.%s.plugin", id);
    copy_text(hotkey.plugin, sizeof(hotkey.plugin),
              bd_config_get_string(config, field, ""));
    snprintf(field, sizeof(field), "hotkey.%s.action_id", id);
    copy_text(hotkey.action_id, sizeof(hotkey.action_id),
              bd_config_get_string(config, field, ""));
    snprintf(field, sizeof(field), "hotkey.%s.window_ms", id);
    hotkey.window_ms = (int)bd_config_get_int(config, field, 50);
    snprintf(field, sizeof(field), "hotkey.%s.sequence_ms", id);
    hotkey.sequence_ms = (int)bd_config_get_int(config, field, 800);
    snprintf(field, sizeof(field), "hotkey.%s.repeat_ms", id);
    hotkey.repeat_ms = (int)bd_config_get_int(config, field, 100);
    snprintf(field, sizeof(field), "hotkey.%s.suppress", id);
    hotkey.suppress = bd_config_get_bool(config, field, false);
    snprintf(field, sizeof(field), "hotkey.%s.confirm", id);
    hotkey.confirm = bd_config_get_bool(config, field, false);
    snprintf(field, sizeof(field), "hotkey.%s.repeat", id);
    hotkey.repeat = bd_config_get_bool(config, field, false);

    int hold_line = 0;
    const char *hold_file = NULL;
    snprintf(field, sizeof(field), "hotkey.%s.hold_ms", id);
    hold_file = bd_config_source_file(config, field, &hold_line);
    bool explicit_hold = hold_file != NULL;
    int configured_hold = (int)bd_config_get_int(config, field, 0);

    if (hotkey.steps[0].hold_ms != 0) {
      if (explicit_hold) {
        diagnostic_add(table, false, id, hold_file != NULL ? hold_file : "",
                       hold_line,
                       "hold_ms ignored: combo uses (hold:%d)",
                       hotkey.steps[0].hold_ms);
      }
      hotkey.hold_ms = hotkey.steps[0].hold_ms;
    } else if (explicit_hold) {
      hotkey.hold_ms = configured_hold;
    }
    if (hotkey.repeat && hotkey.hold_ms == 0) {
      hotkey.hold_ms = 500;
    }
    if (hotkey.steps[0].double_tap) {
      hotkey.hold_ms = 0;
    }

    if (hotkey.hold_ms > 0 &&
        (hotkey.step_count != 1 || hotkey.steps[0].key_count != 1)) {
      diagnostic_add(table, true, id, "", 0,
                     "hold bindings must be a single key (got '%s')",
                     hotkey.combo);
      continue;
    }
    if (hotkey.steps[0].double_tap &&
        (hotkey.step_count != 1 || hotkey.steps[0].key_count != 1)) {
      diagnostic_add(table, true, id, "", 0,
                     "double-tap bindings must be a single key (got '%s')",
                     hotkey.combo);
      continue;
    }
    if (hotkey.steps[0].double_tap && hotkey.hold_ms > 0) {
      diagnostic_add(table, true, id, "", 0,
                     "a binding cannot be both hold and double");
      continue;
    }
    if (hotkey.repeat && hotkey.steps[0].key_count != 1) {
      diagnostic_add(table, true, id, "", 0,
                     "repeat is only supported on single-key bindings");
      continue;
    }
    if (hotkey.suppress) {
      diagnostic_add(table, false, id, "", 0,
                     "suppress=true is accepted but exclusive grab+uinput "
                     "forwarding is not implemented in M1-05");
    }

    copy_text(hotkey.source_file, sizeof(hotkey.source_file),
              combo_file != NULL ? combo_file : "");
    hotkey.source_line = combo_line;

    bool duplicate = false;
    for (size_t existing = 0; existing < table->count; ++existing) {
      const bd_hotkey *other = &table->items[existing];
      if (scope_overlaps(other->scope, hotkey.scope) &&
          combo_equal(other, &hotkey)) {
        char first_source[BD_HOTKEY_SOURCE_MAX + 32];
        char second_source[BD_HOTKEY_SOURCE_MAX + 32];
        describe_source(other, first_source, sizeof(first_source));
        describe_source(&hotkey, second_source, sizeof(second_source));
        conflict_add(table, BD_HOTKEY_CONFLICT_DUPLICATE, (int)existing, -1,
                     "duplicate combo '%s' in '%s' (%s) and '%s' (%s); "
                     "rejecting the later binding",
                     hotkey.combo, other->id, first_source, hotkey.id,
                     second_source);
        ++table->rejected;
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }

    if (table->count >= BD_HOTKEY_MAX) {
      diagnostic_add(table, true, id, "", 0, "too many hotkey bindings");
      ++table->rejected;
      continue;
    }
    table->items[table->count++] = hotkey;
  }

  for (size_t i = 0; i < table->count; ++i) {
    for (size_t j = i + 1; j < table->count; ++j) {
      const bd_hotkey *a = &table->items[i];
      const bd_hotkey *b = &table->items[j];
      if (!scope_overlaps(a->scope, b->scope) || combo_equal(a, b)) {
        continue;
      }
      char a_source[BD_HOTKEY_SOURCE_MAX + 32];
      char b_source[BD_HOTKEY_SOURCE_MAX + 32];
      describe_source(a, a_source, sizeof(a_source));
      describe_source(b, b_source, sizeof(b_source));

      if (steps_prefix(a, b) || steps_prefix(b, a) ||
          first_step_subset(a, b)) {
        conflict_add(table, BD_HOTKEY_CONFLICT_PREFIX, (int)i, (int)j,
                     "'%s' (%s) and '%s' (%s) share a prefix; the chord/hold "
                     "wins when it completes, otherwise the shorter binding "
                     "fires",
                     a->id, a_source, b->id, b_source);
      } else if ((a->steps[0].key_mask & b->steps[0].key_mask) != 0) {
        conflict_add(table, BD_HOTKEY_CONFLICT_OVERLAP, (int)i, (int)j,
                     "'%s' (%s) and '%s' (%s) share their first key; only one "
                     "should fire if both complete",
                     a->id, a_source, b->id, b_source);
      }
    }
  }
  return true;
}

const char *bd_hotkey_trigger_name(int trigger) {
  switch ((bd_input_trigger)trigger) {
    case BD_INPUT_TRIGGER_TAP:
      return "tap";
    case BD_INPUT_TRIGGER_CHORD:
      return "chord";
    case BD_INPUT_TRIGGER_HOLD:
      return "hold";
    case BD_INPUT_TRIGGER_REPEAT:
      return "repeat";
    case BD_INPUT_TRIGGER_DOUBLE:
      return "double";
    case BD_INPUT_TRIGGER_SEQUENCE:
      return "sequence";
    default:
      return "unknown";
  }
}

/* ---------------------------------------------------------- state machine */

static bool binding_active(const bd_input_state *state, size_t index) {
  return state->active[index];
}

static bool is_hold_binding(const bd_hotkey *hotkey) {
  return hotkey->hold_ms > 0;
}

static bool is_double_binding(const bd_hotkey *hotkey) {
  return hotkey->step_count == 1 && hotkey->steps[0].double_tap;
}

static bd_key hold_key(const bd_hotkey *hotkey) {
  return hotkey->steps[0].keys[0];
}

static void emit_key(bd_input_state *state, bd_key key, bool pressed,
                     uint64_t time_ms, bd_input_emit_fn emit, void *user) {
  (void)state;
  bd_input_output event;
  memset(&event, 0, sizeof(event));
  event.kind = BD_INPUT_OUTPUT_KEY;
  event.time_ms = time_ms;
  event.key = key;
  event.pressed = pressed;
  event.binding = -1;
  emit(user, &event);
}

static void emit_action(bd_input_state *state, int binding,
                        bd_input_trigger trigger, bool pressed,
                        uint64_t time_ms, bd_input_emit_fn emit, void *user) {
  (void)state;
  bd_input_output event;
  memset(&event, 0, sizeof(event));
  event.kind = BD_INPUT_OUTPUT_ACTION;
  event.time_ms = time_ms;
  event.key = BD_KEY_UNKNOWN;
  event.pressed = pressed;
  event.binding = binding;
  event.trigger = trigger;
  emit(user, &event);
}

static bool step_contains(const bd_hotkey_step *step, bd_key key) {
  return (step->key_mask & KEY_BIT(key)) != 0;
}

static uint32_t pressed_mask(const bd_input_state *state) {
  uint32_t mask = 0;
  for (int key = 1; key < (int)BD_KEY_COUNT; ++key) {
    if (state->pressed[key]) {
      mask |= KEY_BIT(key);
    }
  }
  return mask;
}

static void cancel_key_pending(bd_input_state *state, bd_key key,
                               int except_binding) {
  for (size_t i = 0; i < state->table->count; ++i) {
    if ((int)i == except_binding || !binding_active(state, i)) {
      continue;
    }
    bd_input_binding_state *runtime = &state->runtime[i];
    const bd_hotkey *hotkey = &state->table->items[i];
    if (runtime->tap_armed && runtime->tap_key == key) {
      runtime->tap_armed = false;
    }
    if (runtime->tap_pending && runtime->tap_key == key) {
      runtime->tap_pending = false;
    }
    if (runtime->hold_pending && is_hold_binding(hotkey) &&
        hold_key(hotkey) == key) {
      runtime->hold_pending = false;
    }
  }
}

static void reset_sequence(bd_input_binding_state *runtime);

static void abort_bindings_with_key(bd_input_state *state, bd_key key,
                                    int except_binding) {
  for (size_t i = 0; i < state->table->count; ++i) {
    if ((int)i == except_binding || !binding_active(state, i)) {
      continue;
    }
    bd_input_binding_state *runtime = &state->runtime[i];
    const bd_hotkey *hotkey = &state->table->items[i];
    if (runtime->step_active && runtime->step >= 0 &&
        runtime->step < hotkey->step_count &&
        step_contains(&hotkey->steps[runtime->step], key)) {
      reset_sequence(runtime);
      continue;
    }
    if (runtime->seq_waiting && runtime->step > 0 &&
        runtime->step <= hotkey->step_count &&
        step_contains(&hotkey->steps[runtime->step - 1], key)) {
      reset_sequence(runtime);
    }
  }
}

static void reset_binding(bd_input_binding_state *runtime) {
  memset(runtime, 0, sizeof(*runtime));
}

static void reset_sequence(bd_input_binding_state *runtime) {
  runtime->seq_waiting = false;
  runtime->seq_ready = false;
  runtime->seq_deadline_ms = 0;
  runtime->step = 0;
  runtime->step_active = false;
  runtime->matched = 0;
  runtime->fired_mask = 0;
}

static void consume_binding_keys(bd_input_state *state, size_t index) {
  const bd_hotkey *hotkey = &state->table->items[index];
  for (int step_index = 0; step_index < hotkey->step_count; ++step_index) {
    const bd_hotkey_step *step = &hotkey->steps[step_index];
    for (int i = 0; i < step->key_count; ++i) {
      state->consumed[step->keys[i]] = true;
      cancel_key_pending(state, step->keys[i], (int)index);
      abort_bindings_with_key(state, step->keys[i], (int)index);
    }
  }
}

static void complete_step(bd_input_state *state, size_t index, int step_index,
                          uint64_t now_ms, bd_input_emit_fn emit, void *user) {
  const bd_hotkey *hotkey = &state->table->items[index];
  bd_input_binding_state *runtime = &state->runtime[index];
  const bd_hotkey_step *step = &hotkey->steps[step_index];

  runtime->step_active = false;
  runtime->matched = 0;

  if (is_hold_binding(hotkey)) {
    runtime->step = 0;
    runtime->hold_pending = true;
    runtime->hold_deadline_ms = now_ms + (uint64_t)hotkey->hold_ms;
    return;
  }
  if (step->double_tap) {
    runtime->step = 0;
    return;
  }

  bool last = (step_index == hotkey->step_count - 1);
  if (!last) {
    runtime->seq_waiting = true;
    runtime->seq_ready = false;
    runtime->seq_deadline_ms = 0;
    runtime->fired_mask = step->key_mask;
    runtime->step = step_index + 1;
    /*
     * A recognised chord prefix consumes its keys.  A single-key step keeps
     * pending tap bindings alive: they are the fallback if the sequence never
     * completes, and `tap_prefix_delay_ms` already delayed them.
     */
    if (step->key_count > 1) {
      for (int i = 0; i < step->key_count; ++i) {
        state->consumed[step->keys[i]] = true;
        cancel_key_pending(state, step->keys[i], (int)index);
        abort_bindings_with_key(state, step->keys[i], (int)index);
      }
    }
    return;
  }

  if (hotkey->step_count == 1 && step->key_count == 1) {
    runtime->step = 0;
    runtime->tap_armed = true;
    runtime->tap_key = step->keys[0];
    return;
  }

  bd_input_trigger trigger = step->key_count > 1 ? BD_INPUT_TRIGGER_CHORD
                                                 : BD_INPUT_TRIGGER_SEQUENCE;
  emit_action(state, (int)index, trigger, true, now_ms, emit, user);
  consume_binding_keys(state, index);
  runtime->step = 0;
  runtime->seq_waiting = false;
  runtime->seq_ready = false;
  runtime->step_fired = true;
  runtime->fired_mask = step->key_mask;
}

static void handle_down(bd_input_state *state, bd_key key, uint64_t now_ms,
                        bd_input_emit_fn emit, void *user) {
  if (key <= BD_KEY_UNKNOWN || key >= BD_KEY_COUNT) {
    return;
  }
  if (state->pressed[key]) {
    return;
  }
  state->pressed[key] = true;
  state->down_ms[key] = now_ms;
  state->passthrough_down[key] = false;
  state->hold_fired[key] = false;
  state->consumed[key] = false;

  /* A second press inside the double-tap window wins over a pending tap. */
  for (size_t i = 0; i < state->table->count; ++i) {
    if (!binding_active(state, i)) {
      continue;
    }
    const bd_hotkey *hotkey = &state->table->items[i];
    if (!is_double_binding(hotkey) || hold_key(hotkey) != key) {
      continue;
    }
    bd_input_binding_state *runtime = &state->runtime[i];
    if (runtime->double_waiting && now_ms <= runtime->double_deadline_ms) {
      emit_action(state, (int)i, BD_INPUT_TRIGGER_DOUBLE, true, now_ms, emit,
                  user);
      runtime->double_waiting = false;
      runtime->double_fired = true;
      runtime->step_fired = true;
      runtime->fired_mask = KEY_BIT(key);
      state->consumed[key] = true;
      cancel_key_pending(state, key, (int)i);
      abort_bindings_with_key(state, key, (int)i);
    } else {
      runtime->double_waiting = false;
    }
  }

  for (size_t i = 0; i < state->table->count; ++i) {
    if (!binding_active(state, i)) {
      continue;
    }
    const bd_hotkey *hotkey = &state->table->items[i];
    bd_input_binding_state *runtime = &state->runtime[i];
    if (is_double_binding(hotkey) || runtime->step_fired ||
        state->consumed[key]) {
      continue;
    }
    if (runtime->tap_pending && runtime->tap_key == key) {
      runtime->tap_pending = false;
    }

    int step_index = 0;
    if (runtime->seq_waiting) {
      if (!runtime->seq_ready) {
        continue;
      }
      if (now_ms > runtime->seq_deadline_ms) {
        reset_sequence(runtime);
        continue;
      }
      step_index = runtime->step;
    } else if (runtime->step_active) {
      step_index = runtime->step;
    }
    if (step_index < 0 || step_index >= hotkey->step_count) {
      continue;
    }
    const bd_hotkey_step *step = &hotkey->steps[step_index];
    if (!step_contains(step, key)) {
      continue;
    }
    if (runtime->step_active && (runtime->matched & KEY_BIT(key)) != 0) {
      continue;
    }
    if (runtime->step_active &&
        now_ms > runtime->step_start_ms + (uint64_t)hotkey->window_ms) {
      reset_sequence(runtime);
      continue;
    }
    if (!runtime->step_active) {
      runtime->step_active = true;
      runtime->step = step_index;
      runtime->step_start_ms = now_ms;
      runtime->matched = KEY_BIT(key);
    } else {
      runtime->matched |= KEY_BIT(key);
    }
    if (runtime->matched != step->key_mask) {
      continue;
    }
    complete_step(state, i, step_index, now_ms, emit, user);
  }

  if (!state->reserved[key]) {
    emit_key(state, key, true, now_ms, emit, user);
    state->passthrough_down[key] = true;
  }
}

static void handle_up(bd_input_state *state, bd_key key, uint64_t now_ms,
                      bd_input_emit_fn emit, void *user) {
  if (key <= BD_KEY_UNKNOWN || key >= BD_KEY_COUNT) {
    return;
  }
  if (!state->pressed[key]) {
    return;
  }
  state->pressed[key] = false;
  uint32_t pressed = pressed_mask(state);

  for (size_t i = 0; i < state->table->count; ++i) {
    if (!binding_active(state, i)) {
      continue;
    }
    const bd_hotkey *hotkey = &state->table->items[i];
    bd_input_binding_state *runtime = &state->runtime[i];

    if (runtime->tap_armed && runtime->tap_key == key) {
      uint64_t delay = state->tap_prefix_delay_ms[key];
      if (state->has_double[key] &&
          (uint64_t)state->double_tap_ms > delay) {
        delay = (uint64_t)state->double_tap_ms;
      }
      runtime->tap_armed = false;
      runtime->tap_pending = true;
      runtime->tap_deadline_ms = now_ms + delay;
    }

    if (runtime->hold_pending && is_hold_binding(hotkey) &&
        hold_key(hotkey) == key) {
      runtime->hold_pending = false;
    }
    if (runtime->hold_active && is_hold_binding(hotkey) &&
        hold_key(hotkey) == key) {
      emit_action(state, (int)i, BD_INPUT_TRIGGER_HOLD, false, now_ms, emit,
                  user);
      runtime->hold_active = false;
      runtime->repeat_next_ms = 0;
    }

    if (is_double_binding(hotkey) && hold_key(hotkey) == key) {
      if (runtime->double_fired) {
        runtime->double_fired = false;
        runtime->step_fired = false;
        runtime->fired_mask = 0;
      } else if (!state->hold_fired[key]) {
        runtime->double_waiting = true;
        runtime->double_deadline_ms = now_ms + (uint64_t)state->double_tap_ms;
      }
    }

    if (runtime->seq_waiting && !runtime->seq_ready &&
        (pressed & runtime->fired_mask) == 0) {
      runtime->seq_ready = true;
      runtime->seq_deadline_ms = now_ms + (uint64_t)hotkey->sequence_ms;
    }

    if (runtime->step_fired && (pressed & runtime->fired_mask) == 0) {
      runtime->step_fired = false;
      runtime->fired_mask = 0;
    }

    if (runtime->step_active && runtime->step >= 0 &&
        runtime->step < hotkey->step_count &&
        step_contains(&hotkey->steps[runtime->step], key)) {
      reset_sequence(runtime);
    }
  }

  if (state->passthrough_down[key]) {
    emit_key(state, key, false, now_ms, emit, user);
    state->passthrough_down[key] = false;
  }
}

/* -------------------------------------------------------------- timers */

typedef enum timer_kind {
  TIMER_HOLD = 0,
  TIMER_REPEAT,
  TIMER_DOUBLE,
  TIMER_SEQUENCE,
  TIMER_TAP
} timer_kind;

static bool timer_better(uint64_t deadline, timer_kind kind, uint64_t best,
                         timer_kind best_kind) {
  if (deadline != best) {
    return deadline < best;
  }
  return (int)kind < (int)best_kind;
}

static bool find_due_timer(const bd_input_state *state, size_t index,
                           uint64_t now_ms, bool inclusive,
                           uint64_t *deadline_out, timer_kind *kind_out) {
  if (!binding_active(state, index)) {
    return false;
  }
  const bd_hotkey *hotkey = &state->table->items[index];
  const bd_input_binding_state *runtime = &state->runtime[index];
  bool any = false;
  uint64_t best = 0;
  timer_kind best_kind = TIMER_HOLD;

  if (runtime->hold_pending) {
    best = runtime->hold_deadline_ms;
    best_kind = TIMER_HOLD;
    any = true;
  }
  if (runtime->hold_active && hotkey->repeat) {
    if (!any || timer_better(runtime->repeat_next_ms, TIMER_REPEAT, best,
                             best_kind)) {
      best = runtime->repeat_next_ms;
      best_kind = TIMER_REPEAT;
      any = true;
    }
  }
  if (runtime->double_waiting) {
    if (!any || timer_better(runtime->double_deadline_ms, TIMER_DOUBLE, best,
                             best_kind)) {
      best = runtime->double_deadline_ms;
      best_kind = TIMER_DOUBLE;
      any = true;
    }
  }
  if (runtime->seq_waiting && runtime->seq_ready) {
    if (!any || timer_better(runtime->seq_deadline_ms, TIMER_SEQUENCE, best,
                             best_kind)) {
      best = runtime->seq_deadline_ms;
      best_kind = TIMER_SEQUENCE;
      any = true;
    }
  }
  if (runtime->tap_pending) {
    if (!any || timer_better(runtime->tap_deadline_ms, TIMER_TAP, best,
                             best_kind)) {
      best = runtime->tap_deadline_ms;
      best_kind = TIMER_TAP;
      any = true;
    }
  }
  if (!any) {
    return false;
  }
  if (inclusive ? best > now_ms : best >= now_ms) {
    return false;
  }
  *deadline_out = best;
  *kind_out = best_kind;
  return true;
}

static void process_timer(bd_input_state *state, size_t index,
                          timer_kind kind, uint64_t deadline,
                          bd_input_emit_fn emit, void *user) {
  const bd_hotkey *hotkey = &state->table->items[index];
  bd_input_binding_state *runtime = &state->runtime[index];

  switch (kind) {
    case TIMER_HOLD: {
      bd_key key = hold_key(hotkey);
      emit_action(state, (int)index, BD_INPUT_TRIGGER_HOLD, true, deadline, emit,
                  user);
      runtime->hold_pending = false;
      runtime->hold_active = true;
      state->hold_fired[key] = true;
      state->consumed[key] = true;
      abort_bindings_with_key(state, key, (int)index);
      if (hotkey->repeat) {
        int repeat_ms = hotkey->repeat_ms > 0 ? hotkey->repeat_ms : 100;
        runtime->repeat_next_ms = deadline + (uint64_t)repeat_ms;
      }
      cancel_key_pending(state, key, (int)index);
      break;
    }
    case TIMER_REPEAT: {
      int repeat_ms = hotkey->repeat_ms > 0 ? hotkey->repeat_ms : 100;
      emit_action(state, (int)index, BD_INPUT_TRIGGER_REPEAT, true, deadline,
                  emit, user);
      runtime->repeat_next_ms = deadline + (uint64_t)repeat_ms;
      break;
    }
    case TIMER_DOUBLE:
      runtime->double_waiting = false;
      break;
    case TIMER_SEQUENCE:
      reset_sequence(runtime);
      break;
    case TIMER_TAP:
      runtime->tap_pending = false;
      emit_action(state, (int)index, BD_INPUT_TRIGGER_TAP, true, deadline, emit,
                  user);
      break;
    default:
      break;
  }
}

static void advance_internal(bd_input_state *state, uint64_t now_ms,
                             bool inclusive, bd_input_emit_fn emit, void *user) {
  for (;;) {
    bool found = false;
    uint64_t best = 0;
    timer_kind best_kind = TIMER_HOLD;
    int best_index = -1;
    for (size_t i = 0; i < state->table->count; ++i) {
      uint64_t deadline = 0;
      timer_kind kind = TIMER_HOLD;
      if (!find_due_timer(state, i, now_ms, inclusive, &deadline, &kind)) {
        continue;
      }
      if (best_index < 0 || timer_better(deadline, kind, best, best_kind)) {
        found = true;
        best = deadline;
        best_kind = kind;
        best_index = (int)i;
      }
    }
    if (!found) {
      break;
    }
    process_timer(state, (size_t)best_index, best_kind, best, emit, user);
    state->last_time_ms = best;
    state->has_time = true;
  }
  if (!state->has_time || now_ms >= state->last_time_ms) {
    state->last_time_ms = now_ms;
    state->has_time = true;
  }
}

void bd_input_state_init(bd_input_state *state,
                         const bd_hotkey_table *table, const char *scope,
                         int double_tap_ms) {
  if (state == NULL) {
    return;
  }
  memset(state, 0, sizeof(*state));
  state->table = table;
  copy_text(state->scope, sizeof(state->scope),
            scope != NULL && *scope != '\0' ? scope : "global");
  state->double_tap_ms = double_tap_ms > 0 ? double_tap_ms : 300;
  if (table == NULL) {
    return;
  }

  for (size_t i = 0; i < table->count; ++i) {
    const bd_hotkey *hotkey = &table->items[i];
    bool active = strcmp(hotkey->scope, "global") == 0 ||
                  strcmp(hotkey->scope, state->scope) == 0;
    state->active[i] = active;
    if (!active) {
      continue;
    }
    bool first_single = hotkey->steps[0].key_count == 1;
    if (hotkey->step_count == 1 && first_single && hotkey->hold_ms == 0 &&
        !hotkey->steps[0].double_tap) {
      state->reserved[hotkey->steps[0].keys[0]] = true;
    } else if (hotkey->step_count > 1 && first_single &&
               !hotkey->steps[0].double_tap) {
      state->reserved[hotkey->steps[0].keys[0]] = true;
      if ((uint64_t)hotkey->sequence_ms >
          state->tap_prefix_delay_ms[hotkey->steps[0].keys[0]]) {
        state->tap_prefix_delay_ms[hotkey->steps[0].keys[0]] =
            (uint64_t)hotkey->sequence_ms;
      }
    }
    if (hotkey->steps[0].double_tap) {
      bd_key key = hotkey->steps[0].keys[0];
      state->has_double[key] = true;
      state->reserved[key] = true;
    }
  }
}

void bd_input_state_feed(bd_input_state *state, bd_key key, bool pressed,
                         uint64_t time_ms, bd_input_emit_fn emit, void *user) {
  if (state == NULL || emit == NULL || key <= BD_KEY_UNKNOWN ||
      key >= BD_KEY_COUNT) {
    return;
  }
  uint64_t now_ms = time_ms;
  if (state->has_time && now_ms < state->last_time_ms) {
    now_ms = state->last_time_ms;
  }
  advance_internal(state, now_ms, !pressed, emit, user);
  if (pressed) {
    handle_down(state, key, now_ms, emit, user);
  } else {
    handle_up(state, key, now_ms, emit, user);
  }
  advance_internal(state, now_ms, true, emit, user);
}

void bd_input_state_advance(bd_input_state *state, uint64_t now_ms,
                            bd_input_emit_fn emit, void *user) {
  if (state == NULL || emit == NULL) {
    return;
  }
  if (state->has_time && now_ms < state->last_time_ms) {
    now_ms = state->last_time_ms;
  }
  advance_internal(state, now_ms, true, emit, user);
}

bool bd_input_state_next_deadline(const bd_input_state *state,
                                  uint64_t *out_ms) {
  if (state == NULL || out_ms == NULL || state->table == NULL) {
    return false;
  }
  bool found = false;
  uint64_t best = 0;
  for (size_t i = 0; i < state->table->count; ++i) {
    if (!binding_active(state, i)) {
      continue;
    }
    const bd_hotkey *hotkey = &state->table->items[i];
    const bd_input_binding_state *runtime = &state->runtime[i];
    uint64_t candidates[5];
    int count = 0;
    if (runtime->hold_pending) {
      candidates[count++] = runtime->hold_deadline_ms;
    }
    if (runtime->hold_active && hotkey->repeat) {
      candidates[count++] = runtime->repeat_next_ms;
    }
    if (runtime->double_waiting) {
      candidates[count++] = runtime->double_deadline_ms;
    }
    if (runtime->seq_waiting && runtime->seq_ready) {
      candidates[count++] = runtime->seq_deadline_ms;
    }
    if (runtime->tap_pending) {
      candidates[count++] = runtime->tap_deadline_ms;
    }
    for (int c = 0; c < count; ++c) {
      if (!found || candidates[c] < best) {
        best = candidates[c];
        found = true;
      }
    }
  }
  if (found) {
    *out_ms = best;
  }
  return found;
}

bool bd_input_state_any_pressed(const bd_input_state *state) {
  if (state == NULL) {
    return false;
  }
  for (int key = 1; key < (int)BD_KEY_COUNT; ++key) {
    if (state->pressed[key]) {
      return true;
    }
  }
  return false;
}

bool bd_input_state_is_pressed(const bd_input_state *state, bd_key key) {
  if (state == NULL || key <= BD_KEY_UNKNOWN || key >= BD_KEY_COUNT) {
    return false;
  }
  return state->pressed[key];
}

void bd_input_state_release_all(bd_input_state *state, uint64_t now_ms,
                                bd_input_emit_fn emit, void *user) {
  if (state == NULL) {
    return;
  }
  if (state->table != NULL) {
    for (size_t i = 0; i < state->table->count; ++i) {
      if (!binding_active(state, i)) {
        continue;
      }
      bd_input_binding_state *runtime = &state->runtime[i];
      if (runtime->hold_active && emit != NULL) {
        emit_action(state, (int)i, BD_INPUT_TRIGGER_HOLD, false, now_ms, emit,
                    user);
      }
      reset_binding(runtime);
    }
  }
  for (int key = 1; key < (int)BD_KEY_COUNT; ++key) {
    if (state->passthrough_down[key]) {
      if (emit != NULL) {
        emit_key(state, (bd_key)key, false, now_ms, emit, user);
      }
      state->passthrough_down[key] = false;
    }
    state->pressed[key] = false;
    state->down_ms[key] = 0;
    state->hold_fired[key] = false;
    state->consumed[key] = false;
  }
  state->last_time_ms = now_ms;
  state->has_time = true;
}
