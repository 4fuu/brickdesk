#ifndef BRICKDESK_SESSION_APP_PROTOCOL_H
#define BRICKDESK_SESSION_APP_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "brickdesk/app.h"

#define BD_APP_PROTOCOL_MAX_LINE (64u * 1024u)
#define BD_APP_PROTOCOL_MAX_PENDING 16
#define BD_APP_PROTOCOL_EVENT_TEXT 4096

#define BD_APP_PROTOCOL_CAP_PAUSE (1u << 0)
#define BD_APP_PROTOCOL_CAP_RESUME (1u << 1)
#define BD_APP_PROTOCOL_CAP_SAVE_STATE (1u << 2)
#define BD_APP_PROTOCOL_CAP_LOAD_STATE (1u << 3)
#define BD_APP_PROTOCOL_CAP_SCREENSHOT (1u << 4)
#define BD_APP_PROTOCOL_CAP_BACKGROUND (1u << 5)
#define BD_APP_PROTOCOL_CAP_DISPLAY_RELEASE (1u << 6)

/* JSON-RPC 2.0 error codes from architecture.md 3.5. */
#define BD_APP_PROTOCOL_ERROR_PARSE (-32700)
#define BD_APP_PROTOCOL_ERROR_REQUEST (-32600)
#define BD_APP_PROTOCOL_ERROR_METHOD (-32601)
#define BD_APP_PROTOCOL_ERROR_PARAMS (-32602)
#define BD_APP_PROTOCOL_ERROR_UNSUPPORTED (-32001)
#define BD_APP_PROTOCOL_ERROR_BUSY (-32002)
#define BD_APP_PROTOCOL_ERROR_TIMEOUT (-32003)
#define BD_APP_PROTOCOL_ERROR_INTERNAL (-32005)

typedef enum bd_app_protocol_event_kind {
  BD_APP_PROTOCOL_EVENT_HELLO = 0,
  BD_APP_PROTOCOL_EVENT_REQUEST,
  BD_APP_PROTOCOL_EVENT_NOTIFICATION,
  BD_APP_PROTOCOL_EVENT_RESPONSE,
  BD_APP_PROTOCOL_EVENT_TIMEOUT,
  BD_APP_PROTOCOL_EVENT_DISCONNECTED,
  BD_APP_PROTOCOL_EVENT_ERROR
} bd_app_protocol_event_kind;

typedef struct bd_app_protocol_event {
  bd_app_protocol_event_kind kind;
  int id;
  int pid;
  int protocol;
  unsigned capabilities; /* BD_APP_PROTOCOL_CAP_* from app.hello */
  bool ok;               /* response: result vs error */
  int error_code;
  char method[BD_APP_METHOD_MAX];
  char params[BD_APP_PROTOCOL_EVENT_TEXT];
  char result[BD_APP_PROTOCOL_EVENT_TEXT];
  char error_message[256];
} bd_app_protocol_event;

typedef struct bd_app_protocol_config {
  const char *socket_path;
  const char *session_id;
  const char *menu_mode;
  const char *state_dir;
  void (*on_event)(void *user, const bd_app_protocol_event *event);
  void *user;
} bd_app_protocol_config;

typedef struct bd_app_protocol bd_app_protocol;

bd_app_protocol *bd_app_protocol_create(const bd_app_protocol_config *config,
                                        char *error, size_t error_size);
void bd_app_protocol_destroy(bd_app_protocol *protocol);

const char *bd_app_protocol_socket_path(const bd_app_protocol *protocol);
bool bd_app_protocol_connected(const bd_app_protocol *protocol);

/*
 * Queues a host -> app request.  Returns the JSON-RPC id, or -1 when the app
 * is not connected or the output buffer is full.
 */
int bd_app_protocol_request(bd_app_protocol *protocol, const char *method,
                            const char *params_json, int timeout_ms);
bool bd_app_protocol_reply(bd_app_protocol *protocol, int id,
                           const char *result_json);
bool bd_app_protocol_reply_error(bd_app_protocol *protocol, int id,
                                 int code, const char *message);

/* Accepts, reads, parses, flushes and expires pending requests. */
void bd_app_protocol_poll(bd_app_protocol *protocol, uint64_t now_ms);
/* Milliseconds until the next request timeout, or -1. */
int bd_app_protocol_next_timeout(const bd_app_protocol *protocol,
                                 uint64_t now_ms);

#endif /* BRICKDESK_SESSION_APP_PROTOCOL_H */
