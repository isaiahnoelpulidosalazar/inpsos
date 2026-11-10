#ifndef PCI_H
#define PCI_H
#include <stdint.h>
#include <stdbool.h>

typedef struct { uint8_t bus, slot, func; } PciAddr;

uint32_t pci_read32(PciAddr a, uint8_t off);
void     pci_write32(PciAddr a, uint8_t off, uint32_t v);
/* Finds the first function with the given class/subclass. */
bool     pci_find_class(uint8_t class_code, uint8_t subclass, PciAddr* out);

#endif
