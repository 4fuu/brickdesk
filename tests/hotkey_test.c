#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "brickdesk/config.h"
#include "brickdesk/input.h"

#define MAX_EVENTS 512

static int g_failures;

#define CHECK(condition, ...)                    \
  do {                                           \
    if (!(condition)) {                          \
      fprintf(stderr, "hotkey_test: ");          \
      fprintf(stderr, __VA_ARGS__);              \
      fprintf(stderr, "\n");                     \
      ++g_failures;                              \
    }                                            \
  } while (0)

typedef struct collector {
  bd_input_output events[MAX_EVENTS];
  size_t count;
} collector;

static void collect(void *user, const bd_input_output *event) {
  collector *sink = (collector *)user;
  if (sink->count < MAX_EVENTS) {
    sink->events[sink->count++] = *event;
  }
}

static void collector_reset(collector *sink) {
  sink->count = 0;
}

static size_t action_count(const collector *sink) {
  size_t count = 0;
  for (size_t i = 0; i < sink->count; ++i) {
    if (sink->events[i].kind == BD_INPUT_OUTPUT_ACTION) {
      ++count;
    }
  }
  return count;
}

static const bd_input_output *action_at(const collector *sink, size_t index) {
  size_t seen = 0;
  for (size_t i = 0; i < sink->count; ++i) {
    if (sink->events[i].kind != BD_INPUT_OUTPUT_ACTION) {
      continue;
    }
    if (seen == index) {
      return &sink->events[i];
    }
    ++seen;
  }
  return NULL;
}

static size_t key_count(const collector *sink) {
  size_t count = 0;
  for (size_t i = 0; i < sink->count; ++i) {
    if (sink->events[i].kind == BD_INPUT_OUTPUT_KEY) {
      ++count;
    }
  }
  return count;
}

static const bd_input_output *key_at(const collector *sink, size_t index) {
  size_t seen = 0;
  for (size_t i = 0; i < sink->count; ++i) {
    if (sink->events[i].kind != BD_INPUT_OUTPUT_KEY) {
      continue;
    }
    if (seen == index) {
      return &sink->events[i];
    }
    ++seen;
  }
  return NULL;
}

static bool write_file(const char *path, const char *content) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    fprintf(stderr, "hotkey_test: cannot write %s: %s\n", path,
            strerror(errno));
    return false;
  }
  bool ok = fputs(content, file) >= 0;
  if (fclose(file) != 0) {
    ok = false;
  }
  return ok;
}

static bool load_table(const char *hotkeys, bd_hotkey_table *table) {
  char directory[] = "/tmp/bd_hotkey_test.XXXXXX";
  char *created = mkdtemp(directory);
  if (created == NULL) {
    fprintf(stderr, "hotkey_test: mkdtemp failed: %s\n", strerror(errno));
    return false;
  }
  char path[512];
  snprintf(path, sizeof(path), "%s/hotkeys.ini", created);
  if (!write_file(path, hotkeys)) {
    rmdir(created);
    return false;
  }

  bd_config_options options;
  options.config_dir = created;
  options.migrate = false;
  char error[256] = {0};
  bd_config *config = bd_config_load(&options, error, sizeof(error));
  if (config == NULL) {
    fprintf(stderr, "hotkey_test: config: %s\n", error);
    unlink(path);
    rmdir(created);
    return false;
  }
  bool ok = bd_hotkey_table_load(table, config);
  bd_config_free(config);
  unlink(path);
  rmdir(created);
  return ok;
}

static void make_state(bd_input_state *state, const bd_hotkey_table *table,
                       const char *scope) {
  bd_input_state_init(state, table, scope, 300);
}

static void feed(bd_input_state *state, collector *sink, bd_key key,
                 bool pressed, uint64_t time_ms) {
  bd_input_state_feed(state, key, pressed, time_ms, collect, sink);
}

static void advance(bd_input_state *state, collector *sink, uint64_t time_ms) {
  bd_input_state_advance(state, time_ms, collect, sink);
}

