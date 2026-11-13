#ifndef BRICKDESK_DEVICE_NET_H
#define BRICKDESK_DEVICE_NET_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Wi-Fi and hotspot backend for the tg5040.
 *
 * The stock image runs wpa_supplicant on wlan0 with its control socket under
 * /etc/wifi/sockets.  Commands go through wpa_cli/udhcpc/ip with argv (never a
 * shell), so SSIDs and passwords cannot be interpreted as shell syntax.
 * BRICKDESK_WIFI_IFACE and BRICKDESK_WIFI_SOCKET_DIR override the defaults for
 * host tests; BRICKDESK_WIFI_CONFIG overrides the supplicant config path.
 */

#define BD_NET_SSID_MAX 64
#define BD_NET_PSK_MAX 128
#define BD_NET_IP_MAX 48
#define BD_NET_MAX_NETWORKS 24
#define BD_NET_MAX_SCAN 32

typedef enum bd_net_state {
  BD_NET_STATE_UNKNOWN = 0,
  BD_NET_STATE_DISABLED,
  BD_NET_STATE_DISCONNECTED,
  BD_NET_STATE_SCANNING,
  BD_NET_STATE_CONNECTING,
  BD_NET_STATE_CONNECTED
} bd_net_state;

typedef struct bd_net_status {
  bd_net_state state;
  bool enabled;
  bool hotspot;
  char ssid[BD_NET_SSID_MAX];
  char ip[BD_NET_IP_MAX];
  int signal;
} bd_net_status;

typedef struct bd_net_network {
  int id; /* wpa_supplicant network id, -1 for scan-only entries */
  char ssid[BD_NET_SSID_MAX];
  int signal;
  bool secured;
  bool saved;
  bool current;
} bd_net_network;

typedef struct bd_net bd_net;

bd_net *bd_net_create(const char *iface, const char *socket_dir);
void bd_net_destroy(bd_net *net);

/* Reads wpa_cli status and the interface operstate. */
bool bd_net_get_status(bd_net *net, bd_net_status *status);
bool bd_net_set_enabled(bd_net *net, bool enabled, char *error,
                        size_t error_size);

/* Starts a scan; results are read by bd_net_scan_results(). */
bool bd_net_scan_start(bd_net *net, char *error, size_t error_size);
bool bd_net_scan_results(bd_net *net, bd_net_network *out, size_t max,
                         size_t *count, char *error, size_t error_size);

/* psk may be NULL or empty for an open network. */
bool bd_net_connect(bd_net *net, const char *ssid, const char *psk,
                    char *error, size_t error_size);
bool bd_net_disconnect(bd_net *net, char *error, size_t error_size);
bool bd_net_list_saved(bd_net *net, bd_net_network *out, size_t max,
                       size_t *count, char *error, size_t error_size);
bool bd_net_forget(bd_net *net, int network_id, char *error,
                   size_t error_size);
bool bd_net_select_saved(bd_net *net, int network_id, char *error,
                         size_t error_size);

bool bd_net_hotspot_start(bd_net *net, const char *ssid, const char *psk,
                          char *error, size_t error_size);
bool bd_net_hotspot_stop(bd_net *net, char *error, size_t error_size);

/*
 * Deep-sleep resource hooks.  suspend stops the supplicant and DHCP client
 * and brings wlan0 down; resume brings it back and restarts the supplicant.
 * Both are safe to call when Wi-Fi was already disabled.
 */
bool bd_net_suspend(char *error, size_t error_size);
bool bd_net_resume(char *error, size_t error_size);

const char *bd_net_state_name(bd_net_state state);

#endif /* BRICKDESK_DEVICE_NET_H */
