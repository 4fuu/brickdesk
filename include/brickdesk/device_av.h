#ifndef BRICKDESK_DEVICE_AV_H
#define BRICKDESK_DEVICE_AV_H

/*
 * Small display/audio capability layer for the TrimUI Brick Pro (TG4040).
 *
 * The API is deliberately narrow: brightness read/write, ALSA volume/mute and
 * the speaker/headphone path.  It does not own power, PMIC, LEDs, rumble or
 * input.  The settings page and the in-app menu both use percent values in
 * 0..100, so the same backend can be installed in bd_settings_system and
 * bd_menu_system_ops without a second unit conversion.
 *
 * Every operation returns a bd_av_result.  A refusal is never reported as
 * success: missing hardware, an unbound control, an out-of-range request and
 * an I/O failure each have a distinct result and an optional human-readable
 * message.  Probing never writes.
 *
 * Scope and evidence are documented in docs/device-av.md.  In particular,
 * /dev/disp is only used for a recognised model and only after its brightness
 * get ioctl succeeds; an unknown device is never written through a guessed
 * path or ioctl.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BD_AV_PATH_MAX 256
#define BD_AV_NAME_MAX 64
#define BD_AV_MESSAGE_MAX 192
#define BD_AV_MAX_CONTROLS 64

typedef enum bd_av_result {
  BD_AV_OK = 0,
  BD_AV_ERR_ARGUMENT,
  BD_AV_ERR_UNSUPPORTED,
  BD_AV_ERR_IO,
  BD_AV_ERR_RANGE,
  BD_AV_ERR_STATE
} bd_av_result;

typedef struct bd_av_error {
  bd_av_result code;
  char message[BD_AV_MESSAGE_MAX];
} bd_av_error;

const char *bd_av_result_name(bd_av_result result);
void bd_av_error_clear(bd_av_error *error);

/* ---------------------------------------------------------------------- */
/* Brightness                                                             */
/* ---------------------------------------------------------------------- */

typedef enum bd_av_brightness_backend {
  BD_AV_BRIGHTNESS_AUTO = 0,
  BD_AV_BRIGHTNESS_SYSFS,
  BD_AV_BRIGHTNESS_DISP,
  BD_AV_BRIGHTNESS_NONE
} bd_av_brightness_backend;

const char *bd_av_brightness_backend_name(bd_av_brightness_backend backend);

typedef struct bd_av_brightness_config {
  /*
   * Prefix before /sys.  NULL or "" means the real /sys.  Tests point this at
   * a fake tree such as "/tmp/bd-av/sysfs".
   */
  const char *sysfs_root;
  /*
   * Explicit backlight directory.  An absolute path is used as-is; a relative
   * path is resolved against sysfs_root.  NULL auto-discovers the first
   * usable <sysfs_root>/sys/class/backlight/NAME entry.
   */
  const char *backlight;
  /* Default /dev/disp. */
  const char *disp_path;
  /* Default /etc/model.  Read only by the /dev/disp backend. */
  const char *model_path;
  /*
   * Comma-separated exact model allowlist for the /dev/disp backend.  NULL
   * defaults to "TG4040".  An empty list disables the backend.  Wildcards are
   * intentionally not supported.
   */
  const char *allowed_models;
  /* AUTO prefers sysfs, then /dev/disp. */
  bd_av_brightness_backend backend;
  /* Raw upper bound for /dev/disp.  0 means the evidenced default, 255. */
  int disp_raw_max;
} bd_av_brightness_config;

typedef struct bd_av_brightness {
  bd_av_brightness_backend backend;
  /* Backlight directory for sysfs, or the disp device path. */
  char device[BD_AV_PATH_MAX];
  /* Device model read for the disp backend; empty for sysfs. */
  char model[BD_AV_NAME_MAX];
  int raw_min;
  int raw_max;
  bool readable;
  bool writable;
  struct {
    const void *disp_io; /* internal: bd_av_disp_io */
  } internal;
} bd_av_brightness;

/*
 * Probes without writing.  A successful probe leaves backend set to SYSFS or
 * DISP.  A failed probe leaves backend NONE and fills error when supplied.
 *
 * sysfs is accepted when a backlight directory has readable brightness and
 * max_brightness files and max_brightness > 0.  /dev/disp is accepted only
 * when the model file matches allowed_models and the get-brightness ioctl
 * (0x103) returns a value inside 0..disp_raw_max.  That get call is the
 * evidence gate: if it is unsupported, the backend is reported unsupported
 * instead of writing blindly.
 */