static void test_single_tap(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.tap_a]\ncombo = A\naction = tap.a\n",
            &table),
        "single tap: table load");
  CHECK(table.count == 1, "single tap: expected 1 binding, got %zu",
        table.count);
  if (table.count != 1) {
    return;
  }
  bd_input_state state;
  make_state(&state, &table, "global");
  collector sink;
  collector_reset(&sink);

  feed(&state, &sink, BD_KEY_A, true, 0);
  CHECK(action_count(&sink) == 0, "single tap fired on press");
  feed(&state, &sink, BD_KEY_A, false, 20);
  CHECK(action_count(&sink) == 1, "single tap did not fire on release");
  const bd_input_output *event = action_at(&sink, 0);
  CHECK(event != NULL && event->trigger == BD_INPUT_TRIGGER_TAP,
        "single tap: wrong trigger");
  CHECK(event != NULL && event->time_ms == 20,
        "single tap: wrong timestamp");
}

static void test_chord(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.chord]\ncombo = L1+R1+START\nwindow_ms = 50\n"
            "action = chord.shot\n",
            &table),
        "chord: table load");
  if (table.count != 1) {
    CHECK(table.count == 1, "chord: expected 1 binding");
    return;
  }
  bd_input_state state;
  make_state(&state, &table, "global");
  collector sink;
  collector_reset(&sink);

  feed(&state, &sink, BD_KEY_L1, true, 0);
  feed(&state, &sink, BD_KEY_R1, true, 10);
  CHECK(action_count(&sink) == 0, "chord fired before complete");
  feed(&state, &sink, BD_KEY_START, true, 40);
  CHECK(action_count(&sink) == 1, "chord did not fire");
  const bd_input_output *event = action_at(&sink, 0);
  CHECK(event != NULL && event->trigger == BD_INPUT_TRIGGER_CHORD,
        "chord: wrong trigger");
  CHECK(event != NULL && event->time_ms == 40, "chord: wrong timestamp");

  feed(&state, &sink, BD_KEY_START, false, 50);
  feed(&state, &sink, BD_KEY_L1, false, 60);
  feed(&state, &sink, BD_KEY_R1, false, 70);
  CHECK(action_count(&sink) == 1, "chord repeated on release");

  feed(&state, &sink, BD_KEY_L1, true, 100);
  feed(&state, &sink, BD_KEY_R1, true, 110);
  feed(&state, &sink, BD_KEY_START, true, 120);
  CHECK(action_count(&sink) == 2, "chord did not re-arm after release");
}

static void test_chord_window_boundary(void) {
  const char *config =
      "[meta]\nschema_version = 1\n"
      "[hotkey.chord]\ncombo = L1+R1+START\nwindow_ms = 50\n"
      "action = chord.shot\n";

  bd_hotkey_table table;
  CHECK(load_table(config, &table), "chord boundary: table load");
  if (table.count != 1) {
    return;
  }

  bd_input_state state;
  collector sink;
  make_state(&state, &table, "global");
  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_L1, true, 0);
  feed(&state, &sink, BD_KEY_R1, true, 10);
  feed(&state, &sink, BD_KEY_START, true, 50);
  CHECK(action_count(&sink) == 1, "chord at exact window rejected");

  make_state(&state, &table, "global");
  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_L1, true, 0);
  feed(&state, &sink, BD_KEY_R1, true, 10);
  feed(&state, &sink, BD_KEY_START, true, 51);
  CHECK(action_count(&sink) == 0, "chord after window accepted");
}

static void test_hold_boundary(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.hold_b]\ncombo = B\nhold_ms = 500\n"
            "action = hold.b\n",
            &table),
        "hold: table load");
  if (table.count != 1) {
    return;
  }

  bd_input_state state;
  collector sink;
  make_state(&state, &table, "global");
  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_B, true, 0);
  advance(&state, &sink, 499);
  CHECK(action_count(&sink) == 0, "hold fired before threshold");
  advance(&state, &sink, 500);
  CHECK(action_count(&sink) == 1, "hold did not fire at threshold");
  const bd_input_output *down = action_at(&sink, 0);
  CHECK(down != NULL && down->trigger == BD_INPUT_TRIGGER_HOLD &&
            down->pressed,
        "hold: wrong down event");
  feed(&state, &sink, BD_KEY_B, false, 600);
  CHECK(action_count(&sink) == 2, "hold did not emit release phase");
  const bd_input_output *up = action_at(&sink, 1);
  CHECK(up != NULL && up->trigger == BD_INPUT_TRIGGER_HOLD && !up->pressed,
        "hold: wrong up event");

  make_state(&state, &table, "global");
  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_B, true, 0);
  feed(&state, &sink, BD_KEY_B, false, 499);
  CHECK(action_count(&sink) == 0, "short hold fired");

  make_state(&state, &table, "global");
  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_B, true, 0);
  feed(&state, &sink, BD_KEY_B, false, 500);
  CHECK(action_count(&sink) == 2,
        "release at exact hold threshold must fire down and up");
}

