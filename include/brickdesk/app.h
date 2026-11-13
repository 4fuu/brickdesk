#ifndef BRICKDESK_APP_H
#define BRICKDESK_APP_H

/*
 * Application packages, suspension sessions and the last-frame handoff.
 *
 * This header is the public contract for M1-06.  It covers three pieces:
 *
 *   - app.json manifests and app-directory scanning (src/session/manifest.c);
 *   - the session state machine and app IPC host (src/session/session.c and
 *     src/session/app_protocol.c);
 *   - the shared-memory last frame written by the in-app menu shim before it
 *     suspends an application (src/session/last_frame.c).
 *
 * The session layer is single threaded: call bd_session_poll() from the
 * desktop event loop, and call the other functions from that same thread.
 * Event callbacks must not call back into the session; record the event and
 * act after bd_session_poll() returns.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BD_APP_ID_MAX 65
#define BD_APP_NAME_MAX 96
#define BD_APP_SUBTITLE_MAX 128
#define BD_APP_VERSION_MAX 32
#define BD_APP_PATH_MAX 512
#define BD_APP_COLOR_MAX 8
#define BD_APP_METHOD_MAX 64
#define BD_APP_MAX_EXEC 16
#define BD_APP_MAX_ENV 32
#define BD_APP_MAX_EXCLUDE_KEYS 16
#define BD_APP_MAX_LIVE_FIELDS 8
#define BD_APP_MANIFEST_MAX_BYTES (256u * 1024u)

/* ------------------------------------------------------------------ */
/* Manifest                                                           */
/* ------------------------------------------------------------------ */

typedef enum bd_app_type {
  BD_APP_TYPE_APP = 0,
  BD_APP_TYPE_GAME
} bd_app_type;

typedef enum bd_app_category {
  BD_APP_CATEGORY_APP = 0,
  BD_APP_CATEGORY_GAME,
  BD_APP_CATEGORY_EMULATOR,
  BD_APP_CATEGORY_TOOL,
  BD_APP_CATEGORY_SERVICE
} bd_app_category;

typedef enum bd_app_cover_span {
  BD_APP_SPAN_1X1 = 0,
  BD_APP_SPAN_2X1,
  BD_APP_SPAN_1X2,
  BD_APP_SPAN_2X2,
  BD_APP_SPAN_3X2,
  BD_APP_SPAN_COUNT
} bd_app_cover_span;

typedef enum bd_app_menu_mode {
  BD_APP_MENU_OVERLAY = 0,
  BD_APP_MENU_EMBEDDED,
  BD_APP_MENU_HANDOFF,
  BD_APP_MENU_NONE
} bd_app_menu_mode;

typedef enum bd_app_hook_type {
  BD_APP_HOOK_NONE = 0,
  BD_APP_HOOK_IPC,
  BD_APP_HOOK_COMMAND
} bd_app_hook_type;

typedef enum bd_app_hook_kind {
  BD_APP_HOOK_PAUSE = 0,
  BD_APP_HOOK_RESUME,
  BD_APP_HOOK_SAVE,
  BD_APP_HOOK_LOAD,
  BD_APP_HOOK_QUIT,
  BD_APP_HOOK_SCREENSHOT,
  BD_APP_HOOK_COUNT
} bd_app_hook_kind;

typedef enum bd_app_live_provider {
  BD_APP_LIVE_NONE = 0,
  BD_APP_LIVE_NOW_PLAYING,
  BD_APP_LIVE_FILE
} bd_app_live_provider;

#define BD_APP_LIVE_FIELD_TITLE (1u << 0)
#define BD_APP_LIVE_FIELD_SUBTITLE (1u << 1)
#define BD_APP_LIVE_FIELD_PROGRESS (1u << 2)
#define BD_APP_LIVE_FIELD_ARTWORK (1u << 3)

typedef struct bd_app_capabilities {
  bool pause;
  bool resume;
  bool save_state;
  bool load_state;
  bool screenshot;
  bool background;
  bool display_release;
  /* Legacy default is true; an app may declare false to opt out. */
  bool rumble;
  bool led_ambient;
} bd_app_capabilities;

