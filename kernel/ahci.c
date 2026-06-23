#include "ahci.h"

HBA_Mem* hba_mem = NULL;
HBA_Port* active_ports[32] = {0};
int active_port_count = 0;

extern void* kmalloc(size_t size);
extern void kfree(void* ptr);
extern void kputs(const char* s);

static inline void outd(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t ind(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

uint32_t pci_read_word(uint16_t bus, uint16_t slot, uint16_t func, uint16_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outd(0xCF8, address);
    return ind(0xCFC);
}

void pci_write_word(uint16_t bus, uint16_t slot, uint16_t func, uint16_t offset, uint32_t value) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outd(0xCF8, address);
    outd(0xCFC, value);
}

void find_ahci() {
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            uint32_t dev_id = pci_read_word(bus, slot, 0, 0x00);
            if ((dev_id & 0xFFFF) == 0xFFFF) continue;
            
            for (uint32_t func = 0; func < 8; func++) {
                uint32_t pci_class = pci_read_word(bus, slot, func, 0x08);
                uint8_t class_code = (pci_class >> 24) & 0xFF;
                uint8_t subclass = (pci_class >> 16) & 0xFF;
                
                if (class_code == 0x01 && subclass == 0x06) {
                    uint32_t abar = pci_read_word(bus, slot, func, 0x24);
                    hba_mem = (HBA_Mem*)(abar & 0xFFFFFFF0);
                    
                    uint32_t cmd = pci_read_word(bus, slot, func, 0x04);
                    pci_write_word(bus, slot, func, 0x04, cmd | 0x07);
                    return;
                }
            }
        }
    }
}

void init_ahci_port(HBA_Port* port) {
    port->cmd &= ~0x0001;
    port->cmd &= ~0x0010;

    volatile int spin = 0;
    while ((port->cmd & 0x4000 || port->cmd & 0x8000)) {
        if (spin++ > 10000000) break;
    }

    uint8_t* clb_mem = kmalloc(1024 + 1024);
    uint32_t clb_addr = ((uint32_t)clb_mem + 1023) & ~1023;
    port->clb = clb_addr;
    port->clbu = 0;

    uint8_t* fb_mem = kmalloc(256 + 256);
    uint32_t fb_addr = ((uint32_t)fb_mem + 255) & ~255;
    port->fb = fb_addr;
    port->fbu = 0;

    memset((void*)clb_addr, 0, 1024);
    memset((void*)fb_addr, 0, 256);

    __sync_synchronize();

    port->cmd |= 0x0010;
    port->cmd |= 0x0001;
}

void init_ahci() {
    find_ahci();
    if (!hba_mem) return;
    
    uint32_t pi = hba_mem->pi;
    for (int i = 0; i < 32; i++) {
        if (pi & (1 << i)) {
            HBA_Port* port = &hba_mem->ports[i];
            uint32_t ssts = port->ssts;
            uint8_t det = ssts & 0x0F;
            if (det == 3) {
                init_ahci_port(port);
                active_ports[active_port_count++] = port;
            }
        }
    }
}

#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define HBA_TIMEOUT           500000000 

bool ahci_read(HBA_Port* port, uint32_t startl, uint32_t starth, uint32_t count, uint16_t* buf) {
    if (!port) return false;
    port->is = (uint32_t)-1;
    port->serr = (uint32_t)-1;
    
    int slot = 0;
    
    CMD_Header* cmd_header = (CMD_Header*)(port->clb);
    cmd_header += slot;
    cmd_header->cfl = sizeof(FIS_REG_H2D) / sizeof(uint32_t);
    cmd_header->w = 0;
    cmd_header->prdtl = 1;
    
    uint8_t* cmd_tbl_mem = kmalloc(sizeof(CMD_Table) + 128);
    uint32_t cmd_tbl_addr = ((uint32_t)cmd_tbl_mem + 127) & ~127;
    memset((void*)cmd_tbl_addr, 0, sizeof(CMD_Table));
    
    cmd_header->ctba = cmd_tbl_addr;
    cmd_header->ctbau = 0;
    
    CMD_Table* cmd_tbl = (CMD_Table*)cmd_tbl_addr;
    cmd_tbl->prdt_entry[0].dba = (uint32_t)buf;
    cmd_tbl->prdt_entry[0].dbau = 0;
    cmd_tbl->prdt_entry[0].dbc = (count * 512) - 1;
    cmd_tbl->prdt_entry[0].i = 1;
    
    FIS_REG_H2D* cmdfis = (FIS_REG_H2D*)(&cmd_tbl->cfis);
    cmdfis->fis_type = 0x27;
    cmdfis->c = 1;
    cmdfis->command = ATA_CMD_READ_DMA_EXT;
    
    cmdfis->lba0 = (uint8_t)startl;
    cmdfis->lba1 = (uint8_t)(startl >> 8);
    cmdfis->lba2 = (uint8_t)(startl >> 16);
    cmdfis->device = 1 << 6;
    
    cmdfis->lba3 = (uint8_t)(startl >> 24);
    cmdfis->lba4 = (uint8_t)starth;
    cmdfis->lba5 = (uint8_t)(starth >> 8);
    cmdfis->count_low = (uint8_t)count;
    cmdfis->count_high = (uint8_t)(count >> 8);
    
    volatile int spin = 0;
    while ((port->tfd & (0x80 | 0x08))) {
        if (spin++ > HBA_TIMEOUT) {
            kfree(cmd_tbl_mem);
            return false;
        }
    }
    
    __sync_synchronize();
    
    port->ci = 1 << slot;
    
    spin = 0;
    while (1) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & (1 << 30)) {
            kfree(cmd_tbl_mem);
            return false;
        }
        if (spin++ > HBA_TIMEOUT) {
            kfree(cmd_tbl_mem);
            return false;
        }
    }
    
    kfree(cmd_tbl_mem);
    return true;
}

