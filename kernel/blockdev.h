/* Abstract block device: the filesystem depends on THIS, not on AHCI.
 * That makes fs.c testable on the host with a RAM disk (tests/test_fs.c). */
#ifndef BLOCKDEV_H
#define BLOCKDEV_H
#include <stdint.h>
#include <stdbool.h>

typedef struct BlockDev {
    void* ctx;
    bool (*read)(void* ctx, uint32_t lba, uint32_t count, void* buf);
    bool (*write)(void* ctx, uint32_t lba, uint32_t count, const void* buf);
    bool (*flush)(void* ctx);
} BlockDev;

static inline bool bd_read(const BlockDev* d, uint32_t lba, uint32_t n, void* buf)        { return d->read(d->ctx, lba, n, buf); }
static inline bool bd_write(const BlockDev* d, uint32_t lba, uint32_t n, const void* buf) { return d->write(d->ctx, lba, n, buf); }
static inline bool bd_flush(const BlockDev* d)                                            { return !d->flush || d->flush(d->ctx); }

#endif
