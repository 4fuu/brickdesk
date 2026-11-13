#define _POSIX_C_SOURCE 200809L

#include "brickdesk/device_power.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *backend_status_name(bd_power_backend_status status) {
  switch (status) {
    case BD_POWER_BACKEND_OK:
      return "ok";
    case BD_POWER_BACKEND_ERROR:
      return "error";
    case BD_POWER_BACKEND_DISABLED:
      return "disabled";
    case BD_POWER_BACKEND_UNSUPPORTED:
      return "unsupported";
    case BD_POWER_BACKEND_NO_EFFECT:
      return "no-effect";
  }
  return "error";
}

static void print_usage(FILE *stream) {
  fprintf(stream,
          "usage: bd-power [options]\n"
          "\n"
          "Read-only probe by default. State-changing actions need explicit\n"
          "opt-in flags and are diagnostics for hardware bring-up.\n"
          "\n"
          "  --root DIR              sysfs root (default /; use a fixture for tests)\n"
          "  --mem-sleep MODE        request mem_sleep mode, e.g. deep or s2idle\n"
          "  --allow-state-changes   allow writes to /sys/power and reboot(2)\n"
          "  --deep-sleep            run one deep sleep/resume cycle (diagnostic)\n"
          "  --poweroff              request standard Linux poweroff\n"
          "  --reboot                request reboot\n"
          "  --yes                   confirm poweroff/reboot\n"
          "  -h, --help              show this help\n"
          "\n"
          "Diagnostic deep sleep calls only the kernel backend: caller hooks,\n"
          "service quiesce and the 20-cycle acceptance matrix are not run.\n");
}

static void print_probe(const bd_power_probe *probe,
                        const bd_power_linux *linux) {
  printf("probe.read_only=yes\n");
  printf("sys.power.state=%s\n", probe->states);
  printf("sys.power.mem_supported=%s\n",
         probe->mem_supported ? "yes" : "no");
  printf("sys.power.mem_sleep.present=%s\n",
         probe->mem_sleep_present ? "yes" : "no");
  if (probe->mem_sleep_present) {
    printf("sys.power.mem_sleep.modes=%s\n", probe->mem_sleep_modes);
    printf("sys.power.mem_sleep.current=%s\n", probe->mem_sleep_current);
  }
  if (probe->usb_online_known) {
    printf("power.usb_online=%d\n", probe->usb_online ? 1 : 0);
  } else {
    printf("power.usb_online=unknown\n");
  }
  if (probe->battery_capacity_known) {
    printf("power.battery_capacity=%d\n",
           probe->battery_capacity_percent);
  } else {
    printf("power.battery_capacity=unknown\n");
  }
  printf("pmic.safe_poweroff=unsupported\n");
  printf("pmic.blocker=%s\n", probe->pmic_blocker);
  printf("backend.state_changes=%s\n",
         linux->allow_state_changes ? "enabled" : "disabled");
  if (probe->error[0] != '\0') {
    printf("probe.error=%s\n", probe->error);
  }
}

static int run_probe(const bd_power_linux *linux) {
  bd_power_probe probe;
  bool ok = bd_power_linux_probe(linux, &probe);
  print_probe(&probe, linux);
  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
  const char *root = NULL;
  const char *mem_sleep = NULL;
  bool allow_state_changes = false;
  bool deep_sleep = false;
  bool poweroff = false;
  bool reboot = false;
  bool yes = false;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (strcmp(arg, "--root") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "bd-power: --root needs a directory\n");
        return 2;
      }
      root = argv[++i];
      continue;
    }
    if (strcmp(arg, "--mem-sleep") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "bd-power: --mem-sleep needs a mode\n");
        return 2;
      }
      mem_sleep = argv[++i];
      continue;
    }
    if (strcmp(arg, "--allow-state-changes") == 0) {
      allow_state_changes = true;
      continue;
    }
    if (strcmp(arg, "--deep-sleep") == 0) {
      deep_sleep = true;
      continue;
    }
    if (strcmp(arg, "--poweroff") == 0) {
      poweroff = true;
      continue;
    }
    if (strcmp(arg, "--reboot") == 0) {
      reboot = true;
      continue;
    }
    if (strcmp(arg, "--yes") == 0) {
      yes = true;
      continue;
    }
    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      print_usage(stdout);
      return 0;
    }
    fprintf(stderr, "bd-power: unknown option: %s\n", arg);
    print_usage(stderr);
    return 2;
  }

  int actions = (deep_sleep ? 1 : 0) + (poweroff ? 1 : 0) +
                (reboot ? 1 : 0);
  if (actions > 1) {
    fprintf(stderr, "bd-power: choose only one action\n");
    return 2;
  }
  if (actions > 0 && !allow_state_changes) {
    fprintf(stderr,
            "bd-power: refusing state change without "
            "--allow-state-changes\n");
    return 2;
  }
  if ((poweroff || reboot) && !yes) {
    fprintf(stderr, "bd-power: poweroff/reboot also require --yes\n");
    return 2;
  }

  char error[256];
  bd_power_linux linux;
  if (!bd_power_linux_init(&linux, root, allow_state_changes, mem_sleep,
                           error, sizeof(error))) {
    fprintf(stderr, "bd-power: %s\n", error);
    return 2;
  }

  if (!deep_sleep && !poweroff && !reboot) {
    return run_probe(&linux);
  }

  bd_power_backend backend = bd_power_linux_backend(&linux);
  if (deep_sleep) {
    if (backend.deep_sleep == NULL) {
      fprintf(stderr, "bd-power: no deep sleep backend\n");
      return 1;
    }
    printf("action=deep-sleep\n");
    printf("warning=backend diagnostic only; caller hooks and service quiesce "
           "are not exercised\n");
    fflush(stdout);
    errno = 0;
    bd_power_backend_status status = backend.deep_sleep(backend.user);
    printf("status=%s\n", backend_status_name(status));
    if (status == BD_POWER_BACKEND_ERROR) {
      printf("errno=%d\n", errno);
      printf("error=%s\n", strerror(errno));
      return 1;
    }
    return status == BD_POWER_BACKEND_OK ? 0 : 1;
  }

  if (backend.shutdown == NULL) {
    fprintf(stderr, "bd-power: no shutdown backend\n");
    return 1;
  }
  printf("action=%s\n", poweroff ? "poweroff" : "reboot");
  printf("warning=standard Linux reboot(2) path; PMIC soft power-off is "
         "unsupported\n");
  fflush(stdout);
  errno = 0;
  bd_power_backend_status status = backend.shutdown(backend.user, reboot);
  printf("status=%s\n", backend_status_name(status));
  if (status == BD_POWER_BACKEND_ERROR) {
    printf("errno=%d\n", errno);
    printf("error=%s\n", strerror(errno));
  }
  return 1;
}
