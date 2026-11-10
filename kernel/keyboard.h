#ifndef KEYBOARD_H
#define KEYBOARD_H
#include <stdint.h>
#include <stdbool.h>

#define SC_KEY_I 0x17

/* Non-blocking: returns a raw make-code (0x01..0x7F) or 0 if no key press is pending. */
uint8_t kbd_poll_scancode(void);
/* Blocking: returns the next printable/control ASCII character ('\n', '\b', '\t' included). */
char kbd_getchar(void);
/* Pulse the CPU reset line through the keyboard controller. */
void kbd_reboot(void) __attribute__((noreturn));

#endif
