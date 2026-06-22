#include "libc.h"

#define HEAP_START 0x100000
#define HEAP_MAX   0x50000

typedef struct BlockHeader {
    size_t size;
    bool is_free;
    struct BlockHeader* next;
} BlockHeader;

static BlockHeader* free_list_head = NULL;

extern void kputc(char c);
extern void kputs(const char* s);
extern char kget_char();

long long get_time_ms(void) {
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    uint64_t tsc = ((uint64_t)high << 32) | low;
    return (long long)(tsc / 2000000);
}

void sleep_ms(long long ms) {
    long long start = get_time_ms();
    while (get_time_ms() - start < ms);
}

void init_allocator() {
    free_list_head = (BlockHeader*)HEAP_START;
    free_list_head->size = HEAP_MAX - sizeof(BlockHeader);
    free_list_head->is_free = true;
    free_list_head->next = NULL;
}

void* kmalloc(size_t size) {
    size = (size + 7) & ~7;
    BlockHeader* curr = free_list_head;
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            if (curr->size >= size + sizeof(BlockHeader) + 8) {
                BlockHeader* next_block = (BlockHeader*)((uint8_t*)curr + sizeof(BlockHeader) + size);
                next_block->size = curr->size - size - sizeof(BlockHeader);
                next_block->is_free = true;
                next_block->next = curr->next;
                curr->size = size;
                curr->is_free = false;
                curr->next = next_block;
            } else {
                curr->is_free = false;
            }
            return (void*)((uint8_t*)curr + sizeof(BlockHeader));
        }
        curr = curr->next;
    }
    return NULL;
}

void kfree(void* ptr) {
    if (!ptr) return;
    BlockHeader* header = (BlockHeader*)((uint8_t*)ptr - sizeof(BlockHeader));
    header->is_free = true;
    
    BlockHeader* curr = free_list_head;
    while (curr) {
        if (curr->is_free && curr->next && curr->next->is_free) {
            curr->size += sizeof(BlockHeader) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

void* krealloc(void* ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return NULL; }
    BlockHeader* header = (BlockHeader*)((uint8_t*)ptr - sizeof(BlockHeader));
    if (header->size >= size) return ptr;
    
    void* new_ptr = kmalloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, header->size);
        kfree(ptr);
    }
    return new_ptr;
}

void* malloc(size_t size) { return kmalloc(size); }
void free(void* ptr) { kfree(ptr); }
void* realloc(void* ptr, size_t size) { return krealloc(ptr, size); }

char* strdup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    memcpy(copy, s, len);
    return copy;
}

void* memset(void* dest, int val, size_t len) {
    uint8_t* ptr = (uint8_t*)dest;
    while (len-- > 0) *ptr++ = (uint8_t)val;
    return dest;
}

void* memcpy(void* dest, const void* src, size_t len) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (len-- > 0) *d++ = *s++;
    return dest;
}

size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

char* strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const uint8_t*)s1 - *(const uint8_t*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while (n > 0 && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(const uint8_t*)s1 - *(const uint8_t*)s2;
}

char* strncpy(char* dest, const char* src, size_t n) {
    char* d = dest;
    while (n > 0 && *src) { *d++ = *src++; n--; }
    while (n > 0) { *d++ = '\0'; n--; }
    return dest;
}

size_t strcspn(const char* s1, const char* s2) {
    size_t len = 0;
    while (s1[len]) {
        for (size_t i = 0; s2[i]; i++) {
            if (s1[len] == s2[i]) return len;
        }
        len++;
    }
    return len;
}

char* strrchr(const char* s, int c) {
    char* last = NULL;
    do {
        if (*s == (char)c) last = (char*)s;
    } while (*s++);
    return last;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    if (*s == (char)c) return (char*)s;
    return NULL;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* p1 = s1;
    const uint8_t* p2 = s2;
    while (n-- > 0) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

int isalpha(int c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}

int isdigit(int c) {
    return (c >= '0' && c <= '9');
}

int strcasecmp(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        char c1 = *s1;
        char c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return (unsigned char)c1 - (unsigned char)c2;
        s1++; s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

long long atoll(const char* str) {
    long long val = 0;
    int sign = 1;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') str++;
    while (*str >= '0' && *str <= '9') {
        val = val * 10 + (*str - '0');
        str++;
    }
    return val * sign;
}

double atof(const char* str) {
    double val = 0.0;
    double factor = 1.0;
    int sign = 1;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') str++;
    while (*str >= '0' && *str <= '9') {
        val = val * 10.0 + (*str - '0');
        str++;
    }
    if (*str == '.') {
        str++;
        while (*str >= '0' && *str <= '9') {
            val = val * 10.0 + (*str - '0');
            factor *= 10.0;
            str++;
        }
    }
    return (val / factor) * sign;
}

static void int_to_str(long long val, char* buf, int base) {
    char temp[64];
    int i = 0, is_neg = 0;
    if (val < 0 && base == 10) { is_neg = 1; val = -val; }
    if (val == 0) temp[i++] = '0';
    while (val > 0) {
        int rem = val % base;
        temp[i++] = rem < 10 ? rem + '0' : rem - 10 + 'a';
        val /= base;
    }
    if (is_neg) temp[i++] = '-';
    int j = 0;
    while (i > 0) buf[j++] = temp[--i];
    buf[j] = '\0';
}

static void float_to_str(double val, char* buf) {
    if (val < 0) { *buf++ = '-'; val = -val; }
    long long int_part = (long long)val;
    int_to_str(int_part, buf, 10);
    while (*buf) buf++;
    *buf++ = '.';
    double frac_part = val - (double)int_part;
    for (int i = 0; i < 6; i++) {
        frac_part *= 10;
        int digit = (int)frac_part;
        *buf++ = digit + '0';
        frac_part -= digit;
    }
    *buf = '\0';
}

int vsnprintf(char* str, size_t size, const char* format, va_list arg) {
    size_t written = 0;
    while (*format && written < size - 1) {
        if (*format == '%') {
            format++;
            if (*format == '\0') break;
            if (*format == 'd' || *format == 'i') {
                int val = va_arg(arg, int);
                char buf[64];
                int_to_str(val, buf, 10);
                for (int i = 0; buf[i] && written < size - 1; i++) str[written++] = buf[i];
            } else if (*format == 'l' && *(format + 1) == 'l' && *(format + 2) == 'd') {
                format += 2;
                long long val = va_arg(arg, long long);
                char buf[64];
                int_to_str(val, buf, 10);
                for (int i = 0; buf[i] && written < size - 1; i++) str[written++] = buf[i];
            } else if (*format == 's') {
                char* s = va_arg(arg, char*);
                if (!s) s = "(null)";
                for (int i = 0; s[i] && written < size - 1; i++) str[written++] = s[i];
            } else if (*format == 'c') {
                char c = (char)va_arg(arg, int);
                str[written++] = c;
            } else if (*format == 'f' || *format == 'g') {
                double val = va_arg(arg, double);
                char buf[64];
                float_to_str(val, buf);
                for (int i = 0; buf[i] && written < size - 1; i++) str[written++] = buf[i];
            } else {
                str[written++] = *format;
            }
        } else {
            str[written++] = *format;
        }
        format++;
    }
    str[written] = '\0';
    return written;
}

int snprintf(char* str, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vsnprintf(str, size, format, args);
    va_end(args);
    return ret;
}

int printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    kputs(buf);
    return 0;
}

int vfprintf(KFILE* stream, const char* format, va_list arg) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), format, arg);
    if (stream == stdout || stream == stderr) {
        kputs(buf);
    } else {
        size_t len = strlen(buf);
        stream->buffer = krealloc(stream->buffer, stream->size + len + 1);
        memcpy(stream->buffer + stream->size, buf, len);
        stream->size += len;
        stream->buffer[stream->size] = '\0';
    }
    return 0;
}

