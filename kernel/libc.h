/* Minimal freestanding C library for inpsos.
 * Deliberately small: unsafe functions (strcpy, strcat, gets, ...) are NOT provided. */
#ifndef LIBC_H
#define LIBC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

/* ---- memory / string ---- */
void*  memset(void* dst, int v, size_t n);
void*  memcpy(void* dst, const void* src, size_t n);
void*  memmove(void* dst, const void* src, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
/* Copies at most n-1 bytes and always NUL terminates (n > 0). Returns strlen(src). */
size_t strlcpy(char* dst, const char* src, size_t n);

int isalpha(int c);
int isdigit(int c);
int isalnum(int c);
int isspace(int c);

/* ---- formatted output ---- */
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);
int snprintf(char* buf, size_t size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
int printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- heap ---- */
void  heap_init(void);
void* malloc(size_t n);
void* calloc(size_t n, size_t sz);
void* realloc(void* p, size_t n);
void  free(void* p);
size_t heap_free_bytes(void);

/* ---- misc ---- */
void abort(void) __attribute__((noreturn));
void panic(const char* msg) __attribute__((noreturn));

#endif
