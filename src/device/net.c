#define _POSIX_C_SOURCE 200809L

#include "brickdesk/device_net.h"

#include "device_process.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BD_NET_IFACE_MAX 32
#define BD_NET_SOCKET_DIR_MAX 128
#define BD_NET_OUTPUT_MAX 8192
#define BD_NET_RUN_TIMEOUT_MS 3000
#define BD_NET_HOTSPOT_DIR "/run/brickdesk"
#define BD_NET_HOSTAPD_CONF BD_NET_HOTSPOT_DIR "/hostapd.conf"
#define BD_NET_HOSTAPD_PID BD_NET_HOTSPOT_DIR "/hostapd.pid"
#define BD_NET_DNSMASQ_CONF BD_NET_HOTSPOT_DIR "/dnsmasq.conf"
#define BD_NET_DNSMASQ_PID BD_NET_HOTSPOT_DIR "/dnsmasq.pid"

struct bd_net {
  char iface[BD_NET_IFACE_MAX];
  char socket_dir[BD_NET_SOCKET_DIR_MAX];
  char supplicant_conf[160];
};

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
}

static void copy_text(char *dst, size_t size, const char *src) {
  if (dst == NULL || size == 0) {
    return;
  }
  snprintf(dst, size, "%s", src != NULL ? src : "");
}

static const char *env_or(const char *name, const char *fallback) {
  const char *value = getenv(name);
  return value != NULL && value[0] != '\0' ? value : fallback;
}

static bool wpa_command(bd_net *net, char *const extra[], char *output,
                        size_t output_size, char *error, size_t error_size) {
  char command[16];
  char *argv[16];
  size_t index = 0;
  snprintf(command, sizeof(command), "%s", "wpa_cli");
  argv[index++] = command;
  if (net->socket_dir[0] != '\0') {
    argv[index++] = "-p";
    argv[index++] = net->socket_dir;
  }
  argv[index++] = "-i";
  argv[index++] = net->iface;
  for (size_t i = 0; extra != NULL && extra[i] != NULL && index + 1 < 16; ++i) {
    argv[index++] = extra[i];
  }
  argv[index] = NULL;
  int code = 0;
  if (!bd_process_run(argv, output, output_size, &code, error, error_size)) {
    return false;
  }
  if (code != 0) {
    set_error(error, error_size, "wpa_cli %s failed (%d)", extra[0], code);
    return false;
  }
  return true;
}

static bool ip_link(bd_net *net, bool up, char *error, size_t error_size) {
  char *argv[] = {"ip", "link", "set", "dev", net->iface, up ? "up" : "down",
                  NULL};
  int code = 0;
  if (!bd_process_run(argv, NULL, 0, &code, error, error_size)) {
    return false;
  }
  if (code != 0) {
    set_error(error, error_size, "ip link %s failed (%d)", up ? "up" : "down",
              code);
    return false;
  }
  return true;
}

static bool interface_up(bd_net *net) {
  char path[128];
  snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", net->iface);
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  char state[32] = {0};
  bool ok = fgets(state, sizeof(state), file) != NULL;
  fclose(file);
  return ok && strncmp(state, "up", 2) == 0;
}

static void start_supplicant(bd_net *net, char *error, size_t error_size) {
  char *argv[] = {"wpa_supplicant", "-B", "-D", "nl80211",
                  "-i",             net->iface,
                  "-c",             net->supplicant_conf,
                  "-O",             net->socket_dir,
                  NULL};
  int code = 0;
  if (!bd_process_run(argv, NULL, 0, &code, error, error_size) && code != 0) {
    return;
  }
}

static void start_dhcp(bd_net *net) {
  char *argv[] = {"udhcpc", "-i", net->iface, "-b", "-q", NULL};
  int code = 0;
  char error[128];
  bd_process_run(argv, NULL, 0, &code, error, sizeof(error));
}