static void test_hint_passthrough(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.hint]\ncombo = SELECT\nhold_ms = 300\n"
            "action = ui.hint\n",
            &table),
        "hint: table load");
  if (table.count != 1) {
    return;
  }
  bd_input_state state;
  collector sink;
  make_state(&state, &table, "global");
  collector_reset(&sink);

  feed(&state, &sink, BD_KEY_SELECT, true, 0);
  CHECK(action_count(&sink) == 0, "hint fired on press");
  const bd_input_output *select_down = key_at(&sink, 0);
  CHECK(select_down != NULL && select_down->key == BD_KEY_SELECT &&
            select_down->pressed,
        "hint key did not pass through on press");

  advance(&state, &sink, 300);
  CHECK(action_count(&sink) == 1, "hint did not fire at threshold");
  const bd_input_output *hint = action_at(&sink, 0);
  CHECK(hint != NULL && hint->trigger == BD_INPUT_TRIGGER_HOLD &&
            hint->pressed,
        "hint: wrong trigger");

  feed(&state, &sink, BD_KEY_RIGHT, true, 400);
  feed(&state, &sink, BD_KEY_RIGHT, false, 450);
  CHECK(key_count(&sink) == 3, "hint blocked another key (got %zu key events)",
        key_count(&sink));

  feed(&state, &sink, BD_KEY_SELECT, false, 500);
  CHECK(action_count(&sink) == 2, "hint did not hide on release");
  const bd_input_output *hide = action_at(&sink, 1);
  CHECK(hide != NULL && hide->trigger == BD_INPUT_TRIGGER_HOLD &&
            !hide->pressed,
        "hint: wrong release phase");
}

static void test_double_tap(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.double_x]\ncombo = X(double)\naction = double.x\n",
            &table),
        "double: table load");
  if (table.count != 1) {
    return;
  }
  bd_input_state state;
  collector sink;
  make_state(&state, &table, "global");
  collector_reset(&sink);

  feed(&state, &sink, BD_KEY_X, true, 0);
  feed(&state, &sink, BD_KEY_X, false, 50);
  CHECK(action_count(&sink) == 0, "double fired after one tap");
  feed(&state, &sink, BD_KEY_X, true, 350);
  CHECK(action_count(&sink) == 1, "double did not fire at exact window");
  const bd_input_output *event = action_at(&sink, 0);
  CHECK(event != NULL && event->trigger == BD_INPUT_TRIGGER_DOUBLE,
        "double: wrong trigger");
  feed(&state, &sink, BD_KEY_X, false, 400);

  make_state(&state, &table, "global");
  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_X, true, 0);
  feed(&state, &sink, BD_KEY_X, false, 50);
  feed(&state, &sink, BD_KEY_X, true, 351);
  CHECK(action_count(&sink) == 0, "double after window accepted");
}

static void test_double_cancels_single(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.tap_x]\ncombo = X\naction = tap.x\n"
            "[hotkey.double_x]\ncombo = X(double)\naction = double.x\n",
            &table),
        "double+single: table load");
  if (table.count != 2) {
    CHECK(table.count == 2, "double+single: expected 2 bindings, got %zu",
          table.count);
    return;
  }
  bd_input_state state;
  collector sink;
  make_state(&state, &table, "global");
  collector_reset(&sink);

  feed(&state, &sink, BD_KEY_X, true, 0);
  feed(&state, &sink, BD_KEY_X, false, 50);
  feed(&state, &sink, BD_KEY_X, true, 100);
  CHECK(action_count(&sink) == 1, "double did not fire");
  feed(&state, &sink, BD_KEY_X, false, 150);
  advance(&state, &sink, 1000);
  CHECK(action_count(&sink) == 1, "single tap fired after double");
  const bd_input_output *event = action_at(&sink, 0);
  CHECK(event != NULL && event->trigger == BD_INPUT_TRIGGER_DOUBLE,
        "double+single: wrong trigger");
}

