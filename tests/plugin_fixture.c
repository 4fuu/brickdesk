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

static void write_result_text(const char *path, const char *text) {
  if (path == NULL) {
    return;
  }
  FILE *file = fopen(path, "wb");
  if (file == NULL) {
    return;
  }
  fwrite(text, 1, strlen(text), file);
  fclose(file);
}

static bool find_long_field(const char *text, const char *field, long *out) {
  const char *position = strstr(text, field);
  if (position == NULL) {
    return false;
  }
  position += strlen(field);
  return sscanf(position, "%ld", out) == 1;
}

static bool find_string_field(const char *text, const char *field, char *out,
                              size_t out_size) {
  const char *position = strstr(text, field);
  if (position == NULL) {
    return false;
  }
  position += strlen(field);
  while (*position == ' ' || *position == '\t') {
    ++position;
  }
  if (*position != '"') {
    return false;
  }
  ++position;
  size_t used = 0;
  while (*position != '\0' && *position != '"' && used + 1 < out_size) {
    if (*position == '\\' && position[1] != '\0') {
      ++position;
    }
    out[used++] = *position++;
  }
  out[used] = '\0';
  return true;
}

static int send_ready(int fd, const char *hello) {
  long id = 1;
  find_long_field(hello, "\"id\":", &id);
  char reply[128];
  int written = snprintf(reply, sizeof(reply),
                         "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{"
                         "\"pid\":%ld,\"api\":%d}}",
                         id, (long)getpid(), BD_PLUGIN_API_VERSION);
  if (written <= 0 || (size_t)written >= sizeof(reply)) {
    return -1;
  }
  return bd_plugin_send_line(fd, reply, (size_t)written) ? 0 : -1;
}

static void log_heartbeat_ping(void) {
  const char *path = getenv("BRICKDESK_TEST_PLUGIN_PING_LOG");
  if (path == NULL || path[0] == '\0') {
    return;
  }
  FILE *file = fopen(path, "a");
  if (file == NULL) {
    return;
  }
  fputs("ping\n", file);
  fclose(file);
}

static int serve_host_message(int fd, const char *line) {
  if (strstr(line, "\"host.shutdown\"") != NULL) {
    return 0;
  }
  if (strstr(line, "\"host.ping\"") != NULL) {
    log_heartbeat_ping();
    long id = 0;
    find_long_field(line, "\"id\":", &id);
    char reply[128];
    int written = snprintf(reply, sizeof(reply),
                           "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{"
                           "\"pong\":true}}",
                           id);
    if (written <= 0 || (size_t)written >= sizeof(reply) ||
        !bd_plugin_send_line(fd, reply, (size_t)written)) {
      return -1;
    }
  }
  return 1;
}