static bool parse_status(const char *text, bd_net_status *status) {
  bool saw_state = false;
  char line[512];
  const char *cursor = text;
  while (cursor != NULL && *cursor != '\0') {
    const char *newline = strchr(cursor, '\n');
    size_t length = newline != NULL ? (size_t)(newline - cursor) : strlen(cursor);
    if (length >= sizeof(line)) {
      length = sizeof(line) - 1;
    }
    memcpy(line, cursor, length);
    line[length] = '\0';
    char *equals = strchr(line, '=');
    if (equals != NULL) {
      *equals = '\0';
      const char *key = line;
      const char *value = equals + 1;
      if (strcmp(key, "wpa_state") == 0) {
        saw_state = true;
        if (strcmp(value, "COMPLETED") == 0) {
          status->state = BD_NET_STATE_CONNECTED;
        } else if (strcmp(value, "SCANNING") == 0) {
          status->state = BD_NET_STATE_SCANNING;
        } else if (strcmp(value, "DISCONNECTED") == 0 ||
                   strcmp(value, "INACTIVE") == 0) {
          status->state = BD_NET_STATE_DISCONNECTED;
        } else {
          status->state = BD_NET_STATE_CONNECTING;
        }
      } else if (strcmp(key, "ssid") == 0) {
        copy_text(status->ssid, sizeof(status->ssid), value);
      } else if (strcmp(key, "ip_address") == 0) {
        copy_text(status->ip, sizeof(status->ip), value);
      }
    }
    cursor = newline != NULL ? newline + 1 : NULL;
  }
  return saw_state;
}

bd_net *bd_net_create(const char *iface, const char *socket_dir) {
  bd_net *net = (bd_net *)calloc(1, sizeof(*net));
  if (net == NULL) {
    return NULL;
  }
  copy_text(net->iface, sizeof(net->iface),
            iface != NULL ? iface : env_or("BRICKDESK_WIFI_IFACE", "wlan0"));
  copy_text(net->socket_dir, sizeof(net->socket_dir),
            socket_dir != NULL
                ? socket_dir
                : env_or("BRICKDESK_WIFI_SOCKET_DIR", "/etc/wifi/sockets"));
  copy_text(net->supplicant_conf, sizeof(net->supplicant_conf),
            env_or("BRICKDESK_WIFI_CONFIG", "/etc/wifi/wpa_supplicant.conf"));
  return net;
}

void bd_net_destroy(bd_net *net) {
  free(net);
}

bool bd_net_get_status(bd_net *net, bd_net_status *status) {
  if (net == NULL || status == NULL) {
    return false;
  }
  memset(status, 0, sizeof(*status));
  status->state = BD_NET_STATE_UNKNOWN;
  status->enabled = interface_up(net);
  status->signal = -1;
  char *extra[] = {"status", NULL};
  char output[BD_NET_OUTPUT_MAX];
  char error[128] = {0};
  if (!wpa_command(net, extra, output, sizeof(output), error, sizeof(error))) {
    status->state = status->enabled ? BD_NET_STATE_DISCONNECTED
                                    : BD_NET_STATE_DISABLED;
    return true;
  }
  parse_status(output, status);
  status->enabled = status->enabled || status->state == BD_NET_STATE_CONNECTED;
  char hotspot_pid[64];
  snprintf(hotspot_pid, sizeof(hotspot_pid), "%s", BD_NET_HOSTAPD_PID);
  status->hotspot = access(hotspot_pid, F_OK) == 0;
  return true;
}

bool bd_net_set_enabled(bd_net *net, bool enabled, char *error,
                        size_t error_size) {
  if (net == NULL) {
    set_error(error, error_size, "missing network backend");
    return false;
  }
  if (enabled) {
    if (!ip_link(net, true, error, error_size)) {
      return false;
    }
    bd_net_status status;
    memset(&status, 0, sizeof(status));
    char *extra[] = {"status", NULL};
    char output[256];
    char probe_error[64];
    if (!wpa_command(net, extra, output, sizeof(output), probe_error,
                     sizeof(probe_error))) {
      start_supplicant(net, error, error_size);
    }
    start_dhcp(net);
    return true;
  }
  char *disconnect[] = {"disconnect", NULL};
  char output[256];
  char ignored[64];
  wpa_command(net, disconnect, output, sizeof(output), ignored,
              sizeof(ignored));
  char *kill_argv[] = {"killall", "udhcpc", NULL};
  int code = 0;
  bd_process_run(kill_argv, NULL, 0, &code, ignored, sizeof(ignored));
  char *kill_supplicant[] = {"killall", "wpa_supplicant", NULL};
  bd_process_run(kill_supplicant, NULL, 0, &code, ignored, sizeof(ignored));
  return ip_link(net, false, error, error_size);
}

bool bd_net_scan_start(bd_net *net, char *error, size_t error_size) {
  char *extra[] = {"scan", NULL};
  char output[256];
  return wpa_command(net, extra, output, sizeof(output), error, error_size);
}

static int parse_signal(const char *value) {
  if (value == NULL) {
    return -1;
  }
  return (int)strtol(value, NULL, 10);
}