static void test_sequence(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.seq]\ncombo = SELECT,UP\nsequence_ms = 800\n"
            "action = seq.action\n",
            &table),
        "sequence: table load");
  if (table.count != 1) {
    return;
  }
  bd_input_state state;
  collector sink;
  make_state(&state, &table, "global");
  collector_reset(&sink);

  feed(&state, &sink, BD_KEY_SELECT, true, 0);
  feed(&state, &sink, BD_KEY_SELECT, false, 20);
  feed(&state, &sink, BD_KEY_UP, true, 820);
  CHECK(action_count(&sink) == 1, "sequence at exact deadline rejected");
  const bd_input_output *event = action_at(&sink, 0);
  CHECK(event != NULL && event->trigger == BD_INPUT_TRIGGER_SEQUENCE,
        "sequence: wrong trigger");
  feed(&state, &sink, BD_KEY_UP, false, 900);

  make_state(&state, &table, "global");
  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_SELECT, true, 0);
  feed(&state, &sink, BD_KEY_SELECT, false, 20);
  feed(&state, &sink, BD_KEY_UP, true, 821);
  CHECK(action_count(&sink) == 0, "sequence after deadline accepted");

  make_state(&state, &table, "global");
  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_SELECT, true, 0);
  feed(&state, &sink, BD_KEY_UP, true, 100);
  feed(&state, &sink, BD_KEY_SELECT, false, 200);
  feed(&state, &sink, BD_KEY_UP, false, 300);
  CHECK(action_count(&sink) == 0,
        "sequence accepted without releasing the first step");
}

static void test_sequence_cancels_tap(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.tap_a]\ncombo = A\naction = tap.a\n"
            "[hotkey.seq]\ncombo = A,B\nsequence_ms = 800\n"
            "action = seq.ab\n",
            &table),
        "sequence+tap: table load");
  if (table.count != 2) {
    CHECK(table.count == 2, "sequence+tap: expected 2 bindings, got %zu",
          table.count);
    return;
  }
  bd_input_state state;
  collector sink;
  make_state(&state, &table, "global");
  collector_reset(&sink);

  feed(&state, &sink, BD_KEY_A, true, 0);
  feed(&state, &sink, BD_KEY_A, false, 20);
  feed(&state, &sink, BD_KEY_B, true, 100);
  CHECK(action_count(&sink) == 1, "sequence did not fire");
  const bd_input_output *event = action_at(&sink, 0);
  CHECK(event != NULL && event->trigger == BD_INPUT_TRIGGER_SEQUENCE,
        "sequence+tap: wrong trigger");
  feed(&state, &sink, BD_KEY_B, false, 120);
  advance(&state, &sink, 1000);
  CHECK(action_count(&sink) == 1, "tap fired after sequence");

  make_state(&state, &table, "global");
  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_A, true, 0);
  feed(&state, &sink, BD_KEY_A, false, 20);
  advance(&state, &sink, 819);
  CHECK(action_count(&sink) == 0, "tap fired before sequence timeout");
  advance(&state, &sink, 820);
  CHECK(action_count(&sink) == 1, "tap did not fire after sequence timeout");
  event = action_at(&sink, 0);
  CHECK(event != NULL && event->trigger == BD_INPUT_TRIGGER_TAP,
        "sequence+tap fallback: wrong trigger");
}

static void test_prefix_menu(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.menu]\ncombo = Menu\nhold_ms = 600\naction = menu.open\n"
            "[hotkey.menu_x]\ncombo = Menu+X\naction = menu.x\n",
            &table),
        "prefix menu: table load");
  if (table.count != 2) {
    CHECK(table.count == 2, "prefix menu: expected 2 bindings, got %zu",
          table.count);
    return;
  }
  bd_input_state state;
  collector sink;
  make_state(&state, &table, "global");
  collector_reset(&sink);

  feed(&state, &sink, BD_KEY_MENU, true, 0);
  feed(&state, &sink, BD_KEY_X, true, 30);
  CHECK(action_count(&sink) == 1, "Menu+X chord did not fire");
  const bd_input_output *chord = action_at(&sink, 0);
  CHECK(chord != NULL && chord->trigger == BD_INPUT_TRIGGER_CHORD,
        "prefix menu: wrong chord trigger");
  advance(&state, &sink, 600);
  CHECK(action_count(&sink) == 1, "Menu hold fired after Menu+X chord");
  feed(&state, &sink, BD_KEY_X, false, 610);
  feed(&state, &sink, BD_KEY_MENU, false, 620);

  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_MENU, true, 1000);
  advance(&state, &sink, 1599);
  CHECK(action_count(&sink) == 0, "Menu hold fired before 600 ms");
  advance(&state, &sink, 1600);
  CHECK(action_count(&sink) == 1, "Menu hold did not fire alone");
  const bd_input_output *hold = action_at(&sink, 0);
  CHECK(hold != NULL && hold->trigger == BD_INPUT_TRIGGER_HOLD,
        "prefix menu: wrong hold trigger");
}

