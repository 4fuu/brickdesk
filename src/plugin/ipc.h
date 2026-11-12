#ifndef BRICKDESK_PLUGIN_IPC_H
#define BRICKDESK_PLUGIN_IPC_H

#include <stdbool.h>
#include <stddef.h>

/* Creates a directory and missing parents (mode 0770). */
bool bd_plugin_make_dirs(const char *path);

/* Creates, binds and listens on a non-blocking AF_UNIX stream socket. */
int bd_plugin_listen_socket(const char *path, char *error, size_t error_size);
/* Connects with retries until timeout_ms; returns a blocking fd or -1. */
 int bd_plugin_connect_socket(const char *path, int timeout_ms, char *error,
                              size_t error_size);

/* Sends all bytes, retrying on EINTR and partial writes. */
bool bd_plugin_send_all(int fd, const char *data, size_t length);
/* Appends a newline and sends the line. */
bool bd_plugin_send_line(int fd, const char *line, size_t length);

#endif /* BRICKDESK_PLUGIN_IPC_H */
