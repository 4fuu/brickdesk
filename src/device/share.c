#define _POSIX_C_SOURCE 200809L

#include "brickdesk/device_share.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "device_process.h"

struct bd_share {
  char iface[32];
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

static const char *service_process(bd_share_service service) {
  switch (service) {
    case BD_SHARE_SSH:
      return "sshd";
    case BD_SHARE_FTP:
      return "vsftpd";
    case BD_SHARE_SAMBA:
      return "smbd";
    case BD_SHARE_ADB:
      return "adbd";
    default:
      return "";
  }
}

static bool process_running(const char *name) {
  char *argv[] = {"pgrep", "-x", (char *)name, NULL};
  int code = 0;
  char error[64];
  if (!bd_process_run(argv, NULL, 0, &code, error, sizeof(error))) {
    return false;
  }
  return code == 0;
}

static bool service_start(bd_share_service service, char *error,
                          size_t error_size) {
  char *argv[10];
  size_t index = 0;
  const char *binary = NULL;
  switch (service) {
    case BD_SHARE_SSH:
      binary = access("/usr/sbin/sshd", X_OK) == 0
                   ? "/usr/sbin/sshd"
                   : (access("/usr/sbin/dropbear", X_OK) == 0
                          ? "/usr/sbin/dropbear"
                          : "sshd");
      argv[index++] = (char *)binary;
      argv[index++] = (char *)(binary != NULL &&
                                       strstr(binary, "dropbear") != NULL
                                   ? "-R"
                                   : "-D");
      if (binary != NULL && strstr(binary, "dropbear") != NULL) {
        argv[index++] = (char *)"-F";
        argv[index++] = (char *)"-E";
      }
      break;
    case BD_SHARE_FTP:
      binary = access("/usr/sbin/vsftpd", X_OK) == 0
                   ? "/usr/sbin/vsftpd"
                   : (access("/usr/bin/tcpsvd", X_OK) == 0 ? "/usr/bin/tcpsvd"
                                                           : "vsftpd");
      argv[index++] = (char *)binary;
      if (strstr(binary, "tcpsvd") != NULL) {
        argv[index++] = (char *)"-vE";
        argv[index++] = (char *)"0.0.0.0";
        argv[index++] = (char *)"21";
        argv[index++] = (char *)"ftpd";
        argv[index++] = (char *)"-w";
      }
      break;
    case BD_SHARE_SAMBA:
      binary = access("/usr/sbin/smbd", X_OK) == 0 ? "/usr/sbin/smbd" : "smbd";
      argv[index++] = (char *)binary;
      argv[index++] = (char *)"-D";
      break;
    case BD_SHARE_ADB:
      binary = access("/usr/bin/adbd", X_OK) == 0
                   ? "/usr/bin/adbd"
                   : (access("/sbin/adbd", X_OK) == 0 ? "/sbin/adbd" : "adbd");
      argv[index++] = (char *)binary;
      break;
    default:
      return false;
  }
  argv[index] = NULL;
  if (service == BD_SHARE_SSH && binary != NULL &&
      strstr(binary, "dropbear") == NULL) {
    /* OpenSSH -D stays in the foreground; detach it instead. */
    char *spawn_argv[] = {(char *)binary, (char *)"-D", NULL};
    return bd_process_spawn(spawn_argv, error, error_size);
  }
  return bd_process_spawn(argv, error, error_size);
}

static bool service_stop(bd_share_service service, char *error,
                         size_t error_size) {
  const char *name = service_process(service);
  if (service == BD_SHARE_SSH) {
    char *sshd[] = {"killall", "sshd", NULL};
    char *dropbear[] = {"killall", "dropbear", NULL};
    int code = 0;
    char ignored[64];
    bd_process_run(sshd, NULL, 0, &code, ignored, sizeof(ignored));
    bd_process_run(dropbear, NULL, 0, &code, ignored, sizeof(ignored));
    return true;
  }
  char *argv[] = {"killall", (char *)name, NULL};
  int code = 0;
  char ignored[64];
  if (!bd_process_run(argv, NULL, 0, &code, ignored, sizeof(ignored))) {
    set_error(error, error_size, "%s", ignored);
    return false;
  }
  if (code != 0) {
    set_error(error, error_size, "cannot stop %s", name);
    return false;
  }
  return true;
}

bd_share *bd_share_create(const char *iface) {
  bd_share *share = (bd_share *)calloc(1, sizeof(*share));
  if (share == NULL) {
    return NULL;
  }
  copy_text(share->iface, sizeof(share->iface),
            iface != NULL ? iface : "wlan0");
  return share;
}

void bd_share_destroy(bd_share *share) {
  free(share);
}

static void read_ip(bd_share *share, char *out, size_t out_size) {
  out[0] = '\0';
  char *argv[] = {"ip", "-4", "addr", "show", "dev", share->iface, NULL};
  char output[4096];
  int code = 0;
  char error[64];
  if (!bd_process_run(argv, output, sizeof(output), &code, error,
                      sizeof(error))) {
    return;
  }
  const char *inet = strstr(output, "inet ");
  if (inet == NULL) {
    return;
  }
  inet += 5;
  size_t length = strcspn(inet, "/ \n");
  if (length >= out_size) {
    length = out_size - 1;
  }
  memcpy(out, inet, length);
  out[length] = '\0';
}

bool bd_share_get_status(bd_share *share, bd_share_status *status) {
  if (share == NULL || status == NULL) {
    return false;
  }
  memset(status, 0, sizeof(*status));
  status->ssh = process_running("sshd") || process_running("dropbear");
  status->ftp = process_running("vsftpd") || process_running("tcpsvd");
  status->samba = process_running("smbd");
  status->adb = process_running("adbd");
  read_ip(share, status->ip, sizeof(status->ip));
  status->usb_mode[0] = '\0';
  return true;
}

bool bd_share_set_service(bd_share *share, bd_share_service service,
                          bool enabled, char *error, size_t error_size) {
  if (share == NULL) {
    set_error(error, error_size, "missing share backend");
    return false;
  }
  return enabled ? service_start(service, error, error_size)
                 : service_stop(service, error, error_size);
}

bool bd_share_set_usb_mode(bd_share *share, const char *mode, char *error,
                           size_t error_size) {
  (void)share;
  if (mode == NULL || mode[0] == '\0') {
    set_error(error, error_size, "missing USB mode");
    return false;
  }
  const char *script = "/usr/trimui/bin/usbmode";
  if (access(script, X_OK) != 0) {
    set_error(error, error_size, "usbmode script not found; saved only");
    return false;
  }
  char *argv[] = {(char *)script, (char *)mode, NULL};
  int code = 0;
  if (!bd_process_run(argv, NULL, 0, &code, error, error_size)) {
    return false;
  }
  if (code != 0) {
    set_error(error, error_size, "usbmode %s failed (%d)", mode, code);
    return false;
  }
  return true;
}