static int settings_request(int fd, long id, const char *method,
                            const char *key, const char *value, char *out,
                            size_t out_size) {
  char params[512];
  int written;
  if (value != NULL) {
    written = snprintf(params, sizeof(params),
                       "{\"key\":\"%s\",\"value\":%s}", key, value);
  } else {
    written = snprintf(params, sizeof(params), "{\"key\":\"%s\"}", key);
  }
  if (written <= 0 || (size_t)written >= sizeof(params)) {
    return -1;
  }
  char line[1024];
  written = snprintf(line, sizeof(line),
                     "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"method\":\"%s\","
                     "\"params\":%s}",
                     id, method, params);
  if (written <= 0 || (size_t)written >= sizeof(line) ||
      !bd_plugin_send_line(fd, line, (size_t)written)) {
    return -1;
  }
  for (;;) {
    char reply[BD_PLUGIN_LINE_MAX];
    if (read_line(fd, reply, sizeof(reply)) < 0) {
      return -1;
    }
    if (strstr(reply, "\"method\"") != NULL) {
      serve_host_message(fd, reply);
      continue;
    }
    long reply_id = 0;
    if (!find_long_field(reply, "\"id\":", &reply_id) || reply_id != id) {
      continue;
    }
    if (strstr(reply, "\"error\"") != NULL) {
      long code = 0;
      char message[128] = {0};
      find_long_field(reply, "\"code\":", &code);
      find_string_field(reply, "\"message\":", message, sizeof(message));
      snprintf(out, out_size, "error %ld %s", code, message);
      return 0;
    }
    if (value != NULL) {
      snprintf(out, out_size, "%s",
               strstr(reply, "\"durable\":false") != NULL ? "unsynced" : "ok");
      return 0;
    }
    const char *value_field = strstr(reply, "\"value\":");
    if (value_field == NULL) {
      snprintf(out, out_size, "error missing value");
      return 0;
    }
    value_field += strlen("\"value\":");
    while (*value_field == ' ' || *value_field == '\t') {
      ++value_field;
    }
    size_t used = 0;
    if (*value_field == '"') {
      ++value_field;
      while (*value_field != '\0' && *value_field != '"' &&
             used + 1 < out_size) {
        out[used++] = *value_field++;
      }
    } else {
      while (*value_field != '\0' && *value_field != ',' &&
             *value_field != '}' && used + 1 < out_size) {
        out[used++] = *value_field++;
      }
    }
    out[used] = '\0';
    char normalized[256];
    snprintf(normalized, sizeof(normalized), "value %s", out);
    snprintf(out, out_size, "%s", normalized);
    return 0;
  }
}

static int run_settings_mode(void) {
  int fd = raw_connect();
  if (fd < 0) {
    return 1;
  }
  char hello[BD_PLUGIN_LINE_MAX];
  if (read_line(fd, hello, sizeof(hello)) < 0 || send_ready(fd, hello) != 0) {
    close(fd);
    return 1;
  }
  const char *action = getenv("BD_FIXTURE_SETTINGS_ACTION");
  const char *key = getenv("BD_FIXTURE_SETTINGS_KEY");
  const char *value = getenv("BD_FIXTURE_SETTINGS_VALUE");
  const char *result_path = getenv("BD_FIXTURE_SETTINGS_RESULT");
  if (action == NULL) {
    action = "get";
  }
  if (key == NULL) {
    key = "show_meta";
  }
  if (value == NULL) {
    value = "false";
  }
  char result[256] = {0};
  int request = -1;
  if (strcmp(action, "set") == 0) {
    request = settings_request(fd, 2, "plugin.settings.set", key, value,
                               result, sizeof(result));
  } else if (strcmp(action, "get") == 0) {
    request = settings_request(fd, 2, "plugin.settings.get", key, NULL,
                               result, sizeof(result));
  } else {
    snprintf(result, sizeof(result), "error unknown action");
    request = 0;
  }
  if (request < 0) {
    snprintf(result, sizeof(result), "error connection failed");
  }
  write_result_text(result_path, result);
  while (read_line(fd, hello, sizeof(hello)) > 0) {
    if (serve_host_message(fd, hello) <= 0) {
      break;
    }
  }
  close(fd);
  return 0;
}

static int run_heartbeat(void) {
  int fd = raw_connect();
  if (fd < 0) {
    return 1;
  }
  char line[BD_PLUGIN_LINE_MAX];
  if (read_line(fd, line, sizeof(line)) < 0 || send_ready(fd, line) != 0) {
    close(fd);
    return 1;
  }
  while (read_line(fd, line, sizeof(line)) > 0) {
    if (serve_host_message(fd, line) <= 0) {
      break;
    }
  }
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
    fprintf(stderr,
            "usage: bd_plugin_fixture "
            "good|long|silent|crash|badui|spin|alloc|settings|heartbeat\n");
    return 2;
  }
  const char *mode = argv[1];
  if (strcmp(mode, "silent") == 0) {
    return run_silent();
  }
  if (strcmp(mode, "spin") == 0) {
    return run_spin();
  }
  if (strcmp(mode, "settings") == 0) {
    return run_settings_mode();
  }
  if (strcmp(mode, "heartbeat") == 0) {
    return run_heartbeat();
  }
  return run_client_mode(mode);
}
