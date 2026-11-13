#ifndef BRICKDESK_PLUGIN_H
#define BRICKDESK_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "brickdesk/gfx.h"
#include "brickdesk/theme.h"
#include "brickdesk/ui.h"

typedef struct bd_plugin_ui bd_plugin_ui;
typedef struct bd_plugin_frame bd_plugin_frame;

/*
 * Out-of-process plugin host and client.
 *
 * The host scans plugin directories, starts plugins on demand, speaks
 * newline-delimited JSON over a Unix stream socket, renders their declarative
 * UI, and reads dynamic tiles from a shared-memory RGBA surface. Plugins never
 * share the desktop address space, so a crash or a busy loop only affects that
 * plugin's tile.
 *
 * Protocol documentation: docs/plugin-api.md.
 */

#define BD_PLUGIN_API_VERSION 1
#define BD_PLUGIN_MAX_PLUGINS 16
#define BD_PLUGIN_MAX_PATH 512
#define BD_PLUGIN_MAX_ID 64
#define BD_PLUGIN_MAX_NAME 64
#define BD_PLUGIN_MAX_VERSION 32
#define BD_PLUGIN_MAX_ENTRY_ARGS 8
#define BD_PLUGIN_MAX_PERMISSIONS 8
#define BD_PLUGIN_MAX_SETTINGS 8
#define BD_PLUGIN_MAX_MENU_ITEMS 8
#define BD_PLUGIN_MAX_SETTING_VALUE 64
#define BD_PLUGIN_MAX_DIAGNOSTICS 32
#define BD_PLUGIN_LINE_MAX 65536
#define BD_PLUGIN_UI_JSON_MAX 49152
#define BD_PLUGIN_UI_MAX_NODES 96
#define BD_PLUGIN_UI_MAX_DEPTH 8
#define BD_PLUGIN_UI_MAX_OPTIONS 64
#define BD_PLUGIN_UI_MAX_CHILDREN 24
#define BD_PLUGIN_TILE_MAX_DIM 400
#define BD_PLUGIN_TILE_MAX_FPS 15
#define BD_PLUGIN_FRAME_MAGIC 0x4c544442u /* "BDTL" little-endian */
#define BD_PLUGIN_FRAME_VERSION 1u
#define BD_PLUGIN_FRAME_FORMAT_RGBA8888 1u

/* Capability bits from the manifest's "capabilities" array. */
#define BD_PLUGIN_CAP_TILE (1u << 0)
#define BD_PLUGIN_CAP_MENU (1u << 1)
#define BD_PLUGIN_CAP_SETTINGS (1u << 2)
#define BD_PLUGIN_CAP_HOTKEY (1u << 3)
#define BD_PLUGIN_CAP_BACKGROUND (1u << 4)

typedef enum bd_plugin_setting_type {
  BD_PLUGIN_SETTING_STRING = 0,
  BD_PLUGIN_SETTING_BOOL,
  BD_PLUGIN_SETTING_INT,
  BD_PLUGIN_SETTING_FLOAT
} bd_plugin_setting_type;

typedef struct bd_plugin_setting_spec {
  char key[40];
  char label[BD_PLUGIN_MAX_NAME];
  bd_plugin_setting_type type;
  char default_value[BD_PLUGIN_MAX_SETTING_VALUE];
} bd_plugin_setting_spec;

typedef struct bd_plugin_menu_item {
  char id[40];
  char label[BD_PLUGIN_MAX_NAME];
} bd_plugin_menu_item;

typedef struct bd_plugin_tile_spec {
  int w; /* grid cells, 1 or 2 */
  int h;
  int refresh_hz; /* 1..15 */
} bd_plugin_tile_spec;

typedef struct bd_plugin_manifest {
  char id[BD_PLUGIN_MAX_ID + 1];
  char name[BD_PLUGIN_MAX_NAME + 1];
  char version[BD_PLUGIN_MAX_VERSION + 1];
  char runtime[16];
  char dir[BD_PLUGIN_MAX_PATH];
  int api;
  bool autostart;
  unsigned capabilities;
  char entry[BD_PLUGIN_MAX_ENTRY_ARGS][BD_PLUGIN_MAX_PATH];
  int entry_count;
  char permissions[BD_PLUGIN_MAX_PERMISSIONS][48];
  int permission_count;
  bd_plugin_tile_spec tile;
  bd_plugin_menu_item menu[BD_PLUGIN_MAX_MENU_ITEMS];
  int menu_count;
  bd_plugin_setting_spec settings[BD_PLUGIN_MAX_SETTINGS];
  int setting_count;
  size_t memory_limit_bytes;
  int nofile_limit;
  int nice_value;
} bd_plugin_manifest;