static void test_release_order(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.chord]\ncombo = L1+R1+START\nwindow_ms = 50\n"
            "action = chord.shot\n",
            &table),
        "release order: table load");
  if (table.count != 1) {
    return;
  }
  bd_input_state state;
  collector sink;
  make_state(&state, &table, "global");
  collector_reset(&sink);

  feed(&state, &sink, BD_KEY_L1, true, 0);
  feed(&state, &sink, BD_KEY_R1, true, 10);
  feed(&state, &sink, BD_KEY_START, true, 20);
  CHECK(action_count(&sink) == 1, "release order: chord did not fire");
  feed(&state, &sink, BD_KEY_R1, false, 30);
  feed(&state, &sink, BD_KEY_R1, true, 35);
  CHECK(action_count(&sink) == 1, "chord repeated while other keys were held");
  feed(&state, &sink, BD_KEY_START, false, 40);
  feed(&state, &sink, BD_KEY_R1, false, 50);
  feed(&state, &sink, BD_KEY_L1, false, 60);
  CHECK(action_count(&sink) == 1, "chord repeated on release");
  feed(&state, &sink, BD_KEY_L1, true, 100);
  feed(&state, &sink, BD_KEY_R1, true, 110);
  feed(&state, &sink, BD_KEY_START, true, 120);
  CHECK(action_count(&sink) == 2, "chord did not re-arm after release order");
}

static void test_out_of_order_and_duplicates(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.tap_a]\ncombo = A\naction = tap.a\n",
            &table),
        "out of order: table load");
  if (table.count != 1) {
    return;
  }
  bd_input_state state;
  collector sink;
  make_state(&state, &table, "global");
  collector_reset(&sink);

  feed(&state, &sink, BD_KEY_A, true, 100);
  feed(&state, &sink, BD_KEY_A, true, 100);
  feed(&state, &sink, BD_KEY_A, false, 50);
  feed(&state, &sink, BD_KEY_A, false, 50);
  CHECK(action_count(&sink) == 1, "duplicate/out-of-order events produced %zu actions",
        action_count(&sink));
  const bd_input_output *event = action_at(&sink, 0);
  CHECK(event != NULL && event->time_ms == 100,
        "out-of-order tap timestamp was not clamped");
}

static void test_repeat(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.vol]\ncombo = VOLUME_UP\nhold_ms = 400\n"
            "repeat = true\nrepeat_ms = 100\naction = vol.up\n",
            &table),
        "repeat: table load");
  if (table.count != 1) {
    return;
  }
  bd_input_state state;
  collector sink;
  make_state(&state, &table, "global");
  collector_reset(&sink);

  feed(&state, &sink, BD_KEY_VOLUME_UP, true, 0);
  advance(&state, &sink, 400);
  CHECK(action_count(&sink) == 1, "repeat: hold did not fire");
  advance(&state, &sink, 500);
  CHECK(action_count(&sink) == 2, "repeat: first repeat missing");
  advance(&state, &sink, 600);
  CHECK(action_count(&sink) == 3, "repeat: second repeat missing");
  feed(&state, &sink, BD_KEY_VOLUME_UP, false, 650);
  CHECK(action_count(&sink) == 4, "repeat: hold release missing");
  advance(&state, &sink, 800);
  CHECK(action_count(&sink) == 4, "repeat continued after release");
  const bd_input_output *repeat = action_at(&sink, 1);
  CHECK(repeat != NULL && repeat->trigger == BD_INPUT_TRIGGER_REPEAT,
        "repeat: wrong trigger");
}

