#include "ahci.h"
#include "libc.h"
#include "pci.h"
#include "timer.h"
#include "config.h"

/* ---- AHCI register / structure layouts (see AHCI 1.3.1) -------------------------------- */
typedef volatile struct {
    uint32_t clb, clbu, fb, fbu, is, ie, cmd, rsv0, tfd, sig, ssts, sctl, serr, sact, ci, sntf, fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} HBA_Port;

typedef volatile struct {
    uint32_t cap, ghc, is, pi, vs, ccc_ctl, ccc_pts, em_loc, em_ctl, cap2, bohc;
    uint8_t  rsv[116];
    uint8_t  vendor[96];
    HBA_Port ports[32];
} HBA_Mem;

typedef struct {
    uint32_t dba, dbau, rsv0;
    uint32_t dbc : 22;
    uint32_t rsv1 : 9;
    uint32_t i : 1;
} __attribute__((packed)) PRDT_Entry;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    PRDT_Entry prdt[1];
} __attribute__((packed)) CMD_Table;

typedef struct {
    uint8_t  cfl : 5, a : 1, w : 1, p : 1;
    uint8_t  r : 1, b : 1, c : 1, rsv0 : 1, pmp : 4;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba, ctbau;
    uint32_t rsv1[4];
} __attribute__((packed)) CMD_Header;

typedef struct {
    uint8_t fis_type;
    uint8_t pmport : 4, rsv0 : 3, c : 1;
    uint8_t command, features_low;
    uint8_t lba0, lba1, lba2, device;
    uint8_t lba3, lba4, lba5, features_high;
    uint8_t count_low, count_high, icc, control;
    uint8_t rsv1[4];
} __attribute__((packed)) FIS_REG_H2D;

_Static_assert(sizeof(HBA_Port)    == 0x80, "HBA_Port layout");
_Static_assert(sizeof(HBA_Mem)     == 0x100 + 32 * 0x80, "HBA_Mem layout");
_Static_assert(sizeof(CMD_Header)  == 32, "CMD_Header layout");
_Static_assert(sizeof(FIS_REG_H2D) == 20, "FIS layout");
_Static_assert(sizeof(PRDT_Entry)  == 16, "PRDT layout");
_Static_assert(sizeof(CMD_Table)   == 144, "CMD_Table layout");

#define GHC_AE        (1u << 31)
#define PORT_CMD_ST   0x0001u
#define PORT_CMD_FRE  0x0010u
#define PORT_CMD_FR   0x4000u
#define PORT_CMD_CR   0x8000u
#define TFD_BSY       0x80u
#define TFD_DRQ       0x08u
#define TFD_ERR       0x01u
#define IS_TFES       (1u << 30)
#define SIG_ATA       0x00000101u
#define FIS_TYPE_H2D  0x27

#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_FLUSH_EXT      0xEA     /* FLUSH CACHE EXT */
#define ATA_FLUSH          0xE7     /* fallback for drives without 48-bit support */

#define MAX_SECTORS_PER_CMD 8192u   /* one PRDT entry covers 4 MiB */
#define BUSY_TIMEOUT_MS     1000
#define CMD_TIMEOUT_MS      5000

/* Everything the controller DMAs to/from, statically allocated and properly aligned.
 * The original kmalloc()ed (and leaked) the command list/FIS per port, and
 * kmalloc()ed + memset a fresh command table for EVERY read, write and flush. */
struct DmaArea {
    uint8_t   clb[1024] __attribute__((aligned(1024)));
    uint8_t   fb[256]   __attribute__((aligned(256)));
    CMD_Table ctbl      __attribute__((aligned(128)));
};

typedef struct {
    HBA_Port* port;
    struct DmaArea* dma;
    BlockDev dev;
} Disk;

_Static_assert(sizeof(struct DmaArea) % 1024 == 0, "DmaArea array elements must stay 1 KiB aligned");

static Disk disks[AHCI_MAX_DISKS];
static struct DmaArea dma_areas[AHCI_MAX_DISKS] __attribute__((aligned(1024)));
static int disk_count;

/* ---- helpers ---------------------------------------------------------------------------- */
static bool wait_clear(volatile uint32_t* reg, uint32_t mask, int timeout_ms) {
    int64_t end = timer_uptime_ms() + timeout_ms;
    while (*reg & mask) {
        if (timer_uptime_ms() > end) return false;
        __asm__ volatile ("pause");
    }
    return true;
}

static void port_stop(HBA_Port* p) {
    p->cmd &= ~(PORT_CMD_ST | PORT_CMD_FRE);
    wait_clear(&p->cmd, PORT_CMD_FR | PORT_CMD_CR, 500);
}

static void port_start(HBA_Port* p) {
    wait_clear(&p->cmd, PORT_CMD_CR, 500);
    p->cmd |= PORT_CMD_FRE;
    p->cmd |= PORT_CMD_ST;
}

/* The one routine that talks to the controller. read, write and flush used to be three
 * ~60 line copy-pastes (and the flush path was the only one checking for allocation failure). */