typedef struct bd_plugin_limits {
  int handshake_timeout_ms;
  int ping_interval_ms;
  int ping_timeout_ms;
  int idle_stop_ms;
  int shutdown_timeout_ms;
  int restart_base_ms;
  int restart_max_ms;
  int restart_window_ms;
  int restart_limit;
} bd_plugin_limits;

bd_plugin_limits bd_plugin_default_limits(void);

typedef struct bd_plugin_host_config {
  const char *plugin_dir;  /* scanned directory; NULL uses BRICKDESK_PLUGIN_DIR */
  const char *data_dir;    /* per-plugin writable data root */
  const char *runtime_dir; /* sockets and shared-memory files */
  const bd_plugin_limits *limits; /* NULL uses bd_plugin_default_limits() */
  int display_width;
  int display_height;
  bool dark;
  uint32_t accent; /* 0xRRGGBB */
  const char *locale;
  bool verbose;
  /*
   * Host-owned state root for persisted plugin settings. NULL or empty uses
   * BRICKDESK_STATE_DIR from the environment, then the build-time state root.
   * The host appends "plugin-settings/<plugin-id>/settings.json"; plugin
   * package directories are never used for state.
   */
  const char *state_dir;
} bd_plugin_host_config;

typedef enum bd_plugin_state {
  BD_PLUGIN_DISCOVERED = 0,
  BD_PLUGIN_STARTING,
  BD_PLUGIN_HANDSHAKING,
  BD_PLUGIN_RUNNING,
  BD_PLUGIN_STOPPING,
  BD_PLUGIN_BACKOFF,
  BD_PLUGIN_QUARANTINED,
  BD_PLUGIN_STOPPED
} bd_plugin_state;

const char *bd_plugin_state_name(bd_plugin_state state);

typedef struct bd_plugin_stats {
  int pid;
  bd_plugin_state state;
  int starts;
  int restarts;
  int crashes;
  int protocol_errors;
  int handshake_timeouts;
  int last_exit_status;
  int ui_generation;
  uint32_t frame_seq;
  int rss_kb;
} bd_plugin_stats;

/*
 * Shared-memory frame surface. The host creates the file and passes its path
 * in host.hello. The plugin maps it, fills the header and pixels, then writes
 * seq last with release ordering.
 */
typedef struct bd_plugin_frame_header {
  uint32_t magic;   /* BD_PLUGIN_FRAME_MAGIC */
  uint32_t version; /* BD_PLUGIN_FRAME_VERSION */
  uint32_t width;
  uint32_t height;
  uint32_t stride;  /* bytes per row, width * 4 */
  uint32_t format;  /* BD_PLUGIN_FRAME_FORMAT_RGBA8888 */
  uint32_t seq;     /* incremented after every completed frame */
  uint32_t reserved;
} bd_plugin_frame_header;

struct bd_plugin_frame {
  int width;
  int height;
  int stride;
  uint32_t seq;
  const uint8_t *rgba;
};

typedef struct bd_plugin_host bd_plugin_host;
typedef struct bd_plugin bd_plugin;

bd_plugin_host *bd_plugin_host_create(const bd_plugin_host_config *config);
void bd_plugin_host_destroy(bd_plugin_host *host);

/*
 * Scans plugin_dir for <id>/plugin.json. Invalid plugins are skipped and
 * recorded as diagnostics. Returns the number of valid plugins, or -1 when
 * the directory cannot be read (error is filled in).
 */
int bd_plugin_host_scan(bd_plugin_host *host, char *error, size_t error_size);
size_t bd_plugin_host_count(const bd_plugin_host *host);
bd_plugin *bd_plugin_host_at(bd_plugin_host *host, size_t index);
bd_plugin *bd_plugin_host_find(bd_plugin_host *host, const char *id);
const bd_plugin_manifest *bd_plugin_host_manifest(const bd_plugin_host *host,
                                                   size_t index);
size_t bd_plugin_host_diagnostic_count(const bd_plugin_host *host);
/* Copies one diagnostic line; returns false for an invalid index. */
bool bd_plugin_host_diagnostic(const bd_plugin_host *host, size_t index,
                               char *out, size_t out_size);

/*
 * Runs lifecycle timers and non-blocking socket IO. timeout_ms follows poll:
 * 0 returns immediately, -1 waits for the next host timer. Returns the number
 * of sockets or timers handled, or -1 on a poll failure. Never blocks on a
 * plugin's stdout or writes.
 */
