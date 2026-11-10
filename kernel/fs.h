/* INPS filesystem: one directory sector (10 entries) + contiguous file extents.
 * The on-disk format is UNCHANGED, so existing installs keep working. */
#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "blockdev.h"
#include "config.h"

typedef struct {
    char     filename[FS_NAME_MAX];
    uint32_t start_sector;
    uint32_t num_sectors;
    uint32_t size;
    uint8_t  used;
    uint8_t  rsv[3];
} __attribute__((packed)) FsEntry;

_Static_assert(sizeof(FsEntry) == 48, "FsEntry is part of the on-disk format");

/* Mounts the filesystem on dev and caches the directory (read once, not on every call).
 * Returns false if the INPS signature is missing. */
bool fs_mount(const BlockDev* dev);
bool fs_is_mounted(void);

int          fs_count(void);                 /* number of files */
const char*  fs_name(int index);             /* index in [0, fs_count()) */
bool         fs_exists(const char* name);
/* Returns a malloc'd, NUL terminated copy of the file (size excludes the NUL), or NULL. */
char*        fs_read(const char* name, size_t* size);
/* Creates or REPLACES a file. */
bool         fs_write(const char* name, const void* data, size_t len);
bool         fs_append(const char* name, const void* data, size_t len);
bool         fs_delete(const char* name);

bool         fs_valid_name(const char* name);

#endif
