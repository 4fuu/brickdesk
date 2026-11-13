#ifndef BRICKDESK_INPUT_CONTROLS_H
#define BRICKDESK_INPUT_CONTROLS_H

/*
 * Vendor trimui_inputd control files.
 *
 * The stock daemon watches these paths; the file exists = feature enabled:
 *
 *   /tmp/trimui_inputd/input_dpad_to_joystick
 *   /tmp/trimui_inputd/input_no_dpad
 *   /tmp/trimui_inputd/turbo_a ... turbo_r2
 *
 * NextUI's tg5040 backend uses touch()/unlink() for the same files.  These
 * helpers create the directory on demand and are safe to call when the daemon
 * is absent (the desktop still records the preference).
 *
 * BRICKDESK_INPUTD_CONTROL_DIR overrides the directory for tests.
 */

#include <stdbool.h>

bool bd_inputd_set_dpad_to_joystick(bool enabled);
bool bd_inputd_set_no_dpad(bool enabled);
bool bd_inputd_set_turbo(bool enabled);
bool bd_inputd_turbo_enabled(void);

#endif /* BRICKDESK_INPUT_CONTROLS_H */
