#ifdef HOSTED_TEST
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#else
#include "libc.h"
#endif
#include "fs.h"

/* Fixes relative to the original create_file()/read_file():
 *  - Replacing a file used to ADD a second directory entry with the same name, and
 *    read_file() always found the OLD one first: `file update` (append) was invisible and
 *    every rewrite leaked a slot.  fs_write() now replaces in place (or relocates).
 *  - Space was allocated bump-pointer only, so deleted files were never reclaimed.
 *    Allocation is now first-fit over the gaps between extents.
 *  - File names were strcpy()'d into a 32-byte field: overflow.  Names are validated.
 *  - The directory sector was re-read from disk on every operation; it is cached now.
 *  - read_file() read into a temporary buffer and then copied into a second one.
 */

static const BlockDev* dev;
static FsEntry dir[FS_MAX_FILES];
static bool mounted;

static bool dir_flush(void) {
    uint8_t sec[SECTOR_SIZE];
    memset(sec, 0, sizeof sec);
    memcpy(sec, dir, sizeof dir);
    memcpy(sec + FS_SIGNATURE_OFF, FS_SIGNATURE, 4);
    return bd_write(dev, FS_DIR_SECTOR, 1, sec);
}

bool fs_mount(const BlockDev* d) {
    uint8_t sec[SECTOR_SIZE];
    mounted = false;
    if (!bd_read(d, FS_DIR_SECTOR, 1, sec)) return false;
    if (memcmp(sec + FS_SIGNATURE_OFF, FS_SIGNATURE, 4) != 0) return false;
    memcpy(dir, sec, sizeof dir);
    for (int i = 0; i < FS_MAX_FILES; i++)                 /* never trust on-disk strings */
        dir[i].filename[FS_NAME_MAX - 1] = '\0';
    dev = d;
    mounted = true;
    return true;
}

bool fs_is_mounted(void) { return mounted; }

bool fs_valid_name(const char* name) {
    size_t n = strlen(name);
    if (n == 0 || n >= FS_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)name[i] < 0x21 || (unsigned char)name[i] > 0x7E) return false;
    return true;
}

static int find(const char* name) {
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (dir[i].used && strcmp(dir[i].filename, name) == 0) return i;
    return -1;
}

int fs_count(void) {
    int n = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) n += dir[i].used ? 1 : 0;
    return n;
}

const char* fs_name(int index) {
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (dir[i].used && index-- == 0) return dir[i].filename;
    return NULL;
}

bool fs_exists(const char* name) { return mounted && find(name) >= 0; }

static uint32_t sectors_for(size_t len) {
    uint32_t n = (uint32_t)((len + SECTOR_SIZE - 1) / SECTOR_SIZE);
    return n ? n : 1;
}

/* First-fit: candidate starts are the beginning of the data area and the end of every extent. */
static bool allocate(uint32_t need, int ignore, uint32_t* out) {
    for (int c = -1; c < FS_MAX_FILES; c++) {
        uint32_t start;
        if (c < 0) start = FS_DATA_START;
        else if (dir[c].used && c != ignore) start = dir[c].start_sector + dir[c].num_sectors;
        else continue;
        if (start + need > FS_DATA_LIMIT) continue;
        bool clash = false;
        for (int i = 0; i < FS_MAX_FILES && !clash; i++) {
            if (!dir[i].used || i == ignore) continue;
            uint32_t a = dir[i].start_sector, b = a + dir[i].num_sectors;
            if (start < b && a < start + need) clash = true;
        }
        if (!clash) { *out = start; return true; }
    }
    return false;
}

char* fs_read(const char* name, size_t* size) {
    if (!mounted) return NULL;
    int i = find(name);
    if (i < 0) return NULL;
    uint32_t nsec = dir[i].num_sectors;
    char* buf = malloc((size_t)nsec * SECTOR_SIZE + 1);    /* one allocation, room for the NUL */
    if (!buf) return NULL;
    if (!bd_read(dev, dir[i].start_sector, nsec, buf)) { free(buf); return NULL; }
    buf[dir[i].size] = '\0';
    if (size) *size = dir[i].size;
    return buf;
}

bool fs_write(const char* name, const void* data, size_t len) {
    if (!mounted || !fs_valid_name(name) || len > (size_t)(FS_DATA_LIMIT - FS_DATA_START) * SECTOR_SIZE) return false;
    uint32_t need = sectors_for(len);
    int slot = find(name);
    uint32_t start;

    if (slot >= 0 && dir[slot].num_sectors >= need) {
        start = dir[slot].start_sector;                    /* rewrite in place */
    } else {
        if (slot < 0) {                                    /* need a free directory entry */
            for (int i = 0; i < FS_MAX_FILES; i++) if (!dir[i].used) { slot = i; break; }
            if (slot < 0) return false;
        }
        if (!allocate(need, slot, &start)) return false;
    }

    /* Data first, directory second: a failure in between leaves the old directory intact. */
    uint32_t full = (uint32_t)(len / SECTOR_SIZE);
    size_t tail = len % SECTOR_SIZE;
    if (full && !bd_write(dev, start, full, data)) return false;
    if (tail || full < need) {
        uint8_t bounce[SECTOR_SIZE];
        memset(bounce, 0, sizeof bounce);
        if (tail) memcpy(bounce, (const uint8_t*)data + (size_t)full * SECTOR_SIZE, tail);
        if (!bd_write(dev, start + full, 1, bounce)) return false;
    }

    FsEntry e;
    memset(&e, 0, sizeof e);
    memcpy(e.filename, name, strlen(name));                /* length validated above */
    e.start_sector = start;
    e.num_sectors = (slot >= 0 && dir[slot].used && dir[slot].start_sector == start && dir[slot].num_sectors > need) ? dir[slot].num_sectors : need;
    e.size = (uint32_t)len;
    e.used = 1;
    FsEntry old = dir[slot];
    dir[slot] = e;
    if (!dir_flush()) { dir[slot] = old; return false; }
    return true;
}

bool fs_append(const char* name, const void* data, size_t len) {
    size_t old_len = 0;
    char* old = fs_read(name, &old_len);
    if (!old) return fs_write(name, data, len);            /* append to a missing file == create */
    char* all = realloc(old, old_len + len + 1);
    if (!all) { free(old); return false; }
    memcpy(all + old_len, data, len);
    bool ok = fs_write(name, all, old_len + len);
    free(all);
    return ok;
}

bool fs_delete(const char* name) {
    if (!mounted) return false;
    int i = find(name);
    if (i < 0) return false;
    FsEntry old = dir[i];
    dir[i].used = 0;
    if (!dir_flush()) { dir[i] = old; return false; }
    return true;
}
