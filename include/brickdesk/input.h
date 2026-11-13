#ifndef BRICKDESK_INPUT_H
#define BRICKDESK_INPUT_H

/*
 * Logical input keys, hotkey bindings, the pure hotkey state machine, evdev
 * device access and the inputd socket protocol.
 *
 * Everything above the platform layer speaks this enum. SDL scancodes and
 * Linux evdev key codes are translated at the edges and never leak into the
 * desktop, the test harness or the plugin protocol.
 *
 * The state machine is deliberately free of IO and wall-clock reads: callers
 * pass timestamped key transitions in and receive action events back. That
 * keeps the host SDL path, the screenshot harness and brickdesk-inputd on one
 * implementation.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ keys */

typedef enum bd_key {
  BD_KEY_UNKNOWN = 0,
  BD_KEY_A,
  BD_KEY_B,
  BD_KEY_X,
  BD_KEY_Y,
  BD_KEY_L1,
  BD_KEY_R1,
  BD_KEY_L2,
  BD_KEY_R2,
  BD_KEY_SELECT,
  BD_KEY_START,
  BD_KEY_MENU,
  BD_KEY_HOME,
  BD_KEY_UP,
  BD_KEY_DOWN,
  BD_KEY_LEFT,
  BD_KEY_RIGHT,
  BD_KEY_POWER,
  BD_KEY_VOLUME_UP,
  BD_KEY_VOLUME_DOWN,
  BD_KEY_COUNT
} bd_key;

/* Canonical upper-case name, e.g. "RIGHT". Never NULL. */
const char *bd_key_name(bd_key key);

/* Case-insensitive parse of a canonical name. Returns BD_KEY_UNKNOWN. */
bd_key bd_key_from_name(const char *name);

/* --------------------------------------------------------------- hotkeys */

#define BD_HOTKEY_ID_MAX 64
#define BD_HOTKEY_COMBO_MAX 128
#define BD_HOTKEY_ACTION_MAX 64
#define BD_HOTKEY_SCOPE_MAX 64
#define BD_HOTKEY_PLUGIN_MAX 64
#define BD_HOTKEY_MAX_STEPS 4
#define BD_HOTKEY_MAX_KEYS 8
#define BD_HOTKEY_MAX 64
#define BD_HOTKEY_CONFLICT_MAX 64
#define BD_HOTKEY_DIAGNOSTIC_MAX 64
#define BD_HOTKEY_SOURCE_MAX 256

/*
 * One step of a combo: a chord such as "L1+R1+START" or the single key
 * "MENU". `hold_ms` and `double_tap` come from the combo modifiers
 * "(hold:N)" and "(double)".
 */
typedef struct bd_hotkey_step {
  bd_key keys[BD_HOTKEY_MAX_KEYS];
  int key_count;
  uint32_t key_mask;
  int hold_ms;      /* 0 when the step has no hold modifier */
  bool double_tap;
} bd_hotkey_step;

typedef struct bd_hotkey {
  char id[BD_HOTKEY_ID_MAX];
  char combo[BD_HOTKEY_COMBO_MAX];
  char action[BD_HOTKEY_ACTION_MAX];
  char scope[BD_HOTKEY_SCOPE_MAX];
  char plugin[BD_HOTKEY_PLUGIN_MAX];
  char action_id[BD_HOTKEY_ACTION_MAX];

  int step_count;
  bd_hotkey_step steps[BD_HOTKEY_MAX_STEPS];

  /*
   * Effective hold threshold in ms. 0 means a tap/chord binding. A value > 0
   * means the action fires at the threshold and has a release phase; this is
   * what the hint bar uses. The loader only sets it when the INI set hold_ms
   * explicitly or the combo used "(hold:N)", or when repeat=true.
   */
  int hold_ms;
  int window_ms;    /* chord window, default 50 */
  int sequence_ms;  /* sequence step window, default 800 */
  int repeat_ms;    /* default 100 */
  bool repeat;
  bool suppress;
  bool confirm;

  char source_file[BD_HOTKEY_SOURCE_MAX];
  int source_line;
} bd_hotkey;

typedef enum bd_hotkey_conflict_kind {
  /* Identical step sequence: the later binding is rejected. */
  BD_HOTKEY_CONFLICT_DUPLICATE = 0,
  /* One combo is a subset/prefix of the other; resolved by priority. */
  BD_HOTKEY_CONFLICT_PREFIX,
  /* The first steps share keys but neither is a subset of the other. */
  BD_HOTKEY_CONFLICT_OVERLAP
} bd_hotkey_conflict_kind;

typedef struct bd_hotkey_conflict {
  bd_hotkey_conflict_kind kind;
  int first;  /* index into bd_hotkey_table.items */
  int second;
  char message[320];
} bd_hotkey_conflict;