typedef struct bd_app_hook {
  bd_app_hook_type type;
  /* IPC method name; default depends on the hook kind. */
  char method[BD_APP_METHOD_MAX];
  /* command hook argv.  Executed asynchronously by the session with the
   * hook's timeout; output is appended to <state_root>/hooks.log. */
  char **exec;
  size_t exec_count;
  /* 0 means "use the session default for this hook". */
  int timeout_ms;
} bd_app_hook;

typedef struct bd_app_menu {
  bd_app_menu_mode mode;
  bool overlay_safe;
  char exclude_keys[BD_APP_MAX_EXCLUDE_KEYS][32];
  size_t exclude_key_count;
} bd_app_menu;

typedef struct bd_app_perf {
  char profile[32];
  char core[64];
} bd_app_perf;

typedef struct bd_app_state {
  /* Optional path relative to BRICKDESK_STATE_DIR. */
  char path[BD_APP_PATH_MAX];
  char format[32];
  int max_slots;
} bd_app_state;

typedef struct bd_app_live {
  bd_app_live_provider provider;
  char state_path[BD_APP_PATH_MAX];
  int refresh_ms;
  unsigned fields; /* BD_APP_LIVE_FIELD_* */
} bd_app_live;

/*
 * A parsed manifest.  Strings in `exec` and `env_*` are owned by the struct;
 * call bd_app_destroy() when done (including after a failed parse, which
 * releases whatever was allocated).
 */
typedef struct bd_app {
  char dir[BD_APP_PATH_MAX];      /* absolute, symlink-resolved app directory */
  char manifest_path[BD_APP_PATH_MAX];
  int schema;
  char id[BD_APP_ID_MAX];
  char name[BD_APP_NAME_MAX];
  char subtitle[BD_APP_SUBTITLE_MAX];
  char version[BD_APP_VERSION_MAX];
  bd_app_type type;
  bd_app_category category;

  char **exec;
  size_t exec_count;
  char cwd[BD_APP_PATH_MAX];      /* absolute */

  char **env_keys;
  char **env_values;
  size_t env_count;

  /* Paths are relative to `dir`.  Empty means absent. */
  char icon[BD_APP_PATH_MAX];
  char covers[BD_APP_SPAN_COUNT][BD_APP_PATH_MAX];
  char backdrop[BD_APP_PATH_MAX];
  char accent[BD_APP_COLOR_MAX];
  char fallback_color[BD_APP_COLOR_MAX];

  bd_app_capabilities caps;
  bd_app_hook hooks[BD_APP_HOOK_COUNT];
  bd_app_menu menu;
  bd_app_perf perf;
  bd_app_state state;
  bd_app_live live;
} bd_app;

/*
 * Parses `manifest_path`.  Returns false and fills `error` (including the
 * manifest path and, when possible, a line number) on invalid input.
 * `out` must be zero-initialised and is safe to destroy after any result.
 */
bool bd_app_parse_file(const char *manifest_path, bd_app *out, char *error,
                       size_t error_size);
/* Same, for an in-memory manifest.  `dir` must be the absolute app dir. */
bool bd_app_parse_json(const char *json, size_t length, const char *dir,
                       const char *manifest_path, bd_app *out, char *error,
                       size_t error_size);
void bd_app_destroy(bd_app *app);
/* Deep copy; `dst` must be zero-initialised. */
bool bd_app_copy(bd_app *dst, const bd_app *src);

const char *bd_app_category_name(bd_app_category category);
const char *bd_app_menu_mode_name(bd_app_menu_mode mode);
const char *bd_app_hook_name(bd_app_hook_kind kind);

/* Span names ("1x1" ... "3x2") and pixel aspect helpers. */
const char *bd_app_span_name(bd_app_cover_span span);
bool bd_app_span_from_name(const char *name, bd_app_cover_span *out);

/* ------------------------------------------------------------------ */
/* App directory scanning                                            */
/* ------------------------------------------------------------------ */

typedef struct bd_app_list bd_app_list;
typedef void (*bd_app_scan_diagnostic_fn)(void *user, const char *path,
                                          const char *message);

bd_app_list *bd_app_list_create(void);
void bd_app_list_destroy(bd_app_list *list);

/*
 * Scans `dir` for direct child directories containing app.json (preferred)
 * or manifest.json.  Only manifests are read; images are checked lazily by
 * bd_app_asset_*().  Invalid manifests are reported through `diagnostic` and
 * skipped.  Entries are sorted by id for a stable UI order.
 *
 * Returns false only when the directory cannot be read or memory runs out.
 */
