#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "brickdesk/plugin.h"
#include "ipc.h"

#define FRAME_SIZE 168

static uint64_t now_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void sleep_ms(int milliseconds) {
  struct timespec delay;
  delay.tv_sec = milliseconds / 1000;
  delay.tv_nsec = (long)(milliseconds % 1000) * 1000 * 1000;
  nanosleep(&delay, NULL);
}

static int raw_connect(void) {
  const char *path = getenv("BRICKDESK_PLUGIN_SOCKET");
  char error[128] = {0};
  if (path == NULL) {
    return -1;
  }
  return bd_plugin_connect_socket(path, 5000, error, sizeof(error));
}

static int read_line(int fd, char *buffer, size_t capacity) {
  size_t used = 0;
  while (used + 1 < capacity) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int ready = poll(&pfd, 1, 5000);
    if (ready <= 0) {
      return -1;
    }
    char c = '\0';
    ssize_t received = recv(fd, &c, 1, 0);
    if (received <= 0) {
      return -1;
    }
    if (c == '\n') {
      buffer[used] = '\0';
      return (int)used;
    }
    buffer[used++] = c;
  }
  return -1;
}

static bool map_frame(const char *path, bd_plugin_frame_header **header,
                      size_t *size) {
  if (path == NULL || path[0] == '\0') {
    return false;
  }
  size_t bytes = sizeof(bd_plugin_frame_header) +
                 (size_t)FRAME_SIZE * FRAME_SIZE * 4u;
  int fd = open(path, O_RDWR);
  if (fd < 0) {
    return false;
  }
  void *mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (mapping == MAP_FAILED) {
    return false;
  }
  *header = (bd_plugin_frame_header *)mapping;
  *size = bytes;
  return true;
}

static void paint_frame(bd_plugin_frame_header *header, uint32_t seq) {
  header->magic = BD_PLUGIN_FRAME_MAGIC;
  header->version = BD_PLUGIN_FRAME_VERSION;
  header->width = FRAME_SIZE;
  header->height = FRAME_SIZE;
  header->stride = FRAME_SIZE * 4;
  header->format = BD_PLUGIN_FRAME_FORMAT_RGBA8888;
  uint8_t *pixels = (uint8_t *)(header + 1);
  for (int y = 0; y < FRAME_SIZE; ++y) {
    for (int x = 0; x < FRAME_SIZE; ++x) {
      uint8_t *pixel = pixels + ((size_t)y * FRAME_SIZE + (size_t)x) * 4u;
      pixel[0] = (uint8_t)(40 + x / 2);
      pixel[1] = (uint8_t)(50 + y / 2);
      pixel[2] = (uint8_t)(70 + (x + y) / 4);
      pixel[3] = 255;
    }
  }
  __atomic_store_n(&header->seq, seq, __ATOMIC_RELEASE);
}

typedef struct fixture_state {
  bd_plugin_client *client;
  bool activated;
} fixture_state;

static const char *LONG_TILE_JSON =
    "{\"type\":\"column\",\"padding\":12,\"gap\":6,\"children\":["
    "{\"type\":\"row\",\"gap\":8,\"children\":["
    "{\"type\":\"icon\",\"name\":\"long\"},"
    "{\"type\":\"text\",\"text\":\"Long fixture\",\"style\":\"title\"}]},"
    "{\"type\":\"text\",\"text\":\"many settings\",\"style\":\"caption\"}]}";

