/* Easec -- public embedding API.
 *
 * The interpreter has NO global state and no dependency on the kernel: everything it needs from
 * the outside world (console, files, clock) comes through EasecHost.  The kernel provides one
 * host (kernel/shell.c); the host-side test-suite provides another (tests/test_easec.c). */
#ifndef EASEC_H
#define EASEC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct EasecVM EasecVM;

typedef struct EasecHost {
    void* ctx;
    void    (*write)(void* ctx, const char* s, size_t n);
    /* Reads one line (without the newline) into buf; false on end of input. */
    bool    (*read_line)(void* ctx, char* buf, size_t cap);
    /* Returns a malloc()'d, NUL-terminated buffer (caller frees) or NULL if the file is missing. */
    char*   (*read_file)(void* ctx, const char* path, size_t* len);
    bool    (*write_file)(void* ctx, const char* path, const char* data, size_t len, bool append);
    bool    (*delete_file)(void* ctx, const char* path);
    int     (*file_count)(void* ctx);
    const char* (*file_name)(void* ctx, int index);
    int64_t (*now_ms)(void* ctx);        /* wall clock, ms since the Unix epoch (UTC) */
    int64_t (*uptime_ms)(void* ctx);     /* monotonic ms */
    void    (*sleep_ms)(void* ctx, int64_t ms);
    size_t  max_bytes;                   /* memory budget for one VM; 0 = 1 MiB */
} EasecHost;

EasecVM* easec_create(const EasecHost* host);
void     easec_destroy(EasecVM* vm);
/* Compiles and runs `source`. Errors are reported through host->write. Returns true on success. */
bool     easec_run(EasecVM* vm, const char* name, const char* source);
size_t   easec_bytes_allocated(const EasecVM* vm);

#endif
