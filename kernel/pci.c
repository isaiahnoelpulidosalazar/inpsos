#include "pci.h"
#include "io.h"

#define CFG_ADDR 0xCF8
#define CFG_DATA 0xCFC

static uint32_t cfg_addr(PciAddr a, uint8_t off) {
    return 0x80000000u | ((uint32_t)a.bus << 16) | ((uint32_t)a.slot << 11) | ((uint32_t)a.func << 8) | (off & 0xFC);
}

uint32_t pci_read32(PciAddr a, uint8_t off) { outl(CFG_ADDR, cfg_addr(a, off)); return inl(CFG_DATA); }
void pci_write32(PciAddr a, uint8_t off, uint32_t v) { outl(CFG_ADDR, cfg_addr(a, off)); outl(CFG_DATA, v); }

bool pci_find_class(uint8_t class_code, uint8_t subclass, PciAddr* out) {
    for (unsigned bus = 0; bus < 256; bus++) {
        for (unsigned slot = 0; slot < 32; slot++) {
            PciAddr a = { (uint8_t)bus, (uint8_t)slot, 0 };
            if ((pci_read32(a, 0x00) & 0xFFFF) == 0xFFFF) continue;          /* no device */
            /* Header type bit 7 says whether functions 1..7 exist. The original probed all 8
             * functions of every present slot. */
            unsigned nfunc = (pci_read32(a, 0x0C) >> 16) & 0x80 ? 8 : 1;
            for (unsigned f = 0; f < nfunc; f++) {
                a.func = (uint8_t)f;
                if ((pci_read32(a, 0x00) & 0xFFFF) == 0xFFFF) continue;
                uint32_t cls = pci_read32(a, 0x08);
                if (((cls >> 24) & 0xFF) == class_code && ((cls >> 16) & 0xFF) == subclass) {
                    *out = a;
                    return true;
                }
            }
        }
    }
    return false;
}