int bd_plugin_host_poll(bd_plugin_host *host, int timeout_ms);
/* Milliseconds until the next timer, or -1. Visible dynamic tiles cap it. */
int bd_plugin_host_next_timeout(const bd_plugin_host *host);
/*
 * Appends every plugin listen and IPC socket to `out`. Returns how many were
 * written; the desktop loop polls these and calls bd_plugin_host_poll(host, 0)
 * when one becomes ready.
 */
int bd_plugin_host_fds(const bd_plugin_host *host, int *out, int max);
/* Consumes the "visual state changed" flag (UI, frame, run state). */
bool bd_plugin_host_take_dirty(bd_plugin_host *host);
void bd_plugin_host_stop_all(bd_plugin_host *host);
const char *bd_plugin_host_runtime_dir(const bd_plugin_host *host);
/*
 * Updates the theme sent in later host.hello messages and notifies running
 * plugins with theme.changed.  The desktop uses this when the settings page
 * changes the accent; the tile renderer owns its already-uploaded colours.
 */
void bd_plugin_host_set_theme(bd_plugin_host *host, bool dark, uint32_t accent);

const bd_plugin_manifest *bd_plugin_manifest_of(const bd_plugin *plugin);
const char *bd_plugin_id(const bd_plugin *plugin);
bd_plugin_state bd_plugin_state_of(const bd_plugin *plugin);
bd_plugin_stats bd_plugin_get_stats(const bd_plugin *plugin);

/* Starts the plugin if it is stopped/backing off. False when quarantined. */
bool bd_plugin_activate(bd_plugin *plugin);
/*
 * Visibility drives on-demand start, idle stop and tile.visible events. A
 * plugin with the background capability is not stopped when invisible.
 */
bool bd_plugin_set_tile_visible(bd_plugin *plugin, bool visible);
bool bd_plugin_set_settings_open(bd_plugin *plugin, bool open);

const bd_plugin_ui *bd_plugin_ui_tile(const bd_plugin *plugin);
const bd_plugin_ui *bd_plugin_ui_settings(const bd_plugin *plugin);
/* Increments on every accepted plugin.ui.set. */
int bd_plugin_ui_generation(const bd_plugin *plugin);

const char *bd_plugin_get_setting(const bd_plugin *plugin, const char *key);
/*
 * Updates the value and notifies the plugin with settings.changed. False means
 * the value was rejected or not applied. If the new file was already renamed
 * into place but its directory sync failed, the value stays applied, the host
 * records a diagnostic, and the protocol request reports {"durable":false}.
 */
bool bd_plugin_set_setting(bd_plugin *plugin, const char *key,
                           const char *value);
/* Sends host.event {"type":type, ...extra}. extra_json may be NULL. */
bool bd_plugin_send_event(bd_plugin *plugin, const char *type,
                          const char *extra_json);
/* Sends ui.activate for a button/toggle/slider/select node id. */
bool bd_plugin_activate_node(bd_plugin *plugin, const char *node_id,
                             const char *event);

/* Shared-memory RGBA surface. Valid once the plugin has started. */
const char *bd_plugin_tile_surface_path(const bd_plugin *plugin);
/* Current frame sequence in the surface, or 0 when none is valid. */
uint32_t bd_plugin_tile_seq(const bd_plugin *plugin);
/*
 * Reads the shared-memory header and returns true once per new sequence
 * number, rate-limited to the plugin's refresh_hz (max 15 fps). The pixel
 * pointer stays valid until the next consume or host_destroy.
 */
bool bd_plugin_tile_consume(bd_plugin *plugin, bd_plugin_frame *out);

/* Clears the crash history and leaves quarantine; the next activate starts. */
void bd_plugin_reset_crash_limit(bd_plugin *plugin);

/* --- declarative UI ---------------------------------------------------- */

typedef enum bd_plugin_ui_node_type {
  BD_PLUGIN_UI_COLUMN = 0,
  BD_PLUGIN_UI_ROW,
  BD_PLUGIN_UI_TEXT,
  BD_PLUGIN_UI_ICON,
  BD_PLUGIN_UI_PROGRESS,
  BD_PLUGIN_UI_TOGGLE,
  BD_PLUGIN_UI_SLIDER,
  BD_PLUGIN_UI_SELECT,
  BD_PLUGIN_UI_BUTTON,
  BD_PLUGIN_UI_SPACER
} bd_plugin_ui_node_type;