bool bd_app_list_scan(bd_app_list *list, const char *dir,
                      bd_app_scan_diagnostic_fn diagnostic, void *user,
                      char *error, size_t error_size);

size_t bd_app_list_count(const bd_app_list *list);
const bd_app *bd_app_list_at(const bd_app_list *list, size_t index);
const bd_app *bd_app_list_find(const bd_app_list *list, const char *id);

/* BRICKDESK_APPS_DIR, or the directory baked in at build time. */
const char *bd_app_default_dir(void);

/* ------------------------------------------------------------------ */
/* Image fallback                                                     */
/* ------------------------------------------------------------------ */

typedef enum bd_app_asset_kind {
  BD_APP_ASSET_COVER = 0, /* a cover image, possibly a smaller span */
  BD_APP_ASSET_BACKDROP,  /* the declared backdrop image */
  BD_APP_ASSET_ICON,      /* art.icon */
  BD_APP_ASSET_MATERIAL,  /* accent/fallback colour + initial */
  BD_APP_ASSET_NEUTRAL    /* surface-2 + generic info icon */
} bd_app_asset_kind;

typedef struct bd_app_asset {
  bd_app_asset_kind kind;
  /* Absolute path for COVER/ICON; empty otherwise. */
  char path[BD_APP_PATH_MAX];
  /* #RRGGBB for MATERIAL; the app accent when known for the other kinds. */
  char color[BD_APP_COLOR_MAX];
  /* First UTF-8 code point of the name; up to 4 bytes. */
  char initial[8];
} bd_app_asset;

/*
 * Resolves the asset the UI should use, following design.md 4.4:
 * requested span, same-orientation smaller span, 1x1, icon, material +
 * initial, neutral.  Only paths that exist inside the app directory are
 * returned; symlink escapes are rejected.
 */
bool bd_app_asset_cover(const bd_app *app, bd_app_cover_span span,
                        bd_app_asset *out);
/* Backdrop: requested image, then the largest available cover, then colour. */
bool bd_app_asset_backdrop(const bd_app *app, bd_app_asset *out);

/* ------------------------------------------------------------------ */
/* Last-frame shared memory                                           */
/* ------------------------------------------------------------------ */

/*
 * Layout: bd_app_last_frame_header followed by `data_size` pixel bytes.
 * The writer maps the file read/write, sets FLAG_WRITING while updating,
 * writes pixels, then publishes the completed header with a new frame_seq
 * and clears FLAG_WRITING.  Readers open read-only and reject a header whose
 * magic, version, dimensions, format, stride or size do not match the file.
 */
#define BD_APP_LAST_FRAME_MAGIC 0x52464442u /* file bytes: "BDFR" */
#define BD_APP_LAST_FRAME_VERSION 1u
#define BD_APP_LAST_FRAME_FILE "last_frame.bin"
#define BD_APP_LAST_FRAME_MAX_DIMENSION 4096u
#define BD_APP_LAST_FRAME_HEADER_SIZE 56u

#define BD_APP_LAST_FRAME_FLAG_WRITING (1u << 0)

typedef enum bd_app_pixel_format {
  BD_APP_PIXEL_RGBA8888 = 1, /* bytes R,G,B,A; stride = width * 4 */
  BD_APP_PIXEL_RGB565 = 2    /* native-endian uint16; stride = width * 2 */
} bd_app_pixel_format;

typedef struct bd_app_last_frame_header {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t width;
  uint32_t height;
  uint32_t format;      /* bd_app_pixel_format */
  uint32_t stride;      /* bytes per row */
  uint64_t frame_seq;   /* increases on every published frame */
  uint64_t timestamp_ms;/* CLOCK_MONOTONIC milliseconds, advisory */
  uint64_t data_size;   /* pixel bytes, >= stride * height */
  uint32_t checksum;    /* reserved, 0 when not used */
  uint32_t flags;       /* BD_APP_LAST_FRAME_FLAG_* */
} bd_app_last_frame_header;

/* The on-disk layout is versioned; do not change field order or packing. */
_Static_assert(sizeof(bd_app_last_frame_header) == BD_APP_LAST_FRAME_HEADER_SIZE,
               "last-frame header layout changed");

typedef struct bd_app_last_frame {
  int fd;
  void *map;
  size_t map_size;
  bd_app_last_frame_header header;
  const uint8_t *pixels;
} bd_app_last_frame;