static bool long_settings_json(char *out, size_t size, bool activated) {
  int used = snprintf(
      out, size,
      "{\"type\":\"column\",\"padding\":16,\"gap\":10,\"children\":["
      "{\"type\":\"text\",\"text\":\"Long fixture settings\",\"style\":\"title\"},");
  if (used < 0) {
    return false;
  }
  for (int i = 0; i < 16; ++i) {
    int written;
    if (i == 15) {
      written = snprintf(out + used, size - (size_t)used,
                         "{\"type\":\"button\",\"id\":\"last_button\","
                         "\"label\":\"Apply last\",\"action\":\"apply_last\"}");
    } else if (i % 3 == 0) {
      written = snprintf(out + used, size - (size_t)used,
                         "{\"type\":\"toggle\",\"id\":\"ctl_%d\","
                         "\"label\":\"Toggle %d\",\"value\":true},",
                         i, i);
    } else if (i % 3 == 1) {
      written = snprintf(out + used, size - (size_t)used,
                         "{\"type\":\"slider\",\"id\":\"ctl_%d\","
                         "\"label\":\"Slider %d\",\"value\":5,"
                         "\"min\":0,\"max\":10,\"step\":1},",
                         i, i);
    } else {
      written = snprintf(out + used, size - (size_t)used,
                         "{\"type\":\"select\",\"id\":\"ctl_%d\","
                         "\"label\":\"Select %d\",\"value\":\"a\","
                         "\"options\":[\"a\",\"b\"]},",
                         i, i);
    }
    if (written < 0 || (size_t)written >= size - (size_t)used) {
      return false;
    }
    used += written;
  }
  if (activated) {
    int written = snprintf(
        out + used, size - (size_t)used,
        ",{\"type\":\"text\",\"text\":\"Last control activated\"}");
    if (written < 0 || (size_t)written >= size - (size_t)used) {
      return false;
    }
    used += written;
  }
  int written = snprintf(out + used, size - (size_t)used, "]}");
  return written > 0 && (size_t)written < size - (size_t)used;
}

static bool set_long_ui(bd_plugin_client *client, bool activated) {
  char json[16384];
  if (client == NULL || !long_settings_json(json, sizeof(json), activated)) {
    return false;
  }
  return bd_plugin_client_set_ui(client, LONG_TILE_JSON, json);
}

static void on_event(void *user, const char *type, const char *params_json) {
  fixture_state *state = (fixture_state *)user;
  if (strcmp(type, "ui.activate") == 0 && state != NULL &&
      strstr(params_json, "\"node\":\"last_button\"") != NULL) {
    state->activated = true;
    set_long_ui(state->client, true);
  }
  if (strcmp(type, "settings.changed") != 0) {
    return;
  }
}

