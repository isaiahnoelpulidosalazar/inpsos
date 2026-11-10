#include "keyboard.h"
#include "io.h"

#define KBD_STATUS 0x64
#define KBD_DATA   0x60
#define ST_OUT_FULL 0x01
#define ST_AUX_DATA 0x20       /* byte comes from the PS/2 mouse, not the keyboard */

static const char layout[128] = {
    0, 0, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
};
static const char shift_layout[128] = {
    0, 0, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
    'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
};

static bool shift_down, caps_lock, extended;

/* Reads one byte if available and updates modifier state. Returns the make code of a
 * key PRESS, or 0 for releases, modifiers, extended (0xE0) keys and mouse bytes. */
uint8_t kbd_poll_scancode(void) {
    uint8_t st = inb(KBD_STATUS);
    if (!(st & ST_OUT_FULL)) return 0;
    uint8_t code = inb(KBD_DATA);
    if (st & ST_AUX_DATA) return 0;
    if (code == 0xE0) { extended = true; return 0; }
    if (extended)     { extended = false; return 0; }   /* arrows etc. are not supported yet */
    switch (code) {
    case 0x2A: case 0x36: shift_down = true;  return 0;
    case 0xAA: case 0xB6: shift_down = false; return 0;
    case 0x3A:            caps_lock = !caps_lock; return 0;
    }
    return (code & 0x80) ? 0 : code;
}

char kbd_getchar(void) {
    for (;;) {
        uint8_t code = kbd_poll_scancode();
        if (!code) { cpu_relax(); continue; }
        char c = shift_down ? shift_layout[code] : layout[code];
        if (caps_lock && c >= 'a' && c <= 'z') c = (char)(c - 32);
        else if (caps_lock && c >= 'A' && c <= 'Z' && shift_down) c = (char)(c + 32);
        if (c) return c;
    }
}

void kbd_reboot(void) {
    while (inb(KBD_STATUS) & 0x02) { }      /* wait for the input buffer to drain */
    outb(KBD_STATUS, 0xFE);                 /* pulse reset */
    for (;;) __asm__ volatile ("cli; hlt");
}
