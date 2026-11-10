#ifndef SHELL_H
#define SHELL_H
#include <stddef.h>

/* Prints `prompt`, reads one line, strips surrounding blanks. Returns its length. */
size_t shell_read_command(const char* prompt, char* buf, size_t cap);
/* Interactive shell of an installed system (the INPS filesystem must be mounted). */
void shell_run(void) __attribute__((noreturn));

#endif