static void add_scan_result(bd_net_network *out, size_t max, size_t *count,
                            const bd_net_network *entry) {
  for (size_t i = 0; i < *count; ++i) {
    if (strcmp(out[i].ssid, entry->ssid) == 0) {
      if (entry->signal > out[i].signal) {
        out[i] = *entry;
      }
      return;
    }
  }
  if (*count >= max) {
    return;
  }
  out[*count] = *entry;
  ++*count;
}

bool bd_net_scan_results(bd_net *net, bd_net_network *out, size_t max,
                         size_t *count, char *error, size_t error_size) {
  if (net == NULL || out == NULL || count == NULL) {
    return false;
  }
  *count = 0;
  char *extra[] = {"scan_results", NULL};
  char output[BD_NET_OUTPUT_MAX];
  if (!wpa_command(net, extra, output, sizeof(output), error, error_size)) {
    return false;
  }
  char *save = NULL;
  char *line = strtok_r(output, "\n", &save);
  if (line != NULL && strstr(line, "bssid") != NULL) {
    line = strtok_r(NULL, "\n", &save);
  }
  while (line != NULL) {
    char *fields[5] = {0};
    int field_count = 0;
    char *field_save = NULL;
    char *field = strtok_r(line, "\t", &field_save);
    while (field != NULL && field_count < 5) {
      fields[field_count++] = field;
      field = strtok_r(NULL, "\t", &field_save);
    }
    if (field_count >= 5 && fields[4][0] != '\0') {
      bd_net_network entry;
      memset(&entry, 0, sizeof(entry));
      entry.id = -1;
      copy_text(entry.ssid, sizeof(entry.ssid), fields[4]);
      entry.signal = parse_signal(fields[2]);
      entry.secured = strstr(fields[3], "WPA") != NULL ||
                      strstr(fields[3], "WEP") != NULL ||
                      strstr(fields[3], "SAE") != NULL;
      add_scan_result(out, max, count, &entry);
    }
    line = strtok_r(NULL, "\n", &save);
  }
  /* Strongest first. */
  for (size_t i = 0; i < *count; ++i) {
    for (size_t j = i + 1; j < *count; ++j) {
      if (out[j].signal > out[i].signal) {
        bd_net_network swap = out[i];
        out[i] = out[j];
        out[j] = swap;
      }
    }
  }
  return true;
}

bool bd_net_connect(bd_net *net, const char *ssid, const char *psk,
                    char *error, size_t error_size) {
  if (net == NULL || ssid == NULL || ssid[0] == '\0') {
    set_error(error, error_size, "missing SSID");
    return false;
  }
  char *add[] = {"add_network", NULL};
  char output[256];
  if (!wpa_command(net, add, output, sizeof(output), error, error_size)) {
    return false;
  }
  int network_id = atoi(output);
  char id[16];
  snprintf(id, sizeof(id), "%d", network_id);
  char quoted_ssid[BD_NET_SSID_MAX + 3];
  snprintf(quoted_ssid, sizeof(quoted_ssid), "\"%s\"", ssid);
  char *set_ssid[] = {"set_network", id, "ssid", quoted_ssid, NULL};
  if (!wpa_command(net, set_ssid, output, sizeof(output), error,
                   error_size)) {
    return false;
  }
  if (psk != NULL && psk[0] != '\0') {
    char quoted_psk[BD_NET_PSK_MAX + 3];
    snprintf(quoted_psk, sizeof(quoted_psk), "\"%s\"", psk);
    char *set_psk[] = {"set_network", id, "psk", quoted_psk, NULL};
    if (!wpa_command(net, set_psk, output, sizeof(output), error,
                     error_size)) {
      return false;
    }
  } else {
    char *set_open[] = {"set_network", id, "key_mgmt", "NONE", NULL};
    if (!wpa_command(net, set_open, output, sizeof(output), error,
                     error_size)) {
      return false;
    }
  }
  char *select[] = {"select_network", id, NULL};
  if (!wpa_command(net, select, output, sizeof(output), error, error_size)) {
    return false;
  }
  char *save[] = {"save_config", NULL};
  wpa_command(net, save, output, sizeof(output), error, error_size);
  start_dhcp(net);
  return true;
}

bool bd_net_disconnect(bd_net *net, char *error, size_t error_size) {
  char *extra[] = {"disconnect", NULL};
  char output[256];
  return wpa_command(net, extra, output, sizeof(output), error, error_size);
}