bool ahci_write(HBA_Port* port, uint32_t startl, uint32_t starth, uint32_t count, uint16_t* buf) {
    if (!port) return false;
    port->is = (uint32_t)-1;
    port->serr = (uint32_t)-1;
    
    int slot = 0;
    
    CMD_Header* cmd_header = (CMD_Header*)(port->clb);
    cmd_header += slot;
    cmd_header->cfl = sizeof(FIS_REG_H2D) / sizeof(uint32_t);
    cmd_header->w = 1;
    cmd_header->prdtl = 1;
    
    uint8_t* cmd_tbl_mem = kmalloc(sizeof(CMD_Table) + 128);
    uint32_t cmd_tbl_addr = ((uint32_t)cmd_tbl_mem + 127) & ~127;
    memset((void*)cmd_tbl_addr, 0, sizeof(CMD_Table));
    
    cmd_header->ctba = cmd_tbl_addr;
    cmd_header->ctbau = 0;
    
    CMD_Table* cmd_tbl = (CMD_Table*)cmd_tbl_addr;
    cmd_tbl->prdt_entry[0].dba = (uint32_t)buf;
    cmd_tbl->prdt_entry[0].dbau = 0;
    cmd_tbl->prdt_entry[0].dbc = (count * 512) - 1;
    cmd_tbl->prdt_entry[0].i = 1;
    
    FIS_REG_H2D* cmdfis = (FIS_REG_H2D*)(&cmd_tbl->cfis);
    cmdfis->fis_type = 0x27;
    cmdfis->c = 1;
    cmdfis->command = ATA_CMD_WRITE_DMA_EXT;
    
    cmdfis->lba0 = (uint8_t)startl;
    cmdfis->lba1 = (uint8_t)(startl >> 8);
    cmdfis->lba2 = (uint8_t)(startl >> 16);
    cmdfis->device = 1 << 6;
    
    cmdfis->lba3 = (uint8_t)(startl >> 24);
    cmdfis->lba4 = (uint8_t)starth;
    cmdfis->lba5 = (uint8_t)(starth >> 8);
    cmdfis->count_low = (uint8_t)count;
    cmdfis->count_high = (uint8_t)(count >> 8);
    
    volatile int spin = 0;
    while ((port->tfd & (0x80 | 0x08))) {
        if (spin++ > HBA_TIMEOUT) {
            kfree(cmd_tbl_mem);
            return false;
        }
    }
    
    __sync_synchronize();
    
    port->ci = 1 << slot;
    
    spin = 0;
    while (1) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & (1 << 30)) {
            kfree(cmd_tbl_mem);
            return false;
        }
        if (spin++ > HBA_TIMEOUT) {
            kfree(cmd_tbl_mem);
            return false;
        }
    }
    
    kfree(cmd_tbl_mem);
    return true;
}

#define ATA_CMD_FLUSH_CACHE 0xE7

bool ahci_flush_cache(HBA_Port* port) {
    if (!port) return false;
    port->is = (uint32_t)-1;
    port->serr = (uint32_t)-1;
    int slot = 0;
    
    CMD_Header* cmd_header = (CMD_Header*)(port->clb);
    cmd_header += slot;
    cmd_header->cfl = sizeof(FIS_REG_H2D) / sizeof(uint32_t);
    cmd_header->w = 0;
    cmd_header->prdtl = 0;
    
    uint8_t* cmd_tbl_mem = kmalloc(sizeof(CMD_Table) + 128);
    if (!cmd_tbl_mem) return false;
    uint32_t cmd_tbl_addr = ((uint32_t)cmd_tbl_mem + 127) & ~127;
    memset((void*)cmd_tbl_addr, 0, sizeof(CMD_Table));
    
    cmd_header->ctba = cmd_tbl_addr;
    cmd_header->ctbau = 0;
    
    CMD_Table* cmd_tbl = (CMD_Table*)cmd_tbl_addr;
    
    FIS_REG_H2D* cmdfis = (FIS_REG_H2D*)(&cmd_tbl->cfis);
    cmdfis->fis_type = 0x27;
    cmdfis->c = 1;
    cmdfis->command = ATA_CMD_FLUSH_CACHE;
    cmdfis->device = 1 << 6;
    
    volatile int spin = 0;
    while ((port->tfd & (0x80 | 0x08))) {
        if (spin++ > HBA_TIMEOUT) {
            kfree(cmd_tbl_mem);
            return false;
        }
    }
    
    __sync_synchronize();
    
    port->ci = 1 << slot;
    
    spin = 0;
    while (1) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & (1 << 30)) {
            kfree(cmd_tbl_mem);
            return false;
        }
        if (spin++ > HBA_TIMEOUT) {
            kfree(cmd_tbl_mem);
            return false;
        }
    }
    
    kfree(cmd_tbl_mem);
    return true;
}