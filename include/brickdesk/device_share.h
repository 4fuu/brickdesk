#ifndef BRICKDESK_DEVICE_SHARE_H
#define BRICKDESK_DEVICE_SHARE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * SSH/FTP/Samba/ADB and USB-mode control for the stock tg5040 image.
 *
 * Services are started with argv and stopped with killall; the backend never
 * runs a shell.  usb_mode calls /usr/trimui/bin/usbmode when it exists and
 * otherwise only persists the preference (the stock script owns the gadget).
 */

#define BD_SHARE_IP_MAX 48
#define BD_SHARE_MODE_MAX 16

typedef enum bd_share_service {
  BD_SHARE_SSH = 0,
  BD_SHARE_FTP,
  BD_SHARE_SAMBA,
  BD_SHARE_ADB
} bd_share_service;

typedef struct bd_share_status {
  bool ssh;
  bool ftp;
  bool samba;
  bool adb;
  char ip[BD_SHARE_IP_MAX];
  char usb_mode[BD_SHARE_MODE_MAX];
} bd_share_status;

typedef struct bd_share bd_share;

bd_share *bd_share_create(const char *iface);
void bd_share_destroy(bd_share *share);

bool bd_share_get_status(bd_share *share, bd_share_status *status);
bool bd_share_set_service(bd_share *share, bd_share_service service,
                          bool enabled, char *error, size_t error_size);
bool bd_share_set_usb_mode(bd_share *share, const char *mode, char *error,
                           size_t error_size);

#endif /* BRICKDESK_DEVICE_SHARE_H */
