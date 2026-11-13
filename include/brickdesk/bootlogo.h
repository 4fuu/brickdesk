#ifndef BRICKDESK_BOOTLOGO_H
#define BRICKDESK_BOOTLOGO_H

/*
 * Boot logo validation and installation for the Brick Pro boot partition.
 *
 * The vendor bootloader reads bootlogo.bmp from the FAT boot partition
 * (/dev/mmcblk0p1 on TG4040).  The file must be an uncompressed 24- or 32-bit
 * BMP at the panel resolution.  Everything is checked before the partition is
 * touched; the first installation keeps a backup of the stock logo.
 *
 * This module never reboots the device.  The caller decides when to reboot.
 */

#include <stdbool.h>
#include <stddef.h>

#define BD_BOOTLOGO_WIDTH 1024
#define BD_BOOTLOGO_HEIGHT 768
#define BD_BOOTLOGO_NAME "bootlogo.bmp"
#define BD_BOOTLOGO_BACKUP "bootlogo.brickdesk-original.bmp"

typedef enum bd_bootlogo_result {
  BD_BOOTLOGO_OK = 0,
  BD_BOOTLOGO_ERR_ARGUMENT,
  BD_BOOTLOGO_ERR_FORMAT,
  BD_BOOTLOGO_ERR_IO,
  BD_BOOTLOGO_ERR_MOUNT
} bd_bootlogo_result;

typedef struct bd_bootlogo_info {
  int width;
  int height;
  int bits_per_pixel;
  unsigned file_bytes;
} bd_bootlogo_info;

const char *bd_bootlogo_result_name(bd_bootlogo_result result);

/*
 * Parses the BMP header and validates dimensions, bit depth, compression and
 * file length.  Never opens the boot partition.
 */
bd_bootlogo_result bd_bootlogo_check(const char *path, bd_bootlogo_info *info,
                                     char *error, size_t error_size);

/*
 * Mounts the boot partition (or BRICKDESK_BOOTLOGO_DIR when set), validates
 * `path` and copies it to bootlogo.bmp.  With restore=true it copies the
 * saved backup instead.  The partition is unmounted again; the device is not
 * rebooted.  Returns MOUNT when the partition cannot be mounted.
 */
bd_bootlogo_result bd_bootlogo_apply(const char *path, bool restore,
                                     char *error, size_t error_size);

#endif /* BRICKDESK_BOOTLOGO_H */
