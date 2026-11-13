#ifndef BRICKDESK_UINPUT_H
#define BRICKDESK_UINPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A virtual evdev device mirroring one physical gamepad.
 *
 * brickdesk-inputd uses it for hotkeys with suppress=true: the physical device
 * is grabbed, every event except the suppressed binding keys is forwarded
 * here, and the foreground app reads the virtual node instead.  Creation
 * fails cleanly when /dev/uinput is missing so the daemon can keep the
 * physical device ungrabbed and never steal input without a replacement.
 */

struct bd_evdev;
typedef struct bd_uinput bd_uinput;

bd_uinput *bd_uinput_create(const struct bd_evdev *source, char *error,
                            size_t error_size);
bool bd_uinput_emit(bd_uinput *uinput, uint16_t type, uint16_t code,
                    int32_t value);
void bd_uinput_sync(bd_uinput *uinput);
/* Releases every key this node still reports as pressed. */
void bd_uinput_release_all(bd_uinput *uinput);
void bd_uinput_destroy(bd_uinput *uinput);

#endif /* BRICKDESK_UINPUT_H */