bd_av_result bd_av_brightness_probe(bd_av_brightness *brightness,
                                    const bd_av_brightness_config *config,
                                    bd_av_error *error);

/* Reads the raw backlight value through the active backend. */
bd_av_result bd_av_brightness_get_raw(const bd_av_brightness *brightness,
                                      int *raw, bd_av_error *error);

/* Writes and reads back a raw value inside raw_min..raw_max. */
bd_av_result bd_av_brightness_set_raw(bd_av_brightness *brightness, int raw,
                                      bd_av_error *error);

/* Reads the current brightness as 0..100. */
bd_av_result bd_av_brightness_get(const bd_av_brightness *brightness,
                                  int *percent, bd_av_error *error);

/*
 * Sets brightness from 0..100.  Values outside that range are rejected with
 * BD_AV_ERR_RANGE and no write.  The raw value is rounded and the result is
 * read back; a backend that does not match the request is an I/O error.
 */
bd_av_result bd_av_brightness_set(bd_av_brightness *brightness, int percent,
                                  bd_av_error *error);

/* ---------------------------------------------------------------------- */
/* Audio                                                                  */
/* ---------------------------------------------------------------------- */

typedef enum bd_av_volume_map {
  /* Default for "digital volume": raw 0 is loudest, raw max is quietest. */
  BD_AV_VOLUME_MAP_DEFAULT = 0,
  BD_AV_VOLUME_MAP_INVERTED,
  BD_AV_VOLUME_MAP_LINEAR
} bd_av_volume_map;

typedef struct bd_av_audio_config {
  /* Directory containing controlC<N>; default /dev/snd. */
  const char *snd_root;
  /* Default /proc/asound/cards.  Used only to discover the card index. */
  const char *proc_cards;
  /* Default /sys/class/speaker/mute; "" disables the hardware mute path. */
  const char *speaker_mute;
  /* Default audiocodec. */
  const char *card_name;
  /* When true, use card_index instead of discovering by card_name. */
  bool card_by_index;
  int card_index;

  /*
   * Exact ALSA mixer control names.  They are matched with strcmp against
   * enumerated MIXER controls; there is no fuzzy or hard-coded-by-number
   * fallback.  "" disables the role.  A missing or wrong-typed control simply
   * leaves the matching capability false.
   *
   * Defaults, evidenced on the Brick Pro:
   *   volume_control      "digital volume"      0..63, inverted
   *   headphone_switch    "Headphone Switch"    boolean
   *   speaker_switch      "HpSpeaker Switch"    boolean
   *   headphone_volume    "Headphone Volume"    0..7
   *   mute_control        ""                    use speaker_mute instead
   */
  const char *volume_control;
  bd_av_volume_map volume_map;
  const char *mute_control;
  const char *speaker_switch;
  const char *headphone_switch;
  const char *headphone_volume;

  /*
   * When false (the default), a headphone jack edge fed through
   * bd_av_audio_set_jack() switches the path automatically.  When true the
   * jack is recorded but the caller owns bd_av_audio_set_route().
   */
  bool manual_route;
} bd_av_audio_config;

typedef enum bd_av_control_type {
  BD_AV_CONTROL_OTHER = 0,
  BD_AV_CONTROL_BOOLEAN,
  BD_AV_CONTROL_INTEGER,
  BD_AV_CONTROL_INTEGER64,
  BD_AV_CONTROL_ENUMERATED
} bd_av_control_type;

typedef struct bd_av_control_info {
  char name[BD_AV_NAME_MAX];
  unsigned int index;
  bd_av_control_type type;
  unsigned int count;
  long min;
  long max;
  long step;
  bool readable;
  bool writable;
  /* ALSA identity; zero numid means "look up by name". */
  unsigned int numid;
  unsigned int device;
  unsigned int subdevice;
  unsigned int access;
} bd_av_control_info;

typedef enum bd_av_route {
  BD_AV_ROUTE_UNKNOWN = 0,
  BD_AV_ROUTE_NONE,
  BD_AV_ROUTE_SPEAKER,
  BD_AV_ROUTE_HEADPHONE,
  BD_AV_ROUTE_BOTH,
  BD_AV_ROUTE_AUTO
} bd_av_route;

const char *bd_av_route_name(bd_av_route route);
const char *bd_av_control_type_name(bd_av_control_type type);

