#ifndef BRICKDESK_DEVICE_BT_H
#define BRICKDESK_DEVICE_BT_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Bluetooth backend built on bluetoothctl.  Scanning runs detached with
 * `bluetoothctl --timeout N scan on` so the desktop never blocks; the UI polls
 * bd_bt_list_devices() after the scan window.  Audio uses bluealsa, and
 * suspend/resume follow the stock tg5040 order: power off, stop bluealsa and
 * bluetoothd, rfkill block; resume rfkill unblock, start bluetoothd/bluealsa.
 */

#define BD_BT_ADDR_MAX 18
#define BD_BT_NAME_MAX 64
#define BD_BT_MAX_DEVICES 24

typedef enum bd_bt_state {
  BD_BT_STATE_UNKNOWN = 0,
  BD_BT_STATE_DISABLED,
  BD_BT_STATE_ENABLED,
  BD_BT_STATE_CONNECTED
} bd_bt_state;

typedef struct bd_bt_status {
  bd_bt_state state;
  bool powered;
  bool discovering;
  char adapter[BD_BT_NAME_MAX];
  char connected_name[BD_BT_NAME_MAX];
  char connected_addr[BD_BT_ADDR_MAX];
} bd_bt_status;

typedef struct bd_bt_device {
  char addr[BD_BT_ADDR_MAX];
  char name[BD_BT_NAME_MAX];
  bool connected;
  bool paired;
} bd_bt_device;

typedef struct bd_bt bd_bt;

bd_bt *bd_bt_create(void);
void bd_bt_destroy(bd_bt *bt);

bool bd_bt_get_status(bd_bt *bt, bd_bt_status *status);
bool bd_bt_set_powered(bd_bt *bt, bool powered, char *error,
                       size_t error_size);
/* Starts a detached scan for scan_seconds. */
bool bd_bt_scan_start(bd_bt *bt, int scan_seconds, char *error,
                      size_t error_size);
bool bd_bt_list_devices(bd_bt *bt, bd_bt_device *out, size_t max,
                        size_t *count, char *error, size_t error_size);
bool bd_bt_connect(bd_bt *bt, const char *addr, char *error,
                   size_t error_size);
bool bd_bt_disconnect(bd_bt *bt, const char *addr, char *error,
                      size_t error_size);
bool bd_bt_forget(bd_bt *bt, const char *addr, char *error,
                  size_t error_size);

bool bd_bt_suspend(char *error, size_t error_size);
bool bd_bt_resume(char *error, size_t error_size);

const char *bd_bt_state_name(bd_bt_state state);

#endif /* BRICKDESK_DEVICE_BT_H */