typedef struct bd_hotkey_diagnostic {
  bool error;
  char binding[BD_HOTKEY_ID_MAX];
  char file[BD_HOTKEY_SOURCE_MAX];
  int line;
  char message[192];
} bd_hotkey_diagnostic;

typedef struct bd_hotkey_table {
  bd_hotkey items[BD_HOTKEY_MAX];
  size_t count;
  size_t rejected;
  bd_hotkey_conflict conflicts[BD_HOTKEY_CONFLICT_MAX];
  size_t conflict_count;
  bd_hotkey_diagnostic diagnostics[BD_HOTKEY_DIAGNOSTIC_MAX];
  size_t diagnostic_count;
} bd_hotkey_table;

struct bd_config;

/*
 * Parses a canonical combo ("MENU", "L1+R1+START", "SELECT,UP",
 * "MENU(hold:800)", "X(double)") into `out`. The parser is independent of the
 * config system so tests and tools can construct bindings directly. `out` is
 * zeroed first; id/action/scope are not set. Returns false and fills `error`
 * on syntax errors.
 */
bool bd_hotkey_parse(bd_hotkey *out, const char *combo, char *error,
                     size_t error_size);

/*
 * Loads every [hotkey.<id>] section from a loaded configuration. Invalid
 * bindings are skipped with a diagnostic; duplicate combos are rejected and
 * reported, while prefix/overlap ambiguity is reported but kept (runtime
 * priority resolves it). Returns false only for invalid arguments or OOM.
 */
bool bd_hotkey_table_load(bd_hotkey_table *table, const struct bd_config *config);

/* Human-readable trigger name, e.g. "chord". Never NULL. */
const char *bd_hotkey_trigger_name(int trigger);

/* -------------------------------------------------------- state machine */

typedef enum bd_input_trigger {
  BD_INPUT_TRIGGER_TAP = 0,
  BD_INPUT_TRIGGER_CHORD,
  BD_INPUT_TRIGGER_HOLD,
  BD_INPUT_TRIGGER_REPEAT,
  BD_INPUT_TRIGGER_DOUBLE,
  BD_INPUT_TRIGGER_SEQUENCE
} bd_input_trigger;

typedef enum bd_input_output_kind {
  /* A logical key that was not reserved by a tap binding. */
  BD_INPUT_OUTPUT_KEY = 0,
  /* A hotkey binding fired; `pressed` is the down/up phase. */
  BD_INPUT_OUTPUT_ACTION
} bd_input_output_kind;

typedef struct bd_input_output {
  bd_input_output_kind kind;
  uint64_t time_ms;
  bd_key key;               /* KEY output */
  bool pressed;             /* KEY output and hold/repeat ACTION phase */
  int binding;              /* ACTION: index into the table */
  bd_input_trigger trigger; /* ACTION */
} bd_input_output;

typedef void (*bd_input_emit_fn)(void *user, const bd_input_output *event);

typedef struct bd_input_binding_state {
  int step;
  bool step_active;
  uint32_t matched;
  uint64_t step_start_ms;

  bool step_fired;
  uint32_t fired_mask;

  bool seq_waiting;
  bool seq_ready;
  uint64_t seq_deadline_ms;

  bool tap_armed;
  bd_key tap_key;
  bool tap_pending;
  uint64_t tap_deadline_ms;

  bool hold_pending;
  uint64_t hold_deadline_ms;
  bool hold_active;
  uint64_t repeat_next_ms;

  bool double_waiting;
  uint64_t double_deadline_ms;
  bool double_fired;
} bd_input_binding_state;

typedef struct bd_input_state {
  const bd_hotkey_table *table;
  bool active[BD_HOTKEY_MAX];
  bd_input_binding_state runtime[BD_HOTKEY_MAX];

  bool pressed[BD_KEY_COUNT];
  uint64_t down_ms[BD_KEY_COUNT];
  bool passthrough_down[BD_KEY_COUNT];
  bool hold_fired[BD_KEY_COUNT];
  bool consumed[BD_KEY_COUNT];
  bool reserved[BD_KEY_COUNT];
  bool has_double[BD_KEY_COUNT];
  uint64_t tap_prefix_delay_ms[BD_KEY_COUNT];

  int double_tap_ms;
  uint64_t last_time_ms;
  bool has_time;
  char scope[BD_HOTKEY_SCOPE_MAX];
} bd_input_state;

/*
 * Initializes a state for the bindings whose scope is "global" or exactly
 * `scope` (pass NULL or "global" for the desktop/global path).
 * `double_tap_ms` comes from input.double_tap_ms (300 by default).
 */
void bd_input_state_init(bd_input_state *state, const bd_hotkey_table *table,
                         const char *scope, int double_tap_ms);