int fprintf(KFILE* stream, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vfprintf(stream, format, args);
    va_end(args);
    return ret;
}

int fputs(const char* s, KFILE* stream) {
    if (stream == stdout || stream == stderr) {
        kputs(s);
    } else {
        size_t len = strlen(s);
        stream->buffer = krealloc(stream->buffer, stream->size + len + 1);
        memcpy(stream->buffer + stream->size, s, len);
        stream->size += len;
        stream->buffer[stream->size] = '\0';
    }
    return 0;
}

extern void* active_ports[32];
extern bool create_file(void* disk, const char* name, const char* content, uint32_t size);
extern bool delete_file(void* disk, const char* name);
extern char* read_file(void* disk, const char* name, uint32_t* out_size);

KFILE* fopen(const char* filename, const char* mode) {
    KFILE* f = malloc(sizeof(KFILE));
    memset(f, 0, sizeof(KFILE));
    strcpy(f->filename, filename);
    
    if (mode[0] == 'r') {
        f->mode = 1;
        uint32_t fsize = 0;
        char* content = read_file(active_ports[1], filename, &fsize);
        if (!content) {
            free(f);
            return NULL;
        }
        f->buffer = content;
        f->size = fsize;
        f->cursor = 0;
    } else if (mode[0] == 'w') {
        f->mode = 2;
        f->buffer = malloc(1);
        f->buffer[0] = '\0';
        f->size = 0;
    } else if (mode[0] == 'a') {
        f->mode = 3;
        uint32_t fsize = 0;
        char* content = read_file(active_ports[1], filename, &fsize);
        if (content) {
            f->buffer = content;
            f->size = fsize;
        } else {
            f->buffer = malloc(1);
            f->buffer[0] = '\0';
            f->size = 0;
        }
        f->cursor = f->size;
    }
    return f;
}

int fclose(KFILE* stream) {
    if (!stream) return -1;
    if (stream->mode == 2 || stream->mode == 3) {
        create_file(active_ports[1], stream->filename, stream->buffer, stream->size);
    }
    if (stream->buffer) free(stream->buffer);
    free(stream);
    return 0;
}

size_t fread(void* ptr, size_t size, size_t nmemb, KFILE* stream) {
    if (!stream || stream->mode != 1) return 0;
    size_t bytes_to_read = size * nmemb;
    if (stream->cursor + bytes_to_read > stream->size) {
        bytes_to_read = stream->size - stream->cursor;
    }
    if (bytes_to_read == 0) return 0;
    memcpy(ptr, stream->buffer + stream->cursor, bytes_to_read);
    stream->cursor += bytes_to_read;
    return bytes_to_read / size;
}

char* fgets(char* s, int size, KFILE* stream) {
    if (stream == stdin) {
        int i = 0;
        while (i < size - 1) {
            char c = kget_char();
            kputc(c);
            if (c == '\n' || c == '\r') {
                s[i++] = '\n';
                break;
            } else if (c == '\b') {
                if (i > 0) i--;
            } else {
                s[i++] = c;
            }
        }
        s[i] = '\0';
        return s;
    } else {
        if (stream->cursor >= stream->size) return NULL;
        int i = 0;
        while (i < size - 1 && stream->cursor < stream->size) {
            char c = stream->buffer[stream->cursor++];
            s[i++] = c;
            if (c == '\n') break;
        }
        s[i] = '\0';
        return s;
    }
}

int remove(const char* filename) {
    return delete_file(active_ports[1], filename) ? 0 : -1;
}

void exit(int status) {
    printf("OS user-process exited with status %d.\n", status);
    while(1);
}

void abort(void) {
    printf("\nKernel panic: abort() called from runtime library.\n");
    while (1);
}