size_t bd_app_last_frame_pixel_size(uint32_t format, uint32_t width,
                                    uint32_t height);

/*
 * Opens and validates a last-frame file.  `frame` must be zero-initialised;
 * call bd_app_last_frame_close() after a successful open.  On failure
 * `error` explains which header field was rejected.
 */
bool bd_app_last_frame_open(const char *path, bd_app_last_frame *frame,
                            char *error, size_t error_size);
void bd_app_last_frame_close(bd_app_last_frame *frame);

/*
 * Copies a tear-free snapshot into `dst`.  The destination is tightly packed
 * at `width * bpp` per row even when the writer used a wider stride.  Returns
 * false when the header changed during the copy or `dst_size` is too small.
 */
bool bd_app_last_frame_copy(const bd_app_last_frame *frame, void *dst,
                            size_t dst_size,
                            bd_app_last_frame_header *out_header, char *error,
                            size_t error_size);

/* Builds <dir>/last-frame-<id>.bin; `id` must be a safe path token. */
bool bd_app_last_frame_path(const char *dir, const char *id, char *out,
                            size_t out_size);

/*
 * Publishes one frame using the writer protocol from docs/app-protocol.md.
 * `pixels` holds height rows of `stride` bytes; rows are stored in the order
 * GL's glReadPixels returns them (bottom row first), matching the demo.
 * `frame_seq` is incremented for the published header and may be NULL.
 * The file is created or truncated; the mapping is private to the call.
 */
bool bd_app_last_frame_write(const char *path, uint32_t width, uint32_t height,
                             bd_app_pixel_format format, const void *pixels,
                             size_t stride, uint64_t *frame_seq, char *error,
                             size_t error_size);

/* ------------------------------------------------------------------ */
/* Session                                                            */
/* ------------------------------------------------------------------ */

typedef enum bd_session_state {
  BD_SESSION_IDLE = 0,
  BD_SESSION_LAUNCHING,
  BD_SESSION_FOREGROUND,
  BD_SESSION_SUSPENDING,
  BD_SESSION_BACKGROUND,
  BD_SESSION_RESUMING,
  BD_SESSION_TERMINATING,
  BD_SESSION_EXITED,
  BD_SESSION_CRASHED
} bd_session_state;

typedef enum bd_session_result {
  BD_SESSION_OK = 0,
  BD_SESSION_NEEDS_CONFIRM,
  BD_SESSION_BUSY,
  BD_SESSION_NOT_FOUND,
  BD_SESSION_INVALID_STATE,
  BD_SESSION_UNSUPPORTED,
  BD_SESSION_SPAWN_FAILED,
  BD_SESSION_IO_ERROR,
  /* The app's state directory is over session.state_quota_mb; nothing was
   * deleted and the app was not started or resumed. */
  BD_SESSION_STATE_QUOTA
} bd_session_result;

typedef struct bd_session_timeouts {
  int hello_ms;   /* wait for app.hello before declaring foreground */
  int pause_ms;   /* default for app.pause */
  int resume_ms;  /* default for app.resume */
  int save_ms;    /* default for state.save */
  int quit_ms;    /* default for app.quit */
  int term_ms;    /* SIGTERM -> SIGKILL grace */
  int kill_ms;    /* SIGKILL -> give up grace */
  int display_ms; /* app.display_release/acquire */
  int freeze_ms;  /* SIGSTOP -> confirm stopped for light sleep */
} bd_session_timeouts;

/*
 * Perf integration is a small vtable so this milestone does not depend on
 * perfd.  A real client can call the perfd socket; tests use a fake.
 * apply() runs before exec, restore() after the app exits.
 */
typedef struct bd_session_perf_ops {
  void *user;
  bool (*apply)(void *user, const char *profile, const char *core);
  bool (*restore)(void *user);
} bd_session_perf_ops;