static void test_conflicts(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.first]\ncombo = Menu\naction = first.action\n"
            "[hotkey.second]\ncombo = Menu\naction = second.action\n"
            "[hotkey.third]\ncombo = Menu+X\naction = third.action\n",
            &table),
        "conflicts: table load");
  CHECK(table.rejected == 1, "conflicts: expected 1 rejected binding, got %zu",
        table.rejected);
  CHECK(table.count == 2, "conflicts: expected 2 accepted bindings, got %zu",
        table.count);

  bool saw_duplicate = false;
  bool saw_prefix = false;
  for (size_t i = 0; i < table.conflict_count; ++i) {
    const bd_hotkey_conflict *conflict = &table.conflicts[i];
    if (conflict->kind == BD_HOTKEY_CONFLICT_DUPLICATE) {
      saw_duplicate = true;
      CHECK(conflict->second == -1, "duplicate conflict: rejected index wrong");
      CHECK(conflict->first >= 0 && (size_t)conflict->first < table.count,
            "duplicate conflict: first index out of range");
      CHECK(strstr(conflict->message, "duplicate") != NULL,
            "duplicate conflict: message lacks 'duplicate'");
    }
    if (conflict->kind == BD_HOTKEY_CONFLICT_PREFIX) {
      saw_prefix = true;
    }
  }
  CHECK(saw_duplicate, "conflicts: duplicate not reported");
  CHECK(saw_prefix, "conflicts: prefix not reported");
  if (table.count > 0) {
    CHECK(table.items[0].source_line > 0,
          "conflicts: source line not filled");
    CHECK(strstr(table.items[0].source_file, "hotkeys.ini") != NULL,
          "conflicts: source file not filled");
  }
}


static void test_hold_cancels_tap(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.tap_y]\ncombo = Y\naction = tap.y\n"
            "[hotkey.hold_y]\ncombo = Y\nhold_ms = 500\naction = hold.y\n",
            &table),
        "hold+tap: table load");
  if (table.count != 2) {
    CHECK(table.count == 2, "hold+tap: expected 2 bindings, got %zu",
          table.count);
    return;
  }
  bd_input_state state;
  collector sink;
  make_state(&state, &table, "global");
  collector_reset(&sink);

  feed(&state, &sink, BD_KEY_Y, true, 0);
  advance(&state, &sink, 500);
  CHECK(action_count(&sink) == 1, "hold+tap: hold did not fire");
  feed(&state, &sink, BD_KEY_Y, false, 600);
  CHECK(action_count(&sink) == 2, "hold+tap: hold release missing");
  advance(&state, &sink, 1000);
  CHECK(action_count(&sink) == 2, "hold+tap: tap fired after a long press");

  make_state(&state, &table, "global");
  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_Y, true, 2000);
  feed(&state, &sink, BD_KEY_Y, false, 2100);
  CHECK(action_count(&sink) == 1, "hold+tap: short press did not tap");
  const bd_input_output *event = action_at(&sink, 0);
  CHECK(event != NULL && event->trigger == BD_INPUT_TRIGGER_TAP,
        "hold+tap: short press used the wrong trigger");
}

static void test_tap_vs_chord(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.menu_x]\ncombo = Menu+X\naction = menu.x\n"
            "[hotkey.tap_menu]\ncombo = Menu\naction = tap.menu\n",
            &table),
        "tap+chord: table load");
  if (table.count != 2) {
    CHECK(table.count == 2, "tap+chord: expected 2 bindings, got %zu",
          table.count);
    return;
  }
  bd_input_state state;
  collector sink;
  make_state(&state, &table, "global");
  collector_reset(&sink);

  feed(&state, &sink, BD_KEY_MENU, true, 0);
  feed(&state, &sink, BD_KEY_X, true, 30);
  feed(&state, &sink, BD_KEY_X, false, 40);
  feed(&state, &sink, BD_KEY_MENU, false, 50);
  CHECK(action_count(&sink) == 1, "tap+chord: chord did not win");
  const bd_input_output *event = action_at(&sink, 0);
  CHECK(event != NULL && event->trigger == BD_INPUT_TRIGGER_CHORD,
        "tap+chord: wrong trigger");
  advance(&state, &sink, 1000);
  CHECK(action_count(&sink) == 1, "tap fired after its chord");

  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_MENU, true, 2000);
  feed(&state, &sink, BD_KEY_MENU, false, 2020);
  CHECK(action_count(&sink) == 1, "tap+chord: Menu alone did not tap");
  event = action_at(&sink, 0);
  CHECK(event != NULL && event->trigger == BD_INPUT_TRIGGER_TAP,
        "tap+chord: Menu alone used the wrong trigger");
}


