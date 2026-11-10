#include "libc.h"

/* The mem* routines use `rep movs/stos`: they are the hot path for the VGA scroll,
 * disk buffers and the interpreter, and -- unlike hand written C loops -- GCC cannot
 * turn them back into calls to themselves (see -fno-tree-loop-distribute-patterns). */

void* memcpy(void* dst, const void* src, size_t n) {
    void* ret = dst;
    uint8_t* d = dst; const uint8_t* s = src;
    size_t words = n >> 2, tail = n & 3;
    __asm__ volatile ("cld; rep movsl" : "+D"(d), "+S"(s), "+c"(words) : : "memory");
    __asm__ volatile ("rep movsb"      : "+D"(d), "+S"(s), "+c"(tail)  : : "memory");
    return ret;
}

void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = dst; const uint8_t* s = src;
    if (d <= s || d >= s + n) return memcpy(dst, src, n);
    d += n - 1; s += n - 1;                       /* overlapping, copy backwards */
    __asm__ volatile ("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

void* memset(void* dst, int v, size_t n) {
    void* ret = dst;
    uint8_t* d = dst;
    uint32_t pat = (uint8_t)v * 0x01010101u;
    size_t words = n >> 2, tail = n & 3;
    __asm__ volatile ("cld; rep stosl" : "+D"(d), "+c"(words) : "a"(pat) : "memory");
    __asm__ volatile ("rep stosb"      : "+D"(d), "+c"(tail)  : "a"(pat) : "memory");
    return ret;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* p = a; const uint8_t* q = b;
    for (size_t i = 0; i < n; i++)
        if (p[i] != q[i]) return p[i] - q[i];
    return 0;
}

size_t strlen(const char* s) {
    const char* p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (uint8_t)*a - (uint8_t)*b : 0;
}

size_t strlcpy(char* dst, const char* src, size_t n) {
    size_t len = strlen(src);
    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return len;
}

int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
