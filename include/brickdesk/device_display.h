#ifndef BRICKDESK_DEVICE_DISPLAY_H
#define BRICKDESK_DEVICE_DISPLAY_H

/*
 * Brick Pro display enhancement backend.
 *
 * Four vendor sysfs attributes under /sys/class/disp/disp/attr carry the same
 * values as NextUI's tg5040 msettings backend (fixed commit b6ec1343):
 *
 *   color_temperature   -200..200
 *   enhance_contrast      10..100
 *   enhance_saturation     0..100
 *   enhance_bright        10..100
 *
 * White-point correction uses the vendor gamma table through /dev/disp:
 * DISP_LCD_SET_GAMMA_TABLE (0x10b), DISP_LCD_GAMMA_CORRECTION_ENABLE (0x10c)
 * and DISP_LCD_GAMMA_CORRECTION_DISABLE (0x10d).  Gains are 0..200 with 100
 * neutral; Brick Pro defaults are 100/94/54.  The LUT is generated here from
 * the sRGB transfer function; no vendor code or table is copied.
 *
 * USB-C DP output is detected read-only from the /sys/class/drm directory (card*-DP-* connectors).
 * The vendor kernel does not expose a documented switch node in the research,
 * so this module reports the connector state and never guesses a write.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BD_DISPLAY_PATH_MAX 512
#define BD_DISPLAY_MESSAGE_MAX 256

typedef enum bd_display_result {
  BD_DISPLAY_OK = 0,
  BD_DISPLAY_ERR_ARGUMENT,
  BD_DISPLAY_ERR_UNSUPPORTED,
  BD_DISPLAY_ERR_IO,
  BD_DISPLAY_ERR_RANGE,
  BD_DISPLAY_ERR_STATE
} bd_display_result;

typedef struct bd_display_error {
  bd_display_result code;
  char message[BD_DISPLAY_MESSAGE_MAX];
} bd_display_error;

const char *bd_display_result_name(bd_display_result result);
void bd_display_error_clear(bd_display_error *error);

typedef struct bd_display_config {
  /* Prefix before /sys; NULL or "" is the real /sys. */
  const char *sysfs_root;
  /* /dev/disp path; NULL is the real device. */
  const char *disp_path;
} bd_display_config;

typedef struct bd_display_settings {
  int color_temperature; /* -200..200 */
  int contrast;          /* 10..100 */
  int saturation;        /* 0..100 */
  int exposure;          /* 10..100 */
  bool white_point;
  int red_gain;          /* 0..200, 100 neutral */
  int green_gain;
  int blue_gain;
} bd_display_settings;

typedef struct bd_display {
  bool available;
  bool white_point_supported;
  char root[BD_DISPLAY_PATH_MAX];
  char attr_dir[BD_DISPLAY_PATH_MAX];
  char disp_path[BD_DISPLAY_PATH_MAX];
  char white_point_error[BD_DISPLAY_MESSAGE_MAX];
  struct {
    const void *file_io; /* internal test seam */
  } internal;
} bd_display;

bd_display_result bd_display_probe(bd_display *display,
                                   const bd_display_config *config,
                                   bd_display_error *error);

/*
 * Writes the four attributes in NextUI's order and then applies the white
 * point.  A missing gamma device does not fail the four sysfs attributes; it
 * sets white_point_supported=false and white_point_error.  A sysfs write
 * failure is an I/O error and stops the sequence.
 */
bd_display_result bd_display_apply(bd_display *display,
                                   const bd_display_settings *settings,
                                   bd_display_error *error);

/* Reads the /sys/class/drm directory (card*-DP-* connectors) without writing. */
bool bd_display_dp_connected(const bd_display *display, bool *connected,
                             char *connector, size_t connector_size);

#ifdef __cplusplus
}
#endif

#endif /* BRICKDESK_DEVICE_DISPLAY_H */