static void test_hold_modifier(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.hold_y]\ncombo = Y(hold:250)\naction = hold.y\n",
            &table),
        "hold modifier: table load");
  if (table.count != 1) {
    CHECK(table.count == 1, "hold modifier: expected 1 binding, got %zu",
          table.count);
    return;
  }
  CHECK(table.items[0].hold_ms == 250, "hold modifier: hold_ms=%d",
        table.items[0].hold_ms);
  bd_input_state state;
  collector sink;
  make_state(&state, &table, "global");
  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_X, true, 0); /* wrong key must not fire */
  advance(&state, &sink, 300);
  CHECK(action_count(&sink) == 0, "hold modifier: wrong key fired");
  feed(&state, &sink, BD_KEY_X, false, 310);
  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_Y, true, 400);
  advance(&state, &sink, 649);
  CHECK(action_count(&sink) == 0, "hold modifier fired before 250 ms");
  advance(&state, &sink, 650);
  CHECK(action_count(&sink) == 1, "hold modifier did not fire at 250 ms");
}



static void test_release_all(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.hold_b]\ncombo = B\nhold_ms = 200\naction = hold.b\n",
            &table),
        "release_all: table load");
  if (table.count != 1) {
    return;
  }
  bd_input_state state;
  collector sink;
  make_state(&state, &table, "global");
  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_B, true, 0);
  advance(&state, &sink, 200);
  CHECK(action_count(&sink) == 1, "release_all: hold did not start");
  bd_input_state_release_all(&state, 300, collect, &sink);
  CHECK(action_count(&sink) == 2, "release_all: hold up missing");
  CHECK(!bd_input_state_any_pressed(&state), "release_all left a stuck key");
  feed(&state, &sink, BD_KEY_B, false, 400);
  CHECK(action_count(&sink) == 2, "release_all: duplicate release fired");
}

static void test_device_mapping(void) {
  bd_device_map_type type;
  unsigned code = 0;
  int direction = 0;
  CHECK(bd_device_map_parse_value("BTN_SOUTH", &type, &code, &direction) &&
            type == BD_DEVICE_MAP_KEY && code == 0x130 && direction == 0,
        "mapping: BTN_SOUTH parse");
  CHECK(bd_device_map_parse_value("ABS_HAT0Y:-1", &type, &code, &direction) &&
            type == BD_DEVICE_MAP_ABS && code == 0x11 && direction == -1,
        "mapping: ABS_HAT0Y:-1 parse");
  CHECK(bd_device_map_parse_value("0x74", &type, &code, &direction) &&
            type == BD_DEVICE_MAP_KEY && code == 0x74,
        "mapping: numeric parse");
  CHECK(!bd_device_map_parse_value("NOT_A_KEY", &type, &code, &direction),
        "mapping: unknown name accepted");

  bd_device_map empty;
  memset(&empty, 0, sizeof(empty));
  bd_evdev fake;
  memset(&fake, 0, sizeof(fake));
  snprintf(fake.name, sizeof(fake.name), "fake gamepad");
  fake.fd = -1;
  bd_device_mapping mapping;
  CHECK(bd_device_map_match(&empty, &fake, &mapping),
        "mapping: builtin fallback missing");
  bd_key key = BD_KEY_UNKNOWN;
  CHECK(bd_device_map_translate_key(&mapping, 0x130, &key) && key == BD_KEY_A,
        "mapping: BTN_SOUTH -> A");
  bd_device_abs_key keys[4];
  int count = bd_device_map_translate_abs(&mapping, 0x11, -1, keys, 4);
  CHECK(count == 2, "mapping: HAT0Y produced %d keys", count);
  bool saw_up = false;
  bool saw_down = false;
  for (int i = 0; i < count; ++i) {
    if (keys[i].key == BD_KEY_UP && keys[i].pressed) {
      saw_up = true;
    }
    if (keys[i].key == BD_KEY_DOWN && !keys[i].pressed) {
      saw_down = true;
    }
  }
  CHECK(saw_up && saw_down, "mapping: HAT0Y direction handling");
}