typedef enum bd_session_event_kind {
  BD_SESSION_EVENT_STATE = 0,   /* state changed */
  BD_SESSION_EVENT_EXIT,        /* app exited (code/signal filled) */
  BD_SESSION_EVENT_CRASH,       /* unexpected exit or signal */
  BD_SESSION_EVENT_HOOK,        /* an IPC hook completed or timed out */
  BD_SESSION_EVENT_NOTIFICATION,/* app.toast / app.log / app.state ... */
  BD_SESSION_EVENT_PROTOCOL_ERROR,
  BD_SESSION_EVENT_NEEDS_ATTENTION, /* background limit reached */
  /*
   * The in-app menu shim asked the session to act on its own app. `method` is
   * "background" or "quit"; `params` is the raw request line. The desktop
   * handles it after bd_session_poll() returns by calling
   * bd_session_background() or bd_session_terminate().
   */
  BD_SESSION_EVENT_MENU_ACTION
} bd_session_event_kind;

typedef struct bd_session_event {
  bd_session_event_kind kind;
  const char *app_id;
  int pid;
  bd_session_state previous;
  bd_session_state state;
  int exit_code;  /* WEXITSTATUS when exited normally */
  int term_signal;/* WTERMSIG when killed */
  const char *hook;   /* hook name for EVENT_HOOK */
  bool hook_ok;
  const char *method; /* protocol method for EVENT_NOTIFICATION */
  const char *params; /* JSON object text, valid during the callback */
  char suggested_app_id[BD_APP_ID_MAX]; /* EVENT_NEEDS_ATTENTION */
} bd_session_event;

typedef void (*bd_session_event_fn)(void *user, const bd_session_event *event);

typedef struct bd_session_config {
  const char *socket_dir; /* NULL: /run/brickdesk, then /tmp */
  const char *frame_dir;  /* NULL: socket_dir */
  const char *state_root; /* NULL: BRICKDESK_STATE_DIR or /mnt/.../state */
  const char *session_id; /* NULL: generated per session */
  int max_background;     /* <=0: 3 */
  int state_quota_mb;     /* <=0: 512; per-app state directory limit */
  bd_session_timeouts timeouts; /* zero fields use defaults */
  bd_session_perf_ops perf;     /* optional */
  bd_session_event_fn on_event;
  void *event_user;
} bd_session_config;

typedef struct bd_session bd_session;

typedef struct bd_session_app_info {
  char app_id[BD_APP_ID_MAX];
  char name[BD_APP_NAME_MAX];
  int pid;
  bd_session_state state;
  bool ipc_connected;
  bool stopped_by_session; /* stopped with SIGSTOP (fast-resume slot) */
  bool frozen;             /* light-sleep stop held (confirmed or thaw-retry) */
  bool display_released;
  bool background;         /* state == BD_SESSION_BACKGROUND */
  bool exited;
  bool crashed;
  uint64_t started_ms;
  uint64_t backgrounded_ms;
  uint64_t last_active_ms;
  int exit_code;
  int term_signal;
} bd_session_app_info;

typedef struct bd_session_eviction {
  char app_id[BD_APP_ID_MAX];
  int pid;
  uint64_t backgrounded_ms;
} bd_session_eviction;

bd_session *bd_session_create(const bd_session_config *config,
                              char *error, size_t error_size);
/* Kills and reaps any remaining app, unlinks sockets and calls perf restore. */
void bd_session_destroy(bd_session *session);

/*
 * Starts `app` in an independent process group and injects BRICKDESK_*.
 * Only one foreground app is allowed; background an existing one first.
 * The instance starts in LAUNCHING and becomes FOREGROUND when app.hello
 * arrives (apps with no IPC hooks become foreground immediately).
 */
bd_session_result bd_session_launch(bd_session *session, const bd_app *app,
                                    char *error, size_t error_size);

/*
 * Requests the foreground app be suspended.  Returns NEEDS_CONFIRM without
 * changing anything when max_background background apps already exist; the
 * caller may evict bd_session_suggest_eviction() and retry.  The session
 * never kills an app on its own here.
 */
bd_session_result bd_session_background(bd_session *session,
                                        const char *app_id);
bd_session_result bd_session_resume(bd_session *session, const char *app_id);
/* Message for the last refused launch/resume, or "" when none. */
const char *bd_session_last_error(const bd_session *session);

