#include "console.h"
#include "keyboard.h"
#include "libc.h"
#include "io.h"

#define VGA_W   80
#define VGA_H   25
#define VGA_MEM ((volatile uint16_t*)0xB8000)
#define BLANK   ((uint16_t)((term_color << 8) | ' '))

static int cur_x, cur_y;
static uint8_t term_color = 0x07;

static void hw_cursor(void) {
    uint16_t pos = (uint16_t)(cur_y * VGA_W + cur_x);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));
}

void console_clear(void) {
    for (int i = 0; i < VGA_W * VGA_H; i++) VGA_MEM[i] = BLANK;
    cur_x = cur_y = 0;
    hw_cursor();
}

void console_init(void) { console_clear(); }

static void scroll_if_needed(void) {
    if (cur_y < VGA_H) return;
    memmove((void*)VGA_MEM, (const void*)(VGA_MEM + VGA_W), (VGA_H - 1) * VGA_W * sizeof(uint16_t));
    for (int i = (VGA_H - 1) * VGA_W; i < VGA_H * VGA_W; i++) VGA_MEM[i] = BLANK;
    cur_y = VGA_H - 1;
}

/* Does NOT touch the hardware cursor: callers batch that. */
static void put(char c) {
    switch (c) {
    case '\n': cur_x = 0; cur_y++; break;
    case '\r': cur_x = 0; break;
    case '\t': cur_x = (cur_x + 8) & ~7; break;
    case '\b':
        if (cur_x > 0) cur_x--;
        else if (cur_y > 0) { cur_y--; cur_x = VGA_W - 1; }
        VGA_MEM[cur_y * VGA_W + cur_x] = BLANK;
        break;
    default:
        VGA_MEM[cur_y * VGA_W + cur_x] = (uint16_t)((term_color << 8) | (uint8_t)c);
        cur_x++;
    }
    if (cur_x >= VGA_W) { cur_x = 0; cur_y++; }
    scroll_if_needed();
}

void console_putc(char c) { put(c); hw_cursor(); }

void console_write(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) put(s[i]);
    hw_cursor();
}

void console_puts(const char* s) { console_write(s, strlen(s)); }

size_t console_readline(char* buf, size_t cap) {
    size_t len = 0;
    for (;;) {
        char c = kbd_getchar();
        if (c == '\n' || c == '\r') { console_putc('\n'); break; }
        if (c == '\b') {
            if (len > 0) { len--; console_putc('\b'); }
        } else if ((uint8_t)c >= 0x20 && len + 1 < cap) {
            buf[len++] = c;
            console_putc(c);
        }
    }
    buf[len] = '\0';
    return len;
}
