#ifndef CONSOLE_H
#define CONSOLE_H
#include <stddef.h>
#include <stdbool.h>

void console_init(void);
void console_clear(void);
void console_putc(char c);
void console_write(const char* s, size_t n);   /* one hardware-cursor update per call */
void console_puts(const char* s);

/* Blocking line editor (echo + backspace). Returns the length; never exceeds cap-1.
 * The edit never reaches back past the start of the line, so the prompt is safe. */
size_t console_readline(char* buf, size_t cap);

#endif