typedef struct bd_plugin_ui_node {
  bd_plugin_ui_node_type type;
  int children[BD_PLUGIN_UI_MAX_CHILDREN]; /* indices into nodes */
  int child_count;
  char id[40];
  char text[128];  /* text / label / value */
  char style[16];  /* title, body, caption, mono */
  char action[40];
  char icon[40];   /* icon node name */
  char value_text[32]; /* select value */
  float value;
  float min_value;
  float max_value;
  float step;
  bool boolean;
  int padding;
  int gap;
  int first_option;
  int option_count;
} bd_plugin_ui_node;

struct bd_plugin_ui {
  bd_plugin_ui_node nodes[BD_PLUGIN_UI_MAX_NODES];
  int node_count;
  char options[BD_PLUGIN_UI_MAX_OPTIONS][32];
  int option_count;
  int root; /* -1 when the tree is empty */
};

/*
 * Parses and validates one node tree. Rejects unknown node types, trees
 * deeper than BD_PLUGIN_UI_MAX_DEPTH, more than BD_PLUGIN_UI_MAX_NODES nodes,
 * oversized strings, missing required fields and non-finite numbers.
 */
bool bd_plugin_ui_parse(const char *json, size_t length, bd_plugin_ui *out,
                        char *error, size_t error_size);

/* --- declarative UI rendering (desktop side) --------------------------- */

typedef struct bd_plugin_render_style {
  const bd_ui_fonts *fonts;
  bd_accent_palette accent;
  bool dark;
} bd_plugin_render_style;

/*
 * Uploads a new shared-memory frame and returns true when the tile needs a
 * redraw (new frame, new UI tree, or run state change). Call before
 * bd_plugin_tile_draw and queue damage for rect on true.
 */
bool bd_plugin_tile_begin_frame(bd_plugin *plugin, bd_gfx *gfx);
void bd_plugin_tile_draw(bd_plugin *plugin, bd_gfx *gfx, bd_rect rect,
                         const bd_plugin_render_style *style);
/* Frees the cached tile texture; call while the GL context is alive. */
void bd_plugin_tile_release(bd_plugin *plugin, bd_gfx *gfx);
/* Renders a declarative tree, including settings controls. */
void bd_plugin_ui_draw(const bd_plugin_ui *ui, bd_gfx *gfx, bd_rect rect,
                       const bd_plugin_render_style *style, int focused_node,
                       int scroll_y);
/* Height the tree needs at `width`, including its padding and gaps. */
int bd_plugin_ui_measure(const bd_plugin_ui *ui, int width);
/*
 * Rect of `node` inside the tree at `width`, relative to the tree origin
 * (0,0) and using the same layout as bd_plugin_ui_draw().  The caller adds
 * the draw rect origin and subtracts its scroll offset.  False when the node
 * is not part of the tree.
 */
bool bd_plugin_ui_node_rect(const bd_plugin_ui *ui, int node, int width,
                            bd_rect *out);

/* --- client SDK (used by C plugins and test fixtures) ------------------ */

typedef struct bd_plugin_client bd_plugin_client;
/* Called for host.event/host.action; params_json is valid during the call. */
typedef void (*bd_plugin_client_event_fn)(void *user, const char *type,
                                          const char *params_json);

bd_plugin_client *bd_plugin_client_connect(const char *socket_path,
                                           int timeout_ms);
void bd_plugin_client_destroy(bd_plugin_client *client);
bool bd_plugin_client_ready(const bd_plugin_client *client);
const char *bd_plugin_client_surface_path(const bd_plugin_client *client);
const char *bd_plugin_client_error(const bd_plugin_client *client);
bool bd_plugin_client_shutdown_requested(const bd_plugin_client *client);
void bd_plugin_client_set_event_handler(bd_plugin_client *client,
                                        bd_plugin_client_event_fn fn,
                                        void *user);
/* Reads and dispatches host messages; returns messages read, 0 on timeout. */
int bd_plugin_client_poll(bd_plugin_client *client, int timeout_ms);
bool bd_plugin_client_set_ui(bd_plugin_client *client, const char *tile_json,
                             const char *settings_json);
bool bd_plugin_client_tile_frame(bd_plugin_client *client, uint32_t seq);
bool bd_plugin_client_notify(bd_plugin_client *client, const char *method,
                             const char *params_json);
bool bd_plugin_client_log(bd_plugin_client *client, const char *level,
                          const char *message);

#endif /* BRICKDESK_PLUGIN_H */