bool bd_net_list_saved(bd_net *net, bd_net_network *out, size_t max,
                       size_t *count, char *error, size_t error_size) {
  if (net == NULL || out == NULL || count == NULL) {
    return false;
  }
  *count = 0;
  char *extra[] = {"list_networks", NULL};
  char output[BD_NET_OUTPUT_MAX];
  if (!wpa_command(net, extra, output, sizeof(output), error, error_size)) {
    return false;
  }
  char *save = NULL;
  char *line = strtok_r(output, "\n", &save);
  if (line != NULL && strstr(line, "network id") != NULL) {
    line = strtok_r(NULL, "\n", &save);
  }
  while (line != NULL && *count < max) {
    char *fields[4] = {0};
    int field_count = 0;
    char *field_save = NULL;
    char *field = strtok_r(line, "\t", &field_save);
    while (field != NULL && field_count < 4) {
      fields[field_count++] = field;
      field = strtok_r(NULL, "\t", &field_save);
    }
    if (field_count >= 3 && fields[1][0] != '\0') {
      bd_net_network *entry = &out[*count];
      memset(entry, 0, sizeof(*entry));
      entry->id = atoi(fields[0]);
      copy_text(entry->ssid, sizeof(entry->ssid), fields[1]);
      entry->saved = true;
      entry->signal = -1;
      entry->current =
          field_count >= 4 && strstr(fields[3], "CURRENT") != NULL;
      ++*count;
    }
    line = strtok_r(NULL, "\n", &save);
  }
  return true;
}

bool bd_net_forget(bd_net *net, int network_id, char *error,
                   size_t error_size) {
  char id[16];
  snprintf(id, sizeof(id), "%d", network_id);
  char *extra[] = {"remove_network", id, NULL};
  char output[256];
  if (!wpa_command(net, extra, output, sizeof(output), error, error_size)) {
    return false;
  }
  char *save[] = {"save_config", NULL};
  return wpa_command(net, save, output, sizeof(output), error, error_size);
}

bool bd_net_select_saved(bd_net *net, int network_id, char *error,
                         size_t error_size) {
  char id[16];
  snprintf(id, sizeof(id), "%d", network_id);
  char output[256];
  char *enable[] = {"enable_network", id, NULL};
  if (!wpa_command(net, enable, output, sizeof(output), error, error_size)) {
    return false;
  }
  char *select[] = {"select_network", id, NULL};
  if (!wpa_command(net, select, output, sizeof(output), error, error_size)) {
    return false;
  }
  start_dhcp(net);
  return true;
}

static bool write_file(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs(text, file) >= 0;
  return fclose(file) == 0 && ok;
}


static void stop_pid(const char *pid_path) {
  FILE *file = fopen(pid_path, "r");
  if (file != NULL) {
    long pid = 0;
    if (fscanf(file, "%ld", &pid) == 1 && pid > 0) {
      kill((pid_t)pid, SIGTERM);
    }
    fclose(file);
  }
  unlink(pid_path);
}

