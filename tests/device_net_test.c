#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "brickdesk/device_net.h"

static int failures;

#define CHECK(condition, ...)                    \
  do {                                           \
    if (!(condition)) {                          \
      fprintf(stderr, "device_net_test: FAIL: ");\
      fprintf(stderr, __VA_ARGS__);              \
      fputc('\n', stderr);                       \
      ++failures;                                \
    }                                            \
  } while (0)

static bool write_script(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }
  bool ok = fputs(text, file) >= 0;
  if (fclose(file) != 0) {
    ok = false;
  }
  chmod(path, 0755);
  return ok;
}

static bool read_text(const char *path, char *out, size_t size) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }
  size_t length = fread(out, 1, size - 1, file);
  out[length] = '\0';
  fclose(file);
  return true;
}

int main(void) {
  char root[] = "/tmp/bd-net-test-XXXXXX";
  if (mkdtemp(root) == NULL) {
    return 1;
  }
  char bin[256];
  char log[256];
  snprintf(bin, sizeof(bin), "%s/bin", root);
  snprintf(log, sizeof(log), "%s/calls.log", root);
  mkdir(bin, 0755);

  char wpa[1024];
  snprintf(wpa, sizeof(wpa), "%s/wpa_cli", bin);
  char script[4096];
  snprintf(script, sizeof(script),
           "#!/bin/sh\n"
           "echo \"$@\" >> '%s'\n"
           "case \"$*\" in\n"
           "  *status*) printf 'wpa_state=COMPLETED\\nssid=TestNet\\n"
           "ip_address=192.168.1.42\\n' ;;\n"
           "  *scan_results*) printf 'bssid / frequency / signal level / "
           "flags / ssid\\n"
           "aa\\t2412\\t-40\\t[WPA2-PSK-CCMP][ESS]\\tStrong\\n"
           "bb\\t2437\\t-70\\t[ESS]\\tOpen\\n"
           "cc\\t2462\\t-50\\t[WPA2-PSK-CCMP][ESS]\\tStrong\\n' ;;\n"
           "  *list_networks*) printf 'network id / ssid / bssid / flags\\n"
           "0\\tHome\\tany\\t[CURRENT]\\n1\\tCafe\\tany\\t\\n' ;;\n"
           "  *add_network*) echo 3 ;;\n"
           "  *) echo OK ;;\n"
           "esac\n",
           log);
  CHECK(write_script(wpa, script), "write wpa_cli stub");

  char ip[1024];
  snprintf(ip, sizeof(ip), "%s/ip", bin);
  snprintf(script, sizeof(script),
           "#!/bin/sh\necho \"ip $@\" >> '%s'\nexit 0\n", log);
  CHECK(write_script(ip, script), "write ip stub");
  char killall[1024];
  snprintf(killall, sizeof(killall), "%s/killall", bin);
  snprintf(script, sizeof(script),
           "#!/bin/sh\necho \"killall $@\" >> '%s'\nexit 0\n", log);
  CHECK(write_script(killall, script), "write killall stub");
  char supplicant[1024];
  snprintf(supplicant, sizeof(supplicant), "%s/wpa_supplicant", bin);
  snprintf(script, sizeof(script),
           "#!/bin/sh\necho \"wpa_supplicant $@\" >> '%s'\nexit 0\n", log);
  CHECK(write_script(supplicant, script), "write wpa_supplicant stub");
  char dhcp[1024];
  snprintf(dhcp, sizeof(dhcp), "%s/udhcpc", bin);
  snprintf(script, sizeof(script),
           "#!/bin/sh\necho \"udhcpc $@\" >> '%s'\nexit 0\n", log);
  CHECK(write_script(dhcp, script), "write udhcpc stub");

  const char *old_path = getenv("PATH");
  char path[2048];
  snprintf(path, sizeof(path), "%s:%s", bin,
           old_path != NULL ? old_path : "/usr/bin:/bin");
  setenv("PATH", path, 1);

  bd_net *net = bd_net_create("wlan0", "/tmp/sockets");
  CHECK(net != NULL, "create");
  if (net == NULL) {
    return 1;
  }
  bd_net_status status;
  CHECK(bd_net_get_status(net, &status), "status");
  CHECK(status.state == BD_NET_STATE_CONNECTED, "state %s",
        bd_net_state_name(status.state));
  CHECK(strcmp(status.ssid, "TestNet") == 0, "ssid %s", status.ssid);
  CHECK(strcmp(status.ip, "192.168.1.42") == 0, "ip %s", status.ip);

  char error[256] = {0};
  CHECK(bd_net_scan_start(net, error, sizeof(error)), "scan start: %s", error);
  bd_net_network results[8];
  size_t count = 0;
  CHECK(bd_net_scan_results(net, results, 8, &count, error, sizeof(error)),
        "scan results: %s", error);
  CHECK(count == 2, "deduplicated scan count %zu", count);
  if (count == 2) {
    CHECK(strcmp(results[0].ssid, "Strong") == 0, "strongest first: %s",
          results[0].ssid);
    CHECK(results[0].secured && !results[1].secured, "security flags");
  }

  CHECK(bd_net_connect(net, "Cafe", "secret", error, sizeof(error)),
        "connect: %s", error);
  char calls[8192];
  CHECK(read_text(log, calls, sizeof(calls)), "read call log");
  CHECK(strstr(calls, "set_network 3 ssid \"Cafe\"") != NULL,
        "ssid argument: %s", calls);
  CHECK(strstr(calls, "set_network 3 psk \"secret\"") != NULL,
        "psk argument");
  CHECK(strstr(calls, "select_network 3") != NULL, "select network");
  CHECK(strstr(calls, "save_config") != NULL, "save config");

  bd_net_network saved[8];
  count = 0;
  CHECK(bd_net_list_saved(net, saved, 8, &count, error, sizeof(error)),
        "list saved: %s", error);
  CHECK(count == 2, "saved count %zu", count);
  if (count == 2) {
    CHECK(saved[0].current, "current flag");
    CHECK(strcmp(saved[1].ssid, "Cafe") == 0, "saved ssid %s",
          saved[1].ssid);
  }
  CHECK(bd_net_forget(net, 1, error, sizeof(error)), "forget: %s", error);
  CHECK(read_text(log, calls, sizeof(calls)), "read call log after forget");
  CHECK(strstr(calls, "remove_network 1") != NULL, "remove_network");

  CHECK(bd_net_set_enabled(net, true, error, sizeof(error)),
        "enable: %s", error);
  CHECK(read_text(log, calls, sizeof(calls)), "read call log after enable");
  CHECK(strstr(calls, "ip link set dev wlan0 up") != NULL, "ip up");
  CHECK(bd_net_set_enabled(net, false, error, sizeof(error)),
        "disable: %s", error);
  CHECK(read_text(log, calls, sizeof(calls)), "read call log after disable");
  CHECK(strstr(calls, "ip link set dev wlan0 down") != NULL, "ip down");

  CHECK(bd_net_suspend(error, sizeof(error)), "suspend: %s", error);
  CHECK(bd_net_resume(error, sizeof(error)), "resume: %s", error);
  CHECK(read_text(log, calls, sizeof(calls)), "read call log after resume");
  CHECK(strstr(calls, "wpa_supplicant -B") != NULL, "supplicant restart");

  bd_net_destroy(net);
  char cleanup[600];
  snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", root);
  if (system(cleanup) != 0) {
    fprintf(stderr, "device_net_test: cleanup failed\n");
  }
  if (failures != 0) {
    fprintf(stderr, "device_net_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("device_net_test: ok\n");
  return 0;
}