/*
 * Feeds one timestamped key transition. `time_ms` must be monotonic; an
 * out-of-order timestamp is clamped to the last processed time. Duplicate
 * down/up events are ignored. Emitted events are delivered through `emit`.
 */
void bd_input_state_feed(bd_input_state *state, bd_key key, bool pressed,
                         uint64_t time_ms, bd_input_emit_fn emit, void *user);

/* Fires any timers due at or before `now_ms`. */
void bd_input_state_advance(bd_input_state *state, uint64_t now_ms,
                            bd_input_emit_fn emit, void *user);

/* Smallest pending timer, or false when the state is idle. */
bool bd_input_state_next_deadline(const bd_input_state *state, uint64_t *out_ms);

bool bd_input_state_any_pressed(const bd_input_state *state);
bool bd_input_state_is_pressed(const bd_input_state *state, bd_key key);

/*
 * Emits release phases for active holds and pass-through keys, then clears
 * all key and binding state. Used on SYN_DROPPED, ungrab and shutdown so no
 * key or hint can stay stuck.
 */
void bd_input_state_release_all(bd_input_state *state, uint64_t now_ms,
                                bd_input_emit_fn emit, void *user);

/* ------------------------------------------------------------- evdev I/O */

#define BD_EVDEV_PATH_MAX 128
#define BD_EVDEV_NAME_MAX 128
#define BD_EVDEV_MAX_DEVICES 16
#define BD_EVDEV_KEY_MAX 0x2ff
#define BD_EVDEV_ABS_MAX 0x3f
#define BD_EVDEV_SW_MAX 0x10

typedef struct bd_evdev {
  int fd;
  char path[BD_EVDEV_PATH_MAX];
  char name[BD_EVDEV_NAME_MAX];
  uint16_t bustype;
  uint16_t vendor;
  uint16_t product;
  uint16_t version;
  bool has_key;
  bool has_abs;
  bool has_sw;
  bool is_gamepad; /* has BTN_SOUTH/BTN_GAMEPAD or a D-pad HAT */
  /*
   * Test backend: a FIFO event* node under --device-dir is read like evdev
   * and described by <path>.meta. `fake_grab_allowed` simulates EVIOCGRAB
   * success/failure without uinput or hardware. Real evdev nodes leave both
   * fields false.
   */
  bool is_fake;
  bool fake_grab_allowed;
  unsigned char key_bits[(BD_EVDEV_KEY_MAX + 8) / 8];
  unsigned char abs_bits[(BD_EVDEV_ABS_MAX + 8) / 8];
  unsigned char sw_bits[(BD_EVDEV_SW_MAX + 8) / 8];
} bd_evdev;

typedef enum bd_evdev_event_kind {
  BD_EVDEV_EVENT_NONE = 0,
  BD_EVDEV_EVENT_KEY,
  BD_EVDEV_EVENT_ABS,
  BD_EVDEV_EVENT_SWITCH,
  BD_EVDEV_EVENT_SYN_DROPPED
} bd_evdev_event_kind;

typedef struct bd_evdev_event {
  bd_evdev_event_kind kind;
  unsigned code;
  int value;
  uint64_t time_ms;
} bd_evdev_event;

/*
 * Scans `dir` (NULL means /dev/input) for event* nodes and opens the ones
 * that expose keys, axes or switches. The returned fds are O_NONBLOCK and
 * close-on-exec. Returns the device count, or -1 with `error` filled in.
 *
 * `bd_evdev_scan_ex` additionally sets `incomplete` when an event* node could
 * not be opened or processed, so a daemon can retry it with backoff even when
 * no directory change follows (for example a transient permission error).
 */
int bd_evdev_scan_ex(const char *dir, bd_evdev *out, size_t max,
                     bool *incomplete, char *error, size_t error_size);
int bd_evdev_scan(const char *dir, bd_evdev *out, size_t max, char *error,
                  size_t error_size);
void bd_evdev_close(bd_evdev *device);

bool bd_evdev_has_key(const bd_evdev *device, unsigned code);
bool bd_evdev_has_abs(const bd_evdev *device, unsigned code);
bool bd_evdev_has_sw(const bd_evdev *device, unsigned code);

/* Reads one event. Returns 1 on an event, 0 when nothing is pending, -1 on error. */
int bd_evdev_read(const bd_evdev *device, bd_evdev_event *out);

/* Reads the current key bitmap; used after SYN_DROPPED. */
bool bd_evdev_key_bits(const bd_evdev *device, unsigned char *bits,
                       size_t bits_size);

/* Current absolute-axis value, or 0 when the axis is unavailable. */
int bd_evdev_abs_value(const bd_evdev *device, unsigned code);

/* EVIOCGRAB helper. Returns false and sets errno on failure. */
bool bd_evdev_grab(bd_evdev *device, bool grab);

