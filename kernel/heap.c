#include "libc.h"
#include "config.h"

/* Address-ordered, doubly linked block list.
 *
 * Compared with the original singleton free list:
 *   - kfree() is O(1): it coalesces with BOTH neighbours instead of re-scanning the whole
 *     list after every free (O(n) per free => O(n^2) for n frees).
 *   - 16-byte header => every payload is 16-byte aligned (the old 12-byte header gave
 *     4-byte aligned payloads, a penalty for doubles/int64 and a trap for DMA buffers).
 *   - Magic values catch double free / wild free / header corruption instead of silently
 *     corrupting the list.
 *   - realloc() grows in place when the next block is free.
 */

#define MAGIC_USED 0xA110C8EDu
#define MAGIC_FREE 0xF4EEB10Cu
#define ALIGN      16u

typedef struct Block {
    uint32_t size;               /* payload bytes */
    uint32_t magic;
    struct Block* prev;
    struct Block* next;
} Block;

_Static_assert(sizeof(Block) == 16 || sizeof(void*) != 4, "Block header must be 16 bytes on i386");
#define HDR ((uint32_t)sizeof(Block))

static Block* head;
static uint8_t* heap_lo;
static uint8_t* heap_hi;
static size_t free_bytes;

void heap_init(void) {
    heap_lo = (uint8_t*)HEAP_START;
    heap_hi = heap_lo + HEAP_SIZE;
    head = (Block*)heap_lo;
    head->size = HEAP_SIZE - HDR;
    head->magic = MAGIC_FREE;
    head->prev = head->next = NULL;
    free_bytes = head->size;
}

size_t heap_free_bytes(void) { return free_bytes; }

static void split(Block* b, uint32_t want) {
    if (b->size >= want + HDR + ALIGN) {
        Block* n = (Block*)((uint8_t*)b + HDR + want);
        n->size = b->size - want - HDR;
        n->magic = MAGIC_FREE;
        n->prev = b;
        n->next = b->next;
        if (n->next) n->next->prev = n;
        b->next = n;
        b->size = want;
        free_bytes -= HDR;            /* the new header is overhead */
    }
}

void* malloc(size_t n) {
    if (n == 0) n = 1;
    if (n > HEAP_SIZE) return NULL;
    uint32_t want = ((uint32_t)n + ALIGN - 1) & ~(ALIGN - 1);
    for (Block* b = head; b; b = b->next) {
        if (b->magic == MAGIC_FREE && b->size >= want) {
            split(b, want);
            b->magic = MAGIC_USED;
            free_bytes -= b->size;
            return (uint8_t*)b + HDR;
        }
    }
    return NULL;
}

static Block* header_of(void* p) {
    uint8_t* q = p;
    if (q < heap_lo + HDR || q >= heap_hi || ((uintptr_t)q & (ALIGN - 1))) panic("free(): invalid pointer");
    Block* b = (Block*)(q - HDR);
    if (b->magic == MAGIC_FREE) panic("free(): double free");
    if (b->magic != MAGIC_USED) panic("free(): heap corruption");
    return b;
}

static void merge_with_next(Block* b) {
    Block* n = b->next;
    if (n && n->magic == MAGIC_FREE) {
        b->size += HDR + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
        free_bytes += HDR;
    }
}

void free(void* p) {
    if (!p) return;
    Block* b = header_of(p);
    b->magic = MAGIC_FREE;
    free_bytes += b->size;
    merge_with_next(b);
    if (b->prev && b->prev->magic == MAGIC_FREE) merge_with_next(b->prev);
}

void* calloc(size_t n, size_t sz) {
    if (sz && n > HEAP_SIZE / sz) return NULL;
    void* p = malloc(n * sz);
    if (p) memset(p, 0, n * sz);
    return p;
}

void* realloc(void* p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }
    Block* b = header_of(p);
    uint32_t want = ((uint32_t)n + ALIGN - 1) & ~(ALIGN - 1);
    if (b->size >= want) return p;
    Block* nx = b->next;                             /* try to grow in place */
    if (nx && nx->magic == MAGIC_FREE && b->size + HDR + nx->size >= want) {
        free_bytes -= nx->size;                      /* nx's payload (+ its header) becomes ours */
        b->size += HDR + nx->size;
        b->next = nx->next;
        if (nx->next) nx->next->prev = b;
        if (b->size >= want + HDR + ALIGN) {         /* give back a large tail */
            uint32_t total = b->size;
            b->size = want;
            Block* t = (Block*)((uint8_t*)b + HDR + want);
            t->size = total - want - HDR;
            t->magic = MAGIC_FREE;
            t->prev = b; t->next = b->next;
            if (t->next) t->next->prev = t;
            b->next = t;
            free_bytes += t->size;
        }
        return p;
    }
    void* q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, b->size);
    free(p);
    return q;
}

void panic(const char* msg) {
    printf("\nKERNEL PANIC: %s\n", msg);
    for (;;) __asm__ volatile ("cli; hlt");
}

void abort(void) { panic("abort() called"); }