bool bd_net_hotspot_start(bd_net *net, const char *ssid, const char *psk,
                          char *error, size_t error_size) {
  if (net == NULL || ssid == NULL || ssid[0] == '\0') {
    set_error(error, error_size, "missing hotspot SSID");
    return false;
  }
  mkdir(BD_NET_HOTSPOT_DIR, 0755);
  char config[1024];
  int written;
  if (psk != NULL && psk[0] != '\0') {
    written = snprintf(config, sizeof(config),
                       "interface=%s\ndriver=nl80211\nssid=%s\nhw_mode=g\n"
                       "channel=6\nwpa=2\nwpa_passphrase=%s\n"
                       "wpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\n",
                       net->iface, ssid, psk);
  } else {
    written = snprintf(config, sizeof(config),
                       "interface=%s\ndriver=nl80211\nssid=%s\nhw_mode=g\n"
                       "channel=6\nwpa=0\n",
                       net->iface, ssid);
  }
  if (written <= 0 || (size_t)written >= sizeof(config) ||
      !write_file(BD_NET_HOSTAPD_CONF, config)) {
    set_error(error, error_size, "cannot write hostapd.conf");
    return false;
  }
  char dnsmasq[512];
  written = snprintf(dnsmasq, sizeof(dnsmasq),
                     "interface=%s\ndhcp-range=192.168.4.10,192.168.4.200,"
                     "255.255.255.0,24h\ndhcp-option=3,192.168.4.1\n"
                     "dhcp-option=6,192.168.4.1\n",
                     net->iface);
  if (written <= 0 || (size_t)written >= sizeof(dnsmasq) ||
      !write_file(BD_NET_DNSMASQ_CONF, dnsmasq)) {
    set_error(error, error_size, "cannot write dnsmasq.conf");
    return false;
  }
  char *kill_argv[] = {"killall", "wpa_supplicant", NULL};
  int code = 0;
  char ignored[64];
  bd_process_run(kill_argv, NULL, 0, &code, ignored, sizeof(ignored));
  char *kill_dhcp[] = {"killall", "udhcpc", NULL};
  bd_process_run(kill_dhcp, NULL, 0, &code, ignored, sizeof(ignored));
  if (!ip_link(net, true, error, error_size)) {
    return false;
  }
  char *flush[] = {"ip", "addr", "flush", "dev", net->iface, NULL};
  bd_process_run(flush, NULL, 0, &code, ignored, sizeof(ignored));
  char *addr[] = {"ip", "addr", "add", "192.168.4.1/24", "dev", net->iface,
                  NULL};
  if (!bd_process_run(addr, NULL, 0, &code, error, error_size) || code != 0) {
    set_error(error, error_size, "cannot assign hotspot address");
    return false;
  }
  char *hostapd[] = {"hostapd", "-B", "-P", BD_NET_HOSTAPD_PID,
                     BD_NET_HOSTAPD_CONF, NULL};
  if (!bd_process_run(hostapd, NULL, 0, &code, error, error_size) || code != 0) {
    set_error(error, error_size, "hostapd failed (%d)", code);
    return false;
  }
  char *dns[] = {"dnsmasq", "--conf-file=" BD_NET_DNSMASQ_CONF, "--pid-file="
                 BD_NET_DNSMASQ_PID, NULL};
  if (!bd_process_run(dns, NULL, 0, &code, error, error_size) || code != 0) {
    stop_pid(BD_NET_HOSTAPD_PID);
    set_error(error, error_size, "dnsmasq failed (%d)", code);
    return false;
  }
  return true;
}

bool bd_net_hotspot_stop(bd_net *net, char *error, size_t error_size) {
  stop_pid(BD_NET_DNSMASQ_PID);
  stop_pid(BD_NET_HOSTAPD_PID);
  if (net != NULL) {
    int code = 0;
    char ignored[64];
    char *flush[] = {"ip", "addr", "flush", "dev", net->iface, NULL};
    bd_process_run(flush, NULL, 0, &code, ignored, sizeof(ignored));
    ip_link(net, false, ignored, sizeof(ignored));
  }
  if (error != NULL && error_size > 0) {
    error[0] = '\0';
  }
  return true;
}

bool bd_net_suspend(char *error, size_t error_size) {
  const char *iface = env_or("BRICKDESK_WIFI_IFACE", "wlan0");
  char *kill_argv[] = {"killall", "udhcpc", NULL};
  int code = 0;
  char ignored[64];
  bd_process_run(kill_argv, NULL, 0, &code, ignored, sizeof(ignored));
  char *kill_supplicant[] = {"killall", "wpa_supplicant", NULL};
  bd_process_run(kill_supplicant, NULL, 0, &code, ignored, sizeof(ignored));
  char *down[] = {"ip", "link", "set", "dev", (char *)iface, "down", NULL};
  if (!bd_process_run(down, NULL, 0, &code, error, error_size)) {
    return false;
  }
  return code == 0;
}

bool bd_net_resume(char *error, size_t error_size) {
  bd_net *net = bd_net_create(NULL, NULL);
  if (net == NULL) {
    set_error(error, error_size, "out of memory");
    return false;
  }
  if (!ip_link(net, true, error, error_size)) {
    bd_net_destroy(net);
    return false;
  }
  start_supplicant(net, error, error_size);
  start_dhcp(net);
  bd_net_destroy(net);
  return true;
}

const char *bd_net_state_name(bd_net_state state) {
  switch (state) {
    case BD_NET_STATE_DISABLED:
      return "disabled";
    case BD_NET_STATE_DISCONNECTED:
      return "disconnected";
    case BD_NET_STATE_SCANNING:
      return "scanning";
    case BD_NET_STATE_CONNECTING:
      return "connecting";
    case BD_NET_STATE_CONNECTED:
      return "connected";
    case BD_NET_STATE_UNKNOWN:
    default:
      return "unknown";
  }
}