/* Reverse lookup for logs and --list/--learn. NULL for unknown codes. */
const char *bd_evdev_key_name(unsigned code);
const char *bd_evdev_abs_name(unsigned code);
const char *bd_evdev_sw_name(unsigned code);

/* --------------------------------------------------------- device mapping */

#define BD_DEVICE_MAX 8
#define BD_DEVICE_MAX_MAPPINGS 32

typedef enum bd_device_map_type {
  BD_DEVICE_MAP_KEY = 0,
  BD_DEVICE_MAP_ABS
} bd_device_map_type;

typedef struct bd_device_map_entry {
  bd_key key;
  bd_device_map_type type;
  unsigned code;
  int direction; /* ABS only: -1, 0 (any nonzero) or 1 */
  char source_file[BD_HOTKEY_SOURCE_MAX];
  int source_line;
} bd_device_map_entry;

typedef struct bd_device_mapping {
  char name[BD_EVDEV_NAME_MAX];
  uint16_t vendor;
  uint16_t product;
  bool has_ids;
  int entry_count;
  bd_device_map_entry entries[BD_DEVICE_MAX_MAPPINGS];
} bd_device_mapping;

typedef struct bd_device_map {
  bd_device_mapping devices[BD_DEVICE_MAX];
  size_t count;
} bd_device_map;

/*
 * Loads [device.<name>] sections from input.ini. Each section starts from the
 * built-in gamepad/keyboard map and overrides the fields it names, so a
 * device works before its exact key codes are confirmed on hardware.
 */
bool bd_device_map_load(bd_device_map *map, const struct bd_config *config);

/*
 * Chooses the mapping for a device: exact vendor/product first, then name,
 * then the built-in mapping. Returns false only when `out` cannot be filled.
 */
bool bd_device_map_match(const bd_device_map *map, const bd_evdev *device,
                         bd_device_mapping *out);

bool bd_device_map_translate_key(const bd_device_mapping *mapping, unsigned code,
                                 bd_key *out);

/* Resolves an absolute axis value to one or more logical keys. */
typedef struct bd_device_abs_key {
  bd_key key;
  bool pressed;
} bd_device_abs_key;

int bd_device_map_translate_abs(const bd_device_mapping *mapping, unsigned code,
                                int value, bd_device_abs_key *out, size_t max);

/* Parses "BTN_SOUTH", "KEY_POWER", "0x130" or "ABS_HAT0Y:-1". */
bool bd_device_map_parse_value(const char *text, bd_device_map_type *type,
                               unsigned *code, int *direction);

/* ------------------------------------------------------------ inputd IPC */

/*
 * brickdesk-inputd speaks newline-delimited JSON over SOCK_STREAM.
 *
 * Client -> daemon:
 *   {"cmd":"subscribe","actions":true,"keys":false}
 *   {"cmd":"grab"}
 *   {"cmd":"ungrab"}
 *   {"cmd":"release_all"}
 *   {"cmd":"ping"}
 *
 * Daemon -> client:
 *   {"type":"hello","version":1,"pid":123,"devices":[...]}
 *   {"type":"action","binding":"hotkey.menu","action":"menu.open",
 *    "combo":"Menu","trigger":"hold","phase":"down","key":"MENU",
 *    "time_ms":1234}
 *   {"type":"key","key":"UP","pressed":true,"source":"TrimUI Gamepad",
 *    "time_ms":1235}
 *   {"type":"switch","switch":"headphone","value":1,"time_ms":1236}
 *   {"type":"grab","active":true,"pending":false,"forced":false}
 *   {"type":"release_all","time_ms":1237}
 *   {"type":"device","action":"added","device":"TrimUI Gamepad",
 *    "path":"/dev/input/event2","grabbed":false,"time_ms":1238}
 *   {"type":"device","action":"removed","device":"TrimUI Gamepad",
 *    "path":"/dev/input/event2","reason":"hangup","time_ms":1239}
 *   {"type":"pong","time_ms":1238}
 *   {"type":"error","cmd":"grab","error":"EBUSY","device":"..."}
 *   {"type":"warning","message":"syn_dropped","device":"...",
 *    "time_ms":1239}
 *   {"type":"warning","message":"hotplug_grab_failed",
 *    "device":"/dev/input/event5","error":"Device or resource busy",
 *    "time_ms":1240}
 *
 * `hello` still lists registered device names. Device add/remove lines are
 * additive diagnostics; clients that do not know them can ignore the type.
 *
 * A line is at most 4096 bytes. A client that cannot keep up is disconnected;
 * the daemon never blocks on a subscriber.
 */
#define BD_INPUTD_PROTOCOL_VERSION 1
#define BD_INPUTD_LINE_MAX 4096

#endif /* BRICKDESK_INPUT_H */
