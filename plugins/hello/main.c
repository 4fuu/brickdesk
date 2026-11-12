#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "brickdesk/plugin.h"

#define FRAME_SIZE 168

static volatile sig_atomic_t g_stop;

static void on_signal(int signal_number) {
  (void)signal_number;
  g_stop = 1;
}

static void tile_json(char *out, size_t out_size, bool show_meta) {
  snprintf(out, out_size,
           "{\"type\":\"column\",\"padding\":14,\"gap\":8,\"children\":["
           "{\"type\":\"row\",\"gap\":8,\"children\":["
           "{\"type\":\"icon\",\"name\":\"hello\"},"
           "{\"type\":\"text\",\"text\":\"你好插件\",\"style\":\"title\"}]},"
           "%s"
           "{\"type\":\"progress\",\"value\":62,\"max\":100},"
           "{\"type\":\"text\",\"text\":\"来自 C 插件\",\"style\":\"caption\"}]}",
           show_meta ? "{\"type\":\"text\",\"text\":\"独立进程 · JSON IPC\","
                       "\"style\":\"caption\"},"
                     : "");
}

static const char *SETTINGS_JSON =
    "{\"type\":\"column\",\"padding\":20,\"gap\":14,\"children\":["
    "{\"type\":\"text\",\"text\":\"你好插件设置\",\"style\":\"title\"},"
    "{\"type\":\"text\",\"text\":\"这些控件由宿主渲染，插件只发送声明。\","
    "\"style\":\"caption\"},"
    "{\"type\":\"toggle\",\"id\":\"show_meta\",\"label\":\"显示副标题\","
    "\"value\":true},"
    "{\"type\":\"slider\",\"id\":\"pace\",\"label\":\"节奏\","
    "\"value\":3,\"min\":1,\"max\":5,\"step\":1},"
    "{\"type\":\"select\",\"id\":\"mode\",\"label\":\"模式\","
    "\"value\":\"calm\",\"options\":[\"calm\",\"bright\",\"night\"]},"
    "{\"type\":\"button\",\"id\":\"refresh\",\"label\":\"刷新\","
    "\"action\":\"refresh\"},"
    "{\"type\":\"text\",\"text\":\"控件由宿主渲染，事件回传插件。\","
    "\"style\":\"caption\"}]}";

static bool map_frame(const char *path, bd_plugin_frame_header **header,
                      size_t *size) {
  if (path == NULL || path[0] == '\0') {
    return false;
  }
  size_t bytes = sizeof(bd_plugin_frame_header) +
                 (size_t)FRAME_SIZE * FRAME_SIZE * 4u;
  int fd = open(path, O_RDWR);
  if (fd < 0) {
    fprintf(stderr, "hello: cannot open tile surface %s\n", path);
    return false;
  }
  void *mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (mapping == MAP_FAILED) {
    fprintf(stderr, "hello: cannot map tile surface\n");
    return false;
  }
  *header = (bd_plugin_frame_header *)mapping;
  *size = bytes;
  return true;
}

static void paint_frame(bd_plugin_frame_header *header) {
  header->magic = BD_PLUGIN_FRAME_MAGIC;
  header->version = BD_PLUGIN_FRAME_VERSION;
  header->width = FRAME_SIZE;
  header->height = FRAME_SIZE;
  header->stride = FRAME_SIZE * 4;
  header->format = BD_PLUGIN_FRAME_FORMAT_RGBA8888;
  uint8_t *pixels = (uint8_t *)(header + 1);
  for (int y = 0; y < FRAME_SIZE; ++y) {
    for (int x = 0; x < FRAME_SIZE; ++x) {
      float tx = (float)x / (float)(FRAME_SIZE - 1);
      float ty = (float)y / (float)(FRAME_SIZE - 1);
      float circle_x = tx - 0.72f;
      float circle_y = ty - 0.28f;
      float glow = 1.0f - (circle_x * circle_x + circle_y * circle_y) * 4.0f;
      if (glow < 0.0f) {
        glow = 0.0f;
      }
      uint8_t *pixel = pixels + ((size_t)y * FRAME_SIZE + (size_t)x) * 4u;
      pixel[0] = (uint8_t)(38.0f + 150.0f * tx * ty + 30.0f * glow);
      pixel[1] = (uint8_t)(34.0f + 80.0f * tx + 46.0f * glow);
      pixel[2] = (uint8_t)(44.0f + 96.0f * ty + 20.0f * glow);
      pixel[3] = 255;
    }
  }
  __atomic_store_n(&header->seq, 1u, __ATOMIC_RELEASE);
}

static void on_event(void *user, const char *type, const char *params_json) {
  bool *show_meta = (bool *)user;
  if (strcmp(type, "settings.changed") != 0) {
    return;
  }
  if (strstr(params_json, "\"key\":\"show_meta\"") == NULL) {
    return;
  }
  *show_meta = strstr(params_json, "\"value\":false") == NULL;
}

int main(int argc, char **argv) {
  const char *socket_path = NULL;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
      socket_path = argv[++i];
    }
  }
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);

  bd_plugin_client *client = bd_plugin_client_connect(socket_path, 5000);
  if (client == NULL) {
    fprintf(stderr, "hello: cannot create client\n");
    return 1;
  }
  if (!bd_plugin_client_ready(client)) {
    fprintf(stderr, "hello: handshake failed: %s\n",
            bd_plugin_client_error(client));
    bd_plugin_client_destroy(client);
    return 1;
  }
  fprintf(stderr, "hello: connected\n");

  bool show_meta = true;
  bd_plugin_frame_header *header = NULL;
  size_t frame_size = 0;
  bool mapped = map_frame(bd_plugin_client_surface_path(client), &header,
                          &frame_size);
  if (mapped) {
    paint_frame(header);
    bd_plugin_client_tile_frame(client, 1);
  }
  char tile[2048];
  tile_json(tile, sizeof(tile), show_meta);
  bd_plugin_client_set_ui(client, tile, SETTINGS_JSON);
  bd_plugin_client_set_event_handler(client, on_event, &show_meta);

  while (!g_stop && !bd_plugin_client_shutdown_requested(client)) {
    int result = bd_plugin_client_poll(client, 500);
    if (result < 0) {
      break;
    }
    static bool last_show_meta = true;
    if (show_meta != last_show_meta) {
      last_show_meta = show_meta;
      tile_json(tile, sizeof(tile), show_meta);
      bd_plugin_client_set_ui(client, tile, NULL);
      bd_plugin_client_log(client, "info", show_meta ? "meta shown" : "meta hidden");
    }
  }

  if (mapped) {
    munmap(header, frame_size);
  }
  bd_plugin_client_destroy(client);
  return 0;
}
