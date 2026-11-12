#ifndef BRICKDESK_PLUGIN_MANIFEST_H
#define BRICKDESK_PLUGIN_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>

#include "brickdesk/plugin.h"

/* Loads and validates <dir>/plugin.json. */
bool bd_plugin_manifest_load(const char *dir, bd_plugin_manifest *out,
                             char *error, size_t error_size);

#endif /* BRICKDESK_PLUGIN_MANIFEST_H */
