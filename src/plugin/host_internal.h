#ifndef BRICKDESK_PLUGIN_HOST_INTERNAL_H
#define BRICKDESK_PLUGIN_HOST_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/un.h>

#include "brickdesk/plugin.h"

#define BD_PLUGIN_MAX_CRASHES 8
#define BD_PLUGIN_OUTPUT_MAX (BD_PLUGIN_LINE_MAX * 2)

struct bd_plugin {
  bd_plugin_manifest manifest;
  bd_plugin_host *host;
  int index;
  bd_plugin_state state;
  int pid;
  int listen_fd;
  int fd;
  char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  char shm_path[BD_PLUGIN_MAX_PATH];
  int shm_fd;
  bd_plugin_frame_header *frame_header;
  size_t frame_size;

  uint32_t last_frame_seq;
  uint32_t announced_seq;
  uint64_t last_frame_read_ms;
  bool frame_clock_started;
  uint64_t started_ms;
  uint64_t running_since_ms;
  uint64_t handshake_deadline_ms;
  uint64_t restart_at_ms;
  uint64_t last_rx_ms;
  uint64_t last_ping_ms;
  uint64_t stop_deadline_ms;
  uint64_t visible_since_ms;
  uint64_t idle_deadline_ms;
  int next_id;
  bool visible;
  bool settings_open;
  bool stop_graceful;
  bool background;

  uint64_t crash_times[BD_PLUGIN_MAX_CRASHES];
  int crash_head;
  int crash_count;
  int starts;
  int restarts;
  int crashes;
  int protocol_errors;
  int handshake_timeouts;
  int last_exit_status;
  int restart_attempt;

  char input[BD_PLUGIN_LINE_MAX + 1];
  size_t input_length;
  char output[BD_PLUGIN_OUTPUT_MAX];
  size_t output_length;

  bool has_tile_ui;
  bool has_settings_ui;
  bd_plugin_ui ui_tile;
  bd_plugin_ui ui_settings;
  int ui_generation;
  char settings_values[BD_PLUGIN_MAX_SETTINGS][BD_PLUGIN_MAX_SETTING_VALUE];

  /* Rendering cache owned by src/plugin/render.c. */
  bd_texture *texture;
  int texture_w;
  int texture_h;
  uint32_t texture_seq;
  int render_ui_generation;
  bd_plugin_state render_state;
  bool render_initialized;
};

struct bd_plugin_host {
  char plugin_dir[BD_PLUGIN_MAX_PATH];
  char data_dir[BD_PLUGIN_MAX_PATH];
  char runtime_dir[BD_PLUGIN_MAX_PATH];
  char state_dir[BD_PLUGIN_MAX_PATH];
  char locale[32];
  bd_plugin_limits limits;
  int display_width;
  int display_height;
  bool dark;
  uint32_t accent;
  bool verbose;

  bd_plugin *plugins;
  int count;

  char diagnostics[BD_PLUGIN_MAX_DIAGNOSTICS][256];
  int diagnostic_count;

  bool dirty;
  bool shutting_down;
};

/* Appends a diagnostic (also printed when verbose). */
void bd_plugin_diagnostic(bd_plugin_host *host, const char *format, ...);
void bd_plugin_mark_dirty(bd_plugin_host *host);
/* Implemented in host.c; used by render.c. */
void bd_plugin_log(const bd_plugin *plugin, const char *format, ...);

#endif /* BRICKDESK_PLUGIN_HOST_INTERNAL_H */
