/* x86 port I/O. One definition for the whole kernel (the original duplicated
 * outb/inb in kernel.c and outd/ind in ahci.c). */
#ifndef IO_H
#define IO_H
#include <stdint.h>

static inline void outb(uint16_t p, uint8_t v)  { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p)           { uint8_t r;  __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(p)); return r; }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p)          { uint32_t r; __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(p)); return r; }
static inline void cpu_relax(void)              { __asm__ volatile ("pause"); }

#endif