/*
 * RAM-only light-sleep freeze of the foreground app.
 *
 * bd_session_freeze() sends SIGSTOP to the instance's process group and waits
 * up to timeouts.freeze_ms for waitpid(WUNTRACED) to report that the managed
 * leader stopped.  It returns BD_SESSION_OK only after that confirmation; the
 * instance stays FOREGROUND, with the same app id, pid and perf lease.
 * Freezing does not emit a STATE/BACKGROUND event, does not change the
 * background count or limit, and does not run app.pause, state.save or any
 * other IPC hook.  It preserves RAM only: it is not power-loss protection and
 * the caller must not treat it as app.pause/save.  Calling it again while
 * frozen returns BD_SESSION_OK.
 *
 * bd_session_thaw() sends SIGCONT only for an instance this session froze.  It
 * never resumes an instance stopped externally, by the background path or by
 * another lifecycle action.  Thawing a running foreground instance returns
 * BD_SESSION_OK without signalling.  If SIGCONT is rejected, the freeze
 * ownership is retained and the call returns an error; a later thaw retries
 * it.  Ownership is cleared only after SIGCONT is delivered or the instance
 * has exited.
 *
 * Both calls return BD_SESSION_INVALID_STATE when the app is not a running
 * foreground instance (including one stopped externally).  freeze() returns
 * BD_SESSION_BUSY while the app is launching, suspending, resuming or
 * terminating, and thaw() returns BD_SESSION_BUSY if a hook/state transfer is
 * pending.  background() and terminate() continue a frozen instance before
 * their normal hooks; if that continuation is rejected they return an error
 * without changing the lifecycle state, so the call can be retried.  A failed
 * freeze whose rollback SIGCONT is rejected leaves the instance frozen for a
 * thaw() retry instead of claiming it resumed.  destroy() kills a frozen
 * instance, and poll() keeps reaping exits while it is frozen.
 */
bd_session_result bd_session_freeze(bd_session *session, const char *app_id);
bd_session_result bd_session_thaw(bd_session *session, const char *app_id);

/*
 * Termination order: declared save hook, declared quit hook, SIGTERM after
 * the hook timeout, SIGKILL after term_ms.  A backgrounded SIGSTOP app is
 * continued first so its IPC hooks can run.
 */
bd_session_result bd_session_terminate(bd_session *session,
                                       const char *app_id);

/* Queues state.save / state.load; completion arrives as EVENT_HOOK. */
bd_session_result bd_session_save_state(bd_session *session,
                                        const char *app_id, int slot);
bd_session_result bd_session_load_state(bd_session *session,
                                        const char *app_id, int slot);

/*
 * Drives waitpid, protocol I/O and hook timers.  Returns the number of
 * milliseconds until the next timer, or -1 when nothing is scheduled.
 * Call it at least every few tens of milliseconds while an app is launching,
 * suspending, resuming or terminating.
 */
int bd_session_poll(bd_session *session);

size_t bd_session_running_count(const bd_session *session);
bool bd_session_running_at(const bd_session *session, size_t index,
                           bd_session_app_info *out);
bool bd_session_find(const bd_session *session, const char *app_id,
                     bd_session_app_info *out);
/* Manifest of a tracked instance (also valid for exited instances). */
const bd_app *bd_session_manifest(const bd_session *session,
                                  const char *app_id);
/*
 * Appends the session's pollable descriptors (menu socket, menu clients and
 * app protocol sockets) to `out`. Returns how many were written. The desktop
 * loop polls these so a menu action or app message wakes it immediately.
 */
int bd_session_fds(const bd_session *session, int *out, int max);
/* Last-frame file for a tracked instance, or NULL when it has none. */
const char *bd_session_last_frame_path(const bd_session *session,
                                       const char *app_id);
/* Removes exited/crashed instances from the list; returns how many. */
size_t bd_session_clear_terminated(bd_session *session);

/* Oldest background app (LRU), used after NEEDS_CONFIRM. */
bool bd_session_suggest_eviction(const bd_session *session,
                                 bd_session_eviction *out);

int bd_session_background_count(const bd_session *session);
int bd_session_max_background(const bd_session *session);

/*
 * Path of the menu control socket injected as BRICKDESK_MENU_SOCKET. The
 * shim connects there and sends {"type":"menu","app":"...","action":"..."};
 * the session turns it into BD_SESSION_EVENT_MENU_ACTION. Returns NULL when
 * the socket could not be created (the overlay still works, but return to
 * desktop and quit have to fall back).
 */
const char *bd_session_menu_socket(const bd_session *session);

const char *bd_session_state_name(bd_session_state state);
const char *bd_session_result_name(bd_session_result result);

#endif /* BRICKDESK_APP_H */