static bool issue(Disk* d, uint8_t ata_cmd, uint32_t lba, uint32_t count, void* buf, bool write) {
    HBA_Port* p = d->port;
    CMD_Table* t = &d->dma->ctbl;

    p->is = 0xFFFFFFFFu;
    p->serr = 0xFFFFFFFFu;

    CMD_Header* h = (CMD_Header*)d->dma->clb;
    memset(h, 0, sizeof *h);                       /* clear every flag; the original left stale bits */
    h->cfl = sizeof(FIS_REG_H2D) / 4;
    h->w = write ? 1 : 0;
    h->prdtl = count ? 1 : 0;
    h->ctba = (uint32_t)(uintptr_t)t;

    memset(t, 0, sizeof *t);
    if (count) {
        t->prdt[0].dba = (uint32_t)(uintptr_t)buf;
        t->prdt[0].dbc = count * SECTOR_SIZE - 1;
        t->prdt[0].i = 1;
    }

    FIS_REG_H2D* f = (FIS_REG_H2D*)t->cfis;
    f->fis_type = FIS_TYPE_H2D;
    f->c = 1;
    f->command = ata_cmd;
    f->device = 1 << 6;                            /* LBA mode */
    f->lba0 = (uint8_t)lba; f->lba1 = (uint8_t)(lba >> 8); f->lba2 = (uint8_t)(lba >> 16);
    f->lba3 = (uint8_t)(lba >> 24);                /* lba4/lba5: upper 16 bits of the 48-bit LBA stay 0 */
    f->count_low = (uint8_t)count; f->count_high = (uint8_t)(count >> 8);

    if (!wait_clear(&p->tfd, TFD_BSY | TFD_DRQ, BUSY_TIMEOUT_MS)) return false;

    __sync_synchronize();                          /* descriptors must be visible before CI is set */
    p->ci = 1;

    int64_t end = timer_uptime_ms() + CMD_TIMEOUT_MS;
    while (p->ci & 1) {
        if (p->is & IS_TFES) return false;
        if (timer_uptime_ms() > end) return false;
        __asm__ volatile ("pause");
    }
    return !(p->is & IS_TFES) && !(p->tfd & TFD_ERR);
}

static bool transfer(Disk* d, uint8_t cmd, uint32_t lba, uint32_t count, uint8_t* buf, bool write) {
    while (count) {
        uint32_t n = count > MAX_SECTORS_PER_CMD ? MAX_SECTORS_PER_CMD : count;
        if (!issue(d, cmd, lba, n, buf, write)) return false;
        lba += n; buf += (size_t)n * SECTOR_SIZE; count -= n;
    }
    return true;
}

static bool disk_read(void* ctx, uint32_t lba, uint32_t n, void* buf) {
    return transfer(ctx, ATA_READ_DMA_EXT, lba, n, buf, false);
}
static bool disk_write(void* ctx, uint32_t lba, uint32_t n, const void* buf) {
    return transfer(ctx, ATA_WRITE_DMA_EXT, lba, n, (uint8_t*)buf, true);
}
static bool disk_flush(void* ctx) {
    return issue(ctx, ATA_FLUSH_EXT, 0, 0, NULL, false) || issue(ctx, ATA_FLUSH, 0, 0, NULL, false);
}

/* ---- bring-up ---------------------------------------------------------------------------- */
int ahci_init(void) {
    PciAddr a;
    if (!pci_find_class(0x01, 0x06, &a)) return 0;            /* mass storage / SATA */

    pci_write32(a, 0x04, pci_read32(a, 0x04) | 0x06);          /* memory space + bus master */
    HBA_Mem* hba = (HBA_Mem*)(uintptr_t)(pci_read32(a, 0x24) & 0xFFFFFFF0u);
    hba->ghc |= GHC_AE;                                        /* the original never set AHCI-enable */

    uint32_t pi = hba->pi;
    for (int i = 0; i < 32 && disk_count < AHCI_MAX_DISKS; i++) {
        if (!(pi & (1u << i))) continue;
        HBA_Port* p = &hba->ports[i];
        if ((p->ssts & 0x0F) != 3 || ((p->ssts >> 8) & 0x0F) != 1) continue;   /* no active link */

        Disk* d = &disks[disk_count];
        d->port = p;
        d->dma = &dma_areas[disk_count];

        port_stop(p);
        memset(d->dma, 0, sizeof *d->dma);
        p->clb = (uint32_t)(uintptr_t)d->dma->clb; p->clbu = 0;
        p->fb  = (uint32_t)(uintptr_t)d->dma->fb;  p->fbu  = 0;
        p->is = 0xFFFFFFFFu; p->serr = 0xFFFFFFFFu; p->ie = 0;   /* polled I/O */
        port_start(p);

        /* Wait for the device to settle instead of the original 1,000,000-iteration busy loop. */
        wait_clear(&p->tfd, TFD_BSY | TFD_DRQ, BUSY_TIMEOUT_MS);
        if (p->sig != SIG_ATA) continue;                       /* skip ATAPI (CD-ROM), enclosure, ... */

        d->dev = (BlockDev){ d, disk_read, disk_write, disk_flush };
        disk_count++;
    }
    return disk_count;
}

int ahci_disk_count(void) { return disk_count; }
const BlockDev* ahci_disk(int i) { return (i >= 0 && i < disk_count) ? &disks[i].dev : NULL; }
