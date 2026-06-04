#ifndef LIBC_H
#define LIBC_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define bool  _Bool
#define true  1
#define false 0

#ifndef NULL
#define NULL  ((void*)0)
#endif

long long get_time_ms(void);
void sleep_ms(long long ms);

void* memset(void* dest, int val, size_t len);
void* memcpy(void* dest, const void* src, size_t len);
size_t strlen(const char* str);
char* strcpy(char* dest, const char* src);
char* strcat(char* dest, const char* src);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
char* strncpy(char* dest, const char* src, size_t n);
size_t strcspn(const char* s1, const char* s2);
char* strrchr(const char* s, int c);
char* strchr(const char* s, int c);
int memcmp(const void* s1, const void* s2, size_t n);
int isalpha(int c);
int isdigit(int c);
int strcasecmp(const char* s1, const char* s2);

long long atoll(const char* str);
double atof(const char* str);

void* malloc(size_t size);
void free(void* ptr);
void* realloc(void* ptr, size_t size);
char* strdup(const char* s);

#define EOF (-1)
#define stdin  ((KFILE*)1)
#define stdout ((KFILE*)2)
#define stderr ((KFILE*)3)

typedef struct {
    char filename[32];
    uint32_t cursor;
    uint32_t size;
    int mode;
    char* buffer;
} KFILE;

#define FILE KFILE

KFILE* fopen(const char* filename, const char* mode);
int fclose(KFILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, KFILE* stream);
int fprintf(KFILE* stream, const char* format, ...);
int printf(const char* format, ...);
int snprintf(char* str, size_t size, const char* format, ...);
int vfprintf(KFILE* stream, const char* format, va_list arg);
int fputs(const char* s, KFILE* stream);
char* fgets(char* s, int size, KFILE* stream);
int remove(const char* filename);

void abort(void);

#endif