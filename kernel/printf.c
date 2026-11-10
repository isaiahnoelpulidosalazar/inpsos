#include "libc.h"
#include "console.h"

/* Small printf: %d %i %u %x %X %c %s %p %%, flags '-' '0', width, .precision (strings),
 * length modifiers l, ll, z.  No floating point: Easec formats its own decimals.
 * Output is truncated to the buffer, but the return value is the length that WOULD have
 * been written (C99 semantics). */

typedef struct { char* buf; size_t cap; size_t len; } Out;

static void out_ch(Out* o, char c) {
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static size_t u64_to_str(char* tmp, uint64_t v, unsigned base, bool upper) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t n = 0;
    if (v == 0) tmp[n++] = '0';
    if (v <= 0xFFFFFFFFu) {                       /* avoid libgcc 64-bit division when possible */
        uint32_t w = (uint32_t)v;
        while (w) { tmp[n++] = digits[w % base]; w /= base; }
    } else {
        while (v) { tmp[n++] = digits[v % base]; v /= base; }
    }
    return n;                                     /* digits are in reverse order */
}

static void out_padded(Out* o, const char* s, size_t n, int width, bool left, char pad) {
    size_t padn = width > (int)n ? (size_t)width - n : 0;
    if (!left) while (padn--) out_ch(o, pad);
    for (size_t i = 0; i < n; i++) out_ch(o, s[i]);
    if (left) while (padn--) out_ch(o, ' ');
}

int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
    Out o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { out_ch(&o, *fmt); continue; }
        fmt++;
        bool left = false; char pad = ' ';
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') pad = '0';
            else break;
        }
        int width = 0, prec = -1;
        while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') { fmt++; prec = 0; while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0'); }
        int lng = 0;                               /* 0 = int, 1 = long, 2 = long long, 3 = size_t */
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') { lng = 3; fmt++; }
        if (!*fmt) break;

        char tmp[72];
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = lng >= 2 ? va_arg(ap, long long) : va_arg(ap, int);
            bool neg = v < 0;
            uint64_t mag = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;   /* safe for INT64_MIN */
            size_t n = u64_to_str(tmp, mag, 10, false);
            char rev[72]; size_t k = 0;
            if (neg) rev[k++] = '-';
            while (n) rev[k++] = tmp[--n];
            out_padded(&o, rev, k, width, left, (pad == '0' && !left && neg) ? ' ' : pad);
            break;
        }
        case 'u': case 'x': case 'X': {
            uint64_t v = lng >= 2 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned int);
            size_t n = u64_to_str(tmp, v, *fmt == 'u' ? 10 : 16, *fmt == 'X');
            char rev[72]; size_t k = 0;
            while (n) rev[k++] = tmp[--n];
            out_padded(&o, rev, k, width, left, pad);
            break;
        }
        case 'p': {
            uint32_t v = (uint32_t)(uintptr_t)va_arg(ap, void*);
            size_t n = u64_to_str(tmp, v, 16, false);
            char rev[72]; size_t k = 0;
            rev[k++] = '0'; rev[k++] = 'x';
            while (n) rev[k++] = tmp[--n];
            out_padded(&o, rev, k, width, left, ' ');
            break;
        }
        case 'c': { char c = (char)va_arg(ap, int); out_padded(&o, &c, 1, width, left, ' '); break; }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            size_t n = strlen(s);
            if (prec >= 0 && (size_t)prec < n) n = (size_t)prec;
            out_padded(&o, s, n, width, left, ' ');
            break;
        }
        case '%': out_ch(&o, '%'); break;
        default:  out_ch(&o, '%'); out_ch(&o, *fmt); break;
        }
        (void)lng;
    }
    if (size) o.buf[o.len < size ? o.len : size - 1] = '\0';
    return (int)o.len;
}

int snprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    console_puts(buf);
    return r;
}