typedef struct bd_av_audio {
  int card_index;
  char card_id[BD_AV_NAME_MAX];

  /* Names as configured; empty means the role was disabled. */
  char volume_control[BD_AV_NAME_MAX];
  char mute_control[BD_AV_NAME_MAX];
  char speaker_switch[BD_AV_NAME_MAX];
  char headphone_switch[BD_AV_NAME_MAX];
  char headphone_volume[BD_AV_NAME_MAX];

  bool has_volume;
  bool volume_inverted;
  long volume_min;
  long volume_max;
  long volume_step;
  unsigned int volume_count;

  bool has_mute;
  bool has_route;
  bool has_headphone_volume;

  /* Last explicit mute request; the hardware is always read back. */
  bool muted;
  bool jack_known;
  bool headphone_plugged;
  bool manual_route;

  size_t control_count;
  bool controls_truncated;
  bd_av_control_info controls[BD_AV_MAX_CONTROLS];

  struct {
    const void *mixer_io; /* internal: bd_av_mixer_io */
    int fd;
    int volume_index;
    int mute_index;
    int speaker_index;
    int headphone_index;
    int headphone_volume_index;
    bool mute_is_alsa;
    char mute_path[BD_AV_PATH_MAX];
  } internal;
} bd_av_audio;

/*
 * Opens the configured card and enumerates all MIXER controls.  Finding the
 * card is required; finding a particular role is not.  Callers must check
 * has_volume/has_mute/has_route before exposing the matching action.  The
 * control table is available through bd_av_audio_control_count/at for
 * diagnostics, so a retail unit with different names is visible instead of
 * silently guessed.
 */
bd_av_result bd_av_audio_open(bd_av_audio *audio,
                              const bd_av_audio_config *config,
                              bd_av_error *error);

void bd_av_audio_close(bd_av_audio *audio);

size_t bd_av_audio_control_count(const bd_av_audio *audio);
const bd_av_control_info *bd_av_audio_control_at(const bd_av_audio *audio,
                                                 size_t index);

/* Reads the bound volume control and converts it to 0..100.  With a
 * multi-channel control, the first channel decides the percentage. */
bd_av_result bd_av_audio_get_volume(const bd_av_audio *audio, int *percent,
                                    bd_av_error *error);

/*
 * Writes 0..100 to the bound volume control, then reads it back.  Values
 * outside 0..100 are rejected without a write.  When a speaker mute is bound,
 * volume 0 engages it and a later non-zero volume clears it unless the caller
 * explicitly muted with bd_av_audio_set_muted().
 *
 * Writes are not transactional: the volume may have changed when a later
 * mute write fails, and a route may have one switch changed when the other
 * fails.  After an error, re-read volume/mute/route instead of assuming
 * nothing happened.
 */
bd_av_result bd_av_audio_set_volume(bd_av_audio *audio, int percent,
                                    bd_av_error *error);

/* Reads the bound mute source: ALSA switch when configured, else sysfs. */
bd_av_result bd_av_audio_get_muted(const bd_av_audio *audio, bool *muted,
                                   bd_av_error *error);

/* Writes and reads back the bound mute source. */
bd_av_result bd_av_audio_set_muted(bd_av_audio *audio, bool muted,
                                   bd_av_error *error);

/*
 * Reads the two path switches and classifies them.  BOTH/NONE are real
 * hardware states and are not treated as an exclusive route; callers can
 * still use bd_av_audio_set_route() to force one path.
 */
bd_av_result bd_av_audio_get_route(const bd_av_audio *audio,
                                   bd_av_route *route, bd_av_error *error);

/*
 * Forces SPEAKER or HEADPHONE by writing both path switches and reading them
 * back.  AUTO selects the route from the last bd_av_audio_set_jack() value;
 * with no jack edge recorded it is rejected as BD_AV_ERR_STATE.
 */
bd_av_result bd_av_audio_set_route(bd_av_audio *audio, bd_av_route route,
                                   bd_av_error *error);

/*
 * Records a headphone jack edge, normally from the input layer's
 * SW_HEADPHONE_INSERT event.  In automatic mode this switches the path; in
 * manual mode it only records state.  If the device has no path switches the
 * state is still recorded and the result is BD_AV_ERR_UNSUPPORTED, so callers
 * can tell that no hardware action happened.
 */
bd_av_result bd_av_audio_set_jack(bd_av_audio *audio, bool plugged,
                                  bd_av_error *error);

#ifdef __cplusplus
}
#endif

#endif /* BRICKDESK_DEVICE_AV_H */