static int run_client_mode(const char *mode) {
  bd_plugin_client *client = bd_plugin_client_connect(NULL, 5000);
  if (client == NULL || !bd_plugin_client_ready(client)) {
    fprintf(stderr, "fixture %s: handshake failed: %s\n", mode,
            client != NULL ? bd_plugin_client_error(client) : "no client");
    bd_plugin_client_destroy(client);
    return 1;
  }
  fixture_state state;
  memset(&state, 0, sizeof(state));
  state.client = client;
  if (strcmp(mode, "crash") == 0) {
    _exit(3);
  }
  if (strcmp(mode, "badui") == 0) {
    const char *too_deep =
        "{\"type\":\"column\",\"children\":[{\"type\":\"column\",\"children\":["
        "{\"type\":\"column\",\"children\":[{\"type\":\"column\",\"children\":["
        "{\"type\":\"column\",\"children\":[{\"type\":\"column\",\"children\":["
        "{\"type\":\"column\",\"children\":[{\"type\":\"column\",\"children\":["
        "{\"type\":\"column\",\"children\":[{\"type\":\"text\",\"text\":\"x\"}]"
        "}]}]}]}]}]}]}]}]}";
    bd_plugin_client_notify(client, "plugin.ui.set", "{\"tile\":null}");
    char payload[2048];
    snprintf(payload, sizeof(payload), "{\"tile\":%s}", too_deep);
    bd_plugin_client_notify(client, "plugin.ui.set", payload);
  } else if (strcmp(mode, "alloc") == 0) {
    size_t bytes = 128u * 1024u * 1024u;
    unsigned char *memory = (unsigned char *)malloc(bytes);
    if (memory == NULL) {
      _exit(7);
    }
    for (size_t i = 0; i < bytes; i += 4096) {
      memory[i] = (unsigned char)i;
    }
    _exit(8);
  } else {
    bd_plugin_frame_header *header = NULL;
    size_t size = 0;
    if (map_frame(bd_plugin_client_surface_path(client), &header, &size)) {
      paint_frame(header, 10);
      bd_plugin_client_tile_frame(client, 10);
    }
    if (strcmp(mode, "long") == 0) {
      (void)set_long_ui(client, false);
    } else {
      bd_plugin_client_set_ui(
          client,
          "{\"type\":\"column\",\"padding\":12,\"gap\":6,\"children\":["
          "{\"type\":\"row\",\"gap\":8,\"children\":["
          "{\"type\":\"icon\",\"name\":\"fixture\"},"
          "{\"type\":\"text\",\"text\":\"Fixture\",\"style\":\"title\"}]},"
          "{\"type\":\"text\",\"text\":\"declarative tile\",\"style\":\"caption\"},"
          "{\"type\":\"progress\",\"value\":50,\"max\":100}]}",
          "{\"type\":\"column\",\"padding\":16,\"gap\":10,\"children\":["
          "{\"type\":\"text\",\"text\":\"Fixture settings\",\"style\":\"title\"},"
          "{\"type\":\"toggle\",\"id\":\"show_meta\",\"label\":\"Show meta\","
          "\"value\":true},"
          "{\"type\":\"slider\",\"id\":\"pace\",\"label\":\"Pace\",\"value\":3,"
          "\"min\":1,\"max\":5,\"step\":1},"
          "{\"type\":\"select\",\"id\":\"mode\",\"label\":\"Mode\","
          "\"value\":\"calm\",\"options\":[\"calm\",\"bright\"]}]}");
    }
    bd_plugin_client_set_event_handler(client, on_event, &state);
    if (header != NULL) {
      munmap(header, size);
    }
  }

  while (!bd_plugin_client_shutdown_requested(client)) {
    int result = bd_plugin_client_poll(client, 250);
    if (result < 0) {
      break;
    }
  }
  bd_plugin_client_destroy(client);
  return 0;
}

static int run_silent(void) {
  int fd = raw_connect();
  if (fd < 0) {
    return 1;
  }
  sleep_ms(10000);
  close(fd);
  return 0;
}

static int run_spin(void) {
  int fd = raw_connect();
  if (fd < 0) {
    return 1;
  }
  char line[4096];
  if (read_line(fd, line, sizeof(line)) < 0) {
    close(fd);
    return 1;
  }
  if (!bd_plugin_send_line(fd,
                           "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"pid\":1,"
                           "\"api\":1}}",
                           strlen("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{"
                                  "\"pid\":1,\"api\":1}}")) ) {
    close(fd);
    return 1;
  }
  uint64_t last_log = 0;
  unsigned counter = 0;
  volatile uint64_t spin = 1;
  for (;;) {
    for (volatile int i = 0; i < 50000; ++i) {
      spin = spin * 1103515245u + 12345u;
    }
    uint64_t now = now_ms();
    if (now - last_log < 20) {
      continue;
    }
    last_log = now;
    char message[128];
    int length = snprintf(message, sizeof(message),
                          "{\"jsonrpc\":\"2.0\",\"method\":\"plugin.log\","
                          "\"params\":{\"level\":\"info\",\"message\":\"spin %u\"}}",
                          counter++);
    if (length < 0 || !bd_plugin_send_line(fd, message, (size_t)length)) {
      break;
    }
  }
  close(fd);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: bd_plugin_fixture good|long|silent|crash|badui|spin|alloc\n");
    return 2;
  }
  const char *mode = argv[1];
  if (strcmp(mode, "silent") == 0) {
    return run_silent();
  }
  if (strcmp(mode, "spin") == 0) {
    return run_spin();
  }
  return run_client_mode(mode);
}