static void test_overlapping_chords(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.first]\ncombo = L1+START\naction = first\n"
            "[hotkey.second]\ncombo = L1+SELECT\naction = second\n",
            &table),
        "overlapping chords: table load");
  if (table.count != 2) {
    CHECK(table.count == 2, "overlapping chords: expected 2 bindings, got %zu",
          table.count);
    return;
  }
  bd_input_state state;
  collector sink;
  make_state(&state, &table, "global");
  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_L1, true, 0);
  feed(&state, &sink, BD_KEY_START, true, 10);
  CHECK(action_count(&sink) == 1, "overlapping chords: first did not fire");
  feed(&state, &sink, BD_KEY_START, false, 20);
  feed(&state, &sink, BD_KEY_SELECT, true, 30);
  CHECK(action_count(&sink) == 1,
        "overlapping chords: second fired after the first consumed L1");
  feed(&state, &sink, BD_KEY_SELECT, false, 40);
  feed(&state, &sink, BD_KEY_L1, false, 50);
  collector_reset(&sink);
  feed(&state, &sink, BD_KEY_L1, true, 100);
  feed(&state, &sink, BD_KEY_SELECT, true, 110);
  CHECK(action_count(&sink) == 1,
        "overlapping chords: second did not fire after a clean press");
}

static void test_scope(void) {
  bd_hotkey_table table;
  CHECK(load_table(
            "[meta]\nschema_version = 1\n"
            "[hotkey.global]\ncombo = Menu\nhold_ms = 400\naction = global.menu\n"
            "[hotkey.app]\ncombo = A\naction = app.action\nscope = app:foo\n",
            &table),
        "scope: table load");
  if (table.count != 2) {
    CHECK(table.count == 2, "scope: expected 2 bindings");
    return;
  }

  bd_input_state global_state;
  collector sink;
  make_state(&global_state, &table, "global");
  collector_reset(&sink);
  feed(&global_state, &sink, BD_KEY_A, true, 0);
  feed(&global_state, &sink, BD_KEY_A, false, 20);
  CHECK(action_count(&sink) == 0, "app-scoped binding fired in global scope");

  bd_input_state app_state;
  make_state(&app_state, &table, "app:foo");
  collector_reset(&sink);
  feed(&app_state, &sink, BD_KEY_A, true, 0);
  feed(&app_state, &sink, BD_KEY_A, false, 20);
  CHECK(action_count(&sink) == 1, "app-scoped binding did not fire in app scope");

  bd_hotkey_table modes;
  CHECK(load_table("[meta]\nschema_version = 1\n"
                   "[hotkey.game]\ncombo = Y\naction = game.action\n"
                   "scope = mode:game\n",
                   &modes),
        "scope: mode table load");
  if (modes.count == 1) {
    bd_input_state game_state;
    make_state(&game_state, &modes, "app:foo");
    collector_reset(&sink);
    feed(&game_state, &sink, BD_KEY_Y, true, 0);
    feed(&game_state, &sink, BD_KEY_Y, false, 20);
    CHECK(action_count(&sink) == 1,
          "mode:game binding did not fire for an app scope");

    make_state(&game_state, &modes, "mode:desktop");
    collector_reset(&sink);
    feed(&game_state, &sink, BD_KEY_Y, true, 0);
    feed(&game_state, &sink, BD_KEY_Y, false, 20);
    CHECK(action_count(&sink) == 0,
          "mode:game binding fired on the desktop");

    make_state(&game_state, &modes, "mode:game");
    collector_reset(&sink);
    feed(&game_state, &sink, BD_KEY_Y, true, 0);
    feed(&game_state, &sink, BD_KEY_Y, false, 20);
    CHECK(action_count(&sink) == 1,
          "mode:game binding did not fire in mode:game scope");
  }
}

int main(void) {
  test_single_tap();
  test_chord();
  test_chord_window_boundary();
  test_hold_boundary();
  test_hint_passthrough();
  test_double_tap();
  test_double_cancels_single();
  test_sequence();
  test_sequence_cancels_tap();
  test_prefix_menu();
  test_release_order();
  test_out_of_order_and_duplicates();
  test_repeat();
  test_hold_cancels_tap();
  test_tap_vs_chord();
  test_overlapping_chords();
  test_hold_modifier();
  test_release_all();
  test_device_mapping();
  test_conflicts();
  test_scope();

  if (g_failures != 0) {
    fprintf(stderr, "hotkey_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("hotkey_test: all cases passed\n");
  return 0;
}
